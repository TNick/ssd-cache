#include "ssd_cache/win_file_copy.h"

#include <array>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <vector>

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

private:
    HANDLE handle_;
};

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

std::optional<std::uint64_t> file_size_bytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(
            extended_path(path).c_str(),
            GetFileExInfoStandard,
            &data
        )) {
        return std::nullopt;
    }

    LARGE_INTEGER size{};
    size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
    size.LowPart = data.nFileSizeLow;
    return static_cast<std::uint64_t>(size.QuadPart);
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
    bool compare_hash_before_overwrite
) {
    CopyResult result;
    result.completed_at = std::chrono::system_clock::now();

    const auto source_size = file_size_bytes(source_abs);
    if (!source_size) {
        result.action = CopyAction::SourceMissing;
        result.error_message = "source file does not exist";
        return result;
    }

    result.source_size_bytes = *source_size;
    const auto cache_size = file_size_bytes(cache_abs);
    if (cache_size) {
        result.cached_size_bytes = *cache_size;

        if (*cache_size == *source_size && !compare_hash_before_overwrite) {
            result.action = CopyAction::SkippedSameSize;
            return result;
        }

        if (*cache_size == *source_size && compare_hash_before_overwrite) {
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
            result.action = CopyAction::SourceMissing;
            result.error_message = "failed to open source";
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
            result.action = CopyAction::Failed;
            result.error_message = "failed to open cache destination";
            return result;
        }

        set_low_io_priority(source.get());
        set_low_io_priority(destination.get());
        BackgroundThreadMode background;
        BCryptAlgorithm algorithm;
        BCryptHash hash(algorithm);
        std::vector<std::byte> buffer(chunk_size_);

        while (true) {
            DWORD read = 0;
            if (!ReadFile(
                    source.get(),
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &read,
                    nullptr
                )) {
                DeleteFileW(extended_path(temp_path).c_str());
                result.action = CopyAction::Failed;
                result.error_message = "failed to read source";
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
                DeleteFileW(extended_path(temp_path).c_str());
                result.action = CopyAction::Failed;
                result.error_message = "failed to write cache destination";
                return result;
            }

            if (sleep_between_chunks_.count() > 0) {
                Sleep(static_cast<DWORD>(sleep_between_chunks_.count()));
            }
        }

        result.hash_hex = hash.finish_hex();
        result.cached_size_bytes = *source_size;
        result.completed_at = std::chrono::system_clock::now();

        if (!MoveFileExW(
                extended_path(temp_path).c_str(),
                extended_path(cache_abs).c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
            )) {
            DeleteFileW(extended_path(temp_path).c_str());
            result.action = CopyAction::Failed;
            result.error_message = "failed to replace cache file";
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

void WinFileCopyEngine::set_chunk_size(std::size_t chunk_size) {
    chunk_size_ = chunk_size;
}

void WinFileCopyEngine::set_sleep_between_chunks(
    std::chrono::milliseconds delay
) {
    sleep_between_chunks_ = delay;
}

}  // namespace ssd_cache
