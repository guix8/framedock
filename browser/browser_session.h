// The browser panel's Chrome, one instance: started on first use, driven over the DevTools protocol (by default the
// --remote-debugging-pipe, else a WebSocket on 127.0.0.1; see devtools_pipe.h), its page shown through
// Page.startScreencast (JPEG frames, decoded here), input sent as Input.* events.
//
// Transport: the pipe unless the options say port. Over the pipe, Starting lasts until Chrome answers a first command
// (Browser.getVersion). If it never does within startTimeoutSec, the pipe closes, or Chrome exits first, the fds
// probably didn't reach Chrome (flatpak run → bwrap → zypak is not verified to pass them): it is restarted at once,
// without counting as a failure, on the port, and stays on the port for the rest of this session (Status::channel).
//
// Lifecycle (phase()):
//   Off → Starting (the process runs; we wait for DevToolsActivePort, or the pipe's first answer) → Connecting
//   (WebSocket; not for the pipe) → Attaching (the page
//   target, its session, the viewport, the first navigation) → Ready. stop(): Stopping (Browser.close, then SIGTERM to
//   its process group, then SIGKILL) → Off. A crash (the process exits, the socket drops) while not stopping restarts
//   it after 1, 2, 4 s, back on the page it showed. The count of restarts in a row starts over only once Chrome has
//   stayed Ready for stableSec (not merely reached it); after kMaxRestarts of them it stays Failed until open() is
//   called again (the panel's 새로고침). A crash within crashBlameSec of putting a page back is blamed on that page:
//   the next start opens the home page instead (about:blank if the home page is the one) and says so (Status::notice).
//   stop() also remembers the page, so opening again after the idle shutdown (or Chrome 끄기) comes back to it.
//
// Screencast: only while visible. Each frame is acked no sooner than 1/fps after the last ack (Chrome sends the next
// frame only after the ack, so this caps the frame rate). Hidden: the screencast stops; unless keepAudio the page's
// media is paused; after idleShutdownSec hidden with nothing audible, Chrome is stopped. An audio check that gets no
// answer within audioCheckTimeoutSec (or fails) counts as "maybe playing": the next checks go on, and Chrome isn't
// stopped until one of them answers, or none has for hungShutdownSec (a hung page). The checks run while hidden (the first one kFirstAudioCheckSec after hiding)
// whether or not the idle shutdown is on, and also read the page's video for the HUD's video controls
// (Status::media, media()).
//
// Page dialogs: alert() is accepted at once. confirm(), prompt() and beforeunload wait for the panel's 확인 / 취소
// (Status::dialogType, answerDialog()); hidden (or hidden while one waits) they are dismissed, the safe answer: no
// "yes" nobody gave, and a page asking before it is left stays.
//
// Popups: a page this one opens is shown instead (the opener's media is paused and its session detached first) and
// closing it attaches to the opener again with a fresh session. At most kMaxOpeners pages wait behind the one shown:
// a further popup closes the oldest (Target.closeTarget), so a page opening window after window can't pile them up.
//
// Renderer crashes (Inspector.targetCrashed / Target.targetCrashed: the page's process, not Chrome): reloaded, but
// not while hidden (reloaded when shown again), and after kMaxPageCrashes within kPageCrashWindowSec the panel shows
// about:blank with a notice instead of reloading again.
//
// Out of view (setInView(false): the panel faces away or is outside the field of view, though "shown"): frames are
// acked at kOutOfViewFps only; back in view the next ack goes at once. Hidden with keepAudio off, the page is paused
// and then frozen (Page.setWebLifecycleState) until shown; a frozen page counts as silent for the idle shutdown.
// Hidden with keepAudio on (headless only), the page's window is minimized (Browser.setWindowBounds): the page is
// hidden to Chrome, which stops drawing it and, after a few seconds, decoding a playing video's picture, while the
// sound goes on (measured 2026-10-31 with YouTube: 14 → 1.3 % of a core at 720p, 38 → 1.9 % at 1080p). Shown again,
// it is restored before the screencast starts. A popup attached while hidden gets its window minimized too.
//
// Single-threaded and poll-driven: the owner polls fds() and calls onPoll(), then tick(); nextWake() says when tick()
// has something to do. Nothing blocks except shutdownBlocking() (app exit). BrowserWorker runs it on its own thread.
#pragma once

