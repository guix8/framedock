// The browser panel's Chrome session. See browser_session.h.
#include "browser_session.h"

#include "app_text.h"
#include "draw.h"  // utf8Prefix

#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kPortCheckSec = 0.1;
constexpr size_t kMoveBackPressure = 512 * 1024;  ///< mouse moves are dropped while this much waits to go out

// The audio check and the video controls: one script around an action on the page's video: the one they acted on
// (marked by a press of the HUD's buttons, mark) if it plays, else one playing with its sound on, else one playing unmuted, else the one they acted on if paused and
// not ended (even at 0 s: −10초 near the start), else one paused part way unmuted (an ended one is none). Only the one
// picked keeps the mark. Muted autoplay (previews, background loops) is never picked by itself. It answers
// "<audible 1/0><none n / playing r / paused p><muted m / -> <position s> <duration s>"; audible is any element
// playing with its sound on, the duration "Infinity" for a live stream or "NaN" before it's known. An action that sets
// `was` (pause everything) adds " w1" / " w0": whether the video it picked played before it (`was` is the script's
// own, never a page global of that name).
std::string mediaScript(const char* action, bool mark) {
    return std::string("(()=>{let was;try{const ms=[...document.querySelectorAll('video,audio')];"
                       "const part=x=>x.currentTime>0&&!x.ended;"
                       "const m=ms.find(x=>x.__fdock&&!x.paused)||ms.find(x=>!x.paused&&!x.muted&&x.volume>0)||"
                       "ms.find(x=>!x.paused&&!x.muted)||ms.find(x=>x.__fdock&&!x.ended)||"
                       "ms.find(x=>part(x)&&!x.muted)||null;"
                       "ms.forEach(x=>{if(x!==m)delete x.__fdock});") +
           (mark ? "if(m)m.__fdock=1;" : "") + action +
           "return (ms.some(x=>!x.paused&&!x.muted&&x.volume>0)?'1':'0')+(!m?'n':m.paused?'p':'r')+"
           "(m&&m.muted?'m':'-')+' '+(m?m.currentTime:-1)+' '+(m?m.duration:NaN)+"
           "(was===undefined?'':was?' w1':' w0')}catch(e){return '0n-'}})()";
}
const std::string kAudibleScript = mediaScript("", false);

}  // namespace

std::string translateScript(const std::string& target) {
    // Google's website translate widget (hidden, its banner too) set to the target as soon as it is ready
    const std::string to = "'" + target + "'";
    return "(()=>{if(window.__fdockTr)return;window.__fdockTr=1;"
           "const d=document.createElement('div');d.id='__fdock_tr';d.style.display='none';"
           "document.body.appendChild(d);"
           "const st=document.createElement('style');st.textContent='.goog-te-banner-frame,.skiptranslate iframe,"
           "#goog-gt-tt,.goog-te-balloon-frame{display:none!important}body{top:0!important}';"
           "document.head.appendChild(st);"
           "window.__fdockTrInit=()=>{new google.translate.TranslateElement({pageLanguage:'auto',includedLanguages:" +
           to +
           ",autoDisplay:false},'__fdock_tr');let n=0;const pick=()=>{const c=document.querySelector('.goog-te-combo');"
           "if(!c||!c.options.length){if(++n<50)setTimeout(pick,200);return}c.value=" +
           to +
           ";c.dispatchEvent(new Event('change'))};pick()};"
           "const s=document.createElement('script');"
           "s.src='https://translate.google.com/translate_a/element.js?cb=__fdockTrInit';"
           "document.head.appendChild(s)})()";
}

const char* chromeLocale(Language language) {
    switch (language) {
        case Language::En: return "en-US";
        case Language::Ja: return "ja-JP";
        case Language::Ko: break;
    }
    return "ko-KR";
}

const char* translateTarget(Language language) {
    switch (language) {
        case Language::En: return "en";
        case Language::Ja: return "ja";
        case Language::Ko: break;
    }
    return "ko";
}

namespace {

/** The page's zoom as CSS zoom on the root element (fullscreen still fills the window); 100: none */
std::string zoomScript(int pct) {
    const std::string value = pct == 100 ? "''" : "'" + std::to_string(pct / 100.0).substr(0, 4) + "'";
    return "(()=>{const a=()=>{if(document.documentElement)document.documentElement.style.zoom=" + value +
           "};a();document.addEventListener('readystatechange',a)})()";
}

/** A phone's user agent made from the desktop one (the same Chrome version) */
std::string mobileUserAgent(const std::string& desktop) {
    std::string ua = desktop.empty()
                         ? "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 "
                           "Safari/537.36"
                         : desktop;
    const auto swap = [&](const std::string& from, const std::string& to) {
        const size_t at = ua.find(from);
        if (at != std::string::npos) ua.replace(at, from.size(), to);
    };
    swap("(X11; Linux x86_64)", "(Linux; Android 14; Pixel 8)");
    swap("(X11; Linux aarch64)", "(Linux; Android 14; Pixel 8)");
    swap(" Safari/", " Mobile Safari/");
    return ua;
}
const char* kPauseScript = "document.querySelectorAll('video,audio').forEach(m=>{try{m.pause()}catch(e){}})";

double steadyNow() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/** mkdir -p, owner only */
void makeDirs(const std::string& path) {
    std::string partial;
    std::stringstream parts(path);
    std::string part;
    if (!path.empty() && path[0] == '/') partial = "/";
    while (std::getline(parts, part, '/')) {
        if (part.empty()) continue;
        partial += part;
        ::mkdir(partial.c_str(), 0700);
        partial += '/';
    }
}

std::string jsonText(const JsonValue* v) { return v != nullptr && v->isString() ? v->text : std::string(); }

}  // namespace

const char* phaseName(BrowserSession::Phase phase) {
    switch (phase) {
        case BrowserSession::Phase::Off: return "off";
        case BrowserSession::Phase::Starting: return "starting";
        case BrowserSession::Phase::Connecting: return "connecting";
        case BrowserSession::Phase::Attaching: return "attaching";
        case BrowserSession::Phase::Ready: return "ready";
        case BrowserSession::Phase::Stopping: return "stopping";
        case BrowserSession::Phase::Restarting: return "restarting";
        case BrowserSession::Phase::Failed: return "failed";
    }
    return "?";
}

BrowserSession::BrowserSession(SessionOptions options) : options_(std::move(options)) {}

BrowserSession::~BrowserSession() {
    closeChannels();
    process_.killNow();
}

void BrowserSession::say(const std::string& line) const {
    if (log) log(line);
}

void BrowserSession::setPhase(Phase phase, double now) {
    if (phase == status_.phase) return;
    say(std::string("phase ") + phaseName(status_.phase) + " -> " + phaseName(phase));
    status_.phase = phase;
    phaseSince_ = now;
    if (phase != Phase::Ready) clearMedia();
    if (phase == Phase::Off || phase == Phase::Failed) {
        // No page: the decoder's buffer goes (the next frame allocates it again)
        releasePixels(frame_);
        frameNew_ = false;
    }
}

void BrowserSession::setOptions(const SessionOptions& options, double now) {
    offSizeFrames_ = offSizeResends_ = 0;  // frames of the old size may still come: counted from the next one
    const bool sizeChanged = options.launch.width != options_.launch.width || options.launch.height != options_.launch.height;
    const bool zoomChanged = options.zoomPct != options_.zoomPct;
    const bool mobileChanged = options.mobile != options_.mobile;
    const bool viewChanged = zoomChanged || mobileChanged;
    options_ = options;
    const bool running = status_.phase != Phase::Off && status_.phase != Phase::Failed && status_.phase != Phase::Stopping;
    if (sizeChanged && running && options_.launch.headless) {
        // Headless Chrome's screen (--screen-info) is set at its start: started again at the new size, on this page
        say("the page size changed: starting Chrome again so its screen matches (fullscreen)");
        stop(now);
        if (status_.phase == Phase::Off) {
            open(now);
        } else {
            reopenAfterStop_ = true;
        }
    } else if (status_.phase == Phase::Ready && sizeChanged && !sessionId_.empty()) {
        if (viewChanged) applyView(zoomChanged);
        send("Emulation.setDeviceMetricsOverride",
             cdp::metricsParams(options_.launch.width, options_.launch.height, options_.mobile), true);
        if (screencasting_) {
            stopScreencast();
            startScreencast();
        }
    } else if (status_.phase == Phase::Ready && viewChanged && !sessionId_.empty()) {
        applyView(zoomChanged);
        // The user agent counts from the next load: this page read again as the mobile (desktop) site
        if (mobileChanged) send("Page.reload", "{}", true);
    }
    if (!visible_) nextAudioCheck_ = std::min(nextAudioCheck_, now + options_.audioCheckSec);
    // 닫으면 소리 changed while hidden: a kept page wakes up, a page no longer kept is paused and frozen
    if (!visible_ && status_.phase == Phase::Ready) {
        if (options_.keepAudio) {
            setFrozen(false);
            if (minimizedWindows_.empty()) minimizeWindow();
        } else if (!frozen_) {
            send("Runtime.evaluate", cdp::evaluateParams(kPauseScript), true);
            setFrozen(true);
        }
    }
}

