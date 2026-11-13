// framecorder (a separate recorder for the Steam Frame, not part of Frame Dock and not shipped with it): the 녹화
// control starts and stops its recorder, ~/.local/bin/framecorder. Frame Dock only runs that program; nothing of it
// is copied here.
//
// The recorder runs as a plain child (posix_spawn, its own process group, stdin /dev/null, its output in Frame
// Dock's log) with the options framecorder's own tab saved in ~/.config/framecorder/ui.conf, and writes its default
// file (~/Videos/framecorder/<time>.mp4 and its .perf.csv). SIGINT stops it and saves (what its "ctrl+c to stop"
// says); if it hasn't ended kStopSec later it gets SIGKILL. Every call returns at once (waitpid WNOHANG); only the
// destructor (Frame Dock exiting) waits, at most kExitWaitSec.
//
// While framecorder's tab (framecorder-ui) runs, Frame Dock asks it instead, the way framecorder-sync passes on a
// phone's request: command.json in ~/.config/framecorder/sync ({"id", "do": record | stop | clip, "from", "at"}), its
// answer and state in status.json there (src/ui/remote.rs on framecorder's dev branch; releases up to 0.1.4 don't
// have it). The recording is then the tab's own, so it also shows one started from the tab or a phone, and while
// framecorder's clips are on (status.json's clips) the control offers 클립 (save the last seconds) next to 녹화.
//
// Without the tab, the recorder runs as a child as above. framecorder's tab knows nothing of that recording, so a
// start is refused while another framecorder recorder runs (found in /proc; framecorder-ui, -sync and -grab don't
// count).
#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace framecorder {

/** ~/.local/bin/framecorder ("" without HOME). */
std::string recorderPath();
/** ~/.config/framecorder/ui.conf (XDG_CONFIG_HOME when absolute; "" without either). */
std::string uiConfPath();
/** @return true if path is an executable regular file */
bool isInstalled(const std::string& path);

/**
 * The recorder's options from framecorder's tab's settings (ui.conf: key=value lines). What is mapped: shape
 * (square 1:1, wide 16:9, tall 9:16) → --aspect, eye (left / right) → --eye, game_audio=false → --no-audio, mic=true →
 * --mic, fps (a number; auto = framecorder's choice) → --fps. Anything else (quality, clips, an unknown value) is left
 * to framecorder's defaults.
 */
std::vector<std::string> argsFromUiConf(const std::string& text);

/**
 * Whether a process is framecorder's recorder: its comm, or its program's file name (argv[0]; argv[1] when argv[0]
 * is an interpreter: python*, sh, bash), is exactly "framecorder" (so framecorder-ui, -sync, -grab are not).
 */
bool isRecorderProcess(const std::string& comm, const std::vector<std::string>& argv);

/**
 * A framecorder recorder running, from procRoot's [pid]/comm and [pid]/cmdline.
 * @param exceptPid not counted (Frame Dock's own child); 0 = none
 * @return its pid, 0 if none
 */
pid_t findRecorder(const std::string& procRoot, pid_t exceptPid);

/** What the 녹화 control shows (ControlsView::recordState's numbers). */
enum class State {
    Idle = 0,
    Recording = 1,
    Saving = 2,  ///< asked to stop, not ended yet
    Failed = 3,  ///< it ended by itself (or couldn't start): shown kNoteSec
    Busy = 4,    ///< another framecorder recorder runs, nothing started: shown kNoteSec
    ClipSaved = 5,  ///< framecorder's tab saved a clip: shown kNoteSec
    Refused = 6,    ///< framecorder's tab said no (or didn't answer in kAnswerSec): shown kNoteSec
};

/** Where framecorder's tab takes requests (command.json) and says how it is (status.json): uiConf's folder + /sync. */
std::string tabDir(const std::string& uiConf);

