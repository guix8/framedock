// The VR keyboard. See vr_keyboard.h.
#include "vr_keyboard.h"

#include "app_text.h"
#include "draw.h"
#include "theme.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr double kMargin = 24;    // around the keys
constexpr double kGap = 8;        // between keys
constexpr double kUnits = 12;     // every row is 12 key units wide
constexpr double kPreviewY = kMargin / 2 + VrKeyboard::kBarHeight;
constexpr double kPreviewH = 64;
constexpr double kRowsY = kPreviewY + kPreviewH + 14;
constexpr double kRowH = 80;
constexpr double kRadius = 14;
constexpr double kShiftLockSec = 0.4;  // two taps on Shift this close lock it

/** One key in a layout row: its id (a character key's id is its character), Shift's character, its width. */
struct Spec {
    const char* id;       ///< "" = an empty gap
    const char* shifted;  ///< nullptr = the same
    double units;
};

const std::vector<Spec> kNumbers = {{"1", nullptr, 1}, {"2", nullptr, 1}, {"3", nullptr, 1}, {"4", nullptr, 1},
                                    {"5", nullptr, 1}, {"6", nullptr, 1}, {"7", nullptr, 1}, {"8", nullptr, 1},
                                    {"9", nullptr, 1}, {"0", nullptr, 1}, {"backspace", nullptr, 2}};
const std::vector<Spec> kBottom = {{"lang", nullptr, 1.5}, {"symbols", nullptr, 1.5}, {"space", nullptr, 5},
                                   {"left", nullptr, 1},   {"right", nullptr, 1},    {"clear", nullptr, 2}};

std::vector<std::vector<Spec>> rowsFor(VrKeyboard::Layout layout) {
    switch (layout) {
        case VrKeyboard::Layout::Hangul:
            return {kNumbers,
                    {{"ㅂ", "ㅃ", 1}, {"ㅈ", "ㅉ", 1}, {"ㄷ", "ㄸ", 1}, {"ㄱ", "ㄲ", 1}, {"ㅅ", "ㅆ", 1}, {"ㅛ", nullptr, 1},
                     {"ㅕ", nullptr, 1}, {"ㅑ", nullptr, 1}, {"ㅐ", "ㅒ", 1}, {"ㅔ", "ㅖ", 1}, {"-", "_", 1}, {"/", "?", 1}},
                    {{"", nullptr, 0.5}, {"ㅁ", nullptr, 1}, {"ㄴ", nullptr, 1}, {"ㅇ", nullptr, 1}, {"ㄹ", nullptr, 1},
                     {"ㅎ", nullptr, 1}, {"ㅗ", nullptr, 1}, {"ㅓ", nullptr, 1}, {"ㅏ", nullptr, 1}, {"ㅣ", nullptr, 1},
                     {"enter", nullptr, 2.5}},
                    {{"shift", nullptr, 2}, {"ㅋ", nullptr, 1}, {"ㅌ", nullptr, 1}, {"ㅊ", nullptr, 1}, {"ㅍ", nullptr, 1},
                     {"ㅠ", nullptr, 1}, {"ㅜ", nullptr, 1}, {"ㅡ", nullptr, 1}, {",", "!", 1}, {".", "?", 1}, {"?", "!", 1}},
                    kBottom};
        case VrKeyboard::Layout::English:
            return {kNumbers,
                    {{"q", "Q", 1}, {"w", "W", 1}, {"e", "E", 1}, {"r", "R", 1}, {"t", "T", 1}, {"y", "Y", 1},
                     {"u", "U", 1}, {"i", "I", 1}, {"o", "O", 1}, {"p", "P", 1}, {"-", "_", 1}, {"/", "?", 1}},
                    {{"", nullptr, 0.5}, {"a", "A", 1}, {"s", "S", 1}, {"d", "D", 1}, {"f", "F", 1}, {"g", "G", 1},
                     {"h", "H", 1}, {"j", "J", 1}, {"k", "K", 1}, {"l", "L", 1}, {"enter", nullptr, 2.5}},
                    {{"shift", nullptr, 2}, {"z", "Z", 1}, {"x", "X", 1}, {"c", "C", 1}, {"v", "V", 1}, {"b", "B", 1},
                     {"n", "N", 1}, {"m", "M", 1}, {",", "!", 1}, {".", "?", 1}, {"@", ":", 1}},
                    kBottom};
        case VrKeyboard::Layout::Symbols:
            return {kNumbers,
                    {{"!", nullptr, 1}, {"@", nullptr, 1}, {"#", nullptr, 1}, {"$", nullptr, 1}, {"%", nullptr, 1},
                     {"^", nullptr, 1}, {"&", nullptr, 1}, {"*", nullptr, 1}, {"(", nullptr, 1}, {")", nullptr, 1},
                     {"-", nullptr, 1}, {"_", nullptr, 1}},
                    {{"", nullptr, 0.5}, {"=", nullptr, 1}, {"+", nullptr, 1}, {"[", nullptr, 1}, {"]", nullptr, 1},
                     {"{", nullptr, 1}, {"}", nullptr, 1}, {";", nullptr, 1}, {":", nullptr, 1}, {"'", nullptr, 1},
                     {"enter", nullptr, 2.5}},
                    {{"", nullptr, 0.5}, {"\"", nullptr, 1}, {"<", nullptr, 1}, {">", nullptr, 1}, {"\\", nullptr, 1},
                     {"|", nullptr, 1}, {"`", nullptr, 1}, {"~", nullptr, 1}, {",", nullptr, 1}, {".", nullptr, 1},
                     {"/", nullptr, 1}, {"?", nullptr, 1}},
                    kBottom};
    }
    return {};
}

