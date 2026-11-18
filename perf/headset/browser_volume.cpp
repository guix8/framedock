// The browser's own volume on libpulse. See browser_volume.h.
#include "browser_volume.h"

#include "audio_sinks.h"

#include <pulse/pulseaudio.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>

namespace {

BrowserVolume* self(void* user) {
    return static_cast<BrowserVolume*>(user);
}

void stateCallback(pa_context*, void* user) {
    self(user)->onState();
}

void eventCallback(pa_context*, pa_subscription_event_type_t type, uint32_t index, void* user) {
    self(user)->onEvent(static_cast<unsigned>(type), index);
}

void streamCallback(pa_context*, const pa_sink_input_info* info, int eol, void* user) {
    self(user)->onStream(info, eol);
}

void doneCallback(pa_context*, int success, void* user) {
    self(user)->onDone(success != 0);
}

void retryCallback(pa_mainloop_api*, pa_time_event*, const struct timeval*, void* user) {
    self(user)->onRetryTimer();
}

}  // namespace

BrowserVolume::~BrowserVolume() {
    if (loop_ == nullptr) return;
    for (int i = 0; i < 30; ++i) {
        pa_threaded_mainloop_lock(loop_);
        const bool busy = inFlight_ > 0;
        pa_threaded_mainloop_unlock(loop_);
        if (!busy) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    pa_threaded_mainloop_lock(loop_);
    if (retryTimer_ != nullptr) {
        pa_threaded_mainloop_get_api(loop_)->time_free(retryTimer_);
        retryTimer_ = nullptr;
    }
    dropContext();
    pa_threaded_mainloop_unlock(loop_);
    pa_threaded_mainloop_stop(loop_);
    pa_threaded_mainloop_free(loop_);
}

void BrowserVolume::setActive(bool active) {
    if (active == lastActive_) return;
    lastActive_ = active;
    if (loop_ == nullptr) {
        if (!active) return;
        loop_ = pa_threaded_mainloop_new();
        if (loop_ == nullptr) {
            std::fprintf(stderr, "[sound] Couldn't make libpulse's main loop for the browser's volume\n");
            return;
        }
        pa_threaded_mainloop_set_name(loop_, "framedock-bvol");
        if (pa_threaded_mainloop_start(loop_) < 0) {
            std::fprintf(stderr, "[sound] Couldn't start libpulse's thread for the browser's volume\n");
            pa_threaded_mainloop_free(loop_);
            loop_ = nullptr;
            return;
        }
    }
    pa_threaded_mainloop_lock(loop_);
    if (active != active_) {
        active_ = active;
        if (active && context_ == nullptr) connect();
        if (!active && inFlight_ == 0) dropContext();  // a command on its way: the retry timer drops it a bit later
        if (!active && context_ != nullptr && retryTimer_ == nullptr) {
            struct timeval when {};
            pa_gettimeofday(&when);
            pa_timeval_add(&when, 300000);
            retryTimer_ = pa_threaded_mainloop_get_api(loop_)->time_new(pa_threaded_mainloop_get_api(loop_), &when,
                                                                         retryCallback, this);
        }
    }
    pa_threaded_mainloop_unlock(loop_);
}

void BrowserVolume::set(int percent, bool muted) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (loop_ == nullptr) {  // not connected yet: setActive's connection uses these
        percent_ = percent;
        muted_ = muted;
        return;
    }
    pa_threaded_mainloop_lock(loop_);
    const bool changed = percent != percent_ || muted != muted_;
    percent_ = percent;
    muted_ = muted;
    if (changed && ready_) readAll();
    pa_threaded_mainloop_unlock(loop_);
}

// ---- On libpulse's thread (or with the loop locked) ----

void BrowserVolume::connect() {
    pa_proplist* props = pa_proplist_new();
    pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, "Frame Dock (browser volume)");
    pa_proplist_sets(props, PA_PROP_APPLICATION_ID, "framedock");
    context_ = pa_context_new_with_proplist(pa_threaded_mainloop_get_api(loop_), "Frame Dock browser volume", props);
    pa_proplist_free(props);
    const auto retry = [this] {
        struct timeval when {};
        pa_gettimeofday(&when);
        pa_timeval_add(&when, static_cast<pa_usec_t>(kRetrySec * 1e6));
        if (retryTimer_ == nullptr) {
            retryTimer_ = pa_threaded_mainloop_get_api(loop_)->time_new(pa_threaded_mainloop_get_api(loop_), &when,
                                                                         retryCallback, this);
        }
    };
    if (context_ == nullptr) return retry();
    pa_context_set_state_callback(context_, stateCallback, this);
    connecting_ = true;
    const int connected = pa_context_connect(context_, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr);
    connecting_ = false;
    if (connected < 0) {
        dropContext();
        retry();
    }
}

