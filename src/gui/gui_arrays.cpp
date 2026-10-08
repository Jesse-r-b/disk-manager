// The Arrays tab: listing, assembling, creating, growing and repairing
// RAID arrays.
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

using detail::human;
using detail::join;

// =============================================================== Arrays tab

void GuiApp::buildArraysTab() {
    arraysStore_ = Gtk::ListStore::create(arrayCols_);
    arraysView_.set_model(arraysStore_);
    arraysView_.append_column("Device", arrayCols_.device);
    arraysView_.append_column("Level", arrayCols_.level);
    arraysView_.append_column("Size", arrayCols_.size);
    {
        auto* col = Gtk::manage(new Gtk::TreeViewColumn("State"));
        auto* renderer = Gtk::manage(new Gtk::CellRendererText());
        col->pack_start(*renderer, false);
        col->add_attribute(renderer->property_text(), arrayCols_.state);
        col->add_attribute(renderer->property_foreground(), arrayCols_.stateColor);
        arraysView_.append_column(*col);
    }
    arraysView_.signal_row_activated().connect(sigc::mem_fun(*this, &GuiApp::onArraysRowActivated));

    arraysScroll_.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    arraysScroll_.add(arraysView_);
    arraysScroll_.set_vexpand(true);
    arraysTab_.set_border_width(6);
    arraysTab_.pack_start(arraysScroll_, Gtk::PACK_EXPAND_WIDGET);

    for (auto* b : {&arraysRefreshBtn_, &arraysDetailBtn_, &arraysAssembleBtn_, &arraysCreateBtn_, &arraysReplaceBtn_,
                    &arraysAddSpareBtn_, &arraysFailRemoveBtn_, &arraysGrowBtn_, &arraysStopBtn_,
                    &arraysMountBtn_})
        arraysButtonBox_.pack_start(*b, Gtk::PACK_SHRINK);
    arraysTab_.pack_start(arraysButtonBox_, Gtk::PACK_SHRINK);

    arraysRefreshBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::refreshArrays));
    arraysDetailBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onArrayDetailClicked));
    arraysAssembleBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onAssembleDiscoveredClicked));
    arraysCreateBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onCreateArrayClicked));
    arraysReplaceBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onReplaceDriveClicked));
    arraysAddSpareBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onAddSpareClicked));
    arraysFailRemoveBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onFailRemoveClicked));
    arraysGrowBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onGrowArrayClicked));
    arraysStopBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onStopArrayClicked));
    arraysMountBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onArrayMountClicked));

    notebook_.append_page(arraysTab_, "RAID Arrays");
    refreshArrays();
}

void GuiApp::refreshArrays() {
    arraysStore_->clear();
    discoveredInactive_.clear();
    if (!mdadm_.available()) return;

    auto arrays = mdadm_.listArrays();
    for (auto& a : arrays) {
        auto row = *arraysStore_->append();
        row[arrayCols_.device] = a.device;
        row[arrayCols_.level] = a.level;
        row[arrayCols_.size] = human(a.arraySizeBytes);
        row[arrayCols_.state] = a.state + (a.degraded ? "  [DEGRADED]" : "");
        row[arrayCols_.stateColor] = a.degraded ? "#c62828" : "#2e7d32";
        row[arrayCols_.discoveredIndex] = -1;
    }

    // Scans every disk's RAID superblock directly, not just what's currently
    // assembled -- surfaces arrays after a reboot without mdadm.conf, disks moved
    // from another machine, or an array someone stopped earlier.
    for (auto& d : mdadm_.scanForArrays()) {
        if (d.active) continue; // already shown above via listArrays()
        discoveredInactive_.push_back(d);
        auto row = *arraysStore_->append();
        row[arrayCols_.device] = d.preferredDevice;
        row[arrayCols_.level] = d.level.empty() ? "raid?" : d.level;
        row[arrayCols_.size] = std::to_string(d.numDevices) + " member(s)";
        row[arrayCols_.state] = "NOT ASSEMBLED";
        row[arrayCols_.stateColor] = "#8a6d00";
        row[arrayCols_.discoveredIndex] = (int)discoveredInactive_.size() - 1;
    }
}

std::string GuiApp::selectedArrayPath() {
    auto iter = arraysView_.get_selection()->get_selected();
    if (!iter) return "";
    Glib::ustring d = (*iter)[arrayCols_.device];
    return d;
}

