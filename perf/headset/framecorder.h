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
// framecorder's tab knows nothing of a recording started here (no IPC), so a start is refused while another
// framecorder recorder runs (found in /proc; framecorder-ui, -sync and -grab don't count).
#pragma once

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
};

class Recorder {
public:
    static constexpr double kStopSec = 10;     ///< SIGINT, then SIGKILL this much later
    static constexpr double kNoteSec = 6;      ///< Failed / Busy stay on the card this long
    static constexpr double kExitWaitSec = 3;  ///< the destructor's wait after SIGINT, before SIGKILL

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
     * The control's press: starts when not recording (refused, Busy, while another recorder runs), stops while
     * recording, nothing while Saving. @return true if the state changed
     */
    bool toggle(double now);
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

    bool start(double now);
    void note(State state, double now);
    /** @return true if the child has ended (and was reaped) */
    bool reap(int& status);
};

}  // namespace framecorder
