// The Recovery tab: ddrescue, and testdisk/photorec in a terminal.
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

// ============================================================= Recovery tab

void GuiApp::buildRecoveryTab() {
    recoveryTab_.set_border_width(8);

    ddrescueGrid_.set_row_spacing(6);
    ddrescueGrid_.set_column_spacing(6);
    ddrescueGrid_.set_border_width(8);

    auto* srcLabel = Gtk::manage(new Gtk::Label("Source device (disk, partition, or /dev/mdX):"));
    srcLabel->set_xalign(0.0);
    ddrescueGrid_.attach(*srcLabel, 0, 0, 1, 1);
    ddrescueSourceEntry_.set_hexpand(true);
    ddrescueGrid_.attach(ddrescueSourceEntry_, 1, 0, 2, 1);

    auto* destLabel = Gtk::manage(new Gtk::Label("Destination image file:"));
    destLabel->set_xalign(0.0);
    ddrescueGrid_.attach(*destLabel, 0, 1, 1, 1);
    ddrescueDestEntry_.set_hexpand(true);
    ddrescueGrid_.attach(ddrescueDestEntry_, 1, 1, 1, 1);
    ddrescueGrid_.attach(ddrescueBrowseBtn_, 2, 1, 1, 1);

    auto* mapLabel = Gtk::manage(new Gtk::Label("Mapfile path (for resuming if interrupted):"));
    mapLabel->set_xalign(0.0);
    ddrescueGrid_.attach(*mapLabel, 0, 2, 1, 1);
    ddrescueMapEntry_.set_hexpand(true);
    ddrescueGrid_.attach(ddrescueMapEntry_, 1, 2, 2, 1);

    ddrescueGrid_.attach(ddrescueStartBtn_, 1, 3, 1, 1);

    ddrescueFrame_.add(ddrescueGrid_);
    recoveryTab_.pack_start(ddrescueFrame_, Gtk::PACK_SHRINK);

    testdiskBox_.set_border_width(8);
    testdiskBox_.pack_start(launchTestdiskBtn_, Gtk::PACK_SHRINK);
    testdiskBox_.pack_start(launchPhotorecBtn_, Gtk::PACK_SHRINK);
    testdiskFrame_.add(testdiskBox_);
    recoveryTab_.pack_start(testdiskFrame_, Gtk::PACK_SHRINK);

    // Keep the mapfile suggestion in sync with the destination path, mirroring
    // the TUI's default of "<dest>.mapfile".
    ddrescueDestEntry_.signal_changed().connect([this]() {
        std::string dest = ddrescueDestEntry_.get_text();
        if (!dest.empty()) ddrescueMapEntry_.set_text(dest + ".mapfile");
    });

    ddrescueBrowseBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onDdrescueBrowseClicked));
    ddrescueStartBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onDdrescueStartClicked));
    launchTestdiskBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onLaunchTestdiskClicked));
    launchPhotorecBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onLaunchPhotorecClicked));

    notebook_.append_page(recoveryTab_, "Data Recovery");
}

void GuiApp::onDdrescueBrowseClicked() {
    // Gtk::FileChooserButton officially only supports ACTION_OPEN /
    // ACTION_SELECT_FOLDER, not ACTION_SAVE, so we use a FileChooserDialog in
    // save mode directly (Browse... button next to a plain Entry) instead.
    Gtk::FileChooserDialog chooser(*this, "Select destination image file", Gtk::FILE_CHOOSER_ACTION_SAVE);
    chooser.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    chooser.add_button("Select", Gtk::RESPONSE_OK);
    chooser.set_do_overwrite_confirmation(true);
    if (chooser.run() == Gtk::RESPONSE_OK) {
        ddrescueDestEntry_.set_text(chooser.get_filename());
    }
}

void GuiApp::onDdrescueStartClicked() {
    if (!recovery_.ddrescueAvailable()) { infoDialog("Recovery", "ddrescue is not installed. Run install.sh first.", Gtk::MESSAGE_ERROR); return; }
    std::string source = ddrescueSourceEntry_.get_text();
    std::string dest = ddrescueDestEntry_.get_text();
    std::string mapFile = ddrescueMapEntry_.get_text();
    if (source.empty() || dest.empty()) { infoDialog("Recovery", "Source device and destination image path are required."); return; }
    if (mapFile.empty()) mapFile = dest + ".mapfile";
    if (!confirmYesNo("Image with ddrescue",
            "Imaging " + source + " -> " + dest + ".\nThis reads " + source + " but never writes to it.")) return;

    runWithProgressDialog("ddrescue " + source,
        [&, source, dest, mapFile](std::atomic<bool>& cancel, const std::function<void(double, const std::string&)>& update) {
            recovery_.rescue(source, dest, mapFile, [&](const RescueProgress& p) {
                std::ostringstream os;
                os << p.currentStatus << "  rescued=" << human(p.rescuedBytes) << "/" << human(p.totalBytes)
                   << "  errors=" << p.errorCount;
                update(p.percent, os.str());
            }, &cancel);
        });
    infoDialog("ddrescue", "Finished (or cancelled). Mapfile kept at:\n" + mapFile + "\nRe-run with the same mapfile to resume.");
}

// testdisk/photorec are full-screen (n)curses programs that need a real
// controlling terminal; we can't embed them in a GTK widget, so -- per the
// task brief -- we spawn them in the user's terminal emulator instead. This
// is a known, documented limitation of this first GUI version.
void GuiApp::spawnInTerminal(const std::vector<std::string>& innerArgv) {
    std::string term;
    if (commandExists("x-terminal-emulator")) term = "x-terminal-emulator";
    else if (commandExists("xterm")) term = "xterm";
    else { infoDialog("Launch", "No terminal emulator found (tried x-terminal-emulator, xterm).", Gtk::MESSAGE_ERROR); return; }

    std::vector<std::string> argv = {term, "-e"};
    for (auto& a : innerArgv) argv.push_back(a);

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    GError* error = nullptr;
    gboolean ok = g_spawn_async(nullptr, cargv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error);
    if (!ok) {
        std::string msg = (error && error->message) ? error->message : "unknown error";
        if (error) g_error_free(error);
        infoDialog("Launch", "Failed to launch terminal: " + msg, Gtk::MESSAGE_ERROR);
    }
}

void GuiApp::onLaunchTestdiskClicked() {
    if (!recovery_.testdiskAvailable()) { infoDialog("Recovery", "testdisk is not installed. Run install.sh first.", Gtk::MESSAGE_ERROR); return; }
    std::string target;
    if (!promptInput("Launch testdisk", "Target device (blank = let testdisk choose):", "", target)) return;
    std::vector<std::string> argv = {"testdisk"};
    if (!target.empty()) argv.push_back(target);
    spawnInTerminal(argv);
}

void GuiApp::onLaunchPhotorecClicked() {
    if (!recovery_.testdiskAvailable()) { infoDialog("Recovery", "photorec is not installed. Run install.sh first.", Gtk::MESSAGE_ERROR); return; }
    std::string target;
    if (!promptInput("Launch photorec", "Target device (blank = let photorec choose):", "", target)) return;
    std::vector<std::string> argv = {"photorec"};
    if (!target.empty()) argv.push_back(target);
    spawnInTerminal(argv);
}

} // namespace dm
