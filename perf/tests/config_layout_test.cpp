// headset config + layout: missing / broken config.json, a broken "layout" key, and the atomic save under kill -9.
// POSIX only (fork, SIGKILL). Works in a temp dir; never touches ~/.config.
#include "atomic_file.h"
#include "config.h"
#include "json.h"
#include "layout_edit.h"
#include "metric_registry.h"
#include "scheduler.h"

#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace frame_perf_overlay;
using W = WidgetType;

std::string tempDir;

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

/** A layout that differs per n, so a torn or stale file is noticeable. */
Layout layoutFor(int n) {
    Layout layout = defaultLayout();
    layout.version = static_cast<uint32_t>(n);
    for (LayoutColumn& column : layout.columns) {
        for (WidgetInstance& w : column.widgets) {
            if (hasCompactView(w.type)) w.compact = (n + (w.type == W::Temp ? 1 : 0)) % 2 == 0;
        }
    }
    return layout;
}

// config.json missing / unparsable / broken layout

void testLoad() {
    std::vector<std::string> warnings;
    std::string error;

    Config c;
    c.alpha = 0.5;  // would be reset to the default by a "missing file" load
    CHECK(loadConfig(tempDir + "/nope.json", c, warnings, error));
    CHECK(c.alpha == Config().alpha);
    CHECK(c.layout.sameWidgets(defaultLayout()));
    CHECK(!c.layoutInFile);

    // unparsable: false, and the caller's config is left alone
    writeFile(tempDir + "/garbage.json", "{ \"alpha\": 0.3, ");
    Config keep;
    keep.alpha = 0.7;
    error.clear();
    CHECK(!loadConfig(tempDir + "/garbage.json", keep, warnings, error));
    CHECK(!error.empty());
    CHECK(keep.alpha == 0.7);

    // a layout that isn't a layout: rest of the config kept, default layout, a warning
    writeFile(tempDir + "/badlayout.json", R"({ "alpha": 0.35, "layout": { "columns": 5 } })");
    Config bad;
    warnings.clear();
    CHECK(loadConfig(tempDir + "/badlayout.json", bad, warnings, error));
    CHECK(bad.alpha == 0.35);
    CHECK(bad.layout.sameWidgets(defaultLayout()));
    CHECK(!bad.layoutInFile);
    CHECK(!warnings.empty());

    // a good custom layout in the file is used
    writeFile(tempDir + "/goodlayout.json",
              R"({ "layout": { "version": 5, "columns": [[{"type":"battery","compact":true}],[{"type":"temp"}]] } })");
    Config good;
    warnings.clear();
    CHECK(loadConfig(tempDir + "/goodlayout.json", good, warnings, error));
    CHECK(good.layoutInFile);
    CHECK(good.layout.version == 5);
    CHECK(good.layout.columns.size() == 2);
    CHECK(good.layout.columns[0].widgets[0].type == W::Battery && good.layout.columns[0].widgets[0].compact);

    // save + load round trip keeps the layout
    Config saved;
    saved.layout = layoutFor(7);
    CHECK(saveConfig(tempDir + "/sub/dir/roundtrip.json", saved, error));
    Config loaded;
    warnings.clear();
    CHECK(loadConfig(tempDir + "/sub/dir/roundtrip.json", loaded, warnings, error));
    CHECK(loaded.layoutInFile);
    CHECK(loaded.layout.version == 7);
    CHECK(loaded.layout.sameWidgets(saved.layout));
}

// Atomic save under kill -9

/** The child: save over and over, forever (until the parent kills it). */
[[noreturn]] void saveLoop(const std::string& path) {
    Config config;
    config.fontPath = std::string(200 * 1024, 'x');  // a big file, so a kill lands mid-write now and then
    std::string error;
    for (int n = 1;; ++n) {
        config.layout = layoutFor(n);
        saveConfig(path, config, error);
    }
}

