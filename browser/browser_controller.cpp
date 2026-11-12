// The browser panel's input. See browser_controller.h.
#include "browser_controller.h"

#include "app_text.h"

#include <algorithm>
#include <cstdint>
#include <cmath>

BrowserController::BrowserController(BrowserPanel& panel, VrKeyboard& keyboard, BrowserInput& input)
    : panel_(panel), keyboard_(keyboard), input_(input) {}

void BrowserController::setStatus(const BrowserSession::Status& status) {
    if (status.sameAs(status_)) return;
    if (status.phase != BrowserSession::Phase::Ready && status_.phase == BrowserSession::Phase::Ready) {
        panel_.clearPage();  // the old picture belongs to a Chrome that is gone
        press_ = PagePress::None;
        ring_ = false;
        menuOpen_ = false;
    }
    // The menu belongs to the page it was opened on; its link arrives a moment after it opened
    if (menuOpen_ && status.url != status_.url) menuOpen_ = false;
    if (menuOpen_ && status.contextSeq == menuSeq_) menuLink_ = status.contextLink;
    status_ = status;
    barDirty_ = true;
}

bool BrowserController::savable() const {
    return status_.phase == BrowserSession::Phase::Ready &&
           (status_.url.rfind("https://", 0) == 0 || status_.url.rfind("http://", 0) == 0);
}

void BrowserController::closePopups() {
    if (!favoritesOpen_ && !menuOpen_ && !viewOpen_) return;
    favoritesOpen_ = menuOpen_ = viewOpen_ = false;
    favoritesNote_.clear();
    barDirty_ = true;
}

void BrowserController::openMenu(double x, double y, double cssX, double cssY) {
    closePopups();
    menuOpen_ = true;
    menuX_ = x;
    menuY_ = y;
    menuLink_.clear();
    input_.contextMenu(cssX, cssY, ++menuSeq_);
    barDirty_ = true;
}

void BrowserController::noteChange(FavoriteStore::Change change) {
    switch (change) {
        case FavoriteStore::Change::Done:
        case FavoriteStore::Change::None: favoritesNote_.clear(); break;
        case FavoriteStore::Change::Full:
            favoritesOpen_ = true;  // why nothing happened: the list, with the note under it
            favoritesNote_ = tr(Tx::BrowserFavoritesFull);
            break;
        case FavoriteStore::Change::Unsaved:
            favoritesNote_ = tr(Tx::BrowserFavoritesUnsaved);
            if (log) log("can't save the favorites: " + favorites_->error());
            break;
    }
    barDirty_ = true;
}

void BrowserController::toggleStar() {
    if (favorites_ == nullptr || !savable()) return;
    const int at = favorites_->find(status_.url);
    if (at >= 0) {
        noteChange(favorites_->remove(static_cast<size_t>(at)));
        return;
    }
    std::string title = status_.title;
    if (title.empty()) {
        title = status_.url.substr(status_.url.find("//") + 2);
    }
    noteChange(favorites_->add(title, status_.url));
}

double BrowserController::nextWake() const {
    double at = keyboardOpen_ ? keyboard_.nextWake() : std::numeric_limits<double>::infinity();
    if (press_ == PagePress::Pending && !ring_) at = std::min(at, downAt_ + kLongPressSec);
    if (quitArmedUntil_ >= 0) at = std::min(at, quitArmedUntil_ + 0.01);
    return at;
}

BrowserBarState BrowserController::barState(bool pinned, double widthM, bool canShrink, bool canGrow) const {
    BrowserBarState s;
    s.status = status_;
    s.keyboardOpen = keyboardOpen_;
    s.addressEditing = addressEditing_;
    s.addressText = addressText_;
    s.addressCaret = addressCaret_;
    s.translated = status_.translated;
    s.viewOpen = viewOpen_;
    s.mobile = mobile_;
    s.zoomPct = zoomPct_;
    s.aspect = aspect_;
    s.addressPreedit = addressEditing_ ? keyboard_.preedit() : std::string();
    s.pinned = pinned;
    s.dragScroll = dragScroll_;
    s.quitArmed = quitArmedUntil_ >= 0;
    s.widthM = widthM;
    s.canShrink = canShrink;
    s.canGrow = canGrow;
    if (favorites_ != nullptr) {
        s.favorites = favorites_->items();
        s.starred = favorites_->find(status_.url) >= 0;
    }
    s.canStar = favorites_ != nullptr && savable();
    s.favoritesOpen = favoritesOpen_;
    s.favoritesNote = favoritesNote_;
    if (menuOpen_) {
        s.menu = {MenuItem::Copy, MenuItem::Paste, MenuItem::SelectAll};
        if (!menuLink_.empty()) s.menu.push_back(MenuItem::OpenLink);  // at the end: the rows above don't move
        s.menuX = menuX_;
        s.menuY = menuY_;
    }
    if (ring_) {
        s.pressRing = true;
        s.ringX = downX_;
        s.ringY = downY_;
    }
    return s;
}

