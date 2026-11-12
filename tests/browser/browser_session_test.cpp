// The browser panel's Chrome session against a fake Chrome (tests/browser/fake_chrome.cpp, a real process speaking
// the DevTools protocol over a real WebSocket): start, attach, frames and the fps cap, input, navigation, hiding,
// the idle shutdown, a crash and the restart, a Chrome that ignores SIGTERM, a helper process left in its group,
// leftovers of an earlier run, popups, and the worker thread. Never starts the real Chrome.
#include "browser_session.h"
#include "browser_worker.h"
#include "chrome_process.h"

#include <signal.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>

#ifndef FAKE_CHROME
#error "FAKE_CHROME (the fake Chrome's path) must be defined"
#endif

namespace {

int failures = 0;

#define CHECK(expr)                                             \
    do {                                                        \
        if (!(expr)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
            ++failures;                                         \
        }                                                       \
    } while (false)

using Phase = BrowserSession::Phase;

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::filesystem::path root;

SessionOptions optionsFor(const std::string& name) {
    SessionOptions o;
    o.launch.command = {FAKE_CHROME};
    o.launch.profileDir = (root / name).string();
    o.launch.logPath = (root / (name + ".log")).string();
    o.homeUrl = "https://home.example/";
    o.fpsCap = 10;
    o.startTimeoutSec = 5;
    return o;
}

/** Runs the session until cond() or timeout (s). @return cond() */
bool pump(BrowserSession& s, const std::function<bool()>& cond, double timeout) {
    const double end = now() + timeout;
    while (!cond()) {
        const double t = now();
        if (t >= end) return false;
        std::vector<pollfd> fds;
        s.fds(fds);
        const double wait = std::clamp(std::min(s.nextWake(t), end) - t, 0.0, 0.02);
        ::poll(fds.data(), fds.size(), static_cast<int>(std::ceil(wait * 1000)));
        s.onPoll(fds, 0, now());
        s.tick(now());
    }
    return true;
}

void pumpFor(BrowserSession& s, double seconds) {
    pump(s, [] { return false; }, seconds);
}

std::string fakeLog(const SessionOptions& o) {
    std::ifstream file(o.launch.profileDir + "/fake-log.txt");
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

size_t count(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) ++n;
    return n;
}

bool gone(pid_t pid) { return pid <= 0 || (::kill(pid, 0) != 0 && errno == ESRCH); }

void testStartFramesInput() {
    SessionOptions o = optionsFor("main");
    BrowserSession s(o);
    std::string logged;
    s.log = [&](const std::string& line) {
        logged += line + "\n";
        if (std::getenv("BROWSER_TEST_VERBOSE")) std::printf("[session] %s\n", line.c_str());
    };
    CHECK(s.phase() == Phase::Off);
    const double t0 = now();
    s.open(t0);
    CHECK(s.phase() == Phase::Starting);
    CHECK(s.pid() > 0);
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    CHECK(s.port() == 0);  // the pipe (the default): no port at all
    CHECK(s.status().channel == "pipe");
    CHECK(!s.pipeFellBack());
    CHECK(pump(s, [&] { return s.status().title == "Title of https://home.example/"; }, 2));
    CHECK(s.status().url == "https://home.example/");
    CHECK(!s.status().loading);
    std::string log = fakeLog(o);
    CHECK(log.find("Target.attachToTarget - {\"targetId\":\"T1\",\"flatten\":true}") != std::string::npos);
    CHECK(log.find("Emulation.setDeviceMetricsOverride S-T1.1 {\"width\":1280,\"height\":720") != std::string::npos);
    CHECK(log.find("Page.navigate S-T1.1 {\"url\":\"https://home.example/\"}") != std::string::npos);
    CHECK(log.find("Page.startScreencast") == std::string::npos);  // not visible yet
    // Our switches: the profile, the pipe (no port, no listening socket), headless
    CHECK(logged.find("--user-data-dir=" + o.launch.profileDir) != std::string::npos);
    CHECK(logged.find("--remote-debugging-pipe") != std::string::npos);
    CHECK(logged.find("--remote-debugging-port") == std::string::npos);
    CHECK(logged.find("--user-agent=") == std::string::npos);  // no "Last Version" yet: Chrome's own
    CHECK(logged.find("DevTools connected over the pipe (FakeChrome/1.0)") != std::string::npos);
    CHECK(log.find("PIPE\n") != std::string::npos);
    CHECK(log.find("Browser.getVersion - {}") != std::string::npos);
    CHECK(!std::filesystem::exists(o.launch.profileDir + "/DevToolsActivePort"));

    // Visible: frames, acked at most fpsCap per second
    s.setVisible(true, now());
    BgraFrame frame;
    CHECK(pump(s, [&] { return s.takeFrame(frame); }, 2));
    CHECK(frame.width == 64 && frame.height == 36);
    if (frame.width == 64) {
        const uint8_t* left = &frame.pixels[(18 * 64 + 2) * 4];
        CHECK(left[2] > 200 && left[0] < 60);  // red on the left (BGRA)
        CHECK(left[3] == 255);
    }
    const uint64_t a0 = s.acks();
    const double p0 = now();
    pumpFor(s, 1.0);
    const double rate = (s.acks() - a0) / (now() - p0);
    std::printf("acks at fps cap 10: %.1f/s\n", rate);
    // The cap is an upper bound (a slow machine only lowers the rate): tight above, generous below
    CHECK(rate >= 4 && rate <= 11.5);
    o.fpsCap = 40;
    s.setOptions(o, now());
    const uint64_t a1 = s.acks();
    const double p1 = now();
    pumpFor(s, 0.5);
    const double fast = (s.acks() - a1) / (now() - p1);
    std::printf("acks at fps cap 40: %.1f/s\n", fast);
    CHECK(fast >= 15 && fast <= 42);

    // Input: a click, text, keys, a wheel turn
    s.mouse(BrowserSession::Mouse::Move, 100, 50, now());
    s.mouse(BrowserSession::Mouse::Down, 100, 50, now());
    s.mouse(BrowserSession::Mouse::Up, 100, 50, now());
    s.mouse(BrowserSession::Mouse::Down, 101, 50, now());  // a second click right after: a double click
    s.mouse(BrowserSession::Mouse::Up, 101, 50, now());
    s.mouse(BrowserSession::Mouse::Up, 101, 50, now());    // no press to end: nothing sent
    s.insertText("고양이");
    s.key(cdp::Key::Enter);
    s.key(cdp::Key::Backspace);
    s.wheel(640, 360, 0, 120);
    pumpFor(s, 0.3);
    log = fakeLog(o);
    CHECK(log.find("\"type\":\"mouseMoved\",\"x\":100,\"y\":50,\"button\":\"none\"") != std::string::npos);
    CHECK(log.find("\"type\":\"mousePressed\",\"x\":100,\"y\":50,\"button\":\"left\",\"buttons\":1,\"clickCount\":1") !=
          std::string::npos);
    CHECK(log.find("\"type\":\"mousePressed\",\"x\":101,\"y\":50,\"button\":\"left\",\"buttons\":1,\"clickCount\":2") !=
          std::string::npos);
    CHECK(count(log, "\"type\":\"mouseReleased\"") == 2);
    CHECK(log.find("Input.insertText S-T1.1 {\"text\":\"고양이\"}") != std::string::npos);
    CHECK(log.find("\"type\":\"keyDown\",\"key\":\"Enter\"") != std::string::npos);
    CHECK(log.find("\"type\":\"rawKeyDown\",\"key\":\"Backspace\"") != std::string::npos);
    CHECK(log.find("\"type\":\"mouseWheel\",\"x\":640,\"y\":360,\"deltaX\":0,\"deltaY\":120") != std::string::npos);

    // The panel's context menu: a right click at the point, the link there; its 복사 / 붙여넣기 / 전체 선택
    s.contextMenu(320, 240, 7);
    CHECK(pump(s, [&] { return s.status().contextSeq == 7; }, 2));
    CHECK(s.status().contextLink == "https://link.example/");
    s.contextMenu(5, 240, 8);
    CHECK(pump(s, [&] { return s.status().contextSeq == 8; }, 2));
    CHECK(s.status().contextLink.empty());
    s.key(cdp::Key::Copy);
    s.key(cdp::Key::Paste);
    s.key(cdp::Key::SelectAll);
    pumpFor(s, 0.3);
    log = fakeLog(o);
    CHECK(log.find("\"type\":\"mousePressed\",\"x\":320,\"y\":240,\"button\":\"right\",\"buttons\":2,\"clickCount\":1") !=
          std::string::npos);
    CHECK(log.find("\"type\":\"mouseReleased\",\"x\":320,\"y\":240,\"button\":\"right\",\"buttons\":0") !=
          std::string::npos);
    CHECK(log.find("elementFromPoint(320, 240)") != std::string::npos);
    CHECK(log.find("\"commands\":[\"copy\"]") != std::string::npos);
    CHECK(log.find("\"commands\":[\"paste\"]") != std::string::npos);
    CHECK(log.find("\"commands\":[\"selectAll\"]") != std::string::npos);

    // Navigation and history
    s.navigate("youtube.com");
    CHECK(pump(s, [&] { return s.status().url == "https://youtube.com" && s.status().canBack; }, 2));
    s.navigate("고양이 동영상");
    CHECK(pump(s, [&] { return s.status().url.find("google.com/search?q=%EA%B3%A0") != std::string::npos; }, 2));
    s.back();
    CHECK(pump(s, [&] { return s.status().url == "https://youtube.com" && s.status().canForward; }, 2));
    s.forward();
    CHECK(pump(s, [&] { return s.status().url.find("google.com/search") != std::string::npos; }, 2));
    s.home();
    CHECK(pump(s, [&] { return s.status().url == "https://home.example/"; }, 2));
    CHECK(count(fakeLog(o), "Page.navigateToHistoryEntry") == 2);

    // Hidden with keepAudio off: the screencast stops and the media pauses
    o.keepAudio = false;
    s.setOptions(o, now());
    s.setVisible(false, now());
    pumpFor(s, 0.2);
    log = fakeLog(o);
    CHECK(log.find("Page.stopScreencast S-T1") != std::string::npos);
    CHECK(log.find("m.pause()") != std::string::npos);
    const uint64_t acksHidden = s.acks();
    pumpFor(s, 0.3);
    CHECK(s.acks() == acksHidden);  // no frames while hidden

    // Idle: hidden and silent this long → Chrome goes
    o.idleShutdownSec = 0.8;
    s.setOptions(o, now());
    const pid_t pid = s.pid();
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 4));
    CHECK(gone(pid));
    CHECK(fakeLog(o).find("Browser.close") != std::string::npos);
    CHECK(findProfileProcesses(o.launch.profileDir).empty());
}

