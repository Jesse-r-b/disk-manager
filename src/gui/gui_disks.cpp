// The Disks tab: the list, SMART detail, partitions and mounting, locating
// and powering down a disk.
#include "gui_app.hpp"
#include "gui_internal.hpp"

#include <glib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace dm {

using detail::computeDiskExtra;
using detail::healthColor;
using detail::healthGlyph;
using detail::human;

// ================================================================ Disks tab

void GuiApp::buildDisksTab() {
    disksStore_ = Gtk::ListStore::create(diskCols_);
    disksView_.set_model(disksStore_);
    disksView_.append_column("Device", diskCols_.path);
    disksView_.append_column("Size", diskCols_.size);
    {
        auto* col = Gtk::manage(new Gtk::TreeViewColumn("Health"));
        auto* renderer = Gtk::manage(new Gtk::CellRendererText());
        col->pack_start(*renderer, false);
        col->add_attribute(renderer->property_text(), diskCols_.status);
        col->add_attribute(renderer->property_foreground(), diskCols_.statusColor);
        disksView_.append_column(*col);
    }
    disksView_.append_column("Model", diskCols_.model);
    disksView_.append_column("Mounted", diskCols_.mounted);
    disksView_.append_column("RAID", diskCols_.raid);
    disksView_.append_column("", diskCols_.system);
    disksView_.signal_row_activated().connect(sigc::mem_fun(*this, &GuiApp::onDisksRowActivated));

    disksScroll_.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    disksScroll_.add(disksView_);
    disksScroll_.set_vexpand(true);
    disksTab_.set_border_width(6);
    disksTab_.pack_start(disksScroll_, Gtk::PACK_EXPAND_WIDGET);

    disksButtonBox_.pack_start(disksRefreshBtn_, Gtk::PACK_SHRINK);
    disksButtonBox_.pack_start(disksDetailsBtn_, Gtk::PACK_SHRINK);
    disksButtonBox_.pack_start(disksPartitionsBtn_, Gtk::PACK_SHRINK);
    disksButtonBox_.pack_start(disksLocateBtn_, Gtk::PACK_SHRINK);
    disksButtonBox_.pack_start(disksPowerDownBtn_, Gtk::PACK_SHRINK);
    disksTab_.pack_start(disksButtonBox_, Gtk::PACK_SHRINK);

    disksRefreshBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::refreshDisks));
    disksDetailsBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onDiskDetailsClicked));
    disksPartitionsBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onDiskPartitionsClicked));
    disksLocateBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onDiskLocateClicked));
    disksPowerDownBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onDiskPowerDownClicked));

    notebook_.append_page(disksTab_, "Disks");
    refreshDisks();
}

void GuiApp::refreshDisks() {
    disksStore_->clear();
    // Fetched once and reused for every disk below -- each of listDisks()/find()
    // would otherwise re-run `lsblk` from scratch on every call.
    auto allDevices = devices_.listAll();
    std::vector<BlockDevice> disks;
    for (auto& d : allDevices) if (d.type == "disk") disks.push_back(d);
    std::string rootDisk = devices_.rootDiskName();
    auto activeArrays = mdadm_.available() ? mdadm_.listArrays() : std::vector<RaidArray>{};
    auto discoveredArrays = mdadm_.available() ? mdadm_.scanForArrays() : std::vector<DiscoveredArray>{};
    for (auto& d : disks) {
        auto report = smart_.query(d.path);
        bool risk = smart_.isAtRisk(report);
        std::string glyph = healthGlyph(report, risk);
        auto extra = computeDiskExtra(d, allDevices, activeArrays, discoveredArrays);
        auto row = *disksStore_->append();
        row[diskCols_.path] = d.path;
        row[diskCols_.size] = human(d.sizeBytes);
        row[diskCols_.status] = glyph;
        row[diskCols_.statusColor] = healthColor(glyph);
        row[diskCols_.model] = d.model.empty() ? "(unknown model)" : d.model;
        row[diskCols_.mounted] = extra.mounted;
        row[diskCols_.raid] = extra.raid;
        row[diskCols_.system] = (!rootDisk.empty() && d.kname == rootDisk) ? "system disk" : "";
    }
}