void BrowserSession::open(double now) {
    switch (status_.phase) {
        case Phase::Off:
        case Phase::Failed:
            if (status_.phase == Phase::Failed) blamedUrl_.clear();  // a retry by hand: every page gets a chance
            failures_ = 0;
            status_.error.clear();
            status_.notice.clear();
            startProcess(now);
            break;
        case Phase::Stopping:  // asked again while closing: finish closing at once, then start fresh
            closeChannels();
            process_.killNow();
            setPhase(Phase::Off, now);
            failures_ = 0;
            startProcess(now);
            break;
        default: break;  // starting, running or about to restart
    }
}

void BrowserSession::startProcess(double now) {
    closeChannels();
    pending_.clear();
    inputInFlight_ = 0;
    targetId_.clear();
    sessionId_.clear();
    openerStack_.clear();
    pageCrashes_.clear();
    reloadWhenShown_ = false;
    attachRetries_ = 0;
    frozen_ = false;
    minimizedWindows_.clear();  // a new Chrome: new windows
    headlessRun_ = options_.launch.headless;
    screencasting_ = ackPending_ = false;
    audioCheckPending_ = false;  // its answer can't come any more
    clearDialog();
    port_ = 0;
    const std::string& profile = options_.launch.profileDir;
    if (profile.empty()) {
        fail(tr(Tx::ChromeNoProfile), now);
        return;
    }
    makeDirs(profile);
    // Owner only, also folders made earlier with a looser mode (the profile holds cookies and logins)
    for (const std::string& dir : options_.launch.privateDirs) {
        if (makePrivateDir(dir)) say("made " + dir + " owner-only (0700)");
    }
    if (makePrivateDir(profile)) say("made the profile owner-only (0700)");
    // A Chrome left over on this profile (Frame Dock died without its death signal) would take the new start's
    // place and exit it at once
    const int leftovers = killProfileProcesses(profile);
    if (leftovers > 0) say("stopped " + std::to_string(leftovers) + " leftover Chrome process(es) on our profile");
    // A Chrome killed without its clean exit leaves its singleton lock; under flatpak it names a PID inside the old
    // sandbox that the new one may reuse, and Chrome would take the profile for in use
    const int singletons = removeStaleSingletons(profile);
    if (singletons > 0) say("removed " + std::to_string(singletons) + " stale Singleton file(s) from the profile");
    ::unlink((profile + "/DevToolsActivePort").c_str());
    ChromeLaunch launch = options_.launch;
    launch.locale = chromeLocale(uiLanguage());  // Chrome's words and the pages' language: the app's, at each start
    if (launch.headless && launch.userAgent.empty()) {
        // Chrome's ordinary user agent (not HeadlessChrome) for the version its last run wrote. The very first run
        // has no such file yet and goes out as HeadlessChrome; after an update the first run names the old version
        std::ifstream file(profile + "/Last Version");
        std::string version;
        std::getline(file, version);
        launch.userAgent = chromeUserAgent(version);
    }
    desktopUserAgent_ = launch.userAgent;
    pipeRun_ = launch.transport == DevToolsTransport::Pipe && !pipeFellBack_;
    int pair[2] = {-1, -1};
    if (pipeRun_ && ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
        say(std::string("socketpair: ") + std::strerror(errno) + ": using the DevTools port");
        pipeRun_ = false;
        pipeFellBack_ = true;
    }
    launch.transport = pipeRun_ ? DevToolsTransport::Pipe : DevToolsTransport::Port;
    const std::vector<std::string> argv = chromeArgv(launch);
    std::string line = "starting:";
    for (const std::string& a : argv) line += " " + a;
    say(line);
    std::string error;
    const bool started = process_.start(argv, options_.launch.logPath, error, pipeRun_ ? pair[1] : -1);
    if (pair[1] >= 0) ::close(pair[1]);  // Chrome has it now (as its fds 3 and 4)
    if (!started) {
        if (pair[0] >= 0) ::close(pair[0]);
        fail(trf(Tx::ChromeStartFailed, {error}), now);
        return;
    }
    say("Chrome PID " + std::to_string(process_.pid()) + (pipeRun_ ? ", DevTools over the pipe" : ", DevTools over a port"));
    nextLogCheck_ = now + options_.logCheckSec;
    status_.channel = pipeRun_ ? "pipe" : "port";
    setPhase(Phase::Starting, now);
    phaseSince_ = now;  // also when it was Starting already (the pipe's fallback): the timeout counts from this start
    if (pipeRun_) {
        // Waits in the socket until Chrome reads its fd 3: the answer says the pipe reached it
        pipe_.attach(pair[0]);
        channel_ = &pipe_;
        send("Browser.getVersion", "{}", false, Pending::Probe);
    } else {
        channel_ = &socket_;
        nextPortCheck_ = now + kPortCheckSec;
    }
}

void BrowserSession::closeChannels() {
    socket_.close();
    pipe_.close();
}

void BrowserSession::fallBackToPort(const std::string& why, double now) {
    say("no answer over --remote-debugging-pipe (" + why +
        "): the fds probably didn't reach Chrome. Using the DevTools port on 127.0.0.1 for the rest of this run");
    pipeFellBack_ = true;
    pipe_.close();
    process_.killNow();
    sweepProfile();
    startProcess(now);
}

void BrowserSession::connected(double now) {
    setPhase(Phase::Attaching, now);
    send("Target.setDiscoverTargets", "{\"discover\":true}", false);
    send("Target.getTargets", "{}", false, Pending::GetTargets);
}

void BrowserSession::sweepProfile() {
    // Chrome's helpers outside its process group (a flatpak's zypak children come from flatpak-portal) that
    // outlived it
    const int left = killProfileProcesses(options_.launch.profileDir);
    if (left > 0) say("stopped " + std::to_string(left) + " Chrome process(es) left on the profile after it ended");
}

void BrowserSession::fail(const std::string& why, double now) {
    say("failed: " + why);
    closeChannels();
    const bool hadProcess = process_.running();
    process_.killNow();
    if (hadProcess) sweepProfile();
    status_.error = why;
    screencasting_ = ackPending_ = false;
    setPhase(Phase::Failed, now);
}

void BrowserSession::rememberPage() {
    const std::string& url = status_.url;
    if (!url.empty() && url != "about:blank" && url != blamedUrl_) restoreUrl_ = url;
}

void BrowserSession::lostChrome(const std::string& why, double now) {
    if (pipeRun_ && status_.phase == Phase::Starting && !pipe_.heardFrom()) {
        fallBackToPort(why, now);
        return;
    }
    say("lost Chrome: " + why);
    if (!suspectUrl_.empty() && now - suspectSince_ <= options_.crashBlameSec) {
        // It went down right after the page came back: that page is the likely cause, don't open it again
        say("it crashed " + std::to_string(static_cast<int>(now - suspectSince_)) + " s after reopening " +
            suspectUrl_ + ": not opening that page again");
        blamedUrl_ = suspectUrl_;
        blamedJustNow_ = true;
        restoreUrl_.clear();
    } else {
        rememberPage();
    }
    suspectUrl_.clear();
    closeChannels();
    process_.killNow();
    sweepProfile();
    pending_.clear();
    inputInFlight_ = 0;
    targetId_.clear();
    sessionId_.clear();
    frozen_ = false;
    screencasting_ = ackPending_ = false;
    ++failures_;
    if (failures_ > kMaxRestarts) {
        fail(why, now);
        return;
    }
    status_.error = why;
    ++status_.restarts;
    restartAt_ = now + options_.restartBaseSec * static_cast<double>(1 << (failures_ - 1));
    setPhase(Phase::Restarting, now);
}