bool isSpecial(const std::string& id) {
    static const char* specials[] = {"space", "backspace", "enter", "shift", "lang",   "symbols",
                                     "left",  "right",     "clear", "close", "handle"};
    for (const char* s : specials) {
        if (id == s) return true;
    }
    return false;
}

bool inside(double px, double py, double x, double y, double w, double h) {
    return px >= x && px <= x + w && py >= y && py <= y + h;
}

/** Backspace: a key cap pointing left with an × in it. */
void drawBackspaceIcon(cairo_t* cr, double cx, double cy, Color c) {
    const double w = 40, h = 26;
    cairo_new_path(cr);
    cairo_move_to(cr, cx - w / 2, cy);
    cairo_line_to(cr, cx - w / 2 + 12, cy - h / 2);
    cairo_line_to(cr, cx + w / 2, cy - h / 2);
    cairo_line_to(cr, cx + w / 2, cy + h / 2);
    cairo_line_to(cr, cx - w / 2 + 12, cy + h / 2);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_set_line_width(cr, 2.6);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
    const double x0 = cx + 2, s = 6;
    cairo_move_to(cr, x0 - s, cy - s);
    cairo_line_to(cr, x0 + s, cy + s);
    cairo_move_to(cr, x0 + s, cy - s);
    cairo_line_to(cr, x0 - s, cy + s);
    cairo_stroke(cr);
}

/** Shift: an arrow pointing up (filled when Shift is on). */
void drawShiftIcon(cairo_t* cr, double cx, double cy, Color c, bool filled, bool locked) {
    cairo_new_path(cr);
    cairo_move_to(cr, cx, cy - 16);
    cairo_line_to(cr, cx + 16, cy + 1);
    cairo_line_to(cr, cx + 7, cy + 1);
    cairo_line_to(cr, cx + 7, cy + 12);
    cairo_line_to(cr, cx - 7, cy + 12);
    cairo_line_to(cr, cx - 7, cy + 1);
    cairo_line_to(cr, cx - 16, cy + 1);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_set_line_width(cr, 2.6);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (filled) {
        cairo_fill_preserve(cr);
    }
    cairo_stroke(cr);
    if (locked) {
        cairo_rectangle(cr, cx - 9, cy + 17, 18, 3);
        cairo_fill(cr);
    }
}

/** Enter: a hooked arrow. */
void drawEnterIcon(cairo_t* cr, double cx, double cy, Color c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_set_line_width(cr, 3);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_new_path(cr);
    cairo_move_to(cr, cx + 18, cy - 14);
    cairo_line_to(cr, cx + 18, cy + 4);
    cairo_line_to(cr, cx - 16, cy + 4);
    cairo_stroke(cr);
    cairo_move_to(cr, cx - 20, cy + 4);
    cairo_line_to(cr, cx - 10, cy - 5);
    cairo_line_to(cr, cx - 10, cy + 13);
    cairo_close_path(cr);
    cairo_fill(cr);
}

