// UI strings for the widgets, in Japanese, English and Korean. Drawing code never contains UI text; it looks it up here.
// The words the fixed panel had (headset/i18n.cpp before the widget grid) keep their Japanese.
#pragma once

#include "reason.h"

#include <string>

namespace frame_perf_overlay {

/** The widgets' language (the headset maps its Language setting onto this). */
enum class WidgetLanguage { Ja, En, Ko };

struct WidgetText {
    WidgetLanguage language;
    const char* naPrefix;  ///< before a reason: "N/A: "
    // Titles
    const char* frame;
    const char* cpu;
    const char* gpuRam;
    const char* power;
    const char* battery;
    const char* temp;
    const char* network;
    const char* general;
    // Frame
    const char* waiting;    ///< connected to SteamVR, no frames yet
    const char* noSteamVr;  ///< not connected to SteamVR
    const char* reproj;     ///< followed by %
    const char* dropped;    ///< followed by a count
    // CPU
    const char* maxCoreOpen;   ///< before the busiest core's % (" (max core ")
    const char* maxCoreClose;  ///< after it (")")
    // GPU & RAM
    const char* gpuLoad;  ///< after the big %: frame GPU ms × app fps, not the whole GPU (core/gpu_load.h)
    const char* clock;
    const char* ram;
    // Power
    const char* rails;  ///< sum of measured rails, followed by W
    // Battery
    const char* batteryNote;  ///< the gauge % vs Steam's number
    const char* flowIn;       ///< W into the battery
    const char* flowOut;      ///< W out of it
    const char* statusCharging;
    const char* statusDischarging;
    const char* statusFull;
    const char* statusPlugged;
    const char* statusIdle;
    // Temp
    const char* cpuTempUnit;  ///< after the big CPU temperature ("°C CPU")
    const char* celsius;
    const char* fan;
    const char* thermalLimit;  ///< badge, followed by CPU / GPU / CPU+GPU
    const char* cpuAndGpu;     ///< both throttled
    const char* tDdr;
    const char* tBattery;
    const char* tDisplay;
    const char* tExhaust;
    const char* tHeatsink;
    // Network
    const char* linkDongle;  ///< the Steam Link dongle has a peer
    const char* linkHome;    ///< home Wi-Fi (also when neither is connected)
    const char* notConnected;
    const char* link;  ///< PHY rate, followed by tx/rx
    // General
    const char* controllers;
    const char* left;
    const char* right;
    const char* time;
    // The full (non-compact) rows and the process lists (upstream PR #5)
    const char* cpuItems;        ///< title of widget 7
    const char* ramItems;        ///< title of widget 8
    const char* msMaxSep;        ///< Frame times: between the average and the worst ("  최대 ")
    const char* capped;          ///< CPU cluster under a Playtime cap, followed by the hardware max GHz
    const char* pctOfCore;       ///< CPU items' column heading
    const char* rss;             ///< RAM items' column heading
    const char* uptime;          ///< General rows
    const char* app;
    const char* noApp;           ///< no scene app (SteamVR Home)
    const char* hudCost;         ///< this app's own cost
    const char* hideRulesError;  ///< CPU / RAM items with Hide system: hide_rules.json didn't load
    // Simple templates (Frame Dock): titles, labels, the throttle state pill, battery full
    const char* simpleA;
    const char* simpleB;
    const char* simpleReproj;  ///< "재투영" (no trailing space)
    const char* simpleRam;
    const char* simpleOk;      ///< pill: SteamVR isn't throttling the app
    const char* simpleHalf;    ///< pill: throttled to half rate
    const char* simpleFull;    ///< battery full ("충전 완료")
    // The controls (Frame Dock): titles, buttons, the line beside the title
    const char* ctlColor;          ///< 색 보정
    const char* ctlCompare;        ///< hold to see the original
    const char* ctlComparing;      ///< while held
    const char* ctlSound;          ///< 소리
    const char* ctlMute;
    const char* ctlPassthrough;
    const char* ctlOff;
    const char* ctlMono;
    const char* ctlColorView;      ///< passthrough in colour
    const char* ctlProfiles;
    const char* ctlInUse;          ///< before the profile's name
    const char* ctlPressAgain;     ///< a switch that loses unsaved changes waits for a second tap
    const char* ctlBrowser;
    const char* ctlOpen;           ///< open the browser panel
    const char* ctlClose;
    const char* ctlChromeOff;
    const char* ctlChromeOn;
    const char* ctlNeedsDock;      ///< in-game interaction is off: the buttons can't be pressed
    const char* ctlUnknown;        ///< not read yet
    const char* ctlOn;             ///< passthrough on, without Camera Switch (no mono / colour to pick)
    const char* ctlRecord;         ///< 녹화 (framecorder's recorder)
    const char* ctlRecordStart;
    const char* ctlRecordStop;
    const char* ctlRecordSaving;   ///< stopped, the recorder still finishing the file
    const char* ctlRecordFailed;   ///< the recorder ended by itself
    const char* ctlRecordBusy;     ///< another framecorder recorder runs (its own tab's): not started
    // Frame's render size (2026-11-13)
    const char* renderPc;  ///< after the size of a streamed game (read from the PC helper)
};

/**
 * @param language the language
 * @return its strings (static)
 */
const WidgetText& widgetText(WidgetLanguage language);

/** A recording's elapsed time: "● 0:42", "● 12:05", "● 1:02:03" (negative counts as 0). */
std::string recordElapsedText(int seconds);

/**
 * Why a reading is missing, in the strings' language (English is reasonText).
 * @param t the strings
 * @param reason the code
 * @return static text
 */
const char* reasonLabel(const WidgetText& t, Reason reason);

/**
 * The simple templates' state pill: "정상" (not throttled), "절반 제한" (half rate), "1/3 제한" and so on.
 * @param t the strings
 * @param throttled throttle steps (0 = none)
 */
std::string simpleThrottleText(const WidgetText& t, int throttled);

/**
 * The battery's time left: "약 40분" / "~40 min" / "約40分", hours from 60 minutes ("약 1시간 10분").
 * @param minutes already rounded (NaN or ≤ 0: "")
 */
std::string remainText(const WidgetText& t, double minutes);
/** remainText in fewer characters for a narrow widget: from an hour up, hours to the half ("약 2.5시간", "~2.5 h"),
 *  whole hours from 10 h ("약 16시간") */
std::string remainTextShort(const WidgetText& t, double minutes);

/**
 * Time since boot, to the minute: "42분" / "42m" / "42分", then hours ("3시간 5분"), then days ("2일 3시간").
 * @param seconds NaN or < 0: "--"
 */
std::string uptimeText(const WidgetText& t, double seconds);

/**
 * The throttle badge: SteamVR is holding the app to 1/(throttled + 1) of the refresh rate.
 * @param t the strings
 * @param hz refresh rate
 * @param throttled throttle steps (1 or more)
 */
std::string throttleBadgeText(const WidgetText& t, double hz, int throttled);

}  // namespace frame_perf_overlay
