// The 기기 page's backends wired to its model: sound (Pulse on PipeWire), passthrough (SteamVR's room view and
// Camera Switch's colour) and matching the home PC's Steam account (Frame Sync's agent). The main loop owns one
// DeviceBackend; each wake it says what is visible, hands it SteamVR's room-view events, calls tick() and sleeps no
// longer than nextWake().
//
// The services below run their work on their own threads (or async APIs) and poke the main loop's eventfd only when
// they have something for a visible page. They are interfaces so the tests drive DeviceBackend with fakes.
//
// Demand: sound and Camera Switch are watched only while the 기기 page (the tab or the Dock's full view) or the
// Dock's quick view is shown; the PC's status is asked every kRefreshSec only while the 기기 page is shown (as
// app.py does while its window is open). A sync that runs goes on when the page is hidden.
#pragma once

#include "audio_sinks.h"
#include "device_model.h"
#include "frame_sync_flow.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/** The default output, its volume and mute (pulse_audio.h). */
class AudioService {
public:
    virtual ~AudioService() = default;
    /** Watch the server (true) or stop (false: no event wakes anything; it lets go of the server a while later). */
    virtual void setWanted(bool wanted) = 0;
    /** @return true (and the reading) if a new one came since the last call */
    virtual bool take(AudioState& out) = 0;
    virtual void selectOutput(const std::string& id) = 0;
    virtual void setVolume(int percent) = 0;  ///< the default output's
    virtual void setMuted(bool muted) = 0;
};

/** Camera Switch's D-Bus service (camera_switch.h). */
class CameraSwitchService {
public:
    virtual ~CameraSwitchService() = default;
    virtual void setWanted(bool wanted) = 0;
    /** @return true (and whether it runs) if that changed since the last call (the first answer counts as a change) */
    virtual bool takeAvailable(bool& available) = 0;
    /** Activate("set-color-passthrough", [<color>]). The action has no state: nothing can read it back. */
    virtual void setColor(bool color) = 0;
};

/** Turns SteamVR's room view (the passthrough) on or off (room_view.h). No public OpenVR call does this. */
class RoomViewControl {
public:
    virtual ~RoomViewControl() = default;
    /** Asks for on or off (only the last wish counts). Returns at once. */
    virtual void setEnabled(bool on) = 0;
    /**
     * @return true (and whether the last wish went out) once its attempt has ended; reported once. "Went out" is not
     * "done": only SteamVR's events say what the room view is.
     */
    virtual bool takeResult(bool& ok) = 0;
    /**
     * true: on and off are set, not toggled, so a command that went out is taken as done when no event follows
     * (SteamVR sends none for SteamVR's own 룸 뷰 button), and each press sends it again (nothing tells the app when
     * that button changes the view)
     */
    virtual bool exact() const { return false; }
};

/** What the Frame Sync worker tells the main loop (frame_sync_worker.h). */
struct SyncEvent {
    enum class Kind {
        Setup,     ///< config and the Frame's accounts read (configured, frame*, cached pcAccounts, avatars)
        Refresh,   ///< a status poll ended (status, reach, error, errorText; pcAccounts and avatars when fetched)
        Progress,  ///< step
        AskGame,   ///< a game runs on the PC: ask (answerGame)
        Finished,  ///< ok, message
        Message,   ///< a line for the page (the PC's 끄기): message, error
    };
    Kind kind = Kind::Refresh;
    bool configured = false;
    std::string frameId;
    std::vector<framesync::FrameAccount> frameAccounts;
    std::optional<std::vector<framesync::PcAccount>> pcAccounts;
    std::map<std::string, std::string> avatars;  ///< steam id -> PNG path (added to what the page has)
    std::optional<framesync::PcStatus> status;
    framesync::Reach reach = framesync::Reach::Unknown;
    framesync::ErrorKind error = framesync::ErrorKind::None;
    std::string errorText;
    framesync::Step step = framesync::Step::Done;
    bool ok = false;
    std::string message;
    bool messageError = false;
};

/** Frame Sync's work (frame_sync_worker.h). Every call returns at once; results come back as SyncEvents. */
class SyncService {
public:
    virtual ~SyncService() = default;
    /** Reads the config, the key's presence and the Frame's Steam accounts (again: the page was shown). */
    virtual void setup() = 0;
    /** One status poll (app.py's refresh). */
    virtual void refresh() = 0;
    virtual void startSync(const std::string& steamId) = 0;
    virtual void answerGame(bool yes) = 0;
    virtual void shutdownPc() = 0;
    /** @return the events since the last call, oldest first */
    virtual std::vector<SyncEvent> take() = 0;
};

/** ~/.local/state/framedock/passthrough-color (XDG_STATE_HOME when absolute): "color" or "mono" */
std::string rememberedColorFile();
/** @return the colour written there (nullopt: none, or unreadable) */
std::optional<bool> readRememberedColor(const std::string& file);
/** Writes it (atomically). Off the main loop: Camera Switch's thread calls it. */
bool writeRememberedColor(const std::string& file, bool color, std::string& error);

