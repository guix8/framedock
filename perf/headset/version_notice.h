// The "new version" notice (2026-11-14): at most once a day, and only while the dashboard tab shows, ask GitHub for
// the latest release's tag and compare it with this build's version. It only tells: nothing is downloaded or
// installed. config.json's "update_check" (default on) turns it off.
//
// The last check (when, and the tag it got) is kept in version_check.json next to config.json, so a restart doesn't
// ask again within the day and still shows a newer version it found.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace version_notice {

constexpr const char* kLatestUrl = "https://api.github.com/repos/guix8/framedock/releases/latest";
/** Where the notice says to get it */
constexpr const char* kReleasesPage = "github.com/guix8/framedock/releases";
constexpr int64_t kIntervalSec = 24 * 3600;
constexpr long kTimeoutMs = 5000;

/** A release version: MAJOR.MINOR.PATCH */
struct Version {
    long major = 0, minor = 0, patch = 0;
};

/**
 * "v0.1.1" or "0.1.1": three numbers and nothing else (a pre-release such as "0.2.0-rc1" isn't one).
 * @return false for anything else
 */
bool parseVersion(const std::string& text, Version& version);

/** @return <0, 0, >0 as a is older than, the same as, newer than b (numerically: 0.10.0 is after 0.9.0) */
int compareVersions(const Version& a, const Version& b);

/** @return true if tag is a version newer than running (false when either doesn't parse) */
bool isNewer(const std::string& tag, const std::string& running);

/**
 * The latest release's tag from GitHub's answer (only "tag_name" is read).
 * @return "" when the body isn't a JSON object with a string tag_name
 */
std::string parseLatestTag(const std::string& body);

/**
 * The once-a-day rule.
 * @param nowUnix the wall clock
 * @param lastUnix the last check (0: never)
 * @return true if a day has passed since the last check, or the clock is now before it (it was set back)
 */
bool checkDue(int64_t nowUnix, int64_t lastUnix);

/** What version_check.json keeps. */
struct State {
    int64_t checkedUnix = 0;  ///< "checked": the last check (asked, whatever the answer)
    std::string latestTag;    ///< "latest": the tag the last answered check got ("" = none yet)
};

/** @return false (state untouched) if the text isn't such a file */
bool parseState(const std::string& text, State& state);
std::string stateToJson(const State& state);

/** @return version_check.json in config.json's folder */
std::string statePath(const std::string& configPath);

/** Asks GitHub off the main thread; the main loop polls it. */
class UpdateChecker {
public:
    /**
     * Reads the state file. Sets up libcurl: construct on the main thread.
     * @param statePath version_check.json
     * @param runningVersion FRAMEDOCK_VERSION
     */
    UpdateChecker(std::string statePath, std::string runningVersion);
    /** Stops a check in flight (curl's progress callback) and waits for it. */
    ~UpdateChecker();
    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    /**
     * Main thread: takes a finished check's answer, and starts one when the tab shows, the setting is on and a day
     * has passed.
     * @param tabVisible the dashboard tab shows
     * @param enabled config.updateCheck
     * @param nowUnix the wall clock
     * @return true if newerTag() changed (redraw the tab)
     */
    bool poll(bool tabVisible, bool enabled, int64_t nowUnix);

    /** @return the newer release's tag ("v0.1.1"), "" when none (or the setting is off) */
    const std::string& newerTag() const { return newer_; }

private:
    std::string path_;
    std::string running_;
    State state_;
    std::string newer_;
    std::thread worker_;
    std::atomic<bool> busy_ {false};
    std::atomic<bool> cancel_ {false};
    std::string fetchedTag_;  ///< the worker's answer ("" = failed); read after busy_ goes false
    std::string fetchError_;

    void save();
};

}  // namespace version_notice