void BrowserSession::setVisible(bool visible, double now) {
    if (visible == visible_) return;
    visible_ = visible;
    if (visible) {
        clearMedia();  // not checked while shown: the HUD's controls go (the panel has the page)
        if (status_.phase == Phase::Ready) {
            setFrozen(false);
            restoreWindows();  // before the screencast: a minimized page sends no frames
            // A renderer crash while hidden is reloaded now that someone sees it
            if (reloadWhenShown_) recoverCrashedPage(now);
            startScreencast();
        }
        return;
    }
    hiddenSince_ = now;
    lastAudibleAt_ = -1e9;
    audioUnknown_ = false;
    unansweredSince_ = -1;
    nextAudioCheck_ = now + std::min(kFirstAudioCheckSec, options_.audioCheckSec);
    if (status_.phase == Phase::Ready) {
        // Nobody can answer a dialog on a hidden panel: cancel it (and the page's script runs on, audio checks too)
        if (!status_.dialogType.empty()) {
            say("hidden with a " + status_.dialogType + " dialog open: dismissed");
            answerDialog(false);
        }
        stopScreencast();
        if (!options_.keepAudio) {
            // Paused, then frozen: no timers, no rendering, no work at all until it is shown again
            send("Runtime.evaluate", cdp::evaluateParams(kPauseScript), true);
            setFrozen(true);
        } else {
            // Kept playing: minimized, so Chrome stops drawing it (and decoding a video's picture)
            minimizeWindow();
        }
    }
}

void BrowserSession::minimizeWindow() {
    if (!headlessRun_ || targetId_.empty()) return;
    send("Browser.getWindowForTarget", cdp::windowForTargetParams(targetId_), false, Pending::Window);
}

void BrowserSession::restoreWindows() {
    for (const int window : minimizedWindows_) send("Browser.setWindowBounds", cdp::windowStateParams(window, false), false);
    minimizedWindows_.clear();
}

void BrowserSession::setFrozen(bool frozen) {
    if (frozen == frozen_ || sessionId_.empty()) return;
    if (send("Page.setWebLifecycleState", cdp::lifecycleParams(frozen), true) != 0 || !frozen) frozen_ = frozen;
    if (frozen_) say("hidden without keeping its audio: the page is frozen until shown");
}

void BrowserSession::setInView(bool inView, double now) {
    if (inView == inView_) return;
    inView_ = inView;
    say(inView ? "back in view: full frame rate" : "out of view: " + std::to_string(static_cast<int>(kOutOfViewFps)) +
                                                     " frames per second");
    // Back in view: a frame held back for the slow rate is acked now (the next one comes at once)
    if (inView && ackPending_) {
        ackDueAt_ = now;
        ackNow(now);
    } else if (ackPending_) {
        ackDueAt_ = std::max(ackDueAt_, lastAckAt_ + ackInterval());
    }
}

double BrowserSession::ackInterval() const {
    return inView_ ? 1.0 / std::max(1, options_.fpsCap) : 1.0 / kOutOfViewFps;
}

void BrowserSession::stop(double now) {
    reopenAfterStop_ = false;  // a stop asked for (Chrome 끄기, the idle shutdown) stays stopped
    switch (status_.phase) {
        case Phase::Off:
        case Phase::Stopping: return;
        case Phase::Failed:
        case Phase::Restarting:
            process_.killNow();
            closeChannels();
            setPhase(Phase::Off, now);
            return;
        default: break;
    }
    rememberPage();  // opened again (after the idle shutdown, Chrome 끄기): back on this page
    suspectUrl_.clear();
    screencasting_ = ackPending_ = false;
    termSent_ = killSent_ = false;
    stopAt_ = now;
    if (channel_->open()) {
        send("Browser.close", "{}", false);  // a clean exit: the profile is written and closed properly
    } else {
        process_.signalGroup(SIGTERM);
        termSent_ = true;
        stopAt_ = now - kCloseWaitSec;  // SIGKILL kTermWaitSec from now
    }
    setPhase(Phase::Stopping, now);
}

void BrowserSession::shutdownBlocking(double timeoutSec) {
    const double start = steadyNow();
    stop(start);
    while (process_.running() && steadyNow() - start < timeoutSec) {
        std::vector<pollfd> list;
        fds(list);
        const double now = steadyNow();
        const double wait = std::clamp(nextWake(now) - now, 0.0, 0.05);
        ::poll(list.data(), list.size(), static_cast<int>(std::ceil(wait * 1000)));
        onPoll(list, 0, steadyNow());
        tick(steadyNow());
    }
    closeChannels();
    if (process_.running()) {
        say("still running after " + std::to_string(timeoutSec) + " s: SIGKILL");
        process_.killNow();
    }
    if (!options_.launch.profileDir.empty()) sweepProfile();
    setPhase(Phase::Off, steadyNow());
}

int BrowserSession::send(const std::string& method, const std::string& params, bool page, Pending kind) {
    if (!channel_->open()) return 0;
    if (page && sessionId_.empty()) return 0;
    int id = 0;
    const std::string text = writer_.command(method, params, page ? sessionId_ : std::string(), id);
    if (!channel_->sendText(text)) return 0;
    if (kind == Pending::Other) return id;  // nobody reads its answer: not remembered (pending_ stays small)
    if (pending_.size() >= kMaxPending) {
        // Chrome stopped answering some of them: the oldest are forgotten (their answers are ignored if they come)
        say("more than " + std::to_string(kMaxPending) + " commands unanswered: forgetting the oldest");
        forget(pending_.begin());
    }
    pending_[id] = kind;
    if (kind == Pending::Input) ++inputInFlight_;
    return id;
}

void BrowserSession::forget(std::map<int, Pending>::iterator it) {
    if (it->second == Pending::Input) --inputInFlight_;
    if (it->second == Pending::Audible && it->first == audioCheckId_) audioCheckPending_ = false;
    pending_.erase(it);
}

void BrowserSession::fds(std::vector<pollfd>& out) const {
    if (channel_->fd() >= 0) {
        out.push_back({channel_->fd(), static_cast<short>(POLLIN | (channel_->wantsWrite() ? POLLOUT : 0)), 0});
    }
    if (process_.exitFd() >= 0) out.push_back({process_.exitFd(), POLLIN, 0});
}

void BrowserSession::onPoll(const std::vector<pollfd>& list, size_t first, double now) {
    for (size_t i = first; i < list.size(); ++i) {
        const pollfd& p = list[i];
        if (p.revents == 0) continue;
        if (p.fd == channel_->fd() && channel_->fd() >= 0) {
            if (p.revents & POLLOUT) channel_->onWritable();
            if (p.revents & (POLLIN | POLLHUP | POLLERR)) {
                std::vector<std::string> messages;
                channel_->onReadable(messages);
                if (status_.phase == Phase::Connecting && !pipeRun_ && socket_.open()) {
                    say("DevTools connected on port " + std::to_string(port_));
                    connected(now);
                }
                for (const std::string& m : messages) handleMessage(m, now);
            }
        } else if (p.fd == process_.exitFd()) {
            tick(now);  // reaps it
        }
    }
}