/** ← / →: a filled triangle. */
void drawArrowIcon(cairo_t* cr, double cx, double cy, Color c, bool left) {
    const double d = left ? -1 : 1;
    cairo_new_path(cr);
    cairo_move_to(cr, cx + d * 11, cy);
    cairo_line_to(cr, cx - d * 8, cy - 12);
    cairo_line_to(cr, cx - d * 8, cy + 12);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_fill(cr);
}

void drawGripDots(cairo_t* cr, double cx, double cy, Color c) {
    for (int row = -1; row <= 1; ++row) {
        for (int col = 0; col < 2; ++col) drawDisc(cr, cx - 6 + col * 12, cy + row * 10, 3.2, c);
    }
}

}  // namespace

VrKeyboard::VrKeyboard(const FontSet& fonts) : fonts_(fonts) {
    // Hangul first for Korean, English for the others (한/영 switches either way)
    layout_ = lettersLayout_ = uiLanguage() == Language::Ko ? Layout::Hangul : Layout::English;
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kWidth, kHeight);
    cr_ = cairo_create(surface_);
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_set_font_options(cr_, options);
    cairo_font_options_destroy(options);
    render();
}

VrKeyboard::~VrKeyboard() {
    cairo_destroy(cr_);
    cairo_surface_destroy(surface_);
}

const uint8_t* VrKeyboard::pixels() const {
    cairo_surface_flush(surface_);
    return cairo_image_surface_get_data(surface_);
}

bool VrKeyboard::writePng(const std::string& path) const {
    return cairo_surface_write_to_png(surface_, path.c_str()) == CAIRO_STATUS_SUCCESS;
}

bool VrKeyboard::repeats(const std::string& id) {
    return !isSpecial(id) || id == "space" || id == "backspace" || id == "left" || id == "right";
}

std::string VrKeyboard::typed(const std::string& id) const {
    for (const Key& k : keys_) {
        if (k.id == id) return k.label;
    }
    return id;
}

const VrKeyboard::Key* VrKeyboard::keyAtPoint(double x, double y) const {
    for (const Key& k : keys_) {
        if (inside(x, y, k.x, k.y, k.w, k.h)) return &k;
    }
    return nullptr;
}

std::string VrKeyboard::keyAt(double x, double y) const {
    const Key* k = keyAtPoint(x, y);
    return k != nullptr ? k->id : std::string();
}

bool VrKeyboard::keyCenter(const std::string& id, double& x, double& y) const {
    for (const Key& k : keys_) {
        if (k.id != id) continue;
        x = k.x + k.w / 2;
        y = k.y + k.h / 2;
        return true;
    }
    return false;
}

void VrKeyboard::emit(KeyboardOutput::Kind kind, const std::string& text) { output_.push_back({kind, text}); }

void VrKeyboard::emitText(const std::string& text) {
    if (text.empty()) return;
    // Consecutive text joins into one insert
    if (!output_.empty() && output_.back().kind == KeyboardOutput::Kind::Text) {
        output_.back().text += text;
    } else {
        emit(KeyboardOutput::Kind::Text, text);
    }
    if (showField_) {
        field_.insert(fieldCaret_, text);
        fieldCaret_ += text.size();
    }
}

std::vector<KeyboardOutput> VrKeyboard::takeOutput() {
    std::vector<KeyboardOutput> out;
    out.swap(output_);
    return out;
}

void VrKeyboard::commit() { emitText(composer_.commit()); }

void VrKeyboard::reset() {
    composer_.reset();
    shift_ = capsLock_ = false;
    repeat_.stop();
    pressedId_.clear();
}

void VrKeyboard::setField(const std::string& text, bool show, size_t caret) {
    field_ = text;
    fieldCaret_ = std::min(caret, text.size());
    showField_ = show;
}

void VrKeyboard::setLayout(Layout layout) {
    if (layout == layout_) return;
    commit();
    layout_ = layout;
    if (layout != Layout::Symbols) lettersLayout_ = layout;
    render();
}

