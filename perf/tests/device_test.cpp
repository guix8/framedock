// The 기기 page: Frame Sync's flow (ported from flow.py, on a fake agent and a fake clock), the page's model (what the
// controls change and queue, the question line, the screen texts), and the page's hit areas: segments, the volume
// slider's drag (a press alone changes nothing), − / ＋ held, disabled parts. Fake state only: nothing reads ~/.config/frame-sync or talks to a PC.
#include "config.h"
#include "dashboard_preview.h"
#include "device_model.h"
#include "device_page.h"
#include "draw.h"
#include "frame_sync_flow.h"

#include <cairo.h>

#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                             \
    do {                                                        \
        if (!(expr)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
            ++failures;                                         \
        }                                                       \
    } while (false)

using framesync::AgentError;
using framesync::ButtonState;
using framesync::ErrorKind;
using framesync::PcStatus;
using framesync::Reach;
using framesync::Step;
using Kind = DeviceAction::Kind;

// ---- flow.py's screen functions ----

PcStatus running(const std::string& id, bool users = true) {
    PcStatus s;
    s.steamRunning = true;
    s.steamId = id;
    s.hasUsers = users;
    return s;
}

void testScreenFunctions() {
    const std::set<std::string> pc = {"A", "B"};
    const PcStatus onA = running("A");
    const PcStatus onB = running("B");
    PcStatus stopped;
    // button_state
    CHECK(framesync::buttonState("", pc, &onA) == ButtonState::NoFrameAccount);
    CHECK(framesync::buttonState("C", pc, &onA) == ButtonState::NotOnPc);
    CHECK(framesync::buttonState("C", std::nullopt, nullptr) == ButtonState::Wake);  // the PC's list unknown
    CHECK(framesync::buttonState("A", pc, nullptr) == ButtonState::Wake);
    CHECK(framesync::buttonState("A", pc, &onA) == ButtonState::Ready);
    CHECK(framesync::buttonState("A", pc, &onB) == ButtonState::Switch);
    CHECK(framesync::buttonState("A", pc, &stopped) == ButtonState::Switch);  // Steam not running
    // strip_text
    CHECK(framesync::stripText(Reach::On, ErrorKind::Auth) == "● 집 PC 연결 오류(앱 열쇠)");
    CHECK(framesync::stripText(Reach::On, ErrorKind::Http) == "● 집 PC 연결 오류");
    CHECK(framesync::stripText(Reach::Unknown, ErrorKind::None) == "집 PC 확인 중…");
    CHECK(framesync::stripText(Reach::On, ErrorKind::None) == "● 집 PC 켜짐");
    CHECK(framesync::stripText(Reach::Off, ErrorKind::None) == "● 집 PC 꺼짐");
    // big_button
    auto big = framesync::bigButton("A", pc, &onB, Reach::On, ErrorKind::Http, "Alex");
    CHECK(big.text == "연결 오류 — 아래 안내를 확인해 주세요" && !big.enabled);
    big = framesync::bigButton("A", pc, &onB, Reach::Unknown, ErrorKind::None, "Alex");
    CHECK(big.text == "집 PC 확인 중…" && !big.enabled);
    big = framesync::bigButton("A", pc, &onB, Reach::Off, ErrorKind::None, "Alex");  // off: its status is ignored
    CHECK(big.text == "집 PC 켜고 Alex로 스팀 켜기" && big.enabled);
    big = framesync::bigButton("A", pc, &onB, Reach::On, ErrorKind::None, "Alex");
    CHECK(big.text == "집 PC를 Alex로 바꾸기" && big.enabled);
    big = framesync::bigButton("A", pc, &onA, Reach::On, ErrorKind::None, "Alex");
    CHECK(big.text == "Alex · 스트리밍 준비됨" && !big.enabled);
    big = framesync::bigButton("C", pc, &onA, Reach::On, ErrorKind::None, "Player 1");
    CHECK(big.text == "Player 1 계정이 집 PC 스팀에 없어요" && !big.enabled);
    big = framesync::bigButton("", pc, &onA, Reach::On, ErrorKind::None, "");
    CHECK(big.text == "프레임에 로그인한 계정을 찾지 못했어요" && !big.enabled);
    // list_rows: not the current one; only the PC's when its list is known; the id when there's no name
    const std::vector<framesync::FrameAccount> frame = {{"A", "a", "에이"}, {"B", "b", "비"}, {"C", "c", "씨"}};
    const std::map<std::string, std::string> names = {{"A", "에이"}, {"B", "비"}};
    CHECK((framesync::listRows(frame, "A", pc, names) == std::vector<framesync::Row> {{"B", "비"}}));
    CHECK((framesync::listRows(frame, "A", std::nullopt, names) ==
           std::vector<framesync::Row> {{"B", "비"}, {"C", "C"}}));
    CHECK(framesync::listRows(frame, "A", std::set<std::string> {}, names).empty());
    // The magic packet
    const std::vector<uint8_t> packet = framesync::magicPacket("01:23:45:ab:CD:ef");
    CHECK(packet.size() == 102 && packet[0] == 0xff && packet[5] == 0xff && packet[6] == 0x01 && packet[11] == 0xef);
    CHECK(packet[96] == 0x01 && packet[101] == 0xef);
    CHECK(framesync::magicPacket("01-23-45-AB-CD-EF") == packet);
    CHECK(framesync::magicPacket("0123456789").empty() && framesync::magicPacket("01:23:45:ab:cd:eg").empty());
}

