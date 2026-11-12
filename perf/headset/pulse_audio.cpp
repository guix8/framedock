// The sound backend on libpulse. See pulse_audio.h.
#include "pulse_audio.h"

#include <pulse/pulseaudio.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace {

PulseAudio* self(void* user) {
    return static_cast<PulseAudio*>(user);
}

void stateCallback(pa_context*, void* user) {
    self(user)->onState();
}

void eventCallback(pa_context*, pa_subscription_event_type_t type, uint32_t, void* user) {
    self(user)->onEvent(static_cast<unsigned>(type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK));
}

void serverCallback(pa_context*, const pa_server_info* info, void* user) {
    self(user)->onServerInfo(info != nullptr ? info->default_sink_name : nullptr,
                             info != nullptr ? info->default_source_name : nullptr);
}

void sinkCallback(pa_context*, const pa_sink_info* info, int eol, void* user) {
    self(user)->onSink(info, eol);
}

void sourceCallback(pa_context*, const pa_source_info* info, int eol, void* user) {
    self(user)->onSource(eol == 0 && info != nullptr ? info->name : nullptr,
                         eol == 0 && info != nullptr && info->monitor_of_sink != PA_INVALID_INDEX, eol);
}

void selectDone(pa_context*, int success, void* user) {
    self(user)->onDone(success != 0, "Setting the default output");
}

void micDone(pa_context*, int success, void* user) {
    self(user)->onDone(success != 0, "Setting the default microphone");
}

void volumeDone(pa_context*, int success, void* user) {
    self(user)->onDone(success != 0, "Setting the volume");
}

void muteDone(pa_context*, int success, void* user) {
    self(user)->onDone(success != 0, "Setting mute");
}

void idleCallback(pa_mainloop_api*, pa_time_event*, const struct timeval*, void* user) {
    self(user)->onIdleTimer();
}

void retryCallback(pa_mainloop_api*, pa_time_event*, const struct timeval*, void* user) {
    self(user)->onRetryTimer();
}

void queryCallback(pa_mainloop_api*, pa_time_event*, const struct timeval*, void* user) {
    self(user)->onQueryTimer();
}

double monotonic() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool sameState(const AudioState& a, const AudioState& b) {
    if (a.known != b.known || a.defaultId != b.defaultId || a.volume != b.volume || a.muted != b.muted ||
        a.outputs.size() != b.outputs.size()) {
        return false;
    }
    for (size_t i = 0; i < a.outputs.size(); ++i) {
        if (a.outputs[i].id != b.outputs[i].id || a.outputs[i].name != b.outputs[i].name ||
            a.outputs[i].builtin != b.outputs[i].builtin) {
            return false;
        }
    }
    return true;
}

}  // namespace

PulseAudio::PulseAudio(int wakeFd) : wakeFd_(wakeFd) {}

PulseAudio::~PulseAudio() {
    if (loop_ == nullptr) return;
    // A volume or mute just sent (the release of a drag at Quit) should reach the server
    for (int i = 0; i < 30; ++i) {
        pa_threaded_mainloop_lock(loop_);
        const bool busy = inFlight_ > 0;
        pa_threaded_mainloop_unlock(loop_);
        if (!busy) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    pa_threaded_mainloop_lock(loop_);
    freeTimer(idleTimer_);
    freeTimer(retryTimer_);
    freeTimer(queryTimer_);
    dropContext();
    pa_threaded_mainloop_unlock(loop_);
    pa_threaded_mainloop_stop(loop_);
    pa_threaded_mainloop_free(loop_);
}

void PulseAudio::setWanted(bool wanted) {
    if (loop_ == nullptr) {
        if (!wanted) return;
        loop_ = pa_threaded_mainloop_new();
        if (loop_ == nullptr) {
            std::fprintf(stderr, "[sound] Couldn't make libpulse's main loop\n");
            return;
        }
        pa_threaded_mainloop_set_name(loop_, "framedock-pulse");
        if (pa_threaded_mainloop_start(loop_) < 0) {
            std::fprintf(stderr, "[sound] Couldn't start libpulse's thread\n");
            pa_threaded_mainloop_free(loop_);
            loop_ = nullptr;
            return;
        }
    }
    pa_threaded_mainloop_lock(loop_);
    if (wanted != wanted_) {
        wanted_ = wanted;
        if (wanted) {
            freeTimer(idleTimer_);
            if (context_ == nullptr) {
                connect();
            } else if (ready_) {
                subscribe(true);
                query();
            }
        } else {
            if (ready_) subscribe(false);
            freeTimer(queryTimer_);
            armTimer(idleTimer_, kIdleDisconnectSec, Timer::Idle);
        }
    }
    pa_threaded_mainloop_unlock(loop_);
}

bool PulseAudio::take(AudioState& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fresh_) return false;
    fresh_ = false;
    out = state_;
    return true;
}