/** What framecorder's tab says in its status.json. */
struct TabStatus {
    bool read = false;       ///< status.json parsed (an object with a pid)
    long pid = 0;
    bool ready = false;      ///< its setup is done
    bool recording = false;
    bool running = false;    ///< recording and not paused
    bool clips = false;      ///< clips are on (its clips is a length, not null)
    bool clipReady = false;  ///< a clip can be saved now
    uint64_t recordedMs = 0;
    uint64_t at = 0;         ///< when it wrote this (unix ms)
    uint64_t done = 0;       ///< the last command it did
    std::string result;      ///< how that went: "ok", or why not
};
/** @return status.json's text read (read = false when it isn't one) */
TabStatus parseTabStatus(const std::string& json);

class Recorder {
public:
    static constexpr double kStopSec = 10;     ///< SIGINT, then SIGKILL this much later
    static constexpr double kNoteSec = 6;      ///< Failed / Busy stay on the card this long
    static constexpr double kExitWaitSec = 3;  ///< the destructor's wait after SIGINT, before SIGKILL
    static constexpr double kPollSec = 1;      ///< status.json looked at at most this often, in tick() (the tab
                                               ///< coming or going, clips on or off); no wake of its own
    static constexpr double kAnswerSec = 4;    ///< a request to the tab not answered by then is given up (Refused)

    /**
     * @param program the recorder (recorderPath())
     * @param uiConf framecorder's tab's settings (uiConfPath(); unreadable = framecorder's defaults)
     * @param procRoot where other recorders are looked for ("/proc")
     */
    Recorder(std::string program, std::string uiConf, std::string procRoot = "/proc");
    ~Recorder();  ///< stops a recording (SIGINT, kExitWaitSec, SIGKILL) and reaps it
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    /** The recorder is installed (checked once, at construction). */
    bool available() const { return available_; }
    /**
     * The control's press: starts when not recording, stops while recording, nothing while Saving or while the
     * tab hasn't answered the last request. With the tab up it asks the tab; else it runs the recorder (refused,
     * Busy, while another recorder runs). @return true if the state changed
     */
    bool toggle(double now);
    /** 클립: asks framecorder's tab to save the last seconds (clipsOffered() only). @return true if it asked */
    bool clip(double now);
    /** framecorder's clips are on and nothing is being recorded: the control offers 클립 next to 녹화 */
    bool clipsOffered() const;
    /** A clip can be asked for now (the tab says so, and no request is waiting for its answer) */
    bool clipUsable() const { return clipsOffered() && tab_.clipReady && pendingId_ == 0; }
    /** Reaps the child, SIGKILL past kStopSec, ends Failed / Busy. @return true if what the card shows changed */
    bool tick(double now);
    /** @return when tick() has something to do (+infinity: nothing) */
    double nextWake(double now) const;
    State state() const { return state_; }
    /** @return whole seconds since the recording started (0 when not recording) */
    int elapsedSec(double now) const;
    pid_t pid() const { return child_; }

private:
    std::string program_;
    std::string uiConf_;
    std::string procRoot_;
    bool available_ = false;
    State state_ = State::Idle;
    pid_t child_ = -1;
    double startedAt_ = 0;
    double stopAt_ = 0;       ///< while Saving: SIGKILL from then
    double noteUntil_ = 0;    ///< while Failed / Busy
    int shownSec_ = -1;       ///< the elapsed second tick() last reported
    std::string tabDir_;      ///< status.json and command.json
    TabStatus tab_;           ///< as last read
    bool tabUp_ = false;      ///< read, its pid runs and its setup is done: requests go to it
    int64_t statusMtime_ = -1;  ///< status.json's mtime (ns) when last read
    double nextPoll_ = 0;
    uint64_t pendingId_ = 0;  ///< a request waiting for its answer (0: none)
    std::string pendingDo_;
    double pendingUntil_ = 0;

    bool start(double now);
    /** nextWake() of the state alone (a recording's clock, a stop's timeout, a note's end) */
    double stateWake(double now) const;
    /** Reads status.json again when it changed, and whether the tab is up. @return true if what it says changed */
    bool pollTab();
    /** Writes command.json for the tab. @return false if it couldn't */
    bool ask(const char* what, double now);
    /** The state that follows from the tab's status (its recording), the answer to a waiting request */
    bool followTab(double now);
    void note(State state, double now);
    /** @return true if the child has ended (and was reaped) */
    bool reap(int& status);
};

}  // namespace framecorder
