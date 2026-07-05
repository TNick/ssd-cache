#include "ssd_cache/win_file_copy.h"

#include <array>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ssd_cache/utf.h"

#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "Bcrypt.lib")

namespace ssd_cache {
namespace {

class Handle {
public:
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    ~Handle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    HANDLE get() const {
        return handle_;
    }

    bool valid() const {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    // Close the wrapped handle early. Required before renaming the temp file:
    // it is opened with an exclusive share mode, so MoveFileExW would fail with
    // a sharing violation while the handle is still open.
    void reset() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_;
};

// Render a Win32 error code as "<code> (<message>)" for logging and the
// cache index's last_error column. FormatMessage gives the same human-readable
// text the system uses (e.g. "The process cannot access the file because it is
// being used by another process."), trimmed of its trailing newline.
std::string describe_error(DWORD error) {
    LPWSTR text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&text),
        0,
        nullptr
    );

    std::wstring message;
    if (length != 0 && text != nullptr) {
        message.assign(text, length);
    }
    if (text != nullptr) {
        LocalFree(text);
    }

    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L'.' || message.back() == L' ')) {
        message.pop_back();
    }

    std::string result = std::to_string(error);
    if (!message.empty()) {
        result += " (" + wide_to_utf8(message) + ")";
    }
    return result;
}

class BCryptAlgorithm {
public:
    BCryptAlgorithm() {
        const NTSTATUS status = BCryptOpenAlgorithmProvider(
            &algorithm_,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0
        );
        if (status < 0) {
            throw std::runtime_error("failed to open SHA-256 provider");
        }
    }

    BCryptAlgorithm(const BCryptAlgorithm&) = delete;
    BCryptAlgorithm& operator=(const BCryptAlgorithm&) = delete;

    ~BCryptAlgorithm() {
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    BCRYPT_ALG_HANDLE get() const {
        return algorithm_;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
};

class BCryptHash {
public:
    explicit BCryptHash(const BCryptAlgorithm& algorithm) : algorithm_(algorithm) {
        DWORD result_size = 0;
        DWORD object_length = 0;
        NTSTATUS status = BCryptGetProperty(
            algorithm_.get(),
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_length),
            sizeof(object_length),
            &result_size,
            0
        );
        if (status < 0) {
            throw std::runtime_error("failed to read hash object length");
        }

        status = BCryptGetProperty(
            algorithm_.get(),
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_length_),
            sizeof(hash_length_),
            &result_size,
            0
        );
        if (status < 0) {
            throw std::runtime_error("failed to read hash length");
        }

        object_.resize(object_length);
        status = BCryptCreateHash(
            algorithm_.get(),
            &hash_,
            object_.data(),
            static_cast<ULONG>(object_.size()),
            nullptr,
            0,
            0
        );
        if (status < 0) {
            throw std::runtime_error("failed to create SHA-256 hash");
        }
    }

    BCryptHash(const BCryptHash&) = delete;
    BCryptHash& operator=(const BCryptHash&) = delete;

    ~BCryptHash() {
        if (hash_ != nullptr) {
            BCryptDestroyHash(hash_);
        }
    }

    void update(const std::vector<std::byte>& data, DWORD size) {
        const NTSTATUS status = BCryptHashData(
            hash_,
            reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data.data())),
            size,
            0
        );
        if (status < 0) {
            throw std::runtime_error("failed to hash data");
        }
    }

    std::string finish_hex() {
        std::vector<unsigned char> digest(hash_length_);
        const NTSTATUS status = BCryptFinishHash(
            hash_,
            digest.data(),
            static_cast<ULONG>(digest.size()),
            0
        );
        if (status < 0) {
            throw std::runtime_error("failed to finish SHA-256 hash");
        }

        static constexpr char kHex[] = "0123456789abcdef";
        std::string result;
        result.reserve(digest.size() * 2);
        for (const auto byte : digest) {
            result.push_back(kHex[(byte >> 4) & 0x0f]);
            result.push_back(kHex[byte & 0x0f]);
        }

        return result;
    }

private:
    const BCryptAlgorithm& algorithm_;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<UCHAR> object_;
    DWORD hash_length_ = 0;
};

std::wstring extended_path(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\")) {
        return path;
    }

    if (path.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }

    return L"\\\\?\\" + path;
}