namespace {

/**
 * @return true if text is well-formed UTF-8 (no stray, cut-off or overlong bytes, no surrogates, nothing past U+10FFFF)
 *         with no character that would hide what the address is: the bidi overrides and isolates (U+202A–U+202E,
 *         U+2066–U+2069)
 */
bool readableUtf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const size_t len = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : (c & 0xF0) == 0xE0 ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        if (len == 0 || i + len > text.size()) return false;
        uint32_t cp = len == 1 ? c : len == 2 ? c & 0x1F : len == 3 ? c & 0x0F : c & 0x07;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char b = static_cast<unsigned char>(text[i + k]);
            if ((b & 0xC0) != 0x80) return false;
            cp = cp << 6 | (b & 0x3F);
        }
        const uint32_t least = len == 2 ? 0x80 : len == 3 ? 0x800 : len == 4 ? 0x10000 : 0;
        if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        if ((cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069)) return false;
        i += len;
    }
    return true;
}

/**
 * @return url with its percent-escaped non-ASCII characters (a Korean search) shown as they are, as Chrome's own
 *         address bar does; ASCII escapes (%20, %2F) stay, and so does anything that wouldn't make valid UTF-8
 */
std::string readableUrl(const std::string& url) {
    const auto hex = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    std::string out;
    for (size_t i = 0; i < url.size();) {
        // A run of %XX bytes at 0x80 and up, decoded together and kept only if it is valid UTF-8
        std::string bytes;
        size_t j = i;
        while (j + 2 < url.size() && url[j] == '%' && hex(url[j + 1]) >= 0 && hex(url[j + 2]) >= 0 &&
               (hex(url[j + 1]) << 4 | hex(url[j + 2])) >= 0x80) {
            bytes += static_cast<char>(hex(url[j + 1]) << 4 | hex(url[j + 2]));
            j += 3;
        }
        if (!bytes.empty() && readableUtf8(bytes)) {
            out += bytes;
            i = j;
        } else if (!bytes.empty()) {
            out += url.substr(i, j - i);
            i = j;
        } else {
            out += url[i++];
        }
    }
    return out;
}

/** @return where the UTF-8 character before pos starts (0 at the start) */
size_t prevCharStart(const std::string& text, size_t pos) {
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) --pos;
    return pos;
}

/** @return where the UTF-8 character at pos ends (text.size() at the end) */
size_t nextCharEnd(const std::string& text, size_t pos) {
    if (pos >= text.size()) return text.size();
    ++pos;
    while (pos < text.size() && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) ++pos;
    return pos;
}

}  // namespace

void BrowserController::syncField() {
    keyboard_.setField(addressText_, addressEditing_, addressCaret_);
    keyboard_.setTitle(tr(addressEditing_ ? Tx::BrowserKeyboardAddress : Tx::BrowserKeyboardPage));
}

void BrowserController::setKeyboardOpen(bool open, bool forAddress) {
    if (open == keyboardOpen_ && (!open || forAddress == addressEditing_)) return;
    if (keyboardOpen_) {
        // Whatever is still composing goes where it was being typed
        keyboard_.commit();
        std::vector<Request> ignored;
        handleOutputs(ignored);
        keyboard_.pointerLeave();
    }
    keyboardOpen_ = open;
    addressEditing_ = open && forAddress;
    if (addressEditing_) {
        // The page's address to edit (the caret at its end; ← → move it, ✕ or 지우기 clear it)
        addressText_ = status_.url == "about:blank" ? std::string() : readableUrl(status_.url);
        addressCaret_ = addressText_.size();
    }
    keyboard_.reset();
    syncField();
    keyboard_.render();
    barDirty_ = keyboardDirty_ = true;
}

