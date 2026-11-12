// SteamVR's room view over vrcmd or vrcompositor's mailbox. See room_view.h.
#include "room_view.h"

#include "json.h"
#include "websocket.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern char** environ;

namespace roomview {

std::string enableMessage(bool on) {
    return std::string(R"({"type":"set_camera_roomview_enabled","enabled":)") + (on ? "true" : "false") + "}";
}

std::string sceneAppsMessage(bool on) {
    return std::string(R"({"type":"set_camera_roomview_enabled_forsceneapps","enabled":)") + (on ? "true" : "false") +
           "}";
}

std::string sendCommand(const std::string& mailbox, const std::string& message) {
    return "mailbox_send " + mailbox + " " + message;
}

std::string probeMessage() {
    return std::string(R"({"type":"get_debug_commands","returnAddress":")") + kOwnMailbox + "\"}";
}

}  // namespace roomview

namespace {

double monotonic() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/** How long the server gets to answer our mailbox_open before the message goes anyway, and to read it after. */
constexpr double kOpenWaitSec = 0.3;
constexpr double kLingerSec = 0.2;

void poke(int fd) {
    if (fd < 0) return;
    const uint64_t one = 1;
    (void)!::write(fd, &one, sizeof(one));
}

/** The first line of text (at most 200 bytes), for one log line. */
std::string firstLine(const std::string& text) {
    std::string line = text.substr(0, text.find('\n'));
    if (line.size() > 200) line.resize(200);
    return line;
}

}  // namespace

// ---- The worker ----

RoomViewWorker::~RoomViewWorker() {
    shutdown();  // a subclass has done this already; here for safety
}

void RoomViewWorker::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    poke(stopFd_);
    if (thread_.joinable()) thread_.join();
    if (stopFd_ >= 0) ::close(stopFd_);
    stopFd_ = -1;
}

void RoomViewWorker::setEnabled(bool on) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return;
        queue_.clear();  // only the last wish counts
        queue_.push_back(on);
        ++wishes_;
        result_ = -1;    // an earlier wish's outcome no longer matters
        if (!thread_.joinable()) {
            stopFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            thread_ = std::thread([this] { run(); });
        }
    }
    cv_.notify_all();
}

bool RoomViewWorker::takeResult(bool& ok) {
    const int r = result_.exchange(-1);
    if (r < 0) return false;
    ok = r == 1;
    return true;
}

void RoomViewWorker::run() {
    while (true) {
        bool on = false;
        unsigned wish = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            on = queue_.front();
            queue_.pop_front();
            wish = wishes_;
        }
        std::string error;
        const bool ok = attempt(on, stopFd_, error);
        if (ok) {
            ++sent_;
        } else {
            std::fprintf(stderr, "[passthrough] Couldn't ask SteamVR to turn the room view %s: %s\n", on ? "on" : "off",
                         error.c_str());
        }
        ++finished_;
        bool latest = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            latest = wish == wishes_;  // else a newer wish is queued: its outcome is the one that counts
            if (latest) result_ = ok ? 1 : 0;
        }
        if (latest) poke(wakeFd_);
    }
}

// ---- The mailbox route ----

RoomViewMailbox::RoomViewMailbox(int port, std::string target, int wakeFd)
    : RoomViewWorker(wakeFd), port_(port), target_(std::move(target)) {}

bool RoomViewMailbox::attempt(bool on, int stopFd, std::string& error) {
    if (!roomview::exchange(port_, roomview::sendCommand(target_, roomview::enableMessage(on)), kLingerSec, stopFd,
                            nullptr, error)) {
        return false;
    }
    std::fprintf(stderr, "[passthrough] Asked SteamVR to turn the room view %s (mailbox %s)\n", on ? "on" : "off",
                 target_.c_str());
    return true;
}

// ---- The vrcmd route ----

RoomViewVrcmd::RoomViewVrcmd(std::string vrcmd, std::string cmdOn, std::string cmdOff, int wakeFd, double timeoutSec)
    : RoomViewWorker(wakeFd), vrcmd_(std::move(vrcmd)), cmdOn_(std::move(cmdOn)), cmdOff_(std::move(cmdOff)),
      timeoutSec_(timeoutSec) {}

bool RoomViewVrcmd::attempt(bool on, int stopFd, std::string& error) {
    const std::string& command = on ? cmdOn_ : cmdOff_;
    std::string output;
    const bool ok = roomview::runVrcmd(vrcmd_, command, timeoutSec_, stopFd, output, error);
    if (!output.empty()) std::fprintf(stderr, "[passthrough] vrcmd said: %s\n", firstLine(output).c_str());
    if (ok) {
        std::fprintf(stderr, "[passthrough] Asked SteamVR to turn the room view %s (vrcmd --compositorcmd %s)\n",
                     on ? "on" : "off", command.c_str());
    }
    return ok;
}

