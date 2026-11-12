// Matching the home PC's Steam account to the Frame's: a port of the Frame Sync app's logic
// (~/.local/share/frame-sync/frame_sync/flow.py, agent_client.py's AgentError and magic packet). No network here: the
// agent on the home PC is behind framesync::Agent, Wake-on-LAN, sleeping and the clock behind SyncHooks, so a
// backend can run runSync() on a worker thread and the tests on a fake clock.
//
// The flow, as in flow.py: wake the PC (WOL) -> the agent answers -> claim("frame") -> a logged-in desktop session ->
// switch Steam -> the result. The limits are flow.py's (its design part 2): the agent 240 s, the login 120 s, the
// switch result 180 s. The result is waited for longer than the agent's own limit (150 s from the request) so that
// when time runs out the agent's reason comes first.
#pragma once

#include "app_text.h"  // the messages

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace framesync {

// ---- flow.py's limits (seconds) ----
constexpr double kWakeLimitSec = 240;    ///< WAKE_LIMIT: the agent answering after WOL
constexpr double kLoginLimitSec = 120;   ///< LOGIN_LIMIT: a logged-in session (and no-session retries of the switch)
constexpr double kResultLimitSec = 180;  ///< RESULT_LIMIT: the switch's result
constexpr double kPollSec = 2;           ///< POLL
// ---- app.py's / agent_client.py's (the backend uses them) ----
constexpr double kRefreshSec = 5;         ///< REFRESH_MS: the status strip's refresh while the page is shown
constexpr double kStatusTimeoutSec = 4;   ///< AgentClient's default: one request of the refresh
constexpr double kSyncTimeoutSec = 12;    ///< SYNC_TIMEOUT: one request during a sync (slow answers just after boot)
constexpr double kAskTimeoutSec = 120;    ///< ASK_TIMEOUT: an unanswered question counts as "취소" (cancel)

// flow.py's MESSAGES are app_text's Tx::SyncUnreachable, SyncAuth, SyncLogin, SyncResult and SyncCancelled

/** The agent's /status: what the strip and the big button need. */
struct PcStatus {
    bool steamRunning = false;  ///< steam.running
    std::string steamId;        ///< steam.steamId (the account Steam runs as)
    std::string gameAppId;      ///< steam.gameAppId ("" = no game)
    bool streaming = false;     ///< streaming
    bool hasUsers = false;      ///< users is non-empty: someone is logged in to a desktop session
    bool operator==(const PcStatus& o) const {
        return steamRunning == o.steamRunning && steamId == o.steamId && gameAppId == o.gameAppId &&
               streaming == o.streaming && hasUsers == o.hasUsers;
    }
    bool operator!=(const PcStatus& o) const { return !(*this == o); }
};

/** One account of the PC's Steam (/steam/accounts). */
struct PcAccount {
    std::string steamId;
    std::string personaName;
    std::string accountName;
    bool hasAvatar = false;
    bool operator==(const PcAccount& o) const {
        return steamId == o.steamId && personaName == o.personaName && accountName == o.accountName &&
               hasAvatar == o.hasAvatar;
    }
    bool operator!=(const PcAccount& o) const { return !(*this == o); }
};

/** One account of the Frame's Steam (loginusers.vdf; frame_steam.accounts()). */
struct FrameAccount {
    std::string steamId;
    std::string accountName;
    std::string personaName;  ///< PersonaName, else AccountName
};

/** Whether the PC answers (app.py's reachable: None / True / False). */
enum class Reach { Unknown, On, Off };
/** A refused / failed request although the PC answered (app.py's error_kind). */
enum class ErrorKind { None, Auth, Http };

/** flow.py's button_state. */
enum class ButtonState { NoFrameAccount, NotOnPc, Wake, Switch, Ready };

/** The PC's account ids, std::nullopt when unknown (the PC was never reached and nothing is cached). */
using IdSet = std::optional<std::set<std::string>>;

/**
 * flow.py's button_state.
 * @param status the agent's /status, null when the PC is off
 */
ButtonState buttonState(const std::string& frameId, const IdSet& pcIds, const PcStatus* status);

/** flow.py's strip_text: the PC's line ("● 집 PC 켜짐", "집 PC 확인 중…", ...). */
std::string stripText(Reach reach, ErrorKind error);

/** flow.py's big_button: (its label, whether it can be pressed). */
struct BigButton {
    std::string text;
    bool enabled = false;
};
BigButton bigButton(const std::string& frameId, const IdSet& pcIds, const PcStatus* status, Reach reach,
                    ErrorKind error, const std::string& name);

/** One row of the list of other accounts. */
struct Row {
    std::string steamId;
    std::string name;
    bool operator==(const Row& o) const { return steamId == o.steamId && name == o.name; }
};

/**
 * flow.py's list_rows: the Frame's accounts but the current one that the PC has too (all of them when the PC's list
 * is unknown), named from names (else the id).
 */
std::vector<Row> listRows(const std::vector<FrameAccount>& frameAccounts, const std::string& frameId,
                          const IdSet& pcIds, const std::map<std::string, std::string>& names);

/**
 * agent_client.py's AgentError. kind: unreachable (seen as "off"), auth (401), http (any other refusal), and ours:
 * cancelled (Frame Dock is quitting and ended the call; never taken for "off", so it never leads to a WOL).
 */
struct AgentError : std::runtime_error {
    enum class Kind { Unreachable, Auth, Http, Cancelled };
    Kind kind;
    int status = 0;       ///< the HTTP status (0 = none)
    std::string code;     ///< the body's "code" (e.g. "no-session")
    std::string message;  ///< the body's "message" (may be empty)
    AgentError(Kind k, int s = 0, std::string c = {}, std::string m = {})
        : std::runtime_error(m.empty() ? "agent error" : m), kind(k), status(s), code(std::move(c)),
          message(std::move(m)) {}
};

/** POST /steam/switch's answer: 200 done, 202 accepted (requestId), 409 refused (code "game" = a game runs). */
struct SwitchReply {
    int status = 0;
    std::string code;
    std::string message;
    std::string requestId;
};

/** GET /steam/switch/<requestId>. */
struct SwitchResult {
    bool done = false;
    bool ok = false;
    std::string message;
};

/** The home PC's agent (agent_client.py's AgentClient). Every call may throw AgentError. */
class Agent {
public:
    virtual ~Agent() = default;
    virtual PcStatus status() = 0;
    virtual void claim() = 0;  ///< POST /claim {"source": "frame"}
    /** POST /steam/switch. 200, 202 and 409 come back; anything else throws (401 as Auth). */
    virtual SwitchReply switchTo(const std::string& steamId, bool force) = 0;
    virtual SwitchResult switchResult(const std::string& requestId) = 0;
};

/** Where a sync is (flow.py's on_progress keys). */
enum class Step { Waking, WaitingLogin, Switching, Done, Error };

/** What runSync needs from outside: time, Wake-on-LAN, progress, and the question when a game runs. */
struct SyncHooks {
    std::function<void(double)> sleep;                                ///< seconds
    std::function<double()> now;                                      ///< monotonic seconds
    std::function<void(const std::string&, const std::string&)> wol;  ///< (mac, broadcast)
    std::function<void(Step, const std::string&)> progress;           ///< (step, its text)
    /** "집 PC에서 게임이 실행 중이에요..." answered (true = 계속). No answer in kAskTimeoutSec = false */
    std::function<bool()> confirmGame;
};

struct SyncOutcome {
    bool ok = false;
    std::string message;  ///< "" when ok
};

/**
 * flow.py's Sync.run: wake if needed, wait for a login, switch, wait for the result. Never throws: every failure ends
 * in progress(Error, message) and {false, message}.
 */
SyncOutcome runSync(Agent& agent, const std::string& targetId, const std::string& mac, const std::string& broadcast,
                    const SyncHooks& hooks);

/** The step's words on the big button while a sync runs (app.py's _progress): "켜는 중", "로그인 대기", ... */
const char* stepText(Step step);

/** agent_client.py's magic_packet: 6 × 0xff, then the MAC 16 times. Empty if mac isn't 12 hex digits. */
std::vector<uint8_t> magicPacket(const std::string& mac);

}  // namespace framesync
