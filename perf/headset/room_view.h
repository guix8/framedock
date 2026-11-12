// Turning SteamVR's room view (the passthrough) on and off. OpenVR has no public call for it (2.15.6): the dashboard
// calls VRHTML.VROverlayInternal.SetCameraRoomViewEnabled (an internal interface), and vrcompositor takes mailbox
// messages. Three routes, picked with FRAMEDOCK_ROOMVIEW (roomview::routeFromEnv):
//
// - openvr (the default): mailbox messages to "vrcompositor_systemlayer" over OpenVR's internal IVRMailbox_001
//   (VR_GetGenericInterface; VrOverlay::sendMailbox, on the app's own SteamVR connection):
//   {"type":"set_camera_roomview_enabled","enabled":<bool>}, what SteamVR's own 룸 뷰 button (VROverlayInternal.
//   SetCameraRoomViewEnabled) sets. It sets the view, no toggle. Live (2026-11-01 19:11, SteamVR 2.15 / build
//   1791331621, in SteamVR's home with no game): false turned the passthrough off and the 룸 뷰 button in SteamVR's
//   menu went off with it, true turned both on; no VREvent_RoomViewShown / Hidden either way, so the app can't see
//   it changed (nor the button pressed in SteamVR). Off also sends set_camera_roomview_enabled_forsceneapps false:
//   that one (sent alone before, 2026-11-01 18:40) gave Hidden / Shown events while the button was off, but did
//   nothing once the button was on.
// - vrcmd: `/opt/steamvr/bin/linuxarm64/vrcmd --compositorcmd <command>`, SteamVR's own tool, which reaches
//   vrcompositor's mailbox "vrcompositor_mailbox" over OpenVR's IPC. That mailbox only takes the debug command
//   camera_room_view_toggle (live, 2026-10-30): the room view flips and SteamVR sends VREvent_RoomViewShown / Hidden.
//   Only a toggle, and live (2026-11-01) one toggle for off got no event and left the view on, so this is no longer
//   the default. set_camera_roomview_enabled and set_camera_roomview_enabled_forsceneapps (with ?enabled=1 / 0) only
//   log "didn't have a handler for" in vrcompositor.txt. FRAMEDOCK_ROOMVIEW_CMD_ON / _OFF override the commands.
// - mailbox: the way SteamVR's own web pages do it (resources/webinterface/dashboard): a WebSocket to vrserver's web
//   server (ws://127.0.0.1:27062/), "mailbox_open <ours>", then
//   "mailbox_send vrcompositor_systemlayer {"type":"set_camera_roomview_enabled","enabled":<bool>}".
//   FRAMEDOCK_ROOMVIEW_MAILBOX picks another mailbox. Live (2026-10-30): vrserver closes the connection, because the
//   dashboard's socket carries ?secret=<…> and vrserver rejects requests without it.
//
// Found by reading SteamVR 2.15 / build 1791331621 (read-only): vrcompositor registers set_camera_roomview_enabled
// and set_camera_roomview_enabled_forsceneapps in the same handler table as update_scene_graph, which the dashboard
// sends to "vrcompositor_systemlayer"; the handlers read the bool "enabled". vrclient's IVRMailbox_001 has four
// methods (vtable order): RegisterMailbox(name, &handle), UndoRegisterMailbox(handle), SendMessage(handle, address,
// message), ReadMessage(handle, buffer, size, &needed).
//
// The openvr route sends at once on the main thread (one quick IPC call). The other two share one worker thread
// (started with the first command) that makes each attempt with a time limit; the main loop never waits for it.
// Only the last wish counts; its outcome comes back through takeResult() and a poke of the main loop's eventfd (a
// superseded wish's outcome is not reported).
#pragma once

#include "device_backend.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace roomview {

/** vrserver's web server (its WebSocket carries the mailboxes) */
constexpr int kWebPort = 27062;
/** The mailbox whose handlers include set_camera_roomview_enabled */
constexpr const char* kTargetMailbox = "vrcompositor_systemlayer";
/** Ours (opened so the server knows who sends) */
constexpr const char* kOwnMailbox = "framedock_roomview";
/** SteamVR's command-line tool (deckard-steamvr-rel) */
constexpr const char* kVrcmd = "/opt/steamvr/bin/linuxarm64/vrcmd";
/** vrcmd --compositorcmd's default commands: the toggle both ways (the only one the mailbox takes, see above) */
constexpr const char* kVrcmdOn = "camera_room_view_toggle";
constexpr const char* kVrcmdOff = "camera_room_view_toggle";
/** Seconds vrcmd may run before it is killed */
constexpr double kVrcmdSec = 5;

/** @return {"type":"set_camera_roomview_enabled","enabled":true|false}: the openvr route's (and the mailbox route's) */
std::string enableMessage(bool on);
/** @return {"type":"set_camera_roomview_enabled_forsceneapps","enabled":true|false} (the openvr route's off sends it too) */
std::string sceneAppsMessage(bool on);
/** @return the WebSocket text frame that sends message to mailbox: "mailbox_send <mailbox> <message>" */
std::string sendCommand(const std::string& mailbox, const std::string& message);
/** @return {"type":"get_debug_commands","returnAddress":"framedock_roomview"}: asks a mailbox what it takes */
std::string probeMessage();

/**
 * One connection to vrserver's web server: open our mailbox, send command (a "mailbox_send ..." line), keep reading
 * for lingerSec, close. Blocks for at most kExchangeSec + lingerSec.
 * @param stopFd readable = give up at once (−1: none)
 * @param replies what the server sent (may be null)
 * @return true if the command went out
 */
