#pragma once

/**
 * @file
 * @brief Drive-letter, network-mapping and volume mount-point helpers, plus the
 *        Windows mount presentation used to enact modes.
 */

#include <optional>
#include <string>

#include "ssd_cache/interfaces.h"

namespace ssd_cache {

/**
 * Builds a drive root path from a letter.
 *
 * @param letter Drive letter (case-insensitive).
 * @return The root path, e.g. "K:\\".
 */
std::wstring drive_root(wchar_t letter);

/**
 * Reads the UNC a network drive is mapped to.
 *
 * @param letter Drive letter to query.
 * @return The mapped UNC, or nullopt if the letter is not a network drive.
 */
std::optional<std::wstring> network_unc_for_drive(wchar_t letter);

/**
 * Maps a network drive letter to a UNC in the current user session.
 *
 * @param letter Drive letter to assign.
 * @param unc UNC path to map.
 * @return True on success or if the letter is already assigned as requested.
 */
bool map_network_drive(wchar_t letter, const std::wstring& unc);

/**
 * Removes a network drive mapping.
 *
 * @param letter Drive letter to unmap.
 * @return True on success or if the letter was not connected.
 */
bool unmap_network_drive(wchar_t letter);

/**
 * Reads the volume GUID name mounted at a drive letter.
 *
 * @param letter Drive letter to query.
 * @return The volume name, or nullopt if nothing is mounted there.
 */
std::optional<std::wstring> volume_name_for_drive(wchar_t letter);

/**
 * Mounts a volume at a drive letter (machine-global mount point).
 *
 * @param volume_name Volume GUID name to mount.
 * @param letter Drive letter to mount it at.
 * @return True on success.
 */
bool assign_volume_to_drive(const std::wstring& volume_name, wchar_t letter);

/**
 * Removes the volume mount point at a drive letter.
 *
 * @param letter Drive letter to unmount.
 * @return True on success or if there was nothing mounted there.
 */
bool remove_drive_mount(wchar_t letter);

/**
 * Windows implementation of IMountPresentation. Applies each mode by remapping
 * the source network drive and moving the cache volume between the cache and
 * presentation letters.
 */
class WinMountPresentation final : public IMountPresentation {
public:
    /**
     * @param config Source UNC and drive letters.
     * @return True on success.
     */
    bool enter_monitor_mode(const AppConfig& config) override;

    /**
     * @param config Source UNC and drive letters.
     * @return True on success.
     */
    bool enter_serve_mode(const AppConfig& config) override;

    /**
     * @param config Source UNC and drive letters.
     * @return True on success.
     */
    bool enter_disabled_mode(const AppConfig& config) override;
};

}  // namespace ssd_cache