void PulseAudio::selectOutput(const std::string& id) {
    if (loop_ == nullptr) return;
    pa_threaded_mainloop_lock(loop_);
    if (ready_) doSelect(id);
    else pendingSelect_ = id;
    pa_threaded_mainloop_unlock(loop_);
}

void PulseAudio::setVolume(int percent) {
    if (loop_ == nullptr) return;
    pa_threaded_mainloop_lock(loop_);
    if (ready_ && !volumeSink_.empty()) doVolume(percent);
    else pendingVolume_ = percent;
    pa_threaded_mainloop_unlock(loop_);
}

void PulseAudio::setMuted(bool muted) {
    if (loop_ == nullptr) return;
    pa_threaded_mainloop_lock(loop_);
    if (ready_ && !volumeSink_.empty()) doMute(muted);
    else pendingMute_ = muted;
    pa_threaded_mainloop_unlock(loop_);
}

// ---- On libpulse's thread (or with the loop locked) ----

void PulseAudio::connect() {
    pa_proplist* props = pa_proplist_new();
    pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, "Frame Dock");
    pa_proplist_sets(props, PA_PROP_APPLICATION_ID, "framedock");
    context_ = pa_context_new_with_proplist(pa_threaded_mainloop_get_api(loop_), "Frame Dock", props);
    pa_proplist_free(props);
    if (context_ == nullptr) {
        logError("pa_context_new failed");
        publish(AudioState());  // unknown (the page says 확인 중…)
        armTimer(retryTimer_, kRetrySec, Timer::Retry);
        return;
    }
    pa_context_set_state_callback(context_, stateCallback, this);
    connecting_ = true;  // a failure reported inside pa_context_connect is handled below, not in onState
    const int connected = pa_context_connect(context_, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr);
    connecting_ = false;
    if (connected < 0) {
        logError(std::string("Couldn't connect: ") + pa_strerror(pa_context_errno(context_)));
        dropContext();
        publish(AudioState());  // no server (e.g. after the idle disconnect): what the page had is stale
        armTimer(retryTimer_, kRetrySec, Timer::Retry);
    }
}

void PulseAudio::dropContext() {
    if (context_ == nullptr) return;
    pa_context_set_state_callback(context_, nullptr, nullptr);
    pa_context_set_subscribe_callback(context_, nullptr, nullptr);
    pa_context_disconnect(context_);
    pa_context_unref(context_);
    context_ = nullptr;
    ready_ = false;
    querying_ = queryAgain_ = false;
    inFlight_ = 0;
    // What belonged to that connection: the next one reads them again before a command acts on them
    freeTimer(queryTimer_);
    defaultSink_.clear();
    defaultSource_.clear();
    micForSink_.clear();
    sources_.clear();
    sourceReads_ = 0;
    reading_.clear();
    channels_.clear();
    volumeSink_.clear();
}

void PulseAudio::onState() {
    if (context_ == nullptr) return;
    switch (pa_context_get_state(context_)) {
        case PA_CONTEXT_READY:
            ready_ = true;
            lastError_.clear();
            pa_context_set_subscribe_callback(context_, eventCallback, this);
            if (wanted_) {
                subscribe(true);
                query();
            }
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED: {
            if (connecting_) break;
            logError(std::string("The sound server went away: ") + pa_strerror(pa_context_errno(context_)));
            ready_ = false;
            // Not freed inside its own callback: the retry timer does that (and connects again if still wanted)
            AudioState unknown;
            publish(unknown);
            armTimer(retryTimer_, wanted_ ? kRetrySec : 0.01, Timer::Retry);
            break;
        }
        default: break;
    }
}

void PulseAudio::onRetryTimer() {
    freeTimer(retryTimer_);
    if (context_ != nullptr && !ready_) dropContext();
    if (wanted_ && context_ == nullptr) connect();
}

void PulseAudio::onIdleTimer() {
    freeTimer(idleTimer_);
    if (wanted_ || inFlight_ > 0) {
        if (!wanted_) armTimer(idleTimer_, 1, Timer::Idle);  // a command still on its way: a little later
        return;
    }
    // state_ stays what the page has: the reading after the next connect goes out only if it differs, and a server
    // that is gone by then makes it "unknown" (connect() / onState)
    dropContext();
}

