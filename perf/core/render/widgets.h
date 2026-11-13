// The widgets, drawn in the fixed panel's look. Each widget reads only the metrics it owns
// (core/metric_registry.h) from the MetricStore.
#pragma once

#include "draw.h"
#include "layout.h"
#include "metric_store.h"
#include "series.h"
#include "thresholds.h"
#include "widget_text.h"

#include <string>
#include <vector>

namespace frame_perf_overlay {

/** Width of one grid column (px): the fixed panel's width. */
constexpr int kColumnWidth = 512;

/** One process-list row (a process, or every process of one name). */
struct ProcessItem {
    std::string name;         ///< what to show: comm, or the exe basename when comm is cut off (processDisplayName)
    int pid = 0;              ///< 0 for a summed row
    int count = 1;            ///< processes in this row
    double cpuCorePct = 0.0;  ///< % of one core
    double rssMiB = 0.0;
};

/** Both process lists, as of the last scan (the rows aren't metrics). */
struct ProcessListsView {
    std::vector<ProcessItem> byCpu;
    std::vector<ProcessItem> byRam;
    bool cpuValid = false;        ///< byCpu has a real interval behind it
    bool hideRulesError = false;  ///< hide_rules.json's last load failed: the cards with Hide system say so
    uint64_t generation = 0;      ///< bumped on every scan (the list widgets redraw only when it moves)
};

/** Profiles the Profiles control shows at most (the first ones, as the dashboard's quick switch). */
constexpr int kControlProfiles = 4;

/**
 * What the controls (isControlWidget) show: Frame Dock fills it in from its models every redraw. The controls read
 * nothing themselves.
 */
struct ControlsView {
    bool interactive = false;   ///< the Dock's laser can press them (in-game interaction on)
    // 색 보정
    bool colorAvailable = false;
    std::string presetNames[3];  ///< 표준, 비비드, 야간 in the app's language
    int presetInUse = -1;        ///< 0-2; -1 = none of them (own values, a P slot)
    bool comparing = false;      ///< the original image is shown (held)
    bool compareUsable = false;  ///< the colour layer runs
    // 소리
    bool soundKnown = false;
    std::string outputName;
    int volume = 0;              ///< 0-100
    bool muted = false;
    // 패스스루
    int passthrough = -1;        ///< 0 off, 1 mono, 2 colour; -1 not known
    bool monoColorUsable = false;  ///< Camera Switch runs: 끔 | 흑백 | 컬러; else 끔 | 켬 (the room view as it is)
    // 프로필
    std::vector<std::string> profileNames;  ///< the first kControlProfiles
    std::vector<std::string> profileIds;    ///< theirs, in the same order (a press acts on the one it showed)
    int activeProfile = -1;
    int armedProfile = -1;       ///< tapped once: switching would lose unsaved changes, a second tap does it
    // 브라우저
    bool chromeRunning = false;
    bool browserOpen = false;
    // 녹화 (framecorder's recorder; the card is left off the HUD while framecorder isn't installed)
    int recordState = 0;    ///< 0 idle, 1 recording, 2 saving (stopped, finishing), 3 ended by itself, 4 another
                            ///< framecorder recorder runs (not started), 5 clip saved, 6 framecorder's tab said no
    int recordSeconds = 0;  ///< while recording: since it started
    bool recordClips = false;      ///< framecorder's clips are on: 녹화 | 클립 (else 녹화 alone)
    bool recordClipUsable = false;  ///< 클립 can be pressed now
    // The button held down now (drawn pressed)
    WidgetType pressedType = WidgetType::None;
    int pressedButton = -1;
};

/** One button of a control, in the same px as the card's top-left given to controlButtons. */
struct ControlButton {
    int id = 0;  ///< ColorPresets: 0-2 the presets, 3 compare. Volume: 0 mute, 1 −, 2 +. Passthrough: 0 off, 1 mono
                 ///< (켬 without Camera Switch), 2 colour. Profiles: the profile's index. Browser: 0. Record: 0
                 ///< 녹화 (start / stop), 1 클립 (framecorder's clips on)
    double x = 0, y = 0, w = 0, h = 0;
    bool usable = true;  ///< pressable (else drawn dim and the press does nothing)
};

/**
 * The buttons of a control widget, in its card at (x, y): what is drawn and what a press hits are the same.
 * @param view what it shows (nullptr: nothing pressable)
 * @return none for any other widget
 */
std::vector<ControlButton> controlButtons(const WidgetInstance& w, const ControlsView* view, double x, double y);

/** What every widget gets to draw with. */
struct WidgetContext {
    const MetricStore* store = nullptr;
    const ProcessListsView* processes = nullptr;  ///< nullptr = no lists (drawn as N/A)
    const ControlsView* controls = nullptr;       ///< nullptr = the controls draw as unavailable (previews)
    Thresholds thresholds;
    double now = 0.0;        ///< monotonic seconds (graph right edge)
    int graphSeconds = 30;   ///< graph window
    int clockFormat = 24;    ///< 12 or 24; 0 = the dashboard's clock "Off": General hides its Time row
    WidgetLanguage language = WidgetLanguage::En;
};

/** Graph history one widget instance keeps between redraws. */
struct WidgetHistory {
    History a;  ///< main series (fps, vph W, CPU °C, GPU load %)
    History b;  ///< second series (GPU °C)
    History c;  ///< Frame: worst CPU frame time per read (ms), for its graphs with Compact off
    History d;  ///< Frame: worst GPU frame time per read (ms)
};

/** A throughput as text, value and unit apart so they can be styled differently. */
struct RateText {
    std::string value;  ///< "24", "4.6", "123", "--"
    std::string unit;   ///< " kbps" / " Mbps" (leading space)
};

/**
 * Network ↓/↑ text: below 1 Mbps whole kbps ("24 kbps", "0 kbps"), else Mbps with one decimal ("1.0", "4.6"),
 * whole from 100 Mbps. NaN (N/A) gives "--". Anything that rounds to 1000 kbps already reads "1.0 Mbps".
 * @param mbps megabits per second
 */
RateText formatRate(double mbps);

/** Width of the Simple B tile's column (px): a small fps tile, about half a column. */
constexpr int kSimpleBWidth = 250;

/**
 * Natural width of the column a widget needs (px): kColumnWidth, or less for a small one (Simple B). A column is as
 * wide as its widest widget, so a layout of small widgets makes a small panel.
 * @param w the instance
 */
int widgetWidth(const WidgetInstance& w);

/**
 * Natural height of a widget's card (px). Network and General follow their visible rows when a context is
 * supplied, and so does the non-compact CPU card (its clusters). A Compact instance is shorter, a non-compact one
 * taller. CPU / RAM items follow their row count.
 * @param w the instance
 * @return height
 */
int widgetHeight(const WidgetInstance& w, const WidgetContext* ctx = nullptr);

/**
 * The card's title (what the editor labels it with).
 * @param w the instance
 * @param language the language
 * @return the title
 */
const char* widgetTitle(const WidgetInstance& w, WidgetLanguage language);

/**
 * Draws the parts that only change with the layout or the theme: the card, the graph beds, the title.
 * @param pen drawing tools
 * @param x card left
 * @param y card top
 * @param w the instance
 * @param language the title's language
 * @param heightOverride the card's height (0 = widgetHeight)
 */
void drawWidgetStatic(const Pen& pen, double x, double y, const WidgetInstance& w, WidgetLanguage language,
                      int heightOverride = 0);

/**
 * What a widget instance's picture depends on: when each metric it owns was last set, the process scan, and the
 * settings it draws with. Equal signatures = identical pixels, so the grid skips that widget.
 * @param w the instance
 * @param ctx values
 * @return a hash
 */
uint64_t widgetSignature(const WidgetInstance& w, const WidgetContext& ctx);

/**
 * Appends this redraw's samples to the instance's graphs. Call once per redraw, before drawWidgetDynamic.
 * @param w the instance
 * @param ctx values
 * @param history its graphs
 */
void pushWidgetHistory(const WidgetInstance& w, const WidgetContext& ctx, WidgetHistory& history);

/**
 * Draws the values on top of the static layer.
 * @param pen drawing tools
 * @param x card left
 * @param y card top
 * @param w the instance
 * @param ctx values
 * @param history its graphs
 */
void drawWidgetDynamic(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx,
                       const WidgetHistory& history);

}  // namespace frame_perf_overlay