#include "app_text.h"  // Language
#include "cdp.h"
#include "chrome_process.h"
#include "devtools_pipe.h"
#include "jpeg_frame.h"
#include "websocket.h"

#include <poll.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

/** @return Chrome's --lang for the UI language: "ko-KR", "en-US", "ja-JP" (set at each start) */
const char* chromeLocale(Language language);
/** @return 번역's target for the UI language: "ko", "en", "ja" (read when it is pressed) */
const char* translateTarget(Language language);
/** @return 번역's script: Google's website translate widget, hidden, set to target ("ko") as soon as it is ready */
std::string translateScript(const std::string& target);

struct SessionOptions {
    ChromeLaunch launch;
    int fpsCap = 30;                ///< screencast frames per second at most
    int jpegQuality = 70;
    std::string homeUrl = "https://www.google.com/";
    bool keepAudio = true;          ///< hidden: keep playing (false: pause the page's media)
    double idleShutdownSec = 600;   ///< hidden and silent this long: stop Chrome (0 = never)
    double audioCheckSec = 15;      ///< hidden: how often to ask the page whether anything plays
    double audioCheckTimeoutSec = 5;  ///< an audio check unanswered this long counts as "maybe playing"
    /// hidden, and no audio check answered this long (or the idle time, if longer): the page counts as hung and Chrome
    /// is stopped like an idle one (only while the idle shutdown is on)
    double hungShutdownSec = 300;
    bool mobile = false;            ///< the mobile version of sites: a phone's user agent and mobile emulation
    int zoomPct = 100;              ///< the page's zoom (CSS zoom on every document: fullscreen still fits the panel)
    double startTimeoutSec = 40;    ///< DevToolsActivePort must appear within this (a flatpak's first start is slow)
    double connectTimeoutSec = 10;
    double restartBaseSec = 1;      ///< restarts in a row wait 1, 2, 4 × this
    double stableSec = 30;          ///< Ready this long: the restarts in a row start over
    double crashBlameSec = 15;      ///< a crash this soon after putting a page back is blamed on that page
    double logCheckSec = 30;        ///< how often chrome.log's size is checked while Chrome runs
    size_t logMaxBytes = 5u << 20;  ///< chrome.log is cut back to nothing past this (within a run)
};

class BrowserSession {
public:
    enum class Phase { Off, Starting, Connecting, Attaching, Ready, Stopping, Restarting, Failed };
    static constexpr int kMaxRestarts = 3;
    static constexpr double kCloseWaitSec = 3;     ///< Browser.close, then SIGTERM
    static constexpr double kTermWaitSec = 2;      ///< SIGTERM, then SIGKILL
    static constexpr double kDoubleClickSec = 0.5;
    static constexpr int kMaxOpeners = 3;               ///< pages kept behind popups (the oldest is closed beyond)
    static constexpr int kMaxPageCrashes = 3;           ///< renderer crashes within the window: about:blank instead
    static constexpr double kPageCrashWindowSec = 60;
    static constexpr double kOutOfViewFps = 2;          ///< frames per second while the panel is out of view
    static constexpr size_t kMaxPending = 256;          ///< outstanding commands remembered at most (oldest go)
    static constexpr int kMaxInputInFlight = 8;         ///< mouse moves are dropped while this many inputs are unanswered
    static constexpr int kOffSizeFrames = 3;            ///< frames of another page size in a row before it is set again
    static constexpr double kOffSizeResendSec = 3;      ///< and then at most this often (also after it was set)
    static constexpr int kOffSizeResendMax = 3;         ///< then left as it is (the panel shows it letterboxed)
    static constexpr size_t kMaxShownText = 2048;       ///< a page's title and dialog text kept at most (characters)
    static constexpr double kFirstAudioCheckSec = 1;    ///< hidden: the first check (the HUD's video controls appear)
    static constexpr double kMaxIdleHoldSec = 3600;     ///< holdIdle keeps a page this long at most