void VrKeyboard::act(const std::string& id, double now, bool repeat) {
    using Kind = KeyboardOutput::Kind;
    if (id == "backspace") {
        if (composer_.backspace()) return;  // a jamo back; nothing reaches the field
        emit(Kind::Backspace);
        if (showField_ && fieldCaret_ > 0) {
            const std::string before = hangul::dropLastChar(field_.substr(0, fieldCaret_));
            field_.erase(before.size(), fieldCaret_ - before.size());
            fieldCaret_ = before.size();
        }
        return;
    }
    if (id == "space") {
        emitText(composer_.input(U' '));
        return;
    }
    if (id == "enter" || id == "left" || id == "right" || id == "clear" || id == "close") {
        if (id == "clear") {
            composer_.reset();
            if (showField_) {
                field_.clear();
                fieldCaret_ = 0;
            }
            emit(Kind::Clear);
            return;
        }
        commit();
        emit(id == "enter" ? Kind::Enter : id == "left" ? Kind::Left : id == "right" ? Kind::Right : Kind::Close);
        return;
    }
    if (id == "shift") {
        if (capsLock_) {
            capsLock_ = shift_ = false;
        } else if (shift_ && now - shiftTappedAt_ <= kShiftLockSec) {
            capsLock_ = true;
        } else {
            shift_ = !shift_;
        }
        shiftTappedAt_ = now;
        render();
        return;
    }
    if (id == "lang") {
        setLayout(layout_ == Layout::Symbols ? lettersLayout_ : layout_ == Layout::Hangul ? Layout::English : Layout::Hangul);
        return;
    }
    if (id == "symbols") {
        setLayout(layout_ == Layout::Symbols ? lettersLayout_ : Layout::Symbols);
        return;
    }
    // A character key: what it shows is what it types (Shift applied)
    const std::string text = typed(id);
    const std::u32string chars = hangul::decodeUtf8(text);
    for (const char32_t c : chars) emitText(composer_.input(c));
    // A one-shot Shift is used up by the first character (a repeat keeps the character it started with)
    if (shift_ && !capsLock_ && !repeat) {
        shift_ = false;
        pressedId_ = id;
        render();
        // The held key repeats what it typed, not the unshifted character it shows now: keep its label
        for (Key& k : keys_) {
            if (k.id == id) k.label = text;
        }
    }
}

bool VrKeyboard::pointerMove(double x, double y, double now) {
    repeat_.touch(now);
    const std::string before = keyAt(px_, py_);
    px_ = x;
    py_ = y;
    const std::string after = keyAt(x, y);
    if (!pressedId_.empty() && after != pressedId_ && repeat_.active()) repeat_.stop();  // slid off the held key
    return before != after;
}

VrKeyboard::Press VrKeyboard::pointerDown(double x, double y, double now) {
    px_ = x;
    py_ = y;
    repeat_.stop();
    const Key* key = keyAtPoint(x, y);
    if (key == nullptr) {
        pressedId_.clear();
        return Press::None;
    }
    pressedId_ = key->id;
    if (key->id == "handle") return Press::Handle;
    const std::string id = key->id;
    act(id, now, false);
    if (repeats(id)) repeat_.start(now);
    render();
    return Press::Key;
}

bool VrKeyboard::pointerUp() {
    repeat_.stop();
    const bool had = !pressedId_.empty();
    pressedId_.clear();
    if (had) render();
    return had;
}

bool VrKeyboard::pointerLeave() {
    repeat_.stop();
    const bool had = !pressedId_.empty() || px_ >= 0;
    pressedId_.clear();
    px_ = py_ = -1;
    if (had) render();
    return had;
}

bool VrKeyboard::tick(double now) {
    if (!repeat_.due(now)) return false;
    if (pressedId_.empty()) {
        repeat_.stop();
        return false;
    }
    act(pressedId_, now, true);
    render();
    return true;
}

