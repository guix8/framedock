// A short notice in front of the eyes (the low battery): one line in a pill, yellow or red, shown for kShowSec by the
// main loop on an overlay of its own that hangs off the headset (pose(): low in the view, 1.5 m ahead), fading in and
// out over kFadeSec. Nothing to press: SteamVR's laser passes through it, and the game keeps running. GuideView (below)
// is the first-run guide on the same overlay.
#pragma once

#include "config.h"

#include <cstdint>
#include <string>

class FontSet;
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

class NoticeView {
public:
    static constexpr int kWidth = 960;
    static constexpr int kHeight = 112;
    static constexpr double kWidthM = 0.5;    ///< at 1.5 m: about 19° wide
    static constexpr double kShowSec = 3.0;
    static constexpr double kDangerShowSec = 4.0;  ///< the red one (10 %) a little longer
    static constexpr double kFadeSec = 0.2;        ///< in and out
    static constexpr double kBgAlpha = 0.6;        ///< the pill's background: the game shows through
    /**
     * Where it hangs off the headset: 1.5 m ahead, 0.60 m down (about 22° under the line of sight, below where a
     * game's action usually is), tilted up toward the eyes.
     */
    static HeadPose pose() { return HeadPose {0.0, -0.60, -1.5, 0.0, 22.0, 0.0}; }

    enum class Level { Warn, Danger };

    explicit NoticeView(const FontSet& fonts);
    ~NoticeView();
    NoticeView(const NoticeView&) = delete;
    NoticeView& operator=(const NoticeView&) = delete;

    /** Draws text (shrunk to fit, down to a minimum size) with the level's colour. */
    void render(const std::string& text, Level level);
    /** @return cairo's premultiplied ARGB32, kWidth × kHeight × 4 */
    const uint8_t* pixels() const;
    bool writePng(const std::string& path) const;

private:
    const FontSet& fonts_;
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
};

/**
 * The first-run guide: a larger card in front of the eyes on the notice's overlay (the battery notice goes first while
 * one is due), in the UI language: a title, four rows (손목 / 누르기 / 옮기기 / 설정) and a footer. The main loop shows
 * it for kShowSec of wear each start until the HUD is pressed once (Config::firstRunGuideDone). Nothing to press.
 */
class GuideView {
public:
    static constexpr int kWidth = 1100;
    static constexpr int kHeight = 400;
    static constexpr double kWidthM = 0.80;    ///< at 1.5 m: about 30° wide
    static constexpr double kShowSec = 60.0;   ///< of wear, each start
    static constexpr double kBgAlpha = 0.92;
    /** In front, a little under the line of sight (the notice's way of hanging off the headset, higher) */
    static HeadPose pose() { return HeadPose {0.0, -0.30, -1.5, 0.0, 11.0, 0.0}; }

    explicit GuideView(const FontSet& fonts);
    ~GuideView();
    GuideView(const GuideView&) = delete;
    GuideView& operator=(const GuideView&) = delete;

    /** Draws the guide in uiLanguage(). */
    void render();
    /** @return cairo's premultiplied ARGB32, kWidth × kHeight × 4 */
    const uint8_t* pixels() const;
    bool writePng(const std::string& path) const;

private:
    const FontSet& fonts_;
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
};
