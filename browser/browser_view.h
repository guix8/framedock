// The browser panel's picture and hit areas: a two-row bar on top (the handle with the page's title, size − / +,
// 방에 고정 / 고정 풀기 (pin in the room), 끌기: 스크롤 | 선택, and apart at the end 끄기 · 닫기; then ◀ ▶ ⟳ ☆, the
// address field (an ✕ at its end while edited), 번역, 보기, 즐겨찾기, 키보드)
// over the page, which is Chrome's latest screencast frame drawn 1:1 (or scaled when its size differs). While Chrome
// isn't ready the page area says why (starting, restarting, failed). While the page waits for an answer to confirm(),
// prompt() or "leave this page?", the second row is that question with 확인 and 취소.
//
// Over the page, drawn again on each new frame until they go: the 즐겨찾기 list and 보기's list (버전, 확대, 비율;
// each under its button), the panel's own
// context menu (복사, 붙여넣기, 전체 선택, 링크 열기) and a ring where a long press is held.
//
// The bar is laid out for a 1280 px wide page and scaled with the page's width (1080p: × 1.5), so it keeps its size
// on the panel whatever the resolution. Hit areas are in texture px like the laser's events.
#pragma once

#include "browser_session.h"
#include "cdp.h"
#include "jpeg_frame.h"

#include <cstdint>
#include <string>
#include <vector>

class FontSet;
struct Pen;
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

struct Bookmark {
    std::string name;
    std::string url;
};

/** A row of the panel's own context menu (the page's right click, or a long press released where it was) */
enum class MenuItem { Copy, Paste, SelectAll, OpenLink };
/** @return its label in uiLanguage() */
const char* menuItemLabel(MenuItem item);

/** What the bar shows besides Chrome's status. */
struct BrowserBarState {
    BrowserSession::Status status;
    bool keyboardOpen = false;
    bool addressEditing = false;   ///< the keyboard types into the address field
    std::string addressText;       ///< what is typed there so far
    size_t addressCaret = 0;       ///< the caret in it (bytes; the composing syllable shows there)
    std::string addressPreedit;    ///< the syllable being composed
    bool pinned = false;           ///< fixed in the room (not following the wrist)
    bool dragScroll = true;        ///< dragging on the page scrolls it (a tap still clicks)
    bool quitArmed = false;        ///< 끄기 tapped once: waits for the second tap (한 번 더)
    bool translated = false;       ///< 번역 is on
    bool viewOpen = false;         ///< 보기's list hangs under its button
    bool mobile = false;           ///< 보기: the mobile version
    int zoomPct = 100;             ///< 보기: the page's zoom
    int aspect = 0;                ///< 보기: 16:9 | 4:3 | 1:1 | 9:16 (0–3)
    double widthM = 0.45;
    bool canShrink = true, canGrow = true;
    // 즐겨찾기 (the saved pages)
    std::vector<Bookmark> favorites;
    bool favoritesOpen = false;    ///< its list hangs under its button
    std::string favoritesNote;     ///< a line at the list's end ("" = none): full, couldn't save
    bool starred = false;          ///< the page showing is saved (★)
    bool canStar = false;          ///< the page showing can be saved (http, https)
    // The context menu (empty = closed) next to the point it was asked at (texture px)
    std::vector<MenuItem> menu;
    double menuX = 0, menuY = 0;
    // A press held long enough on the page (texture px): released there it opens the menu, dragged it selects
    bool pressRing = false;
    double ringX = 0, ringY = 0;
};

class BrowserPanel {
public:
    static constexpr int kDesignWidth = 1280; ///< the page width the bar is laid out for (scale 1)
    static constexpr int kBarHeight = 160;    ///< both rows of the bar at scale 1 (barHeight(): in texture px)
    static constexpr int kHandleHeight = 76;  ///< the first row (holding it carries the panel), at scale 1

    /** A spot on the bar (or the page) that does something, as last drawn. */
    enum class Hit {
        None, Handle, Back, Forward, Reload, Star, Address, Favorites, Keyboard, SizeDown, SizeUp, Pin,
        DragScroll, Close, Quit, Page, DialogAccept, DialogDismiss,
        Translate,       ///< 번역
        View,            ///< 보기: its list (버전, 확대, 비율)
        AddressClear,    ///< the ✕ at the address field's end while it is edited
        ViewMobile,      ///< 보기's 데스크톱 (arg 0) | 모바일 (1)
        ZoomOut, ZoomIn, ZoomReset,
        Aspect,          ///< 보기's 비율 (arg: BrowserBarState::aspect's index)
        FavoriteOpen,    ///< a row of the 즐겨찾기 list (arg: its index)
        FavoriteRemove,  ///< its ✕ (arg: its index)
        MenuRow,         ///< a context menu row (arg: its MenuItem)
        Popup,           ///< elsewhere on the list or the menu (nothing, but not the page under it)
    };

    BrowserPanel(const FontSet& fonts, int viewportWidth, int viewportHeight);
    ~BrowserPanel();
    BrowserPanel(const BrowserPanel&) = delete;
    BrowserPanel& operator=(const BrowserPanel&) = delete;

    /** A new page size (CSS px): the texture becomes viewport + bar (scaled with the width). */
    void setViewport(int width, int height);
    int viewportWidth() const { return viewportW_; }
    int viewportHeight() const { return viewportH_; }

