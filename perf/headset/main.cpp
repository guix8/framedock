// Frame Dock: Frame Perf Overlay (the performance HUD and its dashboard tab) with FrameColor's display controls as
// the tab's Color page. Perf reads only; the Color page writes FrameColor's files (~/.config/framecolor) and the
// SteamVR settings FrameColor wrote. No sysfs writes, no sudo, no camera access.
#include "async_sampler.h"
#include "battery_watch.h"
#include "autostart.h"
#include "bench.h"         // src/: FrameColor's --bench and --bench-compare
#include "browser_controller.h"  // browser/: the browser panel (Phase 3)
#include "browser_loop.h"
#include "browser_view.h"
#include "browser_worker.h"
#include "chrome_process.h"
#include "color_model.h"   // src/: the Color page
#include "color_openvr.h"
#include "gaze_feed.h"
#include "color_page.h"
#include "config.h"
#include "dock.h"
#include "dock_loop.h"
#include "dock_math.h"
#include "dashboard_preview.h"
#include "camera_switch.h"  // the 기기 page's backends
#include "device_backend.h"
#include "device_page.h"   // the 기기 page
#include "dock_view.h"
#include "draw.h"
#include "dump_metrics.h"
#include "framecorder.h"   // 녹화: framecorder's recorder, when installed
#include "frame_sync_worker.h"
#include "grid_renderer.h"
#include "hide_rules.h"
#include "i18n.h"
#include "json.h"
#include "layout.h"
#include "layout_edit.h"
#include "media_strip.h"
#include "notice_view.h"
#include "layout_authority.h"
#include "metric_store.h"
#include "paths.h"
#include "placement.h"
#include "profile_store.h"
#include "pulse_audio.h"
#include "room_view.h"
#include "scheduler.h"
#include "sensor_bridge.h"
#include "sensors.h"
#include "settings_panel.h"
#include "theme.h"
#include "vr_keyboard.h"
#include "vr_overlay.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// The version comes from project(VERSION) in the root CMakeLists.txt
#ifndef FRAMEDOCK_VERSION
#define FRAMEDOCK_VERSION "unknown"
#endif

