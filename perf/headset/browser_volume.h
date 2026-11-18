// The browser's own volume, apart from the output's (the 기기 page's): Chrome's playback streams (sink inputs) on the
// sound server get the level and mute set in the browser's 보기 list. Chrome is started with
// PULSE_PROP_OVERRIDE=application.id=framedock-browser (chrome_process.cpp), so its streams say whose they are
// (audio::isBrowserStream).
//
// libpulse's threaded main loop, connected from the browser's first start until Frame Dock quits (setActive). The client subscribes to sink-input events:
// a new or changed browser stream whose level or mute isn't the browser's gets it set (a server that rounds the
// level is fine: they are compared in whole percent). A new level reads all the streams and sets each browser one.
// The server gone: tried again every kRetrySec while active.
#pragma once

#include <cstdint>

struct pa_threaded_mainloop;
struct pa_context;
struct pa_time_event;

class BrowserVolume {
public:
    static constexpr double kRetrySec = 3;

    BrowserVolume() = default;
    /** Lets the last commands reach the server (up to 0.3 s), then disconnects and stops the thread. */
    ~BrowserVolume();
    BrowserVolume(const BrowserVolume&) = delete;
    BrowserVolume& operator=(const BrowserVolume&) = delete;

    /** The browser is in use (connected, following its streams) or not (disconnected). Cheap to call every frame. */
    void setActive(bool active);
    /** The browser's level (0–100) and mute: set on its streams now (if connected) and on each new one. */
    void set(int percent, bool muted);

    // libpulse's callbacks (run on its thread with the loop locked)
    void onState();
    void onEvent(unsigned type, uint32_t index);
    void onStream(const void* info, int eol);
    void onDone(bool success);
    void onRetryTimer();

private:
    bool lastActive_ = false;  ///< what setActive last had (main thread only)
    pa_threaded_mainloop* loop_ = nullptr;
    pa_context* context_ = nullptr;
    // Guarded by the loop's lock
    bool active_ = false;
    bool ready_ = false;
    bool connecting_ = false;
    int percent_ = 100;
    bool muted_ = false;
    int inFlight_ = 0;
    pa_time_event* retryTimer_ = nullptr;

    void connect();
    void dropContext();
    /** Reads every stream (each browser one gets the level) */
    void readAll();
};