// ---- flow.py's Sync on a fake agent and a fake clock ----

/** A scripted agent: each call takes the next answer of its queue (the last one repeats). */
struct FakeAgent : framesync::Agent {
    struct StatusAnswer {
        bool unreachable = false;
        bool auth = false;
        bool cancelled = false;
        PcStatus status;
    };
    std::deque<StatusAnswer> statuses;
    std::deque<framesync::SwitchReply> switches;
    std::deque<AgentError> switchErrors;  ///< thrown by switchTo first, one per call
    std::deque<framesync::SwitchResult> results;
    bool resultsUnreachable = false;
    int statusCalls = 0, claims = 0, switchCalls = 0, resultCalls = 0;
    std::vector<bool> forces;
    std::string target;
    bool throwStd = false;

    template <typename T>
    static T next(std::deque<T>& q) {
        T value = q.front();
        if (q.size() > 1) q.pop_front();
        return value;
    }
    PcStatus status() override {
        ++statusCalls;
        if (throwStd) throw std::runtime_error("boom");
        const StatusAnswer a = next(statuses);
        if (a.unreachable) throw AgentError(AgentError::Kind::Unreachable, 0, "", "timed out");
        if (a.auth) throw AgentError(AgentError::Kind::Auth, 401);
        if (a.cancelled) throw AgentError(AgentError::Kind::Cancelled);
        return a.status;
    }
    void claim() override { ++claims; }
    framesync::SwitchReply switchTo(const std::string& steamId, bool force) override {
        ++switchCalls;
        target = steamId;
        forces.push_back(force);
        if (!switchErrors.empty()) {
            const AgentError e = switchErrors.front();
            switchErrors.pop_front();
            throw e;
        }
        return next(switches);
    }
    framesync::SwitchResult switchResult(const std::string&) override {
        ++resultCalls;
        if (resultsUnreachable) throw AgentError(AgentError::Kind::Unreachable);
        return next(results);
    }
};

struct Clock {
    double t = 100;
    int sleeps = 0;
};

struct Run {
    FakeAgent agent;
    Clock clock;
    std::vector<Step> steps;
    std::vector<std::string> wol;
    std::deque<bool> answers;
    int asked = 0;

    framesync::SyncOutcome go(const std::string& target = "B") {
        framesync::SyncHooks hooks;
        hooks.sleep = [this](double s) {
            clock.t += s;
            ++clock.sleeps;
        };
        hooks.now = [this] { return clock.t; };
        hooks.wol = [this](const std::string& mac, const std::string& broadcast) { wol.push_back(mac + "@" + broadcast); };
        hooks.progress = [this](Step step, const std::string&) { steps.push_back(step); };
        hooks.confirmGame = [this] {
            ++asked;
            const bool yes = answers.empty() ? false : answers.front();
            if (!answers.empty()) answers.pop_front();
            return yes;
        };
        return framesync::runSync(agent, target, "01:23:45:67:89:ab", "192.168.0.255", hooks);
    }
};

FakeAgent::StatusAnswer up(bool users = true) {
    FakeAgent::StatusAnswer a;
    a.status = running("A", users);
    return a;
}
FakeAgent::StatusAnswer down() {
    FakeAgent::StatusAnswer a;
    a.unreachable = true;
    return a;
}
framesync::SwitchReply reply(int status, const std::string& code = "", const std::string& message = "",
                             const std::string& requestId = "") {
    return {status, code, message, requestId};
}

