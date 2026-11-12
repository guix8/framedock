// The browser's video controls under the HUD: while the browser panel is closed and its page has a video (or audio)
// that plays or is paused (see BrowserSession::Media), the main loop draws this strip under the HUD's own picture, in the same overlay,
// so it shows at the HUD's angle (the wrist fade, the Dock's laser on the HUD) and goes with it. Its title line
// (the page's title without YouTube's "(3) " and " - YouTube", 재생 중 / 멈춤 / 벗어서 멈춤, or the position for a moment after a 10 s jump) and five buttons:
// −10초, 멈추기 / 재생, +10초, 음소거 (filled pink while muted, like the app's other on-buttons) and 브라우저 (the
// browser panel). Only the buttons act: a press elsewhere on the strip does nothing (not the
// HUD's click, so a laser slightly off a button never opens the Dock), and a release up to kReleaseSlop px off the
// pressed button still counts.
//
// Sizes are fixed px: the HUD's px are the same size in the room whatever the layout (width_m per kColumnWidth px).
// Under Simple B's narrow tile the strip is kMinButtonsWidth wide, so its buttons keep a size a laser hits.
#pragma once

#include <cstdint>
#include <string>

class FontSet;
struct Pen;
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

enum class MediaAction { None, Back, PlayPause, Forward, Mute, OpenPanel };

/** What the strip shows. */
struct MediaStripState {
    std::string title;
    bool playing = false;
    bool muted = false;
    bool pausedByRemoval = false;  ///< paused when the headset came off (벗어서 멈춤)
    std::string position;          ///< "1:23 / 4:56" in place of the state (after a 10 s jump; "": the state)
    bool operator==(const MediaStripState& o) const {
        return title == o.title && playing == o.playing && muted == o.muted && pausedByRemoval == o.pausedByRemoval &&
               position == o.position;
    }
    bool operator!=(const MediaStripState& o) const { return !(*this == o); }
};

/** @return "1:23 / 4:56", "1:02:03 / 2:00:00", "1:23" (no length known: a live stream); "" when position < 0 */
std::string mediaPositionText(double position, double duration);

class MediaStrip {
public:
    static constexpr int kHeight = 132;   ///< a 6 px gap on top, then the strip
    static constexpr int kButtons = 5;
    static constexpr int kMinWidth = 200;  ///< narrower HUDs (none today) get no strip
    /// a HUD narrower than this (Simple B, 250 px) gets a strip this wide, the HUD centred over it: ~2 cm buttons
    static constexpr int kMinButtonsWidth = 340;

    explicit MediaStrip(const FontSet& fonts);
    ~MediaStrip();
    MediaStrip(const MediaStrip&) = delete;
    MediaStrip& operator=(const MediaStrip&) = delete;

    /**
     * Draws the strip width px wide (the HUD texture's width, at least kMinButtonsWidth).
     * @param highlighted the Dock's laser is on the HUD: the accent border, like the HUD's
     */
    void render(int width, const MediaStripState& state, bool highlighted);
    /** @return cairo's premultiplied ARGB32, width() × kHeight × 4 (rows of width() × 4 bytes) */
    const uint8_t* pixels() const;
    int width() const { return width_; }
    bool writePng(const std::string& path) const;

    /** @return the button at a point (strip px, y from the strip's top), as last drawn */
    MediaAction hit(double x, double y) const;
    /** @return true if the hover changed (redraw) */
    bool pointerMove(double x, double y);
    /** A press: on a button it is held until pointerUp (true); elsewhere false (nothing happens) */
    bool pointerDown(double x, double y);
    /** The release: the held button's action if released on it or just off it, not on another (else None). */
    MediaAction pointerUp(double x, double y);
    /** The laser left: hover and a held press let go. @return true if anything did (redraw) */
    bool pointerLeave();
    bool pressing() const { return pressed_ != MediaAction::None; }

private:
    const FontSet& fonts_;
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
    int width_ = 0;
    MediaAction hover_ = MediaAction::None;
    MediaAction pressed_ = MediaAction::None;

    /** The button i's area (0..kButtons−1) for the last width */
    void buttonRect(int i, double& x, double& y, double& w, double& h) const;
    /** @param onAccent drawn on the pink fill (dark) instead of a plain button (light) */
    void drawIcon(const Pen& pen, MediaAction action, double cx, double cy, double s, const MediaStripState& state,
                  bool onAccent) const;
};