void testViewAndTranslate() {
    // 보기: the mobile version (a phone's user agent, mobile emulation) and the zoom (CSS zoom on every document);
    // 번역: the content policy bypassed and the page read again, then Google's widget; pressed again or another page:
    // off, the policy back on
    SessionOptions o = optionsFor("view");
    BrowserSession s(o);
    std::string logged;
    s.log = [&](const std::string& line) { logged += line + "\n"; };
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.setVisible(true, now());
    s.navigate("youtube.com");
    CHECK(pump(s, [&] { return s.status().url == "https://youtube.com"; }, 2));
    o.mobile = true;
    s.setOptions(o, now());
    pumpFor(s, 0.2);
    std::string log = fakeLog(o);
    CHECK(log.find("Android 14") != std::string::npos && log.find("\"mobile\":true") != std::string::npos);
    o.zoomPct = 125;
    s.setOptions(o, now());
    pumpFor(s, 0.2);
    log = fakeLog(o);
    CHECK(log.find("Page.addScriptToEvaluateOnNewDocument") != std::string::npos);
    CHECK(log.find("zoom='1.25'") != std::string::npos);
    o.zoomPct = 100;
    o.mobile = false;
    s.setOptions(o, now());
    pumpFor(s, 0.2);
    CHECK(fakeLog(o).find("zoom=''") != std::string::npos);

    s.translate();
    CHECK(s.status().translated);
    CHECK(pump(s, [&] { return logged.find("translate widget") != std::string::npos; }, 2));
    pumpFor(s, 0.2);  // the fake writes what it got
    log = fakeLog(o);
    const size_t bypass = log.find("Page.setBypassCSP S-T1.1 {\"enabled\":true}");
    const size_t reload = log.find("Page.reload", bypass);
    const size_t widget = log.find("translate_a/element.js", reload);
    CHECK(bypass != std::string::npos && reload != std::string::npos && widget != std::string::npos);
    s.translate();  // off: the original, the policy back
    CHECK(!s.status().translated);
    pumpFor(s, 0.2);
    CHECK(fakeLog(o).find("Page.setBypassCSP S-T1.1 {\"enabled\":false}") != std::string::npos);
    s.translate();
    pumpFor(s, 0.3);
    s.navigate("google.com");  // another page: off (the policy back before it is asked for)
    CHECK(!s.status().translated);
    s.translate();
    CHECK(pump(s, [&] { return count(logged, "translate widget") == 2; }, 2));
    s.reload(now());  // ⟳ on a translated page: the original
    CHECK(!s.status().translated);
    pumpFor(s, 0.3);
    CHECK(count(logged, "translate widget") == 2);  // not put in again
    // The page asks "leave this page?" before 번역's reload and the answer is no: no reload, so 번역 and the policy off
    std::ofstream(o.launch.profileDir + "/fake-reload-asks") << "1";
    s.translate();
    CHECK(pump(s, [&] { return s.status().dialogType == "beforeunload"; }, 2));
    const size_t offs = count(fakeLog(o), "Page.setBypassCSP S-T1.1 {\"enabled\":false}");
    s.answerDialog(false);
    CHECK(pump(s, [&] { return !s.status().translated; }, 2));
    pumpFor(s, 0.2);
    CHECK(count(fakeLog(o), "Page.setBypassCSP S-T1.1 {\"enabled\":false}") == offs + 1);
    std::filesystem::remove(o.launch.profileDir + "/fake-reload-asks");
    // Without the question 번역 works again, and off again
    s.translate();
    CHECK(pump(s, [&] { return count(logged, "translate widget") == 3; }, 2));
    s.translate();
    pumpFor(s, 0.2);
    // The mobile switch reads the page again (its user agent counts from the next load); desktop clears the override
    const size_t reloads = count(fakeLog(o), "Page.reload");
    o.mobile = true;
    s.setOptions(o, now());
    o.mobile = false;
    s.setOptions(o, now());
    pumpFor(s, 0.3);
    CHECK(count(fakeLog(o), "Page.reload") == reloads + 2);
    CHECK(fakeLog(o).find("Emulation.setUserAgentOverride S-T1.1 {\"userAgent\":\"\"}") != std::string::npos);
}

void testPageSizeRestarts() {
    // A new page size (720p ↔ 1080p): headless Chrome starts again, so its screen (--screen-info) matches the page and
    // a page gone fullscreen fills the panel; it comes back on the same page
    SessionOptions o = optionsFor("pagesize");
    BrowserSession s(o);
    std::string logged;
    s.log = [&](const std::string& line) { logged += line + "\n"; };
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.setVisible(true, now());
    s.navigate("youtube.com");
    CHECK(pump(s, [&] { return s.status().url == "https://youtube.com"; }, 2));
    const pid_t before = s.pid();
    o.launch.width = 1920;
    o.launch.height = 1080;
    s.setOptions(o, now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready && s.pid() != before; }, 8));
    CHECK(gone(before));
    CHECK(logged.find("starting Chrome again so its screen matches") != std::string::npos);
    CHECK(pump(s, [&] { return s.status().url == "https://youtube.com"; }, 3));
    // Chrome 끄기 while that restart is under way: it stays off
    o.launch.width = 1280;
    o.launch.height = 720;
    s.setOptions(o, now());
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 5));
    pumpFor(s, 0.5);
    CHECK(s.phase() == Phase::Off);
}

void testOffSizeFramesSetTheSizeAgain() {
    // Frames whose page isn't the size we set (the fake always says 1280×720; this page is 1108×832): after a few,
    // the size is set again (not on every frame), and the frames say what they show
    SessionOptions o = optionsFor("offsize");
    o.launch.width = 1108;
    o.launch.height = 832;
    BrowserSession s(o);
    std::string logged;
    s.log = [&](const std::string& line) { logged += line + "\n"; };
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.setVisible(true, now());
    BgraFrame frame;
    CHECK(pump(s, [&] { return s.takeFrame(frame); }, 2));
    CHECK(frame.cssWidth == 1280 && frame.cssHeight == 720);
    CHECK(pump(s, [&] { return logged.find("isn't the page size 1108x832: setting it again") != std::string::npos; },
               BrowserSession::kOffSizeResendSec + 3));
    pumpFor(s, 1.0);
    const std::string log = fakeLog(o);
    // Twice when the page was ready (pageReady, applyView), once for the frames: kOffSizeResendSec keeps it from
    // repeating within 3 s
    const size_t sent = count(log, "Emulation.setDeviceMetricsOverride S-T1.1 {\"width\":1108,\"height\":832");
    if (sent != 3) std::printf("  metrics sent %zu times\n", sent);
    CHECK(sent == 3);
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 5));
}

void testAudibleKeepsRunning() {
    // Hidden with something playing (the audible check says so): Chrome stays; silent: it goes after the idle time
    SessionOptions o = optionsFor("audible");
    o.idleShutdownSec = 0.6;
    o.audioCheckSec = 0.15;
    std::filesystem::create_directories(o.launch.profileDir);
    std::ofstream(o.launch.profileDir + "/fake-audible") << "1";
    BrowserSession s(o);
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.setVisible(true, now());
    s.setVisible(false, now());
    pumpFor(s, 1.5);
    CHECK(s.phase() == Phase::Ready);
    CHECK(count(fakeLog(o), ".some(") >= 5);
    CHECK(fakeLog(o).find("m.pause()") == std::string::npos);  // keepAudio (the default): never paused
    std::filesystem::remove(o.launch.profileDir + "/fake-audible");
    const double silentFrom = now();
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 3));
    CHECK(now() - silentFrom >= 0.4);
}