    /** Draws the bar (and the page area's message when there is no page to show). */
    void renderBar(const BrowserBarState& state);
    /**
     * Puts a frame into the page area: copied 1:1 when it has the viewport's size; another size (Chrome changing the
     * page size, a resize on the way) is scaled to fit with its shape kept, at the top, black beside or under it.
     */
    void showFrame(const BgraFrame& frame);
    /** Forgets the page (Chrome went away): the page area shows the status message again. */
    void clearPage();
    bool hasPage() const { return hasPage_; }

    /** @return what is at a point as last drawn (arg: the bookmark's index) */
    Hit hitAt(double x, double y, int* arg = nullptr) const;
    /**
     * @return the byte offset in the address being edited nearest x (the same units as hitAt's), between characters,
     * as last drawn; npos when it isn't being edited. The composing syllable is not in the text: a point past it is an
     * offset after the caret.
     */
    size_t addressOffsetAt(double x) const;
    /** @return the centre of a hit area as last drawn (tests). False if it isn't there or can't be pressed */
    bool hitCenter(Hit hit, int arg, double& x, double& y) const;
    /** Pointer feedback on the bar. @return true if the hover / press changed (redraw the bar) */
    bool setPointer(double x, double y, bool pressed);
    /**
     * @return where the page shows and the page size it maps to (laser points → page points): the page area and the
     *         viewport, or where the last frame of another size was drawn (scaled to fit, never stretched) and its size
     */
    cdp::ViewMap viewMap() const;

    /** @return the picture: premultiplied ARGB32 (cairo's), width() × height() × 4 */
    const uint8_t* pixels() const;
    int width() const { return viewportW_; }
    int height() const { return barHeight() + viewportH_; }
    /** @return the bar's height in texture px (the page starts there) */
    int barHeight() const { return barH_; }
    /** @return texture px per layout unit of the bar (the page's width / kDesignWidth) */
    double scale() const { return scale_; }
    bool writePng(const std::string& path) const;

private:
    struct Spot {
        Hit hit;
        int arg;
        double x, y, w, h;
        bool usable;
    };
    const FontSet& fonts_;
    int viewportW_, viewportH_;
    // Where the last frame was drawn (texture px) and the page size it shows (CSS px): the page area and the viewport
    // unless that frame had another size
    double pageX_ = 0, pageY_ = 0, pageW_ = 0, pageH_ = 0;
    int pageCssW_ = 0, pageCssH_ = 0;
    double scale_ = 1;             ///< texture px per layout unit
    int barH_ = kBarHeight;        ///< the bar in texture px
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
    std::vector<Spot> spots_;      ///< in layout units (texture px / scale_)
    bool hasPage_ = false;
    double px_ = -1, py_ = -1;     ///< the pointer, layout units
    bool pressed_ = false;
    double favoritesRight_ = 0;    ///< the right edge of the 즐겨찾기 button as last drawn (its list hangs there)
    double viewRight_ = 0;         ///< the right edge of the 보기 button as last drawn (its list hangs there)
    // The address being edited as last drawn (addressOffsetAt): the text and its caret (bytes), where the part drawn
    // before the caret starts, and the x (layout units) where that part, the part after the caret and the room end
    bool editing_ = false;
    std::string editText_;
    size_t editCaret_ = 0, editShownFrom_ = 0;
    double editX_ = 0, editAfterX_ = 0, editRight_ = 0;
    BrowserBarState shown_;        ///< the last state drawn (the overlays are drawn again over each new frame)
    // The page under the overlays (texture px), put back before they are drawn again or when they go
    std::vector<uint8_t> under_;
    int underX_ = 0, underY_ = 0, underW_ = 0, underH_ = 0;
    // The overlays as last drawn (by renderBar), composited again over each new frame without measuring text again
    cairo_surface_t* overlay_ = nullptr;
    int overlayX_ = 0, overlayY_ = 0;

    void makeSurface();
    /** The page fills the page area at the viewport's size (no frame of another size shown) */
    void resetPageRect();
    void button(const Pen& pen, Hit hit, int arg, double x, double y, double w, double h, const std::string& label,
                bool selected, bool usable, double size = 24);
    bool over(const Spot& s) const;
    void drawPageMessage(const Pen& pen, const BrowserSession::Status& status);
    /** The list, the menu and the ring over the page, and their hit areas (pen: the overlay surface's) */
    void drawOverlays(const Pen& pen);
    /** Draws the overlays into overlay_ (none: it goes) */
    void renderOverlay();
    /** Paints overlay_ over the page */
    void compositeOverlay();
    /** @return the overlays' box in texture px (inside the page area); false if there are none */
    bool overlayRect(int& x, int& y, int& w, int& h) const;
    /** Where the 즐겨찾기 list and the context menu go (layout units) */
    void listBox(double& x, double& y, double& w, double& h) const;
    void menuBox(double& x, double& y, double& w, double& h) const;
    void viewBox(double& x, double& y, double& w, double& h) const;
    /** @return the overlays' bounding box (layout units); false if there are none */
    bool overlayBox(double& x, double& y, double& w, double& h) const;
    void saveUnder();
    void restoreUnder();
};

/** @return the waiting page dialog as one line for the bar, in uiLanguage() ("" when there is none) */
std::string browserDialogText(const BrowserSession::Status& status);
/** @return the status as a short line for the page area, in uiLanguage() ("" when the page shows) */
std::string browserStatusText(const BrowserSession::Status& status);