bool is_missing_error(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

struct FileSizeResult {
    std::optional<std::uint64_t> size;
    DWORD error = ERROR_SUCCESS;
};

FileSizeResult file_size_bytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(
            extended_path(path).c_str(),
            GetFileExInfoStandard,
            &data
        )) {
        return FileSizeResult{std::nullopt, GetLastError()};
    }

    LARGE_INTEGER size{};
    size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
    size.LowPart = data.nFileSizeLow;
    return FileSizeResult{
        static_cast<std::uint64_t>(size.QuadPart),
        ERROR_SUCCESS
    };
}

void ensure_destination_directory(const std::wstring& path) {
    const std::filesystem::path file_path(path);
    if (file_path.has_parent_path()) {
        std::filesystem::create_directories(file_path.parent_path());
    }
}

void set_low_io_priority(HANDLE handle) {
    FILE_IO_PRIORITY_HINT_INFO priority{};
    priority.PriorityHint = IoPriorityHintVeryLow;
    SetFileInformationByHandle(
        handle,
        FileIoPriorityHintInfo,
        &priority,
        sizeof(priority)
    );
}

Handle open_existing_for_read(const std::wstring& path) {
    return Handle(CreateFileW(
        extended_path(path).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    ));
}

std::string hash_existing_file(
    const std::wstring& path,
    std::size_t chunk_size
) {
    Handle source = open_existing_for_read(path);
    if (!source.valid()) {
        throw std::runtime_error("failed to open file for hashing");
    }

    set_low_io_priority(source.get());
    BCryptAlgorithm algorithm;
    BCryptHash hash(algorithm);
    std::vector<std::byte> buffer(chunk_size);

    while (true) {
        DWORD read = 0;
        if (!ReadFile(
                source.get(),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read,
                nullptr
            )) {
            throw std::runtime_error("failed to read file for hashing");
        }

        if (read == 0) {
            break;
        }

        hash.update(buffer, read);
    }

    return hash.finish_hex();
}

class BackgroundThreadMode {
public:
    BackgroundThreadMode() {
        entered_ = SetThreadPriority(
            GetCurrentThread(),
            THREAD_MODE_BACKGROUND_BEGIN
        ) != FALSE;
    }

    BackgroundThreadMode(const BackgroundThreadMode&) = delete;
    BackgroundThreadMode& operator=(const BackgroundThreadMode&) = delete;

    ~BackgroundThreadMode() {
        if (entered_) {
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        }
    }

private:
    bool entered_ = false;
};

}  // namespace

WinFileCopyEngine::WinFileCopyEngine()
    : chunk_size_(1024 * 1024), sleep_between_chunks_(2) {}