void testMediaControls() {
    // The HUD's video controls: hidden, the first check soon tells the page's video (also with the idle shutdown off);
    // play / pause and pause-all answer the new state at once; shown again, nothing (the panel has the page)
    SessionOptions o = optionsFor("media");
    o.idleShutdownSec = 0;
    o.audioCheckSec = 30;  // only the first check (kFirstAudioCheckSec after hiding) within this test
    std::filesystem::create_directories(o.launch.profileDir);
    std::ofstream(o.launch.profileDir + "/fake-audible") << "1";
    BrowserSession s(o);
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.setVisible(true, now());
    CHECK(!s.status().media.present);
    s.setVisible(false, now());
    CHECK(pump(s, [&] { return s.status().media.present; }, 3));
    CHECK(s.status().media.playing && !s.status().media.muted);
    s.media(BrowserSession::MediaCommand::PlayPause);
    CHECK(pump(s, [&] { return !s.status().media.playing; }, 2));
    CHECK(s.status().media.present);  // paused part way: the controls stay
    CHECK(fakeLog(o).find("\"userGesture\":true") != std::string::npos);
    s.media(BrowserSession::MediaCommand::PlayPause);
    CHECK(pump(s, [&] { return s.status().media.playing; }, 2));
    s.media(BrowserSession::MediaCommand::Pause);
    CHECK(pump(s, [&] { return !s.status().media.playing; }, 2));
    // Pause everything says whether it stopped something
    CHECK(pump(s, [&] { return s.status().pauseAnswers == 1; }, 2) && s.status().pausedPlaying);
    s.media(BrowserSession::MediaCommand::Pause);
    CHECK(pump(s, [&] { return s.status().pauseAnswers == 2; }, 2) && !s.status().pausedPlaying);
    // Each command's answer is told apart (Media::answer) and has the position
    const int answered = s.status().media.answer;
    CHECK(answered > 0 && s.status().media.position == 42.5 && s.status().media.duration == 300);
    s.media(BrowserSession::MediaCommand::Back);
    CHECK(pump(s, [&] { return s.status().media.answer == answered + 1; }, 2));
    s.media(BrowserSession::MediaCommand::Forward);
    s.media(BrowserSession::MediaCommand::Mute);
    pumpFor(s, 0.3);
    const std::string log = fakeLog(o);
    CHECK(log.find("m.currentTime-10") != std::string::npos && log.find("m.currentTime+10") != std::string::npos);
    // Only the presses mark the video (5 of them), not the audio checks nor pause everything
    CHECK(count(log, "m.__fdock=1") == 5);
    // The one acted on stays picked when paused at 0 s (−10초 near the start)
    CHECK(log.find("x.__fdock&&!x.ended") != std::string::npos);
    CHECK(log.find("m.muted=!m.muted") != std::string::npos);
    CHECK(s.phase() == Phase::Ready);  // the idle shutdown is off
    s.setVisible(true, now());
    CHECK(!s.status().media.present);
    // No media on the page: nothing to control
    std::filesystem::remove(o.launch.profileDir + "/fake-media-paused");
    s.setVisible(false, now());
    pumpFor(s, 1.5);
    CHECK(!s.status().media.present);
}

void testIdleHeldWhileOff() {
    // The headset off with its video paused: no idle shutdown until released, then the idle time counts from the
    // release. The checks go on, and one that hears it play again pauses it again
    SessionOptions o = optionsFor("idlehold");
    o.idleShutdownSec = 0.5;
    o.audioCheckSec = 0.1;
    std::filesystem::create_directories(o.launch.profileDir);
    std::ofstream(o.launch.profileDir + "/fake-audible") << "1";
    BrowserSession s(o);
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.setVisible(true, now());
    s.setVisible(false, now());
    CHECK(pump(s, [&] { return s.status().media.playing; }, 3));
    s.media(BrowserSession::MediaCommand::Pause);
    s.holdIdle(true, now());
    CHECK(pump(s, [&] { return s.status().media.present && !s.status().media.playing; }, 2));
    std::string logged;
    s.log = [&](const std::string& line) { logged += line + "\n"; };
    const size_t checks = count(fakeLog(o), ".some(");
    const int answered = s.status().media.answer;
    pumpFor(s, 1.2);
    CHECK(s.phase() == Phase::Ready && s.idleHeld());
    CHECK(count(fakeLog(o), ".some(") > checks);  // still checked while held
    CHECK(answered > 0 && s.status().media.answer == answered);  // the checks keep the press answers' count
    // The page starts it again by itself: paused again
    std::filesystem::remove(o.launch.profileDir + "/fake-media-paused");
    std::ofstream(o.launch.profileDir + "/fake-audible") << "1";
    CHECK(pump(s, [&] { return logged.find("pausing it again") != std::string::npos; }, 2));
    CHECK(pump(s, [&] { return s.status().media.present && !s.status().media.playing; }, 2));
    CHECK(s.phase() == Phase::Ready);
    const double released = now();
    s.holdIdle(false, now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 4));
    CHECK(now() - released >= 0.4);

    // A page without a video (text only) isn't kept by the hold: the idle shutdown goes on
    SessionOptions t = optionsFor("idlehold-text");
    t.idleShutdownSec = 0.5;
    t.audioCheckSec = 0.1;
    BrowserSession text(t);
    text.open(now());
    CHECK(pump(text, [&] { return text.phase() == Phase::Ready; }, 5));
    text.setVisible(true, now());
    text.setVisible(false, now());
    text.holdIdle(true, now());
    CHECK(pump(text, [&] { return text.phase() == Phase::Off; }, 4));

    // The HUD's controls showing a paused video keep it too (holdPaused), until released
    SessionOptions pz = optionsFor("pausedhold");
    pz.idleShutdownSec = 0.5;
    pz.audioCheckSec = 0.1;
    std::filesystem::create_directories(pz.launch.profileDir);
    std::ofstream(pz.launch.profileDir + "/fake-media-paused") << "1";
    BrowserSession paused(pz);
    paused.open(now());
    CHECK(pump(paused, [&] { return paused.phase() == Phase::Ready; }, 5));
    paused.setVisible(true, now());
    paused.setVisible(false, now());
    CHECK(pump(paused, [&] { return paused.status().media.present; }, 3));
    paused.holdPaused(true, now());
    pumpFor(paused, 1.2);
    CHECK(paused.phase() == Phase::Ready);
    const double pausedReleased = now();
    paused.holdPaused(false, now());
    CHECK(pump(paused, [&] { return paused.phase() == Phase::Off; }, 4));
    CHECK(now() - pausedReleased >= 0.4);

    // A hold that kept nothing doesn't start the idle time over when released (worn again over a text page)
    SessionOptions r = optionsFor("idlehold-release");
    r.idleShutdownSec = 0.6;
    r.audioCheckSec = 0.1;
    BrowserSession rel(r);
    rel.open(now());
    CHECK(pump(rel, [&] { return rel.phase() == Phase::Ready; }, 5));
    rel.setVisible(true, now());
    rel.setVisible(false, now());
    rel.holdIdle(true, now());
    pumpFor(rel, 0.4);
    const double releasedText = now();
    rel.holdIdle(false, now());
    CHECK(pump(rel, [&] { return rel.phase() == Phase::Off; }, 4));
    CHECK(now() - releasedText < 0.45);
}

void testAudioCheckTimeout() {
    // A page that never answers the audio check: each check times out (counted as maybe playing), later checks still
    // go out, and Chrome isn't closed until one answers "silent"
    SessionOptions o = optionsFor("audiohang");
    o.idleShutdownSec = 0.6;
    o.audioCheckSec = 0.15;
    o.audioCheckTimeoutSec = 0.3;
    std::filesystem::create_directories(o.launch.profileDir);
    std::ofstream(o.launch.profileDir + "/fake-audible-hang") << "1";
    BrowserSession s(o);
    std::string logged;
    s.log = [&](const std::string& line) { logged += line + "\n"; };
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.setVisible(true, now());
    s.setVisible(false, now());
    pumpFor(s, 1.6);
    CHECK(s.phase() == Phase::Ready);
    CHECK(count(fakeLog(o), ".some(") >= 3);  // not stuck behind the first unanswered one
    CHECK(count(logged, "counting the page as maybe playing") == 1);  // said once until one answers
    std::filesystem::remove(o.launch.profileDir + "/fake-audible-hang");
    const double answeredFrom = now();
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 4));
    CHECK(now() - answeredFrom >= 0.3);  // the idle time counts from the last unknown check

    // A page that answers none for hungShutdownSec counts as hung: closed like an idle one
    SessionOptions h = optionsFor("audiohung");
    h.idleShutdownSec = 0.6;  // longer than the first check's timeout (0.15 + 0.2 s)
    h.audioCheckSec = 0.15;
    h.audioCheckTimeoutSec = 0.2;
    h.hungShutdownSec = 1.0;
    std::filesystem::create_directories(h.launch.profileDir);
    std::ofstream(h.launch.profileDir + "/fake-audible-hang") << "1";
    BrowserSession hung(h);
    std::string hungLog;
    hung.log = [&](const std::string& line) { hungLog += line + "\n"; };
    hung.open(now());
    CHECK(pump(hung, [&] { return hung.phase() == Phase::Ready; }, 5));
    hung.setVisible(true, now());
    const double hiddenAt = now();
    hung.setVisible(false, now());
    CHECK(pump(hung, [&] { return hung.phase() == Phase::Off; }, 5));
    CHECK(now() - hiddenAt >= 1.0);
    CHECK(hungLog.find("counts as hung") != std::string::npos);
}