void testSyncFlow() {
    {
        // Frame Dock quits during the first status call: cancelled, never taken for "off" (no WOL)
        Run r;
        FakeAgent::StatusAnswer quit;
        quit.cancelled = true;
        r.agent.statuses = {quit};
        const auto outcome = r.go();
        CHECK(!outcome.ok && outcome.message == tr(Tx::SyncCancelled));
        CHECK(r.wol.empty() && r.agent.statusCalls == 1);
        CHECK(r.steps == std::vector<Step>({Step::Error}));
    }
    {
        // Cancelled while waiting for the woken PC: the wait ends there
        Run r;
        FakeAgent::StatusAnswer quit;
        quit.cancelled = true;
        r.agent.statuses = {down(), quit};
        const auto outcome = r.go();
        CHECK(!outcome.ok && outcome.message == tr(Tx::SyncCancelled));
        CHECK(r.wol.size() == 1 && r.agent.statusCalls == 2 && r.agent.claims == 0);
    }
    {
        // The PC answers and someone is logged in: no WOL, no claim; switch at once
        Run r;
        r.agent.statuses = {up()};
        r.agent.switches = {reply(200)};
        const auto out = r.go();
        CHECK(out.ok && out.message.empty());
        CHECK(r.wol.empty() && r.agent.claims == 0 && r.agent.target == "B");
        CHECK((r.steps == std::vector<Step> {Step::WaitingLogin, Step::Switching, Step::Done}));
    }
    {
        // Off: WOL once (the config's MAC and broadcast), poll every 2 s until the agent answers, then claim
        Run r;
        r.agent.statuses = {down(), down(), down(), up()};
        r.agent.switches = {reply(200)};
        const auto out = r.go();
        CHECK(out.ok);
        CHECK((r.wol == std::vector<std::string> {"01:23:45:67:89:ab@192.168.0.255"}));
        CHECK(r.agent.claims == 1 && r.clock.sleeps == 3);
        CHECK((r.steps == std::vector<Step> {Step::Waking, Step::WaitingLogin, Step::Switching, Step::Done}));
    }
    {
        // Never wakes: gives up after WAKE_LIMIT (240 s) with the unreachable message
        Run r;
        r.agent.statuses = {down()};
        const auto out = r.go();
        CHECK(!out.ok && out.message == tr(Tx::SyncUnreachable));
        CHECK(std::fabs(r.clock.t - 100 - framesync::kWakeLimitSec) < 1e-9);
        CHECK(r.clock.sleeps == static_cast<int>(framesync::kWakeLimitSec / framesync::kPollSec));
        CHECK(r.steps.back() == Step::Error && r.agent.claims == 0);
    }
    {
        // Woken, but nobody logs in: LOGIN_LIMIT (120 s); a dropped connection meanwhile is just an empty poll
        Run r;
        r.agent.statuses = {up(false), down(), up(false)};
        const auto out = r.go();
        CHECK(!out.ok && out.message == tr(Tx::SyncLogin));
        CHECK(std::fabs(r.clock.t - 100 - framesync::kLoginLimitSec) < 1e-9 && r.agent.switchCalls == 0);
    }
    {
        // The key is refused: the auth message (not the agent's words)
        Run r;
        FakeAgent::StatusAnswer a;
        a.auth = true;
        r.agent.statuses = {a};
        const auto out = r.go();
        CHECK(!out.ok && out.message == tr(Tx::SyncAuth) && r.wol.empty());
    }
    {
        // A game runs: the question; 취소 ends the run, 계속 switches again with force
        Run r;
        r.agent.statuses = {up()};
        r.agent.switches = {reply(409, "game")};
        auto out = r.go();
        CHECK(!out.ok && out.message == tr(Tx::SyncCancelled) && r.asked == 1);
        Run r2;
        r2.agent.statuses = {up()};
        r2.agent.switches = {reply(409, "game"), reply(200)};
        r2.answers = {true};
        out = r2.go();
        CHECK(out.ok && r2.asked == 1 && (r2.agent.forces == std::vector<bool> {false, true}));
    }
    {
        // Another refusal: the agent's message, else the result message
        Run r;
        r.agent.statuses = {up()};
        r.agent.switches = {reply(409, "busy", "다른 전환이 진행 중이에요")};
        CHECK(r.go().message == "다른 전환이 진행 중이에요");
        Run r2;
        r2.agent.statuses = {up()};
        r2.agent.switches = {reply(409)};
        CHECK(r2.go().message == tr(Tx::SyncResult));
    }
    {
        // No desktop session yet: the switch is retried every 2 s for up to LOGIN_LIMIT, then the agent's error
        Run r;
        r.agent.statuses = {up()};
        r.agent.switches = {reply(200)};
        r.agent.switchErrors = {AgentError(AgentError::Kind::Http, 409, "no-session"),
                                AgentError(AgentError::Kind::Http, 409, "no-session")};
        CHECK(r.go().ok && r.agent.switchCalls == 3 && r.clock.sleeps == 2);
        Run r2;
        r2.agent.statuses = {up()};
        for (int i = 0; i < 100; ++i) {
            r2.agent.switchErrors.push_back(AgentError(AgentError::Kind::Http, 409, "no-session"));
        }
        const auto out = r2.go();
        CHECK(!out.ok && out.message == "집 PC 에이전트 오류(HTTP 409)");
        CHECK(std::fabs(r2.clock.t - 100 - framesync::kLoginLimitSec) < 1e-9);
        // Any other agent error at once, with its message
        Run r3;
        r3.agent.statuses = {up()};
        r3.agent.switchErrors = {AgentError(AgentError::Kind::Http, 500, "", "스팀을 찾지 못했어요")};
        CHECK(r3.go().message == "스팀을 찾지 못했어요" && r3.agent.switchCalls == 1);
    }
    {
        // Accepted (202): poll the result; done and ok
        Run r;
        r.agent.statuses = {up()};
        r.agent.switches = {reply(202, "", "", "req-1")};
        r.agent.results = {{false, false, ""}, {false, false, ""}, {true, true, ""}};
        CHECK(r.go().ok && r.agent.resultCalls == 3);
        // Done but failed: its message
        Run r2;
        r2.agent.statuses = {up()};
        r2.agent.switches = {reply(202, "", "", "req-2")};
        r2.agent.results = {{true, false, "로그인 창이 떴어요"}};
        CHECK(r2.go().message == "로그인 창이 떴어요");
        // Never done (or unreachable all along): RESULT_LIMIT (180 s, longer than the agent's 150 s)
        Run r3;
        r3.agent.statuses = {up()};
        r3.agent.switches = {reply(202, "", "", "req-3")};
        r3.agent.resultsUnreachable = true;
        const auto out = r3.go();
        CHECK(!out.ok && out.message == tr(Tx::SyncResult));
        CHECK(std::fabs(r3.clock.t - 100 - framesync::kResultLimitSec) < 1e-9);
        CHECK(framesync::kResultLimitSec > 150);
    }
    {
        // Anything unexpected still ends the run with a message (the page never stays "in progress")
        Run r;
        r.agent.statuses = {up()};
        r.agent.throwStd = true;
        const auto out = r.go();
        CHECK(!out.ok && out.message == "예상하지 못한 오류: boom" && r.steps.back() == Step::Error);
    }
}

