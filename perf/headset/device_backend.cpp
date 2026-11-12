// The 기기 page's backends on its model. See device_backend.h.
#include "device_backend.h"

#include "atomic_file.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

using framesync::kRefreshSec;

std::string rememberedColorFile() {
    const char* state = std::getenv("XDG_STATE_HOME");
    if (state != nullptr && state[0] == '/') return std::string(state) + "/framedock/passthrough-color";
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr ? home : "") + "/.local/state/framedock/passthrough-color";
}

std::optional<bool> readRememberedColor(const std::string& file) {
    std::ifstream in(file);
    std::string word;
    if (!(in >> word)) return std::nullopt;
    if (word == "color") return true;
    if (word == "mono") return false;
    return std::nullopt;
}

bool writeRememberedColor(const std::string& file, bool color, std::string& error) {
    return frame_perf_overlay::atomicWriteFile(file, color ? "color\n" : "mono\n", error);
}

DeviceBackend::DeviceBackend(DeviceModel& model, std::unique_ptr<AudioService> audio,
                             std::unique_ptr<CameraSwitchService> camera, std::unique_ptr<RoomViewControl> roomView,
                             std::unique_ptr<SyncService> sync)
    : model_(model), audio_(std::move(audio)), camera_(std::move(camera)), roomView_(std::move(roomView)),
      sync_(std::move(sync)) {
    composePassthrough();
}

DeviceBackend::~DeviceBackend() = default;

void DeviceBackend::setRememberedColor(std::optional<bool> color) {
    color_ = color;
    composePassthrough();
}


void DeviceBackend::setVisible(bool soundShown, bool syncShown, double now) {
    if (soundShown != soundShown_) {
        soundShown_ = soundShown;
        if (audio_) audio_->setWanted(soundShown);
        if (camera_) camera_->setWanted(soundShown);
    }
    if (syncShown != syncShown_) {
        syncShown_ = syncShown;
        if (syncShown && sync_) {
            // Shown again: the settings and the Frame's accounts may have changed; the poll follows the setup
            sync_->setup();
            setupPending_ = true;
            nextRefresh_ = now;
        }
    }
}

void DeviceBackend::roomViewEvent(bool shown) {
    std::fprintf(stderr, "[passthrough] SteamVR: the room view is %s%s\n", shown ? "shown" : "hidden",
                 roomViewWish_ ? (*roomViewWish_ == shown ? " (as asked)" : " (asked the other way)") : "");
    roomViewSent_ = false;  // the command's answer (or SteamVR's own toggle: the next tick sees what's left to do)
    if (roomViewWish_ && *roomViewWish_ == shown) roomViewWish_.reset();
    roomViewOn_ = shown;
    roomViewAssumed_ = false;
    composePassthrough();
}

void DeviceBackend::driveRoomView(double now) {
    if (!roomView_ || !roomViewWish_ || roomViewSent_) return;
    // An exact command goes out once per press even when the view seems so already (SteamVR's button may have changed it)
    if (roomViewOn_ == roomViewWish_ && !(roomView_->exact() && roomViewTries_ == 0)) {
        roomViewWish_.reset();
        composePassthrough();
        return;
    }
    if (roomViewTries_ >= kRoomViewTries) {
        std::fprintf(stderr, "[passthrough] The room view didn't come %s after %d commands; leaving it\n",
                     *roomViewWish_ ? "on" : "off", roomViewTries_);
        roomViewWish_.reset();
        composePassthrough();
        return;
    }
    std::fprintf(stderr, "[passthrough] Room view command for %s (last known %s%s, try %d)\n",
                 *roomViewWish_ ? "on" : "off", !roomViewOn_ ? "nothing" : *roomViewOn_ ? "shown" : "hidden",
                 roomViewAssumed_ ? ", as last asked" : "", roomViewTries_ + 1);
    roomView_->setEnabled(*roomViewWish_);
    roomViewSent_ = true;
    roomViewSentOn_ = *roomViewWish_;
    roomViewDeadline_ = now + kRoomViewSendSec + kRoomViewEventSec;
    ++roomViewTries_;
}

