// The Frame Sync flow, ported from flow.py. See frame_sync_flow.h.
#include "frame_sync_flow.h"

#include <cctype>

namespace framesync {

namespace {

/** flow.py's _Failed: ends the run with its message. */
struct Failed : std::runtime_error {
    explicit Failed(const std::string& message) : std::runtime_error(message) {}
};

bool contains(const IdSet& ids, const std::string& id) {
    return ids && ids->count(id) > 0;
}

/** flow.py's Sync, one run. */
class Sync {
public:
    Sync(Agent& agent, const std::string& target, const std::string& mac, const std::string& broadcast,
         const SyncHooks& hooks)
        : agent_(agent), target_(target), mac_(mac), broadcast_(broadcast), hooks_(hooks) {}

    SyncOutcome run() {
        try {
            wakeIfNeeded();
            waitLogin();
            switchSteam();
        } catch (const Failed& failure) {
            return fail(failure.what());
        } catch (const AgentError& error) {
            std::string message;
            if (error.kind == AgentError::Kind::Cancelled) message = tr(Tx::SyncCancelled);
            else if (error.kind == AgentError::Kind::Unreachable) message = tr(Tx::SyncUnreachable);
            else if (error.kind == AgentError::Kind::Auth) message = tr(Tx::SyncAuth);
            else if (!error.message.empty()) message = error.message;
            else message = trf(Tx::SyncAgentHttp, {std::to_string(error.status)});
            return fail(message);
        } catch (const std::exception& error) {
            // So the screen never stays at "in progress", whatever went wrong
            return fail(trf(Tx::SyncUnexpected, {error.what()}));
        }
        hooks_.progress(Step::Done, stepText(Step::Done));
        return {true, ""};
    }

private:
    Agent& agent_;
    const std::string& target_;
    const std::string& mac_;
    const std::string& broadcast_;
    const SyncHooks& hooks_;

    SyncOutcome fail(const std::string& message) {
        hooks_.progress(Step::Error, message);
        return {false, message};
    }

    void wakeIfNeeded() {
        try {
            agent_.status();
            return;
        } catch (const AgentError& error) {
            if (error.kind != AgentError::Kind::Unreachable) throw;
        }
        hooks_.progress(Step::Waking, stepText(Step::Waking));
        hooks_.wol(mac_, broadcast_);
        const double until = hooks_.now() + kWakeLimitSec;
        while (hooks_.now() < until) {
            hooks_.sleep(kPollSec);
            try {
                agent_.status();
            } catch (const AgentError& error) {
                if (error.kind != AgentError::Kind::Unreachable) throw;
                continue;
            }
            agent_.claim();
            return;
        }
        throw Failed(tr(Tx::SyncUnreachable));
    }

    /**
     * flow.py's _transient: a dropped connection while waiting (a slow answer just after boot) counts as one empty
     * poll. The caller keeps the limit.
     */
    PcStatus transientStatus() {
        try {
            return agent_.status();
        } catch (const AgentError& error) {
            if (error.kind != AgentError::Kind::Unreachable) throw;
            return {};
        }
    }

    void waitLogin() {
        hooks_.progress(Step::WaitingLogin, stepText(Step::WaitingLogin));
        const double until = hooks_.now() + kLoginLimitSec;
        while (true) {
            if (transientStatus().hasUsers) return;
            if (hooks_.now() >= until) throw Failed(tr(Tx::SyncLogin));
            hooks_.sleep(kPollSec);
        }
    }

    void switchSteam() {
        hooks_.progress(Step::Switching, stepText(Step::Switching));
        bool force = false;
        const double until = hooks_.now() + kLoginLimitSec;
        while (true) {
            SwitchReply reply;
            try {
                reply = agent_.switchTo(target_, force);
            } catch (const AgentError& error) {
                if (error.code == "no-session" && hooks_.now() < until) {
                    hooks_.sleep(kPollSec);
                    continue;
                }
                throw;
            }
            if (reply.status == 200) return;
            if (reply.status == 409 && reply.code == "game") {
                if (!hooks_.confirmGame()) throw Failed(tr(Tx::SyncCancelled));
                force = true;
                continue;
            }
            if (reply.status == 409) throw Failed(reply.message.empty() ? tr(Tx::SyncResult) : reply.message);
            waitResult(reply.requestId);
            return;
        }
    }