// ---- The model ----

void testAudioModel() {
    DeviceModel m;
    AudioState audio;
    audio.known = true;
    audio.outputs = {{"spk", "", true}, {"bt", "Wireless Headset", false}};
    audio.defaultId = "spk";
    audio.volume = 40;
    m.setAudio(audio);
    CHECK(outputLabel(audio.outputs[0]) == "헤드셋 스피커" && outputLabel(audio.outputs[1]) == "Wireless Headset");
    CHECK(outputLabel({"sink.x", "", false}) == "sink.x");
    m.takeActions();
    // Picking an output: at once in the state, queued once; the same one again does nothing
    m.selectOutput("bt");
    m.selectOutput("bt");
    auto actions = m.takeActions();
    CHECK(actions.size() == 1 && actions[0].kind == Kind::SelectOutput && actions[0].id == "bt");
    CHECK(m.audio.defaultId == "bt");
    // Dragging: changes are queued (not repeats), clamped; the release queues the final value
    const unsigned before = m.revision();
    m.dragVolume(41);
    m.dragVolume(41);
    m.dragVolume(150);
    CHECK(m.volumeDragging() && m.audio.volume == 100 && m.revision() != before);
    // A reading meanwhile doesn't make the knob jump back
    audio.volume = 40;
    m.setAudio(audio);
    CHECK(m.audio.volume == 100);
    m.releaseVolume();
    m.releaseVolume();  // nothing more
    actions = m.takeActions();
    CHECK(actions.size() == 3);
    CHECK(actions[0].kind == Kind::SetVolume && actions[0].value == 41 && !actions[0].final);
    CHECK(actions[1].value == 100 && !actions[1].final);
    CHECK(actions[2].kind == Kind::SetVolume && actions[2].value == 100 && actions[2].final);
    CHECK(!m.volumeDragging());
    m.setAudio(audio);  // after the drag the reading wins
    CHECK(m.audio.volume == 40);
    // Mute
    m.setMuted(true);
    m.setMuted(true);
    actions = m.takeActions();
    CHECK(actions.size() == 1 && actions[0].kind == Kind::SetMuted && actions[0].flag && m.audio.muted);
}

void testPassthroughModel() {
    DeviceModel m;
    m.passthrough = {PassthroughMode::Off, true, false};  // Camera Switch not running
    m.setPassthrough(PassthroughMode::Color);
    CHECK(m.takeActions().empty() && m.passthrough.mode == PassthroughMode::Off);  // no colour to pick
    m.setPassthrough(PassthroughMode::Mono);  // 켬: on, the colour left as it is
    CHECK(m.takeActions().size() == 1 && m.passthrough.mode == PassthroughMode::Mono);
    m.passthrough.mode = PassthroughMode::Off;
    m.passthrough.cameraSwitchAvailable = true;
    m.setPassthrough(PassthroughMode::Mono);
    auto actions = m.takeActions();
    CHECK(actions.size() == 1 && actions[0].kind == Kind::SetPassthrough && actions[0].mode == PassthroughMode::Mono);
    CHECK(m.passthrough.roomViewKnown && !m.passthrough.roomViewConfirmed);
    // Not sure (what we asked for, or a remembered colour): the same button goes out again
    m.setPassthrough(PassthroughMode::Mono);
    CHECK(m.takeActions().size() == 1);
    m.passthrough.roomViewConfirmed = true;
    m.passthrough.color = ColorSource::Remembered;
    m.setPassthrough(PassthroughMode::Mono);
    CHECK(m.takeActions().size() == 1);
    // Sure (SteamVR said it's on, the colour was read): nothing to do
    m.passthrough.roomViewConfirmed = true;
    m.passthrough.color = ColorSource::Read;
    m.setPassthrough(PassthroughMode::Mono);
    CHECK(m.takeActions().empty());
    // 끔 needs no colour: SteamVR's word is enough
    m.passthrough.mode = PassthroughMode::Off;
    m.passthrough.color = ColorSource::Remembered;
    m.setPassthrough(PassthroughMode::Off);
    CHECK(m.takeActions().empty());
    // Unknown state: a press picks it even if it may already be so
    m.passthrough = {PassthroughMode::Off, false, false};
    m.setPassthrough(PassthroughMode::Off);
    actions = m.takeActions();
    CHECK(actions.size() == 1 && m.passthrough.roomViewKnown);
}