void PulseAudio::onQueryTimer() {
    freeTimer(queryTimer_);
    if (wanted_) query();
}

void PulseAudio::subscribe(bool on) {
    const auto mask = on ? static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER)
                         : PA_SUBSCRIPTION_MASK_NULL;
    pa_operation* op = pa_context_subscribe(context_, mask, nullptr, nullptr);
    if (op != nullptr) pa_operation_unref(op);
}

void PulseAudio::onEvent(unsigned facility) {
    if (!wanted_) return;
    if (facility == PA_SUBSCRIPTION_EVENT_SINK || facility == PA_SUBSCRIPTION_EVENT_SERVER) queryCoalesced();
}

void PulseAudio::queryCoalesced() {
    if (!ready_ || queryTimer_ != nullptr) return;  // one is due already
    if (querying_) {
        queryAgain_ = true;  // one more reading after this one, however many events came
        return;
    }
    const double wait = lastQueryAt_ + kQueryGapSec - monotonic();
    if (wait <= 0) query();
    else armTimer(queryTimer_, wait, Timer::Query);
}

void PulseAudio::query() {
    if (!ready_) return;
    if (querying_) {
        queryAgain_ = true;
        return;
    }
    freeTimer(queryTimer_);  // this reading covers it
    querying_ = true;
    lastQueryAt_ = monotonic();
    ++queries_;
    reading_.clear();
    // Answered in order: the server's default first, then the sinks (the list's end finishes the reading)
    pa_operation* a = pa_context_get_server_info(context_, serverCallback, this);
    pa_operation* b = pa_context_get_sink_info_list(context_, sinkCallback, this);
    if (a != nullptr) pa_operation_unref(a);
    if (b != nullptr) {
        pa_operation_unref(b);
    } else {
        querying_ = false;
    }
}

void PulseAudio::onServerInfo(const char* defaultSink, const char* defaultSource) {
    defaultSink_ = defaultSink != nullptr ? defaultSink : "";
    defaultSource_ = defaultSource != nullptr ? defaultSource : "";
}

void PulseAudio::onSink(const void* data, int eol) {
    if (eol == 0 && data != nullptr) {
        const auto* info = static_cast<const pa_sink_info*>(data);
        audio::SinkInfo sink;
        sink.name = info->name != nullptr ? info->name : "";
        sink.description = info->description != nullptr ? info->description : "";
        sink.volume = pa_cvolume_max(&info->volume);
        sink.muted = info->mute != 0;
        if (info->proplist != nullptr) {
            void* state = nullptr;
            while (const char* key = pa_proplist_iterate(info->proplist, &state)) {
                const char* value = pa_proplist_gets(info->proplist, key);
                if (value != nullptr) sink.props[key] = value;
            }
        }
        channels_[sink.name].assign(info->volume.values, info->volume.values + info->volume.channels);
        reading_.push_back(std::move(sink));
        return;
    }
    // The end of the list (or an error: the reading so far)
    querying_ = false;
    const AudioState state = audio::stateFrom(reading_, defaultSink_);
    volumeSink_ = !state.defaultId.empty() ? state.defaultId : defaultSink_;
    speakerSink_.clear();
    for (const AudioOutput& output : state.outputs) {
        if (output.builtin) speakerSink_ = output.id;
    }
    publish(state);
    sendPending();
    if (queryAgain_) {
        queryAgain_ = false;
        queryCoalesced();
    }
}

void PulseAudio::publish(const AudioState& state) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!forcePublish_ && sameState(state, state_)) return;
        forcePublish_ = false;
        state_ = state;
        fresh_ = true;
    }
    if (wanted_ && wakeFd_ >= 0) {
        const uint64_t one = 1;
        (void)!::write(wakeFd_, &one, sizeof(one));
    }
}

void PulseAudio::sendPending() {
    if (!ready_) return;
    if (pendingSelect_) {
        doSelect(*pendingSelect_);
        pendingSelect_.reset();
    }
    if (volumeSink_.empty()) return;
    if (pendingVolume_ >= 0) {
        doVolume(pendingVolume_);
        pendingVolume_ = -1;
    }
    if (pendingMute_) {
        doMute(*pendingMute_);
        pendingMute_.reset();
    }
}

