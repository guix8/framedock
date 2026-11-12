// The browser panel's Chrome process. See chrome_process.h.
#include "chrome_process.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>

namespace {

/** @return the program's full path (argv[0] searched in PATH like execvp, done before fork) */
std::string resolveProgram(const std::string& name) {
    if (name.find('/') != std::string::npos) return ::access(name.c_str(), X_OK) == 0 ? name : std::string();
    const char* path = std::getenv("PATH");
    std::string dirs = path != nullptr && *path != '\0' ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= dirs.size()) {
        const size_t end = std::min(dirs.find(':', start), dirs.size());
        std::string dir = dirs.substr(start, end - start);
        if (dir.empty()) dir = ".";
        const std::string full = dir + "/" + name;
        if (::access(full.c_str(), X_OK) == 0) return full;
        start = end + 1;
    }
    return {};
}

}  // namespace

std::vector<std::string> splitCommand(const std::string& command) {
    std::vector<std::string> out;
    std::string word;
    bool inWord = false;
    char quote = 0;
    for (const char c : command) {
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            } else {
                word += c;
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            inWord = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n') {
            if (inWord) out.push_back(word);
            word.clear();
            inWord = false;
            continue;
        }
        word += c;
        inWord = true;
    }
    if (inWord) out.push_back(word);
    return out;
}

bool isFlatpakRun(const std::vector<std::string>& command) {
    if (command.size() < 3) return false;
    const std::string& program = command[0];
    const size_t slash = program.rfind('/');
    const std::string base = slash == std::string::npos ? program : program.substr(slash + 1);
    return base == "flatpak" && command[1] == "run";
}

std::string chromeUserAgent(const std::string& lastVersion) {
    size_t digits = 0;
    while (digits < lastVersion.size() && digits < 4 && std::isdigit(static_cast<unsigned char>(lastVersion[digits]))) {
        ++digits;
    }
    if (digits == 0 || digits >= lastVersion.size() || lastVersion[digits] != '.') return "";
    return "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" +
           lastVersion.substr(0, digits) + ".0.0.0 Safari/537.36";
}

std::string acceptLanguages(const std::string& locale) {
    const std::string primary = locale.substr(0, locale.find('-'));
    std::string list = locale;
    if (primary != locale) list += "," + primary;
    if (primary != "en") list += ",en-US,en";
    return list;
}

std::vector<std::string> chromeArgv(const ChromeLaunch& launch) {
    std::vector<std::string> argv;
    if (isFlatpakRun(launch.command)) {
        // flatpak run [its options] APP [the app's arguments]: ours go right after "run"
        argv = {launch.command[0], launch.command[1], "--die-with-parent", "--filesystem=" + launch.profileDir};
        argv.insert(argv.end(), launch.command.begin() + 2, launch.command.end());
    } else {
        argv = launch.command;
    }
    const std::string size = std::to_string(launch.width) + "," + std::to_string(launch.height);
    argv.push_back("--user-data-dir=" + launch.profileDir);
    if (launch.transport == DevToolsTransport::Pipe) {
        argv.push_back("--remote-debugging-pipe");  // fds 3 and 4: nothing listens
    } else {
        argv.push_back("--remote-debugging-port=0");  // Chrome picks a free port and writes DevToolsActivePort
        argv.push_back("--remote-debugging-address=127.0.0.1");
    }
    // No --password-store=basic: in Game Mode (XDG_CURRENT_DESKTOP=gamescope) Chrome finds no desktop keyring and uses
    // its basic store anyway; under a KDE session it uses KWallet (SteamOS sets up an unlocked default wallet), which
    // keeps cookies and saved passwords under a wallet key instead of Chrome's fixed one. extra_args can add it back
    const std::vector<std::string> common = {
        "--no-first-run",
        "--no-default-browser-check",
        "--lang=" + launch.locale,
        "--accept-lang=" + acceptLanguages(launch.locale),
        "--window-size=" + size,
        // The panel is "hidden" to Chrome in either mode: keep pages running at full speed while we capture them
        "--disable-background-timer-throttling",
        "--disable-renderer-backgrounding",
        "--disable-backgrounding-occluded-windows",
        // Optimization Guide: no ML models (a fresh profile fetched 41 MB of them within 90 s), no hint lists or hint
        // fetches. Not --disable-component-update: that would also stop CRLSet, PKI metadata and the like
        "--disable-features=Translate,MediaRouter,OptimizationGuideModelDownloading,OptimizationHints,"
        "OptimizationHintsFetching,OptimizationTargetPrediction",
        // navigator.webdriver stays false: a page (Google's sign-in) takes true for a bot and refuses it
        "--disable-blink-features=AutomationControlled",
        // The HTTP cache at 64 MB (unset, Chrome sizes it from the free disk: the profile was 332 MB in two days)
        "--disk-cache-size=67108864",
    };
    argv.insert(argv.end(), common.begin(), common.end());
    if (!launch.userAgent.empty()) argv.push_back("--user-agent=" + launch.userAgent);
    if (launch.headless) {
        argv.push_back("--headless=new");
        // The screen is the page's size too (headless Chrome's own is 800×600): a page gone fullscreen (YouTube's ⛶)
        // takes the screen, and with a smaller one the panel showed only its top left, enlarged
        argv.push_back("--screen-info={0,0 " + std::to_string(launch.width) + "x" + std::to_string(launch.height) + "}");
    } else {
        argv.push_back("--window-position=-32000,-32000");
    }
    argv.insert(argv.end(), launch.extraArgs.begin(), launch.extraArgs.end());
    argv.push_back("about:blank");
    return argv;
}