void testSyncModel() {
    DeviceModel m;
    CHECK(previewDeviceState(m, ""));
    SyncScreen screen = syncScreen(m.sync);
    // The PC on another account: the strip with that account, the big button switches, the others listed
    CHECK(screen.strip == "● 집 PC 켜짐" && screen.tone == SyncScreen::Tone::On && screen.pcAccountName == "Sam");
    CHECK(screen.bigText == "집 PC를 Alex로 바꾸기" && screen.bigEnabled && screen.shutdownEnabled);
    CHECK(screen.rows.size() == 4 && screen.rows[0].name == "Sam" && screen.rowsEnabled);
    // 끄기 asks first; starting a sync answers that question 취소 (no shutdown), then runs
    m.askShutdown();
    CHECK(syncScreen(m.sync).question == "집 PC를 끌까요?" && syncScreen(m.sync).yesText == "끄기");
    m.startSync(m.sync.frameId, false);
    auto actions = m.takeActions();
    CHECK(actions.size() == 1 && actions[0].kind == Kind::StartSync && actions[0].id == m.sync.frameId && !actions[0].flag);
    CHECK(m.sync.question == SyncState::Question::None && m.sync.busy && m.sync.busyName == "Alex");
    screen = syncScreen(m.sync);
    CHECK(!screen.bigEnabled && !screen.rowsEnabled && !screen.shutdownEnabled);
    // Not twice
    m.startSync("76561198000000002", true);
    CHECK(m.takeActions().empty());
    // Progress: waking turns the strip until the end; the big button says the step
    m.syncProgress(Step::Waking);
    m.syncProgress(Step::Switching);
    screen = syncScreen(m.sync);
    CHECK(screen.strip == "● 집 PC 켜는 중" && screen.tone == SyncScreen::Tone::Waking);
    CHECK(screen.bigText == "Alex · 스팀 전환 중");
    // A game runs: the question; no answer in ASK_TIMEOUT is 취소
    m.askGame(10);
    CHECK(syncScreen(m.sync).question == "집 PC에서 게임이 실행 중이에요. 바꾸면 게임이 꺼져요. 계속할까요?");
    CHECK(syncScreen(m.sync).yesText == "계속");
    CHECK(m.nextWake() == 10 + framesync::kAskTimeoutSec);
    CHECK(!m.tick(10 + framesync::kAskTimeoutSec - 1));
    CHECK(m.tick(10 + framesync::kAskTimeoutSec));
    actions = m.takeActions();
    CHECK(actions.size() == 1 && actions[0].kind == Kind::AnswerGame && !actions[0].flag);
    CHECK(m.sync.question == SyncState::Question::None && std::isinf(m.nextWake()));
    // 계속
    m.askGame(20);
    m.answer(true);
    actions = m.takeActions();
    CHECK(actions.size() == 1 && actions[0].kind == Kind::AnswerGame && actions[0].flag);
    // The end: a failure in red; from the list, the reminder to switch the Frame's own account
    m.syncFinished(false, tr(Tx::SyncCancelled));
    screen = syncScreen(m.sync);
    CHECK(!m.sync.busy && screen.message == tr(Tx::SyncCancelled) && screen.messageError);
    CHECK(screen.strip == "● 집 PC 켜짐" && screen.bigEnabled);
    m.startSync("76561198000000002", true);
    CHECK(m.sync.message.empty());
    m.syncFinished(true, "");
    CHECK(m.sync.message == "집 PC를 바꿨어요. 프레임 계정은 스팀 메뉴에서 직접 바꿔 주세요." && !m.sync.messageError);
    m.startSync(m.sync.frameId, false);
    m.syncFinished(true, "");
    CHECK(m.sync.message.empty());
    // 끄기 confirmed queues the shutdown; refused, nothing
    m.takeActions();
    m.askShutdown();
    m.answer(false);
    CHECK(m.takeActions().empty());
    m.askShutdown();
    m.answer(true);
    actions = m.takeActions();
    CHECK(actions.size() == 1 && actions[0].kind == Kind::ShutdownPc);
    // A refresh error goes on the line; it clears when the error is gone
    m.applyRefresh(std::nullopt, Reach::On, ErrorKind::Auth, tr(Tx::SyncAuth));
    screen = syncScreen(m.sync);
    CHECK(screen.message == tr(Tx::SyncAuth) && screen.messageError && screen.tone == SyncScreen::Tone::Error);
    CHECK(!screen.shutdownEnabled && !screen.bigEnabled);
    m.askShutdown();
    CHECK(m.sync.question == SyncState::Question::None);  // not while the PC refuses
    m.applyRefresh(running("76561198000000001"), Reach::On, ErrorKind::None, "");
    screen = syncScreen(m.sync);
    CHECK(screen.message.empty() && screen.bigText == "Alex · 스트리밍 준비됨" && !screen.bigEnabled);
    // Off: the PC's account isn't shown, the button wakes it; the cached list still gives the rows
    m.applyRefresh(std::nullopt, Reach::Off, ErrorKind::None, "");
    screen = syncScreen(m.sync);
    CHECK(screen.strip == "● 집 PC 꺼짐" && screen.pcAccountId.empty() && screen.bigText == "집 PC 켜고 Alex로 스팀 켜기");
    CHECK(screen.rows.size() == 4);
    // No settings: nothing works
    m.sync.configured = false;
    screen = syncScreen(m.sync);
    CHECK(screen.bigText == tr(Tx::SyncNoSettings) && !screen.bigEnabled && screen.strip.empty());
    m.startSync(m.sync.frameId, false);
    CHECK(m.takeActions().empty());
    // Every preview variant is known
    for (size_t i = 0; i < kPreviewDeviceVariantCount; ++i) CHECK(previewDeviceState(m, kPreviewDeviceVariants[i]));
    CHECK(!previewDeviceState(m, "nope"));
}