    /** The page's video (or audio) for the HUD's video controls: read by the hidden panel's audio checks. */
    struct Media {
        bool present = false;  ///< one plays, or is paused (part way, or the one acted on, even at 0 s; none while the
                               ///< panel shows: not checked then)
        bool playing = false;
        bool muted = false;
        double position = -1;  ///< s (−1: none)
        double duration = -1;  ///< s (NaN: not known yet, +inf: live, −1: none)
        int answer = 0;        ///< media()'s answers so far (audio checks keep it): a new one is a press's answer
        /** Equal in what the controls show (position and duration move on every check while it plays) */
        bool operator==(const Media& o) const {
            return present == o.present && playing == o.playing && muted == o.muted && answer == o.answer;
        }
    };
    /** What media() does to the page's video (the one playing, else the paused one: see mediaScript). */
    enum class MediaCommand { Back, PlayPause, Forward, Mute, Pause };

    /** What the panel shows about the page and Chrome. */
    struct Status {
        Phase phase = Phase::Off;
        std::string url;
        std::string title;
        bool canBack = false;
        bool canForward = false;
        bool loading = false;
        std::string error;   ///< why it failed / restarted (short, in uiLanguage())
        std::string notice;  ///< Ready, but something to tell (a page that kept crashing Chrome wasn't reopened)
        int restarts = 0;
        std::string dialogType;     ///< a page dialog waiting for 확인 / 취소: "confirm", "prompt", "beforeunload"
        std::string dialogMessage;  ///< its text
        std::string channel;        ///< how DevTools is reached since the last start: "pipe", "port" ("" before)
        int contextSeq = 0;         ///< the contextMenu() request contextLink answers (0: none yet)
        std::string contextLink;    ///< the link at that request's point ("" = none)
        Media media;
        bool translated = false;     ///< 번역 is on for this page (asked, or done)
        int pauseAnswers = 0;        ///< answers to media(Pause) (shown or hidden)
        bool pausedPlaying = false;  ///< the last of them stopped something that played
        bool sameAs(const Status& o) const {
            return phase == o.phase && url == o.url && title == o.title && canBack == o.canBack &&
                   canForward == o.canForward && loading == o.loading && error == o.error && notice == o.notice &&
                   restarts == o.restarts && dialogType == o.dialogType && dialogMessage == o.dialogMessage &&
                   channel == o.channel && contextSeq == o.contextSeq && contextLink == o.contextLink &&
                   media == o.media && pauseAnswers == o.pauseAnswers && pausedPlaying == o.pausedPlaying &&
                   translated == o.translated;
        }
    };

    explicit BrowserSession(SessionOptions options);
    ~BrowserSession();  ///< kills Chrome if it still runs
    BrowserSession(const BrowserSession&) = delete;
    BrowserSession& operator=(const BrowserSession&) = delete;

    /** fps, home, keep audio and the idle shutdown apply at once; the launch options from the next start. */
    void setOptions(const SessionOptions& options, double now);
    /** Starts Chrome if it isn't running (also out of Failed: a retry). */
    void open(double now);
    /** The panel is shown (screencast on) or hidden / collapsed (off; the media paused unless keepAudio). */
    void setVisible(bool visible, double now);
    bool visible() const { return visible_; }
    /**
     * Shown, but is it in view (facing the user, inside the field of view)? Out of view the screencast slows to
     * kOutOfViewFps; back in view a waiting ack goes at once. Nothing else changes (no pause, no idle timer).
     */
    void setInView(bool inView, double now);
    bool inView() const { return inView_; }
    /** Closes Chrome: gracefully, then by signals (tick() carries it on). */
    void stop(double now);
    /** Failed before anything started (the worker couldn't start its thread): the panel shows why. */
    void failWithoutStart(const std::string& why, double now) { fail(why, now); }
    /** For the app's exit: stop() and wait (polling) until it is gone or timeoutSec passed, then SIGKILL. */
    void shutdownBlocking(double timeoutSec);