class DeviceBackend {
public:
    /**
     * Seconds the released volume stays on the page while readings disagree: a reading of a value sent during the
     * drag can arrive after the release and would make the knob jump back for a moment. A reading of the released
     * value ends it sooner; when it runs out the last reading shows.
     */
    static constexpr double kVolumeHoldSec = 0.5;

    /**
     * Every service may be null (that part then stays "확인 중" and its controls do nothing).
     */
    DeviceBackend(DeviceModel& model, std::unique_ptr<AudioService> audio, std::unique_ptr<CameraSwitchService> camera,
                  std::unique_ptr<RoomViewControl> roomView, std::unique_ptr<SyncService> sync);
    ~DeviceBackend();
    DeviceBackend(const DeviceBackend&) = delete;
    DeviceBackend& operator=(const DeviceBackend&) = delete;

    /**
     * The colour last picked (Camera Switch can't be asked), from an earlier run (readRememberedColor); nullopt = never
     * picked. Camera Switch's service writes it down when a change goes through.
     */
    void setRememberedColor(std::optional<bool> color);
    std::optional<bool> rememberedColor() const { return color_; }

    /**
     * What is shown now.
     * @param soundShown the 기기 page (tab or Dock full view) or the Dock's quick view: sound and passthrough
     * @param syncShown the 기기 page: the PC's account
     */
    void setVisible(bool soundShown, bool syncShown, double now);
    /** SteamVR's VREvent_RoomViewShown / Hidden (an overlay app gets them too: checked live 2026-10-30). */
    void roomViewEvent(bool shown);

    /** Seconds the command itself may take (vrcmd is killed after roomview::kVrcmdSec = 5): no second one meanwhile. */
    static constexpr double kRoomViewSendSec = 6;
    /** Seconds after the command went out until SteamVR's event must have come, or the view counts as unknown. */
    static constexpr double kRoomViewEventSec = 3;
    /** Commands in a row for one wish before giving up (a toggle that lands on the wrong side is sent again). */
    static constexpr int kRoomViewTries = 3;

    /**
     * Takes the page's actions, the services' news and what is due (the volume's throttle, the status poll, the
     * question's timeout). @return true if the model changed (redraw)
     */
    bool tick(double now);
    /** @return when tick() has something to do (+infinity: nothing until a service or an action wakes the loop) */
    double nextWake() const;

private:
    DeviceModel& model_;
    std::unique_ptr<AudioService> audio_;
    std::unique_ptr<CameraSwitchService> camera_;
    std::unique_ptr<RoomViewControl> roomView_;
    std::unique_ptr<SyncService> sync_;
    audio::VolumeThrottle throttle_;
    int heldVolume_ = -1;              ///< the released volume while it is held (−1: none)
    std::string heldSink_;             ///< the output it was released on
    double holdUntil_ = 0;
    std::optional<AudioState> lastReading_;
    bool soundShown_ = false;
    bool syncShown_ = false;
    std::optional<bool> roomViewOn_;   ///< what SteamVR's events said last (nullopt: unknown)
    std::optional<bool> roomViewWish_; ///< the page's choice until SteamVR shows it (the page shows the wish)
    bool roomViewSent_ = false;        ///< a command is out, its event not yet back
    bool roomViewSentOn_ = false;      ///< what that command asked for (the wish may have changed since)
    bool roomViewAssumed_ = false;     ///< roomViewOn_ is what an exact() command set, not SteamVR's word
    double roomViewDeadline_ = 0;      ///< while roomViewSent_: no event by then = unknown (from the send until
                                       ///< the command's outcome, then kRoomViewEventSec from that)
    int roomViewTries_ = 0;            ///< commands sent for this wish
    std::optional<bool> color_;        ///< the colour last picked (Camera Switch has no state to read)
    bool cameraAvailable_ = false;
    bool setupPending_ = false;        ///< the settings are being read: no poll until they are
    bool refreshing_ = false;          ///< a status poll is out
    double nextRefresh_ = 0;           ///< while syncShown_
    unsigned seenRevision_ = 0;

    void handle(const DeviceAction& action, double now);
    /** A reading for the page, the released volume held (kVolumeHoldSec). */
    void applyReading(AudioState state, double now);
    void applySync(const SyncEvent& event, double now);
    /** The page's passthrough from roomViewOn_ (or the wish), color_ and Camera Switch. */
    void composePassthrough();
    /**
     * Brings the room view to the wish, one command at a time: SteamVR only takes a toggle (vrcmd, 2026-10-30), so a
     * command goes out only when the view is known to differ or isn't known, and its event decides what follows.
     */
    void driveRoomView(double now);
};