// ---- The page ----

void testHitMath() {
    // segmentAt follows pillPartLeft: 3 parts in a 310 px pill at 10 (the inside 300 from 15)
    CHECK(segmentAt(9, 10, 310, 3) == -1 && segmentAt(321, 10, 310, 3) == -1);
    CHECK(segmentAt(10, 10, 310, 3) == 0);   // the rim belongs to the end part
    CHECK(segmentAt(114.9, 10, 310, 3) == 0 && segmentAt(115, 10, 310, 3) == 1);
    CHECK(segmentAt(215, 10, 310, 3) == 2 && segmentAt(320, 10, 310, 3) == 2);
    CHECK(segmentAt(50, 10, 310, 0) == -1);
    CHECK(segmentAt(100, 10, 310, 1) == 0);
    // draggedVolume: from the volume where the drag began, the track's width is 100 %, rounded, clamped; where the press
    // began doesn't matter
    CHECK(draggedVolume(40, 300, 300, 400) == 40 && draggedVolume(40, 300, 400, 400) == 65);
    CHECK(draggedVolume(40, 490, 390, 400) == 15 && draggedVolume(40, 100, 200, 400) == 65);
    CHECK(draggedVolume(90, 300, 500, 400) == 100 && draggedVolume(10, 300, 100, 400) == 0);
    CHECK(draggedVolume(40, 300, 301.9, 400) == 40 && draggedVolume(40, 300, 302, 400) == 41);
    CHECK(draggedVolume(40, 300, 500, 0) == 40);
    // steppedVolume: onto the 5 % grid, then whole steps; clamped
    CHECK(steppedVolume(40, 1) == 45 && steppedVolume(40, -1) == 35);
    CHECK(steppedVolume(43, 1) == 45 && steppedVolume(43, -1) == 40);
    CHECK(steppedVolume(100, 1) == 100 && steppedVolume(0, -1) == 0 && steppedVolume(98, 1) == 100);
    CHECK(sliderFraction(5, 0, 0) == 0);
}

struct Canvas {
    FontSet fonts;
    cairo_surface_t* surface;
    cairo_t* cr;
    Canvas() {
        const Config cfg;
        fonts.load(cfg.fontPath, cfg.boldFontPath);
        surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1600, 738);
        cr = cairo_create(surface);
    }
    ~Canvas() {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
    }
    Pen pen() { return {cr, &fonts}; }
};