void BrowserController::handleOutputs(std::vector<Request>& requests) {
    using Kind = KeyboardOutput::Kind;
    for (const KeyboardOutput& o : keyboard_.takeOutput()) {
        if (addressEditing_) {
            addressCaret_ = std::min(addressCaret_, addressText_.size());
            switch (o.kind) {
                case Kind::Text:
                    addressText_.insert(addressCaret_, o.text);
                    addressCaret_ += o.text.size();
                    break;
                case Kind::Backspace: {
                    const size_t from = prevCharStart(addressText_, addressCaret_);
                    addressText_.erase(from, addressCaret_ - from);
                    addressCaret_ = from;
                    break;
                }
                case Kind::Clear:
                    addressText_.clear();
                    addressCaret_ = 0;
                    break;
                case Kind::Left: addressCaret_ = prevCharStart(addressText_, addressCaret_); break;
                case Kind::Right: addressCaret_ = nextCharEnd(addressText_, addressCaret_); break;
                case Kind::Enter: {
                    const std::string text = addressText_;
                    addressEditing_ = false;
                    keyboardOpen_ = false;
                    addressText_.clear();
                    keyboard_.reset();
                    if (!text.empty()) input_.navigate(text);
                    requests.push_back(Request::Keyboard);
                    break;
                }
                case Kind::Close:
                    addressEditing_ = false;
                    keyboardOpen_ = false;
                    addressText_.clear();
                    requests.push_back(Request::Keyboard);
                    break;
            }
            barDirty_ = true;
            continue;
        }
        switch (o.kind) {
            case Kind::Text: input_.insertText(o.text); break;
            case Kind::Backspace: input_.key(cdp::Key::Backspace); break;
            case Kind::Enter: input_.key(cdp::Key::Enter); break;
            case Kind::Left: input_.key(cdp::Key::Left); break;
            case Kind::Right: input_.key(cdp::Key::Right); break;
            case Kind::Clear:
                input_.key(cdp::Key::SelectAll);
                input_.key(cdp::Key::Backspace);
                break;
            case Kind::Close:
                keyboardOpen_ = false;
                requests.push_back(Request::Keyboard);
                barDirty_ = true;
                break;
        }
    }
    syncField();
}

void BrowserController::setViewport(int width, int height) {
    if (width == panel_.viewportWidth() && height == panel_.viewportHeight()) return;
    releasePage();
    closePopups();
    panel_.setViewport(width, height);
    barDirty_ = true;
}

double BrowserController::dragStartPx() const {
    if (panelWidthM_ <= 0 || panel_.width() <= 0) return kMinDragStartPx;
    return std::max(kMinDragStartPx, kDragStartM / panelWidthM_ * panel_.width());
}

void BrowserController::releasePage() {
    if (ring_) barDirty_ = true;
    ring_ = false;
    switch (press_) {
        case PagePress::Held: input_.mouse(BrowserSession::Mouse::Up, pageX_, pageY_); break;
        case PagePress::Pending:   // the laser left before the release: no click
        case PagePress::Scrolling:
        case PagePress::None: break;
    }
    press_ = PagePress::None;
    if (barPressed_ || panel_.setPointer(-1, -1, false)) barDirty_ = true;
    barPressed_ = false;
}

