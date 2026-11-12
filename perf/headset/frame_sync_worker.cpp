// Frame Sync's worker thread. See frame_sync_worker.h.
#include "frame_sync_worker.h"

#include "atomic_file.h"
#include "frame_steam.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using framesync::AgentError;
using framesync::ErrorKind;
using framesync::Reach;

namespace {

double monotonic() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/** app.py's messages for a refused / failed poll (its _refresh_worker). */
void describe(const AgentError& error, SyncEvent& event) {
    if (error.kind == AgentError::Kind::Cancelled) return;  // quitting: nothing is shown
    if (error.kind == AgentError::Kind::Auth) {
        event.error = ErrorKind::Auth;
        event.errorText = tr(Tx::SyncAuth);
    } else if (error.kind != AgentError::Kind::Unreachable) {
        event.error = ErrorKind::Http;
        event.errorText =
            !error.message.empty() ? error.message : trf(Tx::SyncAgentHttp, {std::to_string(error.status)});
    }
}

}  // namespace

FrameSyncWorker::Paths FrameSyncWorker::Paths::defaults() {
    const char* home = std::getenv("HOME");
    const std::string h = home != nullptr ? home : "";
    Paths p;
    p.configDir = framesync::defaultConfigDir();
    p.cacheFile = framesync::defaultCacheFile();
    p.steamDir = framesync::defaultSteamDir();
    p.avatarDir = h + "/.cache/framedock/avatars";
    return p;
}

FrameSyncWorker::FrameSyncWorker(Paths paths, int wakeFd, Wol wol)
    : paths_(std::move(paths)), wakeFd_(wakeFd), wol_(std::move(wol)) {
    if (!wol_) {
        wol_ = [](const std::string& mac, const std::string& broadcast) {
            std::string error;
            if (framesync::sendWol(mac, broadcast, error)) {
                std::fprintf(stderr, "[frame-sync] Wake-on-LAN sent to %s\n", broadcast.c_str());
            } else {
                std::fprintf(stderr, "[frame-sync] Wake-on-LAN failed: %s\n", error.c_str());
            }
        };
    }
}

FrameSyncWorker::~FrameSyncWorker() {
    stop();
}

void FrameSyncWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void FrameSyncWorker::post(Command command) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return;
        if (command.job == Job::Refresh) {
            for (const Command& c : queue_) {
                if (c.job == Job::Refresh) return;
            }
        }
        queue_.push_back(std::move(command));
        if (!thread_.joinable()) {
            framesync::initHttp();  // libcurl's global setup on the caller's thread, only once Frame Sync is used
            thread_ = std::thread([this] { run(); });
        }
    }
    cv_.notify_all();
}

void FrameSyncWorker::setup() {
    post({Job::Setup, {}});
}

void FrameSyncWorker::refresh() {
    post({Job::Refresh, {}});
}

void FrameSyncWorker::startSync(const std::string& steamId) {
    post({Job::Sync, steamId});
}

void FrameSyncWorker::shutdownPc() {
    post({Job::Shutdown, {}});
}

void FrameSyncWorker::answerGame(bool yes) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!asking_) return;
        answer_ = yes;
    }
    cv_.notify_all();
}

std::vector<SyncEvent> FrameSyncWorker::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SyncEvent> taken;
    taken.swap(events_);
    return taken;
}

void FrameSyncWorker::emit(SyncEvent event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(event));
    }
    if (wakeFd_ >= 0) {
        const uint64_t one = 1;
        (void)!::write(wakeFd_, &one, sizeof(one));
    }
}

void FrameSyncWorker::run() {
    while (true) {
        Command command;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            command = std::move(queue_.front());
            queue_.pop_front();
        }
        switch (command.job) {
            case Job::Setup: doSetup(); break;
            case Job::Refresh: doRefresh(); break;
            case Job::Sync: doSync(command.id); break;
            case Job::Shutdown: doShutdown(); break;
        }
    }
}

bool FrameSyncWorker::sleepFor(double seconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    return !cv_.wait_for(lock, std::chrono::duration<double>(seconds), [this] { return stop_.load(); });
}

void FrameSyncWorker::doSetup() {
    settings_ = framesync::loadSettings(paths_.configDir);
    SyncEvent event;
    event.kind = SyncEvent::Kind::Setup;
    event.configured = settings_.has_value();
    const auto users = framesync::parseLoginUsers(framesync::readLoginUsers(paths_.steamDir));
    event.frameAccounts = framesync::frameAccounts(users);
    event.frameId = framesync::currentSteamId(users);
    for (const framesync::FrameAccount& a : event.frameAccounts) {
        const std::string path = framesync::avatarPath(paths_.steamDir, a.steamId);
        if (!path.empty()) event.avatars[a.steamId] = path;
    }
    event.pcAccounts = framesync::loadCachedPcAccounts(paths_.cacheFile);
    if (!event.configured) std::fprintf(stderr, "[frame-sync] No usable settings in %s\n", paths_.configDir.c_str());
    emit(std::move(event));
}