void BrowserSession::tick(double now) {
    if (process_.running() && process_.poll()) {
        say("Chrome exited (" + process_.status() + ")");
        if (status_.phase == Phase::Stopping) {
            closeChannels();
            sweepProfile();
            setPhase(Phase::Off, now);
        } else if (status_.phase != Phase::Off && status_.phase != Phase::Failed) {
            lostChrome(trf(Tx::ChromeExited, {process_.status()}), now);
        }
    }
    // Stopped to start again at a new page size: started now that it is off (whichever way it ended)
    if (reopenAfterStop_ && status_.phase == Phase::Off) {
        reopenAfterStop_ = false;
        open(now);
    }
    if (process_.running() && now >= nextLogCheck_) {
        nextLogCheck_ = now + options_.logCheckSec;
        if (process_.capLog(options_.logMaxBytes)) {
            say("chrome.log passed " + std::to_string(options_.logMaxBytes >> 20) + " MB: cut back to nothing");
        }
    }
    process_.reapLeftovers();
    const bool socketLost = channel_->failed();
    switch (status_.phase) {
        case Phase::Starting: {
            if (pipeRun_) {
                // Waiting for the first answer over the pipe (Pending::Probe)
                if (socketLost) {
                    lostChrome(trf(Tx::ChromePipeClosed, {channel_->error()}), now);
                } else if (now - phaseSince_ >= options_.startTimeoutSec) {
                    lostChrome(tr(Tx::ChromeNotAnswering), now);
                }
                break;
            }
            if (now >= nextPortCheck_) {
                nextPortCheck_ = now + kPortCheckSec;
                std::ifstream file(options_.launch.profileDir + "/DevToolsActivePort");
                std::stringstream content;
                content << file.rdbuf();
                int port = 0;
                std::string path;
                if (cdp::parseActivePort(content.str(), port, path)) {
                    port_ = port;
                    browserPath_ = path;
                    if (socket_.connect(port, path)) {
                        setPhase(Phase::Connecting, now);
                    } else {
                        lostChrome(trf(Tx::ChromeDevToolsFailed, {socket_.error()}), now);
                    }
                    break;
                }
            }
            if (now - phaseSince_ >= options_.startTimeoutSec) lostChrome(tr(Tx::ChromeNotAnswering), now);
            break;
        }
        case Phase::Connecting:
        case Phase::Attaching:
            if (socketLost) {
                lostChrome(trf(Tx::ChromeDevToolsLost, {channel_->error()}), now);
            } else if (now - phaseSince_ >= options_.connectTimeoutSec) {
                lostChrome(tr(Tx::ChromeDevToolsSilent), now);
            }
            break;
        case Phase::Ready:
            if (socketLost) {
                lostChrome(trf(Tx::ChromeDevToolsLost, {channel_->error()}), now);
                break;
            }
            if (failures_ > 0 && now - readySince_ >= options_.stableSec) {
                say("ready for " + std::to_string(static_cast<int>(options_.stableSec)) +
                    " s: the restart count starts over");
                failures_ = 0;
                blamedUrl_.clear();
            }
            if (ackPending_ && now >= ackDueAt_ - 1e-4) ackNow(now);
            if (!visible_) {
                if (audioCheckPending_ && now - audioCheckSentAt_ >= options_.audioCheckTimeoutSec) {
                    // No answer (a busy or stuck page): maybe it plays. The next check goes out as usual
                    if (const auto it = pending_.find(audioCheckId_); it != pending_.end()) forget(it);
                    audioCheckPending_ = false;
                    unknownAudio(now);
                }
                if (frozen_) {
                    // Paused and frozen: nothing plays, and a frozen page can't answer the check anyway
                    audioUnknown_ = false;
                    clearMedia();
                    nextAudioCheck_ = now + options_.audioCheckSec;
                } else if (now >= nextAudioCheck_ && !audioCheckPending_) {
                    nextAudioCheck_ = now + options_.audioCheckSec;
                    audioCheckId_ = send("Runtime.evaluate", cdp::evaluateParams(kAudibleScript), true, Pending::Audible);
                    if (audioCheckId_ != 0) {
                        audioCheckPending_ = true;
                        audioCheckSentAt_ = now;
                    }
                }
                // Stopped only once a check has answered "silent" since the last unknown one; a page that answers none
                // for hungShutdownSec (and the idle time) counts as hung and is stopped the same way
                const double idleSince = std::max(hiddenSince_, lastAudibleAt_);
                if (options_.idleShutdownSec > 0 && !holdingIdle(now) && !audioUnknown_ &&
                    now - idleSince >= options_.idleShutdownSec) {
                    say("hidden and silent for " + std::to_string(static_cast<int>(options_.idleShutdownSec)) +
                        " s: closing Chrome");
                    stop(now);
                } else if (options_.idleShutdownSec > 0 && !holdingIdle(now) && audioUnknown_ && unansweredSince_ >= 0 &&
                           now - unansweredSince_ >= hungAfterSec()) {
                    say("hidden, and no audio check answered for " + std::to_string(static_cast<int>(hungAfterSec())) +
                        " s: the page counts as hung, closing Chrome");
                    stop(now);
                }
            }
            break;
        case Phase::Stopping:
            if (!termSent_ && now >= stopAt_ + kCloseWaitSec) {
                say("Browser.close didn't end it: SIGTERM");
                process_.signalGroup(SIGTERM);
                termSent_ = true;
            }
            if (!killSent_ && now >= stopAt_ + kCloseWaitSec + kTermWaitSec) {
                say("SIGTERM didn't end it: SIGKILL");
                process_.signalGroup(SIGKILL);
                killSent_ = true;
            }
            if (!process_.running()) {
                closeChannels();
                sweepProfile();
                setPhase(Phase::Off, now);
            }
            break;
        case Phase::Restarting:
            if (now >= restartAt_) startProcess(now);
            break;
        case Phase::Off:
        case Phase::Failed: break;
    }
}

double BrowserSession::nextWake(double now) const {
    double next = kInf;
    const auto at = [&](double t) { next = std::min(next, std::max(t, now)); };
    // Without a pidfd the child's exit is noticed by polling
    if (process_.running() && process_.exitFd() < 0) at(now + 0.5);
    if (process_.running()) at(nextLogCheck_);
    if (process_.leftovers() > 0) at(now + 0.5);  // a child that outlived SIGKILL's wait is reaped later
    if (reopenAfterStop_ && status_.phase == Phase::Off) at(now);  // started again (a new page size)
    switch (status_.phase) {
        case Phase::Starting:
            if (pipeRun_) {
                if (channel_->failed()) at(now);
            } else {
                at(nextPortCheck_);
            }
            at(phaseSince_ + options_.startTimeoutSec);
            break;
        case Phase::Connecting:
        case Phase::Attaching: at(phaseSince_ + options_.connectTimeoutSec); break;
        case Phase::Ready:
            if (failures_ > 0) at(readySince_ + options_.stableSec);
            if (ackPending_) at(ackDueAt_);
            if (!visible_) {
                const bool holding = holdingIdle(now);
                if (!frozen_) at(audioCheckPending_ ? audioCheckSentAt_ + options_.audioCheckTimeoutSec : nextAudioCheck_);
                if (holding) at(holdEndsAt());  // the hold's end
                if (options_.idleShutdownSec > 0 && !holding && !audioUnknown_) {
                    at(std::max(hiddenSince_, lastAudibleAt_) + options_.idleShutdownSec);
                }
                if (options_.idleShutdownSec > 0 && !holding && audioUnknown_ && unansweredSince_ >= 0) {
                    at(unansweredSince_ + hungAfterSec());
                }
            }
            if (channel_->failed()) at(now);
            break;
        case Phase::Stopping:
            if (!termSent_) at(stopAt_ + kCloseWaitSec);
            if (!killSent_) at(stopAt_ + kCloseWaitSec + kTermWaitSec);
            if (!process_.running()) at(now);
            break;
        case Phase::Restarting: at(restartAt_); break;
        case Phase::Off:
        case Phase::Failed: break;
    }
    return next;
}

bool BrowserSession::fastFrame(const std::string& text, double now) {
    // The frames are most of the traffic (one per 1/fps, each ~100 KB-1 MB of base64): no JsonValue for them
    static thread_local cdp::ScreencastFrame f;
    static thread_local std::string session;
    switch (cdp::readScreencastMessage(text, f, session)) {
        case cdp::FastFrame::NotAFrame:
        case cdp::FastFrame::Fallback: return false;
        case cdp::FastFrame::Frame: break;
    }
    if (session == sessionId_) onFrame(f, now);
    return true;
}