    // ---- The page (ignored unless Ready) ----
    /** The address bar: a URL or words to search for (cdp::addressToUrl). */
    void navigate(const std::string& text);
    void home();
    /**
     * 번역: Google's translate widget turns the page into the app's language (translateTarget(uiLanguage())). The
     * page's own content policy would refuse its script, so it is bypassed for this page and the page read again first (bypassing only after a load doesn't
     * take). Pressed again, ⟳, or any other load of the page (a link, a form, back): off, the policy back on before
     * that page commits; pressed again reads the original.
     */
    void translate();
    void back();
    void forward();
    /** Reload; out of Failed it starts Chrome again. */
    void reload(double now);
    enum class Mouse { Move, Down, Up };
    /** A laser mouse event at a page point (CSS px). */
    void mouse(Mouse kind, double x, double y, double now);
    /** A wheel turn (CSS px; positive deltaY scrolls down). */
    void wheel(double x, double y, double deltaX, double deltaY);
    void insertText(const std::string& text);
    /** A special key: down and up. */
    void key(cdp::Key key);
    /**
     * The HUD's video controls: 10 s back / forward, play / pause, the page's mute, or pause (every video: the headset
     * came off). The answer updates Status::media at once.
     */
    void media(MediaCommand command);
    /**
     * The headset is off with its video paused (벗으면 영상: 멈춤): while the page has a video (Status::media) no idle
     * shutdown, for kMaxIdleHoldSec at most, so the paused video is still there when it is worn again. The audio
     * checks go on: one that hears it play (a page that started it again) pauses it again, as long as it is held (no
     * kMaxIdleHoldSec limit for that). Without a video (a text
     * page, a frozen page) the idle shutdown goes on as usual. Released after it kept the page, the idle time counts
     * from now.
     */
    void holdIdle(bool hold, double now);
    bool idleHeld() const { return idleHeld_; }
    /**
     * The HUD's video controls show a paused video: kept from the idle shutdown like holdIdle (while it has a video,
     * kMaxIdleHoldSec at most), so it is still there to resume. No pausing again: nobody took the headset off.
     */
    void holdPaused(bool hold, double now);
    /** The panel's 확인 (accept) or 취소 for the waiting page dialog (a prompt gets its default text). */
    void answerDialog(bool accept);
    /**
     * The panel's context menu opens at a page point (CSS px): a right click there (the page's own contextmenu
     * handlers run; headless Chrome shows no menu of its own), then the link under it is looked up
     * (Status::contextLink, with contextSeq = seq once it is known).
     */
    void contextMenu(double x, double y, int seq);

    // ---- I/O ----
    /** Appends the fds to poll (the socket while it exists). */
    void fds(std::vector<pollfd>& out) const;
    /** After poll(): the entries fds() added, starting at first. */
    void onPoll(const std::vector<pollfd>& fds, size_t first, double now);
    /** Time-driven work: the process's exit, waiting for the port file, timeouts, acks, the idle shutdown. */
    void tick(double now);
    /** @return the next time tick() has something to do (+infinity: only socket activity) */
    double nextWake(double now) const;

    // ---- Out ----
    Phase phase() const { return status_.phase; }
    const Status& status() const { return status_; }
    /** @return true (and the frame) if a frame came since the last call; swaps buffers, no copy */
    bool takeFrame(BgraFrame& out);
    pid_t pid() const { return process_.pid(); }
    /** @return the DevTools port Chrome picked (0 before Connecting, and over the pipe) */
    int port() const { return port_; }
    /** @return true once the pipe failed this session and the port is used instead */
    bool pipeFellBack() const { return pipeFellBack_; }
    /** Restarts in a row without a stable run (tests) */
    int failuresInARow() const { return failures_; }
    /** Frames acked so far (tests) */
    uint64_t acks() const { return acks_; }
    /** Commands waiting for their answer (tests) */
    size_t pendingCount() const { return pending_.size(); }
    /** Pages kept behind popups (tests) */
    size_t openers() const { return openerStack_.size(); }
    /** The page is frozen (hidden with keepAudio off) (tests) */
    bool frozen() const { return frozen_; }
    /** A window is minimized (hidden with keepAudio on) (tests) */
    bool minimized() const { return !minimizedWindows_.empty(); }
    /** Logs a line (the worker's stderr; the tests can capture it) */
    std::function<void(const std::string&)> log;

private:
    enum class Pending {
        Probe, GetTargets, CreateTarget, Attach, PageEnable, Metrics, Navigate, History, Back, Forward, Audible,
        Media,        ///< a video control's answer: the media state, as the audio check's
        ContextLink,  ///< the link under a context menu's point
        Window,       ///< the page's window, to be minimized (hidden, keepAudio)
        ZoomScript,   ///< the zoom's script id (Page.addScriptToEvaluateOnNewDocument)
        Input,  ///< Input.*: counted, so mouse moves can be dropped while Chrome is behind
        Other   ///< not remembered at all (nobody reads its answer)
    };

