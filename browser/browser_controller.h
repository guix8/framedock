// What the laser does on the browser panel and its keyboard: the bar's buttons, the page (a click, a drag, scrolling,
// a long press), the 즐겨찾기 list and the context menu, and where the keyboard's outputs go (the page, or the address
// field while it is being edited). No OpenVR and no Chrome here: the page side goes through BrowserInput (the worker
// in the app, a recorder in the tests), and what only the main loop can do (carrying the panel, its size, pinning it,
// closing it) comes back as requests.
//
// On the page with drag-to-scroll: a tap clicks, a drag scrolls. Held kLongPressSec without moving (a ring shows
// there), it becomes a mouse press instead: dragged on, it selects text; let go where it was, it is a right click
// with the panel's own menu (복사, 붙여넣기, 전체 선택, and 링크 열기 on a link). The controller's right button (the
// Frame's thumbstick click, SteamVR's laser mouse right click) opens the same menu at once.
#pragma once

#include "browser_session.h"
#include "browser_view.h"
#include "favorites.h"
#include "vr_keyboard.h"

#include <functional>
#include <limits>
#include <string>
#include <vector>

/** A laser event on one of the browser's overlays (texture px, y from the top). */
struct PanelPointer {
    enum class Type {
        Move, Down, Up, Leave, Scroll,
        SecondaryDown,  ///< the laser mouse's right button (the Frame's thumbstick click)
        SecondaryUp,
    };
    Type type = Type::Move;
    double x = 0, y = 0;
    double scrollDx = 0, scrollDy = 0;  ///< Scroll: SteamVR's smooth scroll deltas (positive y = up)
};

/** The page side: what the controller asks Chrome for. */
class BrowserInput {
public:
    virtual ~BrowserInput() = default;
    virtual void navigate(const std::string& text) = 0;
    virtual void back() = 0;
    virtual void forward() = 0;
    /** Reload, or start Chrome again when it is off or failed */
    virtual void reload() = 0;
    virtual void mouse(BrowserSession::Mouse kind, double x, double y) = 0;
    virtual void wheel(double x, double y, double deltaX, double deltaY) = 0;
    virtual void insertText(const std::string& text) = 0;
    virtual void key(cdp::Key key) = 0;
    /** The page's confirm / prompt / leave question: 확인 (true) or 취소 */
    virtual void answerDialog(bool accept) = 0;
    /** A right click at a page point; the link there comes back as Status::contextLink with contextSeq = seq */
    virtual void contextMenu(double x, double y, int seq) = 0;
};

class BrowserController {
public:
    /**
     * A press that moves this far on the panel (m, in the room) scrolls instead of clicking: the same hand wobble
     * whatever the panel's size or the page's resolution (dragStartPx() turns it into texture px)
     */
    static constexpr double kDragStartM = 0.008;
    static constexpr double kMinDragStartPx = 4;     ///< never less than this (a huge panel, a tiny texture)
    static constexpr double kWheelPxPerUnit = 120;   ///< SteamVR's scroll delta → CSS px (as the Layout page's lanes)
    /** A press on the page held this long without moving: a selection (dragged on) or the context menu (let go) */
    static constexpr double kLongPressSec = 0.5;
    /** 끄기 waits this long for its second tap (as the app's other two-tap buttons) */
    static constexpr double kQuitConfirmSec = 3.0;

    /** Something only the main loop can do. */
    enum class Request {
        Grab,         ///< carry the panel (the handle, or the keyboard's handle) while the trigger is held
        Close,        ///< hide the panel (Chrome keeps running)
        QuitChrome,   ///< 끄기 tapped twice: close Chrome (and the panel)
        SizeDown,
        SizeUp,
        TogglePin,    ///< fixed in the room ↔ on the wrist
        Keyboard,     ///< the keyboard opened or closed (keyboardOpen())
        Translate,    ///< 번역: on, or off again (BrowserSession::translate)
        // 보기's list: the main loop changes the setting (and tells setView)
        MobileOff,
        MobileOn,
        ZoomOut,
        ZoomIn,
        ZoomReset,
        AspectWide,     ///< 16:9
        AspectStandard, ///< 4:3
        AspectSquare,   ///< 1:1
        AspectTall,     ///< 9:16
    };

    BrowserController(BrowserPanel& panel, VrKeyboard& keyboard, BrowserInput& input);