void BrowserSession::handleMessage(const std::string& text, double now) {
    if (fastFrame(text, now)) return;
    const cdp::Message m = cdp::parse(text);
    if (m.kind == cdp::Message::Kind::Response) {
        const auto it = pending_.find(m.id);
        if (it == pending_.end()) return;
        const Pending kind = it->second;
        forget(it);
        if (m.failed && kind == Pending::Probe) {
            handleResponse(m, kind, now);  // an error is an answer too: Chrome is there
            return;
        }
        if (m.failed) {
            if (kind != Pending::Other && kind != Pending::Audible && kind != Pending::Input) {
                say("command " + std::to_string(m.id) + " failed: " + m.errorMessage);
            }
            if (kind == Pending::Audible) {
                // Couldn't ask: maybe playing, as with no answer
                audioCheckPending_ = false;
                unknownAudio(now);
            }
            if (kind == Pending::GetTargets || kind == Pending::CreateTarget || kind == Pending::Attach) {
                if (status_.phase == Phase::Attaching) {
                    lostChrome(trf(Tx::ChromePageFailedWhy, {m.errorMessage}), now);
                } else if (status_.phase == Phase::Ready) {
                    // A popup or the opener we went back to: without a session every navigate / back / reload would
                    // be dropped silently. Find a page again (or restart Chrome on it)
                    attachFailed(m.errorMessage, now);
                }
            }
            return;
        }
        handleResponse(m, kind, now);
    } else if (m.kind == cdp::Message::Kind::Event) {
        handleEvent(m, now);
    }
}

void BrowserSession::dropInputs() {
    // Inputs sent to a session we leave (or a renderer that died) may never be answered: they don't hold back moves
    for (auto it = pending_.begin(); it != pending_.end();) {
        it = it->second == Pending::Input ? pending_.erase(it) : std::next(it);
    }
    inputInFlight_ = 0;
}

void BrowserSession::attachTo(const std::string& targetId) {
    dropInputs();
    targetId_ = targetId;
    sessionId_.clear();
    screencasting_ = ackPending_ = false;
    clearDialog();
    send("Target.attachToTarget", cdp::attachParams(targetId), false, Pending::Attach);
}

void BrowserSession::detachSession() {
    if (sessionId_.empty()) return;
    send("Target.detachFromTarget", cdp::detachParams(sessionId_), false);  // already gone: an error nobody reads
    sessionId_.clear();
    screencasting_ = ackPending_ = false;
}

void BrowserSession::clearDialog() {
    status_.dialogType.clear();
    status_.dialogMessage.clear();
    dialogDefault_.clear();
}

void BrowserSession::holdIdle(bool hold, double now) {
    if (hold == idleHeld_) return;
    // Released after it kept the page: the idle time starts over. One that kept nothing (no video, past
    // kMaxIdleHoldSec) leaves it as it was
    if (!hold && holdingIdle(now)) lastAudibleAt_ = now;
    idleHeld_ = hold;
    if (hold) idleHeldSince_ = now;
}

void BrowserSession::holdPaused(bool hold, double now) {
    if (hold == pausedHeld_) return;
    if (!hold && holdingIdle(now)) lastAudibleAt_ = now;  // as holdIdle
    pausedHeld_ = hold;
    if (hold) pausedHeldSince_ = now;
}

void BrowserSession::unknownAudio(double now) {
    audioUnknown_ = true;
    lastAudibleAt_ = now;
    if (unansweredSince_ < 0) {
        // Said once until a check answers (unanswered, failed, or an answer that isn't one)
        say("an audio check got no answer: counting the page as maybe playing (until a check answers)");
        unansweredSince_ = now;
    }
}

void BrowserSession::media(MediaCommand command) {
    // A frozen page (hidden with keep_audio off) is paused already and wouldn't answer until shown
    if (status_.phase != Phase::Ready || sessionId_.empty() || frozen_) return;
    const char* action = "";
    switch (command) {
        case MediaCommand::Back: action = "if(m)m.currentTime=Math.max(0,m.currentTime-10);"; break;
        case MediaCommand::Forward:
            action = "if(m)m.currentTime=Math.min(isFinite(m.duration)?m.duration:1e9,m.currentTime+10);";
            break;
        case MediaCommand::PlayPause:
            action = "if(m){if(m.paused){const p=m.play();if(p)p.catch(()=>{})}else m.pause()}";
            break;
        case MediaCommand::Mute: action = "if(m)m.muted=!m.muted;"; break;
        case MediaCommand::Pause:
            action = "was=!!m&&!m.paused;ms.forEach(x=>{try{x.pause()}catch(e){}});";
            break;
    }
    // A user gesture: play() after a pause may need one. Only a press marks the video (not the headset's pause)
    send("Runtime.evaluate", cdp::evaluateParams(mediaScript(action, command != MediaCommand::Pause), true), true,
         Pending::Media);
}

void BrowserSession::answerDialog(bool accept) {
    if (status_.dialogType.empty()) return;
    say(std::string(accept ? "accepted" : "dismissed") + " the page's " + status_.dialogType + " dialog");
    send("Page.handleJavaScriptDialog", cdp::dialogParams(accept, status_.dialogType == "prompt" ? dialogDefault_ : ""),
         true);
    clearDialog();
}

void BrowserSession::handleResponse(const cdp::Message& m, Pending kind, double now) {
    switch (kind) {
        case Pending::Probe:
            if (status_.phase == Phase::Starting && pipeRun_) {
                const std::string product = jsonText(m.body.get("product"));
                say("DevTools connected over the pipe" + (product.empty() ? std::string() : " (" + product + ")"));
                connected(now);
            }
            break;
        case Pending::GetTargets: {
            const JsonValue* infos = m.body.get("targetInfos");
            std::string page;
            if (infos != nullptr && infos->type == JsonValue::Type::Array) {
                for (const JsonValue& info : infos->items) {
                    if (jsonText(info.get("type")) == "page") {
                        page = jsonText(info.get("targetId"));
                        break;
                    }
                }
            }
            if (!page.empty()) {
                attachTo(page);
            } else {
                send("Target.createTarget", cdp::createTargetParams("about:blank"), false, Pending::CreateTarget);
            }
            break;
        }
        case Pending::CreateTarget: {
            const std::string id = jsonText(m.body.get("targetId"));
            if (id.empty()) {
                lostChrome(tr(Tx::ChromePageFailed), now);
            } else {
                attachTo(id);
            }
            break;
        }
        case Pending::Attach: {
            sessionId_ = jsonText(m.body.get("sessionId"));
            if (sessionId_.empty()) {
                if (status_.phase == Phase::Ready) {
                    attachFailed("no session", now);
                } else {
                    lostChrome(tr(Tx::ChromeAttachFailed), now);
                }
                break;
            }
            attachRetries_ = 0;
            pageReady(now);
            break;
        }
        case Pending::History:
        case Pending::Back:
        case Pending::Forward: {
            cdp::History history;
            if (!cdp::readHistory(m.body, history)) break;
            status_.canBack = history.canBack();
            status_.canForward = history.canForward();
            if (m.id > lastNavigateId_) {  // asked after our last navigation: it describes the current page
                if (!history.url.empty()) status_.url = history.url;
                if (!history.title.empty()) status_.title = history.title;
            }
            if (kind != Pending::History) {
                const int entry = history.entryAt(kind == Pending::Back ? -1 : 1);
                if (entry >= 0) send("Page.navigateToHistoryEntry", cdp::historyEntryParams(entry), true);
            }
            break;
        }
        case Pending::Audible:
        case Pending::Media: {
            if (kind == Pending::Audible) audioCheckPending_ = false;
            std::string state;
            bool audible = false;
            if (cdp::readStringResult(m.body, state) && state.size() >= 3) {
                audible = state[0] == '1';
                // Hidden only: shown, the panel has the page (and a late answer mustn't bring the controls back)
                clearMedia();
                std::istringstream rest(state.substr(3));
                std::string position, duration, was;
                rest >> position >> duration >> was;
                if (!visible_) {
                    status_.media = Media {state[1] != 'n', state[1] == 'r', state[2] == 'm'};
                    status_.media.position = position.empty() ? -1 : std::strtod(position.c_str(), nullptr);
                    status_.media.duration = duration.empty() ? -1 : std::strtod(duration.c_str(), nullptr);
                }
                // Pause everything's answer (shown or not): whether it stopped something
                if (kind == Pending::Media && !was.empty()) {
                    ++status_.pauseAnswers;
                    status_.pausedPlaying = was == "w1";
                }
                // Counted by media()'s answers only: an audio check keeps the count (it is no answer to a press)
                if (kind == Pending::Media) ++mediaAnswers_;
                status_.media.answer = mediaAnswers_;
                // Something plays aloud while the headset is off and its video was paused: paused again (a page
                // that started it by itself, the next one in a playlist)
                if (audible && kind == Pending::Audible && idleHeld_ && status_.media.present) {
                    say("the page plays while the headset is off: pausing it again");
                    media(MediaCommand::Pause);
                }
            } else if (kind == Pending::Media || !cdp::readBoolResult(m.body, audible)) {
                if (kind == Pending::Audible) unknownAudio(now);  // an answer that isn't one: maybe playing
                break;
            }
            if (kind == Pending::Audible) {
                audioUnknown_ = false;
                unansweredSince_ = -1;
            }
            if (audible) lastAudibleAt_ = now;
            break;
        }
        case Pending::ContextLink: {
            std::string link;
            if (!cdp::readStringResult(m.body, link)) link.clear();
            if (link.size() > kMaxShownText) link.clear();  // not a link anyone taps
            status_.contextSeq = contextSeq_;
            status_.contextLink = link;
            break;
        }
        case Pending::Window: {
            // Still hidden and kept playing (it may have been shown, or no longer kept, since we asked)
            const JsonValue* id = m.body.get("windowId");
            if (id == nullptr || id->type != JsonValue::Type::Number || visible_ || !options_.keepAudio ||
                status_.phase != Phase::Ready) {
                break;
            }
            const int window = static_cast<int>(id->number);
            if (std::find(minimizedWindows_.begin(), minimizedWindows_.end(), window) != minimizedWindows_.end()) break;
            send("Browser.setWindowBounds", cdp::windowStateParams(window, true), false);
            minimizedWindows_.push_back(window);
            say("hidden, keeping its audio: window " + std::to_string(window) + " minimized until shown");
            break;
        }
        case Pending::ZoomScript: {
            const JsonValue* id = m.body.get("identifier");
            if (id == nullptr || !id->isString()) break;
            if (m.id == zoomScriptRequest_) {
                zoomScriptId_ = id->text;
            } else {
                send("Page.removeScriptToEvaluateOnNewDocument", "{\"identifier\":" + jsonString(id->text) + "}", true);
            }
            break;
        }
        case Pending::PageEnable:
        case Pending::Metrics:
        case Pending::Navigate:
        case Pending::Input:
        case Pending::Other: break;
    }
}