namespace {

volatile std::sig_atomic_t gStopRequested = 0;
volatile std::sig_atomic_t gToggleRequested = 0;  ///< SIGUSR1 (sent by a second launch): show / hide the panel
volatile std::sig_atomic_t gDumpRequested = 0;    ///< SIGUSR2 (sent by --debug-dump): write the debug dump
int gWakeFd = -1;  ///< eventfd the signal handlers poke, so poll() in the main loop returns at once

/** Exit code when the user quits from the dashboard (systemd's RestartPreventExitStatus=). */
constexpr int kExitCodeUserQuit = 3;

constexpr int kThumbnailSize = 256;              ///< dashboard thumbnail side (px)
constexpr double kSettingsPollSec = 0.05;        ///< input deadline while the pointer is active on the dashboard tab
constexpr double kSettingsIdlePollSec = 0.1;     ///< input deadline while the tab is visible but idle
constexpr double kSettingsActiveHoldSec = 3.0;   ///< the pointer counts as active this long after its last event
constexpr double kMetricsTickSec = 0.5;          ///< metrics deadline: events, config, demand, due sources (2 Hz)
constexpr double kAutostartRefreshSec = 3.0;     ///< re-read the autostart state this often while the tab is visible
constexpr double kPlacementCoalesceSec = 0.02;   ///< do a placement check this early if we're awake anyway
constexpr double kCoalesceSec = 0.03;  ///< the metrics tick, a panel redraw and the colour model's work: this early too
constexpr double kHeldSaveSec = 0.5;             ///< a held button saves config.json at most this often
// The Dock (in-game interaction)
constexpr double kDockIdlePollSec = 0.1;     ///< collapsed, nothing pointed at: check whether a laser points at the HUD
constexpr double kDockActivePollSec = 0.05;  ///< open, pointed at or carried: aim, grip and the panels' events
constexpr double kAimArriveMarginM = 0.005;  ///< the laser takes a panel within this of its edge...
constexpr double kAimLeaveMarginM = 0.03;    ///< ...and keeps it until this far out (no flicker at the edge)
constexpr double kBrowserActivePollSec = 0.025;  ///< the laser on the browser panel or its keyboard: drags, hover
constexpr double kBrowserShutdownSec = 5.0;      ///< at exit: Chrome closes within this (then SIGKILL)

/** @return true for a Placement page button that moves or changes the HUD (starts the Dock's fine-adjust session) */
bool changesHudPlacement(SettingsAction a) {
    if (a >= SettingsAction::AttachHead && a <= SettingsAction::FadeAngleUp) return true;
    return a == SettingsAction::SizeDown || a == SettingsAction::SizeUp || a == SettingsAction::AlphaDown ||
           a == SettingsAction::AlphaUp || a == SettingsAction::Reset;
}

/** @return the Dock panel's offset from its anchor (the HUD's attachment) */
dockmath::Mat34 dockLocal(const Config& config) {
    switch (config.attachment) {
        case Attachment::LeftWrist: return wristTransform(config.dock.leftWrist);
        case Attachment::RightWrist: return wristTransform(config.dock.rightWrist);
        case Attachment::Head: break;
    }
    return dockmath::headTransform(config.dock.head);
}

const char* collapseReasonName(CollapseReason reason) {
    switch (reason) {
        case CollapseReason::HudClick: return "the HUD clicked";
        case CollapseReason::Button: return "접기";
        case CollapseReason::Timeout: return "no use for a while";
        case CollapseReason::HudHidden: return "the HUD hidden or the headset off";
        case CollapseReason::Disabled: return "in-game use turned off";
        case CollapseReason::Browser: return "the browser panel opened";
        case CollapseReason::None: break;
    }
    return "?";
}

/** The browser panel's page side: each call goes to Chrome's thread (nothing while Chrome was never started). */
class WorkerInput : public BrowserInput {
public:
    explicit WorkerInput(std::unique_ptr<BrowserWorker>& worker) : worker_(worker) {}
    void navigate(const std::string& text) override {
        post([text](BrowserSession& s, double) { s.navigate(text); });
    }
    void back() override { post([](BrowserSession& s, double) { s.back(); }); }
    void forward() override { post([](BrowserSession& s, double) { s.forward(); }); }
    void reload() override { post([](BrowserSession& s, double t) { s.reload(t); }); }
    void mouse(BrowserSession::Mouse kind, double x, double y) override {
        post([kind, x, y](BrowserSession& s, double t) { s.mouse(kind, x, y, t); });
    }
    void wheel(double x, double y, double dx, double dy) override {
        post([x, y, dx, dy](BrowserSession& s, double) { s.wheel(x, y, dx, dy); });
    }
    void insertText(const std::string& text) override {
        post([text](BrowserSession& s, double) { s.insertText(text); });
    }
    void key(cdp::Key key) override { post([key](BrowserSession& s, double) { s.key(key); }); }
    void answerDialog(bool accept) override { post([accept](BrowserSession& s, double) { s.answerDialog(accept); }); }
    void contextMenu(double x, double y, int seq) override {
        post([x, y, seq](BrowserSession& s, double) { s.contextMenu(x, y, seq); });
    }

private:
    std::unique_ptr<BrowserWorker>& worker_;
    void post(std::function<void(BrowserSession&, double)> fn) {
        if (worker_) worker_->post(std::move(fn));
    }
};

/** @return the next zoom step up (dir > 0) or down from pct (kBrowserZoomSteps; stays at the ends) */
int stepZoom(int pct, int dir) {
    const int* first = std::begin(kBrowserZoomSteps);
    const int* last = std::end(kBrowserZoomSteps);
    if (dir > 0) {
        const int* next = std::upper_bound(first, last, pct);
        return next != last ? *next : *(last - 1);
    }
    const int* at = std::lower_bound(first, last, pct);
    return at != first ? *(at - 1) : *first;
}

/** A laser event as the browser panel's controller takes it */
PanelPointer toPanelPointer(const PointerInput& input) {
    PanelPointer p;
    switch (input.type) {
        case PointerInput::Type::Move: p.type = PanelPointer::Type::Move; break;
        case PointerInput::Type::Down:
            p.type = input.secondary ? PanelPointer::Type::SecondaryDown : PanelPointer::Type::Down;
            break;
        case PointerInput::Type::Up:
            p.type = input.secondary ? PanelPointer::Type::SecondaryUp : PanelPointer::Type::Up;
            break;
        case PointerInput::Type::Leave: p.type = PanelPointer::Type::Leave; break;
        case PointerInput::Type::Scroll: p.type = PanelPointer::Type::Scroll; break;
    }
    p.x = input.x;
    p.y = input.y;
    p.scrollDx = input.scrollDx;
    p.scrollDy = input.scrollDy;
    return p;
}

/** Command line. */
struct Options {
    enum class Mode {
        Overlay, Print, DumpPng, DumpSettingsPng, DumpFrameTimings, DebugDump, ContrastReport, Help, Version,
        DumpMetrics, RoomView,
    };
    DumpMetricsOptions dumpMetrics;  // --dump-metrics
    Mode mode = Mode::Overlay;
    std::string roomView;  ///< --room-view: on, off or probe
    std::string configPath;
    std::string pngPath;
    std::string thumbnailPngPath;
    std::string language;     ///< PNG dumps only: ja / en / ko instead of the config's language
    int thumbnailSize = 256;  ///< side of the --thumbnail-png image (px)
    int printCount = 5;
    double dumpSeconds = 4.0;
    double timingSeconds = 5.0;
    bool timingSecondsGiven = false;
    bool secondsGiven = false;
    bool fakeThrottle = false;
    bool previewQuit = false;            ///< --dump-settings-png: draw the "press again to quit" state
    std::string previewColor;            ///< --dump-settings-png --preview-page color: "busy" = the long labels
    std::string previewDevice;           ///< --dump-settings-png --preview-page device: previewDeviceState's variant
    std::string previewRecord;           ///< --dump-png: the 녹화 card's made-up state ("" = idle); the tab's
                                         ///< palette offers 녹화 with any
    bool forceSettingsVisible = false;   ///< diagnostics: run as if the dashboard tab were visible
    std::string previewAutostart;        ///< --dump-settings-png: draw autostart in this state
    std::string previewPage;             ///< --dump-settings-png: placement (default), layout, profiles, color, dock, browser, device
    int previewSelectColumn = -1;        ///< --dump-settings-png --preview-page layout: tap this card first
    int previewSelectIndex = -1;
    bool fakeFrames = false;
    bool fakeWarnings = false;
    double fakeFps = 0.0;
    std::string fakeWifi;  ///< non-empty: fake the wireless link (dBm, "home:dBm" or "none")
    std::string layoutPath;  ///< --dump-png: draw this layout (a JSON file: a layout object or a whole config)
    int benchCount = 20;     ///< --dump-png: renders to time at the end
    bool verbose = false;
};

void onSignal(int /*signal*/) {
    gStopRequested = 1;
    const uint64_t one = 1;
    if (gWakeFd >= 0) (void)!::write(gWakeFd, &one, sizeof(one));
}

void onToggleSignal(int /*signal*/) {
    gToggleRequested = 1;
    const uint64_t one = 1;
    if (gWakeFd >= 0) (void)!::write(gWakeFd, &one, sizeof(one));
}

void onDumpSignal(int /*signal*/) {
    gDumpRequested = 1;
    const uint64_t one = 1;
    if (gWakeFd >= 0) (void)!::write(gWakeFd, &one, sizeof(one));
}

/** SIGTERM / SIGINT stop cleanly, SIGUSR1 toggles the panel, SIGUSR2 writes the debug dump. */
void installSignalHandlers() {
    gWakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    const auto install = [](int signal, void (*handler)(int)) {
        struct sigaction action {};
        action.sa_handler = handler;
        sigemptyset(&action.sa_mask);
        sigaction(signal, &action, nullptr);
    };
    install(SIGUSR1, onToggleSignal);
    install(SIGUSR2, onDumpSignal);
    install(SIGTERM, onSignal);
    install(SIGINT, onSignal);
}

/** @return monotonic seconds */
double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/** @return a steady_clock time point in nowSeconds()'s scale (+infinity for time_point::max()) */
double steadySeconds(std::chrono::steady_clock::time_point t) {
    if (t == std::chrono::steady_clock::time_point::max()) return std::numeric_limits<double>::infinity();
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

/** @return CPU time used by this thread, seconds (for --verbose) */
double cpuSeconds() {
    struct timespec ts {};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

/**
 * Sleeps in one poll() until the timeout or a signal.
 * @param seconds timeout; +infinity waits until something happens
 */
void waitForWake(double seconds) {
    if (gStopRequested || gToggleRequested || gDumpRequested) return;
    struct pollfd fds[1] = {{gWakeFd, POLLIN, 0}};
    int timeoutMs = -1;
    if (std::isfinite(seconds)) timeoutMs = static_cast<int>(std::ceil(std::max(0.0, seconds) * 1000.0));
    const int n = ::poll(fds, 1, timeoutMs);
    uint64_t value = 0;
    if (n > 0 && (fds[0].revents & POLLIN)) (void)!::read(gWakeFd, &value, sizeof(value));
}

/** Waits until the time is up or a stop is requested (for --print and --dump-png). */
void sleepInterruptible(double seconds) {
    const double end = nowSeconds() + seconds;
    while (!gStopRequested) {
        const double left = end - nowSeconds();
        if (left <= 0) break;
        waitForWake(left);
        gToggleRequested = 0;
        gDumpRequested = 0;
    }
}

/** @return local time as "YYYY-MM-DD HH:MM:SS" */
std::string localTimeText() {
    const std::time_t t = std::time(nullptr);
    std::tm tm {};
    localtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return buffer;
}

/** @return the value with the given decimals, "--" for NaN */
std::string fmt(double value, int decimals) {
    if (std::isnan(value)) return "--";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

void printUsage() {
    std::printf(
        "Usage: framedock [options]\n"
        "  (none)               run as a SteamVR overlay (waits for SteamVR if it isn't running)\n"
        "  --bench [S]          FrameColor: measure compositor frame timing and memory for S seconds (1-60, default 8)\n"
        "                       while the correction runs (connects to SteamVR as a Background app)\n"
        "  --bench-compare A B [PAIRS]  FrameColor: alternate sharpness A and B (whole percent 0-100) every 2 s,\n"
        "                       PAIRS times (5-60, default 12), and compare vrcompositor's GPU time\n"
        "  --print [--count N]  without OpenVR, print the readings every second, N times (default 5), and exit\n"
        "  --dump-png PATH      without OpenVR, render the panel to a PNG and exit\n"
        "      --layout FILE    draw this layout instead of the config's (a layout object, or a whole config)\n"
        "      --bench N        then time N more renders and print the average and max (default 20)\n"
        "      --seconds S      collect readings for S seconds first (default 4) so the graphs have data\n"
        "      --fake-frames    fill in fake frame timings and controllers (to check the look)\n"
        "      --fake-warnings  fake bad values (thermal limit, weak signal, low battery) to check warning colours\n"
        "      --fake-fps N     fake the app's fps around N, to see what a drop looks like\n"
        "      --fake-wifi D    fake the dongle link (D = signal in dBm, none = not connected, home:D = home Wi-Fi)\n"
        "      --fake-throttle  fake SteamVR throttling the app to half rate (36 fps at 72 Hz, 50%% reprojection)\n"
        "      --preview-record S  draw a Record card (framecorder) in this state: idle (default), recording,\n"
        "                       saving, failed or busy (made up; nothing is started)\n"
        "  --dump-settings-png PATH  without OpenVR, render the dashboard tab to a PNG and exit\n"
        "      --thumbnail-png PATH  also write the dashboard thumbnail (same image as the launcher icon)\n"
        "      --thumbnail-size N    side of that thumbnail (default 256)\n"
        "      --preview-quit   draw the \"press again to quit\" state\n"
        "      --preview-autostart S  draw autostart in this state (enabled / disabled / notinstalled / busy / failed)\n"
        "      --preview-page P       placement (default), layout, profiles, color, dock, browser or device\n"
        "      --preview-select C,I   with --preview-page layout: select the card at column C, index I (from 0)\n"
        "      --preview-color busy   with --preview-page color: auto compare running, save mode, reset armed\n"
        "                             (gaze: follow gaze, gaze-fit: a fit running, gaze-review: a fit just done,\n"
        "                             gaze-failed: a fit that failed, a dot unsteady)\n"
        "      --preview-device V     with --preview-page device: single, unavailable, sync-pc-off, sync-switching,\n"
        "                             sync-confirm or sync-error (made-up state; nothing is read or sent)\n"
        "      --preview-record S     the Layout page's palette offers Record (as with framecorder installed)\n"
        "  --language ja|en|ko  for the PNGs: draw in this language instead of the config's\n"
        "  --config PATH        config file (default ~/.config/frame-perf-overlay/config.json)\n"
        "  --verbose            while running as an overlay, log the values every few seconds\n"
        "  --force-settings-visible  diagnostics: run as if the dashboard tab were visible (to measure the cost)\n"
        "  --debug-dump         print the running instance's debug dump (sources, reference counts, read counters,\n"
        "                       every metric with its status) and exit\n"
        "  --dump-metrics [--count N]  without the overlay, read every enabled widget's sources once a second and\n"
        "                       print the metrics and read counters N times (default 5)\n"
        "      --widgets LIST   enabled widgets by number, e.g. 1,4,9 (default all)\n"
        "      --vr             also connect to SteamVR as a Background app (frame timing, controllers)\n"
        "  --contrast-report    print the WCAG contrast ratio of every text/background colour pair\n"
        "  --dump-frame-timings [S]  diagnostics: connect to SteamVR and print raw frame timings for S seconds\n"
        "                       (default 5). Read-only (Background app type), so it can run next to the overlay\n"
        "  --room-view on|off|probe  live tests of the Device page's passthrough: send the command the\n"
        "                       app sends for on or off, once (by default both are SteamVR's toggle, so either flips the\n"
        "                       view). FRAMEDOCK_ROOMVIEW=vrcmd (default: vrcmd --compositorcmd FRAMEDOCK_ROOMVIEW_CMD_ON\n"
        "                       / _OFF; probe only prints what would run) or mailbox (FRAMEDOCK_ROOMVIEW_MAILBOX; probe\n"
        "                       asks it which commands it takes)\n"
        "  --version            print the version and exit\n");
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasNext = i + 1 < argc;
        if (arg == "--print") {
            options.mode = Options::Mode::Print;
        } else if (arg == "--count" && hasNext) {
            options.printCount = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--dump-png" && hasNext) {
            options.mode = Options::Mode::DumpPng;
            options.pngPath = argv[++i];
        } else if (arg == "--dump-settings-png" && hasNext) {
            options.mode = Options::Mode::DumpSettingsPng;
            options.pngPath = argv[++i];
        } else if (arg == "--dump-frame-timings") {
            options.mode = Options::Mode::DumpFrameTimings;
            if (hasNext && argv[i + 1][0] != '-') {
                options.timingSeconds = std::fmax(0.5, std::atof(argv[++i]));
                options.timingSecondsGiven = true;
            }
        } else if (arg == "--debug-dump") {
            options.mode = Options::Mode::DebugDump;
        } else if (arg == "--room-view" && hasNext) {
            options.mode = Options::Mode::RoomView;
            options.roomView = argv[++i];
            if (options.roomView != "on" && options.roomView != "off" && options.roomView != "probe") return false;
        } else if (arg == "--dump-metrics") {
            options.mode = Options::Mode::DumpMetrics;
        } else if (arg == "--widgets" && hasNext) {
            if (!parseWidgetList(argv[++i], options.dumpMetrics.widgets)) {
                std::fprintf(stderr, "--widgets wants widget numbers, e.g. 1,4,9\n");
                return false;
            }
        } else if (arg == "--vr") {
            options.dumpMetrics.vr = true;
        } else if (arg == "--thumbnail-png" && hasNext) {
            options.thumbnailPngPath = argv[++i];
        } else if (arg == "--thumbnail-size" && hasNext) {
            options.thumbnailSize = std::max(16, std::min(1024, std::atoi(argv[++i])));
        } else if (arg == "--seconds" && hasNext) {
            options.dumpSeconds = std::fmax(0.0, std::atof(argv[++i]));
            options.secondsGiven = true;
        } else if (arg == "--fake-frames") {
            options.fakeFrames = true;
        } else if (arg == "--fake-fps" && hasNext) {
            options.fakeFps = std::fmax(1.0, std::atof(argv[++i]));
        } else if (arg == "--preview-autostart" && hasNext) {
            options.previewAutostart = argv[++i];
        } else if (arg == "--preview-page" && hasNext) {
            options.previewPage = argv[++i];
        } else if (arg == "--preview-select" && hasNext) {
            if (std::sscanf(argv[++i], "%d,%d", &options.previewSelectColumn, &options.previewSelectIndex) != 2) {
                std::fprintf(stderr, "--preview-select wants COLUMN,INDEX (from 0)\n");
                return false;
            }
        } else if (arg == "--language" && hasNext) {
            options.language = argv[++i];
            Language check = Language::Ko;
            if (!parseLanguage(options.language, check)) {
                std::fprintf(stderr, "--language must be ja, en or ko: %s\n", options.language.c_str());
                return false;
            }
        } else if (arg == "--contrast-report") {
            options.mode = Options::Mode::ContrastReport;
        } else if (arg == "--force-settings-visible") {
            options.forceSettingsVisible = true;
        } else if (arg == "--preview-color" && hasNext) {
            options.previewColor = argv[++i];
        } else if (arg == "--preview-device" && hasNext) {
            options.previewDevice = argv[++i];
        } else if (arg == "--preview-record" && hasNext) {
            options.previewRecord = argv[++i];
            if (options.previewRecord != "idle" && options.previewRecord != "recording" &&
                options.previewRecord != "saving" && options.previewRecord != "failed" &&
                options.previewRecord != "busy") {
                std::fprintf(stderr, "--preview-record must be idle, recording, saving, failed or busy\n");
                return false;
            }
        } else if (arg == "--preview-quit") {
            options.previewQuit = true;
        } else if (arg == "--fake-throttle") {
            options.fakeThrottle = true;
        } else if (arg == "--fake-wifi" && hasNext) {
            options.fakeWifi = argv[++i];
        } else if (arg == "--layout" && hasNext) {
            options.layoutPath = argv[++i];
        } else if (arg == "--bench" && hasNext) {
            options.benchCount = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--fake-warnings") {
            options.fakeWarnings = true;
        } else if (arg == "--config" && hasNext) {
            options.configPath = argv[++i];
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--version") {
            options.mode = Options::Mode::Version;
        } else if (arg == "--help" || arg == "-h") {
            options.mode = Options::Mode::Help;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    if (options.configPath.empty()) options.configPath = defaultConfigPath();
    // Also accept --dump-frame-timings --seconds S
    if (options.mode == Options::Mode::DumpFrameTimings && options.secondsGiven && !options.timingSecondsGiven) {
        options.timingSeconds = std::fmax(0.5, options.dumpSeconds);
    }
    return true;
}

/** Loads the fonts named in the config (FontSet falls back to fontconfig's Noto Sans CJK JP). */
void loadFonts(FontSet& fonts, const Config& config) {
    fonts.load(config.fontPath, config.boldFontPath);
}

/** Prints one sample for --print. */
void printSample(const SensorSample& s) {
    std::printf("  CPU      usage %s%% (max core %s%%)  fastest cluster %s MHz\n", fmt(s.cpuUsagePct, 1).c_str(),
                fmt(s.cpuMaxCorePct, 1).c_str(), fmt(s.cpuFreqMHz, 0).c_str());
    // No GPU load here: it comes from SteamVR's frame timing, which --print doesn't read
    std::printf("  GPU      %s MHz (max %s MHz)\n", fmt(s.gpuFreqMHz, 0).c_str(), fmt(s.gpuMaxFreqMHz, 0).c_str());
    std::printf("  Temp     CPU %s °C  GPU %s °C  DDR %s °C  battery %s °C\n", fmt(s.cpuTempC, 1).c_str(),
                fmt(s.gpuTempC, 1).c_str(), fmt(s.ddrTempC, 1).c_str(), fmt(s.batteryTempC, 1).c_str());
    std::printf("           display %s °C  exhaust %s °C  heatsink %s °C   thermal limit CPU %s / GPU %s\n",
                fmt(s.displayTempC, 1).c_str(), fmt(s.exhaustTempC, 1).c_str(), fmt(s.heatsinkTempC, 1).c_str(),
                s.throttleCpu ? "yes" : "no", s.throttleGpu ? "yes" : "no");
    if (!s.wifi.interfaceUp) {
        std::printf("  Wireless no interface (neither the dongle AP nor a home Wi-Fi station)\n");
    } else if (!s.wifi.connected) {
        std::printf("  Wireless not connected (no peer on the dongle, not on home Wi-Fi)\n");
    } else {
        // The dongle's peer is the PC; home Wi-Fi's is the access point
        std::printf("  %s rx %s Mbps  tx %s Mbps  link tx %s / rx %s Mbps  signal %s dBm\n",
                    s.wifi.homeWifi ? "Wi-Fi   " : "Dongle  ", fmt(s.wifi.rxMbps, 2).c_str(),
                    fmt(s.wifi.txMbps, 2).c_str(), fmt(s.wifi.txLinkMbps, 1).c_str(),
                    fmt(s.wifi.rxLinkMbps, 1).c_str(), fmt(s.wifi.signalDbm, 0).c_str());
    }
    std::printf("  Power    vph %s W  rails %s W\n", fmt(s.powerMainW, 3).c_str(), fmt(s.powerRailsW, 3).c_str());
    std::printf("  Fan      %s rpm\n", fmt(s.fanRpm, 0).c_str());
    std::printf("  Battery  %s%%  %s V  %s mA  %s (charger %s)\n", fmt(s.batteryPct, 0).c_str(),
                fmt(s.batteryVoltageV, 3).c_str(), fmt(s.batteryCurrentMa, 0).c_str(), s.batteryStatus.c_str(),
                s.chargerOnline ? "online" : "offline");
    std::printf("  Memory   %s / %s GiB\n", fmt(s.memUsedGiB, 2).c_str(), fmt(s.memTotalGiB, 2).c_str());
}

/** Demand for the offline modes (--print, --dump-png): someone is "looking" at the terminal / PNG. */
frame_perf_overlay::DemandInputs offlineDemand() {
    frame_perf_overlay::DemandInputs demand;
    demand.hudShown = true;
    demand.hmdInUse = true;
    return demand;
}

/** --print: print the readings a few times without OpenVR. */
int runPrint(const Options& options) {
    Sensors sensors;
    sensors.discover();
    std::printf("== Sensors found ==\n%s\n", sensors.describe().c_str());
    frame_perf_overlay::MetricStore store;
    frame_perf_overlay::Scheduler scheduler;
    SensorBridge bridge(sensors, nullptr, store);
    bridge.registerSources(scheduler);
    scheduler.setEnabledWidgets(SensorBridge::allWidgets());
    scheduler.setDemand(offlineDemand());
    scheduler.runDue(nowSeconds());  // CPU usage baseline
    // Reads every source at least once per print (some run every 2 or 5 s, like on the panel)
    for (int i = 1; i <= options.printCount && !gStopRequested; ++i) {
        sleepInterruptible(1.0);
        scheduler.runDue(nowSeconds());
        std::printf("== [%d/%d] %s ==\n", i, options.printCount, localTimeText().c_str());
        printSample(bridge.sample());
        std::fflush(stdout);
    }
    return 0;
}

/** Fake frame timings to check the look. fps > 0 holds the app's fps around that value. */
FrameStats fakeFrameStats(double t, double fps) {
    FrameStats f;
    f.valid = true;
    f.haveTimes = true;
    f.displayHz = 90.0;
    f.targetMs = 1000.0 / 90.0;
    // Around 90 unless given, with an occasional small dip
    f.appFps = fps > 0 ? fps + 1.5 * std::sin(t * 2.1) : 90.0 - (std::fmod(t, 7.0) < 0.6 ? 6.0 : 0.3 * std::fabs(std::sin(t)));
    f.gpuMs = 8.0 + 1.5 * std::sin(t * 1.3) + (std::fmod(t, 7.0) < 0.6 ? 4.0 : 0.0);
    f.gpuMaxMs = f.gpuMs + 1.0;
    f.cpuMs = 5.0 + 1.0 * std::sin(t * 0.7 + 1.0);
    f.cpuMaxMs = f.cpuMs + 0.8;
    f.frames = 45;
    f.reprojectedPct = f.gpuMs > f.targetMs ? 12.0 : 0.0;
    if (fps > 0) {
        // A dropping-fps picture: heavy GPU, lots of reprojection
        f.gpuMs = 14.5 + 1.0 * std::sin(t * 1.3);
        f.reprojectedPct = 100.0 * (1.0 - fps / 90.0);
    }
    f.dropped = 0;
    f.gpuBusyPct = estimateGpuBusyPct(f.gpuMs, f.appFps);
    return f;
}

/** Puts fake frame stats into the store, the way SensorBridge::readFrameTiming does. */
void storeFakeFrame(frame_perf_overlay::MetricStore& st, const FrameStats& f, double now) {
    using namespace frame_perf_overlay::metric;
    st.setF32(kFrameAppFps, f.appFps, now);
    st.setF32(kFrameDisplayHz, f.displayHz, now);
    st.setF32(kFrameGpuMs, f.gpuMs, now);
    st.setF32(kFrameCpuMs, f.cpuMs, now);
    st.setF32(kFrameGpuMaxMs, f.gpuMaxMs, now);
    st.setF32(kFrameCpuMaxMs, f.cpuMaxMs, now);
    st.setF32(kFrameReprojectedPct, f.reprojectedPct, now);
    st.setI32(kFrameDropped, static_cast<int32_t>(f.dropped), now);
    st.setEnum(kFrameThrottled, static_cast<uint32_t>(f.throttledFrames), now);
    st.setI32(kFrameCount, static_cast<int32_t>(f.frames), now);
    st.setF32(kGpuBusyPct, estimateGpuBusyPct(f.gpuMs, f.appFps), now);
}

/** Fake controller batteries (left charging). low = the low-battery picture (right hand not connected). */
void storeFakeControllers(frame_perf_overlay::MetricStore& st, bool low, double now) {
    using namespace frame_perf_overlay::metric;
    st.setF32(kCtrlLeftPct, low ? 8.0 : 80.0, now);
    st.setEnum(kCtrlLeftCharging, 1, now);
    if (low) {
        st.setNa(kCtrlRightPct, frame_perf_overlay::Reason::NotConnected, now);
        st.setNa(kCtrlRightCharging, frame_perf_overlay::Reason::NotConnected, now);
    } else {
        st.setF32(kCtrlRightPct, 75.0, now);
        st.setEnum(kCtrlRightCharging, 0, now);
    }
}

/** Replaces some readings with bad values to check the warning colours. */
void storeFakeWarnings(frame_perf_overlay::MetricStore& st, double now) {
    using namespace frame_perf_overlay::metric;
    st.setEnum(kThrottleCpu, 1, now);
    st.setEnum(kThrottleGpu, 1, now);
    st.setF32(kGpuBusyPct, 97.0, now);
    st.setF32(kTempCpuC, 86.0, now);
    st.setF32(kBatteryPct, 12.0, now);
    st.setEnum(kBatteryStatus, static_cast<uint32_t>(frame_perf_overlay::BatteryStatus::Discharging), now);
    st.setEnum(kBatteryChargerOnline, 0, now);
    st.setEnum(kNetInterfaceUp, 1, now);
    st.setEnum(kNetConnected, 1, now);
    st.setEnum(kNetHomeWifi, 0, now);
    st.setF32(kNetSignalDbm, -81.0, now);
    st.setF32(kNetRxMbps, 42.5, now);
    st.setF32(kNetTxMbps, 1.2, now);
    st.setF32(kNetTxLinkMbps, 288.2, now);
    st.setF32(kNetRxLinkMbps, 216.1, now);
}

/** Fakes SteamVR throttling the app to half rate (values seen in Half-Life: Alyx). */
void applyFakeThrottle(FrameStats& frame, double t) {
    frame.displayHz = 72.0;
    frame.targetMs = 1000.0 / 72.0;
    frame.appFps = 36.0 - (std::fmod(t, 6.0) < 0.5 ? 2.0 : 0.0);
    frame.gpuMs = 22.0 + 1.0 * std::sin(t * 1.3);
    frame.cpuMs = 4.4 + 0.5 * std::sin(t * 0.7);
    frame.reprojectedPct = 50.0;
    frame.dropped = 0;
    frame.throttledFrames = 1;
}

/** Fakes the link: "-65" (dongle), "home:-47" (home Wi-Fi) or "none" / "home:none" (not connected). */
void storeFakeWifi(frame_perf_overlay::MetricStore& st, const std::string& value, double now) {
    using namespace frame_perf_overlay::metric;
    const bool home = value.rfind("home:", 0) == 0;
    const std::string rest = value.substr(home ? 5 : 0);
    st.setEnum(kNetInterfaceUp, 1, now);
    st.setEnum(kNetConnected, rest == "none" ? 0 : 1, now);
    st.setEnum(kNetHomeWifi, home ? 1 : 0, now);
    if (rest == "none") return;
    st.setF32(kNetSignalDbm, std::atof(rest.c_str()), now);
    st.setF32(kNetRxMbps, 187.4, now);
    st.setF32(kNetTxMbps, 2.1, now);
    st.setF32(kNetTxLinkMbps, 1152.8, now);
    st.setF32(kNetRxLinkMbps, 960.7, now);
}

/** What the widgets draw with. */
frame_perf_overlay::WidgetContext widgetContext(const Config& config, const frame_perf_overlay::MetricStore& store,
                                                double now, const SensorBridge& bridge,
                                                const frame_perf_overlay::ControlsView* controls = nullptr) {
    frame_perf_overlay::WidgetContext ctx;
    ctx.store = &store;
    ctx.processes = &bridge.processLists();
    ctx.controls = controls;
    ctx.thresholds = config.thresholds;
    ctx.now = now;
    ctx.graphSeconds = config.graphSeconds;
    ctx.clockFormat = config.clockFormat;
    ctx.language = widgetLanguage(config.language);
    return ctx;
}

/** What the controls show in --dump-png: a made-up state (nothing is read), as on a Frame in use */
frame_perf_overlay::ControlsView previewControls(const Config& config, const std::string& record) {
    frame_perf_overlay::ControlsView v;
    v.interactive = true;
    v.colorAvailable = v.compareUsable = true;
    const framecolor::Translations& colors = framecolor::getTranslations(colorLanguage(config.language));
    for (int i = 0; i < 3; ++i) v.presetNames[i] = colors.builtinPresets[i];
    v.presetInUse = 0;
    v.soundKnown = true;
    v.outputName = "Wireless Headset";
    v.volume = 45;
    v.passthrough = 2;
    v.monoColorUsable = true;
    const frame_perf_overlay::WidgetText& w = frame_perf_overlay::widgetText(widgetLanguage(config.language));
    v.profileNames = {w.simpleA, w.simpleB, tr(Tx::ProfileBasic, config.language),
                      tr(Tx::ProfileDetailed, config.language)};
    v.activeProfile = 0;
    v.recordState = record == "recording" ? 1
                    : record == "saving"  ? 2
                    : record == "failed"  ? 3
                    : record == "busy"    ? 4
                                          : 0;
    v.recordSeconds = v.recordState == 1 ? 42 : 0;
    return v;
}

/** @return the panel's update interval (seconds): update_interval_ms, which the config clamps to 100-5000 ms */
double updateIntervalSec(const Config& config) {
    return std::max(100, config.updateIntervalMs) / 1000.0;
}

/** Applies a layout to the scheduler (a widget that isn't in it reads nothing, a compact General no full-card rows)
 *  and the process lists' settings. */
void applyLayout(const frame_perf_overlay::Layout& layout, frame_perf_overlay::Scheduler& scheduler,
                 SensorBridge& bridge) {
    scheduler.setEnabledWidgets(frame_perf_overlay::enabledWidgets(layout),
                                frame_perf_overlay::enabledCompact(layout));
    bridge.setProcessOptions(layout);
}

/** hide_rules.json, next to config.json ("--config config.json" has no folder: "."). */
std::string hideRulesPath(const std::string& configPath) {
    const size_t slash = configPath.find_last_of('/');
    return (slash == std::string::npos ? std::string(".") : configPath.substr(0, slash)) + "/hide_rules.json";
}

/** --layout: reads a layout file (a layout object, or a whole config with a "layout" key). */
bool loadLayoutFile(const std::string& path, frame_perf_overlay::Layout& layout) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "Can't read %s\n", path.c_str());
        return false;
    }
    const std::string textIn((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    JsonValue root;
    std::string error;
    std::vector<std::string> warnings;
    if (!parseJson(textIn, root, error)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    const JsonValue* object = root.get("layout") != nullptr ? root.get("layout") : &root;
    const bool ok = frame_perf_overlay::layoutFromJson(*object, layout, warnings, error);
    for (const std::string& w : warnings) std::fprintf(stderr, "[layout] %s\n", w.c_str());
    if (!ok) std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
    return ok;
}

/** --dump-png: collect readings without OpenVR, render the widget grid and write a PNG. */
int runDumpPng(const Options& options) {
    ConfigWatcher watcher(options.configPath);
    Config config = watcher.loadInitial();
    if (!options.language.empty()) parseLanguage(options.language, config.language);
    setUiLanguage(config.language);
    frame_perf_overlay::Layout layout = config.layout;
    if (!options.layoutPath.empty() && !loadLayoutFile(options.layoutPath, layout)) return 1;
    Sensors sensors;
    sensors.discover();
    FontSet fonts;
    loadFonts(fonts, config);
    frame_perf_overlay::GridRenderer renderer(fonts);
    renderer.setLayout(layout);
    frame_perf_overlay::MetricStore store;
    frame_perf_overlay::Scheduler scheduler;
    HideRules hideRules(hideRulesPath(options.configPath));  // read only here (no first-run write); outlives the bridge
    SensorBridge bridge(sensors, nullptr, store);
    bridge.setHideRules(&hideRules);
    bridge.registerSources(scheduler);
    applyLayout(layout, scheduler, bridge);
    scheduler.setUpdateInterval(updateIntervalSec(config));
    scheduler.setDemand(offlineDemand());

    scheduler.runDue(nowSeconds());  // CPU usage baseline
    const double interval = updateIntervalSec(config);
    const double start = nowSeconds();
    const bool fake = options.fakeFrames || options.fakeWarnings || options.fakeFps > 0 || options.fakeThrottle;
    double renderMs = 0.0;
    const frame_perf_overlay::ControlsView controls = previewControls(config, options.previewRecord);
    do {
        sleepInterruptible(interval);
        const double t = nowSeconds();
        scheduler.runDue(t);
        if (fake) {
            FrameStats frame = fakeFrameStats(t - start, options.fakeFps);
            if (options.fakeThrottle) applyFakeThrottle(frame, t - start);
            storeFakeFrame(store, frame, t);
            storeFakeControllers(store, options.fakeWarnings, t);
        }
        if (options.fakeWarnings) storeFakeWarnings(store, t);
        if (!options.fakeWifi.empty()) storeFakeWifi(store, options.fakeWifi, t);
        const double r0 = nowSeconds();
        renderer.render(widgetContext(config, store, t, bridge, &controls));
        renderMs = (nowSeconds() - r0) * 1e3;
    } while (!gStopRequested && nowSeconds() - start < options.dumpSeconds);

    if (!renderer.writePng(options.pngPath)) {
        std::fprintf(stderr, "Couldn't write the PNG: %s\n", options.pngPath.c_str());
        return 1;
    }
    const frame_perf_overlay::GridGeometry& g = renderer.geometry();
    std::printf("Wrote %s (%dx%d texture, layout %dx%d px, %d column%s, scale %.3f, %zu widgets)\n",
                options.pngPath.c_str(), g.textureWidth, g.textureHeight, g.width, g.height, g.columns,
                g.columns == 1 ? "" : "s", g.scale, g.slots.size());
    std::printf("Physical size at width_m %.2f: %.3f x %.3f m (head and wrist alike; width_m is one full column)\n",
                config.widthM, config.widthM * g.widthInColumns(),
                config.widthM * g.widthInColumns() * g.textureHeight / std::max(1, g.textureWidth));
    // Redraw cost with live data: render (cairo) and convert (RGBA for the upload), per redraw
    double sumRender = 0.0, maxRender = 0.0, sumConvert = 0.0, maxConvert = 0.0;
    int changed = 0;
    for (int k = 0; k < options.benchCount; ++k) {
        renderer.markAllDirty();  // worst case: every widget redrawn
        const double t0 = nowSeconds();
        changed += renderer.render(widgetContext(config, store, t0 + 0.5 * k, bridge)) ? 1 : 0;
        const double t1 = nowSeconds();
        renderer.toRgba();
        const double t2 = nowSeconds();
        sumRender += t1 - t0;
        maxRender = std::max(maxRender, t1 - t0);
        sumConvert += t2 - t1;
        maxConvert = std::max(maxConvert, t2 - t1);
    }
    std::printf("Full redraw %.2f ms avg / %.2f max, RGBA convert (FRAME_PERF_RGBA_UPLOAD only) %.2f ms avg / %.2f max "
                "over %d redraws (%d changed); last live render %.2f ms; static layer built %llu time(s)\n",
                sumRender * 1e3 / options.benchCount, maxRender * 1e3, sumConvert * 1e3 / options.benchCount,
                maxConvert * 1e3, options.benchCount, changed, renderMs,
                static_cast<unsigned long long>(renderer.staticBuilds()));
    return 0;
}

/** --dump-settings-png: render the dashboard tab (and thumbnail) without OpenVR. */
int runDumpSettingsPng(const Options& options) {
    ConfigWatcher watcher(options.configPath);
    Config config = watcher.loadInitial();
    if (!options.language.empty()) {
        parseLanguage(options.language, config.language);
        config.languageAuto = false;
    }
    setUiLanguage(config.language);
    FontSet fonts;
    loadFonts(fonts, config);
    SettingsPanel panel(fonts);
    if (options.previewQuit) panel.armQuitForPreview();
    panel.setRecordOffered(!options.previewRecord.empty());  // the Layout page's palette as with framecorder
    // The Color page with made-up state: no host (nothing reaches SteamVR) and start() isn't called, so
    // FrameColor's files are neither read nor written
    framecolor::ColorModel colorModel(nullptr, framecolor::ColorPaths::fromEnvironment());
    previewColorState(colorModel, colorLanguage(config.language), options.previewColor == "busy");
    // Follow gaze, a fit running, a fit just done
    if (options.previewColor == "gaze" || options.previewColor == "gaze-fit" || options.previewColor == "gaze-review" ||
        options.previewColor == "gaze-failed") {
        colorModel.state.sharpenGaze = true;
        if (options.previewColor == "gaze-fit") {
            colorModel.state.gazeFitStep = 1;
            colorModel.state.gazeFitSteps = 9;
        }
        if (options.previewColor == "gaze-review") {
            colorModel.state.gazeFitOutcome = 1;
            colorModel.state.gazeFitReviewing = true;
            colorModel.state.gazeFitReviewLeft = 7;
        }
        if (options.previewColor == "gaze-failed") {
            colorModel.state.gazeFitOutcome = 2;
            colorModel.state.gazeFitFailure = 3;
        }
    }
    framecolor::ColorPage colorPage(colorModel);
    panel.setColorPage(&colorPage);
    // The 기기 page with made-up state (no backend: nothing is read or sent)
    DeviceModel deviceModel;
    if (!previewDeviceState(deviceModel, options.previewDevice)) {
        std::fprintf(stderr, "Unknown --preview-device %s\n", options.previewDevice.c_str());
        return 2;
    }
    DevicePage devicePage(deviceModel);
    panel.setDevicePage(&devicePage);
    // Autostart: the real state (read-only), unless a preview state is given
    Autostart autostart;
    AutostartStatus autostartView = autostart.status();
    const std::string& preview = options.previewAutostart;
    if (preview == "enabled") autostartView = {AutostartStatus::State::Enabled, false, false};
    if (preview == "disabled") autostartView = {AutostartStatus::State::Disabled, false, false};
    if (preview == "notinstalled") autostartView = {AutostartStatus::State::NotInstalled, false, false};
    if (preview == "busy") autostartView = {AutostartStatus::State::Enabled, true, false};
    if (preview == "failed") autostartView = {AutostartStatus::State::Enabled, false, true};
    if (options.previewPage == "placement") panel.showPage(SettingsPage::Placement);
    if (options.previewPage == "layout") panel.showPage(SettingsPage::Layout);
    if (options.previewPage == "color") panel.showPage(SettingsPage::Color);
    if (options.previewPage == "dock") panel.showPage(SettingsPage::Dock);
    if (options.previewPage == "browser") panel.showPage(SettingsPage::Browser);
    if (options.previewPage == "device") panel.showPage(SettingsPage::Device);
    // Made-up profiles (the Profiles page and the footer's quick switch on every page)
    const ProfilesUiState profilesView = previewProfiles(config.language, options.previewPage == "profiles");
    if (options.previewPage == "profiles") {
        panel.showPage(SettingsPage::Profiles);
        panel.selectProfile(profilesView.activeId);
    }
    panel.render(config, autostartView, profilesView);
    if (options.previewPage == "layout" && options.previewSelectColumn >= 0) {
        // Tap a card so the palette shows its settings (the first render synced the editor's cards)
        for (const LayoutEditor::CardBox& card : panel.editor().cards()) {
            if (card.ref.column != options.previewSelectColumn || card.ref.index != options.previewSelectIndex) continue;
            const double x = card.rect.x + card.rect.w / 2, y = card.rect.y + card.rect.h / 2;
            panel.pointerDown(x, y, 0.0);
            panel.pointerUp(x, y);
            panel.render(config, autostartView, profilesView);
        }
    }
    if (!panel.writePng(options.pngPath)) {
        std::fprintf(stderr, "Couldn't write the PNG: %s\n", options.pngPath.c_str());
        return 1;
    }
    std::printf("Wrote %s (%dx%d)\n", options.pngPath.c_str(), panel.width(), panel.height());
    if (!options.thumbnailPngPath.empty()) {
        std::vector<uint8_t> rgba;
        renderThumbnail(fonts, options.thumbnailSize, rgba, options.thumbnailPngPath);
        std::printf("Wrote the thumbnail %s (%dx%d)\n", options.thumbnailPngPath.c_str(), options.thumbnailSize,
                    options.thumbnailSize);
    }
    return 0;
}

/**
 * --dump-frame-timings: connect as a Background app (fails instead of starting SteamVR) and print raw
 * frame timings. Creates no overlay and no Vulkan device, so it can run next to the resident instance.
 */
int runDumpFrameTimings(const Options& options) {
    VrOverlay vr;
    std::string message;
    if (!vr.connectReadOnly(message)) {
        std::fprintf(stderr, "[VR] Can't connect: %s\n", message.c_str());
        return 1;
    }
    vr.dumpFrameTimings(options.timingSeconds, gStopRequested);
    vr.shutdown();
    return 0;
}

/**
 * A config save held back while a dashboard button repeats (hold to repeat): the change shows on the panel at once,
 * the file is written at most every kHeldSaveSec and once more on release.
 */
struct HeldSave {
    bool pending = false;
    double dueAt = std::numeric_limits<double>::infinity();

    void request(double now) {
        if (pending) return;  // the first change of a batch sets the time; later ones ride along
        pending = true;
        dueAt = now + kHeldSaveSec;
    }
    void clear() { *this = HeldSave(); }
};

/** Saves the config file through the watcher (an unreadable file is kept as config.json.bad, not overwritten). */
void saveConfigNow(const Config& config, ConfigWatcher& watcher) {
    std::string error;
    if (!watcher.save(config, error)) std::fprintf(stderr, "[config] Saving failed: %s\n", error.c_str());
}

/**
 * Applies a dashboard action to the config and to the panel, and saves the config file: at once, or (held, a
 * repeating button) through held, which the main loop writes when due or released.
 */
void handleSettingsAction(SettingsAction action, Config& config, ConfigWatcher& watcher, VrOverlay& vr,
                          HeldSave& held, bool hold = false, double now = 0.0) {
    if (!applySettingsAction(action, config)) return;
    vr.applyConfig(config);
    if (hold) {
        held.request(now);
        return;
    }
    saveConfigNow(config, watcher);
    held.clear();  // the whole config was just written
}

/** @return the single-instance lock file path */
std::string lockFilePath() {
    const std::string dir = paths::runtimeDir();
    if (dir == "/tmp") return std::string("/tmp/") + paths::kAppName + "-" + std::to_string(::getuid()) + ".lock";
    return dir + "/" + paths::kAppName + ".lock";
}

/**
 * Takes the single-instance lock. On success writes our PID and keeps the file open (the OS releases the
 * lock when we exit). Otherwise returns the PID of the instance holding it.
 * @param lockFd the open lock file on success
 * @param holderPid the holder's PID on failure (0 if unreadable)
 * @return true if we hold the lock (or locking isn't possible, so just start)
 */
bool acquireInstanceLock(int& lockFd, pid_t& holderPid) {
    lockFd = -1;
    holderPid = 0;
    const std::string path = lockFilePath();
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        std::fprintf(stderr, "[startup] Can't open the lock file %s; starting without the single-instance check\n",
                     path.c_str());
        return true;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        const std::string pid = std::to_string(::getpid()) + "\n";
        if (::ftruncate(fd, 0) != 0 || ::pwrite(fd, pid.data(), pid.size(), 0) < 0) {
            std::fprintf(stderr, "[startup] Couldn't write our PID to the lock file\n");
        }
        lockFd = fd;
        return true;
    }
    // The holder may have just taken the lock and not written its PID yet: retry the read briefly
    for (int attempt = 0; attempt < 10 && holderPid <= 0; ++attempt) {
        char buffer[32] = {};
        const ssize_t n = ::pread(fd, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) holderPid = static_cast<pid_t>(std::atol(buffer));
        if (holderPid <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::close(fd);
    return false;
}

/**
 * Whether we were started as our own systemd unit. INVOCATION_ID is inherited by children of Steam
 * (also a unit) started with +, so check whether our cgroup is this app's unit instead.
 */
bool runningAsService() {
    FILE* file = std::fopen("/proc/self/cgroup", "r");
    if (!file) return false;
    char* line = nullptr;
    size_t capacity = 0;
    bool found = false;
    while (::getline(&line, &capacity, file) >= 0) {
        if (std::strstr(line, (std::string("/") + paths::kAppName + ".service").c_str())) {
            found = true;
            break;
        }
    }
    std::free(line);
    std::fclose(file);
    return found;
}

/** Handles a pending SIGUSR1 like the dashboard's On / Off. @return true if it toggled */
bool handleToggleRequest(Config& config, ConfigWatcher& watcher, VrOverlay& vr, HeldSave& held) {
    if (!gToggleRequested) return false;
    gToggleRequested = 0;
    handleSettingsAction(config.visible ? SettingsAction::ShowOff : SettingsAction::ShowOn, config, watcher, vr, held);
    std::fprintf(stderr, "[startup] A second launch asked to toggle: the panel is now %s\n",
                 config.visible ? "shown" : "hidden");
    return true;
}

/** @return a readable name for an EDeviceActivityLevel value */
const char* activityName(int level) {
    switch (level) {
        case -1: return "unknown";
        case 0: return "idle";
        case 1: return "user interaction";
        case 2: return "user interaction timeout";
        case 3: return "standby";
        case 4: return "idle timeout";
        default: return "?";
    }
}

/** @return true unless the HMD reports standby (off-head reads standby; unknown counts as in use) */
bool hmdInUse(int activityLevel) {
    return activityLevel != 3;  // vr::k_EDeviceActivityLevel_Standby
}

/**
 * Writes the debug dump (signals, sources with reference and read counters, every metric) to
 * paths::debugDumpPath(), via a temp file + rename so a reader never sees half of it.
 */
void writeDebugDump(const frame_perf_overlay::Scheduler& scheduler, const frame_perf_overlay::MetricStore& store,
                    int activityLevel, bool settingsVisible, double now, const std::string& extra = "") {
    std::string text = "framedock " FRAMEDOCK_VERSION " debug dump, pid " +
                       std::to_string(::getpid()) + ", " + localTimeText() + "\n";
    text += "hmd activity level: " + std::to_string(activityLevel) + " (" + activityName(activityLevel) +
            "), dashboard tab visible: " + (settingsVisible ? "yes" : "no") + "\n\n";
    text += scheduler.dump(now);
    text += "\n";
    text += store.dump(now);
    if (!extra.empty()) text += "\n" + extra;
    const std::string path = paths::debugDumpPath();
    const std::string tmp = path + ".tmp";
    FILE* file = std::fopen(tmp.c_str(), "w");
    if (file == nullptr) {
        std::fprintf(stderr, "[debug] Can't write %s\n", tmp.c_str());
        return;
    }
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
    if (std::rename(tmp.c_str(), path.c_str()) != 0) std::fprintf(stderr, "[debug] Can't rename %s\n", tmp.c_str());
}

/** --debug-dump: ask the running instance for its dump (SIGUSR2) and print it. */
/**
 * --room-view on|off|probe over the route the app uses (roomview::routeFromEnv), what came back printed. For the live
 * tests of the passthrough's on / off. vrcmd's probe runs nothing: it shows the commands and whether vrcmd is there.
 */
int runRoomView(const Options& options) {
    std::string warning;
    const roomview::Route route = roomview::routeFromEnv(warning);
    if (!warning.empty()) std::fprintf(stderr, "%s\n", warning.c_str());
    const bool probe = options.roomView == "probe";
    const bool on = options.roomView == "on";
    if (route.kind == roomview::Route::Kind::Openvr) {
        if (probe) {
            std::printf("route: OpenVR's %s\non:  %s\noff: %s, then %s\n", route.mailbox.c_str(),
                        roomview::enableMessage(true).c_str(), roomview::enableMessage(false).c_str(),
                        roomview::sceneAppsMessage(false).c_str());
            return 0;
        }
        VrOverlay vr;
        std::string error;
        if (!vr.connectReadOnly(error)) {
            std::fprintf(stderr, "failed: %s\n", error.c_str());
            return 1;
        }
        // The app's own control (its lines go to stderr)
        RoomViewDirect direct(
            [&vr](const std::string& mailbox, const std::string& message, std::string& sendError) {
                return vr.sendMailbox(mailbox, message, sendError);
            },
            route.mailbox);
        direct.setEnabled(on);
        bool ok = false;
        direct.takeResult(ok);
        vr.shutdown();
        return ok ? 0 : 1;
    }
    if (route.kind == roomview::Route::Kind::Vrcmd) {
        if (probe) {
            std::printf("route: vrcmd (%s)\n", ::access(route.vrcmd.c_str(), X_OK) == 0 ? "found" : "NOT FOUND");
            std::printf("on:  %s --compositorcmd '%s'\n", route.vrcmd.c_str(), route.cmdOn.c_str());
            std::printf("off: %s --compositorcmd '%s'\n", route.vrcmd.c_str(), route.cmdOff.c_str());
            return 0;
        }
        const std::string& command = on ? route.cmdOn : route.cmdOff;
        std::printf("run: %s --compositorcmd '%s'\n", route.vrcmd.c_str(), command.c_str());
        std::fflush(stdout);
        std::string output;
        std::string error;
        const bool ok = roomview::runVrcmd(route.vrcmd, command, roomview::kVrcmdSec, -1, output, error);
        if (!output.empty()) std::printf("output:\n%s%s", output.c_str(), output.back() == '\n' ? "" : "\n");
        if (!ok) {
            std::fprintf(stderr, "failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("exited with 0\n");
        return 0;
    }
    const std::string command =
        roomview::sendCommand(route.mailbox, probe ? roomview::probeMessage() : roomview::enableMessage(on));
    std::vector<std::string> replies;
    std::string error;
    const bool ok = roomview::exchange(roomview::kWebPort, command, probe ? 1.5 : 0.5, -1, &replies, error);
    std::printf("sent: %s\n", ok ? command.c_str() : "(nothing)");
    for (const std::string& reply : replies) std::printf("reply: %s\n", reply.c_str());
    if (!ok) {
        std::fprintf(stderr, "failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}

int runDebugDump() {
    int lockFd = -1;
    pid_t holderPid = 0;
    if (acquireInstanceLock(lockFd, holderPid)) {
        if (lockFd >= 0) ::close(lockFd);
        std::fprintf(stderr, "framedock isn't running\n");
        return 1;
    }
    if (holderPid <= 0) {
        std::fprintf(stderr, "framedock is running, but its PID couldn't be read from %s\n",
                     lockFilePath().c_str());
        return 1;
    }
    const std::string path = paths::debugDumpPath();
    std::remove(path.c_str());
    if (::kill(holderPid, SIGUSR2) != 0) {
        std::fprintf(stderr, "Couldn't signal PID %d: %s\n", static_cast<int>(holderPid), std::strerror(errno));
        return 1;
    }
    for (int i = 0; i < 30; ++i) {
        std::ifstream file(path);
        if (file) {
            std::printf("%s", std::string(std::istreambuf_iterator<char>(file), {}).c_str());
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::fprintf(stderr, "No dump from PID %d after 3 s (%s)\n", static_cast<int>(holderPid), path.c_str());
    return 1;
}

/** What the panel's redraws cost, for the debug dump. */
struct RenderStats {
    uint64_t redraws = 0;
    uint64_t uploads = 0;  ///< redraws whose pixels changed (submitted)
    uint64_t widgets = 0;  ///< widgets redrawn, summed (the rest kept their pixels)
    double renderWallMs = 0.0, renderCpuMs = 0.0, uploadCpuMs = 0.0;
    double maxRenderWallMs = 0.0;

    void add(double wallMs, double cpuMs, double uploadMs, bool changed, size_t widgetsRedrawn) {
        ++redraws;
        widgets += widgetsRedrawn;
        uploads += changed ? 1 : 0;
        renderWallMs += wallMs;
        renderCpuMs += cpuMs;
        uploadCpuMs += uploadMs;
        maxRenderWallMs = std::max(maxRenderWallMs, wallMs);
    }

    std::string text(const frame_perf_overlay::GridGeometry& g) const {
        char buffer[512];
        const double n = redraws > 0 ? static_cast<double>(redraws) : 1.0;
        const double u = uploads > 0 ? static_cast<double>(uploads) : 1.0;
        std::snprintf(buffer, sizeof(buffer),
                      "== Panel redraws ==\npanel %dx%d texture (layout %dx%d, scale %.3f, %d columns, %zu widgets)\n"
                      "redraws %llu, uploads %llu, widgets redrawn %.1f per redraw | render wall %.2f ms avg / %.2f max, render CPU %.2f ms avg | "
                      "submit CPU %.2f ms per upload\n",
                      g.textureWidth, g.textureHeight, g.width, g.height, g.scale, g.columns, g.slots.size(),
                      static_cast<unsigned long long>(redraws), static_cast<unsigned long long>(uploads),
                      static_cast<double>(widgets) / n,
                      renderWallMs / n, maxRenderWallMs, renderCpuMs / n, uploadCpuMs / u);
        return buffer;
    }
};

/**
 * Runs as the overlay. Waits for SteamVR, and exits quietly on SIGTERM.
 * If another instance runs, tells it to toggle the panel and exits at once (no VR_Init).
 *
 * The loop sleeps in one poll() with the timeout at the earliest of these deadlines:
 * - metrics (2 Hz): SteamVR events, config reload, "someone looking" signals,
 * - sources and the panel redraw: at update_interval_ms (the slower sources at their own intervals),
 * - input (50 / 100 ms): dashboard pointer events, only while the dashboard tab is visible,
 * - placement: the wrist fade (1/30 s while fading), only when attached to a hand and the headset is worn.
 * Signals wake it through the same poll().
 */
int runOverlay(const Options& options) {
    int lockFd = -1;
    pid_t holderPid = 0;
    bool locked = acquireInstanceLock(lockFd, holderPid);
    if (!locked && runningAsService()) {
        // The service never toggles (Restart=always would toggle over and over). Wait for the manual one to exit
        std::fprintf(stderr, "[startup] Already running outside the service (PID %d). Waiting for it to exit\n",
                     static_cast<int>(holderPid));
        while (!locked && !gStopRequested) {
            waitForWake(2.0);
            gToggleRequested = 0;
            gDumpRequested = 0;
            locked = acquireInstanceLock(lockFd, holderPid);
        }
        if (!locked) return 0;
        std::fprintf(stderr, "[startup] The other instance exited; starting\n");
    }
    if (!locked) {
        if (holderPid > 0 && ::kill(holderPid, SIGUSR1) == 0) {
            std::fprintf(stderr, "[startup] Already running (PID %d). Toggled its panel; exiting\n",
                         static_cast<int>(holderPid));
            return 0;
        }
        std::fprintf(stderr, "[startup] Seems to be running already, but couldn't signal it (PID %d)\n",
                     static_cast<int>(holderPid));
        return 1;
    }

    ConfigWatcher watcher(options.configPath);
    Config config = watcher.loadInitial();
    // The language everything shows ("auto": Steam's, looked up once here): changes later are picked up in the loop
    setUiLanguage(config.language);
    std::fprintf(stderr, "[language] %s%s\n", languageCode(config.language), config.languageAuto ? " (auto)" : "");
    HeldSave heldSave;  // a held dashboard button's config save, written when due or on release
    if (!config.layoutInFile && !watcher.lastLoadFailed()) {
        // First run, or a config.json from before the widget grid: write the default layout (two columns, the fixed
        // panel's readings) so it can be edited. An unparsable file is left alone: the defaults run in memory and
        // the log says why
        std::string error;
        if (watcher.save(config, error)) {
            std::fprintf(stderr, "[config] Wrote %s\n", watcher.path().c_str());
        } else {
            std::fprintf(stderr, "[config] Couldn't write %s: %s\n", watcher.path().c_str(), error.c_str());
        }
    }
    Sensors sensors;
    sensors.discover();
    FontSet fonts;
    loadFonts(fonts, config);
    // 녹화: framecorder (a separate recorder for the Steam Frame) when it is installed, checked once here. Without it
    // the HUD leaves its card out (the saved layout keeps it) and neither the palette nor the Dock offers it. A
    // recording still running when Frame Dock exits is stopped (and saved) by the recorder's destructor
    framecorder::Recorder recorder(framecorder::recorderPath(), framecorder::uiConfPath());
    std::fprintf(stderr, "[record] framecorder %s\n",
                 recorder.available() ? "installed: Record is offered" : "not installed");
    const auto hudLayout = [&recorder](const frame_perf_overlay::Layout& layout) {
        return recorder.available() ? layout
                                    : frame_perf_overlay::withoutType(layout, frame_perf_overlay::WidgetType::Record);
    };
    frame_perf_overlay::GridRenderer renderer(fonts);
    renderer.setLayout(hudLayout(config.layout));
    SettingsPanel settings(fonts);
    settings.setRecordOffered(recorder.available());
    // The Color page (FrameColor). Its model keeps working while the tab is hidden: auto compare, auto presets
    framecolor::OpenVrColorHost colorHost;
    framecolor::ColorModel colorModel(&colorHost, framecolor::ColorPaths::fromEnvironment());
    colorModel.state.language = colorLanguage(config.language);
    framecolor::ColorPage colorPage(colorModel);
    settings.setColorPage(&colorPage);
    Autostart autostart;  // autostart (systemd user unit): state and switching
    VrOverlay vr;
    vr.setVerbose(options.verbose);

    // Registry + scheduler: nothing is read until the demand says someone is looking
    frame_perf_overlay::MetricStore store;
    frame_perf_overlay::Scheduler scheduler;
    // The slow sensors (power monitor, board temperatures, battery gauge) are read on their own thread: a read that
    // blocks for milliseconds in the driver doesn't stall the main loop. Still only while the scheduler demands them
    AsyncSampler sampler(gWakeFd);
    // Hide system's rules: the built-in list is written if the file is missing, an older file gets the newer
    // built-ins (seedOrUpgrade); edits are picked up live. Declared before the bridge, which points to it
    HideRules hideRules(hideRulesPath(options.configPath));
    {
        std::string error;
        if (!hideRules.seedOrUpgrade(error)) {
            std::fprintf(stderr, "[hide] Couldn't write %s: %s\n", hideRules.path().c_str(), error.c_str());
        }
    }
    SensorBridge bridge(sensors, &vr, store);
    bridge.setHideRules(&hideRules);
    bridge.registerSources(scheduler, &sampler);
    // The battery's time left and its notices: read on their own every BatteryWatch::kReadSec while the headset is
    // worn (the Battery source pauses with the HUD, e.g. the wrist turned away), on the sampler's thread like it
    BatteryWatch batteryWatch;
    double nextBatteryRead = 0;
    const int batteryKey = sampler.add([&sensors](SensorSample& s) { sensors.readBattery(s); },
                                       [&batteryWatch, &store](const SensorSample& s, double now) {
                                           BatteryReading r;
                                           r.pct = s.batteryPct;
                                           r.chargeMah = s.batteryChargeMah;
                                           r.currentMa = s.batteryCurrentMa;
                                           r.discharging = s.batteryStatus == "Discharging";
                                           r.status = s.batteryStatus;
                                           r.chargerOnline = s.chargerOnline;
                                           batteryWatch.feed(r, now);
                                           store.setF32(frame_perf_overlay::metric::kBatteryRemainMin,
                                                        batteryWatch.minutesLeft(), now,
                                                        frame_perf_overlay::Reason::NotSampledYet);
                                       });
    // The notices in front of the eyes (one at a time, NoticeView::kShowSec each)
    NoticeView noticeView(fonts);
    std::vector<BatteryWatch::Notice> notices;
    double noticeHideAt = -1;   // the one shown is hidden then (−1: none shown)
    double noticeShownAt = -1;  // it came up then (fading in over NoticeView::kFadeSec) ...
    double noticeEndsAt = -1;   // ... and is faded out by then
    BatteryWatch::Notice noticeShown {};  // the one up (told again if taking the headset off cuts it short)
    constexpr double kNoticeFadeStepSec = 0.04;
    // The first-run guide on the notice's overlay: GuideView::kShowSec of wear each start until the HUD is pressed
    // (config.firstRunGuideDone); a battery notice goes first and the guide comes back after it
    GuideView guideView(fonts);
    bool guideUp = false;        // it is on the overlay now
    bool guideStale = true;      // it needs drawing (the language changed)
    double guideShownSec = 0;    // shown this long this run
    double guideCountedAt = -1;  // guideShownSec counts up to then
    double guideTryAt = 0;       // a failed show is tried again from then (not every wake)
    // The HUD's video controls (the browser's page while its panel is closed), drawn under the HUD's own picture
    MediaStrip mediaStrip(fonts);
    // The controls on the HUD (the layout's 색 보정 / 소리 / 패스스루 / 프로필 / 브라우저 widgets): what they show, the
    // button held down (drawn pressed; − / + repeat while held, 비교 shows the original while held), and a profile
    // switch that would lose unsaved changes waiting for its second tap
    frame_perf_overlay::ControlsView controlsView;
    frame_perf_overlay::WidgetType ctlHeldType = frame_perf_overlay::WidgetType::None;
    int ctlHeldButton = -1;
    HoldRepeat ctlRepeat;
    std::string ctlArmedProfile;
    double ctlArmedUntil = -1;
    bool chromeRunning = false;
    MediaStripState mediaState;
    BrowserSession::Media browserMedia;  // from Chrome's last status
    std::string browserTitle;
    bool mediaWanted = false;      // the strip should be under the HUD
    bool mediaDirty = true;        // the strip needs drawing
    bool hudStripSent = false;     // the HUD's texture last sent has it
    bool hudSendFailed = false;    // the HUD's last send failed (retried at the panel's own pace)
    int hudGridHeight = 0;         // the HUD's own picture's height in that texture (the strip is below)
    int hudGridLeft = 0, hudGridWidth = 0;  // ... and where it is across it (Simple B: centred over a wider strip)
    bool mediaPausedByRemoval = false;
    bool mediaBrowserWasOpen = false;
    // Seen paused and untouched this long (counted only while the strip is in view and not pointed at): from
    // kMediaHideSec it goes the next time it is out of view (not under the eyes or the laser), until it plays again, a
    // button is pressed, the panel closes or the headset is worn again
    double mediaQuietSec = 0;
    double mediaQuietTickAt = -1;
    bool mediaQuietHidden = false;
    constexpr double kMediaHideSec = 120;
    const auto mediaActive = [&]() {
        mediaQuietSec = 0;
        mediaQuietHidden = false;
    };
    bool mediaPausedHeld = false;  // Chrome told to keep the page for the strip's paused video (holdPaused)
    int mediaPauseAnswersSeen = 0;
    // The position after a −10초 / +10초, in place of the state for kMediaPositionSec: any answer within
    // kMediaSeekAnswerSec of the last one pressed (a quick second press shows its own when it comes)
    double mediaSeekAt = -1;
    constexpr double kMediaSeekAnswerSec = 2;
    int mediaAnswerSeen = 0;
    std::string mediaPosition;
    double mediaPositionUntil = -1;
    constexpr double kMediaPositionSec = 3;
    std::vector<uint8_t> hudPixels;  // the HUD's picture with the strip under it
    applyLayout(config.layout, scheduler, bridge);  // a widget off in the layout reads nothing
    scheduler.setUpdateInterval(updateIntervalSec(config));  // the sources read every update follow update_interval_ms
    // Every live layout edit (the editor, the Profiles page) is a compare-and-set here
    LayoutAuthority layoutAuthority(
        config, [&watcher](const Config& c, std::string& error) { return watcher.save(c, error); },
        [&](const frame_perf_overlay::Layout& layout) {
            renderer.setLayout(hudLayout(layout));
            applyLayout(layout, scheduler, bridge);
        });
    // Layout profiles, in profiles/ next to the config file. Read once here; written only on a button press
    const size_t configSlash = watcher.path().find_last_of('/');  // "--config config.json" has no folder: "."
    const std::string configDir = configSlash == std::string::npos ? "." : watcher.path().substr(0, configSlash);
    ProfileStore profiles(configDir + "/profiles");
    {
        std::vector<std::string> warnings;
        profiles.load(warnings);
        for (const std::string& w : warnings) std::fprintf(stderr, "[profiles] %s\n", w.c_str());
        std::fprintf(stderr, "[profiles] %zu profile(s) in %s, active \"%s\"\n", profiles.profiles().size(),
                     profiles.dir().c_str(), config.activeProfile.c_str());
        // Once each: the built-in templates (간단 A, 간단 B, 기본, 상세) as layout-only profiles (never over the user's
        // own), so the quick switch has them. Not over an unreadable config.json (it isn't saved over)
        if (!watcher.lastLoadFailed()) {
            std::vector<std::string> created, errors;
            const bool seeded = seedTemplates(profiles, config.templatesSeeded, created, errors);
            for (const std::string& name : created) std::fprintf(stderr, "[profiles] Added the template %s\n", name.c_str());
            for (const std::string& e : errors) std::fprintf(stderr, "[profiles] Couldn't add a template: %s\n", e.c_str());
            if (seeded) saveConfigNow(config, watcher);  // a failed write is tried again on the next start
        }
    }
    ProfilesUiState profilesUi;
    std::string profileStatus;  // the last action's result, shown on the page
    std::string pendingRename;  // the profile SteamVR's keyboard is open for
    bool renameFromDock = false;  // the keyboard opens for the Dock panel (its full view) rather than the tab
    /** The Profiles page's state as of now (no revision) */
    const auto profilesState = [&]() {
        ProfilesUiState next;
        const Placement livePlacement = placementOf(config);
        for (const Profile& p : profiles.profiles()) {
            ProfilesUiState::Item item {p.id, p.name, !p.placement.has_value()};
            item.useLoses = profiles.useWouldLose(config.layout, livePlacement, config.activeProfile, p.id);
            next.items.push_back(item);
        }
        for (const std::string& id : profiles.unreadable()) next.unreadable.push_back(id + ".json");
        const Profile* active = profiles.find(config.activeProfile);
        next.activeId = active != nullptr ? active->id : std::string();
        next.unsaved = profiles.unsaved(config.layout, config.activeProfile, &livePlacement);
        next.resetLoses = profiles.resetWouldLose(config.layout, config.activeProfile);
        next.maxProfiles = ProfileStore::kMaxProfiles;
        next.status = profileStatus;
        return next;
    };
    /** Rebuilds the Profiles page's state. @return true if it changed (redraw) */
    const auto refreshProfilesUi = [&]() {
        ProfilesUiState next = profilesState();
        if (next.sameAs(profilesUi)) return false;
        next.revision = profilesUi.revision + 1;
        profilesUi = next;
        return true;
    };
    refreshProfilesUi();
    /**
     * The Profiles page's buttons (Use, Save, ... on the selected / active profile) and the footer's quick switch
     * (ProfileUse on its profile). Writes only here
     */
    const auto handleProfileAction = [&](SettingsAction action, const std::string& selectedId) {
        std::string error;   // a short reason in the UI language, on the tab
        std::string detail;  // the OS's or SteamVR's reason (English), in the log only
        bool ok = false;
        const Profile* selected = profiles.find(selectedId);
        const std::string selectedName = selected != nullptr ? selected->name : std::string();
        /** The live layout -> target and, when given, the placement too, in one write. Nothing changes on failure */
        const auto setLive = [&](const frame_perf_overlay::Layout& target, const std::string& activeAfter,
                                 const std::optional<Placement>& placement = std::nullopt) {
            const LayoutAuthority::Result r = layoutAuthority.useLayout(target, activeAfter, placement);
            if (r.applied() && placement) {
                vr.applyConfig(config);  // the HUD moves now
                heldSave.clear();        // the whole config was just written
            }
            if (!r.applied()) {
                error = tr(Tx::ProfileConfigSaveFailed);
                detail = r.error;
            }
            return r.applied();
        };
        switch (action) {
            case SettingsAction::ProfileNew: {
                std::string id;
                if (profiles.create(config.layout, id, error, placementOf(config))) {
                    ok = true;
                    config.activeProfile = id;
                    if (!watcher.save(config, error)) std::fprintf(stderr, "[config] Saving failed: %s\n", error.c_str());
                    settings.selectProfile(id);
                    profileStatus = profileStatusText(config.language, "saved-as", profiles.find(id)->name, "");
                } else {
                    detail = profiles.lastErrorDetail();
                    profileStatus = profileStatusText(config.language, "new-failed", "", error);
                }
                break;
            }
            case SettingsAction::ProfileUse:
                if (selected == nullptr) break;
                ok = setLive(selected->layout, selected->id, selected->placement);  // no placement: keep the live one
                profileStatus = profileStatusText(config.language, ok ? "using" : "use-failed", selectedName, error);
                break;
            case SettingsAction::ProfileSave: {
                const Profile* active = profiles.find(config.activeProfile);
                if (active == nullptr) break;
                const std::string name = active->name;
                ok = profiles.save(active->id, config.layout, error, placementOf(config));
                if (!ok) detail = profiles.lastErrorDetail();
                profileStatus = profileStatusText(config.language, ok ? "saved-to" : "save-failed", name, error);
                break;
            }
            case SettingsAction::ProfileRename:
                if (selected == nullptr) break;
                if (vr.showKeyboard(uiText(config.language).profileKeyboard, selectedName,
                                    static_cast<uint32_t>(ProfileStore::kMaxNameBytes), error, renameFromDock)) {
                    pendingRename = selected->id;
                    profileStatus.clear();
                    ok = true;
                } else {
                    detail = error;  // SteamVR's
                    profileStatus = profileStatusText(config.language, "keyboard-failed", "", "");
                }
                break;
            case SettingsAction::ProfileDelete:
                if (selected == nullptr) break;
                if (profiles.remove(selectedId, error)) {
                    ok = true;
                    profileStatus = profileStatusText(config.language, "deleted", selectedName, "");
                    if (config.activeProfile == selectedId) {
                        config.activeProfile.clear();
                        if (!watcher.save(config, error)) std::fprintf(stderr, "[config] Saving failed: %s\n", error.c_str());
                    }
                } else {
                    detail = profiles.lastErrorDetail();
                    profileStatus = profileStatusText(config.language, "delete-failed", selectedName, error);
                }
                break;
            case SettingsAction::ProfileReset:  // saved profiles stay as they are until Save
                ok = setLive(frame_perf_overlay::defaultLayout(), config.activeProfile);
                profileStatus = profileStatusText(config.language, ok ? "reset" : "reset-failed", "", error);
                break;
            default: break;
        }
        if (!ok && selected == nullptr && action != SettingsAction::ProfileNew && action != SettingsAction::ProfileSave &&
            action != SettingsAction::ProfileReset) {
            profileStatus = profileStatusText(config.language, "gone", "", "");
        }
        std::fprintf(stderr, "[profiles] %s%s%s\n", profileStatus.c_str(), detail.empty() ? "" : " | ", detail.c_str());
        return ok;
    };
    // The Dock: the HUD as a laser target in games, and the Dock panel (quick view; the full view is a second
    // SettingsPanel with its own pointer state over the same config, profiles and colour model)
    DockController dock;
    DockPanel dockView(fonts, &colorModel);
    SettingsPanel dockSettings(fonts);
    dockSettings.setInDock(true);  // 표시 끔 / 게임 중 조작 끔 there take a second tap
    dockSettings.setRecordOffered(recorder.available());
    framecolor::ColorPage dockColorPage(colorModel);
    dockSettings.setColorPage(&dockColorPage);
    // The 기기 page (the tab, the Dock's full view) and the quick view's 소리 · 패스스루 card on one model, and its
    // backends (each on its own thread or libpulse's, started the first time a page that shows it is seen)
    DeviceModel deviceModel;
    // PC 계정 맞추기 only where Frame Sync is set up (its config.json and key, only looked at; the worker reads them)
    deviceModel.sync.present = framesync::settingsPresent(FrameSyncWorker::Paths::defaults().configDir);
    deviceModel.sync.configured = deviceModel.sync.present;
    if (!deviceModel.sync.configured) std::fprintf(stderr, "[frame-sync] Not set up: the 기기 page leaves it out\n");
    DevicePage tabDevicePage(deviceModel);
    settings.setDevicePage(&tabDevicePage);
    DevicePage dockDevicePage(deviceModel);
    dockSettings.setDevicePage(&dockDevicePage);
    dockView.setDevice(deviceModel);
    std::unique_ptr<DeviceBackend> deviceBackend;
    {
        // The room view's route: FRAMEDOCK_ROOMVIEW=openvr|vrcmd|mailbox and its settings (live tests)
        std::string warning;
        const roomview::Route route = roomview::routeFromEnv(warning);
        if (!warning.empty()) std::fprintf(stderr, "[passthrough] %s\n", warning.c_str());
        std::fprintf(stderr, "[passthrough] The room view goes over %s\n", roomview::describe(route).c_str());
        const std::string colorFile = rememberedColorFile();
        deviceBackend = std::make_unique<DeviceBackend>(
            deviceModel, std::make_unique<PulseAudio>(gWakeFd), std::make_unique<CameraSwitchDbus>(gWakeFd, colorFile),
            roomview::makeControl(route, gWakeFd,
                                  [&vr](const std::string& mailbox, const std::string& message, std::string& error) {
                                      return vr.sendMailbox(mailbox, message, error);
                                  }),
            std::make_unique<FrameSyncWorker>(FrameSyncWorker::Paths::defaults(), gWakeFd));
        deviceBackend->setRememberedColor(readRememberedColor(colorFile));
    }
    unsigned deviceRevision = deviceModel.revision();
    vr.setGripWanted(config.dock.gripInput);  // SteamVR Input's manifest is set at connect
    HeldSave dockSave;  // a grab's new offset, written 0.5 s after the release
    // The browser panel (Phase 3): Chrome on its own thread, started the first time the panel opens; the panel and the
    // VR keyboard under it are two more overlays
    const std::string browserDir = browserloop::defaultBrowserDir();
    {
        // A Chrome (or its helpers) left on our profile by a previous run that died without its death signal: gone
        // now, not first when the panel is opened (we hold the instance lock, so no other Frame Dock uses it)
        const std::string profile = browserDir + "/profile";
        const int left = killProfileProcesses(profile);
        if (left > 0) std::fprintf(stderr, "[browser] Stopped %d Chrome process(es) left by an earlier run\n", left);
        const int singletons = left > 0 ? removeStaleSingletons(profile) : 0;
        if (singletons > 0) std::fprintf(stderr, "[browser] Removed %d stale Singleton file(s)\n", singletons);
    }
    std::unique_ptr<BrowserWorker> browserWorker;
    WorkerInput browserInput(browserWorker);
    int pageW = 0, pageH = 0;
    config.browser.pageSize(pageW, pageH);
    BrowserPanel browserPanel(fonts, pageW, pageH);
    VrKeyboard keyboard(fonts);
    BrowserController browserCtl(browserPanel, keyboard, browserInput);
    browserCtl.setView(config.browser.mobile, config.browser.zoomPct, static_cast<int>(config.browser.aspect));
    // 즐겨찾기 (the panel's ☆ and its list): their own small file, read once here
    FavoriteStore favorites(browserloop::defaultFavoritesPath());
    if (!favorites.load()) {
        std::fprintf(stderr, "[browser] Can't read the favorites (%s): starting with none; the file is moved aside "
                             "on the first change\n", favorites.error().c_str());
    }
    browserCtl.setFavorites(&favorites);
    browserCtl.log = [](const std::string& line) { std::fprintf(stderr, "[browser] %s\n", line.c_str()); };
    BrowserSettings appliedBrowser = config.browser;  // what Chrome, the bar and the placement were last given
    BgraFrame browserFrame;
    uint64_t browserStatusSeen = 0;
    bool browserDirty = true;      // the panel's texture needs sending
    bool browserBarStale = true;   // the bar needs drawing (pin, size)
    bool keyboardDirty = true;     // the keyboard's texture needs sending
    bool browserPlaced = true;     // on its anchor (false: no anchor device; hidden until there is)
    browserloop::SessionVisibility browserVisibility;  // what Chrome was last told (screencast, pause, idle timer)
    browserloop::InViewTimer browserInView;            // facing the user, in the field of view (the frame rate)
    bool browserPinned = false;    // 고정: fixed in the room at browserPinnedWorld
    dockmath::Mat34 browserPinnedWorld {};
    std::fprintf(stderr, "[layout] version %u, %zu widget(s) in %zu column(s), panel %dx%d\n", config.layout.version,
                 frame_perf_overlay::enabledWidgets(config.layout).size(), config.layout.columns.size(),
                 renderer.geometry().textureWidth, renderer.geometry().textureHeight);

    // Wait for SteamVR
    std::string lastMessage;
    while (!gStopRequested) {
        std::string message;
        const VrOverlay::ConnectResult result =
            vr.connect(renderer.geometry().textureWidth, renderer.geometry().textureHeight, settings.width(),
                       settings.height(), message);
        if (result == VrOverlay::ConnectResult::Ok) break;
        if (message != lastMessage) {
            if (result == VrOverlay::ConnectResult::NotRunning) {
                std::fprintf(stderr, "[VR] SteamVR isn't running; waiting (retrying every 3 s)\n");
            } else {
                std::fprintf(stderr, "[VR] Connecting failed: %s (retrying in 3 s)\n", message.c_str());
            }
            lastMessage = message;
        }
        const double retryAt = nowSeconds() + 3.0;
        while (!gStopRequested && nowSeconds() < retryAt) {
            waitForWake(retryAt - nowSeconds());
            handleToggleRequest(config, watcher, vr, heldSave);  // toggles still work while waiting
            if (gDumpRequested) {
                gDumpRequested = 0;
                writeDebugDump(scheduler, store, -1, false, nowSeconds());
            }
        }
    }
    if (gStopRequested) return 0;
    std::fprintf(stderr, "[VR] Connected to SteamVR\n");
    vr.applyConfig(config);
    // FrameColor's start: panel brightness, presets, rules; clears a compare bypass left by a crash
    colorModel.start();
    // Gaze-following sharpening: publishes the gaze for the layer while that area is in use (and gives the eye
    // tracking rate back at the next start if a crash left it raised)
    std::optional<framecolor::GazeFeed> gazeFeed;
    auto gazeFitOutcomeSince = std::chrono::steady_clock::now();
    bool gazeFitBusy = false;  // a gaze fit runs or its outcome shows: the main loop wakes to follow it
    gazeFeed.emplace(colorModel.paths().configDir);  // also without the layer: a crash marker's rate goes back
    std::fprintf(stderr, "[color] Vulkan correction layer %s (%s)%s%s\n",
                 colorModel.state.vulkanLayerActive ? "enabled" : "not enabled",
                 colorModel.paths().layerManifest.c_str(), colorModel.state.messageIsError ? ": " : "",
                 colorModel.state.messageIsError ? colorModel.state.message.c_str() : "");
    if (vr.hasSettings()) {
        std::vector<uint8_t> thumbnail;
        renderThumbnail(fonts, kThumbnailSize, thumbnail);
        vr.submitThumbnail(thumbnail.data(), kThumbnailSize);
    }

    double nextPanel = nowSeconds();
    double nextMetricsTick = 0.0;
    double lastVerbose = 0.0;
    bool settingsDirty = true;
    bool settingsVisible = false;
    bool settingsWasVisible = false;
    double lastAutostartRefresh = 0.0;
    double lastPointerEvent = -1e9;  // last dashboard pointer event (poll faster while it's active)
    int activityLevel = -2;          // last HMD activity level we logged (-2 = none yet)
    bool userQuit = false;
    bool deviceLost = false;  // the Vulkan device is gone: exit with kExitCodeDeviceLost (systemd starts us again)
    bool dashboardOpen = false;  // the whole dashboard: read each wake while our panels can take its laser
    RenderStats renderStats;  // redraw cost, in the debug dump
    unsigned loopCount = 0;  // --verbose: loop iterations since the last log line
    int scrollEventsLogged = 0;  // scroll events logged since the dashboard opened
    VrEvents events;  // the event arrays are reused across loops
    int pastWakes = 0;  // loops in a row whose computed wake time was already past (a spin without the minimum sleep)
    constexpr int kPastWakesToLog = 100;
    bool dockDirty = true;     // the Dock panel needs a redraw
    double nextAim = 0.0;      // the next check whether a laser points at the HUD / the Dock panel
    bool aimLeft = false;      // whose laser was on a panel last (the grab's hand)
    uint32_t aimDevice = 0xffffffff;  // that hand's controller (tracked device index)
    // The controller whose laser pressed last (the tab, the Dock panel): a held compare reads its trigger
    uint32_t pressDevice = 0xffffffff;
    // The controller whose laser last pressed the browser panel or its keyboard (whose hand carries it)
    uint32_t browserPressDevice = 0xffffffff;
    colorModel.setTriggerProbe([&vr, &pressDevice] {
        bool down = false;
        if (!vr.readTrigger(pressDevice, down)) return framecolor::TriggerState::Unreadable;
        return down ? framecolor::TriggerState::Pressed : framecolor::TriggerState::Released;
    });
    dockloop::GripEdges gripEdges;  // the pointing hand's grip: presses, releases, and losing it
    bool grabLeft = false;     // the hand carrying a panel
    uint32_t grabDevice = 0xffffffff;
    dockmath::Mat34 grabOffset {};      // the carried panel relative to the carrying controller
    dockmath::Mat34 grabStartWorld {};  // where the carried panel was in the room when the grab started
    bool dockPlaced = true;    // the Dock panel is on its anchor (false: no anchor device; hidden until there is)
    DockView dockHitView = DockView::Collapsed;  // the view the Dock panel's hit areas were last drawn for
    /** A profile from a quick switch (the tab's footer or the Dock's quick view); results on every footer */
    const auto useQuickProfile = [&](const std::string& id) {
        // The journal says when it replaced unsaved changes (confirmed with a second tap)
        const LiveLoss loss = profiles.useWouldLose(config.layout, placementOf(config), config.activeProfile, id);
        if (loss.any()) {
            std::fprintf(stderr, "[profiles] Quick switch replaces unsaved %s\n",
                         loss.layout && loss.placement ? "layout and placement changes"
                         : loss.layout                 ? "layout changes"
                                                       : "placement changes");
        }
        const bool ok = handleProfileAction(SettingsAction::ProfileUse, id);
        const double t = nowSeconds();
        settings.showActionStatus(profileStatus, !ok, t);
        dockSettings.showActionStatus(profileStatus, !ok, t);
        dockView.showStatus(profileStatus, !ok, t);
        if (profiles.find(id) != nullptr) {
            settings.selectProfile(id);
            dockSettings.selectProfile(id);
        }
        refreshProfilesUi();
        nextPanel = std::min(nextPanel, t);
        settingsDirty = dockDirty = true;
    };
    /** What the HUD's controls show, from the models as they are now */
    const auto refreshControls = [&](double t) {
        frame_perf_overlay::ControlsView v;
        v.interactive = config.dock.enabled;
        v.colorAvailable = true;
        for (int i = 0; i < 3; ++i) {
            v.presetNames[i] = colorModel.presetName(i);
            if (v.presetInUse < 0 && colorModel.presetInUse(i)) v.presetInUse = i;
        }
        v.comparing = colorModel.state.compare;
        v.compareUsable = colorModel.state.vulkanLayerActive;
        v.soundKnown = deviceModel.audio.known;
        for (const AudioOutput& o : deviceModel.audio.outputs) {
            if (o.id == deviceModel.audio.defaultId) v.outputName = outputLabel(o);
        }
        v.volume = deviceModel.audio.volume;
        v.muted = deviceModel.audio.muted;
        v.passthrough = deviceModel.passthrough.roomViewKnown ? static_cast<int>(deviceModel.passthrough.mode) : -1;
        v.monoColorUsable = deviceModel.passthrough.cameraSwitchAvailable;
        for (const ProfilesUiState::Item& item : profilesUi.items) {
            if (static_cast<int>(v.profileNames.size()) >= frame_perf_overlay::kControlProfiles) break;
            if (item.id == profilesUi.activeId) v.activeProfile = static_cast<int>(v.profileNames.size());
            if (item.id == ctlArmedProfile && t <= ctlArmedUntil) v.armedProfile = static_cast<int>(v.profileNames.size());
            v.profileNames.push_back(item.name);
            v.profileIds.push_back(item.id);
        }
        v.chromeRunning = chromeRunning;
        v.browserOpen = dock.browserOpen();
        v.recordState = static_cast<int>(recorder.state());
        v.recordSeconds = recorder.elapsedSec(t);
        v.pressedType = ctlHeldType;
        v.pressedButton = ctlHeldButton;
        controlsView = v;
    };
    /** 녹화 (the HUD's card, the Dock's button): starts or stops framecorder's recorder; both show it at once */
    const auto toggleRecording = [&](double t) {
        if (!recorder.toggle(t)) return;
        dockDirty = true;
        nextPanel = std::min(nextPanel, t);
    };
    /** A − / + of the 소리 control once (pressed, or again while held): the 기기 page's steps, throttled as a drag;
     *  the final value goes on the release (releaseControl) */
    const auto stepVolume = [&](int direction) {
        deviceModel.dragVolume(steppedVolume(deviceModel.audio.volume, direction));
    };
    /** The held control button lets go (the trigger's release, the laser leaving the HUD, a new press) */
    const auto releaseControl = [&]() {
        if (ctlHeldType == frame_perf_overlay::WidgetType::None) return false;
        if (ctlHeldType == frame_perf_overlay::WidgetType::ColorPresets) colorModel.release();
        if (ctlHeldType == frame_perf_overlay::WidgetType::Volume) deviceModel.releaseVolume();  // the final value
        ctlHeldType = frame_perf_overlay::WidgetType::None;
        ctlHeldButton = -1;
        ctlRepeat.stop();
        return true;
    };
    /**
     * A press on the HUD at texture px (x, y) of its own picture: a control's button acts at once (− / + repeat while
     * held, 비교 shows the original while held). @return 1 if a button took it, 0 if it fell on a control's button row
     * but no button (or one that can't be pressed now): nothing happens, -1 if it isn't on a control's buttons (the
     * HUD's own click and grab, as before)
     */
    const auto pressControl = [&](double x, double y, double t) -> int {
        const frame_perf_overlay::GridGeometry& g = renderer.geometry();
        if (g.scale <= 0) return -1;
        const double px = (x - hudGridLeft) / g.scale, py = y / g.scale;
        for (const frame_perf_overlay::WidgetSlot& slot : g.slots) {
            if (!frame_perf_overlay::isControlWidget(slot.widget.type)) continue;
            if (px < slot.x || px > slot.x + slot.w || py < slot.y || py > slot.y + slot.h) continue;
            const std::vector<frame_perf_overlay::ControlButton> buttons =
                frame_perf_overlay::controlButtons(slot.widget, &controlsView, slot.x, slot.y);
            if (buttons.empty() || py < buttons.front().y) return -1;  // the title line: the HUD's, as before
            const frame_perf_overlay::ControlButton* hit = nullptr;
            for (const frame_perf_overlay::ControlButton& b : buttons) {
                if (b.id >= 0 && b.usable && px >= b.x && px <= b.x + b.w && py >= b.y && py <= b.y + b.h) hit = &b;
            }
            if (hit == nullptr) return 0;
            using frame_perf_overlay::WidgetType;
            releaseControl();  // one lost along the way (no release came): not still held under this one
            if (slot.widget.type != WidgetType::Profiles) ctlArmedProfile.clear();  // any other press disarms
            ctlHeldType = slot.widget.type;
            ctlHeldButton = hit->id;
            switch (slot.widget.type) {
                case WidgetType::ColorPresets:
                    colorModel.state.saveMode = false;  // a tap applies, never saves into a slot
                    if (hit->id < 3) {
                        colorModel.pressPreset(hit->id);
                    } else {
                        colorModel.pressCompare();  // the original while held (released on the trigger's release)
                    }
                    settingsDirty = dockDirty = true;
                    break;
                case WidgetType::Volume:
                    if (hit->id == 0) {
                        deviceModel.setMuted(!deviceModel.audio.muted);
                    } else {
                        stepVolume(hit->id == 1 ? -1 : +1);
                        ctlRepeat.start(t);
                    }
                    break;
                case WidgetType::Passthrough:
                    deviceModel.setPassthrough(hit->id == 0   ? PassthroughMode::Off
                                               : hit->id == 1 ? PassthroughMode::Mono
                                                              : PassthroughMode::Color);
                    break;
                case WidgetType::Profiles: {
                    // The profile the button showed (the list may have changed since it was drawn)
                    const size_t i = static_cast<size_t>(hit->id);
                    if (i >= controlsView.profileIds.size()) break;
                    const std::string& shownId = controlsView.profileIds[i];
                    const auto found = std::find_if(profilesUi.items.begin(), profilesUi.items.end(),
                                                    [&](const ProfilesUiState::Item& it) { return it.id == shownId; });
                    if (found == profilesUi.items.end()) break;
                    const ProfilesUiState::Item& item = *found;
                    if (item.id == profilesUi.activeId && !profilesUi.unsaved) break;
                    // The quick switch's rule: one tap, two when it would throw away unsaved changes
                    if (item.useLoses.any() && !(ctlArmedProfile == item.id && t <= ctlArmedUntil)) {
                        ctlArmedProfile = item.id;
                        ctlArmedUntil = t + SettingsPanel::kQuitConfirmSec;  // the line goes when it runs out (loop)
                        break;
                    }
                    ctlArmedProfile.clear();
                    useQuickProfile(item.id);
                    break;
                }
                case WidgetType::Browser:
                    if (dock.browserOpen()) {
                        dock.closeBrowser(t);
                    } else {
                        dock.openBrowser(t);
                    }
                    break;
                case WidgetType::Record: toggleRecording(t); break;
                default: break;
            }
            std::fprintf(stderr, "[controls] %s button %d\n", frame_perf_overlay::widgetName(slot.widget.type),
                         hit->id);
            return 1;
        }
        return -1;
    };

    /**
     * One pointer event on a SettingsPanel: the dashboard tab's, or (inDock) the Dock panel's full view, where a
     * Placement button that moves the HUD also starts the fine-adjust session (the Dock panel stays put)
     */
    const auto handlePanelPointer = [&](SettingsPanel& panel, const PointerInput& input, bool inDock) {
        bool& dirty = inDock ? dockDirty : settingsDirty;
        switch (input.type) {
            case PointerInput::Type::Move: dirty |= panel.pointerMove(input.x, input.y); break;
            case PointerInput::Type::Scroll: {
                const bool scrolled = panel.scroll(input.scrollDy);
                dirty |= scrolled;
                if (!inDock && scrollEventsLogged < 3) {  // the first few per dashboard open: sign and speed
                    ++scrollEventsLogged;
                    std::fprintf(stderr, "[editor] scroll event ydelta %+.3f -> %s (lanes offset %.0f px)\n",
                                 input.scrollDy, scrolled ? "scrolled" : "nothing to scroll", panel.editor().scrollY());
                }
                break;
            }
            case PointerInput::Type::Down: {
                // The two-tap rule needs what Use / Reset would throw away as of now, not as of the last draw
                panel.setProfilesState(profilesState());
                const double t = nowSeconds();
                const SettingsAction action = panel.pointerDown(input.x, input.y, t);
                if (action == SettingsAction::Quit) {
                    std::fprintf(stderr, "[VR] Quit on the %s; exiting\n", inDock ? "Dock panel" : "dashboard tab");
                    userQuit = true;
                } else if (action == SettingsAction::AutostartOn || action == SettingsAction::AutostartOff) {
                    // Only starts systemctl --user enable / disable in a child (doesn't wait for it)
                    autostart.request(action == SettingsAction::AutostartOn);
                } else if (action >= SettingsAction::ProfileNew && action <= SettingsAction::ProfileReset) {
                    renameFromDock = inDock;
                    const bool ok = handleProfileAction(action, panel.selectedProfileId());
                    settings.showActionStatus(profileStatus, !ok, t);  // the footer, every page, both panels
                    dockSettings.showActionStatus(profileStatus, !ok, t);
                    dockView.showStatus(profileStatus, !ok, t);
                    refreshProfilesUi();
                    nextPanel = std::min(nextPanel, t);  // a new layout shows on the panel now
                } else if (action == SettingsAction::BrowserHomeFromPage) {
                    // The page showing now becomes the home page (http / https only)
                    const std::string url = browserWorker ? browserWorker->status().url : std::string();
                    if (browserloop::usableHome(url) && url != config.browser.home) {
                        config.browser.home = url;
                        saveConfigNow(config, watcher);
                        heldSave.clear();
                        std::fprintf(stderr, "[browser] Home page: %s\n", url.c_str());
                    }
                } else if (action == SettingsAction::BrowserStopChrome) {
                    if (browserWorker) browserWorker->post([](BrowserSession& s, double tt) { s.stop(tt); });
                    std::fprintf(stderr, "[browser] Closing Chrome (the Browser page's button)\n");
                } else if (action == SettingsAction::BrowserStartChrome) {
                    // Chrome 켜기: the panel opens (that starts Chrome); already open (Chrome quit under it): Chrome
                    // starts in it. Without in-game interaction there is no panel: Chrome starts, shown once it opens
                    std::fprintf(stderr, "[browser] Starting Chrome (the Browser page's button)\n");
                    if (!config.dock.enabled) {
                        // No panel to show it on: a Chrome started now would run unseen and quit when idle
                        panel.showActionStatus(tr(Tx::BrowserNeedsDock), true, t);
                    } else if (!dock.browserOpen()) {
                        dock.openBrowser(t);
                    } else {
                        if (!browserWorker) {
                            browserWorker = std::make_unique<BrowserWorker>(
                                browserloop::sessionOptions(config.browser, browserDir), gWakeFd);
                        }
                        browserWorker->post([](BrowserSession& s, double tt) { s.open(tt); });
                    }
                } else if (action >= SettingsAction::QuickProfile0 && action <= SettingsAction::QuickProfile5) {
                    // The footer's quick switch, on any page (a second tap first when it would throw away unsaved
                    // changes: the panel only returns it then)
                    const std::string id = panel.quickProfileId(action);
                    if (!id.empty()) useQuickProfile(id);
                } else {
                    // A button that repeats while held saves on release (or every kHeldSaveSec), not per step
                    handleSettingsAction(action, config, watcher, vr, heldSave, panel.repeating(),
                                         t);
                    if (inDock && panel.page() == SettingsPage::Placement && changesHudPlacement(action)) {
                        dock.startAdjust(t);
                    }
                }
                settingsDirty = dockDirty = true;
                break;
            }
            case PointerInput::Type::Up: dirty |= panel.pointerUp(input.x, input.y); break;
            case PointerInput::Type::Leave: dirty |= panel.pointerLeave(); break;
        }
        // An editor drop / palette / settings change goes through the compare-and-set
        uint32_t base = 0;
        frame_perf_overlay::Layout edited;
        if (panel.takeLayoutCommit(base, edited)) {
            const LayoutAuthority::Result r = layoutAuthority.trySetLayout(base, edited);
            panel.layoutCommitResult(r.applied() ? LayoutEditor::CommitOutcome::Applied
                                     : r.outcome == LayoutAuthority::Outcome::Stale ? LayoutEditor::CommitOutcome::Stale
                                                                                    : LayoutEditor::CommitOutcome::Failed);
            std::fprintf(stderr, "[editor] layout edit on version %u: %s (now version %u)%s%s\n", base,
                         r.applied() ? "applied" : (r.outcome == LayoutAuthority::Outcome::Stale ? "stale" : "save failed"),
                         r.version, r.error.empty() ? "" : ": ", r.error.c_str());
            if (r.applied()) nextPanel = std::min(nextPanel, nowSeconds());  // show it on the panel now
            settingsDirty = dockDirty = true;
        }
    };
    /**
     * The Dock panel on its anchor at its own offset (not while frozen in the room or carried). Without an anchor (the
     * wrist's controller off) it is hidden rather than shown wherever it was last, and shown again once it is placed
     */
    const auto placeDockAttached = [&](double t) {
        if (dock.adjusting() || dock.grabbing() == DockTarget::Dock) return;
        const bool placed = vr.placeDockRelative(vr.anchorDevice(config, t), dockLocal(config));
        if (placed == dockPlaced) return;
        dockPlaced = placed;
        if (!placed) {
            std::fprintf(stderr, "[dock] Can't put the Dock panel on its anchor (%s): hidden until it can\n",
                         config.attachment == Attachment::Head ? "the headset" : "the wrist's controller");
            vr.showDock(false);
        } else {
            std::fprintf(stderr, "[dock] The Dock panel is on its anchor again\n");
            dockDirty = true;  // shown with the next draw
        }
    };
    /** A grab begins: the panel follows the hand whose laser is on it, as it is relative to that hand now */
    /** The keyboard relative to the browser panel, for the panel's size now */
    const auto keyboardOffset = [&]() {
        const double browserH = browserloop::panelWidthM(config.browser, browserPanel.width(), browserPanel.height()) *
                                browserPanel.height() / browserPanel.width();
        const double kbW = browserloop::keyboardWidthM(config.browser.widthM);
        return browserloop::keyboardUnderBrowser(browserH, kbW * VrKeyboard::kHeight / VrKeyboard::kWidth);
    };
    /**
     * The browser panel (and the keyboard under it) on its anchor, or fixed in the room (고정); not while carried.
     * Without an anchor (that wrist's controller off) both are hidden until it can be placed
     */
    const auto placeBrowserAttached = [&](double t) {
        if (!dock.browserOpen() || dock.grabbing() == DockTarget::Browser) return;
        bool placed = true;
        if (browserPinned) {
            vr.placeBrowserAbsolute(browserPinnedWorld, keyboardOffset());
        } else {
            placed = vr.placeBrowserRelative(vr.anchorDeviceFor(browserloop::browserAttachment(config), t),
                                             browserloop::browserLocal(config), keyboardOffset());
        }
        if (placed == browserPlaced) return;
        browserPlaced = placed;
        if (!placed) {
            std::fprintf(stderr, "[browser] Can't put the browser panel on its anchor: hidden until it can\n");
            vr.showAux(VrOverlay::Aux::Browser, false);
            vr.showAux(VrOverlay::Aux::Keyboard, false);
        } else {
            std::fprintf(stderr, "[browser] The browser panel is on its anchor again\n");
            browserDirty = keyboardDirty = true;  // shown with the next send
        }
    };
    const auto beginGrab = [&](DockTarget target, double t) {
        const DockPoses poses = vr.readDockPoses(t);
        // The browser panel by its handle: the hand that pressed it (pinned, both rays may be on it). The grip and
        // the other panels: the pointing hand
        const bool left = (target == DockTarget::Browser || target == DockTarget::Keyboard) &&
                                  dock.grabSource() == GrabSource::Trigger
                              ? dockloop::pressingHandLeft(poses, browserPressDevice, aimLeft)
                              : aimLeft;
        const DockPoses::Device& hand = poses.hand(left);
        dockmath::Mat34 world {};
        const bool placed = target == DockTarget::Hud    ? vr.hudWorld(world)
                            : target == DockTarget::Dock ? vr.dockWorld(world)
                                                         : vr.auxWorld(VrOverlay::Aux::Browser, world);
        const char* name = target == DockTarget::Hud ? "HUD" : target == DockTarget::Dock ? "Dock panel" : "browser panel";
        if (!hand.valid || !placed) {
            std::fprintf(stderr, "[dock] Can't carry the %s: %s isn't tracked\n", name,
                         !hand.valid ? "the controller" : "it");
            dock.cancelGrab(t);
            return;
        }
        grabLeft = left;
        grabDevice = hand.index;
        grabOffset = dockmath::grabOffset(hand.pose, world);
        grabStartWorld = world;
        if (target == DockTarget::Hud) {
            vr.grabHud(hand.index, grabOffset);
        } else if (target == DockTarget::Dock) {
            vr.placeDockRelative(hand.index, grabOffset);
        } else {
            vr.placeBrowserRelative(hand.index, grabOffset, keyboardOffset());
        }
        std::fprintf(stderr, "[dock] Carrying the %s with the %s hand (%s)\n", name, grabLeft ? "left" : "right",
                     dock.grabSource() == GrabSource::Grip ? "grip" : "trigger held");
    };
    /**
     * A grab ends: keep where it went (relative to its anchor, saved 0.5 s later), or put it back: cancelled, a device
     * not tracked, or let go where it was grabbed (a slow click is no move: no save, no "unsaved")
     */
    const auto finishGrab = [&](DockTarget target, bool ended, double t) {
        if (target == DockTarget::Browser && browserPinned) {
            // Fixed in the room: it stays where the hand let it go (unless cancelled, untracked or barely moved)
            const DockPoses poses = ended ? vr.readDockPoses(t) : DockPoses {};
            const DockPoses::Device& hand = poses.hand(grabLeft);
            if (ended && hand.valid && hand.index == grabDevice) {
                const dockmath::Mat34 world = dockmath::multiply(hand.pose, grabOffset);
                if (dockmath::movedByGrab(grabStartWorld, world)) browserPinnedWorld = world;
            }
            placeBrowserAttached(t);
            std::fprintf(stderr, "[dock] browser panel (fixed in the room) let go\n");
            return;
        }
        const Attachment anchor =
            target == DockTarget::Browser ? browserloop::browserAttachment(config) : config.attachment;
        const dockloop::GrabRelease release =
            dockloop::releaseGrab(ended, ended ? vr.readDockPoses(t) : DockPoses {}, anchor, grabLeft, grabDevice,
                                  grabStartWorld, grabOffset);
        if (release.keep()) {
            if (target == DockTarget::Browser) {
                browserloop::setBrowserPlacementFrom(release.local, config);
                appliedBrowser = config.browser;  // a new offset, nothing for Chrome
            } else if (target == DockTarget::Hud) {
                // The overlay's centre is below the config's while the video controls hang under the HUD
                dockmath::setHudPlacementFrom(vr.hudBaseFrom(release.local), config);
            } else if (config.attachment == Attachment::Head) {
                config.dock.head = dockmath::headPoseFrom(release.local);
            } else {
                (config.attachment == Attachment::LeftWrist ? config.dock.leftWrist : config.dock.rightWrist) =
                    dockmath::wristPoseFrom(release.local);
            }
            dockSave.request(t);
        }
        if (target == DockTarget::Hud) {
            vr.releaseHud();
            vr.applyConfig(config);
        } else if (target == DockTarget::Dock) {
            placeDockAttached(t);
        } else {
            placeBrowserAttached(t);
        }
        using Outcome = dockloop::GrabRelease::Outcome;
        std::fprintf(stderr, "[dock] %s %s\n",
                     target == DockTarget::Hud ? "HUD" : target == DockTarget::Dock ? "Dock panel" : "browser panel",
                     release.outcome == Outcome::Keep        ? "moved; the new place is saved"
                     : release.outcome == Outcome::InPlace   ? "let go where it was grabbed: no change"
                     : release.outcome == Outcome::Untracked ? "put back where it was (not tracked at the release)"
                                                             : "put back where it was");
        settingsDirty = dockDirty = true;
    };
    /** A button of the HUD's video controls: the browser panel, or a command to the page */
    const auto mediaPressed = [&](MediaAction action, double t) {
        if (action == MediaAction::None || !browserWorker) return;
        // Any other button: the state shows again at once (not a seek's position)
        mediaSeekAt = -1;
        mediaPosition.clear();
        mediaPositionUntil = -1;
        if (action == MediaAction::OpenPanel) {
            std::fprintf(stderr, "[browser] Video controls: opening the panel\n");
            dock.openBrowser(t);
            return;
        }
        using Command = BrowserSession::MediaCommand;
        const Command command = action == MediaAction::Back        ? Command::Back
                                : action == MediaAction::Forward   ? Command::Forward
                                : action == MediaAction::PlayPause ? Command::PlayPause
                                                                   : Command::Mute;
        mediaPausedByRemoval = false;
        mediaActive();
        mediaSeekAt = command == Command::Back || command == Command::Forward ? t : -1;
        browserWorker->post([command](BrowserSession& s, double) { s.media(command); });
    };
    /**
     * The Dock panel's view switched (전체, ← 빠른) since its hit areas were drawn: the pointers let go and the new view
     * is drawn at once, so the rest of this batch of events hits the new view's buttons, not the old one's
     */
    const auto syncDockView = [&](double t) {
        if (!dock.expanded() || dock.view() == dockHitView) return;
        dockView.pointerLeave();
        dockSettings.pointerLeave();
        dockloop::renderDock(dock.view(), dock.adjusting(), dockView, dockSettings, config, autostart.status(),
                             profilesUi, t);
        dockHitView = dock.view();
        dockDirty = true;  // sent with the next draw
    };
    /** A press on the Dock panel's own buttons (quick view, the full view's bar) */
    const auto handleDockPress = [&](const DockPress& press, double t) {
        switch (press.kind) {
            case DockPress::Kind::UseProfile: useQuickProfile(press.profileId); break;
            case DockPress::Kind::ShowFull: dock.openFull(t); break;
            case DockPress::Kind::ShowQuick: dock.openQuick(t); break;
            case DockPress::Kind::Collapse: dock.collapse(CollapseReason::Button, t); break;
            case DockPress::Kind::Done: dock.finishAdjust(t); break;
            case DockPress::Kind::GrabHandle: dock.startGrab(DockTarget::Dock, GrabSource::Trigger, t); break;
            case DockPress::Kind::ColorChanged: settingsDirty = true; break;  // the tab's Color page too
            case DockPress::Kind::DeviceChanged: settingsDirty = true; break;  // the tab's 기기 page too
            case DockPress::Kind::OpenBrowser: dock.openBrowser(t); break;
            case DockPress::Kind::QuitChrome:
                // 끄기 (tapped twice): Chrome closes; its panel goes too if it was open
                if (browserWorker) browserWorker->post([](BrowserSession& s, double tt) { s.stop(tt); });
                if (dock.browserOpen()) dock.closeBrowser(t);
                std::fprintf(stderr, "[browser] Closing Chrome (the Dock's 끄기)\n");
                break;
            case DockPress::Kind::ToggleRecord: toggleRecording(t); break;
            case DockPress::Kind::None: break;
        }
        syncDockView(t);
        dockDirty = true;
    };
    /** What the browser panel's controller asks for (from the panel, the keyboard, a held key's repeat) */
    const auto handleBrowserRequests = [&](const std::vector<BrowserController::Request>& requests, DockTarget from,
                                           double t) {
        for (const BrowserController::Request r : requests) {
            switch (r) {
                case BrowserController::Request::Grab:
                    dock.startGrab(from == DockTarget::Keyboard ? DockTarget::Keyboard : DockTarget::Browser,
                                   GrabSource::Trigger, t);
                    break;
                case BrowserController::Request::Close: dock.closeBrowser(t); break;
                case BrowserController::Request::QuitChrome:
                    if (browserWorker) browserWorker->post([](BrowserSession& s, double tt) { s.stop(tt); });
                    dock.closeBrowser(t);
                    std::fprintf(stderr, "[browser] Closing Chrome (the panel's 끄기)\n");
                    break;
                case BrowserController::Request::SizeDown:
                case BrowserController::Request::SizeUp:
                    config.browser.widthM = browserloop::stepWidth(
                        config.browser.widthM, r == BrowserController::Request::SizeUp ? 1 : -1);
                    appliedBrowser.widthM = config.browser.widthM;
                    dockSave.request(t);
                    browserBarStale = browserDirty = keyboardDirty = true;
                    placeBrowserAttached(t);  // the keyboard moves with the panel's lower edge
                    break;
                case BrowserController::Request::TogglePin: {
                    dockmath::Mat34 world {};
                    vr.readDockPoses(t);
                    if (!browserPinned && vr.auxWorld(VrOverlay::Aux::Browser, world)) {
                        browserPinned = true;
                        browserPinnedWorld = world;
                    } else {
                        browserPinned = false;
                    }
                    std::fprintf(stderr, "[browser] %s\n", browserPinned ? "Fixed in the room" : "Back on its anchor");
                    browserBarStale = true;
                    placeBrowserAttached(t);
                    break;
                }
                case BrowserController::Request::Keyboard: dock.setKeyboardOpen(browserCtl.keyboardOpen(), t); break;
                case BrowserController::Request::Translate:
                    if (browserWorker) browserWorker->post([](BrowserSession& s, double) { s.translate(); });
                    std::fprintf(stderr, "[browser] 번역\n");
                    break;
                // 보기: the setting changes here; the config-changed step below passes it on (Chrome, the bar) and
                // it is saved like the panel's size
                case BrowserController::Request::MobileOff:
                case BrowserController::Request::MobileOn:
                    config.browser.mobile = r == BrowserController::Request::MobileOn;
                    dockSave.request(t);
                    break;
                case BrowserController::Request::ZoomOut:
                case BrowserController::Request::ZoomIn:
                case BrowserController::Request::ZoomReset:
                    config.browser.zoomPct = r == BrowserController::Request::ZoomReset ? 100
                                             : stepZoom(config.browser.zoomPct,
                                                        r == BrowserController::Request::ZoomIn ? 1 : -1);
                    dockSave.request(t);
                    break;
                case BrowserController::Request::AspectWide:
                case BrowserController::Request::AspectStandard:
                case BrowserController::Request::AspectSquare:
                case BrowserController::Request::AspectTall:
                    config.browser.aspect = r == BrowserController::Request::AspectStandard ? BrowserAspect::Standard
                                            : r == BrowserController::Request::AspectSquare ? BrowserAspect::Square
                                            : r == BrowserController::Request::AspectTall   ? BrowserAspect::Tall
                                                                                            : BrowserAspect::Wide;
                    dockSave.request(t);
                    break;
            }
        }
    };
    /**
     * Draws what changed on the browser panel and its keyboard and sends it: only while the panel is seen (open, the
     * headset worn, on its anchor). Hidden, nothing is drawn or uploaded; what changed meanwhile goes up when it shows
     */
    const auto sendBrowser = [&]() {
        if (!browserVisibility.showing()) return;
        if (browserCtl.takeBarDirty() || browserBarStale) {
            browserPanel.renderBar(browserCtl.barState(browserPinned, config.browser.widthM,
                                                       config.browser.widthM > browserloop::kMinWidthM + 1e-9,
                                                       config.browser.widthM < browserloop::kMaxWidthM - 1e-9));
            browserBarStale = false;
            browserDirty = true;
        }
        if (browserDirty) {
            const bool sent = vr.submitAux(VrOverlay::Aux::Browser, browserPanel.pixels(), browserPanel.width(),
                                           browserPanel.height(),
                                           browserloop::panelWidthM(config.browser, browserPanel.width(),
                                                                    browserPanel.height()));
            if (sent) vr.showAux(VrOverlay::Aux::Browser, true);
            browserDirty = false;
        }
        if (browserCtl.takeKeyboardDirty()) keyboardDirty = true;
        if (dock.keyboardOpen() && keyboardDirty) {
            keyboard.render();
            const bool sent = vr.submitAux(VrOverlay::Aux::Keyboard, keyboard.pixels(), VrKeyboard::kWidth,
                                           VrKeyboard::kHeight, browserloop::keyboardWidthM(config.browser.widthM));
            if (sent) vr.showAux(VrOverlay::Aux::Keyboard, true);
            keyboardDirty = false;
        }
    };
    while (!gStopRequested && !userQuit) {
        ++loopCount;
        const double loopStart = nowSeconds();
        // Panel placement and fade (wrist fade, hide when tracking is lost, retry refused calls).
        // Done early if we're awake for something else anyway, to wake less often. Not while the headset is off (in
        // standby nobody sees the wrist fade; it runs again at once when it is worn, below)
        if (hmdInUse(activityLevel) && loopStart >= vr.nextPlacementAt() - kPlacementCoalesceSec) {
            vr.updatePlacement(config, loopStart);
        }

        // ---- Metrics deadline (2 Hz) ----
        // A toggle (SIGUSR1) is handled on whatever wake it caused: waitForWake() returns at once while the flag
        // is set, so leaving it for the next tick would spin
        if (handleToggleRequest(config, watcher, vr, heldSave)) settingsDirty = dockDirty = true;
        const bool metricsTick = loopStart >= nextMetricsTick - kCoalesceSec;
        if (metricsTick) {
            nextMetricsTick = loopStart + kMetricsTickSec;
            if (watcher.reloadIfChanged(config)) {
                vr.applyConfig(config);
                scheduler.setUpdateInterval(updateIntervalSec(config));
                nextPanel = std::min(nextPanel, loopStart);  // a new interval (or anything else) shows at once
                loadFonts(fonts, config);
                renderer.invalidateStatic();  // the fonts may have changed
                // A hand-edited layout takes effect at once (widgets off stop their sources)
                if (renderer.setLayout(hudLayout(config.layout))) {
                    applyLayout(config.layout, scheduler, bridge);
                    std::fprintf(stderr, "[layout] reloaded: version %u, %zu widget(s), panel %dx%d\n",
                                 config.layout.version, frame_perf_overlay::enabledWidgets(config.layout).size(),
                                 renderer.geometry().textureWidth, renderer.geometry().textureHeight);
                }
                settingsDirty = dockDirty = true;
            }
        }
        // ---- Input deadline: dashboard events only while the tab is visible; the HUD's and the Dock panel's while
        // a laser is on them or the Dock is open; SteamVR events on the tick ----
        const bool dockLive = config.dock.enabled && config.visible && hmdInUse(activityLevel);
        // The browser panel doesn't need the HUD shown: only the feature on and the headset worn
        const bool browserLive = config.dock.enabled && hmdInUse(activityLevel);
        const bool dockInput = (dockLive && (dock.expanded() || dock.grabbing() != DockTarget::None ||
                                             dock.interactive(DockTarget::Hud, loopStart) ||
                                             dock.interactive(DockTarget::Dock, loopStart))) ||
                               (browserLive && dock.browserOpen());
        // The dashboard's laser can reach our panels while it is open: whether it is, each wake (one IPC) while a panel
        // of ours can take it, not only on the 2 Hz tick (a press there would wait up to half a second)
        if (dockLive || browserLive) {
            dashboardOpen = vr.dashboardOpen();
            vr.noteDashboardOpen(dashboardOpen, loopStart);
        }
        // Our overlays also after the laser left one, a moment (its last Up / Leave), and all while the dashboard is open
        const bool overlayInput = dockInput || ((dockLive || browserLive) && vr.overlayEventsWanted(loopStart));
        if (metricsTick || settingsVisible || overlayInput) {
            // Each wake only the overlays a laser can be on (dockloop::EventPolling) and the tab while it shows;
            // everything on the 2 Hz tick
            vr.pollEvents(events, metricsTick, overlayInput || metricsTick, metricsTick || settingsVisible);
        } else {
            events.clear();
        }
        // SteamVR itself quitting (VREvent_Quit) exits with 0. Close on the dashboard icon (VREvent_OverlayClosed)
        // is the user quitting, like Quit on the tab: exit code 3, so systemd doesn't restart us
        if (events.quit) break;
        // General's 앞의 앱 row at once, but only while it's wanted (a full General on the panel, someone looking)
        if (events.sceneAppChanged) scheduler.requestRead(frame_perf_overlay::SourceId::SceneApp);
        // SteamVR's room view (passthrough) turned on or off
        if (events.roomView >= 0) {
            std::fprintf(stderr, "[passthrough] SteamVR: room view %s\n", events.roomView == 1 ? "shown" : "hidden");
            deviceBackend->roomViewEvent(events.roomView == 1);
        }
        // The keyboard opened by Rename on the Profiles page
        if (events.keyboardDone && !pendingRename.empty()) {
            std::string error;
            const std::string id = pendingRename;
            pendingRename.clear();
            const bool renamed = profiles.rename(id, events.keyboardText, error);
            profileStatus = renamed ? profileStatusText(config.language, "renamed", profiles.find(id)->name, "")
                                    : profileStatusText(config.language, "rename-failed", "", error);
            std::fprintf(stderr, "[profiles] %s%s%s\n", profileStatus.c_str(), renamed ? "" : " | ",
                         renamed ? "" : profiles.lastErrorDetail().c_str());
            settings.showActionStatus(profileStatus, !renamed, nowSeconds());
            if (refreshProfilesUi()) settingsDirty = dockDirty = true;
        }
        if (events.keyboardClosed) pendingRename.clear();
        if (events.closeRequested) {
            std::fprintf(stderr, "[VR] Closed from the dashboard; exiting\n");
            userQuit = true;
            break;
        }
        if (metricsTick && !vr.steamVrAlive(loopStart)) {
            std::fprintf(stderr, "[VR] vrserver is gone; exiting\n");
            break;
        }

        // Pointer on the dashboard tab
        if (!events.pointer.empty()) {
            lastPointerEvent = loopStart;
            settings.pointerSeen(loopStart);  // a held button's safety stop counts from the last event
            colorModel.pointerSeen(std::chrono::steady_clock::now());  // and a held compare's
            // A press works on the layout as it is now (a file reload may have changed it)
            settingsDirty |= settings.syncLayout(config.layout);
        }
        for (const PointerInput& input : events.pointer) {
            if (input.type == PointerInput::Type::Down) pressDevice = input.device;
            handlePanelPointer(settings, input, false);
        }

        // ---- The Dock: the HUD and the Dock panel worked by the other hand's laser, in a game too ----
        {
            const double t = nowSeconds();
            dock.configure(config.dock.enabled, config.dock.collapseSec, t);
            // HUD hidden, headset off: collapse, and cancel a grab of either panel (also with the Dock collapsed)
            dock.setLive(dockLive, t);
            dock.setBrowserLive(browserLive, t);
            // SteamVR's keyboard opened from the Dock panel (a profile's name): it doesn't collapse under it
            dock.setKeyboardUp(!pendingRename.empty() && renameFromDock, t);
            // Where the pointing hand's laser is (the HUD only while it shows; the Dock panel while open). Idle, this
            // is the only polling the Dock adds: one pose read every kDockIdlePollSec, shared with the wrist fade's
            const bool browserAimable = browserLive && dock.browserOpen();
            if ((dockLive || browserAimable) && t >= nextAim - kPlacementCoalesceSec) {
                const DockPoses poses = vr.readDockPoses(t);
                // A carried panel whose controller or anchor lost tracking goes back where it was (a browser panel
                // fixed in the room has no anchor: only the carrying controller counts)
                const Attachment grabAnchor =
                    dock.grabbing() != DockTarget::Browser ? config.attachment
                    : browserPinned ? (grabLeft ? Attachment::LeftWrist : Attachment::RightWrist)
                                    : browserloop::browserAttachment(config);
                if (dock.grabbing() != DockTarget::None && !dockloop::grabTracked(poses, grabAnchor, grabLeft, grabDevice)) {
                    std::fprintf(stderr, "[dock] Tracking lost while carrying a panel: cancelled\n");
                    dock.cancelGrab(t);
                }
                const PointerHands dockHands = pointerHands(config.dock.pointerHand, config.attachment);
                const PointerHands browserHands = browserloop::browserHands(config, browserPinned);
                bool hudOn = false, dockOn = false, browserOn = false, keyboardOn = false;
                double nearest = std::numeric_limits<double>::infinity();
                const auto aimAt = [&](const dockmath::Mat34& panel, double w, double h, double margin, bool& on,
                                       const PointerHands& hands) {
                    for (const bool left : {true, false}) {
                        if (!(left ? hands.left : hands.right)) continue;
                        const DockPoses::Device& hand = poses.hand(left);
                        if (!hand.valid) continue;
                        const dockmath::PanelHit hit = dockmath::hitPanel(panel, dockmath::rayFromPose(hand.laser));
                        if (!dockmath::onPanel(hit, w, h, margin)) continue;
                        on = true;
                        if (hit.distance < nearest) {
                            nearest = hit.distance;
                            aimLeft = left;
                            aimDevice = hand.index;
                        }
                    }
                };
                dockmath::Mat34 world {};
                if (dockLive && vr.panelShown() && vr.hudWorld(world)) {
                    aimAt(world, vr.hudWidthM(), vr.hudHeightM(),
                          dock.interactive(DockTarget::Hud, t) ? kAimLeaveMarginM : kAimArriveMarginM, hudOn, dockHands);
                }
                if (dockLive && dock.expanded() && vr.dockWorld(world)) {
                    aimAt(world, vr.dockWidthM(), vr.dockHeightM(),
                          dock.interactive(DockTarget::Dock, t) ? kAimLeaveMarginM : kAimArriveMarginM, dockOn,
                          dockHands);
                }
                if (browserAimable && vr.auxWorld(VrOverlay::Aux::Browser, world)) {
                    aimAt(world, vr.auxWidthM(VrOverlay::Aux::Browser), vr.auxHeightM(VrOverlay::Aux::Browser),
                          dock.interactive(DockTarget::Browser, t) ? kAimLeaveMarginM : kAimArriveMarginM, browserOn,
                          browserHands);
                }
                if (browserAimable && dock.keyboardOpen() && vr.auxWorld(VrOverlay::Aux::Keyboard, world)) {
                    aimAt(world, vr.auxWidthM(VrOverlay::Aux::Keyboard), vr.auxHeightM(VrOverlay::Aux::Keyboard),
                          dock.interactive(DockTarget::Keyboard, t) ? kAimLeaveMarginM : kAimArriveMarginM, keyboardOn,
                          browserHands);
                }
                dock.updateAim(hudOn, dockOn, browserOn, keyboardOn, t);
                // The browser panel in view (facing the user, in the field of view): full frame rate; out of view
                // for kOutOfViewSec, Chrome sends about 2 frames a second. When the check can't run (no HMD pose)
                // a running countdown is forgotten: nothing would move its deadline, and the loop would spin on it
                if (browserAimable && poses.hmd.valid && vr.auxWorld(VrOverlay::Aux::Browser, world)) {
                    const bool seen = browserloop::panelInView(world, vr.auxWidthM(VrOverlay::Aux::Browser),
                                                               vr.auxHeightM(VrOverlay::Aux::Browser), poses.hmd.pose);
                    if (browserInView.update(seen, t) && browserWorker) {
                        const bool inView = browserInView.inView();
                        browserWorker->post([inView](BrowserSession& s, double tt) { s.setInView(inView, tt); });
                    }
                } else {
                    browserInView.pause();
                }
                // A ray dwelling on a panel (not taking the laser yet) is checked again soon, and at the dwell's end
                const double dwellEnds = dock.dwellEndsAt();
                const bool onBrowser = dock.interactive(DockTarget::Browser, t) || dock.interactive(DockTarget::Keyboard, t);
                const bool busy = dock.expanded() || dock.grabbing() != DockTarget::None ||
                                  dock.interactive(DockTarget::Hud, t) || dock.interactive(DockTarget::Dock, t) ||
                                  onBrowser || std::isfinite(dwellEnds);
                nextAim = t + (onBrowser ? kBrowserActivePollSec : busy ? kDockActivePollSec : kDockIdlePollSec);
                if (dwellEnds > t) nextAim = std::min(nextAim, dwellEnds);
                // On a wrist the fade reads the poses every kDockIdlePollSec anyway: check right after it, on its poses
                if (!busy && vr.nextPlacementAt() <= nextAim + kPlacementCoalesceSec) {
                    nextAim = std::max(t + kDockActivePollSec, vr.nextPlacementAt());
                }
                // An out-of-view countdown ending sooner: the next check (which updates it) runs right then
                nextAim = browserloop::alignAimWithInView(nextAim, browserInView, t);
            } else if (!dockLive && !browserAimable) {
                dock.updateAim(false, false, false, false, t);
                browserInView.pause();  // headset off or the panel closed: no in-view checks, no countdown
            }
            // The grip (SteamVR Input), read only while a laser is on a panel or one is carried
            const bool gripWatch = (dockLive || browserAimable) && config.dock.gripInput && vr.gripAvailable() &&
                                   (dock.grabbing() != DockTarget::None || dock.interactive(DockTarget::Hud, t) ||
                                    dock.interactive(DockTarget::Dock, t) || dock.interactive(DockTarget::Browser, t) ||
                                    dock.interactive(DockTarget::Keyboard, t));
            // A grip already held when the laser arrived (a game's "hold the object") is no grab: only a press that
            // starts while pointing counts. Not readable any more while down (the watch ended, SteamVR made the action
            // inactive) is no release: a grip grab is cancelled, not kept wherever the hand is
            bool grip = false;
            const bool gripRead =
                gripWatch && vr.readGrip(dock.grabbing() != DockTarget::None ? grabLeft : aimLeft, grip);
            switch (gripEdges.update(gripRead, grip)) {
                case dockloop::GripEdges::Change::Pressed: dock.grip(true, t); break;
                case dockloop::GripEdges::Change::Released: dock.grip(false, t); break;
                case dockloop::GripEdges::Change::Lost: dock.gripLost(t); break;
                case dockloop::GripEdges::Change::None: break;
            }
            // The laser on the HUD: a click opens / collapses the Dock, a long press carries the HUD. Not while the Dock
            // isn't live (HUD hidden, headset off): setLive collapsed it, and a late event must not open it again
            if (!dockLive) {
                events.hudPointer.clear();
                events.dockPointer.clear();
                releaseControl();
            }
            if (!events.dockPointer.empty()) colorModel.pointerSeen(std::chrono::steady_clock::now());
            // The video controls under the HUD's own picture take every press on them: their buttons act, the rest of
            // the strip (its title line, the gaps) does nothing, so a laser slightly off a button never opens the Dock
            // A press is the HUD's or the strip's by where it started. Simple B's clear sides beside the HUD's own
            // picture (over the wider strip) take nothing either
            if (!events.hudPointer.empty() && ctlHeldType != frame_perf_overlay::WidgetType::None) {
                colorModel.pointerSeen(std::chrono::steady_clock::now());  // a held 비교 on the HUD
            }
            for (const PointerInput& input : events.hudPointer) {
                const bool onStrip = hudStripSent && input.y >= hudGridHeight;
                const bool offGrid = !onStrip && hudStripSent &&
                                     (input.x < hudGridLeft || input.x >= hudGridLeft + hudGridWidth);
                const double sx = input.x, sy = input.y - hudGridHeight;
                switch (input.type) {
                    case PointerInput::Type::Down:
                        if (onStrip) {
                            mediaDirty |= mediaStrip.pointerDown(sx, sy);
                            dock.hudActivity(t);
                        } else {
                            mediaDirty |= mediaStrip.pointerLeave();
                            if (offGrid) {
                                dock.hudActivity(t);
                            } else {
                                // A control's button acts; its row's gaps do nothing; anything else is the HUD's
                                // click (the Dock) and grab, as before
                                const int control = pressControl(input.x, input.y, t);
                                if (control > 0) {
                                    // A held 비교 checks this controller's trigger (as on the Dock panel)
                                    pressDevice = input.device != 0xffffffff ? input.device : aimDevice;
                                }
                                if (control >= 0) {
                                    dock.hudActivity(t);
                                    if (control > 0) nextPanel = std::min(nextPanel, t);  // show it pressed now
                                } else {
                                    dock.hudDown(t);
                                }
                            }
                        }
                        break;
                    case PointerInput::Type::Up:
                        if (releaseControl()) {
                            dock.hudActivity(t);
                            nextPanel = std::min(nextPanel, t);
                        } else if (mediaStrip.pressing()) {
                            mediaPressed(mediaStrip.pointerUp(sx, sy), t);
                            mediaDirty = true;
                            dock.hudActivity(t);
                        } else if ((onStrip || offGrid) && dock.grabbing() != DockTarget::Hud) {
                            // Off the HUD's own picture: a press of the HUD let go here is no click, and no grab
                            // later either (a carried HUD is let go by its release anywhere, below)
                            dock.hudActivity(t);
                            dock.hudLeave(t);
                        } else {
                            dock.hudUp(t);
                        }
                        break;
                    case PointerInput::Type::Move:
                        if (hudStripSent) mediaDirty |= mediaStrip.pointerMove(sx, onStrip ? sy : -1);
                        ctlRepeat.touch(t);
                        dock.hudActivity(t);
                        break;
                    case PointerInput::Type::Leave:
                        mediaDirty |= mediaStrip.pointerLeave();
                        if (releaseControl()) nextPanel = std::min(nextPanel, t);
                        dock.hudLeave(t);
                        break;
                    case PointerInput::Type::Scroll: break;
                }
            }
            // At once (not at the 2 Hz tick) unless the last send failed: then only at the tick, not every wake
            if (mediaDirty && (mediaWanted || hudStripSent) && !hudSendFailed) nextPanel = std::min(nextPanel, t);
            // The laser on the Dock panel
            for (const PointerInput& input : events.dockPointer) {
                if (!dock.expanded()) break;
                syncDockView(t);  // the hit areas belong to the view showing now
                dockSettings.pointerSeen(t);
                dockView.pointerSeen(t);
                // The pressing controller (the event's own, or the hand the aim check found on the panel)
                if (input.type == PointerInput::Type::Down) {
                    pressDevice = input.device != 0xffffffff ? input.device : aimDevice;
                }
                if (dock.view() == DockView::Quick) {
                    switch (input.type) {
                        case PointerInput::Type::Move:
                            dockDirty |= dockView.pointerMove(input.x, input.y);
                            dock.dockActivity(t);
                            break;
                        case PointerInput::Type::Down:
                            dock.dockDown(t);
                            dockView.setProfiles(profilesState());
                            handleDockPress(dockView.pointerDown(input.x, input.y, t), t);
                            break;
                        case PointerInput::Type::Up:
                            dockDirty |= dockView.pointerUp(input.x, input.y);
                            dock.dockUp(t);
                            break;
                        case PointerInput::Type::Leave:
                            dockDirty |= dockView.pointerLeave();
                            dock.dockLeave(t);
                            break;
                        case PointerInput::Type::Scroll: dock.dockActivity(t); break;
                    }
                    continue;
                }
                // The full view: the bar on top, the pages under it (their y starts below the bar)
                PointerInput shifted = input;
                shifted.y -= DockPanel::kBarHeight;
                const bool onBar = input.y < DockPanel::kBarHeight;
                switch (input.type) {
                    case PointerInput::Type::Move:
                        dockDirty |= dockView.barMove(onBar ? input.x : -1, onBar ? input.y : -1);
                        handlePanelPointer(dockSettings, shifted, true);  // off its buttons on the bar
                        dock.dockActivity(t);
                        break;
                    case PointerInput::Type::Down:
                        dock.dockDown(t);
                        if (onBar) {
                            handleDockPress(dockView.barDown(input.x, input.y), t);
                        } else {
                            dockSettings.syncLayout(config.layout);
                            handlePanelPointer(dockSettings, shifted, true);
                        }
                        break;
                    case PointerInput::Type::Up:
                        dockDirty |= dockView.barUp();
                        handlePanelPointer(dockSettings, shifted, true);
                        dock.dockUp(t);
                        break;
                    case PointerInput::Type::Leave:
                        dockDirty |= dockView.pointerLeave();
                        handlePanelPointer(dockSettings, shifted, true);
                        dock.dockLeave(t);
                        break;
                    case PointerInput::Type::Scroll:
                        handlePanelPointer(dockSettings, shifted, true);
                        dock.dockActivity(t);
                        break;
                }
            }
            // The laser on the browser panel and on its keyboard
            const auto panelEvent = [&](DockTarget target, const PointerInput& input) {
                if (input.secondary) {  // the right button: activity, not a press the state machine holds on to
                    dock.panelActivity(target, t);
                    return;
                }
                switch (input.type) {
                    case PointerInput::Type::Down: dock.panelDown(target, t); break;
                    case PointerInput::Type::Up: dock.panelUp(target, t); break;
                    case PointerInput::Type::Leave: dock.panelLeave(target, t); break;
                    case PointerInput::Type::Move:
                    case PointerInput::Type::Scroll: dock.panelActivity(target, t); break;
                }
            };
            // the drag distance is a size on the panel
            browserCtl.setPanelWidthM(
                browserloop::panelWidthM(config.browser, browserPanel.width(), browserPanel.height()));
            for (const PointerInput& input : events.browserPointer) {
                if (!dock.browserOpen()) break;
                // The controller first (a press on the handle asks for the grab), then the state machine (a release ends
                // a trigger grab)
                if (input.type == PointerInput::Type::Down && !input.secondary) browserPressDevice = input.device;
                // A press on the page itself while worn (not the bar, its handle or ✕): what the video does from then
                // on is theirs (no 벗어서 멈춤)
                if (input.type == PointerInput::Type::Down && hmdInUse(activityLevel) &&
                    browserPanel.hitAt(input.x, input.y) == BrowserPanel::Hit::Page) {
                    mediaPausedByRemoval = false;
                }
                handleBrowserRequests(browserCtl.panelPointer(toPanelPointer(input), t), DockTarget::Browser, t);
                panelEvent(DockTarget::Browser, input);
            }
            for (const PointerInput& input : events.keyboardPointer) {
                if (!dock.keyboardOpen()) break;
                if (input.type == PointerInput::Type::Down) browserPressDevice = input.device;
                handleBrowserRequests(browserCtl.keyboardPointer(toPanelPointer(input), t), DockTarget::Keyboard, t);
                panelEvent(DockTarget::Keyboard, input);
            }
            // A held key's repeat; a long press on the page (its ring)
            if (dock.browserOpen()) handleBrowserRequests(browserCtl.tick(t), DockTarget::Keyboard, t);
            dock.tick(t);
            for (const DockController::Event& e : dock.takeEvents()) {
                switch (e.type) {
                    case DockController::Event::Type::Opened:
                        std::fprintf(stderr, "[dock] Opened (%s view)\n", dock.view() == DockView::Full ? "full" : "quick");
                        if (!config.firstRunGuideDone) {
                            // The HUD was pressed: the first-run guide has done its job (it goes with the next wake)
                            config.firstRunGuideDone = true;
                            saveConfigNow(config, watcher);
                        }
                        dockView.pointerLeave();
                        dockSettings.pointerLeave();
                        placeDockAttached(t);
                        dockDirty = true;
                        break;
                    case DockController::Event::Type::ViewChanged:
                        // The pointers let go and the new view was drawn when it switched (syncDockView)
                        dockDirty = true;
                        break;
                    case DockController::Event::Type::Collapsed:
                        std::fprintf(stderr, "[dock] Collapsed (%s)\n", collapseReasonName(e.reason));
                        vr.showDock(false);
                        dockView.pointerLeave();
                        dockSettings.pointerLeave();
                        break;
                    case DockController::Event::Type::AdjustStarted: {
                        // Frozen where it is in the room, so the buttons stay under the laser while the HUD hand moves
                        vr.readDockPoses(t);
                        dockmath::Mat34 world {};
                        if (vr.dockWorld(world)) vr.placeDockAbsolute(world);
                        std::fprintf(stderr, "[dock] Fine adjust: the Dock panel stays put until 완료\n");
                        dockDirty = true;
                        break;
                    }
                    case DockController::Event::Type::AdjustEnded:
                        placeDockAttached(t);
                        dockDirty = true;
                        break;
                    case DockController::Event::Type::GrabStarted: beginGrab(e.target, t); break;
                    case DockController::Event::Type::GrabEnded: finishGrab(e.target, true, t); break;
                    case DockController::Event::Type::GrabCancelled: finishGrab(e.target, false, t); break;
                    case DockController::Event::Type::Leave:
                        // Pointed away: what its pointer held lets go (a held button, compare, an editor drag, a
                        // press on the page, a held key)
                        if (e.target == DockTarget::Browser) {
                            browserCtl.releasePage();
                        } else if (e.target == DockTarget::Keyboard) {
                            PanelPointer leave;
                            leave.type = PanelPointer::Type::Leave;
                            browserCtl.keyboardPointer(leave, t);
                        } else {
                            // The HUD (also when it stopped taking the laser, e.g. the wrist turned away, with no
                            // FocusLeave of its own): the strip's hover and held button let go
                            if (e.target == DockTarget::Hud) mediaDirty |= mediaStrip.pointerLeave();
                            dockDirty |= dockloop::applyLeave(e, dockView, dockSettings);
                        }
                        break;
                    case DockController::Event::Type::BrowserOpened: {
                        if (!browserWorker) {
                            std::fprintf(stderr, "[browser] Starting Chrome's thread (profile %s/profile)\n",
                                         browserDir.c_str());
                            browserWorker = std::make_unique<BrowserWorker>(
                                browserloop::sessionOptions(config.browser, browserDir), gWakeFd);
                        }
                        browserWorker->post([](BrowserSession& s, double tt) { s.open(tt); });  // shown below
                        std::fprintf(stderr, "[browser] Opened\n");
                        browserPinned = false;
                        browserPlaced = true;
                        browserInView = browserloop::InViewTimer {};  // opened: in view (until seen otherwise)
                        browserWorker->post([](BrowserSession& s, double tt) { s.setInView(true, tt); });
                        browserCtl.setStatus(browserWorker->status());
                        browserBarStale = browserDirty = true;
                        placeBrowserAttached(t);
                        break;
                    }
                    case DockController::Event::Type::BrowserClosed:
                        std::fprintf(stderr, "[browser] Closed (Chrome %s)\n",
                                     config.browser.keepAudio ? "keeps playing" : "pauses its media");
                        vr.showAux(VrOverlay::Aux::Browser, false);
                        vr.showAux(VrOverlay::Aux::Keyboard, false);
                        browserCtl.releasePage();
                        browserCtl.closePopups();
                        if (browserCtl.keyboardOpen()) browserCtl.setKeyboardOpen(false, false);
                        break;  // Chrome hears it below (hidden)
                    case DockController::Event::Type::KeyboardOpened:
                        keyboardDirty = true;
                        placeBrowserAttached(t);
                        break;
                    case DockController::Event::Type::KeyboardClosed:
                        vr.showAux(VrOverlay::Aux::Keyboard, false);
                        if (browserCtl.keyboardOpen()) browserCtl.setKeyboardOpen(false, false);
                        break;
                }
            }
            if (dock.expanded()) placeDockAttached(t);  // follows a new attachment (a profile, a reload)
            if (dock.browserOpen()) placeBrowserAttached(t);  // the same for the browser panel (auto = the HUD's wrist)
            // Chrome works for a panel someone sees: closed, the headset off or no anchor to hang on, it is hidden
            // (screencast off, media paused per keep_audio, the idle shutdown counting); seen again, it goes on
            if (browserVisibility.update(browserloop::panelShowing(dock.browserOpen(), browserLive, browserPlaced))) {
                const bool showing = browserVisibility.showing();
                std::fprintf(stderr, "[browser] Panel %s\n",
                             showing                ? "seen: Chrome shows the page"
                             : !dock.browserOpen() ? "closed: Chrome hidden"
                             : !browserLive        ? "out of sight (headset off): Chrome hidden"
                                                   : "off its anchor: Chrome hidden");
                if (browserWorker) {
                    browserWorker->post([showing](BrowserSession& s, double tt) { s.setVisible(showing, tt); });
                }
                if (showing) {
                    browserBarStale = browserDirty = keyboardDirty = true;  // what changed while hidden goes up now
                } else if (dock.browserOpen()) {
                    browserCtl.releasePage();  // a press on the page doesn't stay held while nobody sees it
                }
            }
            // SteamVR's laser only while a hand points at a panel: the game has the controller back otherwise. The
            // HUD takes laser input only then too, so the dashboard's laser still passes through it as before
            const bool hudTakes = dockLive && dock.interactive(DockTarget::Hud, t);
            vr.setHudInput(hudTakes);
            vr.setInteractive(DockTarget::Hud, hudTakes);
            vr.setInteractive(DockTarget::Dock, dockLive && dock.expanded() && dock.interactive(DockTarget::Dock, t));
            vr.setInteractive(DockTarget::Browser,
                              browserLive && dock.browserOpen() && dock.interactive(DockTarget::Browser, t));
            vr.setInteractive(DockTarget::Keyboard,
                              browserLive && dock.keyboardOpen() && dock.interactive(DockTarget::Keyboard, t));
            if (renderer.setHighlight(dockLive && dock.hudHighlighted(t))) {
                nextPanel = std::min(nextPanel, t);
                mediaDirty = true;  // the video controls' border goes with the HUD's
            }
            // Hold to repeat in the full view (as on the tab); a Leave above already stopped it if pointed away
            if (dock.view() == DockView::Full) {
                const SettingsAction held =
                    dockSettings.takeRepeat(t, [&] { return dock.interactive(DockTarget::Dock, nowSeconds()); });
                if (held != SettingsAction::None) {
                    handleSettingsAction(held, config, watcher, vr, heldSave, true, t);
                    settingsDirty = dockDirty = true;
                }
            }
        }
        if (userQuit) break;
        // The HUD's − / + held: again when due (it stops on the release, the laser leaving, the HUD going). A due
        // repeat first checks the HUD still has the laser: a lost release doesn't run the volume to 0 or 100
        {
            const double t = nowSeconds();
            if (ctlHeldType != frame_perf_overlay::WidgetType::None && !dock.interactive(DockTarget::Hud, t)) {
                releaseControl();
                nextPanel = std::min(nextPanel, t);
            }
            if (ctlRepeat.due(t)) {
                stepVolume(ctlHeldButton == 1 ? -1 : +1);
                nextPanel = std::min(nextPanel, t);
            }
            if (!ctlArmedProfile.empty() && t > ctlArmedUntil) {
                ctlArmedProfile.clear();  // "한 번 더 누르면" goes
                nextPanel = std::min(nextPanel, t);
            }
        }
        // Hold to repeat: the held button acts again when due. The file is written when the button is let go
        // (release, leave, page switch, tab hidden) or every kHeldSaveSec while it's held. Safety: a repeat that is
        // due checks the tab is still visible (not only on the 2 Hz tick), and none comes 10 s after the last
        // pointer event (HoldRepeat::kIdleStopSec)
        if (settingsVisible) {
            const double t = nowSeconds();
            const bool wasRepeating = settings.repeating();
            const SettingsAction held = settings.takeRepeat(
                t, [&] { return options.forceSettingsVisible || vr.settingsVisibility().visible(); });
            if (wasRepeating && !settings.repeating() && held == SettingsAction::None) {
                std::fprintf(stderr, "[dashboard] Held button stopped (tab hidden or no pointer event for %.0f s)\n",
                             HoldRepeat::kIdleStopSec);
                settingsDirty = true;
            }
            if (held != SettingsAction::None) {
                handleSettingsAction(held, config, watcher, vr, heldSave, true, t);
                settingsDirty = dockDirty = true;
            }
        }
        if (heldSave.pending && ((!settings.repeating() && !dockSettings.repeating()) || nowSeconds() >= heldSave.dueAt)) {
            saveConfigNow(config, watcher);
            heldSave.clear();
        }
        if (dockSave.pending && nowSeconds() >= dockSave.dueAt) {  // a grab's new place
            saveConfigNow(config, watcher);
            dockSave.clear();
            heldSave.clear();
        }

        if (metricsTick) {
            settingsDirty |= settings.tick(loopStart);  // "press again to quit" expiry
            if (autostart.poll()) settingsDirty = dockDirty = true;  // reap systemctl if it finished
            // Whether the tab is actually visible (the dashboard being open isn't enough)
            const SettingsVisibility visibility = vr.settingsVisibility();
            const bool nowVisible = visibility.visible() || options.forceSettingsVisible;
            dashboardOpen = visibility.dashboardVisible;
            vr.noteDashboardOpen(dashboardOpen, loopStart);
            // GPU memory of panels closed for a while (they get a new texture when they open again)
            vr.releaseIdleTextures(loopStart);
            if (nowVisible != settingsVisible) {
                std::fprintf(stderr, "[dashboard] Tab %s (dashboard %s, tab %s, overlay %s%s)\n",
                             nowVisible ? "visible: polling the pointer every 100 ms (50 ms while active)" : "hidden",
                             visibility.dashboardVisible ? "open" : "closed",
                             visibility.activeTab ? "selected" : "not selected",
                             visibility.overlayVisible ? "visible" : "hidden",
                             options.forceSettingsVisible ? ", --force-settings-visible" : "");
            }
            if (settingsVisible && !nowVisible) settingsDirty |= settings.pointerLeave();  // cancel a drag or a hold
            settingsVisible = nowVisible;
            // The Color page's display mode check (a /proc scan) runs only while someone can see the page, or a rule
            // needs it; shown again, its "Now:" line is brought up to date at once
            colorModel.setPageVisible((settingsVisible && settings.page() == SettingsPage::Color) ||
                                          (dock.view() == DockView::Full && dockSettings.page() == SettingsPage::Color),
                                      std::chrono::steady_clock::now());
            if (!visibility.dashboardVisible) scrollEventsLogged = 0;
            // The Profiles page ("unsaved changes" after any edit)
            if (refreshProfilesUi()) settingsDirty = dockDirty = true;

            // "Someone looking": recomputes the reference counts whenever a signal changes
            const int level = vr.hmdActivityLevel();
            if (level != activityLevel) {
                std::fprintf(stderr, "[demand] HMD activity level %d (%s)\n", level, activityName(level));
                // Worn again: the placement check skipped while it was off runs now, not at a deadline long past
                if (hmdInUse(level) && !hmdInUse(activityLevel)) vr.updatePlacement(config, nowSeconds());
                // Taken off: the page's video pauses (벗으면 영상: 멈춤) and Chrome isn't closed for idleness while off,
                // so it is still there to resume. Worn again it stays paused (the HUD's ▶); the idle time starts then
                if (!hmdInUse(level) && hmdInUse(activityLevel) && config.browser.pauseWhenRemoved && browserWorker) {
                    std::fprintf(stderr, "[browser] Headset off: pausing the page's video\n");
                    browserWorker->post([](BrowserSession& s, double tt) {
                        s.media(BrowserSession::MediaCommand::Pause);
                        s.holdIdle(true, tt);
                    });
                    // Shown if it was playing, or still from the last time it came off (worn for a moment in between);
                    // the pause's answer also tells (Status::pausedPlaying: the panel was open, the state not known)
                    mediaPausedByRemoval = mediaPausedByRemoval || browserMedia.playing;
                    mediaSeekAt = -1;
                    mediaPosition.clear();
                    mediaPositionUntil = -1;
                    mediaDirty = true;
                }
                if (hmdInUse(level) && !hmdInUse(activityLevel) && browserWorker) {
                    browserWorker->post([](BrowserSession& s, double tt) { s.holdIdle(false, tt); });
                    mediaActive();  // the paused strip shows its kMediaHideSec again
                }
                activityLevel = level;
            }
            // The battery (its own reads, while worn): the next reading arrives through the sampler; the controllers
            // are read here (OpenVR)
            if (hmdInUse(level) && loopStart >= nextBatteryRead) {
                nextBatteryRead = loopStart + BatteryWatch::kReadSec;
                sampler.request(batteryKey);
                const ControllerStatus c = vr.readControllers();
                batteryWatch.feedControllers({c.left.present, c.left.pct, c.left.charging},
                                             {c.right.present, c.right.pct, c.right.charging});
            }
            if (dock.view() == DockView::Quick) {
                DockBattery row;
                const BatteryReading& h = batteryWatch.headset();
                row.headsetPct = h.pct;
                // Not on battery: what the HUD says too (충전 중, 충전 완료, 전원 연결 / 대기); a status not read: nothing
                if (!std::isnan(h.pct) && !h.discharging) {
                    row.headsetCharging = h.status == "Charging";
                    const frame_perf_overlay::WidgetText& wt =
                        frame_perf_overlay::widgetText(widgetLanguage(uiLanguage()));
                    const char* plugged = h.chargerOnline ? wt.statusPlugged : wt.statusIdle;
                    row.headsetState = h.status == "Charging"       ? wt.statusCharging
                                       : h.status == "Full"         ? wt.simpleFull
                                       : h.status == "Not charging" ? plugged
                                                                    : "";
                }
                row.minutesText = minutesText(batteryWatch.minutesLeft());
                row.left = {batteryWatch.left().present, batteryWatch.left().pct, batteryWatch.left().charging};
                row.right = {batteryWatch.right().present, batteryWatch.right().pct, batteryWatch.right().charging};
                row.warnPct = config.thresholds.batteryWarnPct;
                row.critPct = config.thresholds.batteryCritPct;
                row.controllerWarnPct = config.thresholds.controllerWarnPct;
                row.controllerCritPct = config.thresholds.controllerCritPct;
                dockDirty |= dockView.setBattery(row);
            }
            // Shown = what the overlay really shows: off when hidden, and also while the wrist fade or lost tracking
            // hides it (placement keeps running, so it shows again by itself)
            const bool panelShown = vr.panelShown();
            frame_perf_overlay::DemandInputs demand;
            demand.hudShown = panelShown;
            demand.hmdInUse = hmdInUse(level);
            // Redraws start again (shown again, or the headset back from standby): fresh pixels now, not at the old
            // deadline
            const bool drawing = demand.hudShown && demand.hmdInUse;
            if (drawing && !(scheduler.demand().hudShown && scheduler.demand().hmdInUse)) {
                nextPanel = std::min(nextPanel, loopStart);
            }
            demand.editorOpen = settingsVisible || dock.view() == DockView::Full;  // the tab's pages, either place
            if (someoneLooking(demand) != someoneLooking(scheduler.demand())) {
                std::fprintf(stderr, "[demand] %s\n", someoneLooking(demand)
                                                          ? "Someone is looking: reading sources"
                                                          : "Nobody is looking: all sources paused");
            }
            scheduler.setDemand(demand);
        }
        // The language changed (the header's button, a hand-edited config.json): everything shows it from now on
        if (config.language != uiLanguage()) {
            std::fprintf(stderr, "[language] %s%s\n", languageCode(config.language),
                         config.languageAuto ? " (auto)" : "");
            setUiLanguage(config.language);
            renderer.invalidateStatic();
            nextPanel = std::min(nextPanel, nowSeconds());
            settingsDirty = dockDirty = mediaDirty = guideStale = true;
            browserBarStale = browserDirty = keyboardDirty = true;
            // The keyboard's letters follow it while nothing is typed with them
            if (!dock.keyboardOpen()) {
                keyboard.setLayout(config.language == Language::Ko ? VrKeyboard::Layout::Hangul
                                                                   : VrKeyboard::Layout::English);
            }
        }
        // The Color page's own timing, also while the tab is hidden: current.txt while a slider moves, auto
        // compare turns, the display mode and scene app once a second (auto presets)
        colorModel.state.language = colorLanguage(config.language);
        // Work due within kCoalesceSec is done now, in this wake, rather than in a wake of its own
        const auto colorNow = std::chrono::steady_clock::now();
        if (colorModel.tick(colorNow + std::chrono::milliseconds(static_cast<int>(kCoalesceSec * 1000)))) {
            settingsDirty = dockDirty = true;
        }
        if (gazeFeed) {
            gazeFeed->setEnabled(colorModel.gazeFollowActive());
            if (colorModel.state.gazeFitPressed) {
                colorModel.state.gazeFitPressed = false;
                gazeFeed->toggleFit();
            }
            // The fit's progress on the page; an outcome shows for a while (a fit done: as long as its tint stays, a
            // failure long enough to read why)
            const auto fit = gazeFeed->fitStatus();
            const int outcome = static_cast<int>(fit.outcome);
            if (outcome != 0 && outcome != colorModel.state.gazeFitOutcome) gazeFitOutcomeSince = colorNow;
            const auto outcomeShown = std::chrono::seconds(fit.outcome == framecolor::GazeFeed::FitOutcome::Failed ? 12 : 6);
            if (outcome != 0 && !fit.reviewing && colorNow - gazeFitOutcomeSince > outcomeShown) gazeFeed->clearFitOutcome();
            const int failure = static_cast<int>(fit.failure);
            if (fit.step != colorModel.state.gazeFitStep || fit.steps != colorModel.state.gazeFitSteps ||
                outcome != colorModel.state.gazeFitOutcome || fit.reviewing != colorModel.state.gazeFitReviewing ||
                fit.reviewLeft != colorModel.state.gazeFitReviewLeft || failure != colorModel.state.gazeFitFailure) {
                colorModel.state.gazeFitReviewing = fit.reviewing;
                colorModel.state.gazeFitReviewLeft = fit.reviewLeft;
                colorModel.state.gazeFitFailure = failure;
                colorModel.state.gazeFitStep = fit.step;
                colorModel.state.gazeFitSteps = fit.steps;
                colorModel.state.gazeFitOutcome = outcome;
                settingsDirty = true;
            }
            // While a fit runs or an outcome shows, wake often enough to follow it
            gazeFitBusy = fit.step >= 0 || outcome != 0 || fit.reviewing;
        }

        // Dashboard tab: draw only while visible and only when something changed
        if (metricsTick && settingsVisible) {
            // Re-read the autostart state when the tab appears and every few seconds (catches outside changes)
            const double t = nowSeconds();
            if (!settingsWasVisible || t - lastAutostartRefresh >= kAutostartRefreshSec) {
                lastAutostartRefresh = t;
                if (autostart.refresh()) {
                    const AutostartStatus::State autostartState = autostart.status().state;
                    std::fprintf(stderr, "[autostart] State changed: %s\n",
                                 autostartState == AutostartStatus::State::Enabled
                                     ? "enabled"
                                     : (autostartState == AutostartStatus::State::Disabled ? "disabled"
                                                                                           : "not installed"));
                    settingsDirty = true;
                }
            }
        }
        // The 기기 page's backends: what is shown (sound and passthrough also on the quick view; the PC's account only
        // on the page), the controls' actions, the services' news. A change in either panel redraws both
        {
            const bool dockSeen = hmdInUse(activityLevel);  // the Dock panel counts only while the headset is worn
            const bool devicePage =
                (settingsVisible && settings.page() == SettingsPage::Device) ||
                (dockSeen && dock.view() == DockView::Full && dockSettings.page() == SettingsPage::Device);
            const double t = nowSeconds();
            // ...and while the HUD shows a 소리 or 패스스루 control (its values, its buttons)
            const bool hudControls = dockSeen && scheduler.demand().hudShown &&
                                     std::any_of(renderer.geometry().slots.begin(), renderer.geometry().slots.end(),
                                                 [](const frame_perf_overlay::WidgetSlot& s) {
                                                     return s.widget.type == frame_perf_overlay::WidgetType::Volume ||
                                                            s.widget.type == frame_perf_overlay::WidgetType::Passthrough;
                                                 });
            deviceBackend->setVisible(devicePage || (dockSeen && dock.view() == DockView::Quick) || hudControls,
                                      devicePage, t);
            deviceBackend->tick(t);
        }
        if (deviceModel.revision() != deviceRevision) {
            deviceRevision = deviceModel.revision();
            settingsDirty = dockDirty = true;
        }
        // 녹화: the recorder's end (asked for, or by itself), its clock; the HUD's card follows at its next redraw
        {
            const double t = nowSeconds();
            if (recorder.tick(t)) nextPanel = std::min(nextPanel, t);
            DockRecord record;
            record.available = recorder.available();
            record.state = static_cast<int>(recorder.state());
            record.seconds = recorder.elapsedSec(t);
            if (dockView.setRecord(record)) dockDirty = true;
        }
        // The Dock panel: drawn only while open and only when something changed (what both panels show sets both
        // flags: profiles, the colour model, a reload, autostart)
        if (dock.expanded()) {
            const double t = nowSeconds();
            dockDirty |= dockView.tick(t);
            if (metricsTick) dockDirty |= dockSettings.tick(t);
            if (dock.view() == DockView::Full) dockDirty |= dockSettings.syncLayout(config.layout);
            if (dockDirty) {
                dockloop::renderDock(dock.view(), dock.adjusting(), dockView, dockSettings, config, autostart.status(),
                                     profilesUi, t);
                dockHitView = dock.view();
                // The same pixel size in both views: the full view is wider in proportion
                const double widthM = config.dock.widthM * dockView.width() / DockPanel::kQuickWidth;
                const bool sent = vr.submitDock(dockView.pixels(), dockView.width(), dockView.height(), widthM);
                if (sent && dockPlaced) vr.showDock(true);  // not before it is on its anchor
                dockDirty = false;
            }
        }
        // The browser panel: Chrome's status and frames (its thread wakes us when either changed), settings changed
        // on the Browser page or in the file, then what changed goes up
        if (config.browser != appliedBrowser) {
            int newW = 0, newH = 0, oldW = 0, oldH = 0;
            config.browser.pageSize(newW, newH);
            appliedBrowser.pageSize(oldW, oldH);
            const bool viewport = newW != oldW || newH != oldH;
            if (browserWorker) {
                browserWorker->post([o = browserloop::sessionOptions(config.browser, browserDir)](BrowserSession& s,
                                                                                                    double tt) {
                    s.setOptions(o, tt);
                });
            }
            browserCtl.setView(config.browser.mobile, config.browser.zoomPct, static_cast<int>(config.browser.aspect));
            if (viewport) {
                browserCtl.setViewport(newW, newH);
                std::fprintf(stderr, "[browser] Page size %dx%d (%s)\n", newW, newH,
                             browserAspectName(config.browser.aspect));
            }
            appliedBrowser = config.browser;
            browserBarStale = browserDirty = keyboardDirty = true;
            placeBrowserAttached(nowSeconds());
        }
        if (browserWorker) {
            if (browserWorker->statusVersion() != browserStatusSeen) {
                browserStatusSeen = browserWorker->statusVersion();
                const BrowserSession::Status st = browserWorker->status();
                browserCtl.setStatus(st);
                BrowserPageInfo info;
                info.url = st.url;
                info.status = browserloop::statusLine(st, browserWorker->pid());
                info.running = st.phase != BrowserSession::Phase::Off && st.phase != BrowserSession::Phase::Failed;
                chromeRunning = info.running;
                if (settings.setBrowserInfo(info)) settingsDirty = true;
                if (dockSettings.setBrowserInfo(info)) dockDirty = true;
                if (dockView.setChromeRunning(info.running)) dockDirty = true;  // 끄기 beside 브라우저
                if (!info.running) releasePixels(browserFrame);  // Chrome stopped: its last frame's memory goes
                browserMedia = st.media;
                browserTitle = st.title;
                // Playing again while worn (from here or the panel). Not while off: the page started it by itself, and
                // the session pauses it again
                if (st.media.playing && hmdInUse(activityLevel)) mediaPausedByRemoval = false;
                // Pause everything stopped something while the headset is off (taken off, or the page started again)
                if (st.pauseAnswers != mediaPauseAnswersSeen) {
                    mediaPauseAnswersSeen = st.pauseAnswers;
                    if (st.pausedPlaying && !hmdInUse(activityLevel)) {
                        mediaPausedByRemoval = true;
                        mediaDirty = true;
                    }
                }
                // A −10초 / +10초's answer: its position shows for a moment
                if (st.media.answer != mediaAnswerSeen) {
                    mediaAnswerSeen = st.media.answer;
                    const double t = nowSeconds();
                    if (mediaSeekAt >= 0 && t - mediaSeekAt < kMediaSeekAnswerSec) {
                        mediaPosition = mediaPositionText(st.media.position, st.media.duration);
                        mediaPositionUntil = mediaPosition.empty() ? -1 : t + kMediaPositionSec;
                    }
                }
            }
            if (browserWorker->takeFrame(browserFrame) && dock.browserOpen()) {
                browserPanel.showFrame(browserFrame);
                browserDirty = true;
            }
        } else {
            browserMedia = BrowserSession::Media {};
        }
        // The HUD's video controls: a video (or audio) playing or paused on the page, its panel closed, the
        // Dock's laser on (in-game interaction) and the HUD drawn from cairo's pixels as they are
        // A paused one goes after kMediaHideSec untouched in view (the browser panel still has it)
        {
            const double t = nowSeconds();
            const bool open = dock.browserOpen();
            // Opened after it was worn again: what it shows from then on is theirs, not the removal's
            if (open && !mediaBrowserWasOpen && hmdInUse(activityLevel)) mediaPausedByRemoval = false;
            if (!open && mediaBrowserWasOpen) mediaActive();
            mediaBrowserWasOpen = open;
            const bool inView = scheduler.demand().hudShown && hmdInUse(activityLevel);
            const bool pointed = dock.interactive(DockTarget::Hud, t);
            if (mediaQuietTickAt >= 0 && mediaWanted && inView && !pointed) mediaQuietSec += t - mediaQuietTickAt;
            mediaQuietTickAt = t;
            if (browserMedia.playing || !browserMedia.present) mediaActive();
            // Not while someone looks at it or points at it: it goes when the HUD next leaves the view
            if (mediaQuietSec >= kMediaHideSec && !inView && !pointed) mediaQuietHidden = true;
            if (t >= mediaPositionUntil) mediaPosition.clear();
            const bool wanted = browserWorker && browserMedia.present && !open && config.dock.enabled &&
                                vr.panelTakesCairoPixels() && !mediaQuietHidden;
            // Its paused video is kept from Chrome's idle shutdown while it can be seen (BrowserSession::holdPaused): worn,
            // the HUD switched on
            const bool pausedHold = wanted && !browserMedia.playing && hmdInUse(activityLevel) && config.visible;
            if (browserWorker && pausedHold != mediaPausedHeld) {
                mediaPausedHeld = pausedHold;
                browserWorker->post([pausedHold](BrowserSession& s, double tt) { s.holdPaused(pausedHold, tt); });
            }
            if (!browserWorker) mediaPausedHeld = false;
            MediaStripState state;
            if (wanted) {
                state.title = browserTitle;
                state.playing = browserMedia.playing;
                state.muted = browserMedia.muted;
                state.pausedByRemoval = mediaPausedByRemoval && !browserMedia.playing;
                state.position = mediaPosition;
            }
            if (wanted != mediaWanted || state != mediaState) {
                if (wanted != mediaWanted) {
                    std::fprintf(stderr, "[browser] Video controls under the HUD: %s\n", wanted ? "shown" : "gone");
                }
                mediaWanted = wanted;
                mediaState = state;
                mediaDirty = true;
                nextPanel = std::min(nextPanel, nowSeconds());
            }
            if (!wanted) mediaStrip.pointerLeave();
        }
        sendBrowser();

        // A layout changed elsewhere (a file reload, a profile) redraws the open tab at once, and cancels an editor
        // drag made on the old version
        if (settingsVisible) settingsDirty |= settings.syncLayout(config.layout);
        if (settingsVisible && (settingsDirty || !settingsWasVisible)) {
            settings.render(config, autostart.status(), profilesUi);
            vr.submitSettings(settings.pixels());  // premultiplied BGRA as drawn (no RGBA conversion)
            settingsDirty = false;
        }
        settingsWasVisible = settingsVisible;

        // ---- Sources: only the due ones, and only while someone is looking ----
        const double c0 = cpuSeconds();
        sampler.applyReady(nowSeconds());  // slow sensors read on their thread since the last wake
        scheduler.runDue(nowSeconds());
        const double c1 = cpuSeconds();

        // ---- The battery notices: one at a time in front of the eyes, NoticeView::kShowSec each ----
        {
            for (const BatteryWatch::Notice& n : batteryWatch.takeNotices()) notices.push_back(n);
            const double t = nowSeconds();
            // The first-run guide while nothing else wants the overlay; it steps aside for a notice
            if (guideUp) {
                guideShownSec += t - guideCountedAt;
                guideCountedAt = t;
            }
            // Only while the HUD can be pressed (shown, in-game use on): that's what ends it for good
            const bool guideWanted = !config.firstRunGuideDone && config.visible && config.dock.enabled &&
                                     guideShownSec < GuideView::kShowSec && hmdInUse(activityLevel) &&
                                     notices.empty() && noticeHideAt < 0;
            if (guideUp && (!guideWanted || guideStale)) {
                vr.showAux(VrOverlay::Aux::Notice, false);
                guideUp = vr.auxShown(VrOverlay::Aux::Notice);  // a failed hide is tried again in a second
                if (guideUp) guideTryAt = t + 1.0;
                if (!guideUp && config.firstRunGuideDone) std::fprintf(stderr, "[guide] Done: the HUD was pressed\n");
            }
            if (!guideUp && guideWanted && t >= guideTryAt) {
                guideTryAt = t + 1.0;
                guideView.render();
                guideStale = false;
                if (vr.placeNotice(dockmath::headTransform(GuideView::pose())) &&
                    vr.submitAux(VrOverlay::Aux::Notice, guideView.pixels(), GuideView::kWidth, GuideView::kHeight,
                                 GuideView::kWidthM)) {
                    vr.setAuxAlpha(VrOverlay::Aux::Notice, 1);
                    vr.showAux(VrOverlay::Aux::Notice, true);
                    guideUp = vr.auxShown(VrOverlay::Aux::Notice);
                    guideCountedAt = t;
                    if (guideShownSec == 0 && guideUp) std::fprintf(stderr, "[guide] Showing the first-run guide\n");
                }
            }
            // In and out over kFadeSec (0 from its end on, also while a failed hide is tried again)
            if (noticeHideAt >= 0 && !hmdInUse(activityLevel) && t < noticeEndsAt) {
                // Taken off (or standby) while it showed: cut short, so told again from the next reading
                batteryWatch.rearm(noticeShown);
                noticeEndsAt = t;
            }
            if (noticeHideAt >= 0) {
                const double fade = std::min(t - noticeShownAt, noticeEndsAt - t) / NoticeView::kFadeSec;
                vr.setAuxAlpha(VrOverlay::Aux::Notice, std::clamp(fade, 0.0, 1.0));
            }
            if (noticeHideAt >= 0 && (t >= noticeHideAt || !hmdInUse(activityLevel))) {
                vr.showAux(VrOverlay::Aux::Notice, false);
                // Hidden for sure, or tried again in a second (not left in front of the eyes)
                noticeHideAt = vr.auxShown(VrOverlay::Aux::Notice) ? t + 1.0 : -1;
            }
            // Off: the ones waiting go (their %s would be old by the time it is worn again); the watch tells again
            // from a fresh reading then
            if (!hmdInUse(activityLevel) && !notices.empty()) {
                for (const BatteryWatch::Notice& n : notices) batteryWatch.rearm(n);
                notices.clear();
            }
            if (noticeHideAt < 0 && !notices.empty() && hmdInUse(activityLevel) && !guideUp) {
                const BatteryWatch::Notice n = notices.front();
                notices.erase(notices.begin());
                const std::string pct = std::to_string(static_cast<int>(std::lround(n.pct))) + "%";
                std::string line;
                NoticeView::Level level = NoticeView::Level::Warn;
                switch (n.kind) {
                    case BatteryWatch::Notice::Kind::HeadsetLow:
                        line = std::isnan(n.minutes) ? trf(Tx::NoticeHeadsetLow, {pct})
                                                     : trf(Tx::NoticeHeadsetLowTime, {pct, minutesText(n.minutes)});
                        break;
                    case BatteryWatch::Notice::Kind::HeadsetCritical:
                        line = std::isnan(n.minutes)
                                   ? trf(Tx::NoticeHeadsetCritical, {pct})
                                   : trf(Tx::NoticeHeadsetCriticalTime, {pct, minutesText(n.minutes)});
                        level = NoticeView::Level::Danger;
                        break;
                    case BatteryWatch::Notice::Kind::ControllerLow:
                        line = trf(n.left ? Tx::NoticeLeftControllerLow : Tx::NoticeRightControllerLow, {pct});
                        break;
                }
                std::fprintf(stderr, "[battery] Notice: %s\n", line.c_str());
                noticeView.render(line, level);
                if (vr.placeNotice(dockmath::headTransform(NoticeView::pose())) &&
                    vr.submitAux(VrOverlay::Aux::Notice, noticeView.pixels(), NoticeView::kWidth, NoticeView::kHeight,
                                 NoticeView::kWidthM)) {
                    vr.setAuxAlpha(VrOverlay::Aux::Notice, 0);
                    vr.showAux(VrOverlay::Aux::Notice, true);
                    noticeShown = n;
                    noticeShownAt = t;
                    noticeEndsAt = noticeHideAt =
                        t + (level == NoticeView::Level::Danger ? NoticeView::kDangerShowSec : NoticeView::kShowSec);
                } else if (vr.hasAux(VrOverlay::Aux::Notice)) {
                    // Couldn't be shown: due again from the next reading that still says so (not when there is no
                    // notice overlay at all: then it would only be tried and logged every reading)
                    std::fprintf(stderr, "[battery] The notice couldn't be shown; told again later\n");
                    batteryWatch.rearm(n);
                }
            }
        }

        // ---- Panel redraw (2 Hz at most). Only while it's shown and the headset is in use: nobody sees it
        // otherwise. The texture is converted and uploaded only when a pixel changed ----
        double now = nowSeconds();
        const double panelInterval = updateIntervalSec(config);
        if (now >= nextPanel - kCoalesceSec) {
            if (scheduler.demand().hudShown && scheduler.demand().hmdInUse) {
                const double c2 = cpuSeconds();
                const double w2 = nowSeconds();
                refreshControls(now);
                const bool changed = renderer.render(widgetContext(config, store, now, bridge, &controlsView));
                const double c3 = cpuSeconds();
                const double w3 = nowSeconds();
                bool sent = false;
                const frame_perf_overlay::GridGeometry& g = renderer.geometry();
                bool strip = mediaWanted && g.textureWidth >= MediaStrip::kMinWidth;
                // A narrow HUD (Simple B) gets a wider strip, the HUD's picture centred over it, so the buttons keep a
                // size a laser hits
                const int stripWidth = std::max(g.textureWidth, MediaStrip::kMinButtonsWidth);
                const int left = (stripWidth - g.textureWidth) / 2;
                if (strip && (mediaDirty || mediaStrip.width() != stripWidth || mediaStrip.pixels() == nullptr)) {
                    mediaStrip.render(stripWidth, mediaState, renderer.highlight());
                }
                if (strip && mediaStrip.pixels() == nullptr) strip = false;  // cairo couldn't make its surface
                // A failed send goes again at this pace even if nothing changed since
                if (changed || strip != hudStripSent || (strip && mediaDirty) || hudSendFailed) {
                    if (strip) {
                        // The HUD's picture with the video controls under it: one texture, so they show and fade
                        // with the HUD and take its laser
                        const int w = stripWidth;
                        const size_t rowBytes = static_cast<size_t>(w) * 4;
                        const size_t gridRow = static_cast<size_t>(g.textureWidth) * 4;
                        hudPixels.assign(rowBytes * (g.textureHeight + MediaStrip::kHeight), 0);
                        for (int y = 0; y < g.textureHeight; ++y) {
                            std::memcpy(hudPixels.data() + y * rowBytes + left * 4, renderer.pixels() + y * gridRow, gridRow);
                        }
                        std::memcpy(hudPixels.data() + rowBytes * g.textureHeight, mediaStrip.pixels(),
                                    rowBytes * MediaStrip::kHeight);
                        sent = vr.submitPanel(hudPixels.data(), w, g.textureHeight + MediaStrip::kHeight,
                                              g.widthInColumns() * w / g.textureWidth, MediaStrip::kHeight);
                    } else {
                        // cairo's pixels go up as they are (premultiplied BGRA) unless FRAME_PERF_RGBA_UPLOAD is set
                        const uint8_t* pixels =
                            vr.panelTakesCairoPixels() ? renderer.pixels() : renderer.toRgba().data();
                        sent = vr.submitPanel(pixels, g.textureWidth, g.textureHeight, g.widthInColumns());
                    }
                    hudSendFailed = !sent;
                    if (sent) {
                        hudStripSent = strip;
                        hudGridHeight = g.textureHeight;
                        hudGridLeft = strip ? left : 0;
                        hudGridWidth = g.textureWidth;
                        mediaDirty = false;
                    }
                }
                if (!strip) mediaDirty = false;  // nothing of it to draw (a failed send retries on the next change)
                const double c5 = cpuSeconds();
                renderStats.add((w3 - w2) * 1e3, (c3 - c2) * 1e3, (c5 - c3) * 1e3, changed, renderer.lastRedrawn());
                if (options.verbose && now - lastVerbose >= 2.0) {
                    lastVerbose = now;
                    std::fprintf(stderr,
                                 "[values] loops %u | dashboard %s tab %s | %s | CPU ms: sources %.2f render %.2f "
                                 "convert+submit %.2f\n",
                                 loopCount, vr.dashboardOpen() ? "open" : "closed", settingsVisible ? "visible" : "hidden",
                                 changed ? (sent ? "uploaded" : "upload failed") : "unchanged, not uploaded",
                                 (c1 - c0) * 1e3, (c3 - c2) * 1e3, (c5 - c3) * 1e3);
                    loopCount = 0;
                }
            }
            // Next redraw (don't try to catch up after a delay)
            nextPanel += panelInterval;
            if (nextPanel < now) nextPanel = now;
        }

        if (gDumpRequested) {
            gDumpRequested = 0;
            writeDebugDump(scheduler, store, activityLevel, settingsVisible, nowSeconds(),
                           renderStats.text(renderer.geometry()));
        }
        if (vr.deviceLost()) {
            std::fprintf(stderr, "[Vulkan] The device was lost (VK_ERROR_DEVICE_LOST): exiting with %d so the service "
                                 "starts again\n", dockloop::kExitCodeDeviceLost);
            deviceLost = true;
            break;
        }

        // ---- Sleep until the earliest deadline ----
        now = nowSeconds();
        double wakeAt = std::min(nextMetricsTick, scheduler.nextDue(now));
        if (scheduler.demand().hudShown) wakeAt = std::min(wakeAt, nextPanel);  // no redraws while it's hidden
        if (settingsVisible) {
            const double pollSec =
                now - lastPointerEvent < kSettingsActiveHoldSec ? kSettingsPollSec : kSettingsIdlePollSec;
            wakeAt = std::min(wakeAt, now + pollSec);
        }
        if (noticeHideAt >= 0) {
            wakeAt = std::min(wakeAt, noticeHideAt);
            // The fade's steps, else its start out
            if (now < noticeShownAt + NoticeView::kFadeSec || (now > noticeEndsAt - NoticeView::kFadeSec && now < noticeEndsAt)) {
                wakeAt = std::min(wakeAt, now + kNoticeFadeStepSec);
            } else if (now < noticeEndsAt) {
                wakeAt = std::min(wakeAt, noticeEndsAt - NoticeView::kFadeSec);
            }
        }
        // The first-run guide's end (kShowSec of wear)
        // (a failed hide past it: the retry in a second, not a busy loop)
        if (guideUp) {
            const double guideEnd = now + std::max(0.0, GuideView::kShowSec - guideShownSec);
            wakeAt = std::min(wakeAt, std::max(guideEnd, guideTryAt));
        }
        // The video controls: the position's end (a paused strip's time is counted at the HUD's own redraws)
        if (mediaWanted && mediaPositionUntil > now) wakeAt = std::min(wakeAt, mediaPositionUntil);
        // A held button's next repeat and its pending save (+infinity when nothing is held)
        if (settingsVisible) wakeAt = std::min(wakeAt, settings.nextRepeatAt());
        if (gazeFitBusy) wakeAt = std::min(wakeAt, now + 0.25);
        wakeAt = std::min(wakeAt, ctlRepeat.nextAt());
        if (ctlArmedUntil > now) wakeAt = std::min(wakeAt, ctlArmedUntil);  // the "press again" line goes
        if (heldSave.pending) wakeAt = std::min(wakeAt, heldSave.dueAt);
        // +infinity when no placement check is planned; none while the headset is off (the 2 Hz tick notices it worn)
        if (hmdInUse(activityLevel)) wakeAt = std::min(wakeAt, vr.nextPlacementAt());
        // The Dock: the aim check (idle: every kDockIdlePollSec), its timers, the full view's held button, a save
        if ((config.dock.enabled && config.visible && hmdInUse(activityLevel)) ||
            (config.dock.enabled && hmdInUse(activityLevel) && dock.browserOpen())) {
            wakeAt = std::min(wakeAt, nextAim);
        }
        // A held key's repeat, a press on the page becoming long
        if (dock.browserOpen()) wakeAt = std::min(wakeAt, browserCtl.nextWake());
        // The laser just taken off a panel (or the dashboard just closed): the last Up / Leave is fetched soon
        if ((dockLive || browserLive) && vr.overlayEventsDraining(now)) {
            wakeAt = std::min(wakeAt, now + kDockActivePollSec);
        }
        // (The browser's out-of-view countdown needs no wake of its own: nextAim is moved up to its end, and it is
        // paused whenever the check can't run, so it never leaves a deadline in the past.)
        wakeAt = std::min(wakeAt, dock.nextWake(now));
        if (dock.expanded()) {
            wakeAt = std::min(wakeAt, dockView.nextWake());
            if (dock.view() == DockView::Full) wakeAt = std::min(wakeAt, dockSettings.nextRepeatAt());
        }
        if (dockSave.pending) wakeAt = std::min(wakeAt, dockSave.dueAt);
        wakeAt = std::min(wakeAt, steadySeconds(colorModel.nextWake(std::chrono::steady_clock::now())));
        // The volume drag's last value, the PC's status poll while the 기기 page shows, the game question's timeout
        wakeAt = std::min(wakeAt, deviceBackend->nextWake());
        wakeAt = std::min(wakeAt, recorder.nextWake(now));  // a recording's clock, its stop's timeout
        // Never less than a short sleep: a deadline already past (from any bug) must not spin a core. One now and
        // then is a late deadline; many in a row would have been a spin, which the log tells once per streak
        pastWakes = wakeAt <= now ? pastWakes + 1 : 0;
        if (pastWakes == kPastWakesToLog) {
            std::fprintf(stderr, "[loop] %d wakes in a row were already due; sleeping %.0f ms each, not spinning\n",
                         kPastWakesToLog, dockloop::kMinSleepSec * 1e3);
        }
        waitForWake(dockloop::sleepSeconds(wakeAt, now));
    }

    // Same cleanup for SIGTERM / SIGINT, SteamVR quitting, vrserver vanishing, a lost device, and Quit, in this order:
    // the game gets its controllers back and our panels go at once; the layer never keeps showing the original
    // image; SteamVR is let go; only then Chrome is closed (gracefully, then by signals; it may take seconds), so that
    // nothing of it outlives Frame Dock
    dockloop::ExitSteps exitSteps;
    exitSteps.releaseInput = [&] {
        for (const DockTarget target : {DockTarget::Hud, DockTarget::Dock, DockTarget::Browser, DockTarget::Keyboard}) {
            vr.setInteractive(target, false);
        }
        vr.setHudInput(false);
    };
    exitSteps.hideOverlays = [&] {
        vr.showDock(false);
        vr.showAux(VrOverlay::Aux::Browser, false);
        vr.showAux(VrOverlay::Aux::Keyboard, false);
    };
    exitSteps.saveConfig = [&] {
        // A held button's change (or a grab's new place) that wasn't written yet
        if (heldSave.pending || dockSave.pending) saveConfigNow(config, watcher);
    };
    exitSteps.colorOff = [&] {
        if (gazeFeed) gazeFeed->stop();  // gives the eye tracking rate back while SteamVR is still connected
        colorModel.shutdown();
    };
    exitSteps.vrOff = [&] {
        vr.shutdown();
        std::fprintf(stderr, "[VR] Shut down\n");
    };
    exitSteps.browserOff = [&] {
        if (!browserWorker) return;
        std::fprintf(stderr, "[browser] Closing Chrome\n");
        browserWorker->shutdown(kBrowserShutdownSec);
        browserWorker.reset();
    };
    dockloop::runExitSteps(exitSteps);
    // The 기기 page's threads: a sync or a poll in progress ends at once, a volume just set still reaches the server
    deviceBackend.reset();
    if (lockFd >= 0) ::close(lockFd);
    // A user quit gets the exit code systemd's RestartPreventExitStatus= keeps from restarting; a lost device one
    // that is restarted
    if (deviceLost) return dockloop::kExitCodeDeviceLost;
    return userQuit ? kExitCodeUserQuit : 0;
}

/**
 * The soft limit on open files up to kOpenFileLimit (or the hard limit). SteamVR's client library keeps small
 * /dev/shm segments open and their number grows (live, 2026-11-01: about 1.4 a minute in this app, the same in other
 * OpenVR clients), so the default 1024 would run out after some 9–14 hours of SteamVR. Nothing here uses select().
 */
void raiseOpenFileLimit() {
    constexpr rlim_t kOpenFileLimit = 16384;
    rlimit limit {};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) return;
    const rlim_t want = limit.rlim_max == RLIM_INFINITY ? kOpenFileLimit : std::min(kOpenFileLimit, limit.rlim_max);
    if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur >= want) return;
    const rlim_t before = limit.rlim_cur;
    limit.rlim_cur = want;
    if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) {
        std::fprintf(stderr, "[app] Couldn't raise the open file limit from %llu: %s\n",
                     static_cast<unsigned long long>(before), std::strerror(errno));
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Line-buffer stderr so each line reaches the journal at once
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    raiseOpenFileLimit();

    // FrameColor's bench commands first: `--bench N` as the first argument is FrameColor's, not Perf's. Before
    // installSignalHandlers(), as in FrameColor: those handlers only set a flag the bench never looks at, so Ctrl-C or
    // SIGTERM would no longer end a plain --bench. With the default handling it ends at once (it changes no files);
    // --bench-compare installs its own handlers, which stop the run and restore current.txt
    if (const std::optional<int> bench = framecolor::runBenchCommand(argc, argv)) return *bench;
    installSignalHandlers();

    Options options;
    if (!parseOptions(argc, argv, options)) {
        printUsage();
        return 2;
    }
    switch (options.mode) {
        case Options::Mode::Help: printUsage(); return 0;
        case Options::Mode::Version: std::printf("framedock %s\n", FRAMEDOCK_VERSION); return 0;
        case Options::Mode::ContrastReport: return printContrastReport();
        case Options::Mode::Print: return runPrint(options);
        case Options::Mode::DumpPng: return runDumpPng(options);
        case Options::Mode::DumpSettingsPng: return runDumpSettingsPng(options);
        case Options::Mode::DumpFrameTimings: return runDumpFrameTimings(options);
        case Options::Mode::DebugDump: return runDebugDump();
        case Options::Mode::RoomView: return runRoomView(options);
        case Options::Mode::DumpMetrics:
            options.dumpMetrics.count = options.printCount;
            return runDumpMetrics(options.dumpMetrics, gStopRequested);
        case Options::Mode::Overlay: break;
    }
    return runOverlay(options);
}
