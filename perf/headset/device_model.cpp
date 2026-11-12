// The 기기 page's state. See device_model.h.
#include "device_model.h"

#include "app_text.h"

#include <algorithm>
#include <limits>

using framesync::ErrorKind;
using framesync::Reach;

std::string outputLabel(const AudioOutput& output) {
    if (output.builtin) return tr(Tx::DeviceSpeakers);
    return output.name.empty() ? output.id : output.name;
}

std::map<std::string, std::string> SyncState::names() const {
    std::map<std::string, std::string> result;
    for (const framesync::FrameAccount& a : frameAccounts) result[a.steamId] = a.personaName;
    if (pcAccounts) {
        for (const framesync::PcAccount& a : *pcAccounts) result.emplace(a.steamId, a.personaName);  // setdefault
    }
    return result;
}

framesync::IdSet SyncState::pcIds() const {
    if (!pcAccounts) return std::nullopt;
    std::set<std::string> ids;
    for (const framesync::PcAccount& a : *pcAccounts) ids.insert(a.steamId);
    return ids;
}

SyncScreen syncScreen(const SyncState& state) {
    SyncScreen screen;
    const auto names = state.names();
    const auto nameOf = [&names](const std::string& id) {
        const auto it = names.find(id);
        return it != names.end() ? it->second : std::string();
    };
    if (!state.configured) {
        // app.py without its settings: nothing works, the button says why
        screen.bigText = tr(Tx::SyncNoSettings);
        screen.bigAccountId = state.frameId;
        return screen;
    }
    const bool answering = state.reach == Reach::On && state.errorKind == ErrorKind::None;
    screen.strip =
        state.waking ? std::string(tr(Tx::SyncPcWaking)) : framesync::stripText(state.reach, state.errorKind);
    screen.tone = state.errorKind != ErrorKind::None ? SyncScreen::Tone::Error
                  : state.waking                     ? SyncScreen::Tone::Waking
                  : state.reach == Reach::On         ? SyncScreen::Tone::On
                                                     : SyncScreen::Tone::Muted;
    if (answering && state.status) {
        screen.pcAccountId = state.status->steamId;
        screen.pcAccountName = screen.pcAccountId.empty() ? std::string() : nameOf(screen.pcAccountId);
        if (state.status->streaming) screen.extra = tr(Tx::SyncStreaming);
        if (!state.status->gameAppId.empty()) {
            screen.extra += std::string(screen.extra.empty() ? "" : " · ") + tr(Tx::SyncInGame);
        }
    }
    screen.shutdownEnabled = answering && !state.busy;

    const framesync::IdSet pcIds = state.pcIds();
    const framesync::PcStatus* status = state.status ? &*state.status : nullptr;
    const framesync::BigButton big =
        framesync::bigButton(state.frameId, pcIds, status, state.reach, state.errorKind, nameOf(state.frameId));
    screen.bigText = big.text;
    screen.bigEnabled = big.enabled && !state.busy;
    if (state.busy && state.step && *state.step != framesync::Step::Error) {
        screen.bigText = state.busyName + " · " + framesync::stepText(*state.step);  // app.py's _progress
    }
    screen.bigAccountId = state.frameId;
    screen.rows = framesync::listRows(state.frameAccounts, state.frameId, pcIds, names);
    screen.rowsEnabled = !state.busy;
    if (state.question == SyncState::Question::Game) {
        screen.question = tr(Tx::SyncAskGame);
        screen.yesText = tr(Tx::SyncContinue);
    } else if (state.question == SyncState::Question::Shutdown) {
        screen.question = tr(Tx::SyncAskShutdown);
        screen.yesText = tr(Tx::SyncTurnOff);
    }
    screen.message = state.message;
    screen.messageError = state.messageError;
    return screen;
}

bool DeviceModel::applyRefresh(const std::optional<framesync::PcStatus>& status, Reach reach, ErrorKind error,
                               const std::string& errorText) {
    const SyncState before = sync;
    const bool hadError = sync.errorKind != ErrorKind::None;
    sync.status = status;
    sync.reach = reach;
    sync.errorKind = error;
    if (!errorText.empty()) {
        sync.message = errorText;
        sync.messageError = true;
    } else if (hadError) {
        sync.message.clear();
        sync.messageError = false;
    }
    // The poll runs every 5 s and mostly says the same: a redraw (and both panels' texture upload) only on a change
    const bool changed = sync.status != before.status || sync.reach != before.reach ||
                         sync.errorKind != before.errorKind || sync.message != before.message ||
                         sync.messageError != before.messageError;
    if (changed) ++revision_;
    return changed;
}

void DeviceModel::setAudio(const AudioState& state) {
    const int dragged = audio.volume;
    audio = state;
    if (dragging_) audio.volume = dragged;
    ++revision_;
}

