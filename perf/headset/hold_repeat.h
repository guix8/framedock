// Hold to repeat: the timing of a dashboard button held down with the laser's trigger. The press itself acts once
// (the caller does that); then the button repeats after kDelaySec, every kIntervalSec, and every kFastIntervalSec once
// it has been held kFastAfterSec. A safety stop ends it when no pointer event came for kIdleStopSec (a lost release:
// SteamVR not sending the button-up). Single-threaded (the main loop); times are monotonic seconds.
#pragma once

#include <limits>

class HoldRepeat {
public:
    static constexpr double kDelaySec = 0.4;          ///< from the press to the first repeat
    static constexpr double kIntervalSec = 0.1;       ///< between repeats
    static constexpr double kFastAfterSec = 2.0;      ///< held this long (from the press): faster
    static constexpr double kFastIntervalSec = 0.05;  ///< between repeats after that
    static constexpr double kIdleStopSec = 10.0;      ///< no pointer event for this long: stop (safety)

    /** The button was pressed (and acted once): the first repeat comes kDelaySec later. */
    void start(double now) {
        active_ = true;
        pressedAt_ = now;
        lastEventAt_ = now;
        next_ = now + kDelaySec;
    }

    /** A pointer event came in (a move on the held button): the safety stop counts from now. */
    void touch(double now) {
        if (active_ && now > lastEventAt_) lastEventAt_ = now;
    }

    /** Released, left, page switched, dashboard hidden: no more repeats. */
    void stop() { active_ = false; }

    /** @return true while a button is held */
    bool active() const { return active_; }

    /** @return when the next repeat is due (+infinity when nothing is held), for the main loop's sleep */
    double nextAt() const { return active_ ? next_ : std::numeric_limits<double>::infinity(); }

    /**
     * Whether a repeat is due now; if so, schedules the next one. A caller that woke up late gets one repeat, not a
     * burst: the next one is an interval after now when it fell more than an interval behind. More than kIdleStopSec
     * after the last pointer event (start / touch) it stops instead.
     * @param now monotonic seconds
     * @return true if the held button should act once more
     */
    bool due(double now) {
        if (!active_ || now < next_ - 1e-6) return false;  // a hair early still counts (float sums of intervals)
        if (now - lastEventAt_ > kIdleStopSec) {  // the release got lost somewhere: don't run on forever
            active_ = false;
            return false;
        }
        const double interval = now - pressedAt_ >= kFastAfterSec ? kFastIntervalSec : kIntervalSec;
        next_ = now - next_ >= interval ? now + interval : next_ + interval;
        return true;
    }

private:
    bool active_ = false;
    double pressedAt_ = 0.0;
    double lastEventAt_ = 0.0;
    double next_ = 0.0;
};
