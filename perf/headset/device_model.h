// The 기기 page's state (sound, passthrough, matching the home PC's Steam account) and what its controls ask for.
// Shared by the dashboard tab's page, the Dock's full view and the Dock's quick view, like ColorModel is.
//
// No backend here. A backend fills the state in (setAudio, passthrough, sync, syncProgress / syncFinished / askGame)
// and drains takeActions(); the controls change the state at once (a dragged volume, a picked output) so the page
// answers the laser without waiting for it. What the PNG dumps show is made up (dashboard_preview).
#pragma once

#include "frame_sync_flow.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

// ---- Sound ----

/** One audio output (a PipeWire / Pulse sink). */
struct AudioOutput {
    std::string id;        ///< what the backend selects it by (the sink's name)
    std::string name;      ///< its description ("Wireless Headset")
    bool builtin = false;  ///< the headset's own speakers: shown as "헤드셋 스피커" (Tx::DeviceSpeakers)
};

struct AudioState {
    bool known = false;               ///< read at least once (else the page says 확인 중…)
    std::vector<AudioOutput> outputs;
    std::string defaultId;            ///< the output in use
    int volume = 0;                   ///< the default output's volume, 0-100 %
    bool muted = false;
};

/** What an output is called on the page: "헤드셋 스피커" for the built-in one, else its description (else its id). */
std::string outputLabel(const AudioOutput& output);

// ---- Passthrough ----

enum class PassthroughMode { Off, Mono, Color };  ///< 끔 | 흑백 | 컬러

/** Where 흑백 / 컬러 comes from: Camera Switch's action has no state, so the backend can only remember its choice. */
enum class ColorSource {
    Read,        ///< known (the made-up previews)
    Remembered,  ///< the colour Frame Dock picked last (Camera Switch's automatic switch may have changed it since)
    Unknown,     ///< never picked: shown as 흑백
};

struct PassthroughState {
    PassthroughMode mode = PassthroughMode::Off;
    bool roomViewKnown = false;          ///< whether passthrough is on could be read (else nothing is selected)
    bool cameraSwitchAvailable = false;  ///< Camera Switch runs: 흑백 / 컬러 can be picked
    ColorSource color = ColorSource::Read;
    /** roomViewKnown came from SteamVR's event (else it is what Frame Dock asked for, which may not have happened) */
    bool roomViewConfirmed = false;
};

// ---- Matching the PC's account (Frame Sync) ----

/** Frame Sync's screen state (app.py's App fields). The backend fills in what it learns. */
struct SyncState {
    /// Frame Sync's files are there: ~/.config/frame-sync/config.json and its key (main checks at start,
    /// framesync::settingsPresent). False: the 기기 page leaves PC 계정 맞추기 out
    bool present = true;
    /// ... and they read fine (the Setup event). False with present: the card says what's wrong
    bool configured = true;
    std::string frameId;     ///< the Frame's account (loginusers.vdf, AutoLogin "1"); "" = none
    std::vector<framesync::FrameAccount> frameAccounts;
    std::optional<std::vector<framesync::PcAccount>> pcAccounts;  ///< the PC's (cached while it's off); none = unknown
    std::optional<framesync::PcStatus> status;  ///< the last /status (only meaningful while reach == On)
    framesync::Reach reach = framesync::Reach::Unknown;
    framesync::ErrorKind errorKind = framesync::ErrorKind::None;
    std::map<std::string, std::string> avatars;  ///< steam id -> a PNG (the Frame's avatarcache, or one the PC sent)

    // A run (app.py's busy and _progress)
    bool busy = false;
    std::string busyName;                     ///< whose account it switches to
    std::optional<framesync::Step> step;      ///< the last progress ("" until the first)
    bool fromList = false;                    ///< started from the list of other accounts
    bool waking = false;                      ///< this run woke the PC: the strip says 켜는 중 until it ends

    /** The question line inside the page (VR may not show another window). */
    enum class Question { None, Game, Shutdown };
    Question question = Question::None;
    double questionUntil = 0;  ///< Game: unanswered by then counts as 취소 (kAskTimeoutSec)

    std::string message;  ///< the line under the big button ("" = none)
    bool messageError = false;

    /** SteamID -> display name: the Frame's persona names, then the PC's for the rest (app.py's names()). */
    std::map<std::string, std::string> names() const;
    /** The PC's account ids (std::nullopt = unknown). */
    framesync::IdSet pcIds() const;
};