void BrowserSession::pageReady(double now) {
    send("Page.enable", "{}", true, Pending::PageEnable);
    send("Inspector.enable", "{}", true);  // Inspector.targetCrashed (the page's renderer died)
    frozen_ = false;  // a fresh session on a page: active
    offSizeFrames_ = offSizeResends_ = 0;  // the size goes now: frames from before it don't count against it
    offSizeResentAt_ = now;
    send("Emulation.setDeviceMetricsOverride",
         cdp::metricsParams(options_.launch.width, options_.launch.height, options_.mobile), true, Pending::Metrics);
    zoomScriptId_.clear();  // a fresh session: none of ours yet
    zoomScriptRequest_ = 0;
    translate_ = Translate::Off;
    status_.translated = false;
    applyView(true);
    if (status_.phase == Phase::Attaching) {
        // A fresh Chrome: home, or after a crash or a stop the page it was on. Not a page that crashed it right after
        // being put back (blamed): then home, or about:blank when home is that page
        const bool restoring = !restoreUrl_.empty();
        std::string url = restoring ? restoreUrl_ : options_.homeUrl;
        restoreUrl_.clear();
        if (!blamedUrl_.empty() && url == blamedUrl_) url = "about:blank";
        if (!url.empty()) {
            lastNavigateId_ = send("Page.navigate", cdp::navigateParams(url), true, Pending::Navigate);
            status_.url = url;
            status_.loading = true;
        }
        suspectUrl_ = restoring ? url : std::string();
        suspectSince_ = now;
        if (blamedJustNow_) {
            blamedJustNow_ = false;
            std::string shown = blamedUrl_;
            for (const char* scheme : {"https://", "http://"}) {
                if (shown.rfind(scheme, 0) == 0) shown = shown.substr(std::char_traits<char>::length(scheme));
            }
            status_.notice =
                trf(url == "about:blank" || url.empty() ? Tx::ChromeCrashBlank : Tx::ChromeCrashHome, {shown});
            say("opened " + (url.empty() ? std::string("nothing") : url) + " instead of " + blamedUrl_);
        }
        // The restart count starts over only after stableSec of this (tick), not here: a page that crashes Chrome
        // every time it loads would otherwise restart it forever
        readySince_ = now;
        status_.error.clear();
        // Hidden from the start (opened to keep playing, or a restart while collapsed): idle counts from now
        hiddenSince_ = now;
        lastAudibleAt_ = -1e9;
        nextAudioCheck_ = now + options_.audioCheckSec;
        setPhase(Phase::Ready, now);
        say("ready: page " + targetId_);
    }
    if (visible_) {
        startScreencast();
    } else if (!options_.keepAudio && status_.phase == Phase::Ready) {
        setFrozen(true);
    } else if (status_.phase == Phase::Ready) {
        minimizeWindow();  // a popup (or a restart) while hidden, kept playing
    }
    requestHistory();
}

void BrowserSession::attachFailed(const std::string& why, double now) {
    say("couldn't attach to the page (" + why + ")");
    sessionId_.clear();
    if (++attachRetries_ > 2) {
        attachRetries_ = 0;
        lostChrome(trf(Tx::ChromeAttachFailedWhy, {why}), now);
        return;
    }
    // The first page Chrome lists (or a new one): the panel shows something that takes input again
    targetId_.clear();
    send("Target.getTargets", "{}", false, Pending::GetTargets);
}

void BrowserSession::pageCrashed(double now) {
    pageCrashes_.push_back(now);
    pageCrashes_.erase(std::remove_if(pageCrashes_.begin(), pageCrashes_.end(),
                                      [&](double t) { return now - t > kPageCrashWindowSec; }),
                       pageCrashes_.end());
    screencasting_ = ackPending_ = false;  // a crashed renderer sends no frames; started again with the reload
    dropInputs();
    if (!visible_) {
        say("the page crashed while hidden: reloading it when it is shown");
        reloadWhenShown_ = true;
        return;
    }
    recoverCrashedPage(now);
}

void BrowserSession::recoverCrashedPage(double now) {
    reloadWhenShown_ = false;
    if (static_cast<int>(pageCrashes_.size()) >= kMaxPageCrashes) {
        say("the page crashed " + std::to_string(pageCrashes_.size()) + " times within " +
            std::to_string(static_cast<int>(kPageCrashWindowSec)) + " s: opening about:blank instead");
        std::string shown = status_.url;
        for (const char* scheme : {"https://", "http://"}) {
            if (shown.rfind(scheme, 0) == 0) shown = shown.substr(std::char_traits<char>::length(scheme));
        }
        pageCrashes_.clear();
        const int id = send("Page.navigate", cdp::navigateParams("about:blank"), true, Pending::Navigate);
        if (id != 0) lastNavigateId_ = id;
        status_.url = "about:blank";
        status_.notice = trf(Tx::ChromePageHung, {shown.substr(0, utf8Prefix(shown, 200))});
        suspectUrl_.clear();
    } else {
        say("the page crashed: reloading");
        send("Page.reload", "{}", true);
    }
    if (visible_) startScreencast();
    (void)now;
}

