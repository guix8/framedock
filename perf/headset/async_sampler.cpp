// Slow sensor reads on their own thread. See async_sampler.h.
#include "async_sampler.h"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <system_error>

AsyncSampler::AsyncSampler(int wakeFd) : wakeFd_(wakeFd) {
    try {
        thread_ = std::thread(&AsyncSampler::run, this);
    } catch (const std::system_error& e) {
        std::fprintf(stderr, "[sensors] No sampler thread (%s): slow sensors are read on the main loop\n", e.what());
    }
}

AsyncSampler::~AsyncSampler() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

int AsyncSampler::add(Read read, Apply apply) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot slot;
    slot.read = std::move(read);
    slot.apply = std::move(apply);
    slots_.push_back(std::move(slot));
    return static_cast<int>(slots_.size()) - 1;
}

void AsyncSampler::request(int key) {
    if (!thread_.joinable()) {
        // No thread: read here and now, as without a sampler
        SensorSample sample;
        Read read;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            read = slots_[static_cast<size_t>(key)].read;
            ++reads_;
        }
        read(sample);
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& slot = slots_[static_cast<size_t>(key)];
        slot.sample = std::move(sample);
        slot.ready = slot.busy = true;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& slot = slots_[static_cast<size_t>(key)];
        if (slot.busy) return;  // one in flight (or read, not applied yet): it will be fresh enough
        slot.busy = slot.asked = true;
    }
    cv_.notify_one();
}

int AsyncSampler::applyReady(double now) {
    std::vector<std::pair<Apply*, SensorSample>> done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Slot& slot : slots_) {
            if (!slot.ready) continue;
            done.emplace_back(&slot.apply, std::move(slot.sample));
            slot.ready = slot.busy = false;
        }
    }
    // Outside the lock: the store is the main thread's (slots_ isn't resized after the sources are registered)
    for (auto& [apply, sample] : done) (*apply)(sample, now);
    return static_cast<int>(done.size());
}

int AsyncSampler::reads() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reads_;
}

void AsyncSampler::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        size_t index = slots_.size();
        cv_.wait(lock, [&] {
            if (stop_) return true;
            for (size_t i = 0; i < slots_.size(); ++i) {
                if (slots_[i].asked) {
                    index = i;
                    return true;
                }
            }
            return false;
        });
        if (stop_) return;
        Slot& slot = slots_[index];
        slot.asked = false;
        const Read read = slot.read;
        lock.unlock();
        SensorSample sample;
        read(sample);  // the slow part, with nothing locked
        lock.lock();
        ++reads_;
        slots_[index].sample = std::move(sample);
        slots_[index].ready = true;
        // One wake for the batch: the sources the scheduler asked for in one pass are read back to back, and the main
        // loop is woken once, after the last of them (not once per source)
        bool moreAsked = false;
        for (const Slot& other : slots_) moreAsked |= other.asked;
        if (!moreAsked && !stop_ && wakeFd_ >= 0) {
            const uint64_t one = 1;
            (void)!::write(wakeFd_, &one, sizeof(one));
        }
    }
}
