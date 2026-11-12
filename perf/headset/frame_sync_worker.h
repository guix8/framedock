// Frame Sync's work on its own thread (app.py's worker threads): reading the settings and the Frame's accounts, the
// status poll (status, the PC's accounts, their avatars), a whole sync (runSync, minutes at most) and the PC's 끄기.
// One job at a time, in order; a poll asked for while one is queued is dropped. Results go back as SyncEvents and
// the main loop's eventfd is poked. The thread starts with the first job and is joined on destruction; a call in
// progress (HTTP, a sync's sleep, the game question) ends at once then.
#pragma once

#include "device_backend.h"
#include "frame_sync_agent.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

class FrameSyncWorker : public SyncService {
public:
    struct Paths {
        std::string configDir;  ///< config.json and token
        std::string cacheFile;  ///< the PC's account list (shared with app.py)
        std::string steamDir;   ///< the Frame's Steam (loginusers.vdf, avatarcache)
        std::string avatarDir;  ///< avatars the PC sent (PNG files the page loads)
        /** ~/.config/frame-sync, ~/.cache/frame-sync/pc-accounts.json, ~/.local/share/Steam, ~/.cache/framedock/avatars */
        static Paths defaults();
    };
    /** Sends the magic packet (the default logs and calls framesync::sendWol). */
    using Wol = std::function<void(const std::string& mac, const std::string& broadcast)>;

    /** @param wakeFd written when events are ready (−1: none) */
    FrameSyncWorker(Paths paths, int wakeFd, Wol wol = {});
    ~FrameSyncWorker() override;
    FrameSyncWorker(const FrameSyncWorker&) = delete;
    FrameSyncWorker& operator=(const FrameSyncWorker&) = delete;

    void setup() override;
    void refresh() override;
    void startSync(const std::string& steamId) override;
    void answerGame(bool yes) override;
    void shutdownPc() override;
    std::vector<SyncEvent> take() override;

    /** Ends the job in progress and joins the thread (also done by the destructor). */
    void stop();

private:
    enum class Job { Setup, Refresh, Sync, Shutdown };
    struct Command {
        Job job;
        std::string id;
    };
    Paths paths_;
    int wakeFd_;
    Wol wol_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Command> queue_;
    std::vector<SyncEvent> events_;
    std::atomic<bool> stop_ {false};
    bool asking_ = false;            ///< the game question is open (guarded by mutex_)
    std::optional<bool> answer_;     ///< its answer
    std::thread thread_;

    // The worker thread's own
    std::optional<framesync::Settings> settings_;
    std::set<std::string> avatarsAsked_;  ///< the PC's avatars fetched (or found missing) once per run, as app.py

    void post(Command command);
    void run();
    void emit(SyncEvent event);
    void doSetup();
    void doRefresh();
    void doSync(const std::string& target);
    void doShutdown();
    /** Sleeps unless stopping. @return false if stopping */
    bool sleepFor(double seconds);
    bool confirmGame();
};
