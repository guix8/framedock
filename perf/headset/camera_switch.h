// Camera Switch (Arcturus Vision Camera's helper, the systemd user unit arcturus-camera-switch.service) over D-Bus.
// Its GApplication owns com.arcturusvision.CameraPreview on the session bus and exports org.gtk.Actions on
// /com/arcturusvision/CameraPreview with one action, set-color-passthrough(b): stateless (DescribeAll says
// (true, 'b', [])), so the colour in use can't be read back. Running = the name has an owner (GetNameOwner once,
// then NameOwnerChanged).
//
// sd-bus on its own thread: it sleeps in poll() on the bus and a command eventfd. The main loop is poked only when
// availability changed while wanted. The connection opens the first time the page is shown and stays (the signal
// only comes when Camera Switch starts or stops). If the bus fails the thread ends (Camera Switch then counts as not
// running) and the next setWanted(true) starts it again.
#pragma once

#include "device_backend.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/** Whether the bus name has an owner, from GetNameOwner's answer and NameOwnerChanged, in the order they came. */
class NameOwnerTracker {
public:
    /** GetNameOwner answered: an owner (true) or org.freedesktop.DBus.Error.NameHasNoOwner (false). */
    void answer(bool owned) { set(owned); }
    /** NameOwnerChanged for the name. @param newOwner "" = it went away */
    void changed(const std::string& newOwner) { set(!newOwner.empty()); }
    /** @return true (and the state) once per change; the first answer counts as one */
    bool take(bool& available) {
        if (!changed_) return false;
        changed_ = false;
        available = owned_.value_or(false);
        return true;
    }
    /** The bus connection was lost: not known any more (a change if it was running). */
    void lost() {
        if (owned_.value_or(false)) changed_ = true;
        owned_.reset();
    }
    std::optional<bool> owned() const { return owned_; }

private:
    std::optional<bool> owned_;
    bool changed_ = false;
    void set(bool owned) {
        if (owned_ == owned) return;
        owned_ = owned;
        changed_ = true;
    }
};

class CameraSwitchDbus : public CameraSwitchService {
public:
    static constexpr const char* kName = "com.arcturusvision.CameraPreview";
    static constexpr const char* kPath = "/com/arcturusvision/CameraPreview";
    static constexpr const char* kAction = "set-color-passthrough";

    /**
     * @param wakeFd written when availability changes while wanted (−1: none)
     * @param colorFile where a colour Camera Switch accepted is written down ("" = nowhere)
     */
    CameraSwitchDbus(int wakeFd, std::string colorFile);
    ~CameraSwitchDbus() override;  ///< joins the thread
    CameraSwitchDbus(const CameraSwitchDbus&) = delete;
    CameraSwitchDbus& operator=(const CameraSwitchDbus&) = delete;

    void setWanted(bool wanted) override;
    bool takeAvailable(bool& available) override;
    void setColor(bool color) override;

    /** Tests: the Activate calls answered so far (true = it went through). */
    int callsAnswered() const { return answered_.load(); }

    // sd-bus callbacks (its thread)
    void onOwner(bool owned);
    void onOwnerChanged(const std::string& newOwner);
    void onActivated(bool ok, const std::string& error);

private:
    int wakeFd_;
    std::string colorFile_;
    int commandFd_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_ {false};
    std::atomic<bool> wanted_ {false};
    std::atomic<bool> running_ {false};  ///< the thread is in run() (false once it ended on its own)
    std::atomic<int> answered_ {0};
    std::mutex mutex_;
    NameOwnerTracker tracker_;       ///< guarded by mutex_
    std::vector<bool> commands_;     ///< colours to send, guarded by mutex_
    std::deque<bool> asked_;         ///< colours sent and not answered yet (the thread's own)

    void run();
    /** run()'s body; returns when stopping or when the bus failed */
    void serve();
    void wake();
};