void testKillDuringSave() {
    const std::string path = tempDir + "/kill/config.json";
    std::string error;
    Config first;
    first.fontPath = std::string(200 * 1024, 'x');
    first.layout = layoutFor(1);
    CHECK(saveConfig(path, first, error));

    const int kKills = 200;
    int bad = 0;
    srand(12345);
    for (int k = 0; k < kKills; ++k) {
        const pid_t pid = fork();
        if (pid < 0) {
            CHECK(false && "fork failed");
            return;
        }
        if (pid == 0) saveLoop(path);
        struct timespec delay = {0, (1 + rand() % 25) * 1000000L};  // 1-25 ms
        nanosleep(&delay, nullptr);
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);

        Config loaded;
        std::vector<std::string> warnings;
        std::string loadError;
        const bool ok = loadConfig(path, loaded, warnings, loadError);
        const bool intact = ok && loaded.layoutInFile && loaded.layout.columns.size() == defaultLayout().columns.size() &&
                            loaded.layout.sameWidgets(layoutFor(static_cast<int>(loaded.layout.version))) &&
                            loaded.fontPath.size() == 200 * 1024;
        if (!intact) {
            ++bad;
            std::printf("kill %d: config.json not intact (load ok=%d, error=%s)\n", k, ok ? 1 : 0, loadError.c_str());
        }
    }
    CHECK(bad == 0);
    std::printf("kill -9 test: %d kills, %d broken files\n", kKills, bad);
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// No config file at all (a fresh install): the watcher starts from freshConfig(), as loadConfig does
void testFreshInstallThroughWatcher() {
    makeDirectories(tempDir + "/fresh");
    ConfigWatcher watcher(tempDir + "/fresh/config.json");
    const Config config = watcher.loadInitial();
    CHECK(config.attachment == Attachment::LeftWrist && config.languageAuto && !config.firstRunGuideDone);
}

// An unreadable config is kept as .bad, not overwritten
void testBrokenConfigKeptAsBad() {
    const std::string path = tempDir + "/broken/config.json";
    makeDirectories(tempDir + "/broken");
    const std::string original = "{ \"alpha\": 0.4, oops this isn't JSON\n";
    writeFile(path, original);
    writeFile(path + ".bad", "an older bad file");

    ConfigWatcher watcher(path);
    Config config = watcher.loadInitial();
    CHECK(watcher.lastLoadFailed());
    CHECK(readFile(path) == original);  // loading alone never touches it

    // What a dashboard action / second-launch toggle does: change something, save through the watcher
    config.visible = false;
    std::string error;
    CHECK(watcher.save(config, error));
    CHECK(readFile(path + ".bad") == original);  // the user's text, byte-identical (older .bad replaced)
    std::vector<std::string> warnings;
    Config reloaded;
    CHECK(loadConfig(path, reloaded, warnings, error));
    CHECK(!reloaded.visible && reloaded.layoutInFile);
    CHECK(!watcher.lastLoadFailed());

    // The next save just saves (the .bad copy stays as it was)
    config.alpha = 0.7;
    CHECK(watcher.save(config, error));
    CHECK(readFile(path + ".bad") == original);
}

// A "layout" the loader had to reject is kept as .bad on the next save, like an unreadable file; a file from
// before the grid (no "layout"), or a layout that only needed tidying, is not
void testRejectedLayoutKeptAsBad() {
    makeDirectories(tempDir + "/rejected");
    const std::string path = tempDir + "/rejected/config.json";
    const std::string original = R"({ "alpha": 0.4, "layout": { "columns": 5 } })";
    writeFile(path, original);

    Config loaded;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(loadConfig(path, loaded, warnings, error));
    CHECK(loaded.layoutRejected && !loaded.layoutInFile);
    CHECK(loaded.alpha == 0.4 && loaded.layout.sameWidgets(defaultLayout()));

    ConfigWatcher watcher(path);
    Config config = watcher.loadInitial();
    CHECK(!watcher.lastLoadFailed());
    CHECK(config.layoutRejected);
    CHECK(readFile(path) == original);  // loading alone never touches it
    config.visible = false;
    CHECK(watcher.save(config, error));
    CHECK(readFile(path + ".bad") == original);  // the user's text, byte-identical
    Config reloaded;
    warnings.clear();
    CHECK(loadConfig(path, reloaded, warnings, error));
    CHECK(!reloaded.visible && reloaded.layoutInFile && !reloaded.layoutRejected && warnings.empty());

    // the next save just saves; the .bad copy stays as it was
    config.alpha = 0.7;
    CHECK(watcher.save(config, error));
    CHECK(readFile(path + ".bad") == original);

    // a config from before the grid: saved over, nothing kept
    const std::string old = tempDir + "/rejected/old.json";
    writeFile(old, R"({ "alpha": 0.4 })");
    ConfigWatcher oldWatcher(old);
    Config oldConfig = oldWatcher.loadInitial();
    CHECK(!oldConfig.layoutRejected && !oldConfig.layoutInFile);
    CHECK(oldWatcher.save(oldConfig, error));
    CHECK(access((old + ".bad").c_str(), F_OK) != 0);

    // a layout that was only tidied (all columns blank -> default) is usable, so it isn't kept either
    const std::string blank = tempDir + "/rejected/blank.json";
    writeFile(blank, R"({ "layout": { "columns": [[]] } })");
    ConfigWatcher blankWatcher(blank);
    Config blankConfig = blankWatcher.loadInitial();
    CHECK(!blankConfig.layoutRejected && blankConfig.layoutInFile);
    CHECK(blankWatcher.save(blankConfig, error));
    CHECK(access((blank + ".bad").c_str(), F_OK) != 0);
}