namespace roomview {

Route routeFromEnv(std::string& warning) {
    const auto env = [](const char* name) -> const char* {
        const char* value = std::getenv(name);
        return value != nullptr && value[0] != '\0' ? value : nullptr;
    };
    Route route;
    if (const char* kind = env("FRAMEDOCK_ROOMVIEW")) {
        const std::string k = kind;
        if (k == "mailbox") route.kind = Route::Kind::Mailbox;
        else if (k == "vrcmd") route.kind = Route::Kind::Vrcmd;
        else if (k != "openvr") warning = "FRAMEDOCK_ROOMVIEW=" + k + " is none of openvr, vrcmd, mailbox: openvr is used";
    }
    if (const char* v = env("FRAMEDOCK_ROOMVIEW_MAILBOX")) route.mailbox = v;
    if (const char* v = env("FRAMEDOCK_ROOMVIEW_CMD_ON")) route.cmdOn = v;
    if (const char* v = env("FRAMEDOCK_ROOMVIEW_CMD_OFF")) route.cmdOff = v;
    return route;
}

std::string describe(const Route& route) {
    if (route.kind == Route::Kind::Openvr) return "OpenVR's mailbox " + route.mailbox;
    if (route.kind == Route::Kind::Mailbox) return "mailbox " + route.mailbox;
    return "vrcmd --compositorcmd (on \"" + route.cmdOn + "\", off \"" + route.cmdOff + "\")";
}

std::unique_ptr<RoomViewControl> makeControl(const Route& route, int wakeFd, MailboxSend send) {
    if (route.kind == Route::Kind::Openvr) return std::make_unique<RoomViewDirect>(std::move(send), route.mailbox);
    if (route.kind == Route::Kind::Mailbox) return std::make_unique<RoomViewMailbox>(kWebPort, route.mailbox, wakeFd);
    return std::make_unique<RoomViewVrcmd>(route.vrcmd, route.cmdOn, route.cmdOff, wakeFd);
}

bool runVrcmd(const std::string& vrcmd, const std::string& command, double timeoutSec, int stopFd,
              std::string& output, std::string& error) {
    constexpr size_t kMaxOutput = 4096;
    int pipeFds[2];
    if (::pipe2(pipeFds, O_CLOEXEC) != 0) {
        error = std::string("pipe2: ") + std::strerror(errno);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], 1);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], 2);
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none;
    sigemptyset(&none);
    sigset_t defaults;
    sigemptyset(&defaults);
    for (const int s : {SIGTERM, SIGINT, SIGHUP, SIGUSR1, SIGUSR2, SIGPIPE, SIGCHLD}) sigaddset(&defaults, s);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);  // its own group: a timeout kills whatever it started too
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
    std::string arg0 = vrcmd;
    std::string flag = "--compositorcmd";
    std::string arg = command;
    char* argv[] = {arg0.data(), flag.data(), arg.data(), nullptr};
    pid_t pid = -1;
    const int spawned = ::posix_spawn(&pid, vrcmd.c_str(), &actions, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    ::close(pipeFds[1]);
    if (spawned != 0) {
        ::close(pipeFds[0]);
        error = "couldn't run " + vrcmd + ": " + std::strerror(spawned);
        return false;
    }
    const int pidFd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
    const double deadline = monotonic() + timeoutSec;
    bool exited = false;
    bool pipeOpen = true;
    int status = 0;
    while (true) {
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
            break;
        }
        const double left = deadline - monotonic();
        if (left <= 0) {
            error = "no answer in " + std::to_string(static_cast<int>(timeoutSec)) + " s";
            break;
        }
        struct pollfd fds[3] = {{pipeOpen ? pipeFds[0] : -1, POLLIN, 0}, {pidFd, POLLIN, 0}, {stopFd, POLLIN, 0}};
        // Without a pidfd (old kernels) the exit is seen at the next turn: short turns once the output has ended
        const int capMs = pidFd >= 0 || pipeOpen ? 1000 : 20;
        const int timeoutMs = std::min(capMs, static_cast<int>(std::ceil(left * 1000)));
        if (::poll(fds, 3, timeoutMs) < 0 && errno != EINTR) {
            error = std::string("poll: ") + std::strerror(errno);
            break;
        }
        if (fds[2].revents & POLLIN) {
            error = "shutting down";
            break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buffer[1024];
            const ssize_t n = ::read(pipeFds[0], buffer, sizeof(buffer));
            if (n > 0) {
                output.append(buffer, std::min(static_cast<size_t>(n), kMaxOutput - std::min(kMaxOutput, output.size())));
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                pipeOpen = false;
            }
        }
    }
    while (exited && pipeOpen && output.size() < kMaxOutput) {
        // What it wrote just before it ended (not waiting: something it started may still hold the pipe)
        struct pollfd p = {pipeFds[0], POLLIN, 0};
        if (::poll(&p, 1, 0) <= 0) break;
        char buffer[1024];
        const ssize_t n = ::read(pipeFds[0], buffer, sizeof(buffer));
        if (n <= 0) break;
        output.append(buffer, std::min(static_cast<size_t>(n), kMaxOutput - output.size()));
    }
    if (!exited) {
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
        (void)::waitpid(pid, &status, 0);  // reaped at once after SIGKILL
    }
    if (pidFd >= 0) ::close(pidFd);
    ::close(pipeFds[0]);
    if (!exited) return false;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
    error = WIFEXITED(status) ? "vrcmd exited with " + std::to_string(WEXITSTATUS(status))
                              : "vrcmd ended by signal " + std::to_string(WTERMSIG(status));
    return false;
}

}  // namespace roomview

