// Slow sensor reads off the main loop. Some sysfs files answer only after the driver talks to its chip (the power
// monitor: 1.4 ms of CPU and up to 9 ms blocked per read on the Frame; the board temperatures and the battery
// gauge too), and the main loop must not stall on them. A source registered here is read on this class's thread:
// the scheduler's read (main thread, only when the source is demanded, as before) asks for a reading, and the
// result is stored by applyReady() on the main thread, which the finished read wakes (wakeFd).
#pragma once

#include "sensors.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class AsyncSampler {
public:
    using Read = std::function<void(SensorSample&)>;            ///< runs on the sampler's thread
    using Apply = std::function<void(const SensorSample&, double now)>;  ///< runs on the main thread

    /** @param wakeFd an eventfd (or −1) written when a reading is ready, so the main loop's poll() returns */
    explicit AsyncSampler(int wakeFd = -1);
    ~AsyncSampler();  ///< finishes the read in progress, then joins
    AsyncSampler(const AsyncSampler&) = delete;
    AsyncSampler& operator=(const AsyncSampler&) = delete;

    /** Registers a source. @return its key for request() */
    int add(Read read, Apply apply);
    /** Asks for a reading of key (main thread). Nothing more while one is asked for and not applied yet. */
    void request(int key);
    /** Stores every finished reading (main thread). @return how many */
    int applyReady(double now);
    /** @return true if the thread runs (false: request() reads at once on the caller's thread) */
    bool threaded() const { return thread_.joinable(); }
    /** Readings done so far (tests) */
    int reads() const;

private:
    struct Slot {
        Read read;
        Apply apply;
        bool asked = false;   ///< requested, not read yet
        bool busy = false;    ///< requested and not applied yet
        bool ready = false;   ///< read, waiting for applyReady
        SensorSample sample;
    };
    int wakeFd_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Slot> slots_;
    bool stop_ = false;
    int reads_ = 0;
    std::thread thread_;
    void run();
};