    SessionOptions options_;
    Status status_;
    ChromeProcess process_;
    ws::Client socket_;
    devtools::PipeClient pipe_;
    DevToolsChannel* channel_ = &socket_;   ///< the one this start uses
    bool pipeRun_ = false;                  ///< this start uses the pipe
    bool pipeFellBack_ = false;             ///< the pipe didn't work: the port for the rest of this session
    cdp::Writer writer_;
    std::map<int, Pending> pending_;
    JpegDecoder decoder_;
    BgraFrame frame_;
    bool frameNew_ = false;
    /// frames whose page isn't the size we set (in a row), and when the size was last sent again for them
    int offSizeFrames_ = 0;
    double offSizeResentAt_ = -1e9;
    int offSizeResends_ = 0;  ///< sent again since the size last matched (at most kOffSizeResendMax)

    double phaseSince_ = 0;
    double nextPortCheck_ = 0;
    int port_ = 0;
    std::string browserPath_;
    std::string targetId_;
    std::string sessionId_;
    std::vector<std::string> openerStack_;  ///< the pages a popup came from (back to them when it closes)
    std::string restoreUrl_;                ///< after a crash or a stop: the page it was on (the next start opens it)
    std::string suspectUrl_;                ///< the page just put back (a crash soon after is blamed on it)
    double suspectSince_ = 0;
    std::string blamedUrl_;                 ///< a page that crashed Chrome right after being put back: not again
    bool blamedJustNow_ = false;            ///< the next Ready says so (Status::notice)
    double readySince_ = 0;
    bool visible_ = false;
    bool screencasting_ = false;
    bool ackPending_ = false;
    int ackSessionId_ = 0;
    double ackDueAt_ = 0;
    double lastAckAt_ = -1e9;
    uint64_t acks_ = 0;
    double hiddenSince_ = 0;
    double lastAudibleAt_ = -1e9;
    double nextAudioCheck_ = 0;
    bool audioCheckPending_ = false;
    int audioCheckId_ = 0;
    int contextSeq_ = 0;          ///< the last contextMenu() request (its link answer carries it)
    double audioCheckSentAt_ = 0;
    bool audioUnknown_ = false;             ///< the last check went unanswered or failed: maybe playing
    bool idleHeld_ = false;                 ///< holdIdle(true) (applies only while the page has a video)
    int mediaAnswers_ = 0;                  ///< Media::answer's count
    bool pausedHeld_ = false;               ///< holdPaused(true)
    double pausedHeldSince_ = 0;
    double unansweredSince_ = -1;           ///< hidden: no audio check answered since (−1: the last one did)
    /** An audio check that couldn't be asked or answered: maybe playing */
    void unknownAudio(double now);
    double hungAfterSec() const { return std::max(options_.hungShutdownSec, options_.idleShutdownSec); }
    /** @return when the hold keeping the page now ends */
    double holdEndsAt() const {
        double end = 0;
        if (idleHeld_) end = std::max(end, idleHeldSince_ + kMaxIdleHoldSec);
        if (pausedHeld_) end = std::max(end, pausedHeldSince_ + kMaxIdleHoldSec);
        return end;
    }
    /** No video (Status::media emptied) with media()'s answer count kept */
    void clearMedia() {
        status_.media = Media {};
        status_.media.answer = mediaAnswers_;
    }
    double idleHeldSince_ = 0;
    /** @return true while the hold keeps the page: asked for, a video there, not past kMaxIdleHoldSec */
    bool holdingIdle(double now) const {
        return status_.media.present && ((idleHeld_ && now - idleHeldSince_ < kMaxIdleHoldSec) ||
                                         (pausedHeld_ && now - pausedHeldSince_ < kMaxIdleHoldSec));
    }
    std::string dialogDefault_;             ///< the waiting prompt()'s default text
    int failures_ = 0;                      ///< restarts in a row (back to 0 after stableSec of Ready)
    double restartAt_ = 0;
    double stopAt_ = 0;
    bool termSent_ = false;
    bool reopenAfterStop_ = false;  ///< stopping to start again (a new page size)
    std::string desktopUserAgent_;  ///< the user agent this start went out with ("" = Chrome's own)
    std::string zoomScriptId_;      ///< Page.addScriptToEvaluateOnNewDocument's id for the zoom ("" = none)
    int zoomScriptRequest_ = 0;     ///< the latest such request (an older one's answer is removed when it comes)
    /**
     * 번역: Reloading = the policy bypassed and the reload asked; Loading = that reload started (the main frame's first
     * frameStartedLoading after it); On = it loaded (a load event while Reloading or Loading) and the widget went in. Any other main-frame load, a load that
     * stopped without its load event, or a dialog answered "no" while Reloading / Loading (the page's "leave this
     * page?" before the reload) turns it off, so the bypass never reaches a page that wasn't asked for.
     */
    enum class Translate { Off, Reloading, Loading, On };
    Translate translate_ = Translate::Off;
    /** The page's mobile emulation, user agent and zoom as options_ says (a new page session, or the options changed) */
    void applyView(bool zoomChanged);
    void translateOff();
    bool killSent_ = false;
    int buttons_ = 0;
    int clickCount_ = 0;
    double lastDownAt_ = -1e9, lastDownX_ = 0, lastDownY_ = 0;
    std::string mainFrameId_;
    int lastNavigateId_ = 0;  ///< a history answer to a request older than this doesn't set the URL (it is stale)
    int inputInFlight_ = 0;   ///< Pending::Input entries in pending_
    bool inView_ = true;
    bool frozen_ = false;     ///< Page.setWebLifecycleState frozen was sent (hidden, keepAudio off)
    std::vector<int> minimizedWindows_;  ///< windows minimized while hidden (keepAudio on), restored when shown
    bool headlessRun_ = true;            ///< this Chrome was started headless (only then are windows minimized)
    std::vector<double> pageCrashes_;  ///< renderer crashes within kPageCrashWindowSec
    bool reloadWhenShown_ = false;     ///< a renderer crash while hidden: reload when shown
    double lastCrashAt_ = -1e9;        ///< the last renderer crash event...
    bool lastCrashWasInspector_ = false;  ///< ...which kind it was (Inspector. or Target.targetCrashed)...
    bool crashPaired_ = true;          ///< ...and whether its other event has come
    int attachRetries_ = 0;            ///< failed attaches in Ready since the last good one
    double nextLogCheck_ = 0;