void DeviceBackend::composePassthrough() {
    PassthroughState& p = model_.passthrough;
    p.cameraSwitchAvailable = cameraAvailable_;
    const std::optional<bool> shown = roomViewWish_ ? roomViewWish_ : roomViewOn_;
    p.roomViewKnown = shown.has_value();
    p.roomViewConfirmed = !roomViewWish_ && roomViewOn_.has_value() && !roomViewAssumed_;
    if (!shown.value_or(false)) {
        p.mode = PassthroughMode::Off;
    } else {
        // Without Camera Switch the cameras keep the colour it set last; never set, they are taken as 흑백
        p.mode = color_.value_or(false) ? PassthroughMode::Color : PassthroughMode::Mono;
    }
    p.color = color_ ? ColorSource::Remembered : ColorSource::Unknown;
    model_.touch();
}

void DeviceBackend::handle(const DeviceAction& action, double now) {
    using Kind = DeviceAction::Kind;
    switch (action.kind) {
        case Kind::SelectOutput:
            if (audio_) audio_->selectOutput(action.id);
            break;
        case Kind::SetVolume: {
            const int value = throttle_.push(action.value, action.final, now);
            if (value >= 0 && audio_) audio_->setVolume(value);
            if (action.final) {
                heldVolume_ = action.value;
                heldSink_ = model_.audio.defaultId;
                holdUntil_ = now + kVolumeHoldSec;
            }
            break;
        }
        case Kind::SetMuted:
            if (audio_) audio_->setMuted(action.flag);
            break;
        case Kind::SetPassthrough:
            std::fprintf(stderr, "[passthrough] Asked: %s\n",
                         action.mode == PassthroughMode::Off ? "off" : action.mode == PassthroughMode::Mono ? "mono" : "color");
            if (action.mode != PassthroughMode::Off) {
                // The colour first (always: Camera Switch's automatic switch may have changed it)
                const bool color = action.mode == PassthroughMode::Color;
                if (camera_ && cameraAvailable_) {
                    camera_->setColor(color);
                    color_ = color;
                }
            }
            if (roomView_) {
                // Then the view: shown as asked while the commands go out; SteamVR's events say where it ends
                roomViewWish_ = action.mode != PassthroughMode::Off;
                roomViewTries_ = 0;
                driveRoomView(now);
            }
            composePassthrough();
            break;
        case Kind::StartSync:
            if (sync_) sync_->startSync(action.id);
            break;
        case Kind::AnswerGame:
            if (sync_) sync_->answerGame(action.flag);
            break;
        case Kind::ShutdownPc:
            if (sync_) sync_->shutdownPc();
            break;
    }
}

void DeviceBackend::applyReading(AudioState state, double now) {
    if (heldVolume_ >= 0) {
        if (now >= holdUntil_ || !state.known || state.defaultId != heldSink_ || state.volume == heldVolume_) {
            heldVolume_ = -1;  // it arrived, ran out, or the output changed
        } else {
            state.volume = heldVolume_;  // an older value sent during the drag
        }
    }
    model_.setAudio(state);
}

void DeviceBackend::applySync(const SyncEvent& event, double now) {
    SyncState& s = model_.sync;
    bool avatarsChanged = false;
    for (const auto& [id, path] : event.avatars) {
        std::string& known = s.avatars[id];
        if (known != path) {
            known = path;
            avatarsChanged = true;
        }
    }
    switch (event.kind) {
        case SyncEvent::Kind::Setup:
            setupPending_ = false;
            s.configured = event.configured;
            s.frameId = event.frameId;
            s.frameAccounts = event.frameAccounts;
            if (event.pcAccounts && !s.pcAccounts) s.pcAccounts = event.pcAccounts;  // the cache until the PC answers
            model_.touch();
            nextRefresh_ = now;
            break;
        case SyncEvent::Kind::Refresh: {
            refreshing_ = false;
            bool changed = avatarsChanged;
            if (event.pcAccounts && event.pcAccounts != s.pcAccounts) {
                s.pcAccounts = event.pcAccounts;
                changed = true;
            }
            // Only a real change redraws (the poll comes every kRefreshSec)
            if (!model_.applyRefresh(event.status, event.reach, event.error, event.errorText) && changed) model_.touch();
            break;
        }
        case SyncEvent::Kind::Progress: model_.syncProgress(event.step); break;
        case SyncEvent::Kind::AskGame: model_.askGame(now); break;
        case SyncEvent::Kind::Finished:
            model_.syncFinished(event.ok, event.message);
            nextRefresh_ = now;  // app.py's _finish refreshes at once
            break;
        case SyncEvent::Kind::Message: model_.showMessage(event.message, event.messageError); break;
    }
}

