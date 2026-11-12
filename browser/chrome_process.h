// Starting and stopping the browser panel's own Chrome: its command line, the child process in its own process group,
// and making sure nothing of it outlives Frame Dock.
//
// The default command is the system flatpak (flatpak run com.google.Chrome). Its sandbox can't see
// ~/.local/share/framedock/browser unless asked, so a "flatpak run" command gets --filesystem=<profile> (for this run
// only, not a persistent override) and --die-with-parent. The profile is always ours (--user-data-dir), never the
// owner's normal Chrome profile. DevTools goes over --remote-debugging-pipe (fds 3 and 4, nothing listening), or with
// the port transport over port 0 (Chrome picks a free one and writes it to DevToolsActivePort in the profile) on
// 127.0.0.1. See devtools_pipe.h.
#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

/** How the session talks to Chrome's DevTools */
enum class DevToolsTransport {
    Pipe,  ///< --remote-debugging-pipe: Chrome's fds 3 / 4 (only we can drive it)
    Port,  ///< --remote-debugging-port=0 on 127.0.0.1 (any local process can connect while it runs)
};

struct ChromeLaunch {
    std::vector<std::string> command = {"flatpak", "run", "com.google.Chrome"};  ///< the program and its own arguments
    DevToolsTransport transport = DevToolsTransport::Pipe;
    std::string profileDir;          ///< --user-data-dir (created before the start)
    /// Folders made owner-only (0700) before each start if they aren't: the profile's parents (framedock/, browser/)
    std::vector<std::string> privateDirs;
    std::string logPath;             ///< Chrome's stdout and stderr ("" = /dev/null)
    bool headless = true;            ///< --headless=new; false: a normal window far off screen
    int width = 1280, height = 720;  ///< the window (CSS px at scale 1)
    /// --user-agent ("" = Chrome's own). Headless Chrome calls itself HeadlessChrome there, and Google's sign-in
    /// refuses such a browser: the session sets Chrome's ordinary string (chromeUserAgent)
    std::string userAgent;
    std::vector<std::string> extraArgs;  ///< config: more Chrome switches (GPU experiments)
    /// --lang (Chrome's own words, the pages' language) and the first of --accept-lang: the app's UI language
    std::string locale = "ko-KR";
};

/** @return --accept-lang for a locale: "ko-KR,ko,en-US,en", "ja-JP,ja,en-US,en", "en-US,en" */
std::string acceptLanguages(const std::string& locale);

/** @return a command line split at spaces ('...' and "..." keep spaces; no other shell syntax) */
std::vector<std::string> splitCommand(const std::string& command);
/** @return true if the command runs a flatpak app (its argv gets --filesystem and --die-with-parent) */
bool isFlatpakRun(const std::vector<std::string>& command);
/**
 * @return the user agent Chrome sends when it isn't headless, for the version in the profile's "Last Version" file
 *         (Chrome writes it each run, e.g. "156.0.8078.47"): "Mozilla/5.0 (X11; Linux x86_64) … Chrome/156.0.0.0
 *         Safari/537.36", the reduced form Chrome itself sends on Linux (also on ARM). "" if it isn't a version
 */
std::string chromeUserAgent(const std::string& lastVersion);
/** @return the whole argv: the command, the flatpak options when it is one, then Chrome's switches */
std::vector<std::string> chromeArgv(const ChromeLaunch& launch);
/**
 * Makes a folder owner-only (0700) if it is ours and isn't already (a symlink is left alone).
 * @return true if its mode was changed
 */
bool makePrivateDir(const std::string& path);

class ChromeProcess {
public:
    ChromeProcess() = default;
    ~ChromeProcess();  ///< kills what is left (SIGKILL to the group) and reaps it
    ChromeProcess(const ChromeProcess&) = delete;
    ChromeProcess& operator=(const ChromeProcess&) = delete;

