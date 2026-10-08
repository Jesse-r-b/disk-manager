// Helpers shared by the gui_*.cpp files: number formatting, health glyphs
// and the per-disk mount/RAID summary. Internal to the GUI executable.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gui_app.hpp"

namespace dm {
namespace detail {

std::string human(uint64_t n);
std::string healthGlyph(const SmartReport& r, bool atRisk);
std::string healthColor(const std::string& glyph);
std::string join(const std::vector<std::string>& items, const std::string& sep);

// Mountpoint(s) and RAID membership for a disk, checking the disk itself and
// every partition on it. RAID membership covers both currently-assembled arrays
// (from listArrays()) and arrays found on-disk but not assembled (from
// scanForArrays()) -- a disk can be a "member" either way.
struct DiskExtra { std::string mounted; std::string raid; };

// `allDevices` must be a single already-fetched DeviceManager::listAll() result,
// reused across every disk in the caller's loop -- DeviceManager::find() re-runs
// `lsblk` from scratch on every call, so calling it once per partition per disk
// here turns a single screen refresh into dozens of redundant lsblk invocations
// (this was the actual cause of a "very slow to launch" bug: not the
// display/pkexec/sudo plumbing, just this).
DiskExtra computeDiskExtra(const BlockDevice& d, const std::vector<BlockDevice>& allDevices,
                            const std::vector<RaidArray>& activeArrays,
                            const std::vector<DiscoveredArray>& discoveredArrays);

}  // namespace detail
}  // namespace dm
