// The window itself: construction, the progress dialog that runs long jobs
// off the UI thread, and the generic prompts. Each tab has its own file.
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

namespace detail {

std::string human(uint64_t n) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; ++u; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(v < 10 ? 2 : 1) << v << units[u];
    return os.str();
}

std::string healthGlyph(const SmartReport& r, bool atRisk) {
    if (!r.supported) return "?";
    if (atRisk) return "!";
    return r.healthy ? "OK" : "FAIL";
}

std::string healthColor(const std::string& glyph) {
    if (glyph == "OK") return "#2e7d32";   // green
    if (glyph == "!") return "#b8860b";    // yellow/amber
    if (glyph == "FAIL") return "#c62828"; // red
    return "#757575";                      // grey ("?")
}

std::string join(const std::vector<std::string>& items, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += sep;
        out += items[i];
    }
    return out;
}

DiskExtra computeDiskExtra(const BlockDevice& d, const std::vector<BlockDevice>& allDevices,
                            const std::vector<RaidArray>& activeArrays,
                            const std::vector<DiscoveredArray>& discoveredArrays) {
    std::vector<std::string> names = {d.path};
    for (auto& c : d.childPaths) names.push_back(c);
    auto contains = [&](const std::string& p) { return std::find(names.begin(), names.end(), p) != names.end(); };

    std::vector<std::string> mounted;
    for (auto& n : names) {
        auto it = std::find_if(allDevices.begin(), allDevices.end(),
                                [&](const BlockDevice& bd) { return bd.path == n; });
        if (it != allDevices.end() && !it->mountpoint.empty()) mounted.push_back(it->mountpoint);
    }

    std::vector<std::string> raidHits;
    for (auto& arr : activeArrays) {
        for (auto& m : arr.members) {
            if (contains(m.device)) { raidHits.push_back(arr.device); break; }
        }
    }
    for (auto& disc : discoveredArrays) {
        if (disc.active) continue; // already covered via activeArrays above
        bool hit = std::any_of(disc.devices.begin(), disc.devices.end(), contains);
        if (hit) raidHits.push_back(disc.preferredDevice + " (not assembled)");
    }

    DiskExtra extra;
    extra.mounted = mounted.empty() ? "-" : join(mounted, ", ");
    extra.raid = raidHits.empty() ? "-" : join(raidHits, ", ");
    return extra;
}

}  // namespace detail

namespace {

// Modal dialog that drives a Gtk::ProgressBar from a worker thread. All
// worker-thread -> UI communication goes through Glib::Dispatcher, which is
// the only thread-safe way to wake up the GTK main loop from another thread;
// the worker must never touch any Gtk::Widget directly.
class ProgressDialog : public Gtk::Dialog {
public:
    ProgressDialog(Gtk::Window& parent, const std::string& title)
        : Gtk::Dialog(title, parent, true) {
        set_default_size(480, 130);
        get_content_area()->set_spacing(8);
        get_content_area()->set_border_width(10);
        label_.set_line_wrap(true);
        label_.set_xalign(0.0);
        label_.set_text("starting...");
        get_content_area()->pack_start(label_, Gtk::PACK_SHRINK);
        bar_.set_show_text(false);
        get_content_area()->pack_start(bar_, Gtk::PACK_SHRINK);
        add_button("Cancel", Gtk::RESPONSE_CANCEL);
        show_all_children();
        dispatcher_.connect(sigc::mem_fun(*this, &ProgressDialog::onDispatch));
    }

    std::atomic<bool>& cancelFlag() { return cancel_; }

    // Safe to call from the worker thread.
    void update(double percent, const std::string& msg) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            pendingPercent_ = percent;
            pendingMsg_ = msg;
        }
        dispatcher_.emit();
    }

    // Safe to call from the worker thread; signals completion.
    void finish() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            done_ = true;
        }
        dispatcher_.emit();
    }

protected:
    void on_response(int response_id) override {
        if (response_id == Gtk::RESPONSE_CANCEL) cancel_.store(true);
        Gtk::Dialog::on_response(response_id);
    }

private:
    void onDispatch() {
        double p;
        std::string m;
        bool d;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            p = pendingPercent_;
            m = pendingMsg_;
            d = done_;
        }
        label_.set_text(m);
        bar_.set_fraction(std::min(1.0, std::max(0.0, p / 100.0)));
        if (d) response(Gtk::RESPONSE_OK);
    }

    Gtk::Label label_;
    Gtk::ProgressBar bar_;
    Glib::Dispatcher dispatcher_;
    std::mutex mutex_;
    double pendingPercent_ = 0.0;
    std::string pendingMsg_;
    bool done_ = false;
    std::atomic<bool> cancel_{false};
};

} // namespace