void BrowserVolume::dropContext() {
    if (context_ == nullptr) return;
    pa_context_set_state_callback(context_, nullptr, nullptr);
    pa_context_set_subscribe_callback(context_, nullptr, nullptr);
    pa_context_disconnect(context_);
    pa_context_unref(context_);
    context_ = nullptr;
    ready_ = false;
    inFlight_ = 0;
}

void BrowserVolume::onState() {
    if (context_ == nullptr) return;
    switch (pa_context_get_state(context_)) {
        case PA_CONTEXT_READY: {
            ready_ = true;
            pa_context_set_subscribe_callback(context_, eventCallback, this);
            pa_operation* op = pa_context_subscribe(context_, PA_SUBSCRIPTION_MASK_SINK_INPUT, nullptr, nullptr);
            if (op != nullptr) pa_operation_unref(op);
            readAll();
            break;
        }
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            if (connecting_) break;
            ready_ = false;
            // Not freed inside its own callback: the retry timer does that (and connects again if still active)
            if (retryTimer_ == nullptr) {
                struct timeval when {};
                pa_gettimeofday(&when);
                pa_timeval_add(&when, static_cast<pa_usec_t>((active_ ? kRetrySec : 0.01) * 1e6));
                retryTimer_ = pa_threaded_mainloop_get_api(loop_)->time_new(pa_threaded_mainloop_get_api(loop_),
                                                                             &when, retryCallback, this);
            }
            break;
        default: break;
    }
}

void BrowserVolume::onRetryTimer() {
    pa_threaded_mainloop_get_api(loop_)->time_free(retryTimer_);
    retryTimer_ = nullptr;
    if (context_ != nullptr && (!ready_ || !active_)) dropContext();
    if (active_ && context_ == nullptr) connect();
}

void BrowserVolume::readAll() {
    pa_operation* op = pa_context_get_sink_input_info_list(context_, streamCallback, this);
    if (op != nullptr) pa_operation_unref(op);
}

void BrowserVolume::onEvent(unsigned type, uint32_t index) {
    if (!ready_ || (type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) != PA_SUBSCRIPTION_EVENT_SINK_INPUT) return;
    const unsigned kind = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    if (kind != PA_SUBSCRIPTION_EVENT_NEW && kind != PA_SUBSCRIPTION_EVENT_CHANGE) return;
    pa_operation* op = pa_context_get_sink_input_info(context_, index, streamCallback, this);
    if (op != nullptr) pa_operation_unref(op);
}

void BrowserVolume::onStream(const void* data, int eol) {
    if (eol != 0 || data == nullptr || context_ == nullptr) return;
    const auto* info = static_cast<const pa_sink_input_info*>(data);
    if (info->proplist == nullptr || info->volume.channels == 0) return;
    std::map<std::string, std::string> props;
    for (const char* key : {PA_PROP_APPLICATION_ID, PA_PROP_APPLICATION_PROCESS_BINARY}) {
        if (const char* value = pa_proplist_gets(info->proplist, key)) props[key] = value;
    }
    if (!audio::isBrowserStream(props)) return;
    // Raw values: a stream above 100 % gets the level too; percentOf(volumeOf(p)) == p, so our own change comes back
    // equal and isn't set again
    if (pa_cvolume_max(&info->volume) != audio::volumeOf(percent_)) {
        pa_cvolume cv;
        pa_cvolume_set(&cv, info->volume.channels, audio::volumeOf(percent_));
        pa_operation* op = pa_context_set_sink_input_volume(context_, info->index, &cv, doneCallback, this);
        if (op != nullptr) {
            ++inFlight_;
            pa_operation_unref(op);
            std::fprintf(stderr, "[sound] Browser stream %u: %d%%\n", info->index, percent_);
        }
    }
    if ((info->mute != 0) != muted_) {
        pa_operation* op = pa_context_set_sink_input_mute(context_, info->index, muted_ ? 1 : 0, doneCallback, this);
        if (op != nullptr) {
            ++inFlight_;
            pa_operation_unref(op);
            std::fprintf(stderr, "[sound] Browser stream %u: %s\n", info->index, muted_ ? "muted" : "unmuted");
        }
    }
}

void BrowserVolume::onDone(bool success) {
    if (inFlight_ > 0) --inFlight_;
    if (!success && context_ != nullptr) {
        std::fprintf(stderr, "[sound] Setting the browser's volume was refused: %s\n",
                     pa_strerror(pa_context_errno(context_)));
    }
}