std::string GuiApp::selectedDiskPath() {
    auto iter = disksView_.get_selection()->get_selected();
    if (!iter) return "";
    Glib::ustring p = (*iter)[diskCols_.path];
    return p;
}

void GuiApp::onDisksRowActivated(const Gtk::TreeModel::Path&, Gtk::TreeViewColumn*) {
    onDiskDetailsClicked();
}

void GuiApp::onDiskDetailsClicked() {
    std::string path = selectedDiskPath();
    if (path.empty()) { infoDialog("Disks", "Select a disk first."); return; }
    showSmartDetailDialog(path);
}

void GuiApp::onDiskPartitionsClicked() {
    std::string path = selectedDiskPath();
    if (path.empty()) { infoDialog("Disks", "Select a disk first."); return; }
    showPartitionsDialog(path);
    refreshDisks();
}

void GuiApp::onDiskLocateClicked() {
    std::string path = selectedDiskPath();
    if (path.empty()) { infoDialog("Disks", "Select a disk first."); return; }
    if (locate_.isActive(path)) {
        locate_.stop(path);
        infoDialog("Locate", "Stopped locating " + path);
        return;
    }
    auto method = locate_.bestMethodFor(path);
    if (method == LocateMethod::None) {
        infoDialog("Locate", "No way to identify this drive was found\n(no enclosure LED support and drive not readable).");
        return;
    }
    locate_.start(path, 0);
    std::string how = method == LocateMethod::Enclosure
        ? "Lighting the enclosure locate LED for this slot."
        : "No addressable LED found -- driving repeated read-only accesses so the\n"
          "drive's activity light flickers. Watch for the blinking drive, then come\n"
          "back here and click Locate again to stop.";
    infoDialog("Locate " + path, how);
}

