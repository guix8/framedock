// The browser panel's main-loop helpers. See browser_loop.h.
#include "browser_loop.h"

#include "app_text.h"
#include "chrome_process.h"
#include "placement.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace browserloop {

std::string defaultBrowserDir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base;
    if (xdg != nullptr && xdg[0] == '/') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = std::string(home != nullptr ? home : ".") + "/.local/share";
    }
    return base + "/framedock/browser";
}

std::string defaultFavoritesPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg != nullptr && xdg[0] == '/') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = std::string(home != nullptr ? home : ".") + "/.config";
    }
    return base + "/framedock/favorites.json";
}

Attachment browserAttachment(const Config& config) {
    switch (config.browser.anchor) {
        case BrowserAnchor::LeftWrist: return Attachment::LeftWrist;
        case BrowserAnchor::RightWrist: return Attachment::RightWrist;
        case BrowserAnchor::Head: return Attachment::Head;
        case BrowserAnchor::Auto: break;
    }
    return config.attachment;
}

dockmath::Mat34 browserLocal(const Config& config) {
    switch (browserAttachment(config)) {
        case Attachment::LeftWrist: return wristTransform(config.browser.leftWrist);
        case Attachment::RightWrist: return wristTransform(config.browser.rightWrist);
        case Attachment::Head: break;
    }
    return dockmath::headTransform(config.browser.head);
}

void setBrowserPlacementFrom(const dockmath::Mat34& local, Config& config) {
    switch (browserAttachment(config)) {
        case Attachment::LeftWrist: config.browser.leftWrist = dockmath::wristPoseFrom(local); break;
        case Attachment::RightWrist: config.browser.rightWrist = dockmath::wristPoseFrom(local); break;
        case Attachment::Head: config.browser.head = dockmath::headPoseFrom(local); break;
    }
}

double panelWidthM(const BrowserSettings& browser, int panelWidth, int panelHeight) {
    if (browser.aspect != BrowserAspect::Tall || panelWidth <= 0 || panelHeight <= 0) return browser.widthM;
    return browser.widthM * panelWidth / panelHeight;
}

double keyboardWidthM(double browserWidthM) { return std::clamp(browserWidthM * 0.9, 0.35, 0.6); }

dockmath::Mat34 keyboardUnderBrowser(double browserHeightM, double keyboardHeightM) {
    const double a = kKeyboardTiltDeg * M_PI / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    dockmath::Mat34 m = dockmath::identity();
    // Rx(−tilt): the keyboard's up (its +y) leans back, away from the user; its face (+z) turns up toward the eyes
    m.m[1][1] = static_cast<float>(c);
    m.m[1][2] = static_cast<float>(s);
    m.m[2][1] = static_cast<float>(-s);
    m.m[2][2] = static_cast<float>(c);
    // Its top edge (local y = +h/2) sits kKeyboardGapM under the panel's lower edge
    const double half = keyboardHeightM / 2;
    m.m[1][3] = static_cast<float>(-browserHeightM / 2 - kKeyboardGapM - half * c);
    m.m[2][3] = static_cast<float>(half * s);
    return m;
}

PointerHands browserHands(const Config& config, bool pinned) {
    if (pinned) return {true, true};
    PointerHands hands = pointerHands(config.dock.pointerHand, config.attachment);
    const Attachment at = browserAttachment(config);
    if (at == Attachment::LeftWrist) hands.left = false;
    if (at == Attachment::RightWrist) hands.right = false;
    if (!hands.left && !hands.right) {
        // The setting names the hand the panel hangs on: the other one points
        hands.left = at != Attachment::LeftWrist;
        hands.right = at != Attachment::RightWrist;
    }
    return hands;
}

bool panelShowing(bool open, bool live, bool placed) { return open && live && placed; }

double stepWidth(double widthM, int direction) {
    const double next = std::round((widthM + direction * kWidthStepM) * 1000.0) / 1000.0;
    return std::clamp(next, kMinWidthM, kMaxWidthM);
}