    /**
     * Starts argv[0] (searched in PATH) in a new process group, stdout / stderr to logPath. The child gets
     * PR_SET_PDEATHSIG (SIGKILL): if this thread or process dies, so does the child (and, through flatpak's
     * --die-with-parent, its sandbox). Every other fd of ours is closed in the child.
     * @param devtoolsFd ≥ 0: becomes the child's fd 3 and fd 4 (--remote-debugging-pipe); the caller closes its copy
     * The log of the previous start is kept as <logPath>.1; the new one is opened for appending (see capLog).
     */
    bool start(const std::vector<std::string>& argv, const std::string& logPath, std::string& error,
               int devtoolsFd = -1);
    /** @return true until the child has exited and was reaped */
    bool running() const { return pid_ > 0; }
    pid_t pid() const { return pid_; }
    /** @return a pidfd that becomes readable when the child exits (poll it instead of polling waitpid), or −1 */
    int exitFd() const { return pidfd_; }
    /**
     * Reaps the child if it exited (never blocks).
     * @return true if it exited just now; status() says how
     */
    bool poll();
    /** @return the last exit as text ("exit 0", "signal 9") */
    const std::string& status() const { return status_; }
    /** Sends a signal to the child's whole process group. */
    void signalGroup(int signal);
    /**
     * Last resort: SIGKILL to the group, then waits for the child (bounded; a child in D state is left to the
     * kernel and logged).
     */
    void killNow();
    /**
     * Keeps the log small within a run: past maxBytes it is cut back to nothing (Chrome's fds append, so its next
     * line lands at the start) with a line saying so.
     * @return true if it was cut
     */
    bool capLog(size_t maxBytes);
    /** Reaps children killNow() gave up waiting for (never blocks). */
    void reapLeftovers();
    /** @return children killed but not reaped yet */
    size_t leftovers() const { return leftovers_.size(); }
    /** killNow()'s wait for the child after SIGKILL (tests shorten it) */
    double killWaitSec = 2.0;

private:
    pid_t pid_ = -1;
    int pidfd_ = -1;
    std::string status_;
    int logFd_ = -1;                 ///< our copy of Chrome's log fd (its size is checked, it is cut)
    std::vector<pid_t> leftovers_;   ///< killed, not reaped yet (a child in D state when killNow gave up)

    void closePidfd();
};

/**
 * Leftovers of a Chrome that used this profile (a crash of Frame Dock that its death signal didn't cover, or helpers
 * outside its process group: a flatpak Chrome's zypak children are spawned through flatpak-portal, in their own
 * sandbox): our own processes with an option naming the profile, --user-data-dir=<profileDir> or any
 * --name=<profileDir>[/...] (crashpad's --database=<profile>/Crash Reports, flatpak's --filesystem=<profile>). A bare
 * path argument (someone's `ls <profile>`) doesn't count, and another Chrome (the owner's, Frame3D's) never matches.
 * "Our own" is the real UID in /proc/N/status: a non-dumpable process (Chrome's sandboxed helpers, a setuid bwrap)
 * shows root as the owner of /proc/N.
 * @return their PIDs
 */
std::vector<pid_t> findProfileProcesses(const std::string& profileDir);
/** @return true if this argv entry is an option naming the profile (or a path under it) */
bool namesProfile(const std::string& arg, const std::string& profileDir);
/** @return the real UID in /proc/<pid>/status ("Uid:"), or −1 if it can't be read */
long processRealUid(pid_t pid);
/** SIGTERM, a short wait, then SIGKILL for whatever findProfileProcesses still finds. @return how many there were */
int killProfileProcesses(const std::string& profileDir);
/**
 * After a Chrome on this profile was killed (no chance to clean up): removes SingletonLock, SingletonSocket and
 * SingletonCookie from the profile, but only when no live process uses the profile. Under flatpak the lock names a
 * PID inside the old sandbox, which the next sandbox may well reuse, so Chrome can't tell it is stale.
 * @return how many were removed (0 also when a process still uses the profile: then nothing is touched)
 */
int removeStaleSingletons(const std::string& profileDir);
