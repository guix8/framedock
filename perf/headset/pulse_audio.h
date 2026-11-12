// The 기기 page's sound on the Pulse API (pipewire-pulse on SteamOS), with libpulse's threaded main loop: its own
// thread sleeps until the server says something. While wanted, the client subscribes to sink and server events and
// reads the sinks again after each (several events in a row make one reading); a reading that differs from the last
// one is handed to the main loop (take()) and its eventfd poked. Not wanted: the subscription is dropped at once (no
// event wakes anyone) and the connection is closed kIdleDisconnectSec later.
//
// Readings are at most one per kQueryGapSec: a burst of events (a game opening its streams, another app dragging the
// volume) makes one reading at once and one more after the gap, however many came.
//
// Choosing an output makes the microphone on the same hardware the default source too (audio::micFor; the sources are
// read once for that, not followed).
//
// Commands (default output, volume, mute) are sent without waiting for the answer; the server's change events bring
// the new state back. A refused command makes the sinks be read again and that reading handed over even if it looks
// the same as the last one (the page showed the command's value meanwhile). A command given before the connection is
// ready waits for it (only the last of each kind).
#pragma once

#include "device_backend.h"

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct pa_threaded_mainloop;
struct pa_context;
struct pa_time_event;
struct pa_cvolume;

class PulseAudio : public AudioService {
public:
    /** Seconds after the page is hidden until the connection is closed (shown again in between: kept). */
    static constexpr double kIdleDisconnectSec = 30;
    /** Seconds between connection attempts while wanted and the server is gone. */
    static constexpr double kRetrySec = 3;
    /** Seconds from one reading's start to the next one's while events keep coming. */
    static constexpr double kQueryGapSec = 0.1;

    /** @param wakeFd written when a new reading is ready while wanted (−1: none). Nothing starts until setWanted. */
    explicit PulseAudio(int wakeFd);
    /** Lets the last commands reach the server (up to 0.3 s), then disconnects and stops the thread. */
    ~PulseAudio() override;
    PulseAudio(const PulseAudio&) = delete;
    PulseAudio& operator=(const PulseAudio&) = delete;

    void setWanted(bool wanted) override;
    bool take(AudioState& out) override;
    void selectOutput(const std::string& id) override;
    void setVolume(int percent) override;
    void setMuted(bool muted) override;

    // libpulse's callbacks (run on its thread with the loop locked)
    void onState();
    void onEvent(unsigned facility);
    void onServerInfo(const char* defaultSink, const char* defaultSource);
    void onSink(const void* info, int eol);
    void onSource(const char* name, bool monitor, int eol);
    void onDone(bool success, const char* what);
    void onIdleTimer();
    void onRetryTimer();
    void onQueryTimer();

    /** Tests: readings started so far. */
    int queries() const { return queries_.load(); }

private:
    int wakeFd_;
    pa_threaded_mainloop* loop_ = nullptr;
    pa_context* context_ = nullptr;
    // Guarded by the loop's lock
    bool wanted_ = false;
    bool ready_ = false;
    bool connecting_ = false;  ///< inside pa_context_connect
    bool querying_ = false;
    bool queryAgain_ = false;
    std::string defaultSink_;
    std::string defaultSource_;
    std::string speakerSink_;               ///< the headset speaker's sink in the last reading (the fallback microphone)
    std::string micForSink_;                ///< the output whose microphone the source reading looks for
    std::vector<std::string> sources_;      ///< that reading so far
    int sourceReads_ = 0;                   ///< source readings on their way (the last one decides)
    std::vector<audio::SinkInfo> reading_;
    std::map<std::string, std::vector<uint32_t>> channels_;  ///< each sink's channel volumes (the balance is kept)
    std::string volumeSink_;                                  ///< the sink the page's volume and mute act on
    pa_time_event* idleTimer_ = nullptr;
    pa_time_event* retryTimer_ = nullptr;
    pa_time_event* queryTimer_ = nullptr;  ///< the next reading of a burst
    double lastQueryAt_ = -1e300;          ///< when the last reading started (monotonic seconds)
    std::atomic<int> queries_ {0};
    std::optional<std::string> pendingSelect_;
    int pendingVolume_ = -1;
    std::optional<bool> pendingMute_;
    int inFlight_ = 0;
    std::string lastError_;  ///< logged once per new error

    // The reading for the main thread
    std::mutex mutex_;
    bool fresh_ = false;
    bool forcePublish_ = false;  ///< the next reading goes out even if it looks like state_ (a refused command)
    AudioState state_;           ///< the last reading handed over (what the page has, but for its own changes)

    void connect();
    void dropContext();
    void subscribe(bool on);
    void query();
    /** query() now, or after kQueryGapSec since the last one started (once, however often this is called) */
    void queryCoalesced();
    void publish(const AudioState& state);
    void sendPending();
    void doSelect(const std::string& name);
    /** Reads the sources and makes micForSink_'s microphone the default (onSource). */
    void followMic();
    void doVolume(int percent);
    void doMute(bool muted);
    enum class Timer { Idle, Retry, Query };
    void armTimer(pa_time_event*& timer, double seconds, Timer which);
    void freeTimer(pa_time_event*& timer);
    void logError(const std::string& what);
};
