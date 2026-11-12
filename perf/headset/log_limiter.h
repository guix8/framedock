// Keeps a repeating failure from filling the journal: a message is logged when it is new for its key, and the same
// message again only after kRepeatSec (saying how many were left out meanwhile). No OpenVR here (tested offline).
#pragma once

#include <map>
#include <functional>
#include <string>
#include <string_view>

class LogLimiter {
public:
    static constexpr double kRepeatSec = 60.0;

    /**
     * @param key what failed (e.g. "SetOverlayAlpha", "browser texture")
     * @param message the error text: a different one for the key is logged at once
     * @param now monotonic seconds
     * @param suppressed set to how many of this message were left out since it was last logged
     * @return true if it should be logged now
     */
    bool allow(const std::string& key, const std::string& message, double now, int& suppressed) {
        Entry& e = entries_[key];
        suppressed = 0;
        if (e.logged && e.message == message && now - e.at < kRepeatSec) {
            ++e.suppressed;
            return false;
        }
        suppressed = e.message == message ? e.suppressed : 0;
        e.logged = true;
        e.message = message;
        e.at = now;
        e.suppressed = 0;
        return true;
    }

    /** The key works again: its next failure is logged at once. Called on every success: no allocation (a
     *  string_view lookup in the transparent map, and nothing at all while no failure is remembered). */
    void clear(const char* key) {
        if (entries_.empty()) return;
        const auto it = entries_.find(std::string_view(key));
        if (it != entries_.end()) entries_.erase(it);
    }

private:
    struct Entry {
        bool logged = false;
        std::string message;
        double at = 0;
        int suppressed = 0;
    };
    std::map<std::string, Entry, std::less<>> entries_;
};
