#include "battery_watch.h"

#include "app_text.h"

#include <algorithm>

double roundMinutes(double minutes) {
    if (std::isnan(minutes)) return minutes;
    if (minutes >= 15) return std::round(minutes / 5) * 5;
    return std::max(1.0, std::round(minutes));
}

std::string minutesText(double minutes) {
    // The HUD's words for the time left (the battery widgets'), in the app's language
    return frame_perf_overlay::remainText(frame_perf_overlay::widgetText(widgetLanguage(uiLanguage())), minutes);
}

void BatteryWatch::feed(const BatteryReading& reading, double now) {
    // On the charger: no time. A reading that only can't be used (a "Not charging" blip off the charger, a status or
    // current that couldn't be read, a moment under kMinCurrentMa) is skipped: the average and the time shown stay,
    // unless no usable one came for kGapSec
    const bool charger = onCharger(reading);
    const double drawn = -reading.currentMa;  // out of the battery
    const bool usable = !charger && reading.discharging && drawn >= kMinCurrentMa && reading.chargeMah > 0;
    const bool gap = usableAt_ < 0 || now - usableAt_ > kGapSec || now < usableAt_;
    if (charger || (!usable && gap)) {
        averageSince_ = -1;
        shown_ = std::numeric_limits<double>::quiet_NaN();
    } else if (usable) {
        if (averageSince_ < 0 || gap) {
            averageMa_ = drawn;
            averageSince_ = now;
            shown_ = std::numeric_limits<double>::quiet_NaN();
        } else {
            averageMa_ += (1.0 - std::exp(-(now - usableAt_) / kAverageSec)) * (drawn - averageMa_);
        }
        usableAt_ = now;
        if (now - averageSince_ >= kMinDataSec && (std::isnan(shown_) || now - shownAt_ >= kShowEverySec)) {
            shown_ = roundMinutes(std::min(kMaxMinutes, reading.chargeMah / averageMa_ * 60.0));
            shownAt_ = now;
        }
    }
    last_ = reading;

    // The notices: only while discharging, each once until charging or kRearmPct above its threshold
    const double pct = reading.pct;
    if (std::isnan(pct)) return;
    // Re-armed only by the charger, not by a status that couldn't be read nor "Not charging" off the charger
    if (charger || pct >= kLowPct + kRearmPct) lowDone_ = false;
    if (charger || pct >= kCriticalPct + kRearmPct) criticalDone_ = false;
    if (!reading.discharging) return;
    if (pct <= kCriticalPct && !criticalDone_) {
        notices_.push_back({Notice::Kind::HeadsetCritical, pct, false, shown_});
        criticalDone_ = lowDone_ = true;
    } else if (pct <= kLowPct && !lowDone_) {
        notices_.push_back({Notice::Kind::HeadsetLow, pct, false, shown_});
        lowDone_ = true;
    }
}

bool BatteryWatch::onCharger(const BatteryReading& reading) {
    // Charging, full, or "Not charging" with the charger online (held at a level). The gauge says "Not charging" off
    // the charger for a moment now and then
    return reading.status == "Charging" || reading.status == "Full" ||
           (reading.status == "Not charging" && reading.chargerOnline);
}

void BatteryWatch::controller(const HandBattery& hand, bool left, bool& done) {
    // Off, or 0 % (what a controller may report just after it connects): nothing to tell, nothing re-arms
    if (!hand.present || !(hand.pct > 0)) return;
    if (hand.charging || hand.pct >= kControllerLowPct + kRearmPct) done = false;
    if (!hand.charging && hand.pct <= kControllerLowPct && !done) {
        notices_.push_back({Notice::Kind::ControllerLow, hand.pct, left});
        done = true;
    }
}

void BatteryWatch::feedControllers(const HandBattery& left, const HandBattery& right) {
    left_ = left;
    right_ = right;
    controller(left, true, leftDone_);
    controller(right, false, rightDone_);
}

void BatteryWatch::rearm(const Notice& notice) {
    switch (notice.kind) {
        case Notice::Kind::HeadsetLow: lowDone_ = false; break;
        case Notice::Kind::HeadsetCritical: criticalDone_ = false; break;
        case Notice::Kind::ControllerLow: (notice.left ? leftDone_ : rightDone_) = false; break;
    }
}

std::vector<BatteryWatch::Notice> BatteryWatch::takeNotices() {
    std::vector<Notice> out;
    out.swap(notices_);
    return out;
}