bool exchange(int port, const std::string& command, double lingerSec, int stopFd, std::vector<std::string>* replies,
              std::string& error);
/** Seconds one exchange may take before the linger (connect, open, send). */
constexpr double kExchangeSec = 3;

/**
 * Runs `<vrcmd> --compositorcmd <command>` (no shell; stdin /dev/null, stdout and stderr into output, at most 4 KiB)
 * and reaps it. Killed when timeoutSec runs out or stopFd becomes readable (−1: none).
 * @return true if it exited with 0
 */
bool runVrcmd(const std::string& vrcmd, const std::string& command, double timeoutSec, int stopFd,
              std::string& output, std::string& error);

/** Which route the app and `framedock --room-view` use, from the environment. */
struct Route {
    enum class Kind { Openvr, Vrcmd, Mailbox };
    Kind kind = Kind::Openvr;              ///< FRAMEDOCK_ROOMVIEW=openvr|vrcmd|mailbox
    std::string mailbox = kTargetMailbox;  ///< FRAMEDOCK_ROOMVIEW_MAILBOX
    std::string vrcmd = kVrcmd;
    std::string cmdOn = kVrcmdOn;          ///< FRAMEDOCK_ROOMVIEW_CMD_ON
    std::string cmdOff = kVrcmdOff;        ///< FRAMEDOCK_ROOMVIEW_CMD_OFF
};
/** @param warning set when FRAMEDOCK_ROOMVIEW has an unknown value (the default is used then) */
Route routeFromEnv(std::string& warning);
/** "OpenVR's mailbox <name>", "vrcmd ..." or "mailbox <name>", for the log */
std::string describe(const Route& route);

/**
 * Sends message to a SteamVR mailbox over the app's OpenVR connection (VrOverlay::sendMailbox).
 * @return false (and why) when it didn't go out
 */
using MailboxSend = std::function<bool(const std::string& mailbox, const std::string& message, std::string& error)>;

/**
 * The app's control for route.
 * @param wakeFd poked when an attempt's outcome is ready (−1: none)
 * @param send the openvr route's way out
 */
std::unique_ptr<RoomViewControl> makeControl(const Route& route, int wakeFd, MailboxSend send);

}  // namespace roomview

/**
 * The worker thread both routes share: the last wish, one attempt at a time, its outcome reported. A subclass calls
 * shutdown() first thing in its destructor (the thread calls attempt()).
 */
class RoomViewWorker : public RoomViewControl {
public:
    explicit RoomViewWorker(int wakeFd) : wakeFd_(wakeFd) {}
    ~RoomViewWorker() override;
    RoomViewWorker(const RoomViewWorker&) = delete;
    RoomViewWorker& operator=(const RoomViewWorker&) = delete;

    void setEnabled(bool on) override;
    bool takeResult(bool& ok) override;

    /** Tests: attempts finished so far, and how many of them got the wish out. */
    int finished() const { return finished_.load(); }
    int sent() const { return sent_.load(); }

protected:
    /** One attempt (on the worker thread). @param stopFd readable = give up at once */
    virtual bool attempt(bool on, int stopFd, std::string& error) = 0;
    /** Ends an attempt in progress and joins the thread. */
    void shutdown();

private:
    int wakeFd_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<bool> queue_;
    bool stop_ = false;
    unsigned wishes_ = 0;           ///< setEnabled calls so far (guarded by mutex_)
    int stopFd_ = -1;               ///< wakes an attempt's poll() at shutdown
    std::atomic<int> result_ {-1};  ///< the last wish's outcome: −1 none (or taken), 0 failed, 1 went out
    std::atomic<int> finished_ {0};
    std::atomic<int> sent_ {0};
    std::thread thread_;

    void run();
};

/** The mailbox route (see the top of this file). */
class RoomViewMailbox : public RoomViewWorker {
public:
    /**
     * @param port the web server's port (tests: a fake server)
     * @param target the mailbox to send to (FRAMEDOCK_ROOMVIEW_MAILBOX overrides it in the app, for live tests)
     */
    explicit RoomViewMailbox(int port = roomview::kWebPort, std::string target = roomview::kTargetMailbox,
                             int wakeFd = -1);
    ~RoomViewMailbox() override { shutdown(); }

protected:
    bool attempt(bool on, int stopFd, std::string& error) override;

private:
    int port_;
    std::string target_;
};

/** The vrcmd route (see the top of this file). */
class RoomViewVrcmd : public RoomViewWorker {
public:
    RoomViewVrcmd(std::string vrcmd, std::string cmdOn, std::string cmdOff, int wakeFd = -1,
                  double timeoutSec = roomview::kVrcmdSec);
    ~RoomViewVrcmd() override { shutdown(); }

protected:
    bool attempt(bool on, int stopFd, std::string& error) override;

private:
    std::string vrcmd_;
    std::string cmdOn_;
    std::string cmdOff_;
    double timeoutSec_;
};

/** The openvr route (see the top of this file): the message goes out in setEnabled, on the caller's thread. */
class RoomViewDirect : public RoomViewControl {
public:
    RoomViewDirect(roomview::MailboxSend send, std::string target = roomview::kTargetMailbox)
        : send_(std::move(send)), target_(std::move(target)) {}

    void setEnabled(bool on) override;
    bool takeResult(bool& ok) override;
    bool exact() const override { return true; }

private:
    roomview::MailboxSend send_;
    std::string target_;
    int result_ = -1;  ///< the last wish's outcome: −1 none (or taken), 0 failed, 1 went out
};