// The user removes every widget in the editor: the empty layout is saved, comes back empty, and is not mistaken
// for a broken file
void testEmptiedLayoutRoundTrip() {
    makeDirectories(tempDir + "/emptied");
    const std::string path = tempDir + "/emptied/config.json";
    Config config;
    config.layout.version = 4;
    while (!config.layout.columns.empty()) CHECK(removeWidget(config.layout, {0, 0}));  // what the editor's Remove does
    CHECK(config.layout.columns.empty());
    ConfigWatcher watcher(path);
    std::string error;
    CHECK(watcher.save(config, error));

    Config loaded;
    std::vector<std::string> warnings;
    CHECK(loadConfig(path, loaded, warnings, error));
    CHECK(loaded.layout.columns.empty() && loaded.layout.version == 4);
    CHECK(loaded.layoutInFile && !loaded.layoutRejected && warnings.empty());

    // and the running app's watcher: same, and a later save doesn't set the file aside
    ConfigWatcher reopened(path);
    Config again = reopened.loadInitial();
    CHECK(!reopened.lastLoadFailed() && !again.layoutRejected && again.layout.columns.empty());
    again.alpha = 0.6;
    CHECK(reopened.save(again, error));
    CHECK(access((path + ".bad").c_str(), F_OK) != 0);
    CHECK(loadConfig(path, loaded, warnings, error) && loaded.layout.columns.empty() && loaded.alpha == 0.6);
}

// control chars / quotes in the font paths survive save -> load

void testFontPathEscaping() {
    Config config;
    config.fontPath = std::string("a\tb\nc\x01") + "d";
    config.boldFontPath = R"(q"uote\back\slash")";
    const std::string path = tempDir + "/escape.json";
    std::string error;
    CHECK(saveConfig(path, config, error));
    Config loaded;
    std::vector<std::string> warnings;
    CHECK(loadConfig(path, loaded, warnings, error));
    CHECK(loaded.fontPath == config.fontPath);
    CHECK(loaded.boldFontPath == config.boldFontPath);
}

// The Dock's settings ("dock"): defaults, save -> load, out-of-range values clamped, a bad hand named, and a file
// without the key (Perf's, an older Frame Dock's) keeps the defaults
void testNonFiniteAndHugeFiles() {
    // A nan / inf that got into the settings (a bad grab, a hand edit clamped oddly) is saved as a number: the file
    // stays JSON and loads next time
    Config config;
    config.posX = std::numeric_limits<double>::quiet_NaN();
    config.widthM = std::numeric_limits<double>::infinity();
    config.dock.head.yaw = -std::numeric_limits<double>::infinity();
    config.browser.leftWrist.z = std::numeric_limits<double>::quiet_NaN();
    config.thresholds.tempWarnC = std::numeric_limits<double>::quiet_NaN();
    const std::string path = tempDir + "/nonfinite.json";
    std::string error;
    CHECK(saveConfig(path, config, error));
    const std::string text = readFile(path);
    CHECK(text.find("nan") == std::string::npos && text.find("inf") == std::string::npos);
    Config loaded;
    std::vector<std::string> warnings;
    CHECK(loadConfig(path, loaded, warnings, error));
    CHECK(std::isfinite(loaded.posX) && std::isfinite(loaded.widthM) && std::isfinite(loaded.dock.head.yaw));
    // A file far larger than any config: refused without reading it all
    const std::string huge = tempDir + "/huge.json";
    writeFile(huge, "{\"alpha\": 0.5, \"x\": \"" + std::string(kMaxConfigBytes + 10, 'a') + "\"}");
    CHECK(!loadConfig(huge, loaded, warnings, error));
    CHECK(error.find("MiB") != std::string::npos);
}