CopyResult WinFileCopyEngine::copy_and_hash(
    const std::wstring& source_abs,
    const std::wstring& cache_abs,
    bool compare_hash_before_overwrite,
    const std::atomic_bool& cancelled
) {
    CopyResult result;
    result.completed_at = std::chrono::system_clock::now();

    const auto source_size = file_size_bytes(source_abs);
    if (!source_size.size) {
        if (is_missing_error(source_size.error)) {
            result.action = CopyAction::SourceMissing;
            result.error_message =
                "source file does not exist: error " +
                describe_error(source_size.error);
            return result;
        }

        result.action = CopyAction::Failed;
        result.error_message =
            "failed to inspect source: error " +
            describe_error(source_size.error);
        return result;
    }

    result.source_size_bytes = *source_size.size;
    const auto cache_size = file_size_bytes(cache_abs);
    if (cache_size.size) {
        result.cached_size_bytes = *cache_size.size;

        if (*cache_size.size == *source_size.size &&
            !compare_hash_before_overwrite) {
            result.action = CopyAction::SkippedSameSize;
            return result;
        }

        if (*cache_size.size == *source_size.size &&
            compare_hash_before_overwrite) {
            const auto source_hash = hash_existing_file(source_abs, chunk_size_);
            const auto cache_hash = hash_existing_file(cache_abs, chunk_size_);
            if (source_hash == cache_hash) {
                result.action = CopyAction::SkippedHashMatch;
                result.hash_hex = source_hash;
                return result;
            }
        }
    }

    try {
        ensure_destination_directory(cache_abs);
        const std::wstring temp_path = cache_abs + L".ssd-cache.tmp";

        Handle source = open_existing_for_read(source_abs);
        if (!source.valid()) {
            const DWORD open_error = GetLastError();
            if (is_missing_error(open_error)) {
                result.action = CopyAction::SourceMissing;
                result.error_message =
                    "source file disappeared before copy: error " +
                    describe_error(open_error);
                return result;
            }

            result.action = CopyAction::Failed;
            result.error_message =
                "failed to open source: error " + describe_error(open_error);
            return result;
        }

        Handle destination(CreateFileW(
            extended_path(temp_path).c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr
        ));
        if (!destination.valid()) {
            const DWORD open_error = GetLastError();
            result.action = CopyAction::Failed;
            result.error_message =
                "failed to open cache destination: error " +
                describe_error(open_error);
            return result;
        }

        set_low_io_priority(source.get());
        set_low_io_priority(destination.get());
        BackgroundThreadMode background;
        BCryptAlgorithm algorithm;
        BCryptHash hash(algorithm);
        std::vector<std::byte> buffer(chunk_size_);

        while (true) {
            if (cancelled.load()) {
                destination.reset();
                DeleteFileW(extended_path(temp_path).c_str());
                result.action = CopyAction::Cancelled;
                result.error_message = "copy cancelled";
                return result;
            }

            DWORD read = 0;
            if (!ReadFile(
                    source.get(),
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &read,
                    nullptr
                )) {
                const DWORD read_error = GetLastError();
                DeleteFileW(extended_path(temp_path).c_str());
                if (is_missing_error(read_error)) {
                    result.action = CopyAction::SourceMissing;
                    result.error_message =
                        "source file disappeared during copy: error " +
                        describe_error(read_error);
                    return result;
                }

                result.action = CopyAction::Failed;
                result.error_message =
                    "failed to read source: error " +
                    describe_error(read_error);
                return result;
            }

            if (read == 0) {
                break;
            }

            hash.update(buffer, read);

            DWORD written = 0;
            if (!WriteFile(
                    destination.get(),
                    buffer.data(),
                    read,
                    &written,
                    nullptr
                ) || written != read) {
                const DWORD write_error = GetLastError();
                DeleteFileW(extended_path(temp_path).c_str());
                result.action = CopyAction::Failed;
                result.error_message =
                    "failed to write cache destination: error " +
                    describe_error(write_error);
                return result;
            }

            if (sleep_between_chunks_.count() > 0) {
                Sleep(static_cast<DWORD>(sleep_between_chunks_.count()));
            }
        }

        result.hash_hex = hash.finish_hex();
        result.cached_size_bytes = *source_size.size;
        result.completed_at = std::chrono::system_clock::now();

        // Release both handles before the rename. The temp file was opened with
        // an exclusive share mode (share flag 0), so MoveFileExW would otherwise
        // fail with a sharing violation while the handle is still open.
        source.reset();
        destination.reset();

        if (!MoveFileExW(
                extended_path(temp_path).c_str(),
                extended_path(cache_abs).c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
            )) {
            const DWORD move_error = GetLastError();
            DeleteFileW(extended_path(temp_path).c_str());
            result.action = CopyAction::Failed;
            result.error_message =
                "failed to replace cache file: error " +
                describe_error(move_error);
            return result;
        }

        result.action = CopyAction::Copied;
        return result;
    } catch (const std::exception& error) {
        result.action = CopyAction::Failed;
        result.error_message = error.what();
        return result;
    }
}

RemoveResult WinFileCopyEngine::remove_cached_file(
    const std::wstring& cache_abs
) {
    RemoveResult result;
    const auto temp_path = cache_abs + L".ssd-cache.tmp";
    DeleteFileW(extended_path(temp_path).c_str());

    if (DeleteFileW(extended_path(cache_abs).c_str())) {
        result.removed = true;
        return result;
    }

    const DWORD delete_error = GetLastError();
    if (is_missing_error(delete_error)) {
        result.missing = true;
        return result;
    }

    result.error_message =
        "failed to delete cache file: error " + describe_error(delete_error);
    return result;
}

void WinFileCopyEngine::set_chunk_size(std::size_t chunk_size) {
    chunk_size_ = chunk_size;
}

void WinFileCopyEngine::set_sleep_between_chunks(
    std::chrono::milliseconds delay
) {
    sleep_between_chunks_ = delay;
}

std::optional<std::uint64_t> WinFreeSpaceProvider::free_bytes(
    const std::wstring& path
) const {
    ULARGE_INTEGER free_bytes_available{};
    if (!GetDiskFreeSpaceExW(
            path.c_str(),
            &free_bytes_available,
            nullptr,
            nullptr
        )) {
        return std::nullopt;
    }

    return static_cast<std::uint64_t>(free_bytes_available.QuadPart);
}

}  // namespace ssd_cache
