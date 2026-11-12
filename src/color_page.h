// The Color page of the Frame Dock dashboard tab: FrameColor's dashboard redrawn in Perf's style for the
// landscape tab. Drawing and hit-testing only; ColorModel does the work.
#pragma once

#include "color_model.h"

#include <string>
#include <vector>

struct Pen;  // core/render/draw.h

namespace framecolor {

/** Where the page may draw (dashboard px). */
struct ColorPageArea {
    double headerLeft = 0, headerRight = 0;  ///< the free part of the header row (the compare buttons go here)
    double headerTop = 0, headerHeight = 0;
    double left = 0, top = 0, width = 0, height = 0;  ///< the cards, between the header row and the footer
};

/** What a spot on the page does. */
enum class ColorHit { None, Slider, Channel, Preset, SaveMode, Reset, Compare, AutoCompare, SharpenArea, AutoRule, GazeFit };

class ColorPage {
public:
    explicit ColorPage(ColorModel& model) : model_(model) {}
    ColorPage(const ColorPage&) = delete;
    ColorPage& operator=(const ColorPage&) = delete;

    ColorModel& model() { return model_; }
    const ColorModel& model() const { return model_; }

    /** Draws the page in the model's language and lays out its hit areas. */
    void render(const Pen& pen, const ColorPageArea& area);

    /** @return true if a redraw is needed (hover changed, or a slider moved) */
    bool pointerMove(double x, double y);
    /** A press on the page (not on the panel's own header / footer buttons). @return true (redraw) */
    bool pointerDown(double x, double y, Clock::time_point now = Clock::now());
    /** @return true if a press on the page ended (redraw) */
    bool pointerUp();
    /** The pointer left the tab, the tab was hidden or the page was switched away. @return true (redraw) */
    bool pointerLeave();
    /** A press on one of the panel's own buttons while this page is shown. */
    void dismiss() { model_.dismiss(); }

    /**
     * The bottom line of the tab on this page: the last message, or a hint when the Vulkan layer is off.
     * @return false if there is nothing to show
     */
    bool statusLine(std::string& text, bool& error) const;

    /** One spot that does something, as last drawn (dashboard px). */
    struct Hit {
        double x, y, w, h;
        ColorHit kind;
        int arg;  ///< slider index, preset index (0-5), channel, rule
    };

    /** What is at a point as last drawn. @param arg its argument (slider index, preset index, ...) */
    ColorHit hitAt(double x, double y, int* arg = nullptr) const;
    /** The centre of a hit area as last drawn (tests). @return false if it isn't there */
    bool hitCenter(ColorHit kind, int arg, double& x, double& y) const;
    /** Every hit area as last drawn (tests). */
    const std::vector<Hit>& hits() const { return hits_; }

private:

    ColorModel& model_;
    std::vector<Hit> hits_;
    double px_ = -1, py_ = -1;  ///< the pointer (-1 = not on the tab)
    bool pressed_ = false;      ///< a press began on the page and has not been released
    double trackX_ = 0, trackW_ = 1;  ///< slider track as last drawn

    const Hit* hitTest(double x, double y) const;
    double fractionAt(double x) const { return (x - trackX_) / trackW_; }

    void button(const Pen& pen, double x, double y, double w, double h, const std::string& label, ColorHit kind,
                int arg, bool selected, bool usable, double size = 22);
    void ruleButton(const Pen& pen, double x, double y, double w, double h, const std::string& name,
                    const std::string& value, int rule, bool selected, bool usable);
    void slider(const Pen& pen, double y, double h, double labelX, double labelW, double valueRight,
                const std::string& title, int index, bool usable, int tint);
};

}  // namespace framecolor