ChromeProcess::~ChromeProcess() {
    if (pid_ > 0) killNow();
    reapLeftovers();
    closePidfd();
    if (logFd_ >= 0) ::close(logFd_);
}

bool makePrivateDir(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st {};
    bool changed = false;
    if (::fstat(fd, &st) == 0 && st.st_uid == ::getuid() && (st.st_mode & 077) != 0) {
        changed = ::fchmod(fd, st.st_mode & 0700) == 0;
    }
    ::close(fd);
    return changed;
}

bool ChromeProcess::start(const std::vector<std::string>& argv, const std::string& logPath, std::string& error,
                          int devtoolsFd) {
    if (pid_ > 0) {
        error = "already running";
        return false;
    }
    if (argv.empty()) {
        error = "no command";
        return false;
    }
    const std::string program = resolveProgram(argv[0]);
    if (program.empty()) {
        error = "not found: " + argv[0];
        return false;
    }
    // Everything the child needs is made before fork(): after it only async-signal-safe calls
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    reapLeftovers();
    // The last start's log is kept beside (one generation); this one is appended to, so capLog can cut it back
    if (!logPath.empty()) ::rename(logPath.c_str(), (logPath + ".1").c_str());
    const int out = logPath.empty() ? ::open("/dev/null", O_WRONLY | O_CLOEXEC)
                                    : ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0600);
    if (out < 0) {
        error = "log " + logPath + ": " + std::strerror(errno);
        return false;
    }
    const int in = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork: ") + std::strerror(errno);
        ::close(out);
        if (in >= 0) ::close(in);
        return false;
    }
    if (pid == 0) {
        // Dies with us (this thread, precisely). If the parent died before this line, go now
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (::getppid() != parent) ::_exit(127);
        ::setpgid(0, 0);  // its own group: signalGroup() reaches every process it starts
        if (in >= 0) ::dup2(in, 0);
        ::dup2(out, 1);
        ::dup2(out, 2);
        // The DevTools pipe: our socketpair's other end as fd 3 (Chrome reads) and fd 4 (Chrome writes). Moved up
        // first so that putting it on 3 can't close it
        unsigned firstToClose = 3;
        if (devtoolsFd >= 0) {
            const int high = ::fcntl(devtoolsFd, F_DUPFD, 10);
            if (high < 0 || ::dup2(high, 3) != 3 || ::dup2(high, 4) != 4) ::_exit(126);
            firstToClose = 5;
        }
        // Nothing else of ours leaks into Chrome (the OpenVR socket, the lock file, eventfds)
        if (::syscall(SYS_close_range, firstToClose, ~0u, 0u) != 0) {
            for (int fd = static_cast<int>(firstToClose); fd < 4096; ++fd) ::close(fd);
        }
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        for (const int s : {SIGTERM, SIGINT, SIGUSR1, SIGUSR2, SIGPIPE, SIGCHLD}) ::signal(s, SIG_DFL);
        ::execv(program.c_str(), args.data());
        ::_exit(127);
    }
    ::setpgid(pid, pid);  // also from here, so signalGroup works even if the child hasn't run yet
    if (logFd_ >= 0) ::close(logFd_);
    logFd_ = logPath.empty() ? -1 : out;  // kept: the same open file as Chrome's stdout / stderr
    if (logFd_ < 0) ::close(out);
    if (in >= 0) ::close(in);
    pid_ = pid;
    status_.clear();
    closePidfd();
    pidfd_ = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));  // -1 on an old kernel: the owner polls instead
    if (pidfd_ >= 0) ::fcntl(pidfd_, F_SETFD, FD_CLOEXEC);
    return true;
}

bool ChromeProcess::poll() {
    if (pid_ <= 0) return false;
    int status = 0;
    const pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r == 0) return false;
    if (r < 0 && errno == EINTR) return false;
    if (r < 0) {
        status_ = "lost (" + std::string(std::strerror(errno)) + ")";
    } else if (WIFEXITED(status)) {
        status_ = "exit " + std::to_string(WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        status_ = "signal " + std::to_string(WTERMSIG(status));
    } else {
        status_ = "ended";
    }
    // Whatever it left in its group goes too (a helper that outlived the browser process)
    ::kill(-pid_, SIGKILL);
    pid_ = -1;
    closePidfd();
    return true;
}

void ChromeProcess::closePidfd() {
    if (pidfd_ >= 0) ::close(pidfd_);
    pidfd_ = -1;
}