    void setPhase(Phase phase, double now);
    void say(const std::string& line) const;
    void startProcess(double now);
    void fail(const std::string& why, double now);
    /** Kills whatever still runs on our profile once Chrome itself is gone (killProfileProcesses) */
    void sweepProfile();
    void lostChrome(const std::string& why, double now);
    /** The pipe never answered: start again at once on the port (not counted as a failure) */
    void fallBackToPort(const std::string& why, double now);
    void closeChannels();
    /** Chrome answered: attach to its page */
    void connected(double now);
    /** Keeps the page Chrome shows for the next start (not about:blank, not a page blamed for crashes). */
    void rememberPage();
    int send(const std::string& method, const std::string& params, bool page, Pending kind = Pending::Other);
    void handleMessage(const std::string& text, double now);
    void handleResponse(const cdp::Message& m, Pending kind, double now);
    void handleEvent(const cdp::Message& m, double now);
    void attachTo(const std::string& targetId);
    /** Detaches our page session (a popup takes over, or the page went away) */
    void detachSession();
    void clearDialog();
    void pageReady(double now);
    void startScreencast();
    void stopScreencast();
    void requestHistory();
    void ackNow(double now);
    /** The frame's ack is due 1/fps after the last (kOutOfViewFps while out of view) */
    double ackInterval() const;
    /** A renderer crash of our page: reload, later (hidden), or about:blank (crashing again and again) */
    void pageCrashed(double now);
    void recoverCrashedPage(double now);
    /** An attach (popup, back to an opener) failed in Ready: find a page again, or give up and restart Chrome */
    void attachFailed(const std::string& why, double now);
    void forget(std::map<int, Pending>::iterator it);
    void dropInputs();
    /** Page.screencastFrame without the full JSON parse (the large data string isn't copied). @return handled */
    bool fastFrame(const std::string& text, double now);
    void onFrame(const cdp::ScreencastFrame& f, double now);
    void setFrozen(bool frozen);
    /** Hidden, keeping the audio: asks for the page's window, minimized when the answer comes (headless only) */
    void minimizeWindow();
    /** Shown: the minimized windows back to normal */
    void restoreWindows();
};

const char* phaseName(BrowserSession::Phase phase);