namespace roomview {

bool exchange(int port, const std::string& command, double lingerSec, int stopFd, std::vector<std::string>* replies,
              std::string& error) {
    ws::Client client;
    if (!client.connect(port, "/")) {
        error = client.error();
        return false;
    }
    enum class Phase { Connecting, Opened, Sending, Lingering } phase = Phase::Connecting;
    double deadline = monotonic() + kExchangeSec;
    double phaseUntil = deadline;
    bool sent = false;
    int logged = 0;
    std::vector<std::string> messages;
    while (true) {
        const double now = monotonic();
        if (now >= deadline) {
            if (!sent) error = "no answer in time";
            break;
        }
        const double until = std::min(deadline, phaseUntil);
        struct pollfd fds[2] = {
            {client.fd(), static_cast<short>(POLLIN | (client.wantsWrite() ? POLLOUT : 0)), 0},
            {stopFd, POLLIN, 0},
        };
        const int timeoutMs = static_cast<int>(std::ceil(std::max(0.0, until - now) * 1000));
        if (::poll(fds, 2, timeoutMs) < 0) {
            error = "poll failed";
            break;
        }
        if (fds[1].revents & POLLIN) {
            error = "shutting down";
            break;
        }
        if (fds[0].revents & POLLOUT) client.onWritable();
        messages.clear();
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) client.onReadable(messages);
        for (const std::string& m : messages) {
            if (replies != nullptr) replies->push_back(m);
            if (logged++ < 3) std::fprintf(stderr, "[passthrough] SteamVR said: %.200s\n", m.c_str());
        }
        if (client.failed()) {
            if (!sent) error = client.error();
            break;
        }
        const double t = monotonic();
        if (phase == Phase::Connecting && client.open()) {
            client.sendText(std::string("mailbox_open ") + kOwnMailbox);
            phase = Phase::Opened;
            phaseUntil = t + kOpenWaitSec;
        }
        if (phase == Phase::Opened && (!messages.empty() || t >= phaseUntil)) {
            client.sendText(command);
            phase = Phase::Sending;
            phaseUntil = deadline;
        }
        if (phase == Phase::Sending && client.queued() == 0) {
            sent = true;
            deadline = std::max(deadline, t + lingerSec);
            phase = Phase::Lingering;  // a moment for the server to read it before the socket closes
            phaseUntil = t + lingerSec;
        }
        if (phase == Phase::Lingering && t >= phaseUntil) break;
    }
    client.close();
    return sent;
}

}  // namespace roomview

void RoomViewDirect::setEnabled(bool on) {
    // What SteamVR's own 룸 뷰 button sends (its toggle in the dashboard follows); off also clears the scene apps' one
    // (the previous version of this route set that, and it shows the view over a game)
    std::string error;
    const std::string message = roomview::enableMessage(on);
    bool ok = send_ && send_(target_, message, error);
    if (ok && !on) {
        const std::string scene = roomview::sceneAppsMessage(false);
        if (!send_(target_, scene, error)) std::fprintf(stderr, "[passthrough] Couldn't send %s: %s\n", scene.c_str(), error.c_str());
    }
    if (ok) std::fprintf(stderr, "[passthrough] Sent %s to %s\n", message.c_str(), target_.c_str());
    else std::fprintf(stderr, "[passthrough] Couldn't send the room view message: %s\n", error.c_str());
    result_ = ok ? 1 : 0;
}

bool RoomViewDirect::takeResult(bool& ok) {
    if (result_ < 0) return false;
    ok = result_ == 1;
    result_ = -1;
    return true;
}