void DeviceModel::syncProgress(framesync::Step step) {
    sync.step = step;
    if (step == framesync::Step::Waking) sync.waking = true;
    ++revision_;
}

void DeviceModel::syncFinished(bool ok, const std::string& message) {
    sync.busy = false;
    sync.step.reset();
    sync.waking = false;
    if (sync.question == SyncState::Question::Game) sync.question = SyncState::Question::None;
    if (ok && sync.fromList) {
        sync.message = tr(Tx::SyncSwitchedFromList);
        sync.messageError = false;
    } else if (ok) {
        sync.message.clear();
        sync.messageError = false;
    } else {
        sync.message = message;
        sync.messageError = true;
    }
    ++revision_;
}

void DeviceModel::askGame(double now) {
    sync.question = SyncState::Question::Game;
    sync.questionUntil = now + framesync::kAskTimeoutSec;
    ++revision_;
}

void DeviceModel::showMessage(const std::string& text, bool error) {
    sync.message = text;
    sync.messageError = error;
    ++revision_;
}

void DeviceModel::selectOutput(const std::string& id) {
    if (id.empty() || id == audio.defaultId) return;
    audio.defaultId = id;
    queue(DeviceAction(DeviceAction::Kind::SelectOutput, id));
}

void DeviceModel::dragVolume(int percent) {
    percent = std::clamp(percent, 0, 100);
    dragging_ = true;
    if (percent == audio.volume) return;
    audio.volume = percent;
    DeviceAction action(DeviceAction::Kind::SetVolume);
    action.value = percent;
    queue(action);
}

void DeviceModel::releaseVolume() {
    if (!dragging_) return;
    dragging_ = false;
    DeviceAction action(DeviceAction::Kind::SetVolume);
    action.value = audio.volume;
    action.final = true;
    queue(action);
}

void DeviceModel::setMuted(bool muted) {
    if (muted == audio.muted) return;
    audio.muted = muted;
    DeviceAction action(DeviceAction::Kind::SetMuted);
    action.flag = muted;
    queue(action);
}

void DeviceModel::setPassthrough(PassthroughMode mode) {
    // Without Camera Switch only on (the colour left as it is: asked as Mono) and off
    if (mode == PassthroughMode::Color && !passthrough.cameraSwitchAvailable) return;
    const bool sure = passthrough.roomViewKnown && passthrough.roomViewConfirmed &&
                      (mode == PassthroughMode::Off || passthrough.color == ColorSource::Read);
    if (sure && passthrough.mode == mode) return;
    passthrough.mode = mode;
    passthrough.roomViewKnown = true;  // what the backend makes it; its next reading corrects it
    passthrough.roomViewConfirmed = false;
    DeviceAction action(DeviceAction::Kind::SetPassthrough);
    action.mode = mode;
    queue(action);
}

void DeviceModel::startSync(const std::string& steamId, bool fromList) {
    if (sync.busy || steamId.empty() || !sync.configured) return;
    if (sync.question == SyncState::Question::Shutdown) answer(false);
    const auto names = sync.names();
    const auto it = names.find(steamId);
    sync.busy = true;
    sync.busyName = it != names.end() ? it->second : std::string();
    sync.step.reset();
    sync.waking = false;
    sync.fromList = fromList;
    sync.message.clear();
    sync.messageError = false;
    DeviceAction action(DeviceAction::Kind::StartSync, steamId);
    action.flag = fromList;
    queue(action);
}

void DeviceModel::askShutdown() {
    if (sync.reach != Reach::On || sync.errorKind != ErrorKind::None || sync.busy) return;
    sync.question = SyncState::Question::Shutdown;
    ++revision_;
}

void DeviceModel::answer(bool yes) {
    const SyncState::Question question = sync.question;
    sync.question = SyncState::Question::None;
    if (question == SyncState::Question::Game) {
        DeviceAction action(DeviceAction::Kind::AnswerGame);
        action.flag = yes;
        queue(action);
    } else if (question == SyncState::Question::Shutdown && yes) {
        queue(DeviceAction(DeviceAction::Kind::ShutdownPc));
    } else {
        ++revision_;
    }
}

bool DeviceModel::tick(double now) {
    if (sync.question == SyncState::Question::Game && now >= sync.questionUntil) {
        answer(false);  // app.py's confirm_game: no answer in ASK_TIMEOUT is 취소
        return true;
    }
    return false;
}

double DeviceModel::nextWake() const {
    if (sync.question == SyncState::Question::Game) return sync.questionUntil;
    return std::numeric_limits<double>::infinity();
}

std::vector<DeviceAction> DeviceModel::takeActions() {
    std::vector<DeviceAction> taken;
    taken.swap(actions_);
    return taken;
}