std::vector<BrowserController::Request> BrowserController::panelPointer(const PanelPointer& p, double now) {
    std::vector<Request> requests;
    const cdp::ViewMap map = panel_.viewMap();
    double cssX = 0, cssY = 0;
    const bool onPage = map.toPage(p.x, p.y, cssX, cssY);
    using Hit = BrowserPanel::Hit;
    const auto onPopup = [](Hit h) {
        return h == Hit::FavoriteOpen || h == Hit::FavoriteRemove || h == Hit::MenuRow || h == Hit::Popup ||
               h == Hit::ViewMobile || h == Hit::ZoomOut || h == Hit::ZoomIn || h == Hit::ZoomReset || h == Hit::Aspect;
    };
    switch (p.type) {
        case PanelPointer::Type::Leave: releasePage(); break;
        case PanelPointer::Type::Scroll: {
            // The thumbstick (when SteamVR sends it to an overlay): a wheel at the laser's spot
            if (std::fabs(p.scrollDx) + std::fabs(p.scrollDy) > 0) {
                input_.wheel(pageX_, pageY_, p.scrollDx * kWheelPxPerUnit, -p.scrollDy * kWheelPxPerUnit);
            }
            break;
        }
        case PanelPointer::Type::SecondaryDown: {
            // The laser mouse's right button: the page's right click and our menu, at once
            const Hit hit = panel_.hitAt(p.x, p.y);
            if (hit != Hit::Page || press_ != PagePress::None) break;
            openMenu(p.x, p.y, cssX, cssY);
            break;
        }
        case PanelPointer::Type::SecondaryUp: break;
        case PanelPointer::Type::Move: {
            const bool overPopup = onPopup(panel_.hitAt(p.x, p.y));
            if (barPressed_ || !onPage || overPopup) {
                if (panel_.setPointer(p.x, p.y, barPressed_)) barDirty_ = true;
            } else if (panel_.setPointer(-1, -1, false)) {
                barDirty_ = true;
            }
            if (press_ == PagePress::Pending && std::hypot(p.x - downX_, p.y - downY_) >= dragStartPx()) {
                if (now - downAt_ >= kLongPressSec) {
                    // Held, then dragged: a mouse press where it went down, so the drag selects (text, a slider)
                    press_ = PagePress::Held;
                    if (ring_) barDirty_ = true;
                    ring_ = false;
                    input_.mouse(BrowserSession::Mouse::Move, pageX_, pageY_);
                    input_.mouse(BrowserSession::Mouse::Down, pageX_, pageY_);
                } else {
                    press_ = PagePress::Scrolling;
                }
            }
            if (press_ == PagePress::Scrolling) {
                // The page follows the laser: moving it up scrolls down
                const double sx = map.viewportW / map.areaW, sy = map.viewportH / map.areaH;
                input_.wheel(pageX_, pageY_, (lastX_ - p.x) * sx, (lastY_ - p.y) * sy);
                lastX_ = p.x;
                lastY_ = p.y;
                break;
            }
            if (!onPage || press_ == PagePress::Pending) break;  // a tap clicks where it was pressed
            if (overPopup && press_ == PagePress::None) break;  // hovering the list or the menu, not the page
            lastX_ = p.x;
            lastY_ = p.y;
            pageX_ = cssX;
            pageY_ = cssY;
            input_.mouse(BrowserSession::Mouse::Move, cssX, cssY);
            break;
        }
        case PanelPointer::Type::Down: {
            int arg = 0;
            const Hit hit = panel_.hitAt(p.x, p.y, &arg);
            // Any other press lets an armed 끄기 go
            const bool quitWasArmed = quitArmedUntil_ >= 0 && now <= quitArmedUntil_;
            if (hit != Hit::Quit && quitArmedUntil_ >= 0) {
                quitArmedUntil_ = -1;
                barDirty_ = true;
            }
            if (onPopup(hit)) {
                barPressed_ = true;
                if (panel_.setPointer(p.x, p.y, true)) barDirty_ = true;
                if (hit == Hit::FavoriteOpen && favorites_ != nullptr &&
                    static_cast<size_t>(arg) < favorites_->items().size()) {
                    const std::string url = favorites_->items()[static_cast<size_t>(arg)].url;
                    closePopups();
                    if (addressEditing_) {
                        setKeyboardOpen(false, false);
                        requests.push_back(Request::Keyboard);
                    }
                    input_.navigate(url);
                } else if (hit == Hit::FavoriteRemove && favorites_ != nullptr) {
                    noteChange(favorites_->remove(static_cast<size_t>(arg)));
                } else if (hit == Hit::ViewMobile) {
                    requests.push_back(arg == 1 ? Request::MobileOn : Request::MobileOff);  // the list stays open
                } else if (hit == Hit::ZoomOut || hit == Hit::ZoomIn || hit == Hit::ZoomReset) {
                    requests.push_back(hit == Hit::ZoomOut  ? Request::ZoomOut
                                       : hit == Hit::ZoomIn ? Request::ZoomIn
                                                            : Request::ZoomReset);
                } else if (hit == Hit::Aspect) {
                    const Request aspects[4] = {Request::AspectWide, Request::AspectStandard, Request::AspectSquare,
                                                Request::AspectTall};
                    if (arg >= 0 && arg < 4 && arg != aspect_) {
                        closePopups();  // Chrome starts again at the new shape
                        requests.push_back(aspects[arg]);
                    }
                } else if (hit == Hit::MenuRow) {
                    switch (static_cast<MenuItem>(arg)) {
                        case MenuItem::Copy: input_.key(cdp::Key::Copy); break;
                        case MenuItem::Paste: input_.key(cdp::Key::Paste); break;
                        case MenuItem::SelectAll: input_.key(cdp::Key::SelectAll); break;
                        case MenuItem::OpenLink:
                            if (!menuLink_.empty()) input_.navigate(menuLink_);
                            break;
                    }
                    closePopups();
                }
                break;
            }
            // An open list or menu: a press elsewhere closes it (on the page, that is all it does)
            const bool listWasOpen = favoritesOpen_;
            const bool viewWasOpen = viewOpen_;
            if (favoritesOpen_ || menuOpen_ || viewOpen_) {
                closePopups();
                if (hit == Hit::Page) break;
            }
            if (hit == Hit::Page) {
                lastX_ = downX_ = p.x;
                lastY_ = downY_ = p.y;
                pageX_ = cssX;
                pageY_ = cssY;
                if (dragScroll_) {
                    press_ = PagePress::Pending;  // a click on the release, a scroll if it moves, long: see the top
                    downAt_ = now;
                    ring_ = false;
                } else {
                    press_ = PagePress::Held;
                    input_.mouse(BrowserSession::Mouse::Move, cssX, cssY);
                    input_.mouse(BrowserSession::Mouse::Down, cssX, cssY);
                }
                break;
            }
            barPressed_ = hit != Hit::None;
            if (panel_.setPointer(p.x, p.y, barPressed_)) barDirty_ = true;
            switch (hit) {
                case Hit::Handle: requests.push_back(Request::Grab); break;
                case Hit::Back: input_.back(); break;
                case Hit::Forward: input_.forward(); break;
                case Hit::Reload: input_.reload(); break;
                case Hit::Star: toggleStar(); break;
                case Hit::Address:
                    if (addressEditing_) {
                        // Already editing: the caret goes where the field was pressed (what is composing goes in first)
                        size_t offset = panel_.addressOffsetAt(p.x);
                        if (offset == std::string::npos) break;
                        const size_t caretBefore = addressCaret_;
                        keyboard_.commit();
                        handleOutputs(requests);
                        if (offset >= caretBefore) offset += addressCaret_ - caretBefore;  // at or after the syllable
                        // Measured on the bar as last drawn: the text may have changed since, so a character's start
                        offset = std::min(offset, addressText_.size());
                        while (offset > 0 && offset < addressText_.size() &&
                               (static_cast<unsigned char>(addressText_[offset]) & 0xC0) == 0x80) {
                            --offset;
                        }
                        addressCaret_ = offset;
                        syncField();
                        barDirty_ = keyboardDirty_ = true;
                        break;
                    }
                    setKeyboardOpen(true, true);
                    requests.push_back(Request::Keyboard);
                    break;
                case Hit::AddressClear:
                    keyboard_.reset();  // the composing syllable goes too
                    addressText_.clear();
                    addressCaret_ = 0;
                    syncField();
                    keyboardDirty_ = true;
                    break;
                case Hit::Translate:
                    if (addressEditing_) {
                        setKeyboardOpen(false, false);
                        requests.push_back(Request::Keyboard);
                    }
                    requests.push_back(Request::Translate);
                    break;
                case Hit::View:
                    viewOpen_ = !viewWasOpen;
                    barDirty_ = true;
                    break;
                case Hit::Favorites:
                    favoritesOpen_ = !listWasOpen;
                    barDirty_ = true;
                    break;
                case Hit::Keyboard:
                    // Open: typing into the page. Open for the address: switch to the page. Open for the page: close
                    setKeyboardOpen(!(keyboardOpen_ && !addressEditing_), false);
                    requests.push_back(Request::Keyboard);
                    break;
                case Hit::SizeDown: requests.push_back(Request::SizeDown); break;
                case Hit::SizeUp: requests.push_back(Request::SizeUp); break;
                case Hit::Pin: requests.push_back(Request::TogglePin); break;
                case Hit::DragScroll:
                    dragScroll_ = !dragScroll_;
                    barDirty_ = true;
                    break;
                case Hit::Quit:
                    if (quitWasArmed) {
                        quitArmedUntil_ = -1;
                        closePopups();
                        if (keyboardOpen_) {
                            setKeyboardOpen(false, false);
                            requests.push_back(Request::Keyboard);
                        }
                        requests.push_back(Request::QuitChrome);
                    } else {
                        quitArmedUntil_ = now + kQuitConfirmSec;  // the first tap: asks for a second
                    }
                    barDirty_ = true;
                    break;
                case Hit::Close:
                    closePopups();
                    if (keyboardOpen_) {
                        setKeyboardOpen(false, false);
                        requests.push_back(Request::Keyboard);
                    }
                    requests.push_back(Request::Close);
                    break;
                case Hit::DialogAccept: input_.answerDialog(true); break;
                case Hit::DialogDismiss: input_.answerDialog(false); break;
                case Hit::FavoriteOpen:
                case Hit::FavoriteRemove:
                case Hit::MenuRow:
                case Hit::ViewMobile:
                case Hit::ZoomOut:
                case Hit::ZoomIn:
                case Hit::ZoomReset:
                case Hit::Aspect:
                case Hit::Popup:
                case Hit::Page:
                case Hit::None: break;
            }
            break;
        }
        case PanelPointer::Type::Up: {
            if (barPressed_) {
                barPressed_ = false;
                if (panel_.setPointer(p.x, p.y, false)) barDirty_ = true;
            }
            switch (press_) {
                case PagePress::Pending:
                    if (now - downAt_ >= kLongPressSec) {
                        // Held without moving: the right click and the panel's menu where it was pressed
                        ring_ = false;
                        press_ = PagePress::None;
                        openMenu(downX_, downY_, pageX_, pageY_);
                        break;
                    }
                    // A tap: the click at the press's spot (where the laser was aimed when the trigger went down)
                    input_.mouse(BrowserSession::Mouse::Move, pageX_, pageY_);
                    input_.mouse(BrowserSession::Mouse::Down, pageX_, pageY_);
                    input_.mouse(BrowserSession::Mouse::Up, pageX_, pageY_);
                    break;
                case PagePress::Held:
                    if (onPage) {
                        pageX_ = cssX;
                        pageY_ = cssY;
                    }
                    input_.mouse(BrowserSession::Mouse::Up, pageX_, pageY_);
                    break;
                case PagePress::Scrolling:
                case PagePress::None: break;
            }
            press_ = PagePress::None;
            break;
        }
    }
    return requests;
}