void testDockSettings() {
    const Config defaults;
    CHECK(defaults.dock.enabled && defaults.dock.pointerHand == DockHand::Auto && defaults.dock.collapseSec == 20);
    Config config;
    config.dock.enabled = false;
    config.dock.pointerHand = DockHand::Right;
    config.dock.widthM = 0.42;
    config.dock.collapseSec = 0;
    config.dock.gripInput = false;
    config.dock.head.x = 0.123;
    config.dock.head.pitch = -12.5;
    config.dock.leftWrist.z = -0.2;
    config.dock.rightWrist.yaw = 33.3;
    const std::string path = tempDir + "/dock.json";
    std::string error;
    CHECK(saveConfig(path, config, error));
    Config loaded;
    std::vector<std::string> warnings;
    CHECK(loadConfig(path, loaded, warnings, error));
    CHECK(warnings.empty());
    CHECK(!loaded.dock.enabled && loaded.dock.pointerHand == DockHand::Right && loaded.dock.collapseSec == 0);
    CHECK(std::fabs(loaded.dock.widthM - 0.42) < 1e-9 && !loaded.dock.gripInput);
    CHECK(!(loaded.dock.head != config.dock.head));
    CHECK(!(loaded.dock.leftWrist != config.dock.leftWrist) && !(loaded.dock.rightWrist != config.dock.rightWrist));

    writeFile(tempDir + "/dock-bad.json",
              R"({"dock": {"pointer_hand": "both", "width_m": 5, "collapse_sec": -3, "nope": 1}})");
    Config bad;
    warnings.clear();
    CHECK(loadConfig(tempDir + "/dock-bad.json", bad, warnings, error));
    CHECK(bad.dock.pointerHand == DockHand::Auto && std::fabs(bad.dock.widthM - 0.60) < 1e-9 && bad.dock.collapseSec == 0);
    CHECK(warnings.size() >= 4);

    writeFile(tempDir + "/dock-none.json", R"({"visible": true})");
    Config none;
    warnings.clear();
    CHECK(loadConfig(tempDir + "/dock-none.json", none, warnings, error));
    CHECK(none.dock.enabled && none.dock.pointerHand == DockHand::Auto && !(none.dock.leftWrist != defaultDockWrist()));
}