void VrKeyboard::drawKey(const Pen& pen, const Key& key, bool special) {
    const bool pressed = key.id == pressedId_;
    const bool over = inside(px_, py_, key.x, key.y, key.w, key.h);
    const bool shiftOn = key.id == "shift" && (shift_ || capsLock_);
    Color fill = special ? kCard : kControl;
    if (shiftOn) fill = kAccent;
    if (pressed) fill = shiftOn ? kAccentPressed : kControlHover;
    pen.color(fill);
    pen.roundedRect(key.x, key.y, key.w, key.h, kRadius);
    cairo_fill(pen.cr);
    strokeRounded(pen, key.x, key.y, key.w, key.h, kRadius, pressed || over ? kAccent : (special ? kBorder : kControlHover),
                  pressed || over ? 3 : 1.5);
    const Color fg = shiftOn ? kOnAccent : kText;
    const double cx = key.x + key.w / 2, cy = key.y + key.h / 2;
    if (key.id == "backspace") {
        drawBackspaceIcon(pen.cr, cx, cy, fg);
        return;
    }
    if (key.id == "shift") {
        drawShiftIcon(pen.cr, cx, cy, fg, shift_ || capsLock_, capsLock_);
        return;
    }
    if (key.id == "enter") {
        drawEnterIcon(pen.cr, cx, cy, fg);
        return;
    }
    if (key.id == "left" || key.id == "right") {
        drawArrowIcon(pen.cr, cx, cy, fg, key.id == "left");
        return;
    }
    if (key.id == "space") {
        const char* name = tr(layout_ == Layout::Hangul    ? Tx::KeyboardHangul
                              : layout_ == Layout::English ? Tx::KeyboardEnglish
                                                           : Tx::KeyboardSymbols);
        textCentered(pen, cx, centerBaseline(key.y, key.h, 22), name, 22, kTextMuted, false);
        return;
    }
    if (key.id == "lang") {
        // 한/영 (한/A outside Korean) with the current one bright
        const double size = 28;
        const char* latin = tr(Tx::KeyboardLatinKey);
        const double wHan = pen.measure("한", size, true), wSlash = pen.measure("/", size, true);
        const double wEng = pen.measure(latin, size, true);
        double x = cx - (wHan + wSlash + wEng) / 2;
        const double base = centerBaseline(key.y, key.h, size);
        x += pen.text(x, base, "한", size, layout_ == Layout::Hangul ? kAccentText : kTextMuted, true);
        x += pen.text(x, base, "/", size, kTextMuted, true);
        pen.text(x, base, latin, size, layout_ == Layout::English ? kAccentText : kTextMuted, true);
        return;
    }
    // The compatibility jamo are small glyphs in Noto Sans CJK: a larger size makes them as tall as the letters
    const std::u32string chars = hangul::decodeUtf8(key.label);
    const double size = special ? 24 : (!chars.empty() && hangul::isJamo(chars[0]) ? 42 : 32);
    const double fitted = fitSize(pen, key.label, size, 14, key.w - 12, special);
    textCentered(pen, cx, centerBaseline(key.y, key.h, fitted), key.label, fitted, fg, special);
}

