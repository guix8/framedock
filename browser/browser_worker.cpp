// The browser session's thread. See browser_worker.h.
#include "browser_worker.h"

#include "app_text.h"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

double steadyNow() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int BrowserWorker::failWakeForTest = 0;

BrowserWorker::BrowserWorker(SessionOptions options, int notifyFd)
    : session_(std::move(options)), notifyFd_(notifyFd) {
    session_.log = [](const std::string& line) { std::fprintf(stderr, "[browser] %s\n", line.c_str()); };
    std::string error;
    if (!makeCommandFds(error)) {
        // Without a way to wake it the thread could sleep in poll() forever and shutdown() would hang on join: it
        // doesn't start at all, and the panel says so
        session_.failWithoutStart(trf(Tx::BrowserThreadFailed, {error}), steadyNow());
        status_ = session_.status();
        ++statusVersion_;
        return;
    }
    thread_ = std::thread([this] { run(); });
    started_ = true;
}

BrowserWorker::~BrowserWorker() {
    shutdown(shutdownTimeout_);
    if (commandWriteFd_ >= 0 && commandWriteFd_ != commandFd_) ::close(commandWriteFd_);
    if (commandFd_ >= 0) ::close(commandFd_);
}

bool BrowserWorker::makeCommandFds(std::string& error) {
    commandFd_ = failWakeForTest >= 1 ? -1 : ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (commandFd_ >= 0) {
        commandWriteFd_ = commandFd_;
        return true;
    }
    error = std::string("eventfd: ") + std::strerror(failWakeForTest >= 1 ? EMFILE : errno);
    std::fprintf(stderr, "[browser] %s; using a pipe to wake Chrome's thread\n", error.c_str());
    int fds[2] = {-1, -1};
    if (failWakeForTest >= 2 || ::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        error += std::string(", pipe: ") + std::strerror(failWakeForTest >= 2 ? EMFILE : errno);
        std::fprintf(stderr, "[browser] %s: Chrome's thread doesn't start\n", error.c_str());
        return false;
    }
    commandFd_ = fds[0];
    commandWriteFd_ = fds[1];
    return true;
}

void BrowserWorker::wake(int fd) {
    if (fd < 0) return;
    const uint64_t one = 1;
    (void)!::write(fd, &one, sizeof(one));  // a full pipe (EAGAIN) is already a pending wake
}

void BrowserWorker::drainCommands() {
    uint64_t value[8];
    while (::read(commandFd_, value, sizeof(value)) > 0) {
    }
}

void BrowserWorker::post(std::function<void(BrowserSession&, double)> fn) {
    if (!started_) return;  // no thread: nothing would ever run it
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.push_back(std::move(fn));
    }
    wake(commandWriteFd_);
}

BrowserSession::Status BrowserWorker::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool BrowserWorker::takeFrame(BgraFrame& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sharedNew_) return false;
    std::swap(out, shared_);
    sharedNew_ = false;
    return true;
}

void BrowserWorker::shutdown(double timeoutSec) {
    if (!thread_.joinable()) return;
    shutdownTimeout_ = timeoutSec;
    quit_ = true;
    wake(commandWriteFd_);
    thread_.join();
}

void BrowserWorker::publish() {
    bool changed = false;
    const bool frame = session_.takeFrame(local_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!session_.status().sameAs(status_)) {
            status_ = session_.status();
            changed = true;
        }
        if (frame) {
            std::swap(shared_, local_);
            sharedNew_ = true;
        }
        if (status_.phase == BrowserSession::Phase::Off || status_.phase == BrowserSession::Phase::Failed) {
            // Chrome stopped: the frames handed on go (the main loop lets go of its own too)
            releasePixels(local_);
            releasePixels(shared_);
            sharedNew_ = false;
        }
    }
    pid_ = session_.pid();
    if (changed) ++statusVersion_;
    if (changed || frame) wake(notifyFd_);
}

void BrowserWorker::run() {
    std::vector<pollfd> fds;
    std::vector<std::function<void(BrowserSession&, double)>> batch;
    while (!quit_) {
        fds.clear();
        fds.push_back({commandFd_, POLLIN, 0});
        session_.fds(fds);
        const double now = steadyNow();
        const double wakeAt = session_.nextWake(now);
        int timeoutMs = -1;
        if (std::isfinite(wakeAt)) timeoutMs = static_cast<int>(std::ceil(std::max(0.0, wakeAt - now) * 1000.0));
        const int n = ::poll(fds.data(), fds.size(), timeoutMs);
        const double t = steadyNow();
        if (n > 0 && (fds[0].revents & POLLIN)) drainCommands();
        // The socket's events first (they belong to the fds just polled), then the commands
        if (n > 0) session_.onPoll(fds, 1, t);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            batch.swap(commands_);
        }
        for (auto& fn : batch) fn(session_, t);
        batch.clear();
        session_.tick(steadyNow());
        publish();
    }
    session_.shutdownBlocking(shutdownTimeout_);
    publish();
}