void BrowserSession::handleEvent(const cdp::Message& m, double now) {
    const std::string& method = m.method;
    if (method == "Page.screencastFrame") {
        if (m.sessionId != sessionId_) return;
        cdp::ScreencastFrame f;
        if (!cdp::readScreencastFrame(m.body, f)) return;
        onFrame(f, now);
        return;
    }
    if (method == "Page.frameNavigated") {
        const JsonValue* frame = m.body.get("frame");
        if (frame == nullptr || m.sessionId != sessionId_) return;
        if (frame->get("parentId") == nullptr) {
            mainFrameId_ = jsonText(frame->get("id"));
            const std::string url = jsonText(frame->get("url"));
            if (!url.empty()) status_.url = url;
            requestHistory();
        }
        return;
    }
    if (method == "Page.navigatedWithinDocument") {
        if (m.sessionId != sessionId_) return;
        if (jsonText(m.body.get("frameId")) == mainFrameId_) {
            const std::string url = jsonText(m.body.get("url"));
            if (!url.empty()) status_.url = url;
        }
        requestHistory();
        return;
    }
    if (method == "Page.frameStartedLoading" || method == "Page.frameStoppedLoading") {
        if (m.sessionId != sessionId_) return;
        const std::string frameId = jsonText(m.body.get("frameId"));
        if (mainFrameId_.empty() || frameId == mainFrameId_) {
            status_.loading = method == "Page.frameStartedLoading";
            // 번역's own reload starts; any other load of a translated (or translating) page, or a load that ended
            // without its load event (stopped, or the reload never really went): 번역 off and the content policy
            // back before that page commits
            if (status_.loading && translate_ == Translate::Reloading) translate_ = Translate::Loading;
            else if (status_.loading && translate_ != Translate::Off) translateOff();
            else if (!status_.loading && translate_ == Translate::Loading) translateOff();
            if (!status_.loading) requestHistory();
        }
        return;
    }
    if (method == "Page.loadEventFired") {
        if (m.sessionId != sessionId_) return;
        status_.loading = false;
        requestHistory();
        // The reload loaded (Reloading too: its start may not have come). A "no" to "leave this page?" loads nothing
        if (translate_ == Translate::Loading || translate_ == Translate::Reloading) {
            translate_ = Translate::On;
            send("Runtime.evaluate", cdp::evaluateParams(translateScript(translateTarget(uiLanguage()))), true);
            say("번역: Google's translate widget on the page");
        }
        return;
    }
    if (method == "Page.javascriptDialogOpening") {
        if (m.sessionId != sessionId_) return;
        const std::string type = jsonText(m.body.get("type"));
        if (type == "alert") {
            // Only says something: accepting it changes nothing but lets the page go on
            send("Page.handleJavaScriptDialog", cdp::dialogParams(true, ""), true);
            return;
        }
        status_.dialogType = type.empty() ? "confirm" : type;
        const std::string message = jsonText(m.body.get("message"));
        status_.dialogMessage = message.substr(0, utf8Prefix(message, kMaxShownText));
        dialogDefault_ = jsonText(m.body.get("defaultPrompt"));
        say("the page opened a " + status_.dialogType + " dialog");
        // Nobody sees the panel: cancel (never a "yes" nobody gave; a page asking before it is left stays)
        if (!visible_) answerDialog(false);
        return;
    }
    if (method == "Page.javascriptDialogClosed") {
        if (m.sessionId != sessionId_) return;
        clearDialog();
        // "No" before 번역's reload went (the page's "leave this page?"): no reload comes, so no bypass stays
        const JsonValue* result = m.body.get("result");
        const bool declined = result != nullptr && result->type == JsonValue::Type::Bool && !result->boolean;
        if (declined && (translate_ == Translate::Reloading || translate_ == Translate::Loading)) translateOff();
        return;
    }
    if (method == "Inspector.targetCrashed" || method == "Target.targetCrashed") {
        // The page's renderer (not Chrome)
        const bool ours = method == "Inspector.targetCrashed"
                              ? !sessionId_.empty() && m.sessionId == sessionId_
                              : !targetId_.empty() && jsonText(m.body.get("targetId")) == targetId_;
        if (!ours || status_.phase != Phase::Ready) return;
        // The same crash may come as both events, one right after the other: the second of a pair isn't another one
        const bool inspector = method == "Inspector.targetCrashed";
        if (!crashPaired_ && inspector != lastCrashWasInspector_ && now - lastCrashAt_ < 0.5) {
            crashPaired_ = true;
            return;
        }
        lastCrashWasInspector_ = inspector;
        lastCrashAt_ = now;
        crashPaired_ = false;
        pageCrashed(now);
        return;
    }
    if (method == "Target.targetInfoChanged") {
        const JsonValue* info = m.body.get("targetInfo");
        if (info == nullptr || jsonText(info->get("targetId")) != targetId_) return;
        const std::string url = jsonText(info->get("url"));
        const std::string title = jsonText(info->get("title"));
        if (!url.empty()) status_.url = url;
        status_.title = title.substr(0, utf8Prefix(title, kMaxShownText));  // the page sets it: kept short
        return;
    }
    if (method == "Target.targetCreated") {
        const JsonValue* info = m.body.get("targetInfo");
        if (info == nullptr || jsonText(info->get("type")) != "page") return;
        if (status_.phase != Phase::Ready || targetId_.empty()) return;
        if (jsonText(info->get("openerId")) != targetId_) return;
        // A popup or a link to a new tab: show it; closing it comes back here. The opener's session goes (a fresh
        // one is attached when we come back), so no old session lingers with its events
        say("new page from this one: switching to it");
        if (screencasting_) stopScreencast();
        // Nobody sees the opener any more: its media stops (a page left playing behind a popup can't be paused)
        send("Runtime.evaluate", cdp::evaluateParams(kPauseScript), true);
        detachSession();
        openerStack_.push_back(targetId_);
        while (static_cast<int>(openerStack_.size()) > kMaxOpeners) {
            // A page opening window after window: the oldest waiting one is closed
            say("more than " + std::to_string(kMaxOpeners) + " pages behind popups: closing the oldest");
            send("Target.closeTarget", cdp::closeTargetParams(openerStack_.front()), false);
            openerStack_.erase(openerStack_.begin());
        }
        attachTo(jsonText(info->get("targetId")));
        return;
    }
    if (method == "Target.targetDestroyed" || method == "Target.detachedFromTarget") {
        const std::string id = jsonText(m.body.get("targetId"));
        const std::string session = jsonText(m.body.get("sessionId"));
        // A detach is ours only by its session: the late detach of a session we left (the opener's, before a popup)
        // names a target we may be back on with a new session
        const bool ours = method == "Target.detachedFromTarget" ? !session.empty() && session == sessionId_
                                                                 : !id.empty() && id == targetId_;
        if (!ours || status_.phase != Phase::Ready) {
            if (method == "Target.targetDestroyed" && !id.empty()) {
                openerStack_.erase(std::remove(openerStack_.begin(), openerStack_.end(), id), openerStack_.end());
            }
            return;
        }
        if (method == "Target.targetDestroyed") detachSession();  // its session may not have said so yet
        sessionId_.clear();
        screencasting_ = ackPending_ = false;
        clearDialog();
        if (!openerStack_.empty()) {
            const std::string back = openerStack_.back();
            openerStack_.pop_back();
            attachTo(back);
        } else {
            targetId_.clear();
            send("Target.createTarget", cdp::createTargetParams(options_.homeUrl.empty() ? "about:blank" : options_.homeUrl),
                 false, Pending::CreateTarget);
        }
        return;
    }
}

void BrowserSession::onFrame(const cdp::ScreencastFrame& f, double now) {
    std::string error;
    if (decoder_.decode(reinterpret_cast<const uint8_t*>(f.jpeg.data()), f.jpeg.size(), frame_, error)) {
        frameNew_ = true;
        frame_.cssWidth = f.deviceWidth;
        frame_.cssHeight = f.deviceHeight;
        // The page isn't at the size we set (a page or Chrome changed it: the panel would show it letterboxed): set it
        // again after a few such frames, at most every kOffSizeResendSec
        const bool offSize = (f.deviceWidth > 0 && std::lround(f.deviceWidth) != options_.launch.width) ||
                             (f.deviceHeight > 0 && std::lround(f.deviceHeight) != options_.launch.height) ||
                             (f.deviceWidth <= 0 && (frame_.width != options_.launch.width ||
                                                     frame_.height != options_.launch.height));
        offSizeFrames_ = offSize ? offSizeFrames_ + 1 : 0;
        if (!offSize) offSizeResends_ = 0;
        if (offSizeFrames_ >= kOffSizeFrames && now - offSizeResentAt_ >= kOffSizeResendSec &&
            offSizeResends_ < kOffSizeResendMax && !sessionId_.empty()) {
            offSizeResentAt_ = now;
            ++offSizeResends_;
            say("frame " + std::to_string(frame_.width) + "x" + std::to_string(frame_.height) + " (page " +
                std::to_string(std::lround(f.deviceWidth)) + "x" + std::to_string(std::lround(f.deviceHeight)) +
                ") isn't the page size " + std::to_string(options_.launch.width) + "x" +
                std::to_string(options_.launch.height) + ": setting it again");
            send("Emulation.setDeviceMetricsOverride",
                 cdp::metricsParams(options_.launch.width, options_.launch.height, options_.mobile), true,
                 Pending::Metrics);
        }
    } else {
        say("frame: " + error);
    }
    if (!screencasting_) return;  // stopped meanwhile: no ack wanted
    ackPending_ = true;
    ackSessionId_ = f.sessionId;
    ackDueAt_ = std::max(now, lastAckAt_ + ackInterval());
    if (now >= ackDueAt_) ackNow(now);
}