bool DeviceBackend::tick(double now) {
    const unsigned before = model_.revision();
    for (int pass = 0; pass < 2; ++pass) {
        for (const DeviceAction& action : model_.takeActions()) handle(action, now);
        if (pass == 0) model_.tick(now);  // an unanswered question becomes 취소 (an action, taken in pass 1)
    }
    if (audio_) {
        const int value = throttle_.sendDue(now);
        if (value >= 0) audio_->setVolume(value);
        AudioState state;
        if (audio_->take(state)) {
            lastReading_ = state;
            applyReading(state, now);
        } else if (heldVolume_ >= 0 && now >= holdUntil_) {
            // No reading of the released value came: the last reading is what the server has
            heldVolume_ = -1;
            if (lastReading_ && lastReading_->known && lastReading_->defaultId == heldSink_ &&
                !model_.volumeDragging() && model_.audio.volume != lastReading_->volume) {
                model_.audio.volume = lastReading_->volume;
                model_.touch();
            }
        }
    }
    bool sent = false;
    if (roomView_ && roomView_->takeResult(sent) && roomViewSent_) {
        if (sent) {
            // It went out: its event should follow soon (a slow vrcmd doesn't count against the wait)
            roomViewDeadline_ = std::min(roomViewDeadline_, now + kRoomViewEventSec);
        } else {
            // The command didn't reach SteamVR: give up this wish (the view is what the last event said)
            std::fprintf(stderr, "[passthrough] The room view command didn't reach SteamVR\n");
            roomViewSent_ = false;
            roomViewWish_.reset();
            composePassthrough();
        }
    }
    if (roomViewSent_ && now >= roomViewDeadline_) {
        roomViewSent_ = false;
        if (roomView_->exact()) {
            // Set, not toggled: taken as done (SteamVR tells no change of its 룸 뷰 button's view). A wish that came
            // after it is still to send (driveRoomView below)
            std::fprintf(stderr, "[passthrough] No word from SteamVR: the room view taken as %s\n",
                         roomViewSentOn_ ? "on" : "off");
            roomViewOn_ = roomViewSentOn_;
            roomViewAssumed_ = true;
            if (roomViewWish_ == roomViewSentOn_) roomViewWish_.reset();
        } else {
            // Sent but no event: whether the view is on is not known (SteamVR's next event says)
            std::fprintf(stderr, "[passthrough] No word from SteamVR after the room view command\n");
            roomViewOn_.reset();
            roomViewWish_.reset();
        }
        composePassthrough();
    }
    driveRoomView(now);
    bool available = false;
    if (camera_ && camera_->takeAvailable(available) && available != cameraAvailable_) {
        cameraAvailable_ = available;
        composePassthrough();
    }
    if (sync_) {
        for (const SyncEvent& event : sync_->take()) applySync(event, now);
        // The status poll, every kRefreshSec while the page is shown (not during a run, not twice at once)
        if (syncShown_ && !setupPending_ && model_.sync.configured && now >= nextRefresh_) {
            if (!refreshing_ && !model_.sync.busy) {
                sync_->refresh();
                refreshing_ = true;
            }
            nextRefresh_ = now + kRefreshSec;
        }
    }
    return model_.revision() != before;
}

double DeviceBackend::nextWake() const {
    double wake = std::min(throttle_.nextDue(), model_.nextWake());
    if (heldVolume_ >= 0) wake = std::min(wake, holdUntil_);
    if (roomViewSent_) wake = std::min(wake, roomViewDeadline_);
    if (sync_ && syncShown_ && !setupPending_ && model_.sync.configured && !model_.sync.busy && !refreshing_) {
        wake = std::min(wake, nextRefresh_);
    }
    return wake;
}
