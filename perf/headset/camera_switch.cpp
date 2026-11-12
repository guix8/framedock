// Camera Switch over D-Bus (sd-bus). See camera_switch.h.
#include "camera_switch.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace {

CameraSwitchDbus* self(void* user) {
    return static_cast<CameraSwitchDbus*>(user);
}

int ownerReply(sd_bus_message* reply, void* user, sd_bus_error*) {
    // An error answer (NameHasNoOwner) means it doesn't run
    self(user)->onOwner(!sd_bus_message_is_method_error(reply, nullptr));
    return 0;
}

int ownerChanged(sd_bus_message* message, void* user, sd_bus_error*) {
    const char* name = nullptr;
    const char* oldOwner = nullptr;
    const char* newOwner = nullptr;
    if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) < 0 || name == nullptr) return 0;
    if (std::strcmp(name, CameraSwitchDbus::kName) == 0) self(user)->onOwnerChanged(newOwner != nullptr ? newOwner : "");
    return 0;
}

int activateReply(sd_bus_message* reply, void* user, sd_bus_error*) {
    const sd_bus_error* error = sd_bus_message_get_error(reply);
    self(user)->onActivated(error == nullptr, error != nullptr && error->message != nullptr ? error->message : "");
    return 0;
}

}  // namespace

CameraSwitchDbus::CameraSwitchDbus(int wakeFd, std::string colorFile)
    : wakeFd_(wakeFd), colorFile_(std::move(colorFile)) {}

CameraSwitchDbus::~CameraSwitchDbus() {
    stop_ = true;
    if (commandFd_ >= 0) {
        const uint64_t one = 1;
        (void)!::write(commandFd_, &one, sizeof(one));
    }
    if (thread_.joinable()) thread_.join();
    if (commandFd_ >= 0) ::close(commandFd_);
}

void CameraSwitchDbus::setWanted(bool wanted) {
    wanted_ = wanted;
    if (!wanted) return;
    if (thread_.joinable()) {
        if (running_) return;
        thread_.join();  // it ended after a bus failure: start again
    }
    if (commandFd_ < 0) commandFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (commandFd_ < 0) {
        std::fprintf(stderr, "[passthrough] eventfd failed: %s\n", std::strerror(errno));
        return;
    }
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

bool CameraSwitchDbus::takeAvailable(bool& available) {
    std::lock_guard<std::mutex> lock(mutex_);
    return tracker_.take(available);
}

void CameraSwitchDbus::setColor(bool color) {
    if (commandFd_ < 0) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.push_back(color);
    }
    const uint64_t one = 1;
    (void)!::write(commandFd_, &one, sizeof(one));
}

void CameraSwitchDbus::wake() {
    if (wanted_ && wakeFd_ >= 0) {
        const uint64_t one = 1;
        (void)!::write(wakeFd_, &one, sizeof(one));
    }
}

void CameraSwitchDbus::onOwner(bool owned) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tracker_.answer(owned);
    }
    std::fprintf(stderr, "[passthrough] Camera Switch %s\n", owned ? "runs" : "doesn't run");
    wake();
}

void CameraSwitchDbus::onOwnerChanged(const std::string& newOwner) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tracker_.changed(newOwner);
    }
    std::fprintf(stderr, "[passthrough] Camera Switch %s\n", newOwner.empty() ? "stopped" : "started");
    wake();
}

void CameraSwitchDbus::onActivated(bool ok, const std::string& error) {
    // Answers come in the order of the calls (one peer)
    const bool color = !asked_.empty() && asked_.front();
    if (!asked_.empty()) asked_.pop_front();
    if (!ok) {
        std::fprintf(stderr, "[passthrough] Camera Switch refused the colour change: %s\n", error.c_str());
    } else if (!colorFile_.empty()) {
        std::string why;
        if (!writeRememberedColor(colorFile_, color, why)) {
            std::fprintf(stderr, "[passthrough] Couldn't write down the colour: %s\n", why.c_str());
        }
    }
    ++answered_;
}

void CameraSwitchDbus::run() {
    asked_.clear();
    serve();
    if (!stop_) {
        // The bus is gone: Camera Switch can't be reached until the page is shown again (setWanted starts us again)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tracker_.lost();
            commands_.clear();
        }
        wake();
    }
    running_ = false;
}

void CameraSwitchDbus::serve() {
    sd_bus* bus = nullptr;
    int r = sd_bus_open_user(&bus);
    if (r < 0) {
        std::fprintf(stderr, "[passthrough] Couldn't reach the session bus: %s\n", std::strerror(-r));
        return;
    }
    // The signal first, then the question: an answer can't miss a change made in between
    r = sd_bus_add_match_async(bus, nullptr,
                               "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                               "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                               "arg0='com.arcturusvision.CameraPreview'",
                               ownerChanged, nullptr, this);
    if (r >= 0) {
        r = sd_bus_call_method_async(bus, nullptr, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                     "org.freedesktop.DBus", "GetNameOwner", ownerReply, this, "s", kName);
    }
    if (r < 0) std::fprintf(stderr, "[passthrough] D-Bus setup failed: %s\n", std::strerror(-r));
    while (!stop_) {
        // Commands: Activate("set-color-passthrough", [<b>], {}), not waited for (the answer is only logged)
        std::vector<bool> colors;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            colors.swap(commands_);
        }
        for (const bool color : colors) {
            r = sd_bus_call_method_async(bus, nullptr, kName, kPath, "org.gtk.Actions", "Activate", activateReply,
                                         this, "sava{sv}", kAction, 1, "b", color ? 1 : 0, 0);
            if (r < 0) {
                std::fprintf(stderr, "[passthrough] Calling Camera Switch failed: %s\n", std::strerror(-r));
            } else {
                asked_.push_back(color);
                std::fprintf(stderr, "[passthrough] Asked Camera Switch for %s\n", color ? "colour" : "black and white");
            }
        }
        while ((r = sd_bus_process(bus, nullptr)) > 0) {
        }
        if (r < 0) {
            std::fprintf(stderr, "[passthrough] The session bus failed: %s\n", std::strerror(-r));
            break;
        }
        uint64_t usec = UINT64_MAX;
        int timeoutMs = -1;
        if (sd_bus_get_timeout(bus, &usec) >= 0 && usec != UINT64_MAX) {
            struct timespec ts {};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const uint64_t nowUsec = static_cast<uint64_t>(ts.tv_sec) * 1000000u + ts.tv_nsec / 1000;
            timeoutMs = usec > nowUsec ? static_cast<int>((usec - nowUsec + 999) / 1000) : 0;
        }
        struct pollfd fds[2] = {{sd_bus_get_fd(bus), static_cast<short>(sd_bus_get_events(bus)), 0},
                                {commandFd_, POLLIN, 0}};
        if (::poll(fds, 2, timeoutMs) < 0 && errno != EINTR) break;
        if (fds[1].revents & POLLIN) {
            uint64_t value = 0;
            (void)!::read(commandFd_, &value, sizeof(value));
        }
    }
    sd_bus_flush_close_unref(bus);
}