// The browser panel's settings ("browser"): defaults, save -> load (extra args with quotes and Korean, 보기's settings),
// out-of-range values clamped, bad names warned about, and a file without the key keeps the defaults
void testBrowserSettings() {
    const Config defaults;
    CHECK(defaults.browser.chromeCommand == "flatpak run com.google.Chrome" && defaults.browser.headless);
    CHECK(defaults.browser.transportPipe);
    CHECK(defaults.browser.fps == 30 && defaults.browser.keepAudio && defaults.browser.idleShutdownMin == 10);
    CHECK(defaults.browser.pauseWhenRemoved);
    CHECK(defaults.browser.aspect == BrowserAspect::Wide && !defaults.browser.mobile && defaults.browser.zoomPct == 100);
    // The page's size: 720p's pixels in the 비율's shape (even numbers)
    int pw = 0, ph = 0;
    defaults.browser.pageSize(pw, ph);
    CHECK(pw == 1280 && ph == 720);
    BrowserSettings shaped;
    shaped.aspect = BrowserAspect::Tall;
    shaped.pageSize(pw, ph);
    CHECK(pw == 720 && ph == 1280);
    shaped.aspect = BrowserAspect::Square;
    shaped.pageSize(pw, ph);
    CHECK(pw == 960 && ph == 960);
    shaped.aspect = BrowserAspect::Standard;
    shaped.viewportWidth = 1920;
    shaped.viewportHeight = 1080;
    shaped.pageSize(pw, ph);
    CHECK(pw % 2 == 0 && ph % 2 == 0 && std::abs(pw * 3 - ph * 4) < 12 && std::abs(pw * ph - 1920 * 1080) < 4000);
    CHECK(defaults.browser.anchor == BrowserAnchor::Auto && defaults.browser.viewportWidth == 1280);
    Config config;
    config.browser.chromeCommand = "/usr/bin/chromium --ozone-platform=x11";
    config.browser.headless = false;
    config.browser.transportPipe = false;
    config.browser.home = "https://m.naver.com/?q=\"한글\"";
    config.browser.aspect = BrowserAspect::Square;
    config.browser.mobile = true;
    config.browser.zoomPct = 125;
    config.browser.fps = 24;
    config.browser.viewportWidth = 1600;
    config.browser.viewportHeight = 900;
    config.browser.jpegQuality = 80;
    config.browser.keepAudio = false;
    config.browser.pauseWhenRemoved = false;
    config.browser.idleShutdownMin = 0;
    config.browser.widthM = 0.62;
    config.browser.anchor = BrowserAnchor::RightWrist;
    config.browser.extraArgs = {"--enable-features=VaapiVideoDecoder", "--ignore-gpu-blocklist"};
    config.browser.head.z = -1.1;
    config.browser.leftWrist.pitch = -150;
    config.browser.rightWrist.y = 0.2;
    const std::string path = tempDir + "/browser.json";
    std::string error;
    CHECK(saveConfig(path, config, error));
    Config loaded;
    std::vector<std::string> warnings;
    CHECK(loadConfig(path, loaded, warnings, error));
    CHECK(warnings.empty());
    for (const std::string& w : warnings) std::printf("  warning: %s\n", w.c_str());
    CHECK(loaded.browser == config.browser);

    writeFile(tempDir + "/browser-bad.json",
              R"({"browser": {"mode": "fullscreen", "transport": "tcp", "fps": 500, "width": 100, "width_m": 9, "anchor": "knee",
                 "bookmarks": [{"name": "x"}], "aspect": "2:1", "zoom_pct": 900, "extra_args": "--x",
                 "what": 1}})");
    Config bad;
    warnings.clear();
    CHECK(loadConfig(tempDir + "/browser-bad.json", bad, warnings, error));
    CHECK(bad.browser.headless && bad.browser.fps == 60 && bad.browser.viewportWidth == 640);
    CHECK(bad.browser.transportPipe);
    CHECK(std::fabs(bad.browser.widthM - 1.5) < 1e-9 && bad.browser.anchor == BrowserAnchor::Auto);
    CHECK(bad.browser.aspect == BrowserAspect::Wide && bad.browser.zoomPct == 200);  // the old "bookmarks": ignored
    CHECK(warnings.size() >= 8);

    writeFile(tempDir + "/browser-none.json", R"({"visible": true})");
    Config none;
    warnings.clear();
    CHECK(loadConfig(tempDir + "/browser-none.json", none, warnings, error));
    CHECK(none.browser == BrowserSettings {});

}

// A config.json from v0.3.2 (before the widget grid), written exactly as that version's saveConfig wrote it:
// same key order and spacing, numbers through %.4g. Every value is off its default.
const char* const kOldConfig = R"({
  "visible": false,
  "update_check": false,
  "language": "ja",
  "attachment": "left_wrist",
  "position": { "x": 0.12, "y": -0.3, "z": -0.65 },
  "rotation": { "yaw": 12.5, "pitch": -7.5, "roll": 3 },
  "left_wrist": { "x": 0.01, "y": 0.07, "z": 0.09, "pitch": -80, "yaw": 5, "roll": -4 },
  "right_wrist": { "x": -0.02, "y": 0.06, "z": 0.1, "pitch": -100, "yaw": -6.5, "roll": 8 },
  "wrist_fade": false,
  "wrist_fade_end_deg": 60,
  "clock_format": 12,
  "width_m": 0.24,
  "alpha": 0.75,
  "update_interval_ms": 750,
  "graph_seconds": 60,
  "font": "/opt/fonts/Custom-Regular.ttf",
  "font_bold": "/opt/fonts/Custom-Bold.ttf",
  "thresholds": {
    "fps_warn_ratio": 0.9,
    "fps_crit_ratio": 0.6,
    "frame_warn_ratio": 0.8,
    "frame_crit_ratio": 1.1,
    "reproj_warn_pct": 8,
    "reproj_crit_pct": 25,
    "temp_warn_c": 65,
    "temp_crit_c": 75,
    "battery_warn_pct": 35,
    "battery_crit_pct": 12,
    "power_warn_w": 12,
    "power_crit_w": 15,
    "cpu_warn_pct": 80,
    "cpu_crit_pct": 95,
    "gpu_warn_pct": 82,
    "gpu_crit_pct": 92,
    "wifi_warn_dbm": -65,
    "wifi_crit_dbm": -75,
    "controller_warn_pct": 25,
    "controller_crit_pct": 8
  }
}
)";