void BrowserSession::startScreencast() {
    if (sessionId_.empty()) return;
    send("Page.startScreencast", cdp::screencastParams(options_.launch.width, options_.launch.height, options_.jpegQuality),
         true);
    screencasting_ = true;
    ackPending_ = false;
}

void BrowserSession::stopScreencast() {
    if (screencasting_ && !sessionId_.empty()) send("Page.stopScreencast", "{}", true);
    screencasting_ = false;
    ackPending_ = false;
}

void BrowserSession::requestHistory() { send("Page.getNavigationHistory", "{}", true, Pending::History); }

void BrowserSession::ackNow(double now) {
    if (!ackPending_) return;
    send("Page.screencastFrameAck", cdp::screencastAckParams(ackSessionId_), true);
    ackPending_ = false;
    lastAckAt_ = now;
    ++acks_;
}

bool BrowserSession::takeFrame(BgraFrame& out) {
    if (!frameNew_) return false;
    std::swap(out, frame_);
    // The decoder writes into frame_ next time: give it the old buffer's size back so it is reused
    frame_.sequence = out.sequence;
    frameNew_ = false;
    return true;
}

void BrowserSession::navigate(const std::string& text) {
    const std::string url = cdp::addressToUrl(text);
    if (url.empty()) return;
    if (translate_ != Translate::Off) translateOff();  // the policy back before the next page
    const int id = send("Page.navigate", cdp::navigateParams(url), true, Pending::Navigate);
    if (id != 0) {
        lastNavigateId_ = id;
        status_.url = url;
        status_.loading = true;
        status_.notice.clear();
        suspectUrl_.clear();  // a crash from here on is this page's, not the one put back
        pageCrashes_.clear();
    }
}

void BrowserSession::home() {
    if (!options_.homeUrl.empty()) navigate(options_.homeUrl);
}

void BrowserSession::back() {
    if (translate_ != Translate::Off) translateOff();
    if (send("Page.getNavigationHistory", "{}", true, Pending::Back) != 0) suspectUrl_.clear();
}
void BrowserSession::forward() {
    if (translate_ != Translate::Off) translateOff();
    if (send("Page.getNavigationHistory", "{}", true, Pending::Forward) != 0) suspectUrl_.clear();
}

void BrowserSession::reload(double now) {
    if (status_.phase == Phase::Failed || status_.phase == Phase::Off) {
        open(now);
        return;
    }
    if (translate_ != Translate::Off) translateOff();  // ⟳ on a translated page: the original
    send("Page.reload", "{}", true);
}

void BrowserSession::applyView(bool zoomChanged) {
    if (sessionId_.empty()) return;
    send("Emulation.setDeviceMetricsOverride",
         cdp::metricsParams(options_.launch.width, options_.launch.height, options_.mobile), true, Pending::Metrics);
    // Desktop: the override cleared ("" = the --user-agent switch's, with its client hints; an override without
    // metadata would drop sec-ch-ua)
    send("Emulation.setUserAgentOverride",
         cdp::userAgentParams(options_.mobile ? mobileUserAgent(desktopUserAgent_) : std::string(), options_.mobile), true);
    if (!zoomChanged) return;
    // The zoom: on every document from now on (the next page too) and on this one at once
    if (!zoomScriptId_.empty()) {
        send("Page.removeScriptToEvaluateOnNewDocument", "{\"identifier\":" + jsonString(zoomScriptId_) + "}", true);
        zoomScriptId_.clear();
    }
    const std::string script = zoomScript(options_.zoomPct);
    // Only the latest one's id is kept: an older one's answer, still on its way, is removed when it comes
    zoomScriptRequest_ = options_.zoomPct == 100 ? 0
                         : send("Page.addScriptToEvaluateOnNewDocument", "{\"source\":" + jsonString(script) + "}",
                                true, Pending::ZoomScript);
    send("Runtime.evaluate", cdp::evaluateParams(script), true);
}

void BrowserSession::translate() {
    if (status_.phase != Phase::Ready || sessionId_.empty() || frozen_) return;
    if (translate_ != Translate::Off) {
        translateOff();
        send("Page.reload", "{}", true);  // the original
        return;
    }
    // Still loading: stopped first, so the reload's own frameStartedLoading comes (Blink sends it only when a frame
    // goes from not loading to loading); the stop's frameStoppedLoading comes before Reloading counts
    if (status_.loading) send("Page.stopLoading", "{}", true);
    send("Page.setBypassCSP", "{\"enabled\":true}", true);
    send("Page.reload", "{}", true);
    translate_ = Translate::Reloading;
    status_.translated = true;
}

void BrowserSession::translateOff() {
    translate_ = Translate::Off;
    status_.translated = false;
    send("Page.setBypassCSP", "{\"enabled\":false}", true);
}

void BrowserSession::mouse(Mouse kind, double x, double y, double now) {
    switch (kind) {
        case Mouse::Move:
            // Chrome is behind (bytes not sent yet, or inputs it hasn't answered): a later move says the same
            if (channel_->queued() > kMoveBackPressure || inputInFlight_ >= kMaxInputInFlight) return;
            send("Input.dispatchMouseEvent",
                 cdp::mouseParams("mouseMoved", x, y, buttons_ != 0 ? cdp::Button::Left : cdp::Button::None, 0, buttons_),
                 true, Pending::Input);
            break;
        case Mouse::Down: {
            const bool again = now - lastDownAt_ < kDoubleClickSec && std::hypot(x - lastDownX_, y - lastDownY_) < 6;
            clickCount_ = again ? std::min(clickCount_ + 1, 3) : 1;
            lastDownAt_ = now;
            lastDownX_ = x;
            lastDownY_ = y;
            buttons_ = 1;
            send("Input.dispatchMouseEvent", cdp::mouseParams("mousePressed", x, y, cdp::Button::Left, clickCount_, 1),
                 true, Pending::Input);
            break;
        }
        case Mouse::Up:
            if (buttons_ == 0) return;  // no press to end (it started off the page)
            buttons_ = 0;
            send("Input.dispatchMouseEvent",
                 cdp::mouseParams("mouseReleased", x, y, cdp::Button::Left, std::max(1, clickCount_), 0), true,
                 Pending::Input);
            break;
    }
}

void BrowserSession::wheel(double x, double y, double deltaX, double deltaY) {
    send("Input.dispatchMouseEvent", cdp::wheelParams(x, y, deltaX, deltaY), true, Pending::Input);
}

void BrowserSession::insertText(const std::string& text) {
    if (!text.empty()) send("Input.insertText", cdp::insertTextParams(text), true, Pending::Input);
}

void BrowserSession::contextMenu(double x, double y, int seq) {
    if (status_.phase != Phase::Ready) return;
    send("Input.dispatchMouseEvent", cdp::mouseParams("mouseMoved", x, y, cdp::Button::None, 0, 0), true, Pending::Input);
    send("Input.dispatchMouseEvent", cdp::mouseParams("mousePressed", x, y, cdp::Button::Right, 1, 2), true,
         Pending::Input);
    send("Input.dispatchMouseEvent", cdp::mouseParams("mouseReleased", x, y, cdp::Button::Right, 1, 0), true,
         Pending::Input);
    contextSeq_ = seq;
    send("Runtime.evaluate", cdp::evaluateParams(cdp::linkAtScript(x, y)), true, Pending::ContextLink);
}

void BrowserSession::key(cdp::Key key) {
    send("Input.dispatchKeyEvent", cdp::keyParams(key, true), true, Pending::Input);
    send("Input.dispatchKeyEvent", cdp::keyParams(key, false), true, Pending::Input);
}