std::vector<BrowserController::Request> BrowserController::keyboardPointer(const PanelPointer& p, double now) {
    std::vector<Request> requests;
    if (!keyboardOpen_) return requests;
    switch (p.type) {
        case PanelPointer::Type::Move:
            if (keyboard_.pointerMove(p.x, p.y, now)) keyboardDirty_ = true;
            break;
        case PanelPointer::Type::Down: {
            const VrKeyboard::Press press = keyboard_.pointerDown(p.x, p.y, now);
            if (press == VrKeyboard::Press::Handle) requests.push_back(Request::Grab);
            keyboardDirty_ = true;
            handleOutputs(requests);
            break;
        }
        case PanelPointer::Type::Up:
            if (keyboard_.pointerUp()) keyboardDirty_ = true;
            break;
        case PanelPointer::Type::Leave:
            if (keyboard_.pointerLeave()) keyboardDirty_ = true;
            break;
        case PanelPointer::Type::Scroll:
        case PanelPointer::Type::SecondaryDown:
        case PanelPointer::Type::SecondaryUp: break;
    }
    if (addressEditing_) barDirty_ = true;  // the composing syllable shows in the address field
    return requests;
}

std::vector<BrowserController::Request> BrowserController::tick(double now) {
    std::vector<Request> requests;
    if (press_ == PagePress::Pending && !ring_ && now - downAt_ >= kLongPressSec) {
        ring_ = true;  // long enough: let go for the menu, drag to select
        barDirty_ = true;
    }
    if (quitArmedUntil_ >= 0 && now > quitArmedUntil_) {
        quitArmedUntil_ = -1;
        barDirty_ = true;
    }
    if (keyboardOpen_ && keyboard_.tick(now)) {
        keyboardDirty_ = true;
        handleOutputs(requests);
        if (addressEditing_) barDirty_ = true;
    }
    return requests;
}