const char* const kOldKeys[] = {"visible",    "update_check",       "language",      "attachment",
                                "position",   "rotation",           "left_wrist",    "right_wrist",
                                "wrist_fade", "wrist_fade_end_deg", "clock_format",  "width_m",
                                "alpha",      "update_interval_ms", "graph_seconds", "font",
                                "font_bold",  "thresholds"};

/** Every value of kOldConfig, as the loader must have read it. */
void checkOldValues(const Config& c) {
    CHECK(!c.visible && !c.updateCheck);
    CHECK(c.language == Language::Ja && !c.languageAuto);  // a fixed language
    CHECK(c.attachment == Attachment::LeftWrist);
    CHECK(c.posX == 0.12 && c.posY == -0.3 && c.posZ == -0.65);
    CHECK(c.yawDeg == 12.5 && c.pitchDeg == -7.5 && c.rollDeg == 3);
    CHECK(c.leftWrist.x == 0.01 && c.leftWrist.y == 0.07 && c.leftWrist.z == 0.09);
    CHECK(c.leftWrist.pitch == -80 && c.leftWrist.yaw == 5 && c.leftWrist.roll == -4);
    CHECK(c.rightWrist.x == -0.02 && c.rightWrist.y == 0.06 && c.rightWrist.z == 0.1);
    CHECK(c.rightWrist.pitch == -100 && c.rightWrist.yaw == -6.5 && c.rightWrist.roll == 8);
    CHECK(!c.wristFade && c.wristFadeEndDeg == 60);
    CHECK(c.clockFormat == 12);
    CHECK(c.widthM == 0.24 && c.alpha == 0.75);
    CHECK(c.updateIntervalMs == 750 && c.graphSeconds == 60);
    CHECK(c.fontPath == "/opt/fonts/Custom-Regular.ttf" && c.boldFontPath == "/opt/fonts/Custom-Bold.ttf");
    const Thresholds& t = c.thresholds;
    CHECK(t.fpsWarnRatio == 0.9 && t.fpsCritRatio == 0.6);
    CHECK(t.frameWarnRatio == 0.8 && t.frameCritRatio == 1.1);
    CHECK(t.reprojWarnPct == 8 && t.reprojCritPct == 25);
    CHECK(t.tempWarnC == 65 && t.tempCritC == 75);
    CHECK(t.batteryWarnPct == 35 && t.batteryCritPct == 12);
    CHECK(t.powerWarnW == 12 && t.powerCritW == 15);
    CHECK(t.cpuWarnPct == 80 && t.cpuCritPct == 95);
    CHECK(t.gpuWarnPct == 82 && t.gpuCritPct == 92);
    CHECK(t.wifiWarnDbm == -65 && t.wifiCritDbm == -75);
    CHECK(t.controllerWarnPct == 25 && t.controllerCritPct == 8);
}