void testDialogs() {
    SessionOptions o = optionsFor("dialogs");
    BrowserSession s(o);
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.setVisible(true, now());
    // alert(): accepted at once, nothing to show
    s.navigate("https://alert.example/");
    CHECK(pump(s, [&] { return fakeLog(o).find("Page.handleJavaScriptDialog S-T1.1 {\"accept\":true}") != std::string::npos; }, 2));
    CHECK(s.status().dialogType.empty());
    // confirm(): waits for the panel; 취소 dismisses it
    s.navigate("https://confirm.example/");
    CHECK(pump(s, [&] { return s.status().dialogType == "confirm"; }, 2));
    CHECK(s.status().dialogMessage == "Sure?");
    pumpFor(s, 0.2);
    CHECK(count(fakeLog(o), "Page.handleJavaScriptDialog") == 1);  // not answered by itself
    s.answerDialog(false);
    CHECK(s.status().dialogType.empty());
    CHECK(pump(s, [&] { return fakeLog(o).find("Page.handleJavaScriptDialog S-T1.1 {\"accept\":false}") != std::string::npos; }, 2));
    // prompt(): 확인 answers with its default text
    s.navigate("https://prompt.example/");
    CHECK(pump(s, [&] { return s.status().dialogType == "prompt"; }, 2));
    s.answerDialog(true);
    CHECK(pump(s, [&] { return fakeLog(o).find("{\"accept\":true,\"promptText\":\"fill\"}") != std::string::npos; }, 2));
    // beforeunload while it shows, then hidden: dismissed (the page stays), nobody saw it
    s.navigate("https://beforeunload.example/");
    CHECK(pump(s, [&] { return s.status().dialogType == "beforeunload"; }, 2));
    s.setVisible(false, now());
    CHECK(s.status().dialogType.empty());
    CHECK(pump(s, [&] { return count(fakeLog(o), "{\"accept\":false}") == 2; }, 2));
    // Hidden when it opens: dismissed at once
    s.navigate("https://confirm.example/again");
    CHECK(pump(s, [&] { return count(fakeLog(o), "{\"accept\":false}") == 3; }, 2));
    CHECK(s.status().dialogType.empty());
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
}

void testCrashRestart() {
    SessionOptions o = optionsFor("crash");
    BrowserSession s(o);
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    CHECK(pump(s, [&] { return s.status().title == "Title of https://home.example/"; }, 2));
    const pid_t first = s.pid();
    s.navigate("https://crash.example/");
    CHECK(pump(s, [&] { return s.phase() == Phase::Restarting; }, 3));
    CHECK(s.status().restarts == 1);
    // The socket or the process: whichever was noticed first
    CHECK(s.status().error.find("exit 3") != std::string::npos || s.status().error.find("연결이 끊겼어요") != std::string::npos);
    CHECK(gone(first));
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    CHECK(s.pid() != first);
    // Back on the page it was on
    CHECK(pump(s, [&] { return s.status().url == "https://crash.example/"; }, 2));
    CHECK(pump(s, [&] { return count(fakeLog(o), "Page.navigate S-T1.1 {\"url\":\"https://crash.example/\"}") == 2; }, 2));
    CHECK(count(fakeLog(o), "START ") == 2);
    CHECK(s.status().error.empty());  // cleared once it is ready again
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
    CHECK(findProfileProcesses(o.launch.profileDir).empty());
}

void testCrashLoopBlamesThePage() {
    // A page that crashes Chrome every time it loads: put back once, then blamed; the next start opens home and says
    // so, instead of restarting forever (Ready alone doesn't reset the count)
    SessionOptions o = optionsFor("crashloop");
    o.restartBaseSec = 0.1;
    o.stableSec = 1.0;
    o.crashBlameSec = 3;
    std::filesystem::create_directories(o.launch.profileDir);
    std::ofstream(o.launch.profileDir + "/fake-crash-always") << "1";
    BrowserSession s(o);
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready && s.status().title == "Title of https://home.example/"; }, 5));
    s.navigate("https://crash.example/");
    CHECK(pump(s, [&] { return s.phase() == Phase::Restarting; }, 3));
    CHECK(s.failuresInARow() == 1);
    // Back up and on the crashing page again: down again at once
    CHECK(pump(s, [&] { return s.status().restarts == 2; }, 5));
    CHECK(s.failuresInARow() == 2);
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready && s.status().title == "Title of https://home.example/"; }, 5));
    CHECK(count(fakeLog(o), "{\"url\":\"https://crash.example/\"}") == 2);  // asked once, put back once: not again
    CHECK(count(fakeLog(o), "START ") == 3);
    CHECK(s.status().notice == "Chrome이 계속 종료되어 홈을 열었어요: crash.example/");
    CHECK(s.status().error.empty());
    // Ready is not yet stable: the count stays until stableSec has passed, then starts over
    CHECK(s.failuresInARow() == 2);
    CHECK(pump(s, [&] { return s.failuresInARow() == 0; }, 2));
    CHECK(s.phase() == Phase::Ready);
    CHECK(count(fakeLog(o), "START ") == 3);
    // The notice goes with the next navigation
    s.navigate("https://fine.example/");
    CHECK(s.status().notice.empty());
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
}

void testCrashAlwaysEndsFailed() {
    // A Chrome that goes down on any page: home is blamed too (about:blank next), and after kMaxRestarts restarts in a
    // row it stays Failed
    SessionOptions o = optionsFor("crashany");
    o.restartBaseSec = 0.05;
    o.crashBlameSec = 3;
    std::filesystem::create_directories(o.launch.profileDir);
    std::ofstream(o.launch.profileDir + "/fake-crash-any") << "1";
    BrowserSession s(o);
    std::string logged;
    s.log = [&](const std::string& line) { logged += line + "\n"; };
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Failed; }, 10));
    CHECK(s.status().restarts == BrowserSession::kMaxRestarts);
    CHECK(count(fakeLog(o), "START ") == BrowserSession::kMaxRestarts + 1);
    CHECK(count(fakeLog(o), "{\"url\":\"about:blank\"}") >= 1);  // home was blamed
    CHECK(logged.find("not opening that page again") != std::string::npos);
    CHECK(!s.status().error.empty());
    CHECK(findProfileProcesses(o.launch.profileDir).empty());
    // 새로고침 tries again from scratch (and fails the same way here)
    std::filesystem::remove(o.launch.profileDir + "/fake-crash-any");
    s.reload(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
}

void testReopenAfterIdleShutdown() {
    // Closed long enough for the idle shutdown: opening again comes back to the page it showed
    SessionOptions o = optionsFor("reopen");
    o.idleShutdownSec = 0.5;
    BrowserSession s(o);
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.navigate("https://page.example/");
    CHECK(pump(s, [&] { return s.status().title == "Title of https://page.example/"; }, 2));
    s.setVisible(true, now());
    s.setVisible(false, now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 4));
    s.open(now());
    s.setVisible(true, now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    CHECK(s.status().url == "https://page.example/");
    CHECK(pump(s, [&] { return count(fakeLog(o), "{\"url\":\"https://page.example/\"}") == 2; }, 2));
    CHECK(count(fakeLog(o), "{\"url\":\"https://home.example/\"}") == 1);  // only the very first start
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
}

void testPortTransport() {
    // "transport": "port": the WebSocket on 127.0.0.1, as before
    SessionOptions o = optionsFor("port");
    o.launch.transport = DevToolsTransport::Port;
    BrowserSession s(o);
    std::string logged;
    s.log = [&](const std::string& line) { logged += line + "\n"; };
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    CHECK(s.port() > 0 && s.status().channel == "port");
    CHECK(logged.find("--remote-debugging-port=0") != std::string::npos);
    CHECK(logged.find("--remote-debugging-address=127.0.0.1") != std::string::npos);
    CHECK(logged.find("--remote-debugging-pipe") == std::string::npos);
    s.setVisible(true, now());
    BgraFrame frame;
    CHECK(pump(s, [&] { return s.takeFrame(frame); }, 2));
    s.navigate("https://port.example/");
    CHECK(pump(s, [&] { return s.status().title == "Title of https://port.example/"; }, 2));
    CHECK(fakeLog(o).find("PIPE") == std::string::npos);
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
}

