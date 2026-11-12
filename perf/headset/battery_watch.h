// The headset's time left on battery and the low-battery notices (the headset at 20 % and 10 %, a controller at 20 %).
//
// The main loop feeds it a reading every kReadSec while the headset is worn, independent of the HUD (whose sources
// pause while it's hidden, e.g. the wrist turned away). The time left is the charge left (charge_now, the fuel gauge's
// coulomb count) over the discharge current averaged with a kAverageSec time constant: the gauge's own
// time_to_empty_avg is not used (on the Frame it reads 368634 s even when full). No time on the charger or until
// kMinDataSec of discharge has been averaged. A reading that can't be used (a status blip, a failed read) is skipped;
// no usable one for longer than kGapSec (headset off, idle) starts over.
//
// A notice is due once per threshold: the headset crossing kLowPct / kCriticalPct while discharging (only the more
// serious one when both are crossed at once, e.g. already at 8 % at start), a controller at kControllerLowPct or
// below while not charging. Each comes back after charging (on the charger) or kRearmPct above its threshold.
//
// Pure logic: no files, no OpenVR, no drawing.
#pragma once

#include <cmath>
#include <limits>
#include <string>
#include <vector>

/** One read of the headset battery. NaN = not read. */
struct BatteryReading {
    double pct = std::numeric_limits<double>::quiet_NaN();        ///< capacity (%)
    double chargeMah = std::numeric_limits<double>::quiet_NaN();  ///< charge left (mAh)
    double currentMa = std::numeric_limits<double>::quiet_NaN();  ///< driver sign: + into the battery, − out of it
    bool discharging = false;  ///< status "Discharging" (charging, full, plugged in or unknown: false)
    std::string status;        ///< the gauge's status as read ("Charging", "Full", "Not charging", ...)
    bool chargerOnline = false;  ///< the charger's online (tells "Not charging" plugged in from idle)
};

/** One controller's battery (ControllerBattery's fields, kept free of history.h). */
struct HandBattery {
    bool present = false;
    double pct = std::numeric_limits<double>::quiet_NaN();
    bool charging = false;
};

class BatteryWatch {
public:
    static constexpr double kReadSec = 15;         ///< how often the main loop reads (while the headset is worn)
    static constexpr double kAverageSec = 180;     ///< the discharge current's time constant
    static constexpr double kMinDataSec = 60;      ///< averaged this long before a time is given
    static constexpr double kGapSec = 120;         ///< no usable reading for longer than this: the average starts over
    static constexpr double kShowEverySec = 60;    ///< the time given changes at most this often
    static constexpr double kMinCurrentMa = 30;    ///< less drawn than this: skipped (idle, or the gauge settling)
    static constexpr double kMaxMinutes = 24 * 60;
    static constexpr double kLowPct = 20;
    static constexpr double kCriticalPct = 10;
    static constexpr double kControllerLowPct = 20;
    static constexpr double kRearmPct = 3;

    struct Notice {
        enum class Kind { HeadsetLow, HeadsetCritical, ControllerLow };
        Kind kind;
        double pct;           ///< the reading that set it off
        bool left = false;    ///< ControllerLow: which hand
        double minutes = std::numeric_limits<double>::quiet_NaN();  ///< the headset's time left (NaN: unknown)
    };

    /** A headset reading at now (monotonic seconds). */
    void feed(const BatteryReading& reading, double now);
    /** Both controllers, read at the same pace. */
    void feedControllers(const HandBattery& left, const HandBattery& right);

    /** @return the time left in minutes, rounded (roundMinutes); NaN when unknown */
    double minutesLeft() const { return shown_; }
    const BatteryReading& headset() const { return last_; }
    const HandBattery& left() const { return left_; }
    const HandBattery& right() const { return right_; }
    /** @return the notices due since the last call (oldest first) */
    std::vector<Notice> takeNotices();
    /** A notice taken but never shown (the headset came off): due again from the next reading that still says so. */
    void rearm(const Notice& notice);

private:
    BatteryReading last_;
    HandBattery left_, right_;
    double usableAt_ = -1;  ///< the last reading that went into the average
    double averageMa_ = 0;
    double averageSince_ = -1;  ///< when the average started (−1: not averaging)
    double shown_ = std::numeric_limits<double>::quiet_NaN();
    double shownAt_ = -1;
    bool lowDone_ = false, criticalDone_ = false;
    bool leftDone_ = false, rightDone_ = false;
    std::vector<Notice> notices_;

    void controller(const HandBattery& hand, bool left, bool& done);
    static bool onCharger(const BatteryReading& reading);
};

/**
 * Rounds a time left for showing: whole 5 minutes from 15 minutes up, whole minutes below (at least 1).
 * @return the minutes (NaN stays NaN)
 */
double roundMinutes(double minutes);

/** @return in uiLanguage(): "약 40분", "약 1시간 10분", "약 2시간" / "~40 min" / "約40分" (NaN: "") */
std::string minutesText(double minutes);