SessionOptions sessionOptions(const BrowserSettings& browser, const std::string& dir) {
    SessionOptions o;
    o.launch.command = splitCommand(browser.chromeCommand);
    if (o.launch.command.empty()) o.launch.command = splitCommand(BrowserSettings {}.chromeCommand);
    // The flatpak's zypak launcher closes inherited fds 3/4 at once (tried on the headset 2026-10-29: the pipe
    // breaks before Chrome reads it), so a flatpak Chrome goes straight to the port instead of failing over.
    const bool flatpak = !o.launch.command.empty() && o.launch.command.front() == "flatpak";
    o.launch.transport = browser.transportPipe && !flatpak ? DevToolsTransport::Pipe : DevToolsTransport::Port;
    o.launch.profileDir = dir + "/profile";
    o.launch.logPath = dir + "/chrome.log";
    // framedock/ and framedock/browser/ (the profile's parents) owner-only too
    const size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos && slash > 0) o.launch.privateDirs.push_back(dir.substr(0, slash));
    o.launch.privateDirs.push_back(dir);
    o.launch.headless = browser.headless;
    browser.pageSize(o.launch.width, o.launch.height);  // 720p / 1080p's pixels in the 보기 list's 비율
    o.mobile = browser.mobile;
    o.zoomPct = browser.zoomPct;
    o.launch.extraArgs = browser.extraArgs;
    o.fpsCap = browser.fps;
    o.jpegQuality = browser.jpegQuality;
    o.homeUrl = browser.home;
    o.keepAudio = browser.keepAudio;
    o.idleShutdownSec = browser.idleShutdownMin * 60.0;
    return o;
}

std::string statusLine(const BrowserSession::Status& status, pid_t pid) {
    using Phase = BrowserSession::Phase;
    switch (status.phase) {
        case Phase::Off: return tr(Tx::ChromeOff);
        case Phase::Starting:
        case Phase::Connecting:
        case Phase::Attaching: return tr(Tx::ChromeStarting);
        case Phase::Ready:
            return trf(status.channel == "pipe"   ? Tx::ChromeRunningPipe
                       : status.channel == "port" ? Tx::ChromeRunningPort
                                                  : Tx::ChromeRunning,
                       {std::to_string(static_cast<int>(pid))});
        case Phase::Stopping: return tr(Tx::ChromeClosing);
        case Phase::Restarting: return trf(Tx::ChromeRestarting, {std::to_string(status.restarts)});
        case Phase::Failed: return trf(Tx::ChromeFailed, {status.error});
    }
    return {};
}

bool usableHome(const std::string& url) { return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0; }

bool panelInView(const dockmath::Mat34& panel, double widthM, double heightM, const dockmath::Mat34& head) {
    // Its front toward the head (the wrist fade's measure; at 90° it is edge-on)
    if (wristFacingAlpha(panel, head, 90.0) <= 0.0) return false;
    double toPanel[3];
    double distance = 0;
    for (int i = 0; i < 3; ++i) {
        toPanel[i] = panel.m[i][3] - head.m[i][3];
        distance += toPanel[i] * toPanel[i];
    }
    distance = std::sqrt(distance);
    if (!std::isfinite(distance) || distance < 1e-6) return true;  // at the eye: nothing to tell
    // Where the head looks: its −z axis
    double cosine = 0;
    for (int i = 0; i < 3; ++i) cosine += -head.m[i][2] * toPanel[i] / distance;
    const double angle = std::acos(std::clamp(cosine, -1.0, 1.0)) * 180.0 / M_PI;
    // Some of the panel within the cone: its half-diagonal as seen from the head widens the cone
    const double radius = std::atan2(0.5 * std::hypot(widthM, heightM), distance) * 180.0 / M_PI;
    return angle - radius < kInViewHalfFovDeg;
}

}  // namespace browserloop