bool GuiApp::selectedIsDiscovered(DiscoveredArray* outDisc) {
    auto iter = arraysView_.get_selection()->get_selected();
    if (!iter) return false;
    int idx = (*iter)[arrayCols_.discoveredIndex];
    if (idx < 0) return false;
    if (outDisc && idx < (int)discoveredInactive_.size()) *outDisc = discoveredInactive_[idx];
    return true;
}

bool GuiApp::guardNotDiscovered(const std::string& actionName) {
    if (!selectedIsDiscovered(nullptr)) return false;
    infoDialog(actionName, "This array isn't assembled yet. Select it and click \"Assemble\" first.");
    return true;
}

void GuiApp::onArraysRowActivated(const Gtk::TreeModel::Path&, Gtk::TreeViewColumn*) {
    if (selectedIsDiscovered(nullptr)) onAssembleDiscoveredClicked();
    else onArrayDetailClicked();
}

void GuiApp::onArrayDetailClicked() {
    if (guardNotDiscovered("Detail")) return;
    std::string path = selectedArrayPath();
    if (path.empty()) { infoDialog("RAID", "Select an array first."); return; }
    showArrayDetailDialog(path);
}

void GuiApp::onAssembleDiscoveredClicked() {
    DiscoveredArray disc;
    if (!selectedIsDiscovered(&disc)) { infoDialog("Assemble", "Select a \"NOT ASSEMBLED\" array first."); return; }

    std::ostringstream os;
    os << "Found on-disk but not currently assembled:\n\n"
       << "Suggested name: " << disc.preferredDevice << "\n"
       << "Level: " << (disc.level.empty() ? "(unknown)" : disc.level) << "\n"
       << "Metadata version: " << disc.metadataVersion << "\n"
       << "UUID: " << disc.uuid << "\n"
       << "Members (" << disc.devices.size() << "):\n";
    for (auto& dev : disc.devices) os << "  " << dev << "\n";
    os << "\nAssemble this array now?";
    if (!confirmYesNo("Assemble array", os.str(), Gtk::MESSAGE_QUESTION)) return;

    auto res = mdadm_.assembleArray(disc.preferredDevice, disc.devices);
    infoDialog("Assemble", res.ok() ? "Assembled as " + disc.preferredDevice + "." : ("Failed:\n" + res.stdErr),
               res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
    refreshArrays();
}

void GuiApp::onCreateArrayClicked() {
    showCreateArrayDialog();
    refreshArrays();
}

void GuiApp::onReplaceDriveClicked() {
    if (guardNotDiscovered("Replace drive")) return;
    std::string path = selectedArrayPath();
    if (path.empty()) { infoDialog("RAID", "Select an array first."); return; }
    showReplaceDriveDialog(path);
    refreshArrays();
}

void GuiApp::onAddSpareClicked() {
    if (guardNotDiscovered("Add spare")) return;
    std::string mdPath = selectedArrayPath();
    if (mdPath.empty()) { infoDialog("Add spare", "Select an array first."); return; }
    std::string dev;
    if (!promptInput("Add spare to " + mdPath, "Device path to add as spare (e.g. /dev/sdd1):", "", dev) || dev.empty()) return;
    auto res = mdadm_.addDevice(mdPath, dev);
    infoDialog("Add device", res.ok() ? "Added." : ("Failed:\n" + res.stdErr),
               res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
    refreshArrays();
}

void GuiApp::onFailRemoveClicked() {
    if (guardNotDiscovered("Fail+remove")) return;
    std::string mdPath = selectedArrayPath();
    if (mdPath.empty()) { infoDialog("Fail+remove", "Select an array first."); return; }
    std::string dev;
    if (!promptInput("Fail+remove from " + mdPath, "Device path to fail and remove:", "", dev) || dev.empty()) return;
    if (!confirmYesNo("Fail + remove device",
            "This marks " + dev + " faulty and removes it from " + mdPath + ".")) return;
    mdadm_.failDevice(mdPath, dev);
    auto res = mdadm_.removeDevice(mdPath, dev);
    infoDialog("Remove device", res.ok() ? "Removed." : ("Failed:\n" + res.stdErr),
               res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
    refreshArrays();
}

void GuiApp::onGrowArrayClicked() {
    if (guardNotDiscovered("Grow")) return;
    std::string mdPath = selectedArrayPath();
    if (mdPath.empty()) { infoDialog("Grow", "Select an array first."); return; }
    std::string newDev;
    if (!promptInput("Grow " + mdPath, "Device to add before growing (blank to skip, if already added):", "", newDev)) return;
    if (!newDev.empty()) {
        auto res = mdadm_.addDevice(mdPath, newDev);
        if (!res.ok()) { infoDialog("Add device", "Failed:\n" + res.stdErr, Gtk::MESSAGE_ERROR); return; }
    }
    std::string countStr;
    if (!promptInput("Grow " + mdPath, "New total number of raid-devices:", "", countStr) || countStr.empty()) return;
    int count = 0;
    try { count = std::stoi(countStr); } catch (...) { infoDialog("Grow", "Invalid number.", Gtk::MESSAGE_ERROR); return; }
    if (!confirmYesNo("Grow array", "Growing " + mdPath + " to " + countStr + " devices. This can take a long time.")) return;
    auto res = mdadm_.growRaidDevices(mdPath, count);
    infoDialog("Grow", res.ok() ? "Grow started; check array detail for progress." : ("Failed:\n" + res.stdErr),
               res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
    refreshArrays();
}

void GuiApp::onStopArrayClicked() {
    if (guardNotDiscovered("Stop array")) return;
    std::string mdPath = selectedArrayPath();
    if (mdPath.empty()) { infoDialog("Stop array", "Select an array first."); return; }
    if (!confirmYesNo("Stop array",
            "This stops (deactivates) " + mdPath + ". Data is not lost, but it must be re-assembled to use again.")) return;
    auto res = mdadm_.stopArray(mdPath);
    infoDialog("Stop", res.ok() ? "Stopped." : ("Failed:\n" + res.stdErr),
               res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
    refreshArrays();
}

void GuiApp::onArrayMountClicked() {
    if (guardNotDiscovered("Mount")) return;
    std::string mdPath = selectedArrayPath();
    if (mdPath.empty()) { infoDialog("Mount", "Select an array first."); return; }
    auto devOpt = devices_.find(mdPath);
    if (!devOpt) { infoDialog("Mount", "Could not read device info for " + mdPath, Gtk::MESSAGE_ERROR); return; }
    showMountManageDialog(*devOpt);
    refreshArrays();
}

void GuiApp::showArrayDetailDialog(const std::string& mdPath) {
    Gtk::Dialog dlg("Array: " + mdPath, *this, true);
    dlg.set_default_size(540, 420);
    dlg.add_button("Close", Gtk::RESPONSE_CLOSE);
    auto* content = dlg.get_content_area();
    content->set_spacing(8);
    content->set_border_width(8);

    Gtk::Label summary;
    summary.set_xalign(0.0);
    summary.set_line_wrap(true);
    content->pack_start(summary, Gtk::PACK_SHRINK);

    RaidMemberColumns cols;
    auto store = Gtk::ListStore::create(cols);
    Gtk::TreeView tv(store);
    tv.append_column("Member", cols.device);
    tv.append_column("State", cols.state);
    Gtk::ScrolledWindow scroll;
    scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    scroll.add(tv);
    scroll.set_vexpand(true);
    content->pack_start(scroll, Gtk::PACK_EXPAND_WIDGET);

    Gtk::Label rebuildLabel;
    rebuildLabel.set_xalign(0.0);
    Gtk::ProgressBar bar;
    content->pack_start(rebuildLabel, Gtk::PACK_SHRINK);
    content->pack_start(bar, Gtk::PACK_SHRINK);

    auto refresh = [&]() -> bool {
        auto detOpt = mdadm_.detail(mdPath);
        if (!detOpt) {
            summary.set_text("Array no longer present.");
            return true;
        }
        auto& d = *detOpt;
        std::ostringstream os;
        os << "Level: " << d.level << "   State: " << d.state << (d.degraded ? " [DEGRADED]" : "") << "\n";
        os << "Size: " << human(d.arraySizeBytes) << "   Active/Working/Failed/Spare: "
           << d.activeDevices << "/" << d.workingDevices << "/" << d.failedDevices << "/" << d.spareDevices;
        summary.set_text(os.str());
        store->clear();
        for (auto& m : d.members) {
            auto row = *store->append();
            row[cols.device] = m.device;
            row[cols.state] = m.state;
        }
        auto rebuild = mdadm_.rebuildStatus(mdPath);
        if (rebuild.active) {
            bar.show();
            rebuildLabel.show();
            bar.set_fraction(std::min(1.0, std::max(0.0, rebuild.percent / 100.0)));
            std::ostringstream rs;
            rs << rebuild.operation << ": " << std::fixed << std::setprecision(1) << rebuild.percent
               << "%  speed=" << rebuild.speed << "  eta=" << rebuild.timeRemaining;
            rebuildLabel.set_text(rs.str());
        } else {
            bar.hide();
            rebuildLabel.hide();
        }
        return true;
    };

    dlg.show_all_children();
    refresh();
    sigc::connection timer = Glib::signal_timeout().connect([&]() { return refresh(); }, 1000);

    dlg.run();
    timer.disconnect();
}

void GuiApp::showCreateArrayDialog() {
    if (!mdadm_.available()) { infoDialog("RAID", "mdadm is not installed. Run install.sh first.", Gtk::MESSAGE_ERROR); return; }
    auto disks = devices_.listDisks();
    std::string rootDisk = devices_.rootDiskName();

    Gtk::Dialog dlg("Create RAID Array", *this, true);
    dlg.set_default_size(500, 540);
    auto* content = dlg.get_content_area();
    content->set_spacing(8);
    content->set_border_width(8);

    Gtk::Label pickLabel("Select 2 or more member disks (system disk is excluded):");
    pickLabel.set_xalign(0.0);
    content->pack_start(pickLabel, Gtk::PACK_SHRINK);

    Gtk::ScrolledWindow diskScroll;
    diskScroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    diskScroll.set_size_request(-1, 160);
    Gtk::Box diskBox(Gtk::ORIENTATION_VERTICAL, 2);
    std::vector<std::pair<std::string, Gtk::CheckButton*>> diskChecks;
    for (auto& d : disks) {
        if (!rootDisk.empty() && d.kname == rootDisk) continue;
        std::ostringstream label;
        label << d.path << "  " << human(d.sizeBytes) << "  " << (d.model.empty() ? "" : d.model)
              << (d.mountpoint.empty() ? "" : "  [mounted at " + d.mountpoint + "]");
        auto* cb = Gtk::manage(new Gtk::CheckButton(label.str()));
        diskBox.pack_start(*cb, Gtk::PACK_SHRINK);
        diskChecks.emplace_back(d.path, cb);
    }
    diskScroll.add(diskBox);
    content->pack_start(diskScroll, Gtk::PACK_SHRINK);

    if (diskChecks.size() < 2) {
        infoDialog("Create array", "Need at least 2 non-system disks to create a RAID array.", Gtk::MESSAGE_ERROR);
        return;
    }

    Gtk::Label levelLabel("RAID level:");
    levelLabel.set_xalign(0.0);
    content->pack_start(levelLabel, Gtk::PACK_SHRINK);
    Gtk::ComboBoxText levelCombo;
    levelCombo.append("0", "0 (striping, no redundancy)");
    levelCombo.append("1", "1 (mirror)");
    levelCombo.append("5", "5 (parity, N-1 usable)");
    levelCombo.append("6", "6 (dual parity, N-2 usable)");
    levelCombo.append("10", "10 (mirrored stripes)");
    levelCombo.set_active_id("1");
    content->pack_start(levelCombo, Gtk::PACK_SHRINK);

    Gtk::Label nameLabel("New array device name:");
    nameLabel.set_xalign(0.0);
    content->pack_start(nameLabel, Gtk::PACK_SHRINK);
    Gtk::Entry nameEntry;
    nameEntry.set_text("/dev/md0");
    content->pack_start(nameEntry, Gtk::PACK_SHRINK);

    Gtk::Label warnLabel;
    warnLabel.set_xalign(0.0);
    warnLabel.set_line_wrap(true);
    warnLabel.set_markup("<b>Warning:</b> creating the array DESTROYS ALL DATA on every selected disk.");
    content->pack_start(warnLabel, Gtk::PACK_SHRINK);

    Gtk::Label confirmLabel("Type \"yes\" to enable the Create button:");
    confirmLabel.set_xalign(0.0);
    content->pack_start(confirmLabel, Gtk::PACK_SHRINK);
    Gtk::Entry confirmEntry;
    content->pack_start(confirmEntry, Gtk::PACK_SHRINK);

    dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    auto* createBtn = dlg.add_button("Create", Gtk::RESPONSE_OK);
    createBtn->set_sensitive(false);
    confirmEntry.signal_changed().connect([&confirmEntry, createBtn]() {
        createBtn->set_sensitive(confirmEntry.get_text() == "yes");
    });

    dlg.show_all_children();
    int resp = dlg.run();
    if (resp != Gtk::RESPONSE_OK) return;

    std::vector<std::string> members;
    for (auto& kv : diskChecks)
        if (kv.second->get_active()) members.push_back(kv.first);
    if (members.size() < 2) { infoDialog("Create array", "Select at least 2 disks.", Gtk::MESSAGE_ERROR); return; }
    std::string level = levelCombo.get_active_id();
    if (level.empty()) level = "1";
    std::string mdPath = nameEntry.get_text();
    if (mdPath.empty()) { infoDialog("Create array", "Enter a device name.", Gtk::MESSAGE_ERROR); return; }

    runWithProgressDialog("Creating " + mdPath, [&](std::atomic<bool>&, const std::function<void(double, const std::string&)>& update) {
        update(10, "running mdadm --create...");
        auto res = mdadm_.createArray(mdPath, level, members);
        update(100, res.ok() ? "done" : ("failed: " + res.stdErr));
    });
    infoDialog("Create array", "mdadm --create issued for " + mdPath + ".\nCheck the RAID Arrays tab for sync progress.");
}

void GuiApp::showReplaceDriveDialog(const std::string& mdPath) {
    auto detOpt = mdadm_.detail(mdPath);
    if (!detOpt) { infoDialog("Replace drive", "Could not read array detail for " + mdPath, Gtk::MESSAGE_ERROR); return; }

    Gtk::Dialog dlg("Replace drive in " + mdPath, *this, true);
    dlg.set_default_size(500, 380);
    auto* content = dlg.get_content_area();
    content->set_spacing(8);
    content->set_border_width(8);

    Gtk::Label pickLabel("Select the failing/failed member to replace:");
    pickLabel.set_xalign(0.0);
    content->pack_start(pickLabel, Gtk::PACK_SHRINK);

    RaidMemberColumns cols;
    auto store = Gtk::ListStore::create(cols);
    for (auto& m : detOpt->members) {
        auto report = smart_.query(m.device);
        bool risk = smart_.isAtRisk(report);
        auto row = *store->append();
        row[cols.device] = m.device;
        row[cols.state] = m.state + (risk ? "  [SMART AT-RISK]" : "");
    }
    Gtk::TreeView tv(store);
    tv.append_column("Member", cols.device);
    tv.append_column("State", cols.state);
    Gtk::ScrolledWindow scroll;
    scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    scroll.set_size_request(-1, 140);
    scroll.add(tv);
    content->pack_start(scroll, Gtk::PACK_SHRINK);

    Gtk::Button locateBtn("Locate selected drive (blink activity light)");
    content->pack_start(locateBtn, Gtk::PACK_SHRINK);
    locateBtn.signal_clicked().connect([&, this]() {
        auto iter = tv.get_selection()->get_selected();
        if (!iter) { infoDialog("Locate", "Select a member first."); return; }
        Glib::ustring dev = (*iter)[cols.device];
        auto method = locate_.bestMethodFor(dev);
        if (method == LocateMethod::None) { infoDialog("Locate", "No way to identify this drive was found."); return; }
        locate_.start(dev, 15);
        infoDialog("Locating", "Watch the drive bay for 15 seconds of activity on " + dev + ".");
    });

    Gtk::Label newDevLabel("Path of the REPLACEMENT drive/partition (already inserted):");
    newDevLabel.set_xalign(0.0);
    content->pack_start(newDevLabel, Gtk::PACK_SHRINK);
    Gtk::Entry newDevEntry;
    content->pack_start(newDevEntry, Gtk::PACK_SHRINK);

    dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    dlg.add_button("Replace", Gtk::RESPONSE_OK);
    dlg.show_all_children();
    int resp = dlg.run();
    if (resp != Gtk::RESPONSE_OK) return;

    auto iter = tv.get_selection()->get_selected();
    if (!iter) { infoDialog("Replace drive", "Select a member to replace."); return; }
    Glib::ustring oldDev = (*iter)[cols.device];
    std::string newDev = newDevEntry.get_text();
    if (newDev.empty()) { infoDialog("Replace drive", "Enter the replacement device path."); return; }
    if (!confirmYesNo("Replace drive",
            "Replacing " + oldDev + " with " + newDev + " in " + mdPath + ".\n"
            "Any data currently on " + newDev + " will be destroyed.")) return;

    auto res = mdadm_.replaceDevice(mdPath, oldDev, newDev);
    if (!res.ok()) { infoDialog("Replace drive", "Failed:\n" + res.stdErr, Gtk::MESSAGE_ERROR); return; }

    watchRebuild(mdPath, "Rebuilding " + mdPath);
}

} // namespace dm