void testPipeFallsBackToPort() {
    // The pipe's fds don't reach Chrome (flatpak run → bwrap → zypak, unverified): whether it never answers, the pipe
    // closes, or Chrome exits at once, the next start uses the port, at once and without counting a failure, and so
    // does every later start of this session
    struct Case {
        const char* marker;
        double timeout;
        const char* why;
    };
    for (const Case c : {Case {"fake-pipe-deaf", 0.8, "Chrome이 응답하지 않아요"},
                         Case {"fake-pipe-closed", 5, "DevTools 파이프가 닫혔어요"},
                         Case {"fake-pipe-exit", 5, ""}}) {  // its exit or the pipe's end, whichever is seen first
        SessionOptions o = optionsFor(std::string("fallback-") + c.marker);
        o.startTimeoutSec = c.timeout;
        std::filesystem::create_directories(o.launch.profileDir);
        std::ofstream(o.launch.profileDir + "/" + c.marker) << "1";
        BrowserSession s(o);
        std::string logged;
        s.log = [&](const std::string& line) { logged += line + "\n"; };
        const double t0 = now();
        s.open(now());
        CHECK(s.status().channel == "pipe");
        CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, c.timeout + 5));
        const double took = now() - t0;
        std::printf("pipe → port (%s): ready after %.1f s\n", c.marker, took);
        if (c.timeout < 1) CHECK(took >= c.timeout - 0.05);
        if (c.timeout > 1) CHECK(took < 2.5);  // noticed at once, not after the start timeout
        CHECK(s.pipeFellBack());
        CHECK(s.status().channel == "port" && s.port() > 0);
        CHECK(s.status().restarts == 0 && s.failuresInARow() == 0);  // not a crash
        CHECK(logged.find("no answer over --remote-debugging-pipe (" + std::string(c.why)) != std::string::npos);
        CHECK(logged.find("Using the DevTools port on 127.0.0.1 for the rest of this run") != std::string::npos);
        CHECK(count(fakeLog(o), "START ") == 2);
        CHECK(count(fakeLog(o), "PIPE") == 1);
        // A later start (a crash) stays on the port
        if (std::string(c.marker) == "fake-pipe-deaf") {
            s.navigate("https://crash.example/");
            CHECK(pump(s, [&] { return s.phase() == Phase::Restarting; }, 3));
            CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
            CHECK(s.status().channel == "port");
            CHECK(count(fakeLog(o), "PIPE") == 1 && count(fakeLog(o), "START ") == 3);
        }
        s.stop(now());
        CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
        CHECK(findProfileProcesses(o.launch.profileDir).empty());
    }
}

void testPrivateDirs() {
    // The browser folders are owner-only before each start, also when they were made with a looser mode
    SessionOptions o = optionsFor("private/framedock/browser/profile");
    const std::string browserDir = (root / "private/framedock/browser").string();
    const std::string framedockDir = (root / "private/framedock").string();
    o.launch.privateDirs = {framedockDir, browserDir};
    std::filesystem::create_directories(o.launch.profileDir);
    for (const std::string& d : {framedockDir, browserDir, o.launch.profileDir}) ::chmod(d.c_str(), 0755);
    BrowserSession s(o);
    s.open(now());
    for (const std::string& d : {framedockDir, browserDir, o.launch.profileDir}) {
        struct stat st {};
        CHECK(::stat(d.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
    }
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
}

void testIgnoresTerm() {
    ::setenv("FAKE_CHROME_MODE", "ignore-term", 1);
    SessionOptions o = optionsFor("stubborn");
    BrowserSession s(o);
    s.open(now());
    ::unsetenv("FAKE_CHROME_MODE");
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    const pid_t pid = s.pid();
    const double t0 = now();
    s.stop(t0);
    CHECK(s.phase() == Phase::Stopping);
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 8));
    const double took = now() - t0;
    std::printf("a Chrome that ignores close and SIGTERM ended after %.1f s\n", took);
    CHECK(took >= BrowserSession::kCloseWaitSec + BrowserSession::kTermWaitSec - 0.2);
    CHECK(gone(pid));
}