    void waitResult(const std::string& requestId) {
        const double until = hooks_.now() + kResultLimitSec;
        while (hooks_.now() < until) {
            SwitchResult result;
            try {
                result = agent_.switchResult(requestId);
            } catch (const AgentError& error) {
                if (error.kind != AgentError::Kind::Unreachable) throw;
            }
            if (result.done) {
                if (result.ok) return;
                throw Failed(result.message.empty() ? tr(Tx::SyncResult) : result.message);
            }
            hooks_.sleep(kPollSec);
        }
        throw Failed(tr(Tx::SyncResult));
    }
};

}  // namespace

ButtonState buttonState(const std::string& frameId, const IdSet& pcIds, const PcStatus* status) {
    if (frameId.empty()) return ButtonState::NoFrameAccount;
    if (pcIds && !contains(pcIds, frameId)) return ButtonState::NotOnPc;
    if (status == nullptr) return ButtonState::Wake;
    return status->steamRunning && status->steamId == frameId ? ButtonState::Ready : ButtonState::Switch;
}

std::string stripText(Reach reach, ErrorKind error) {
    if (error == ErrorKind::Auth) return tr(Tx::SyncStripAuth);
    if (error != ErrorKind::None) return tr(Tx::SyncStripError);
    if (reach == Reach::Unknown) return tr(Tx::SyncStripChecking);
    return tr(reach == Reach::On ? Tx::SyncStripOn : Tx::SyncStripOff);
}

BigButton bigButton(const std::string& frameId, const IdSet& pcIds, const PcStatus* status, Reach reach,
                    ErrorKind error, const std::string& name) {
    if (error != ErrorKind::None) return {tr(Tx::SyncBigError), false};
    if (reach == Reach::Unknown) return {tr(Tx::SyncStripChecking), false};
    switch (buttonState(frameId, pcIds, reach == Reach::On ? status : nullptr)) {
        case ButtonState::NoFrameAccount: return {tr(Tx::SyncBigNoFrameAccount), false};
        case ButtonState::NotOnPc: return {trf(Tx::SyncBigNotOnPc, {name}), false};
        case ButtonState::Wake: return {trf(Tx::SyncBigWake, {name}), true};
        case ButtonState::Switch: return {trf(Tx::SyncBigSwitch, {name}), true};
        case ButtonState::Ready: return {trf(Tx::SyncBigReady, {name}), false};
    }
    return {};
}

std::vector<Row> listRows(const std::vector<FrameAccount>& frameAccounts, const std::string& frameId,
                          const IdSet& pcIds, const std::map<std::string, std::string>& names) {
    std::vector<Row> rows;
    for (const FrameAccount& a : frameAccounts) {
        if (a.steamId == frameId || (pcIds && !contains(pcIds, a.steamId))) continue;
        const auto it = names.find(a.steamId);
        rows.push_back({a.steamId, it != names.end() ? it->second : a.steamId});
    }
    return rows;
}

SyncOutcome runSync(Agent& agent, const std::string& targetId, const std::string& mac, const std::string& broadcast,
                    const SyncHooks& hooks) {
    return Sync(agent, targetId, mac, broadcast, hooks).run();
}

const char* stepText(Step step) {
    switch (step) {
        case Step::Waking: return tr(Tx::SyncStepWaking);
        case Step::WaitingLogin: return tr(Tx::SyncStepLogin);
        case Step::Switching: return tr(Tx::SyncStepSwitching);
        case Step::Done: return tr(Tx::SyncStepDone);
        case Step::Error: break;
    }
    return "";
}

std::vector<uint8_t> magicPacket(const std::string& mac) {
    std::string digits;
    for (const char c : mac) {
        if (c != ':' && c != '-') digits += c;
    }
    if (digits.size() != 12) return {};
    uint8_t address[6];
    for (int i = 0; i < 6; ++i) {
        int value = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(digits[i * 2 + k])));
            if (c >= '0' && c <= '9') value = value * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') value = value * 16 + (c - 'a' + 10);
            else return {};
        }
        address[i] = static_cast<uint8_t>(value);
    }
    std::vector<uint8_t> packet(6, 0xff);
    for (int n = 0; n < 16; ++n) packet.insert(packet.end(), address, address + 6);
    return packet;
}

}  // namespace framesync