/** What the 기기 page shows for Frame Sync, worked out from SyncState with flow.py's functions (app.py's render). */
struct SyncScreen {
    std::string strip;          ///< "● 집 PC 켜짐", "● 집 PC 켜는 중", ...
    enum class Tone { Muted, On, Waking, Error } tone = Tone::Muted;  ///< the dot's colour
    std::string pcAccountId;    ///< the account the PC's Steam runs as ("" = unknown / off)
    std::string pcAccountName;
    std::string extra;          ///< "스트리밍 중 · 게임 중"
    bool shutdownEnabled = false;  ///< 끄기
    std::string bigText;
    bool bigEnabled = false;
    std::string bigAccountId;   ///< whose avatar the big button shows (the Frame's account)
    std::vector<framesync::Row> rows;
    bool rowsEnabled = false;   ///< not during a run
    std::string question;       ///< "" = no question line
    std::string yesText;        ///< the question's yes button: 계속 (a game runs), 끄기 (the PC's 끄기)
    std::string message;
    bool messageError = false;
};
SyncScreen syncScreen(const SyncState& state);

// ---- Actions ----

/** What a control asks the backend to do. */
struct DeviceAction {
    enum class Kind {
        SelectOutput,    ///< id: make it the default output
        SetVolume,       ///< value 0-100; final = false while dragging, true on release
        SetMuted,        ///< flag
        SetPassthrough,  ///< mode
        StartSync,       ///< id: the account to switch the PC to; flag = picked from the list
        AnswerGame,      ///< flag = 계속 (false = 취소, or no answer in kAskTimeoutSec)
        ShutdownPc,      ///< the PC's 끄기, confirmed
    };
    explicit DeviceAction(Kind k, std::string actionId = {}) : kind(k), id(std::move(actionId)) {}
    Kind kind;
    std::string id;
    int value = 0;
    bool flag = false;
    bool final = false;
    PassthroughMode mode = PassthroughMode::Off;
};

/** The state shared by the pages, and the queue of what they asked for. */
class DeviceModel {
public:
    AudioState audio;
    PassthroughState passthrough;
    SyncState sync;

    // ---- From the backend ----
    /**
     * app.py's _apply_refresh: a status poll's result. An error goes on the message line (red); the line clears when
     * an earlier error is gone.
     * @param status /status when the PC answered
     * @param errorText the error's words ("" = none)
     * @return true if anything changed (only then the views redraw)
     */
    bool applyRefresh(const std::optional<framesync::PcStatus>& status, framesync::Reach reach,
                      framesync::ErrorKind error, const std::string& errorText);
    /** A fresh reading. While the volume is dragged the dragged value stays (the reading would make it jump). */
    void setAudio(const AudioState& state);
    /** app.py's _progress: the big button says "<name> · <step>"; waking also changes the strip. */
    void syncProgress(framesync::Step step);
    /** app.py's _finish. */
    void syncFinished(bool ok, const std::string& message);
    /** The agent says a game runs: the question line asks (no answer in kAskTimeoutSec = 취소). */
    void askGame(double now);
    /** A line under the big button (the PC's 끄기 answered). */
    void showMessage(const std::string& text, bool error);
    /** The backend changed sync / passthrough fields directly: the views redraw. */
    void touch() { ++revision_; }

    // ---- From the controls (change the state at once, queue the action) ----
    void selectOutput(const std::string& id);
    /** The volume slider moved to percent (pressed or dragged). Queues only a change. */
    void dragVolume(int percent);
    /** The slider was let go (or the laser left): queues the final value. Nothing if no drag runs. */
    void releaseVolume();
    bool volumeDragging() const { return dragging_; }
    void setMuted(bool muted);
    /**
     * 흑백 / 컬러 only while Camera Switch runs. Pressing what is selected does nothing only when that is known for
     * sure (SteamVR said whether the view is on, and the colour was read); otherwise the press goes out again, so it
     * can put right a wrong guess (Camera Switch's automatic switch, an attempt that failed).
     */
    void setPassthrough(PassthroughMode mode);
    /**
     * The big button (the Frame's account) or a row's 선택. Not during a run. An open "끌까요?" question is answered
     * 취소 first (app.py: so a 계속 pressed during the switch can't turn the PC off).
     */
    void startSync(const std::string& steamId, bool fromList);
    /** 끄기: asks "집 PC를 끌까요?" (only while the PC answers and no run is going). */
    void askShutdown();
    /** 계속 / 취소 on the question line. */
    void answer(bool yes);

    /** The question's timeout. @return true if something changed (redraw) */
    bool tick(double now);
    /** @return when tick() has something to do (+infinity: nothing) */
    double nextWake() const;

    /** The queued actions, oldest first; the queue is emptied. */
    std::vector<DeviceAction> takeActions();
    /** Bumped by every change: the views compare it to redraw each other's changes. */
    unsigned revision() const { return revision_; }

private:
    std::vector<DeviceAction> actions_;
    bool dragging_ = false;
    unsigned revision_ = 0;

    void queue(DeviceAction action) {
        actions_.push_back(std::move(action));
        ++revision_;
    }
};