// ============================================================ construction

GuiApp::GuiApp() {
#ifdef DM_VERSION
    set_title("disk-manager " DM_VERSION);
#else
    set_title("disk-manager");
#endif
    set_icon_name("disk-manager");
    set_default_size(950, 620);
    set_border_width(4);
    add(vbox_);

    rootInfoBar_.set_message_type(Gtk::MESSAGE_WARNING);
    rootInfoBar_.set_show_close_button(true);
    rootInfoLabel_.set_line_wrap(true);
    rootInfoLabel_.set_xalign(0.0);
    rootInfoLabel_.set_text(
        "Not running as root. Viewing disks and SMART data works read-only, but "
        "partitioning, formatting, mounting, RAID management, firmware updates, "
        "and recovery all require root. Re-run with sudo for full functionality.");
    if (auto* box = dynamic_cast<Gtk::Box*>(rootInfoBar_.get_content_area()))
        box->pack_start(rootInfoLabel_, Gtk::PACK_SHRINK);
    rootInfoBar_.signal_response().connect([this](int) { rootInfoBar_.hide(); });
    vbox_.pack_start(rootInfoBar_, Gtk::PACK_SHRINK);

    vbox_.pack_start(notebook_, Gtk::PACK_EXPAND_WIDGET);

    buildDisksTab();
    buildArraysTab();
    buildFirmwareTab();
    buildRecoveryTab();

    show_all_children();
    if (isRoot()) rootInfoBar_.hide();
}

void GuiApp::runWithProgressDialog(const std::string& title,
        const std::function<void(std::atomic<bool>& cancel,
                                  const std::function<void(double, const std::string&)>& update)>& work) {
    ProgressDialog dlg(*this, title);
    std::thread worker([&]() {
        work(dlg.cancelFlag(), [&](double p, const std::string& s) { dlg.update(p, s); });
        dlg.finish();
    });
    dlg.run();
    if (worker.joinable()) worker.join();
}

void GuiApp::watchRebuild(const std::string& mdPath, const std::string& title) {
    runWithProgressDialog(title, [&](std::atomic<bool>& cancel, const std::function<void(double, const std::string&)>& update) {
        while (!cancel.load()) {
            auto rb = mdadm_.rebuildStatus(mdPath);
            if (!rb.active) { update(100, "rebuild complete or not reported by mdstat"); return; }
            std::ostringstream os;
            os << rb.operation << "  speed=" << rb.speed << "  eta=" << rb.timeRemaining;
            update(rb.percent, os.str());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        update(0, "monitoring cancelled (rebuild continues in the background)");
    });
}

// ========================================================= generic dialogs

bool GuiApp::confirmYesNo(const std::string& title, const std::string& message, Gtk::MessageType type) {
    Gtk::MessageDialog dlg(*this, message, false, type, Gtk::BUTTONS_YES_NO, true);
    dlg.set_title(title);
    int resp = dlg.run();
    return resp == Gtk::RESPONSE_YES;
}

void GuiApp::infoDialog(const std::string& title, const std::string& message, Gtk::MessageType type) {
    Gtk::MessageDialog dlg(*this, message, false, type, Gtk::BUTTONS_OK, true);
    dlg.set_title(title);
    dlg.run();
}

bool GuiApp::promptInput(const std::string& title, const std::string& label, const std::string& initial, std::string& out) {
    Gtk::Dialog dlg(title, *this, true);
    dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    dlg.add_button("OK", Gtk::RESPONSE_OK);
    dlg.set_default_response(Gtk::RESPONSE_OK);

    auto* content = dlg.get_content_area();
    content->set_spacing(6);
    content->set_border_width(8);
    Gtk::Label lbl(label);
    lbl.set_xalign(0.0);
    lbl.set_line_wrap(true);
    Gtk::Entry entry;
    entry.set_text(initial);
    entry.set_activates_default(true);
    content->pack_start(lbl, Gtk::PACK_SHRINK);
    content->pack_start(entry, Gtk::PACK_SHRINK);

    dlg.show_all_children();
    int resp = dlg.run();
    if (resp == Gtk::RESPONSE_OK) {
        out = entry.get_text();
        return true;
    }
    return false;
}

} // namespace dm