bool overlap(const DevicePage::Hit& a, const DevicePage::Hit& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

void checkApart(const DevicePage& page, double maxX, double maxY) {
    const auto& hits = page.hits();
    for (size_t i = 0; i < hits.size(); ++i) {
        CHECK(hits[i].x >= 0 && hits[i].y >= 0 && hits[i].x + hits[i].w <= maxX && hits[i].y + hits[i].h <= maxY);
        for (size_t k = i + 1; k < hits.size(); ++k) {
            // Neighbouring segments share an edge, never more
            if (overlap(hits[i], hits[k])) {
                std::printf("overlap: kinds %d/%d args %d/%d\n", static_cast<int>(hits[i].kind),
                            static_cast<int>(hits[k].kind), hits[i].arg, hits[k].arg);
                CHECK(false);
            }
        }
    }
}

void testPage() {
    Canvas canvas;
    DeviceModel m;
    previewDeviceState(m, "");
    DevicePage page(m);
    const DevicePageArea area {28, 124, 1544, 468};
    page.render(canvas.pen(), area);
    checkApart(page, 1572, 592);
    double x = 0, y = 0;
    // Two outputs, three passthrough modes, the slider, mute, the big button, four accounts in two columns, 끄기
    CHECK(page.hitCenter(DeviceHit::Output, 0, x, y) && page.hitCenter(DeviceHit::Output, 1, x, y));
    CHECK(page.hitCenter(DeviceHit::Volume, 0, x, y) && page.hitCenter(DeviceHit::Mute, 0, x, y));
    for (int mode = 0; mode < 3; ++mode) CHECK(page.hitCenter(DeviceHit::Passthrough, mode, x, y));
    CHECK(page.hitCenter(DeviceHit::SyncBig, 0, x, y) && page.hitCenter(DeviceHit::Shutdown, 0, x, y));
    for (int row = 0; row < 4; ++row) CHECK(page.hitCenter(DeviceHit::SyncRow, row, x, y));
    CHECK(!page.hitCenter(DeviceHit::SyncRow, 4, x, y));
    CHECK(!page.hitCenter(DeviceHit::AnswerYes, 0, x, y));
    m.takeActions();

    // An output: picked at once
    page.hitCenter(DeviceHit::Output, 0, x, y);
    CHECK(page.pointerDown(x, y, 0));
    page.pointerUp();
    auto actions = m.takeActions();
    CHECK(actions.size() == 1 && actions[0].kind == Kind::SelectOutput && m.audio.defaultId == m.audio.outputs[0].id);

    // The slider: a press alone changes nothing, wherever it lands (a laser brushing the right end isn't 100 %)
    page.render(canvas.pen(), area);
    double trackX = 0, trackW = 0;
    page.volumeTrack(trackX, trackW);
    page.hitCenter(DeviceHit::Volume, 0, x, y);
    const int start = m.audio.volume;
    CHECK(start == 45);
    CHECK(page.hitAt(trackX + trackW + 15, y) == DeviceHit::Volume);
    CHECK(page.pointerDown(trackX + trackW + 15, y, 0) && m.audio.volume == start);
    CHECK(!page.pointerMove(trackX + trackW + 10, y) && m.audio.volume == start);  // jitter: under kVolumeDragStartPx
    CHECK(page.pointerUp() && m.audio.volume == start && !m.volumeDragging());
    CHECK(m.takeActions().empty());
    // A drag starts where it passes kVolumeDragStartPx, from the volume then (the backend's 50 % that came in after
    // the press, not the 45 % it found), and moves it by the distance dragged since (a quarter of the track: 25 %),
    // also off the row
    CHECK(page.pointerDown(trackX + 10, y, 0) && m.audio.volume == start);
    AudioState later = m.audio;
    later.volume = 50;
    m.setAudio(later);
    const double dragX = trackX + 10 + kVolumeDragStartPx;
    CHECK(page.pointerMove(dragX, y) && m.audio.volume == 50 && m.volumeDragging());
    CHECK(page.pointerMove(dragX + trackW * 0.25, y) && m.audio.volume == 75);
    CHECK(page.pointerMove(dragX + trackW, y + 300) && m.audio.volume == 100);  // off the row: still dragging
    CHECK(page.pointerMove(dragX - trackW * 0.25, y) && m.audio.volume == 25);
    CHECK(page.pointerMove(dragX - trackW * 0.75, y) && m.audio.volume == 0);
    CHECK(page.pointerUp() && !m.volumeDragging());
    actions = m.takeActions();
    CHECK(actions.size() == 5 && actions.back().final && actions.back().value == 0);
    // The laser leaving mid-drag lets go where it is
    page.pointerDown(trackX + trackW / 2, y, 0);
    page.pointerMove(trackX + trackW / 2 + kVolumeDragStartPx, y);
    page.pointerMove(trackX + trackW / 2 + kVolumeDragStartPx + trackW * 0.3, y);
    CHECK(page.pointerLeave() && !m.volumeDragging());
    actions = m.takeActions();
    CHECK(!actions.empty() && actions.back().final && actions.back().value == 30);

    // − / ＋: a step at once, more while held (HoldRepeat's timing), the final value on the release
    page.render(canvas.pen(), area);
    CHECK(page.hitCenter(DeviceHit::VolumeDown, 0, x, y) && page.hitAt(x, y) == DeviceHit::VolumeDown);
    CHECK(page.pointerDown(x, y, 10.0) && m.audio.volume == 25 && m.volumeDragging());
    CHECK(page.nextRepeatAt() == 10.0 + HoldRepeat::kDelaySec);
    CHECK(!page.takeRepeat(10.2) && m.audio.volume == 25);
    CHECK(page.takeRepeat(10.0 + HoldRepeat::kDelaySec) && m.audio.volume == 20);
    CHECK(page.takeRepeat(10.0 + HoldRepeat::kDelaySec + HoldRepeat::kIntervalSec) && m.audio.volume == 15);
    CHECK(page.pointerUp() && !m.volumeDragging() && std::isinf(page.nextRepeatAt()));
    actions = m.takeActions();
    CHECK(actions.size() == 4 && actions.back().final && actions.back().value == 15);
    page.hitCenter(DeviceHit::VolumeUp, 0, x, y);
    CHECK(page.pointerDown(x, y, 20.0) && m.audio.volume == 20);
    // Sliding off the held button stops the steps; the volume is let go on the release
    double sx = 0, sy = 0;
    page.hitCenter(DeviceHit::Mute, 0, sx, sy);
    page.pointerMove(sx, sy);
    CHECK(std::isinf(page.nextRepeatAt()) && !page.takeRepeat(30.0) && m.audio.volume == 20);
    page.pointerUp();
    actions = m.takeActions();
    CHECK(actions.size() == 2 && actions.back().final && actions.back().value == 20 && !m.volumeDragging());
    // Up to 100 and no further
    m.dragVolume(98);
    m.releaseVolume();
    m.takeActions();
    page.hitCenter(DeviceHit::VolumeUp, 0, x, y);
    page.pointerDown(x, y, 40.0);
    CHECK(m.audio.volume == 100 && page.takeRepeat(40.0 + HoldRepeat::kDelaySec) && m.audio.volume == 100);
    page.pointerLeave();
    actions = m.takeActions();
    CHECK(actions.size() == 2 && actions.back().final && actions.back().value == 100);

    // Mute, passthrough
    page.hitCenter(DeviceHit::Mute, 0, x, y);
    page.pointerDown(x, y, 0);
    page.pointerUp();
    CHECK(m.audio.muted);
    page.hitCenter(DeviceHit::Passthrough, static_cast<int>(PassthroughMode::Mono), x, y);
    page.pointerDown(x, y, 0);
    page.pointerUp();
    CHECK(m.passthrough.mode == PassthroughMode::Mono);

    // A row's 선택: that account, from the list
    m.takeActions();
    page.render(canvas.pen(), area);
    page.hitCenter(DeviceHit::SyncRow, 1, x, y);
    page.pointerDown(x, y, 0);
    page.pointerUp();
    actions = m.takeActions();
    CHECK(actions.size() == 1 && actions[0].kind == Kind::StartSync && actions[0].id == "76561198000000003" &&
          actions[0].flag);
    // During the run: the big button and the rows can't be pressed; a game question shows 계속 / 취소
    m.askGame(0);
    page.render(canvas.pen(), area);
    checkApart(page, 1572, 592);
    CHECK(!page.hitCenter(DeviceHit::SyncBig, 0, x, y) && !page.hitCenter(DeviceHit::SyncRow, 0, x, y));
    CHECK(!page.hitCenter(DeviceHit::Shutdown, 0, x, y));
    CHECK(page.hitCenter(DeviceHit::AnswerNo, 0, x, y));
    page.pointerDown(x, y, 0);
    page.pointerUp();
    actions = m.takeActions();
    CHECK(actions.size() == 1 && actions[0].kind == Kind::AnswerGame && !actions[0].flag);

    // Camera Switch off: 끔 | 켬 (no 컬러 to pick); one output: one segment
    previewDeviceState(m, "unavailable");
    page.render(canvas.pen(), area);
    checkApart(page, 1572, 592);
    CHECK(page.hitCenter(DeviceHit::Passthrough, 0, x, y));
    CHECK(page.hitCenter(DeviceHit::Passthrough, 1, x, y) && !page.hitCenter(DeviceHit::Passthrough, 2, x, y));
    CHECK(!page.hitCenter(DeviceHit::Output, 0, x, y) && !page.hitCenter(DeviceHit::Volume, 0, x, y));
    CHECK(!page.hitCenter(DeviceHit::SyncBig, 0, x, y));  // the PC not checked yet
    previewDeviceState(m, "single");
    page.render(canvas.pen(), area);
    CHECK(page.hitCenter(DeviceHit::Output, 0, x, y) && !page.hitCenter(DeviceHit::Output, 1, x, y));

    // The Dock's card: three rows inside the given width
    previewDeviceState(m, "");
    DevicePage quick(m);
    quick.renderQuick(canvas.pen(), 48, 672, 904);
    checkApart(quick, 952, 672 + DevicePage::kQuickRowsH);
    CHECK(quick.hitCenter(DeviceHit::Output, 1, x, y) && quick.hitCenter(DeviceHit::Volume, 0, x, y));
    CHECK(quick.hitCenter(DeviceHit::Passthrough, 2, x, y) && !quick.hitCenter(DeviceHit::SyncBig, 0, x, y));
    // ...on the same model: the dashboard's page sees what the Dock changed
    quick.hitCenter(DeviceHit::Passthrough, 0, x, y);
    quick.pointerDown(x, y, 0);
    quick.pointerUp();
    CHECK(m.passthrough.mode == PassthroughMode::Off);
}

}  // namespace

int main() {
    testScreenFunctions();
    testSyncFlow();
    testAudioModel();
    testPassthroughModel();
    testSyncModel();
    testHitMath();
    testPage();
    if (failures > 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("device: all passed\n");
    return 0;
}
