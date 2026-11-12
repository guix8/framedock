// framecorder's recorder as a child. See framecorder.h.
#include "framecorder.h"

#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <thread>

extern char** environ;

namespace framecorder {

namespace {

constexpr const char* kName = "framecorder";

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool isInterpreter(const std::string& name) {
    return name.rfind("python", 0) == 0 || name == "sh" || name == "bash";
}

bool readSmall(const std::string& path, std::string& out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buffer[4096];
    const ssize_t n = ::read(fd, buffer, sizeof(buffer));
    ::close(fd);
    if (n < 0) return false;
    out.assign(buffer, static_cast<size_t>(n));
    return true;
}

double steadyNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

std::string recorderPath() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') return "";
    return std::string(home) + "/.local/bin/framecorder";
}

std::string uiConfPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] == '/') return std::string(xdg) + "/framecorder/ui.conf";
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') return "";
    return std::string(home) + "/.config/framecorder/ui.conf";
}

bool isInstalled(const std::string& path) {
    struct stat st {};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

std::vector<std::string> argsFromUiConf(const std::string& text) {
    std::map<std::string, std::string> conf;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        conf[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    std::vector<std::string> args;
    const auto value = [&](const char* key) {
        const auto it = conf.find(key);
        return it == conf.end() ? std::string() : it->second;
    };
    const std::string shape = value("shape");
    const char* aspect = shape == "square" ? "1:1" : shape == "wide" ? "16:9" : shape == "tall" ? "9:16" : nullptr;
    if (aspect != nullptr) args.insert(args.end(), {"--aspect", aspect});
    const std::string eye = value("eye");
    if (eye == "left" || eye == "right") args.insert(args.end(), {"--eye", eye});
    const std::string fps = value("fps");
    if (!fps.empty() && fps.size() <= 3 && fps.find_first_not_of("0123456789") == std::string::npos && fps != "0") {
        args.insert(args.end(), {"--fps", fps});
    }
    if (value("game_audio") == "false") args.push_back("--no-audio");
    if (value("mic") == "true") args.push_back("--mic");
    return args;
}

bool isRecorderProcess(const std::string& comm, const std::vector<std::string>& argv) {
    if (comm == kName) return true;
    if (argv.empty()) return false;
    const std::string first = baseName(argv[0]);
    if (first == kName) return true;
    return isInterpreter(first) && argv.size() > 1 && baseName(argv[1]) == kName;
}

pid_t findRecorder(const std::string& procRoot, pid_t exceptPid) {
    DIR* dir = ::opendir(procRoot.c_str());
    if (dir == nullptr) return 0;
    pid_t found = 0;
    while (const dirent* entry = ::readdir(dir)) {
        char* end = nullptr;
        const long pid = std::strtol(entry->d_name, &end, 10);
        if (pid <= 0 || *end != '\0' || pid == exceptPid) continue;
        const std::string base = procRoot + "/" + entry->d_name;
        std::string comm, cmdline;
        if (!readSmall(base + "/comm", comm)) continue;  // gone, or not readable
        comm = trim(comm);
        readSmall(base + "/cmdline", cmdline);  // empty for a kernel thread
        std::vector<std::string> argv;
        size_t at = 0;
        while (at < cmdline.size() && argv.size() < 2) {
            const size_t nul = cmdline.find('\0', at);
            const size_t stop = nul == std::string::npos ? cmdline.size() : nul;
            argv.push_back(cmdline.substr(at, stop - at));
            at = stop + 1;
        }
        if (isRecorderProcess(comm, argv)) {
            found = static_cast<pid_t>(pid);
            break;
        }
    }
    ::closedir(dir);
    return found;
}

Recorder::Recorder(std::string program, std::string uiConf, std::string procRoot)
    : program_(std::move(program)), uiConf_(std::move(uiConf)), procRoot_(std::move(procRoot)) {
    available_ = isInstalled(program_);
}

Recorder::~Recorder() {
    if (child_ <= 0) return;
    std::fprintf(stderr, "[record] Frame Dock is exiting: stopping the recording (PID %d)\n", static_cast<int>(child_));
    if (state_ == State::Recording) {
        ::kill(-child_, SIGINT);
        ::kill(child_, SIGINT);
    }
    int status = 0;
    const double until = steadyNow() + kExitWaitSec;
    while (steadyNow() < until) {
        if (reap(status)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::kill(-child_, SIGKILL);
    ::kill(child_, SIGKILL);
    (void)::waitpid(child_, &status, 0);
    std::fprintf(stderr, "[record] The recorder didn't end in %.0f s: killed\n", kExitWaitSec);
    child_ = -1;
}

bool Recorder::reap(int& status) {
    if (child_ <= 0) return true;
    const pid_t done = ::waitpid(child_, &status, WNOHANG);
    if (done == 0) return false;
    if (done < 0 && errno == EINTR) return false;
    child_ = -1;  // reaped (or not ours any more: ECHILD)
    return true;
}

void Recorder::note(State state, double now) {
    state_ = state;
    noteUntil_ = now + kNoteSec;
}

bool Recorder::start(double now) {
    const pid_t other = findRecorder(procRoot_, 0);
    if (other > 0) {
        std::fprintf(stderr, "[record] Not started: a framecorder recorder runs already (PID %d)\n",
                     static_cast<int>(other));
        note(State::Busy, now);
        return true;
    }
    std::string conf;
    if (!uiConf_.empty()) {
        std::ifstream file(uiConf_, std::ios::binary);
        if (file) conf.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    std::vector<std::string> args = argsFromUiConf(conf);
    args.insert(args.begin(), program_);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, 2, 1);  // what it says goes to Frame Dock's log
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
    posix_spawnattr_setpgroup(&attr, 0);  // its own group: the stop reaches whatever it started too
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
    pid_t pid = -1;
    const int spawned = ::posix_spawn(&pid, program_.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    std::string line;
    for (size_t i = 1; i < args.size(); ++i) line += " " + args[i];
    if (spawned != 0) {
        std::fprintf(stderr, "[record] Couldn't run %s: %s\n", program_.c_str(), std::strerror(spawned));
        note(State::Failed, now);
        return true;
    }
    std::fprintf(stderr, "[record] Recording: %s%s (PID %d)\n", program_.c_str(), line.c_str(), static_cast<int>(pid));
    child_ = pid;
    state_ = State::Recording;
    startedAt_ = now;
    shownSec_ = 0;
    return true;
}

bool Recorder::toggle(double now) {
    if (!available_) return false;
    switch (state_) {
        case State::Recording:
            ::kill(-child_, SIGINT);  // its "ctrl+c to stop": it saves the file
            ::kill(child_, SIGINT);
            state_ = State::Saving;
            stopAt_ = now + kStopSec;
            std::fprintf(stderr, "[record] Stopping after %d s (SIGINT)\n", elapsedSec(now));
            return true;
        case State::Saving: return false;
        case State::Idle:
        case State::Failed:
        case State::Busy: return start(now);
    }
    return false;
}

bool Recorder::tick(double now) {
    bool changed = false;
    if (child_ > 0) {
        int status = 0;
        if (reap(status)) {
            if (state_ == State::Saving) {
                std::fprintf(stderr, "[record] The recorder ended (%s %d)\n",
                             WIFSIGNALED(status) ? "signal" : "exit code",
                             WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
                state_ = State::Idle;
            } else {
                std::fprintf(stderr, "[record] The recorder ended by itself after %d s (%s %d)\n", elapsedSec(now),
                             WIFSIGNALED(status) ? "signal" : "exit code",
                             WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
                note(State::Failed, now);
            }
            changed = true;
        } else if (state_ == State::Saving && now >= stopAt_) {
            std::fprintf(stderr, "[record] The recorder didn't end in %.0f s: SIGKILL\n", kStopSec);
            ::kill(-child_, SIGKILL);
            ::kill(child_, SIGKILL);
            stopAt_ = std::numeric_limits<double>::infinity();  // reaped at the next tick
        }
    }
    if ((state_ == State::Failed || state_ == State::Busy) && now >= noteUntil_) {
        state_ = State::Idle;
        changed = true;
    }
    if (state_ == State::Recording && elapsedSec(now) != shownSec_) {
        shownSec_ = elapsedSec(now);
        changed = true;
    }
    return changed;
}

double Recorder::nextWake(double now) const {
    const double inf = std::numeric_limits<double>::infinity();
    switch (state_) {
        case State::Recording: {
            // The next whole second (the card's clock); it also notices the recorder ending by itself
            const double next = startedAt_ + elapsedSec(now) + 1;
            return next > now ? next : now + 1;
        }
        case State::Saving: return std::min(stopAt_, now + 0.25);  // its end, polled
        case State::Failed:
        case State::Busy: return noteUntil_;
        case State::Idle: break;
    }
    return inf;
}

int Recorder::elapsedSec(double now) const {
    if (state_ != State::Recording && state_ != State::Saving) return 0;
    return now > startedAt_ ? static_cast<int>(now - startedAt_) : 0;
}

}  // namespace framecorder