void PulseAudio::doSelect(const std::string& name) {
    pa_operation* op = pa_context_set_default_sink(context_, name.c_str(), selectDone, this);
    if (op == nullptr) return logError("Setting the default output failed");
    ++inFlight_;
    pa_operation_unref(op);
    volumeSink_ = name;  // the volume that follows is the new output's (its own reading comes soon)
    micForSink_ = name;
    followMic();
}

void PulseAudio::followMic() {
    if (sourceReads_ == 0) sources_.clear();  // else the one on its way adds to it: the names are what counts
    pa_operation* op = pa_context_get_source_info_list(context_, sourceCallback, this);
    if (op == nullptr) return logError("Reading the microphones failed");
    ++sourceReads_;
    pa_operation_unref(op);
}

void PulseAudio::onSource(const char* name, bool monitor, int eol) {
    if (eol == 0) {
        if (name != nullptr && !monitor) sources_.emplace_back(name);
        return;
    }
    // The end of a list (an error: what came so far). The last reading on its way decides, for the last choice
    if (sourceReads_ > 0) --sourceReads_;
    if (sourceReads_ > 0 || micForSink_.empty()) return;
    const std::string mic = audio::micFor(micForSink_, sources_, speakerSink_);
    micForSink_.clear();
    if (mic.empty()) return;  // sent even if it looks the same: defaultSource_ is only as fresh as the last reading
    pa_operation* op = pa_context_set_default_source(context_, mic.c_str(), micDone, this);
    if (op == nullptr) return logError("Setting the default microphone failed");
    ++inFlight_;
    pa_operation_unref(op);
    defaultSource_ = mic;
    std::fprintf(stderr, "[sound] The microphone follows the output: %s\n", mic.c_str());
}

void PulseAudio::doVolume(int percent) {
    const pa_volume_t target = audio::volumeOf(percent);
    pa_cvolume cv;
    const auto known = channels_.find(volumeSink_);
    if (known != channels_.end() && !known->second.empty() && known->second.size() <= PA_CHANNELS_MAX) {
        cv.channels = static_cast<uint8_t>(known->second.size());
        for (size_t i = 0; i < known->second.size(); ++i) cv.values[i] = known->second[i];
        if (pa_cvolume_max(&cv) == PA_VOLUME_MUTED) pa_cvolume_set(&cv, cv.channels, target);
        else pa_cvolume_scale(&cv, target);  // keeps the balance
    } else {
        pa_cvolume_set(&cv, 2, target);
    }
    pa_operation* op = pa_context_set_sink_volume_by_name(context_, volumeSink_.c_str(), &cv, volumeDone, this);
    if (op == nullptr) return logError("Setting the volume failed");
    ++inFlight_;
    pa_operation_unref(op);
}

void PulseAudio::doMute(bool muted) {
    pa_operation* op = pa_context_set_sink_mute_by_name(context_, volumeSink_.c_str(), muted ? 1 : 0, muteDone, this);
    if (op == nullptr) return logError("Setting mute failed");
    ++inFlight_;
    pa_operation_unref(op);
}

void PulseAudio::onDone(bool success, const char* what) {
    if (inFlight_ > 0) --inFlight_;
    if (!success && context_ != nullptr) {
        logError(std::string(what) + " was refused: " + pa_strerror(pa_context_errno(context_)));
        // The page shows the refused value: read the real one and hand it over even if nothing changed
        {
            std::lock_guard<std::mutex> lock(mutex_);
            forcePublish_ = true;
        }
        query();
    }
}

void PulseAudio::armTimer(pa_time_event*& timer, double seconds, Timer which) {
    pa_mainloop_api* api = pa_threaded_mainloop_get_api(loop_);
    struct timeval when {};
    pa_gettimeofday(&when);
    pa_timeval_add(&when, static_cast<pa_usec_t>(seconds * 1e6));
    if (timer != nullptr) {
        api->time_restart(timer, &when);
    } else {
        const pa_time_event_cb_t callback = which == Timer::Idle    ? idleCallback
                                            : which == Timer::Retry ? retryCallback
                                                                    : queryCallback;
        timer = api->time_new(api, &when, callback, this);
    }
}

void PulseAudio::freeTimer(pa_time_event*& timer) {
    if (timer == nullptr) return;
    pa_threaded_mainloop_get_api(loop_)->time_free(timer);
    timer = nullptr;
}

void PulseAudio::logError(const std::string& what) {
    if (what == lastError_) return;
    lastError_ = what;
    std::fprintf(stderr, "[sound] %s\n", what.c_str());
}