void ChromeProcess::signalGroup(int signal) {
    if (pid_ <= 0) return;
    if (::kill(-pid_, signal) != 0) ::kill(pid_, signal);
}

void ChromeProcess::killNow() {
    if (pid_ <= 0) return;
    ::kill(-pid_, SIGKILL);
    ::kill(pid_, SIGKILL);
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(killWaitSec);
    while (pid_ > 0) {
        if (poll()) return;
        if (std::chrono::steady_clock::now() >= end) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (pid_ > 0) {
        // Still not reaped (a child stuck in D state): not forgotten. Reaped later (reapLeftovers, never blocking),
        // so no zombie stays behind for the rest of the run
        std::fprintf(stderr, "[browser] Chrome (PID %d) didn't go within %.1f s of SIGKILL; reaping it later\n",
                     static_cast<int>(pid_), killWaitSec);
        leftovers_.push_back(pid_);
        pid_ = -1;
        closePidfd();
    }
}

void ChromeProcess::reapLeftovers() {
    for (auto it = leftovers_.begin(); it != leftovers_.end();) {
        // Whatever its group still has, signalled before the reap: until we reap the leader its PID (and so the
        // group's id) can't be anyone else's. After the reap it could be reused, and kill(-pid) could hit a stranger
        ::kill(-*it, SIGKILL);
        int status = 0;
        const pid_t r = ::waitpid(*it, &status, WNOHANG);
        if (r == *it || (r < 0 && errno == ECHILD)) {
            it = leftovers_.erase(it);
        } else {
            ++it;
        }
    }
}

bool ChromeProcess::capLog(size_t maxBytes) {
    if (logFd_ < 0) return false;
    struct stat st {};
    if (::fstat(logFd_, &st) != 0 || static_cast<size_t>(st.st_size) <= maxBytes) return false;
    if (::ftruncate(logFd_, 0) != 0) return false;
    const std::string line = "[framedock] chrome.log passed " + std::to_string(maxBytes >> 20) + " MB: cut back here\n";
    (void)!::write(logFd_, line.data(), line.size());
    return true;
}

bool namesProfile(const std::string& arg, const std::string& profileDir) {
    if (profileDir.empty() || arg.size() < 3 || arg.compare(0, 2, "--") != 0) return false;
    const size_t eq = arg.find('=');
    if (eq == std::string::npos || eq < 3) return false;
    const size_t at = eq + 1, n = profileDir.size();
    if (arg.compare(at, n, profileDir) != 0) return false;
    return arg.size() == at + n || arg[at + n] == '/';
}

long processRealUid(pid_t pid) {
    std::ifstream file("/proc/" + std::to_string(static_cast<long>(pid)) + "/status");
    std::string line;
    while (std::getline(file, line)) {
        if (line.compare(0, 4, "Uid:") != 0) continue;
        char* end = nullptr;
        const long uid = std::strtol(line.c_str() + 4, &end, 10);
        return end != line.c_str() + 4 ? uid : -1;
    }
    return -1;
}

std::vector<pid_t> findProfileProcesses(const std::string& profileDir) {
    std::vector<pid_t> found;
    if (profileDir.empty()) return found;
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return found;
    const long me = static_cast<long>(::getuid());
    while (const dirent* entry = ::readdir(proc)) {
        char* end = nullptr;
        const long pid = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || pid <= 1 || pid == ::getpid()) continue;
        // Not stat(/proc/N): a non-dumpable process's directory belongs to root
        if (processRealUid(static_cast<pid_t>(pid)) != me) continue;
        std::ifstream file(std::string("/proc/") + entry->d_name + "/cmdline", std::ios::binary);
        const std::string cmdline((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        size_t start = 0;
        while (start < cmdline.size()) {
            const size_t zero = std::min(cmdline.find('\0', start), cmdline.size());
            if (namesProfile(cmdline.substr(start, zero - start), profileDir)) {
                found.push_back(static_cast<pid_t>(pid));
                break;
            }
            start = zero + 1;
        }
    }
    ::closedir(proc);
    return found;
}

int killProfileProcesses(const std::string& profileDir) {
    const std::vector<pid_t> first = findProfileProcesses(profileDir);
    for (const pid_t pid : first) ::kill(pid, SIGTERM);
    for (int i = 0; i < 25 && !findProfileProcesses(profileDir).empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    for (const pid_t pid : findProfileProcesses(profileDir)) ::kill(pid, SIGKILL);
    return static_cast<int>(first.size());
}

int removeStaleSingletons(const std::string& profileDir) {
    if (profileDir.empty() || !findProfileProcesses(profileDir).empty()) return 0;
    int removed = 0;
    for (const char* name : {"SingletonLock", "SingletonSocket", "SingletonCookie"}) {
        // Symlinks (to "host-pid", a socket, a cookie): unlink removes the link, never what it points at
        if (::unlink((profileDir + "/" + name).c_str()) == 0) ++removed;
    }
    return removed;
}