void GuiApp::onDiskPowerDownClicked() {
    std::string path = selectedDiskPath();
    if (path.empty()) { infoDialog("Power down", "Select a disk first."); return; }
    auto devOpt = devices_.find(path);
    if (!devOpt) { infoDialog("Power down", "Could not read device info for " + path, Gtk::MESSAGE_ERROR); return; }
    auto& dev = *devOpt;

    if (!power_.hdparmAvailable()) { infoDialog("Power down", "hdparm is not installed."); return; }

    if (dev.kname == devices_.rootDiskName()) {
        infoDialog("Power down", "Refusing to power down the system disk.", Gtk::MESSAGE_ERROR);
        return;
    }
    if (!power_.supportsStandby(dev.transport)) {
        infoDialog("Power down",
            path + " is " + (dev.transport.empty() ? "not a spinning ATA/SATA disk" : dev.transport) +
            " -- ATA standby doesn't apply (no platters/heads to park).", Gtk::MESSAGE_ERROR);
        return;
    }

    // Refuse if this disk (or any partition on it) is a member of a currently
    // active RAID array -- spinning it down mid-array risks the array treating
    // it as failed.
    std::vector<std::string> namesToCheck = {dev.path};
    for (auto& c : dev.childPaths) namesToCheck.push_back(c);
    for (auto& arr : mdadm_.listArrays()) {
        for (auto& m : arr.members) {
            if (std::find(namesToCheck.begin(), namesToCheck.end(), m.device) != namesToCheck.end()) {
                infoDialog("Power down",
                    path + " has a member in active RAID array " + arr.device + " (" + m.device + ").\n"
                    "Powering it down could make the array treat it as failed. Stop or\n"
                    "otherwise take the array down first if you really need to do this.",
                    Gtk::MESSAGE_ERROR);
                return;
            }
        }
    }

    std::vector<std::string> mounted;
    for (auto& name : namesToCheck) {
        auto d = devices_.find(name);
        if (d && !d->mountpoint.empty()) mounted.push_back(d->mountpoint);
    }
    if (!mounted.empty()) {
        std::ostringstream os;
        os << path << " has mounted filesystems:\n";
        for (auto& mp : mounted) os << "  " << mp << "\n";
        os << "\nUnmount them and continue?";
        if (!confirmYesNo("Power down", os.str(), Gtk::MESSAGE_QUESTION)) return;
        for (auto& mp : mounted) {
            auto res = filesystems_.unmount(mp);
            if (!res.ok()) { infoDialog("Unmount failed", "Could not unmount " + mp + ":\n" + res.stdErr, Gtk::MESSAGE_ERROR); return; }
        }
    }

    if (!confirmYesNo("Power down",
            "Spin down " + path + " now?\nIt will spin back up automatically on its next access.",
            Gtk::MESSAGE_QUESTION)) return;
    auto res = power_.standby(path);
    infoDialog("Power down", res.ok() ? path + " is now in standby." : ("Failed:\n" + res.stdErr),
               res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
}

void GuiApp::showSmartDetailDialog(const std::string& diskPath) {
    auto r = smart_.query(diskPath);
    Gtk::Dialog dlg("SMART: " + diskPath, *this, true);
    dlg.set_default_size(580, 500);
    dlg.add_button("Close", Gtk::RESPONSE_CLOSE);
    auto* content = dlg.get_content_area();
    content->set_spacing(6);
    content->set_border_width(8);

    Gtk::Label summary;
    summary.set_xalign(0.0);
    summary.set_line_wrap(true);
    std::ostringstream os;
    if (!r.supported) {
        os << "SMART not available for " << diskPath << "\n";
        if (!r.error.empty()) os << "Error: " << r.error;
    } else {
        os << "Overall health: " << (r.healthy ? "PASSED" : "FAILED") << "\n";
        os << "At-risk heuristic: " << (smart_.isAtRisk(r) ? "YES -- consider replacing" : "no") << "\n";
        if (!r.modelFamily.empty()) os << "Model family: " << r.modelFamily << "\n";
        if (!r.firmwareVersion.empty()) os << "Firmware: " << r.firmwareVersion << "\n";
        if (r.temperatureC) os << "Temperature: " << *r.temperatureC << " C\n";
        if (r.powerOnHours) os << "Power-on hours: " << *r.powerOnHours << "\n";
        if (r.powerCycles) os << "Power cycles: " << *r.powerCycles << "\n";
        if (r.reallocatedSectors) os << "Reallocated sectors: " << *r.reallocatedSectors << "\n";
        if (r.pendingSectors) os << "Pending sectors: " << *r.pendingSectors << "\n";
        if (r.uncorrectableSectors) os << "Uncorrectable sectors: " << *r.uncorrectableSectors << "\n";
        if (r.nvmePercentageUsed) os << "NVMe percentage used: " << (int)*r.nvmePercentageUsed << "%\n";
        if (r.nvmeMediaErrors) os << "NVMe media errors: " << *r.nvmeMediaErrors << "\n";
    }
    summary.set_text(os.str());
    content->pack_start(summary, Gtk::PACK_SHRINK);

    if (r.supported && !r.attributes.empty()) {
        struct AttrColumns : public Gtk::TreeModelColumnRecord {
            Gtk::TreeModelColumn<int> id, value, worst, threshold;
            Gtk::TreeModelColumn<Glib::ustring> name, raw, failed;
            AttrColumns() { add(id); add(name); add(value); add(worst); add(threshold); add(raw); add(failed); }
        } cols;
        auto store = Gtk::ListStore::create(cols);
        for (auto& a : r.attributes) {
            auto row = *store->append();
            row[cols.id] = a.id;
            row[cols.name] = a.name;
            row[cols.value] = a.value;
            row[cols.worst] = a.worst;
            row[cols.threshold] = a.threshold;
            row[cols.raw] = a.rawValue;
            row[cols.failed] = a.failed ? "FAILED" : "";
        }
        auto* tv = Gtk::manage(new Gtk::TreeView(store));
        tv->append_column("ID", cols.id);
        tv->append_column("Name", cols.name);
        tv->append_column("Value", cols.value);
        tv->append_column("Worst", cols.worst);
        tv->append_column("Thresh", cols.threshold);
        tv->append_column("Raw", cols.raw);
        tv->append_column("Failed", cols.failed);
        auto* scroll = Gtk::manage(new Gtk::ScrolledWindow());
        scroll->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        scroll->add(*tv);
        scroll->set_vexpand(true);
        content->pack_start(*scroll, Gtk::PACK_EXPAND_WIDGET);
    }

    dlg.show_all_children();
    dlg.run();
}

void GuiApp::showPartitionsDialog(const std::string& diskPath) {
    Gtk::Dialog dlg("Partitions: " + diskPath, *this, true);
    dlg.set_default_size(680, 440);
    dlg.add_button("Close", Gtk::RESPONSE_CLOSE);
    auto* content = dlg.get_content_area();
    content->set_spacing(6);
    content->set_border_width(8);

    struct PartColumns : public Gtk::TreeModelColumnRecord {
        Gtk::TreeModelColumn<int> number;
        Gtk::TreeModelColumn<Glib::ustring> path, size, fsType, flags;
        PartColumns() { add(number); add(path); add(size); add(fsType); add(flags); }
    } cols;
    auto store = Gtk::ListStore::create(cols);
    Gtk::TreeView tv(store);
    tv.append_column("#", cols.number);
    tv.append_column("Path", cols.path);
    tv.append_column("Size", cols.size);
    tv.append_column("Filesystem", cols.fsType);
    tv.append_column("Flags", cols.flags);
    Gtk::ScrolledWindow scroll;
    scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    scroll.add(tv);
    scroll.set_vexpand(true);
    content->pack_start(scroll, Gtk::PACK_EXPAND_WIDGET);

    Gtk::Label diskInfoLabel;
    diskInfoLabel.set_xalign(0.0);
    content->pack_start(diskInfoLabel, Gtk::PACK_SHRINK);

    auto reload = [&]() {
        store->clear();
        auto table = partitions_.read(diskPath);
        for (auto& p : table.partitions) {
            auto row = *store->append();
            row[cols.number] = p.number;
            row[cols.path] = p.path;
            row[cols.size] = human(p.sizeBytes);
            row[cols.fsType] = p.fsType.empty() ? "(no fs)" : p.fsType;
            row[cols.flags] = p.flags;
        }
        diskInfoLabel.set_text("Table: " + (table.label.empty() ? std::string("(none)") : table.label) +
                                "   Size: " + human(table.sizeBytes));
    };
    reload();

    Gtk::Box btnBox(Gtk::ORIENTATION_HORIZONTAL, 6);
    Gtk::Button createBtn("Create Partition");
    Gtk::Button mountBtn("Mount/Manage");
    Gtk::Button formatBtn("Format");
    Gtk::Button resizeBtn("Resize");
    Gtk::Button bootBtn("Toggle Boot Flag");
    Gtk::Button deleteBtn("Delete");
    for (auto* b : {&createBtn, &mountBtn, &formatBtn, &resizeBtn, &bootBtn, &deleteBtn})
        btnBox.pack_start(*b, Gtk::PACK_SHRINK);
    content->pack_start(btnBox, Gtk::PACK_SHRINK);

    auto getSelectedPartition = [&]() -> std::optional<PartitionEntry> {
        auto iter = tv.get_selection()->get_selected();
        if (!iter) return std::nullopt;
        int num = (*iter)[cols.number];
        auto table = partitions_.read(diskPath);
        for (auto& p : table.partitions)
            if (p.number == num) return p;
        return std::nullopt;
    };

    createBtn.signal_clicked().connect([&, this]() {
        auto table = partitions_.read(diskPath);
        if (table.label.empty() || table.label == "loop") {
            if (!confirmYesNo("Create partition table",
                    "Disk has no partition table (or is unpartitioned). Create a GPT table now?\n"
                    "This erases any existing data on " + diskPath + ".")) return;
            auto res = partitions_.createTable(diskPath, "gpt");
            if (!res.ok()) { infoDialog("Create table failed", res.stdErr, Gtk::MESSAGE_ERROR); return; }
        }
        std::string fsType, startStr, endStr;
        if (!promptInput("New partition", "Filesystem type hint (ext4, xfs, fat32, linux-swap, or blank):", "ext4", fsType)) return;
        if (!promptInput("New partition", "Start offset in MiB:", "1", startStr) || startStr.empty()) return;
        if (!promptInput("New partition", "End offset in MiB (blank = rest of disk):", "", endStr)) return;
        uint64_t startBytes = 0, endBytes = 0;
        try { startBytes = std::stoull(startStr) * 1024ULL * 1024ULL; }
        catch (...) { infoDialog("Create partition", "Invalid start offset.", Gtk::MESSAGE_ERROR); return; }
        if (!endStr.empty()) {
            try { endBytes = std::stoull(endStr) * 1024ULL * 1024ULL; }
            catch (...) { infoDialog("Create partition", "Invalid end offset.", Gtk::MESSAGE_ERROR); return; }
        }
        auto res = partitions_.createPartition(diskPath, fsType, startBytes, endBytes);
        infoDialog("Create partition", res.ok() ? "Partition created." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        reload();
    });

    mountBtn.signal_clicked().connect([&, this]() {
        auto part = getSelectedPartition();
        if (!part) { infoDialog("Mount", "Select a partition first."); return; }
        auto devOpt = devices_.find(part->path);
        if (!devOpt) { infoDialog("Mount", "Could not read device info for " + part->path, Gtk::MESSAGE_ERROR); return; }
        showMountManageDialog(*devOpt);
        reload();
    });

    formatBtn.signal_clicked().connect([&, this]() {
        auto part = getSelectedPartition();
        if (!part) { infoDialog("Format", "Select a partition first."); return; }
        std::string fsType;
        if (!promptInput("Format " + part->path, "Filesystem type (ext4, xfs, btrfs, vfat, ntfs):", "ext4", fsType) || fsType.empty()) return;
        if (!confirmYesNo("Format " + part->path,
                "This ERASES ALL DATA on " + part->path + " (" + human(part->sizeBytes) + ").")) return;
        auto res = filesystems_.makeFilesystem(part->path, fsType);
        infoDialog("mkfs " + part->path, res.ok() ? "Filesystem created." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        reload();
    });

    resizeBtn.signal_clicked().connect([&, this]() {
        auto part = getSelectedPartition();
        if (!part) { infoDialog("Resize", "Select a partition first."); return; }
        std::string endStr;
        if (!promptInput("Resize " + part->path, "New end offset in MiB (from start of disk):", "", endStr) || endStr.empty()) return;
        uint64_t endBytes = 0;
        try { endBytes = std::stoull(endStr) * 1024ULL * 1024ULL; }
        catch (...) { infoDialog("Resize", "Invalid value.", Gtk::MESSAGE_ERROR); return; }
        if (!confirmYesNo("Resize " + part->path,
                "Resizing " + part->path + " does not touch the filesystem inside it.\n"
                "Grow the filesystem afterwards from the mount-management dialog.")) return;
        auto res = partitions_.resizePartition(diskPath, part->number, endBytes);
        infoDialog("Resize " + part->path, res.ok() ? "Partition table updated." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        reload();
    });

    bootBtn.signal_clicked().connect([&, this]() {
        auto part = getSelectedPartition();
        if (!part) { infoDialog("Boot flag", "Select a partition first."); return; }
        bool turnOn = part->flags.find("boot") == std::string::npos;
        auto res = partitions_.setFlag(diskPath, part->number, "boot", turnOn);
        infoDialog("Boot flag", res.ok() ? "Done." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        reload();
    });

    deleteBtn.signal_clicked().connect([&, this]() {
        auto part = getSelectedPartition();
        if (!part) { infoDialog("Delete", "Select a partition first."); return; }
        if (!confirmYesNo("Delete partition",
                "This DELETES partition " + part->path + " and everything on it.")) return;
        auto res = partitions_.deletePartition(diskPath, part->number);
        infoDialog("Delete", res.ok() ? "Deleted." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        reload();
    });

    dlg.show_all_children();
    dlg.run();
}

void GuiApp::showMountManageDialog(const BlockDevice& devIn) {
    std::string devicePath = devIn.path;
    Gtk::Dialog dlg("Mount management: " + devicePath, *this, true);
    dlg.set_default_size(500, 300);
    dlg.add_button("Close", Gtk::RESPONSE_CLOSE);
    auto* content = dlg.get_content_area();
    content->set_spacing(8);
    content->set_border_width(8);

    Gtk::Label status;
    status.set_xalign(0.0);
    status.set_line_wrap(true);
    content->pack_start(status, Gtk::PACK_SHRINK);

    auto currentDev = [&]() -> BlockDevice {
        auto d = devices_.find(devicePath);
        return d ? *d : devIn;
    };
    auto refreshStatus = [&]() {
        auto dev = currentDev();
        bool inFstab = filesystems_.isInFstab(dev.path);
        std::ostringstream os;
        os << "Device: " << dev.path << "\nFilesystem: " << (dev.fsType.empty() ? "(none)" : dev.fsType)
           << "\nCurrently mounted at: " << (dev.mountpoint.empty() ? "(not mounted)" : dev.mountpoint)
           << "\nIn /etc/fstab: " << (inFstab ? "yes" : "no");
        status.set_text(os.str());
    };
    refreshStatus();

    Gtk::Box btnBox(Gtk::ORIENTATION_VERTICAL, 6);
    Gtk::Button mountBtn("Mount now");
    Gtk::Button unmountBtn("Unmount");
    Gtk::Button growBtn("Grow filesystem to fill partition");
    Gtk::Button addFstabBtn("Add persistent entry to /etc/fstab");
    Gtk::Button removeFstabBtn("Remove entry from /etc/fstab");
    for (auto* b : {&mountBtn, &unmountBtn, &growBtn, &addFstabBtn, &removeFstabBtn})
        btnBox.pack_start(*b, Gtk::PACK_SHRINK);
    content->pack_start(btnBox, Gtk::PACK_SHRINK);

    mountBtn.signal_clicked().connect([&, this]() {
        auto dev = currentDev();
        std::string mp;
        if (!promptInput("Mount", "Mountpoint path:", "/mnt/" + dev.name, mp) || mp.empty()) return;
        auto res = filesystems_.mount(dev.path, mp, dev.fsType);
        infoDialog("Mount", res.ok() ? ("Mounted at " + mp) : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        refreshStatus();
    });
    unmountBtn.signal_clicked().connect([&, this]() {
        auto dev = currentDev();
        auto res = filesystems_.unmount(dev.mountpoint.empty() ? dev.path : dev.mountpoint);
        infoDialog("Unmount", res.ok() ? "Unmounted." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        refreshStatus();
    });
    growBtn.signal_clicked().connect([&, this]() {
        auto dev = currentDev();
        auto res = filesystems_.resize(dev.path, dev.fsType, dev.mountpoint);
        infoDialog("Resize filesystem", res.ok() ? "Filesystem grown." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        refreshStatus();
    });
    addFstabBtn.signal_clicked().connect([&, this]() {
        auto dev = currentDev();
        std::string mp, opts;
        if (!promptInput("fstab", "Mountpoint for fstab entry:",
                dev.mountpoint.empty() ? ("/mnt/" + dev.name) : dev.mountpoint, mp) || mp.empty()) return;
        if (!promptInput("fstab", "Mount options:", "defaults", opts)) return;
        auto res = filesystems_.addToFstab(dev.path, mp, dev.fsType, opts);
        infoDialog("fstab", res.ok() ? "Added (backup of /etc/fstab saved)." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        refreshStatus();
    });
    removeFstabBtn.signal_clicked().connect([&, this]() {
        auto dev = currentDev();
        auto res = filesystems_.removeFromFstab(dev.path);
        infoDialog("fstab", res.ok() ? "Removed (backup of /etc/fstab saved)." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        refreshStatus();
    });

    dlg.show_all_children();
    dlg.run();
}

} // namespace dm