void VrKeyboard::render() {
    const Pen pen {cr_, &fonts_};
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr_);
    cairo_restore(cr_);
    std::vector<Key> previous;
    previous.swap(keys_);

    // The panel
    pen.color(kBg);
    pen.roundedRect(0, 0, kWidth, kHeight, 24);
    cairo_fill(cr_);
    strokeRounded(pen, 1, 1, kWidth - 2, kHeight - 2, 24, kControlHover, 2);

    // The handle bar: grip dots and the title (holding it carries the panel the keyboard hangs under), close on the right
    const double closeW = 150;
    const Key handle {"handle", "", 0, 0, kWidth - closeW - kMargin, static_cast<double>(kBarHeight)};
    const bool handleActive = pressedId_ == "handle" || inside(px_, py_, handle.x, handle.y, handle.w, handle.h);
    pen.color(handleActive ? kControlHover : kCard);
    pen.roundedRect(0, 0, kWidth, kBarHeight, 24);
    cairo_fill(cr_);
    cairo_rectangle(cr_, 0, kBarHeight / 2.0, kWidth, kBarHeight / 2.0);
    cairo_fill(cr_);
    drawGripDots(cr_, kMargin + 10, kBarHeight / 2.0, handleActive ? kAccent : kTextMuted);
    const std::string title = title_.empty() ? std::string(tr(Tx::KeyboardTitle)) : title_;
    const double titleW = pen.text(kMargin + 36, centerBaseline(0, kBarHeight, 26), title, 26, kText, true);
    {
        const double left = kMargin + 36 + titleW + 20, room = kWidth - closeW - kMargin - 20 - left;
        const std::string hint = tr(Tx::KeyboardHandleHint);
        const double size = fitSize(pen, hint, 18, 14, room, false);
        if (room > 60) {
            pen.text(left, centerBaseline(0, kBarHeight, size), ellipsize(pen, hint, size, room, false), size,
                     kTextMuted);
        }
    }
    keys_.push_back(handle);
    const Key close {"close", tr(Tx::KeyboardClose), kWidth - closeW - 10, 8, closeW, kBarHeight - 16.0};
    keys_.push_back(close);
    drawKey(pen, close, true);

    // The preview line: the field (when there is one), the syllable being composed, the layout and Shift
    drawCard(pen, kMargin, kPreviewY, kWidth - 2 * kMargin, kPreviewH, 16, kCard, kCard, 0);
    const std::string preedit = composer_.preedit();
    const double base = centerBaseline(kPreviewY, kPreviewH, 30);
    double x = kMargin + 20;
    const double badgeW = 220;
    const double room = kWidth - 2 * kMargin - badgeW - 40;
    std::string rest;  // the field after the caret
    if (showField_) {
        const size_t caret = std::min(fieldCaret_, field_.size());
        // At most the last 240 bytes before the caret are measured (a long address fits nowhere near that many)
        std::string shown = field_.substr(caret > 240 ? caret - 240 : 0, std::min<size_t>(caret, 240));
        while (!shown.empty() && (static_cast<unsigned char>(shown[0]) & 0xC0) == 0x80) shown.erase(0, 1);
        rest = field_.substr(caret, 240);
        // Keep the caret in view: cut from the front while it is too wide
        while (!shown.empty() && pen.measure(shown + preedit, 30, false) > room - 20) {
            shown = hangul::decodeUtf8(shown).size() > 1 ? shown.substr(hangul::utf8(hangul::decodeUtf8(shown)[0]).size())
                                                          : std::string();
        }
        x += pen.text(x, base, shown, 30, kText);
    } else if (preedit.empty()) {
        const std::string hint = tr(layout_ == Layout::Hangul ? Tx::KeyboardHintHangul : Tx::KeyboardHintOther);
        const double size = fitSize(pen, hint, 20, 15, room - 20, false);
        pen.text(x, centerBaseline(kPreviewY, kPreviewH, size), ellipsize(pen, hint, size, room - 20, false), size,
                 kTextMuted);
    } else {
        x += pen.text(x, base, tr(Tx::KeyboardComposing), 20, kTextMuted);
    }
    if (!preedit.empty()) {
        const double w = pen.text(x, base, preedit, 30, kAccentText, true);
        pen.color(kAccent);
        cairo_rectangle(cr_, x, base + 6, w, 3);
        cairo_fill(cr_);
        x += w;
    }
    if (showField_) {  // the caret, then the rest of the field as far as it fits
        pen.color(kAccent);
        cairo_rectangle(cr_, x + 3, kPreviewY + 14, 3, kPreviewH - 28);
        cairo_fill(cr_);
        const double restRoom = kMargin + 20 + room - (x + 10);
        if (!rest.empty() && restRoom > 30) pen.text(x + 10, base, ellipsize(pen, rest, 30, restRoom, false), 30, kText);
    }
    std::string badge = tr(layout_ == Layout::Hangul    ? Tx::KeyboardHangul
                           : layout_ == Layout::English ? Tx::KeyboardEnglish
                                                        : Tx::KeyboardSymbolsBadge);
    if (capsLock_) {
        badge += " · CAPS";
    } else if (shift_) {
        badge += " · Shift";
    }
    pen.text(kWidth - kMargin - 20, centerBaseline(kPreviewY, kPreviewH, 22), badge, 22, kAccentText, true, true);

    // The keys
    const double unitW = (kWidth - 2 * kMargin + kGap) / kUnits;
    const std::vector<std::vector<Spec>> rows = rowsFor(layout_);
    const bool shifted = shift_ || capsLock_;
    for (size_t r = 0; r < rows.size(); ++r) {
        double units = 0;
        const double y = kRowsY + r * (kRowH + kGap);
        for (const Spec& spec : rows[r]) {
            const double kx = kMargin + units * unitW;
            const double kw = spec.units * unitW - kGap;
            units += spec.units;
            if (spec.id[0] == '\0') continue;
            Key key {spec.id, spec.id, kx, y, kw, kRowH};
            const std::string id = spec.id;
            if (shifted && spec.shifted != nullptr) key.label = spec.shifted;
            if (id == "symbols") key.label = layout_ == Layout::Symbols ? (lettersLayout_ == Layout::Hangul ? "가나다" : "ABC") : "!#1";
            if (id == "clear") key.label = tr(Tx::KeyboardClear);
            // A held key keeps typing what it typed (a one-shot Shift ran out after the first)
            for (const Key& old : previous) {
                if (old.id == id && id == pressedId_ && repeat_.active() && !isSpecial(id)) key.label = old.label;
            }
            keys_.push_back(key);
            drawKey(pen, key, isSpecial(id));
        }
    }
}
