// The Firmware tab: available updates and applying one.
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

// ============================================================= Firmware tab

void GuiApp::buildFirmwareTab() {
    firmwareTab_.set_border_width(6);
    firmwareAvailable_ = firmware_.fwupdAvailable();
    if (!firmwareAvailable_) {
        firmwareUnavailableLabel_.set_line_wrap(true);
        firmwareUnavailableLabel_.set_text(
            "fwupdmgr is not installed or not available on this system.\n"
            "Firmware update management is disabled. Install fwupd (e.g. via install.sh) "
            "to enable this tab.");
        firmwareTab_.pack_start(firmwareUnavailableLabel_, Gtk::PACK_SHRINK);
        notebook_.append_page(firmwareTab_, "Firmware");
        return;
    }

    firmwareStore_ = Gtk::ListStore::create(firmwareCols_);
    firmwareView_.set_model(firmwareStore_);
    firmwareView_.append_column("Device", firmwareCols_.devicePath);
    firmwareView_.append_column("Vendor / Name", firmwareCols_.vendorName);
    firmwareView_.append_column("Current", firmwareCols_.current);
    firmwareView_.append_column("Latest", firmwareCols_.latest);
    firmwareView_.append_column("Update available", firmwareCols_.updateAvailable);
    firmwareView_.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &GuiApp::onFirmwareSelectionChanged));

    firmwareScroll_.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    firmwareScroll_.add(firmwareView_);
    firmwareScroll_.set_vexpand(true);
    firmwareTab_.pack_start(firmwareScroll_, Gtk::PACK_EXPAND_WIDGET);

    firmwareButtonBox_.pack_start(firmwareRefreshBtn_, Gtk::PACK_SHRINK);
    firmwareButtonBox_.pack_start(firmwareUpdateBtn_, Gtk::PACK_SHRINK);
    firmwareTab_.pack_start(firmwareButtonBox_, Gtk::PACK_SHRINK);
    firmwareUpdateBtn_.set_sensitive(false);

    firmwareRefreshBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onFirmwareRefreshClicked));
    firmwareUpdateBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onFirmwareUpdateClicked));

    notebook_.append_page(firmwareTab_, "Firmware");
    refreshFirmware(false);
}

void GuiApp::refreshFirmware(bool refreshFromNetwork) {
    if (!firmwareAvailable_) return;
    if (refreshFromNetwork) {
        runWithProgressDialog("Refreshing firmware metadata",
            [&](std::atomic<bool>&, const std::function<void(double, const std::string&)>& update) {
                update(50, "fwupdmgr refresh...");
                auto res = firmware_.refreshMetadata();
                update(100, res.ok() ? "done" : "failed (continuing with cached metadata)");
            });
    }
    auto disks = devices_.listDisks();
    std::vector<std::string> paths;
    for (auto& d : disks) paths.push_back(d.path);
    firmwareInfos_ = firmware_.listStorageDevices(paths);

    firmwareStore_->clear();
    for (size_t i = 0; i < firmwareInfos_.size(); ++i) {
        auto& f = firmwareInfos_[i];
        auto row = *firmwareStore_->append();
        row[firmwareCols_.index] = static_cast<int>(i);
        row[firmwareCols_.devicePath] = f.devicePath.empty() ? "(unmatched device)" : f.devicePath;
        row[firmwareCols_.vendorName] = f.vendor + " " + f.name;
        row[firmwareCols_.current] = f.currentVersion;
        row[firmwareCols_.latest] = f.latestVersion;
        row[firmwareCols_.updateAvailable] = f.updateAvailable ? "Yes" : "No";
    }
    firmwareUpdateBtn_.set_sensitive(false);
    if (firmwareInfos_.empty()) {
        infoDialog("Firmware", "No fwupd-manageable storage devices found on this system.");
    }
}

void GuiApp::onFirmwareRefreshClicked() {
    refreshFirmware(true);
}

void GuiApp::onFirmwareSelectionChanged() {
    auto iter = firmwareView_.get_selection()->get_selected();
    if (!iter) { firmwareUpdateBtn_.set_sensitive(false); return; }
    int idx = (*iter)[firmwareCols_.index];
    bool avail = idx >= 0 && idx < (int)firmwareInfos_.size() && firmwareInfos_[idx].updateAvailable;
    firmwareUpdateBtn_.set_sensitive(avail);
}

void GuiApp::onFirmwareUpdateClicked() {
    auto iter = firmwareView_.get_selection()->get_selected();
    if (!iter) return;
    int idx = (*iter)[firmwareCols_.index];
    if (idx < 0 || idx >= (int)firmwareInfos_.size()) return;
    auto& f = firmwareInfos_[idx];
    if (!f.updateAvailable) { infoDialog("Firmware", "No update available for " + f.name + "."); return; }
    std::ostringstream warn;
    warn << "Updating firmware on " << f.name << " (" << f.currentVersion << " -> " << f.latestVersion << ").\n"
         << "Do not power off or remove this drive during the update -- an\n"
         << "interrupted firmware write can permanently damage the drive.";
    if (!confirmYesNo("Firmware update", warn.str())) return;

    runWithProgressDialog("Updating firmware on " + f.name,
        [&](std::atomic<bool>&, const std::function<void(double, const std::string&)>& update) {
            update(20, "fwupdmgr update...");
            auto res = firmware_.applyUpdate(f.fwupdDeviceId);
            update(100, res.ok() ? "done" : ("failed: " + res.stdErr));
        });
    infoDialog("Firmware update", "Update issued (a reboot may be required). Check status with Refresh metadata.");
    refreshFirmware(false);
}

} // namespace dm