void testGroupChild() {
    ::setenv("FAKE_CHROME_MODE", "spawn-child", 1);
    SessionOptions o = optionsFor("group");
    BrowserSession s(o);
    s.open(now());
    ::unsetenv("FAKE_CHROME_MODE");
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    pid_t child = -1;
    std::ifstream(o.launch.profileDir + "/fake-child.pid") >> child;
    CHECK(child > 0);
    CHECK(!gone(child));
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
    // The helper in its process group went with it (it was its child: reaped by init once killed)
    for (int i = 0; i < 50 && !gone(child); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(gone(child));
}

void testStartTimeoutAndLeftovers() {
    // A Chrome left running on our profile by an earlier run is stopped before a new start
    SessionOptions o = optionsFor("leftover");
    std::filesystem::create_directories(o.launch.profileDir);
    ::setenv("FAKE_CHROME_MODE", "no-port", 1);
    ChromeProcess stray;
    std::string error;
    CHECK(stray.start({FAKE_CHROME, "--user-data-dir=" + o.launch.profileDir}, "", error));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(findProfileProcesses(o.launch.profileDir).size() == 1);
    // A Chrome that never opens its port: the start times out and it is restarted
    o.startTimeoutSec = 0.6;
    o.launch.transport = DevToolsTransport::Port;
    BrowserSession s(o);
    std::string logged;
    s.log = [&](const std::string& line) { logged += line + "\n"; };
    s.open(now());
    ::unsetenv("FAKE_CHROME_MODE");
    CHECK(logged.find("stopped 1 leftover Chrome process") != std::string::npos);
    for (int i = 0; i < 50 && stray.running(); ++i) {
        stray.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(!stray.running());
    CHECK(pump(s, [&] { return s.phase() == Phase::Restarting; }, 3));
    CHECK(s.status().error == "Chrome이 응답하지 않아요");
    // The restart (no-port is gone from the environment now) comes up
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
}

void testFlatpakLikeLeftovers() {
    // A non-dumpable process on our profile is found by the real UID in /proc/N/status (on kernels that show such a
    // process's /proc/N as root's, stat() would have skipped it; this machine's kernel may not, see the line printed)
    SessionOptions o = optionsFor("nondumpable");
    std::filesystem::create_directories(o.launch.profileDir);
    std::ofstream(o.launch.profileDir + "/fake-nondumpable") << "1";
    ::setenv("FAKE_CHROME_MODE", "no-port", 1);
    ChromeProcess stray;
    std::string error;
    CHECK(stray.start({FAKE_CHROME, "--user-data-dir=" + o.launch.profileDir}, "", error));
    ::unsetenv("FAKE_CHROME_MODE");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    struct stat st {};
    CHECK(::stat(("/proc/" + std::to_string(stray.pid())).c_str(), &st) == 0);
    std::printf("a non-dumpable process's /proc entry is owned by UID %u here (ours %u)\n", st.st_uid, ::getuid());
    CHECK(processRealUid(stray.pid()) == static_cast<long>(::getuid()));
    CHECK(findProfileProcesses(o.launch.profileDir).size() == 1);
    // While it uses the profile its Singleton files stay; once it is gone they go
    for (const char* name : {"SingletonLock", "SingletonSocket", "SingletonCookie"}) {
        std::filesystem::create_symlink("steamdeck-2", o.launch.profileDir + "/" + name);
    }
    CHECK(removeStaleSingletons(o.launch.profileDir) == 0);
    CHECK(std::filesystem::is_symlink(o.launch.profileDir + "/SingletonLock"));
    CHECK(killProfileProcesses(o.launch.profileDir) == 1);
    for (int i = 0; i < 50 && stray.running(); ++i) {
        stray.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(removeStaleSingletons(o.launch.profileDir) == 3);
    CHECK(!std::filesystem::is_symlink(o.launch.profileDir + "/SingletonLock"));

    // Matching: an option naming the profile (or a path under it), never a bare path or a longer name
    const std::string p = "/home/u/.local/share/framedock/browser/profile";
    CHECK(namesProfile("--user-data-dir=" + p, p));
    CHECK(namesProfile("--database=" + p + "/Crash Reports", p));
    CHECK(namesProfile("--filesystem=" + p, p));
    CHECK(!namesProfile(p, p));
    CHECK(!namesProfile("--user-data-dir=" + p + "2", p));
    CHECK(!namesProfile("--user-data-dir=/home/u/.config/google-chrome", p));
    CHECK(!namesProfile("-x=" + p.substr(0, 10), p));

    // A helper outside Chrome's process group and session (a zypak child from flatpak-portal) goes when Chrome
    // stops, and a stale lock left by a killed Chrome is gone before the next start
    SessionOptions h = optionsFor("helper");
    std::filesystem::create_directories(h.launch.profileDir);
    std::ofstream(h.launch.profileDir + "/fake-detached-helper") << "1";
    std::filesystem::create_symlink("steamdeck-2", h.launch.profileDir + "/SingletonLock");
    BrowserSession s(h);
    std::string logged;
    s.log = [&](const std::string& line) { logged += line + "\n"; };
    s.open(now());
    CHECK(logged.find("removed 1 stale Singleton file(s)") != std::string::npos);
    CHECK(!std::filesystem::is_symlink(h.launch.profileDir + "/SingletonLock"));
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    pid_t helper = -1;
    for (int i = 0; i < 50 && helper <= 0; ++i) {
        std::ifstream(h.launch.profileDir + "/fake-helper.pid") >> helper;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(helper > 0);
    CHECK(::getpgid(helper) != s.pid());
    CHECK(findProfileProcesses(h.launch.profileDir).size() == 2);
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
    for (int i = 0; i < 50 && !gone(helper); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(gone(helper));
    CHECK(logged.find("left on the profile after it ended") != std::string::npos);
    std::filesystem::remove(h.launch.profileDir + "/fake-detached-helper");
}

void testFailedAfterRetries() {
    SessionOptions o = optionsFor("missing");
    o.launch.command = {"/nonexistent/chrome-for-the-test"};
    BrowserSession s(o);
    s.open(now());
    CHECK(s.phase() == Phase::Failed);
    CHECK(s.status().error.find("Chrome을 시작하지 못했어요") == 0);
    // 새로고침 out of Failed tries again (and fails again here)
    s.reload(now());
    CHECK(s.phase() == Phase::Failed);
}

void testPopup() {
    ::setenv("FAKE_CHROME_MODE", "no-page", 1);
    SessionOptions o = optionsFor("popup");
    BrowserSession s(o);
    s.open(now());
    ::unsetenv("FAKE_CHROME_MODE");
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    std::string log = fakeLog(o);
    CHECK(log.find("Target.createTarget - {\"url\":\"about:blank\"}") != std::string::npos);
    CHECK(log.find("Target.attachToTarget - {\"targetId\":\"T2\"") != std::string::npos);
    s.setVisible(true, now());
    s.navigate("https://popup.example/");
    CHECK(pump(s, [&] { return fakeLog(o).find("Page.startScreencast S-P1.2") != std::string::npos; }, 2));
    // The opener's session was let go before the popup's was attached
    log = fakeLog(o);
    CHECK(log.find("Target.detachFromTarget - {\"sessionId\":\"S-T2.1\"}") != std::string::npos);
    CHECK(log.find("Target.detachFromTarget") < log.find("Target.attachToTarget - {\"targetId\":\"P1\""));
    s.navigate("https://close.example/");
    // Back on the opener with a new session; the late detach of its old one doesn't throw it off again
    CHECK(pump(s, [&] { return fakeLog(o).find("Page.startScreencast S-T2.3") != std::string::npos; }, 2));
    pumpFor(s, 0.3);
    log = fakeLog(o);
    CHECK(count(log, "Target.attachToTarget - {\"targetId\":\"T2\"") == 2);
    CHECK(count(log, "Target.attachToTarget") == 3);
    s.navigate("https://after.example/");
    CHECK(pump(s, [&] { return s.status().title == "Title of https://after.example/"; }, 2));
    CHECK(fakeLog(o).find("Page.navigate S-T2.3 {\"url\":\"https://after.example/\"}") != std::string::npos);
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
}

void testPopupChainIsCapped() {
    // A page opening popup after popup: at most kMaxOpeners pages wait behind the one shown (the oldest is closed),
    // and an opener's media is paused when it is left
    SessionOptions o = optionsFor("popups");
    BrowserSession s(o);
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.setVisible(true, now());
    for (int i = 1; i <= 5; ++i) {
        s.navigate("https://popup.example/" + std::to_string(i));
        const std::string attached = "Target.attachToTarget - {\"targetId\":\"P" + std::to_string(i) + "\"";
        CHECK(pump(s, [&] { return fakeLog(o).find(attached) != std::string::npos && s.status().phase == Phase::Ready; }, 2));
        pumpFor(s, 0.1);
    }
    CHECK(s.openers() == static_cast<size_t>(BrowserSession::kMaxOpeners));
    const std::string log = fakeLog(o);
    CHECK(count(log, "Target.closeTarget") == 2);
    CHECK(log.find("Target.closeTarget - {\"targetId\":\"T1\"}") != std::string::npos);
    CHECK(log.find("Target.closeTarget - {\"targetId\":\"P1\"}") != std::string::npos);
    CHECK(log.find("Runtime.evaluate S-T1.1 {\"expression\":\"document.querySelectorAll('video,audio').forEach") !=
          std::string::npos);  // the first opener paused before it was left
    CHECK(count(log, "m.pause()") == 5);
    // Back through the chain: P4, P3, P2, then nothing is left (a new page)
    for (int i = 0; i < 3; ++i) {
        s.navigate("https://close.example/");
        pumpFor(s, 0.3);
    }
    CHECK(s.openers() == 0);
    CHECK(s.phase() == Phase::Ready);
    s.navigate("https://close.example/");  // the last popup: no opener left, a new page
    CHECK(pump(s, [&] { return fakeLog(o).find("Target.createTarget") != std::string::npos; }, 2));
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
}

void testRendererCrashes() {
    // A page whose renderer crashes: reloaded; again and again within a minute: about:blank with a notice. Hidden:
    // not reloaded until shown
    SessionOptions o = optionsFor("renderfail");
    BrowserSession s(o);
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.setVisible(true, now());
    s.navigate("https://renderfail.example/");
    CHECK(pump(s, [&] { return !s.status().notice.empty(); }, 3));
    // The fake logs a command when it reads it: wait for the about:blank to arrive
    CHECK(pump(s, [&] { return fakeLog(o).find("Page.navigate S-T1.1 {\"url\":\"about:blank\"}") != std::string::npos; }, 2));
    std::string log = fakeLog(o);
    CHECK(count(log, "RENDERCRASH") == 3);
    CHECK(count(log, "Page.reload") == 2);  // twice, then about:blank
    CHECK(log.find("Page.navigate S-T1.1 {\"url\":\"about:blank\"}") != std::string::npos);
    CHECK(s.status().url == "about:blank");
    CHECK(s.status().notice.find("페이지가 계속 멈춰서 빈 페이지를 열었어요: renderfail.example/") == 0);
    CHECK(s.phase() == Phase::Ready);  // Chrome itself is fine
    // Hidden: the crash isn't reloaded until the panel is shown again
    s.setVisible(false, now());
    s.navigate("https://renderfail.example/hidden");
    CHECK(pump(s, [&] { return count(fakeLog(o), "RENDERCRASH") == 4; }, 2));
    pumpFor(s, 0.3);
    CHECK(count(fakeLog(o), "Page.reload") == 2);
    s.setVisible(true, now());
    CHECK(pump(s, [&] { return count(fakeLog(o), "Page.reload") >= 3; }, 2));  // and on (it crashes each time)
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
}

void testAttachFailureInReady() {
    // A popup that can't be attached to: the session finds a page again (navigate works), not a silent dead panel
    SessionOptions o = optionsFor("attachfail");
    std::filesystem::create_directories(o.launch.profileDir);
    std::ofstream(o.launch.profileDir + "/fake-attach-fail-P1") << "1";
    BrowserSession s(o);
    std::string logged;
    s.log = [&](const std::string& line) { logged += line + "\n"; };
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.setVisible(true, now());
    s.navigate("https://popup.example/");
    CHECK(pump(s, [&] { return count(fakeLog(o), "Target.attachToTarget") == 3; }, 2));
    pumpFor(s, 0.2);
    CHECK(logged.find("couldn't attach to the page") != std::string::npos);
    CHECK(count(fakeLog(o), "Target.getTargets") == 2);
    s.navigate("https://after.example/");
    CHECK(pump(s, [&] { return fakeLog(o).find("Page.navigate S-T1.2 {\"url\":\"https://after.example/\"}") !=
                                std::string::npos; }, 2));
    CHECK(s.phase() == Phase::Ready && s.failuresInARow() == 0);
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
}

void testInputBackPressureAndPending() {
    // Inputs Chrome doesn't answer hold back further mouse moves (not presses); commands nobody reads aren't remembered
    SessionOptions o = optionsFor("inputhang");
    BrowserSession s(o);
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    pumpFor(s, 0.2);
    for (int i = 0; i < 50; ++i) s.reload(now());  // Page.reload: answered, never remembered
    CHECK(s.pendingCount() == 0);
    pumpFor(s, 0.2);
    std::ofstream(o.launch.profileDir + "/fake-input-hang") << "1";
    for (int i = 0; i < 40; ++i) s.mouse(BrowserSession::Mouse::Move, 10 + i, 10, now());
    pumpFor(s, 0.2);
    CHECK(count(fakeLog(o), "\"type\":\"mouseMoved\"") == static_cast<size_t>(BrowserSession::kMaxInputInFlight));
    s.mouse(BrowserSession::Mouse::Down, 5, 5, now());  // a press always goes
    pumpFor(s, 0.1);
    CHECK(count(fakeLog(o), "mousePressed") == 1);
    CHECK(s.pendingCount() <= static_cast<size_t>(BrowserSession::kMaxInputInFlight) + 1);
    // Answered again (the stuck inputs dropped with a new session, or answered): moves flow again
    std::filesystem::remove(o.launch.profileDir + "/fake-input-hang");
    s.navigate("https://popup.example/");  // a new session: the unanswered inputs no longer hold moves back
    CHECK(pump(s, [&] { return s.openers() == 1 && s.status().phase == Phase::Ready; }, 2));
    pumpFor(s, 0.2);
    s.mouse(BrowserSession::Mouse::Move, 99, 99, now());
    pumpFor(s, 0.1);
    CHECK(fakeLog(o).find("\"type\":\"mouseMoved\",\"x\":99") != std::string::npos);
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
}

void testOutOfViewAndFrozen() {
    // Out of view: about 2 frames a second; back in view: full rate at once. Hidden without keeping the audio: frozen,
    // no audio checks, and the idle shutdown still comes
    SessionOptions o = optionsFor("inview");
    o.fpsCap = 20;
    BrowserSession s(o);
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    s.setVisible(true, now());
    pumpFor(s, 0.5);
    s.setInView(false, now());
    pumpFor(s, 0.3);
    // Counted over 2 s (about 4 at kOutOfViewFps): never more than the slow rate allows (load only lowers it), and
    // still some (the picture isn't stopped)
    uint64_t a0 = s.acks();
    double t0 = now();
    pumpFor(s, 2.0);
    const uint64_t slowAcks = s.acks() - a0;
    const double slowSec = now() - t0;
    std::printf("acks out of view: %llu in %.2f s\n", static_cast<unsigned long long>(slowAcks), slowSec);
    CHECK(slowAcks >= 1 && slowAcks <= static_cast<uint64_t>(slowSec * BrowserSession::kOutOfViewFps) + 2);
    // Back in view: the full rate (20 here) at once; a third of it is plenty to tell it from 2 a second
    s.setInView(true, now());
    a0 = s.acks();
    t0 = now();
    pumpFor(s, 1.0);
    const double back = (s.acks() - a0) / (now() - t0);
    std::printf("acks back in view: %.1f/s\n", back);
    CHECK(back >= 6.5);

    o.keepAudio = false;
    o.idleShutdownSec = 1.0;
    o.audioCheckSec = 0.1;
    s.setOptions(o, now());
    s.setVisible(false, now());
    pumpFor(s, 0.3);
    std::string log = fakeLog(o);
    CHECK(s.frozen());
    CHECK(log.find("Page.setWebLifecycleState S-T1.1 {\"state\":\"frozen\"}") != std::string::npos);
    CHECK(log.find("m.pause()") < log.find("setWebLifecycleState"));
    s.setVisible(true, now());
    pumpFor(s, 0.2);
    CHECK(!s.frozen() && fakeLog(o).find("{\"state\":\"active\"}") != std::string::npos);
    s.setVisible(false, now());
    const double hiddenAt = now();
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 4));
    CHECK(now() - hiddenAt >= 0.9);
    CHECK(count(fakeLog(o), ".some(") == 0);  // a frozen page isn't asked
}

void testHiddenMinimized() {
    // Hidden with keepAudio (the default): the page's window is minimized (Chrome stops drawing it), restored before
    // the screencast when shown. A popup attached while hidden is minimized too. Never in window mode
    SessionOptions o = optionsFor("minimized");
    BrowserSession s(o);
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    // Opened hidden (as when it starts again while collapsed): minimized from the start
    const std::string min1 = "Browser.setWindowBounds - {\"windowId\":1,\"bounds\":{\"windowState\":\"minimized\"}}";
    CHECK(pump(s, [&] { return fakeLog(o).find(min1) != std::string::npos; }, 2));
    CHECK(s.minimized());
    CHECK(fakeLog(o).find("Browser.getWindowForTarget - {\"targetId\":\"T1\"}") != std::string::npos);
    s.setVisible(true, now());
    CHECK(!s.minimized());
    CHECK(pump(s, [&] { return fakeLog(o).find("Page.startScreencast S-T1") != std::string::npos; }, 2));
    std::string log = fakeLog(o);
    CHECK(log.find("{\"windowId\":1,\"bounds\":{\"windowState\":\"normal\"}}") < log.find("Page.startScreencast S-T1"));
    s.setVisible(false, now());
    CHECK(pump(s, [&] { return count(fakeLog(o), min1) == 2; }, 2));
    CHECK(s.minimized());
    log = fakeLog(o);
    CHECK(log.find("m.pause()") == std::string::npos && !s.frozen());
    s.navigate("https://popup.example/");
    CHECK(pump(s, [&] {
        return fakeLog(o).find("{\"windowId\":101,\"bounds\":{\"windowState\":\"minimized\"}}") != std::string::npos;
    }, 2));
    s.setVisible(true, now());
    CHECK(!s.minimized());
    CHECK(pump(s, [&] { return fakeLog(o).find("Page.startScreencast S-P1") != std::string::npos; }, 2));
    log = fakeLog(o);
    CHECK(log.find("{\"windowId\":1,\"bounds\":{\"windowState\":\"normal\"}}") != std::string::npos);
    CHECK(log.find("{\"windowId\":101,\"bounds\":{\"windowState\":\"normal\"}}") < log.find("Page.startScreencast S-P1"));
    // Shown again before the window's answer came: nothing is minimized
    s.setVisible(false, now());
    s.setVisible(true, now());
    pumpFor(s, 0.3);
    CHECK(!s.minimized());
    CHECK(count(fakeLog(o), "\"windowState\":\"minimized\"") == 3);
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));

    SessionOptions w = optionsFor("minimized-window");
    w.launch.headless = false;
    BrowserSession sw(w);
    sw.open(now());
    CHECK(pump(sw, [&] { return sw.phase() == Phase::Ready; }, 5));
    sw.setVisible(true, now());
    sw.setVisible(false, now());
    pumpFor(sw, 0.3);
    CHECK(!sw.minimized() && fakeLog(w).find("Browser.getWindowForTarget") == std::string::npos);
    sw.stop(now());
    CHECK(pump(sw, [&] { return sw.phase() == Phase::Off; }, 6));
}

void testChromeLogCapped() {
    // chrome.log: the last start's kept as chrome.log.1, and cut back within a run past its size limit
    SessionOptions o = optionsFor("logcap");
    o.logCheckSec = 0.1;
    o.logMaxBytes = 1u << 20;
    std::filesystem::create_directories(o.launch.profileDir);
    std::ofstream(o.launch.logPath) << "the last run's log\n";
    std::ofstream(o.launch.profileDir + "/fake-log-flood") << "1";
    BrowserSession s(o);
    std::string logged;
    s.log = [&](const std::string& line) { logged += line + "\n"; };
    s.open(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
    CHECK(pump(s, [&] { return logged.find("chrome.log passed") != std::string::npos; }, 3));
    // The flood is over once it answers (Ready); a cut made while it still wrote is followed by another one
    pumpFor(s, 4 * o.logCheckSec);
    CHECK(std::filesystem::file_size(o.launch.logPath) < o.logMaxBytes);
    std::ifstream kept(o.launch.logPath + ".1");
    std::string first;
    std::getline(kept, first);
    CHECK(first == "the last run's log");
    s.stop(now());
    CHECK(pump(s, [&] { return s.phase() == Phase::Off; }, 6));
}

void testKillNowReapsLater() {
    // killNow() that gives up waiting doesn't forget the child: it is reaped later, without blocking
    ChromeProcess p;
    p.killWaitSec = 0;
    std::string error;
    CHECK(p.start({"/bin/sleep", "30"}, "", error));
    const pid_t pid = p.pid();
    p.killNow();
    CHECK(!p.running());
    const double end = now() + 3;
    while (p.leftovers() > 0 && now() < end) {
        p.reapLeftovers();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(p.leftovers() == 0);
    int status = 0;
    CHECK(::waitpid(pid, &status, WNOHANG) < 0 && errno == ECHILD);  // reaped: no zombie left
}

void testWorker() {
    SessionOptions o = optionsFor("worker");
    const int wake = ::eventfd(0, EFD_CLOEXEC);
    pid_t pid = -1;
    {
        BrowserWorker worker(o, wake);
        worker.post([](BrowserSession& s, double t) {
            s.open(t);
            s.setVisible(true, t);
        });
        BgraFrame frame;
        bool ready = false, gotFrame = false;
        const double end = now() + 5;
        while (now() < end && !(ready && gotFrame)) {
            pollfd p {wake, POLLIN, 0};
            ::poll(&p, 1, 100);
            uint64_t v = 0;
            if (p.revents & POLLIN) (void)!::read(wake, &v, sizeof(v));
            ready = worker.status().phase == Phase::Ready;
            gotFrame = gotFrame || worker.takeFrame(frame);
        }
        CHECK(ready);
        CHECK(gotFrame && frame.width == 64);
        CHECK(worker.statusVersion() > 0);
        pid = worker.pid();
        CHECK(pid > 0);
    }  // the destructor closes Chrome and joins
    CHECK(gone(pid));
    CHECK(findProfileProcesses(o.launch.profileDir).empty());
    ::close(wake);
}

void testWorkerWithoutEventfd() {
    // No eventfd: a pipe wakes the thread instead, and the worker works as usual
    SessionOptions o = optionsFor("worker-pipe");
    BrowserWorker::failWakeForTest = 1;
    pid_t pid = -1;
    {
        BrowserWorker worker(o, -1);
        BrowserWorker::failWakeForTest = 0;
        CHECK(worker.started());
        worker.post([](BrowserSession& s, double t) { s.open(t); });
        const double end = now() + 5;
        while (now() < end && worker.status().phase != Phase::Ready) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(worker.status().phase == Phase::Ready);
        pid = worker.pid();
        // Many posts in a row (more wakes than a pipe holds unread) neither block nor get lost
        std::atomic<int> ran {0};  // incremented on the worker's thread, read here
        for (int i = 0; i < 20000; ++i) worker.post([&ran](BrowserSession&, double) { ++ran; });
        const double end2 = now() + 5;
        while (now() < end2 && ran < 20000) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(ran == 20000);
        const double t0 = now();
        worker.shutdown(5);
        CHECK(now() - t0 < 4);
    }
    CHECK(gone(pid));
    // Neither: no thread, the status says why, and shutting down returns at once (no join on a sleeping thread)
    BrowserWorker::failWakeForTest = 2;
    {
        BrowserWorker worker(optionsFor("worker-none"), -1);
        BrowserWorker::failWakeForTest = 0;
        CHECK(!worker.started());
        CHECK(worker.status().phase == Phase::Failed);
        CHECK(worker.status().error.find("브라우저 스레드를 시작하지 못했어요") == 0);
        CHECK(worker.statusVersion() > 0);
        std::atomic<bool> ran {false};
        worker.post([&ran](BrowserSession&, double) { ran = true; });
        const double t0 = now();
        worker.shutdown(5);
        CHECK(now() - t0 < 0.5);
        CHECK(!ran);
        CHECK(worker.pid() <= 0);
    }
}

void testUserAgentFromLastVersion() {
    SessionOptions o = optionsFor("ua");
    std::filesystem::create_directories(o.launch.profileDir);
    std::ofstream(o.launch.profileDir + "/Last Version") << "156.0.8078.47";
    for (const bool headless : {true, false}) {
        o.launch.headless = headless;
        BrowserSession s(o);
        std::string logged;
        s.log = [&](const std::string& line) { logged += line + "\n"; };
        s.open(now());
        CHECK(pump(s, [&] { return s.phase() == Phase::Ready; }, 5));
        // Headless: Chrome's ordinary string for that version. A window says Chrome anyway: nothing added
        CHECK((logged.find("--user-agent=Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                           "Chrome/156.0.0.0 Safari/537.36") != std::string::npos) == headless);
        CHECK(logged.find("--disable-blink-features=AutomationControlled") != std::string::npos);
        s.shutdownBlocking(5);
    }
}

void testArgv() {
    ChromeLaunch l;
    l.profileDir = "/home/u/.local/share/framedock/browser/profile";
    l.width = 1280;
    l.height = 720;
    const std::vector<std::string> a = chromeArgv(l);
    CHECK(a.size() > 8);
    CHECK(a[0] == "flatpak" && a[1] == "run" && a[2] == "--die-with-parent");
    CHECK(a[3] == "--filesystem=/home/u/.local/share/framedock/browser/profile");
    CHECK(a[4] == "com.google.Chrome");
    const auto has = [&](const std::string& s) { return std::find(a.begin(), a.end(), s) != a.end(); };
    CHECK(has("--user-data-dir=/home/u/.local/share/framedock/browser/profile"));
    CHECK(has("--remote-debugging-pipe"));  // the default: nothing listens
    CHECK(!has("--remote-debugging-port=0"));
    CHECK(!has("--remote-debugging-address=127.0.0.1"));
    CHECK(!has("--password-store=basic"));
    CHECK(has("--headless=new"));
    CHECK(has("--window-size=1280,720"));
    CHECK(has("--screen-info={0,0 1280x720}"));  // fullscreen takes the page's size, not headless Chrome's 800×600
    CHECK(!has("--mute-audio"));
    CHECK(has("--disable-blink-features=AutomationControlled"));  // navigator.webdriver false
    CHECK(has("--disk-cache-size=67108864"));  // the HTTP cache capped
    // No Optimization Guide model downloads into the profile
    CHECK(std::any_of(a.begin(), a.end(), [](const std::string& s) {
        return s.rfind("--disable-features=", 0) == 0 && s.find(",OptimizationGuideModelDownloading,") != std::string::npos;
    }));
    CHECK(std::none_of(a.begin(), a.end(), [](const std::string& s) { return s.rfind("--user-agent=", 0) == 0; }));
    CHECK(a.back() == "about:blank");
    // The user agent of a Chrome that isn't headless, from the profile's "Last Version"
    CHECK(chromeUserAgent("156.0.8078.47") ==
          "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/156.0.0.0 Safari/537.36");
    CHECK(chromeUserAgent("").empty() && chromeUserAgent("abc").empty() && chromeUserAgent("156").empty() &&
          chromeUserAgent("1234567.1").empty());
    ChromeLaunch ua = l;
    ua.userAgent = chromeUserAgent("157.0.1.2");
    ua.extraArgs = {"--user-agent=mine"};  // extra_args come later: Chrome takes the last one
    const std::vector<std::string> uv = chromeArgv(ua);
    const auto at = [&](const std::string& s) { return std::find(uv.begin(), uv.end(), s) - uv.begin(); };
    CHECK(at("--user-agent=" + ua.userAgent) < at("--user-agent=mine"));
    ChromeLaunch port = l;
    port.transport = DevToolsTransport::Port;
    const std::vector<std::string> pa = chromeArgv(port);
    CHECK(std::find(pa.begin(), pa.end(), "--remote-debugging-port=0") != pa.end());
    CHECK(std::find(pa.begin(), pa.end(), "--remote-debugging-address=127.0.0.1") != pa.end());
    CHECK(std::find(pa.begin(), pa.end(), "--remote-debugging-pipe") == pa.end());
    l.headless = false;
    l.command = splitCommand("/usr/bin/chromium --enable-features=VaapiVideoDecoder");
    l.extraArgs = {"--ignore-gpu-blocklist"};
    const std::vector<std::string> b = chromeArgv(l);
    CHECK(b[0] == "/usr/bin/chromium" && b[1] == "--enable-features=VaapiVideoDecoder");
    CHECK(std::find(b.begin(), b.end(), "--headless=new") == b.end());
    CHECK(std::find(b.begin(), b.end(), "--window-position=-32000,-32000") != b.end());
    CHECK(std::find(b.begin(), b.end(), "--ignore-gpu-blocklist") != b.end());
    CHECK(std::find(b.begin(), b.end(), "--die-with-parent") == b.end());
    CHECK(splitCommand("flatpak run 'com.google.Chrome' \"a b\"") ==
          (std::vector<std::string> {"flatpak", "run", "com.google.Chrome", "a b"}));
    CHECK(isFlatpakRun(splitCommand("/usr/bin/flatpak run com.google.Chrome")));
    CHECK(!isFlatpakRun(splitCommand("flatpak info com.google.Chrome")));
    CHECK(!isFlatpakRun(splitCommand("google-chrome")));
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/framedock-browser-test-XXXXXX";
    const char* base = std::getenv("FRAMEDOCK_TEST_TMP");
    std::string dir;
    if (base != nullptr && *base != '\0') {
        dir = std::string(base) + "/browser-session-" + std::to_string(::getpid());
        std::filesystem::create_directories(dir);
    } else {
        dir = ::mkdtemp(tmpl);
    }
    root = dir;
    ::unsetenv("FAKE_CHROME_MODE");
    testArgv();
    testUserAgentFromLastVersion();
    testStartFramesInput();
    testAudibleKeepsRunning();
    testMediaControls();
    testIdleHeldWhileOff();
    testAudioCheckTimeout();
    testPageSizeRestarts();
    testOffSizeFramesSetTheSizeAgain();
    testViewAndTranslate();
    testDialogs();
    testCrashRestart();
    testCrashLoopBlamesThePage();
    testCrashAlwaysEndsFailed();
    testReopenAfterIdleShutdown();
    testPortTransport();
    testPipeFallsBackToPort();
    testPrivateDirs();
    testIgnoresTerm();
    testGroupChild();
    testStartTimeoutAndLeftovers();
    testFlatpakLikeLeftovers();
    testFailedAfterRetries();
    testPopup();
    testPopupChainIsCapped();
    testRendererCrashes();
    testAttachFailureInReady();
    testInputBackPressureAndPending();
    testOutOfViewAndFrozen();
    testHiddenMinimized();
    testChromeLogCapped();
    testKillNowReapsLater();
    testWorker();
    testWorkerWithoutEventfd();
    // Nothing of any fake Chrome is left
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_directory()) CHECK(findProfileProcesses(entry.path().string()).empty());
    }
    std::filesystem::remove_all(root);
    if (failures == 0) std::printf("browser-session: all passed\n");
    return failures == 0 ? 0 : 1;
}