void FrameSyncWorker::doRefresh() {
    SyncEvent event;
    event.kind = SyncEvent::Kind::Refresh;
    if (!settings_) {
        emit(std::move(event));  // Unknown: nothing to ask
        return;
    }
    event.reach = Reach::Off;
    framesync::HttpAgent agent(settings_->agentUrl, settings_->token, framesync::kStatusTimeoutSec, &stop_);
    try {
        event.status = agent.status();
        event.reach = Reach::On;
        JsonValue raw;
        std::vector<framesync::PcAccount> accounts = agent.accounts(&raw);
        std::string error;
        if (!framesync::saveCachedPcAccounts(paths_.cacheFile, raw, error)) {
            std::fprintf(stderr, "[frame-sync] Couldn't save the PC's accounts: %s\n", error.c_str());
        }
        event.pcAccounts = accounts;
        for (const framesync::PcAccount& a : accounts) {
            if (!a.hasAvatar || avatarsAsked_.count(a.steamId) > 0 ||
                !framesync::avatarPath(paths_.steamDir, a.steamId).empty()) {
                continue;
            }
            const std::optional<std::string> png = agent.avatar(a.steamId);
            avatarsAsked_.insert(a.steamId);
            if (!png || !framesync::validSteamId(a.steamId)) continue;
            const std::string path = paths_.avatarDir + "/" + a.steamId + ".png";
            if (frame_perf_overlay::atomicWriteFile(path, *png, error)) {
                event.avatars[a.steamId] = path;
            } else {
                std::fprintf(stderr, "[frame-sync] Couldn't keep an avatar: %s\n", error.c_str());
            }
        }
    } catch (const AgentError& error) {
        describe(error, event);
    } catch (const std::exception& error) {
        event.error = ErrorKind::Http;  // so the poll never dies quietly
        event.errorText = trf(Tx::SyncUnexpected, {error.what()});
    }
    if (stop_) return;
    emit(std::move(event));
}

bool FrameSyncWorker::confirmGame() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        asking_ = true;
        answer_.reset();
    }
    SyncEvent ask;
    ask.kind = SyncEvent::Kind::AskGame;
    emit(std::move(ask));
    std::unique_lock<std::mutex> lock(mutex_);
    // The page answers 취소 itself after kAskTimeoutSec; this bound only keeps a lost answer from hanging the run
    cv_.wait_for(lock, std::chrono::duration<double>(framesync::kAskTimeoutSec + 5),
                 [this] { return stop_ || answer_.has_value(); });
    asking_ = false;
    return answer_.value_or(false);
}

void FrameSyncWorker::doSync(const std::string& target) {
    SyncEvent done;
    done.kind = SyncEvent::Kind::Finished;
    if (!settings_) {
        done.message = tr(Tx::SyncNoSettings);
        emit(std::move(done));
        return;
    }
    framesync::HttpAgent agent(settings_->agentUrl, settings_->token, framesync::kSyncTimeoutSec, &stop_);
    framesync::SyncHooks hooks;
    hooks.sleep = [this](double seconds) {
        if (!sleepFor(seconds)) throw std::runtime_error(tr(Tx::SyncQuitting));
    };
    hooks.now = monotonic;
    hooks.wol = [this](const std::string& mac, const std::string& broadcast) {
        if (stop_) throw std::runtime_error(tr(Tx::SyncQuitting));  // never wake the PC on the way out
        wol_(mac, broadcast);
    };
    hooks.progress = [this](framesync::Step step, const std::string&) {
        if (step == framesync::Step::Error || step == framesync::Step::Done) return;  // Finished says it
        SyncEvent event;
        event.kind = SyncEvent::Kind::Progress;
        event.step = step;
        emit(std::move(event));
    };
    hooks.confirmGame = [this] { return confirmGame(); };
    std::fprintf(stderr, "[frame-sync] Switching the PC's Steam\n");
    const framesync::SyncOutcome outcome = framesync::runSync(agent, target, settings_->pcMac, settings_->broadcast, hooks);
    std::fprintf(stderr, "[frame-sync] Sync %s%s\n", outcome.ok ? "done" : "failed: ", outcome.message.c_str());
    if (stop_) return;
    done.ok = outcome.ok;
    done.message = outcome.message;
    emit(std::move(done));
}

void FrameSyncWorker::doShutdown() {
    SyncEvent event;
    event.kind = SyncEvent::Kind::Message;
    if (!settings_) return;
    framesync::HttpAgent agent(settings_->agentUrl, settings_->token, framesync::kStatusTimeoutSec, &stop_);
    try {
        agent.shutdown();
        event.message = tr(Tx::SyncShutdownSoon);
    } catch (const AgentError& error) {
        event.message = !error.message.empty() ? error.message : tr(Tx::SyncShutdownFailed);
        event.messageError = true;
    } catch (const std::exception&) {
        event.message = tr(Tx::SyncShutdownFailed);
        event.messageError = true;
    }
    if (stop_) return;
    emit(std::move(event));
}
