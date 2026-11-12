// The browser panel's Chrome session on its own thread, so socket I/O, JSON, base64 and JPEG decoding never hold up
// the main loop (the HUD, the Dock, SteamVR's events). The thread sleeps in one poll() on the DevTools socket, the
// Chrome process's pidfd and a command eventfd, with the session's next deadline as the timeout: no busy loop.
//
// The main thread posts commands (closures run on the worker thread with the session) and takes the latest frame
// and status; the worker pokes the main loop's eventfd when either changed. Frames are swapped, never copied.
#pragma once

#include "browser_session.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class BrowserWorker {
public:
    /**
     * @param notifyFd an eventfd the worker writes 1 to when a frame or the status changed (the main loop's wake), or −1
     */
    BrowserWorker(SessionOptions options, int notifyFd);
    /**
     * Tests only: 1 makes the command eventfd fail (the worker falls back to a pipe), 2 makes the pipe fail too (no
     * thread starts: the status says Failed, posts are dropped and shutdown returns at once).
     */
    static int failWakeForTest;
    /** Shuts Chrome down (bounded) and joins the thread. */
    ~BrowserWorker();
    BrowserWorker(const BrowserWorker&) = delete;
    BrowserWorker& operator=(const BrowserWorker&) = delete;

    /** Runs fn on the worker thread with the session and the time (in order, soon). */
    void post(std::function<void(BrowserSession&, double)> fn);

    /** @return the session's status as of its last change */
    BrowserSession::Status status() const;
    /** @return the status's change count (cheap to compare) */
    uint64_t statusVersion() const { return statusVersion_.load(); }
    /** @return true (and the frame, by swapping buffers) if a frame came since the last call */
    bool takeFrame(BgraFrame& out);
    /** @return false if the thread couldn't start (no wake-up fd: nothing runs, the status says why) */
    bool started() const { return started_; }
    /** @return Chrome's PID (−1 when it isn't running) */
    pid_t pid() const { return pid_.load(); }

    /**
     * Closes Chrome and ends the thread. Blocks up to timeoutSec (plus a SIGKILL's reaping). Safe to call twice.
     */
    void shutdown(double timeoutSec);

private:
    BrowserSession session_;
    int notifyFd_;
    int commandFd_ = -1;       ///< polled by the thread (an eventfd, or a pipe's read end)
    int commandWriteFd_ = -1;  ///< written by post() (the same eventfd, or the pipe's write end)
    bool started_ = false;
    std::thread thread_;
    std::atomic<bool> quit_ {false};
    double shutdownTimeout_ = 5.0;
    mutable std::mutex mutex_;
    std::vector<std::function<void(BrowserSession&, double)>> commands_;
    BrowserSession::Status status_;
    std::atomic<uint64_t> statusVersion_ {0};
    BgraFrame shared_;
    bool sharedNew_ = false;
    BgraFrame local_;
    std::atomic<pid_t> pid_ {-1};

    void run();
    void publish();
    void wake(int fd);
    /** Makes the command wake-up fds. @return false (and why) if neither an eventfd nor a pipe could be made */
    bool makeCommandFds(std::string& error);
    void drainCommands();
};