    /** 보기's state as the settings are now (mobile, zoom %, 비율 0–3 as BrowserBarState::aspect) */
    void setView(bool mobile, int zoomPct, int aspect) {
        if (mobile == mobile_ && zoomPct == zoomPct_ && aspect == aspect_) return;
        mobile_ = mobile;
        zoomPct_ = zoomPct;
        aspect_ = aspect;
        barDirty_ = true;
    }
    bool viewOpen() const { return viewOpen_; }
    /** 즐겨찾기 (☆ and its list); nullptr: neither does anything */
    void setFavorites(FavoriteStore* store) { favorites_ = store; }
    bool favoritesOpen() const { return favoritesOpen_; }
    bool menuOpen() const { return menuOpen_; }
    /** Closes the 즐겨찾기 list and the context menu */
    void closePopups();
    /** Problems worth a log line (a favorite that couldn't be saved) */
    std::function<void(const std::string&)> log;

    /** A laser event on the panel. @return what the main loop must do */
    std::vector<Request> panelPointer(const PanelPointer& p, double now);
    /** A laser event on the keyboard. */
    std::vector<Request> keyboardPointer(const PanelPointer& p, double now);
    /** A held key's repeat (its outputs go where they belong); a long press's ring. */
    std::vector<Request> tick(double now);
    /** @return the next repeat of a held key, or when a press on the page becomes long (+infinity: none) */
    double nextWake() const;

    /** Opens or closes the keyboard. Closing finishes the composing syllable into its field first. */
    void setKeyboardOpen(bool open, bool forAddress);
    bool keyboardOpen() const { return keyboardOpen_; }
    bool addressEditing() const { return addressEditing_; }
    const std::string& addressText() const { return addressText_; }
    bool dragScroll() const { return dragScroll_; }
    void setDragScroll(bool on) { dragScroll_ = on; }
    /** The panel's width in the room (m): sets the drag distance in texture px */
    void setPanelWidthM(double widthM) { panelWidthM_ = widthM; }
    /** @return kDragStartM in texture px for the panel's width and its texture's width now */
    double dragStartPx() const;

    /**
     * A new page size (the Browser page's 720p / 1080p): the panel's texture changes, so the list, the menu and a
     * held press (all at the old texture px) go first.
     */
    void setViewport(int width, int height);
    /** The laser left the panel (or it was hidden): a press on the page is released, a drag ends. */
    void releasePage();
    /** Chrome's status as of now (the bar shows it; a page that went away is cleared). */
    void setStatus(const BrowserSession::Status& status);
    /** @return the bar's state as of now (for BrowserPanel::renderBar) */
    BrowserBarState barState(bool pinned, double widthM, bool canShrink, bool canGrow) const;

    /** @return true (and clears it) if the bar / the keyboard need drawing again */
    bool takeBarDirty() {
        const bool d = barDirty_;
        barDirty_ = false;
        return d;
    }
    bool takeKeyboardDirty() {
        const bool d = keyboardDirty_;
        keyboardDirty_ = false;
        return d;
    }

private:
    BrowserPanel& panel_;
    VrKeyboard& keyboard_;
    BrowserInput& input_;
    // 보기's list and what it shows
    bool viewOpen_ = false;
    bool mobile_ = false;
    int zoomPct_ = 100;
    int aspect_ = 0;
    size_t addressCaret_ = 0;  ///< the address field's caret (bytes into addressText_)
    BrowserSession::Status status_;
    bool keyboardOpen_ = false;
    bool addressEditing_ = false;
    std::string addressText_;
    bool dragScroll_ = true;
    double panelWidthM_ = 0.45;
    bool barDirty_ = true;
    bool keyboardDirty_ = true;
    // The page under the laser
    enum class PagePress { None, Pending, Scrolling, Held };
    PagePress press_ = PagePress::None;
    double downAt_ = 0;                ///< when the pending press went down
    bool ring_ = false;                ///< the pending press is long (its ring shows)
    double downX_ = 0, downY_ = 0;     ///< texture px of the press
    double lastX_ = 0, lastY_ = 0;     ///< texture px of the last event on the page
    double pageX_ = 0, pageY_ = 0;     ///< its page point
    bool barPressed_ = false;
    // 즐겨찾기
    FavoriteStore* favorites_ = nullptr;
    bool favoritesOpen_ = false;
    double quitArmedUntil_ = -1;  ///< 끄기 waits for its second tap until then (−1: not armed)
    std::string favoritesNote_;
    // The context menu: where (texture px), which request, the link it found
    bool menuOpen_ = false;
    double menuX_ = 0, menuY_ = 0;
    int menuSeq_ = 0;
    std::string menuLink_;

    void handleOutputs(std::vector<Request>& requests);
    void syncField();
    void openMenu(double x, double y, double cssX, double cssY);
    void toggleStar();
    void noteChange(FavoriteStore::Change change);
    /** @return true if the page showing can be saved (http, https) */
    bool savable() const;
};