void testOldConfigUpgrade() {
    const std::string path = tempDir + "/old/config.json";
    makeDirectories(tempDir + "/old");
    writeFile(path, kOldConfig);

    // loads clean: no warnings, every value carried over, and the grid is the one-column default
    Config c;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(loadConfig(path, c, warnings, error));
    for (const std::string& w : warnings) std::printf("unexpected warning: %s\n", w.c_str());
    CHECK(warnings.empty());
    checkOldValues(c);
    CHECK(!c.layoutInFile);
    CHECK(c.layout.sameWidgets(defaultLayout()));
    CHECK(c.layout.columns.size() == 2);
    CHECK(c.activeProfile.empty());

    // save and load again: values and the layout survive, the layout is now in the file
    const std::string savedPath = tempDir + "/old/saved.json";
    CHECK(saveConfig(savedPath, c, error));
    Config again;
    warnings.clear();
    CHECK(loadConfig(savedPath, again, warnings, error));
    CHECK(warnings.empty());
    checkOldValues(again);
    CHECK(again.layoutInFile);
    CHECK(again.layout.sameWidgets(c.layout));
    CHECK(again.layout.version == c.layout.version);

    // the saved file still has the 18 keys an older version reads
    JsonValue root;
    CHECK(parseJson(readFile(savedPath), root, error));
    for (const char* key : kOldKeys) {
        if (root.get(key) == nullptr) std::printf("saved config lost key %s\n", key);
        CHECK(root.get(key) != nullptr);
    }
    const JsonValue* thresholds = root.get("thresholds");
    CHECK(thresholds != nullptr && thresholds->members.size() == 20);

    // a broken "layout" next to an old config: the rest is kept, the grid falls back to the default, with a warning
    std::string broken = kOldConfig;
    broken.replace(broken.rfind('}'), 1, ",\n  \"layout\": { \"columns\": \"nope\" }\n}");
    writeFile(tempDir + "/old/broken-layout.json", broken);
    Config fallback;
    warnings.clear();
    CHECK(loadConfig(tempDir + "/old/broken-layout.json", fallback, warnings, error));
    checkOldValues(fallback);
    CHECK(!fallback.layoutInFile);
    CHECK(fallback.layout.sameWidgets(defaultLayout()));
    CHECK(warnings.size() == 1);
}

// update_interval_ms from a legacy config is the panel's redraw interval and the cadence of the per-update sources
// (main.cpp hands max(100, ms) / 1000 to Scheduler::setUpdateInterval)
void testUpdateIntervalFromLegacyConfig() {
    const struct {
        int writtenMs;
        int loadedMs;
        double seconds;
    } cases[] = {{100, 100, 0.1}, {50, 100, 0.1}, {1000, 1000, 1.0}, {500, 500, 0.5}};
    for (const auto& c : cases) {
        const std::string path = tempDir + "/interval-" + std::to_string(c.writtenMs) + ".json";
        writeFile(path, "{\n  \"update_interval_ms\": " + std::to_string(c.writtenMs) + "\n}\n");
        Config config;
        std::vector<std::string> warnings;
        std::string error;
        CHECK(loadConfig(path, config, warnings, error));
        CHECK(config.updateIntervalMs == c.loadedMs);
        CHECK(warnings.empty() == (c.writtenMs == c.loadedMs));  // 50 is clamped with a warning

        Scheduler scheduler;
        scheduler.setUpdateInterval(config.updateIntervalMs / 1000.0);
        CHECK(std::fabs(scheduler.interval(SourceId::CpuLoad) - c.seconds) < 1e-9);
        CHECK(std::fabs(scheduler.interval(SourceId::FrameTiming) - c.seconds) < 1e-9);
        CHECK(std::fabs(scheduler.interval(SourceId::GpuClock) - c.seconds) < 1e-9);
        CHECK(std::fabs(scheduler.interval(SourceId::Power) - 2.0) < 1e-9);  // fixed
        CHECK(std::fabs(scheduler.interval(SourceId::Battery) - 5.0) < 1e-9);
    }
}

}  // namespace

int main() {
    char pattern[] = "/tmp/frame-perf-overlay-config-tests/cfg-XXXXXX";
    makeDirectories("/tmp/frame-perf-overlay-config-tests");
    const char* dir = mkdtemp(pattern);
    if (dir == nullptr) {
        std::printf("mkdtemp failed\n");
        return 1;
    }
    tempDir = dir;
    testLoad();
    testKillDuringSave();
    testFreshInstallThroughWatcher();
    testBrokenConfigKeptAsBad();
    testRejectedLayoutKeptAsBad();
    testEmptiedLayoutRoundTrip();
    testFontPathEscaping();
    testNonFiniteAndHugeFiles();
    testDockSettings();
    testBrowserSettings();
    testOldConfigUpgrade();
    testUpdateIntervalFromLegacyConfig();
    const std::string cleanup = "rm -rf '" + tempDir + "'";
    if (std::system(cleanup.c_str()) != 0) std::printf("(could not remove %s)\n", tempDir.c_str());
    if (failures == 0) std::printf("config-layout-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
