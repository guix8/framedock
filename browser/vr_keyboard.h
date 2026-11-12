// The VR keyboard: a panel of keys drawn on the CPU (cairo), typed with the laser, under the browser panel (or the
// Dock panel for a profile's name). Layouts: 한글 두벌식 (composed into syllables by hangul::Composer, the syllable in
// the making shown in a preview line), English QWERTY with Shift (a double tap locks it), numbers and symbols; 한/영,
// space, backspace (one jamo at a time while composing), enter, ← →, 지우기 (clear the field) and a close key.
// Character keys, space, backspace and the arrows repeat while held (HoldRepeat, as the dashboard's buttons).
//
// The keyboard only produces outputs (text to insert, keys); its owner sends them on (the browser's insertText and key
// events, or a profile name being edited). The idea of a keyboard as rows of key units with a grab bar on top, drawn
// on the CPU, follows Frametop's keyboard (screens/keyboard.cpp, MIT); see THIRD_PARTY_LICENSES.md.
#pragma once

#include "hangul.h"
#include "hold_repeat.h"

#include <cstdint>
#include <string>
#include <vector>

class FontSet;
struct Pen;
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

/** Something the keyboard asks its owner to do. */
struct KeyboardOutput {
    enum class Kind {
        Text,       ///< insert text (finished syllables, letters, a space)
        Backspace,  ///< delete the character before the caret (nothing was being composed)
        Enter,
        Left,
        Right,
        Clear,      ///< 지우기: empty the field
        Close,      ///< hide the keyboard
    };
    Kind kind;
    std::string text;
};

class VrKeyboard {
public:
    static constexpr int kWidth = 1280;
    static constexpr int kHeight = 600;
    static constexpr int kBarHeight = 64;  ///< the handle bar on top (holding it carries the panel it hangs under)

    enum class Layout { Hangul, English, Symbols };

    /** What a press on the keyboard was. */
    enum class Press { None, Key, Handle };

    /** One key or spot as last drawn. */
    struct Key {
        std::string id;     ///< "ㄱ", "a", "1", "space", "backspace", "enter", "shift", "lang", "symbols", "left",
                            ///< "right", "clear", "close", "handle"
        std::string label;  ///< what it shows (Shift applied)
        double x, y, w, h;
    };

    explicit VrKeyboard(const FontSet& fonts);
    ~VrKeyboard();
    VrKeyboard(const VrKeyboard&) = delete;
    VrKeyboard& operator=(const VrKeyboard&) = delete;

    /** Draws the keyboard (and lays out its keys). */
    void render();

    /** @return true if the hover changed (redraw) */
    bool pointerMove(double x, double y, double now);
    /** A press: a key acts at once (and starts repeating if it repeats); the handle asks for a grab. */
    Press pointerDown(double x, double y, double now);
    /** @return true if a press ended (redraw) */
    bool pointerUp();
    /** The laser left: the repeat stops, the hover clears. @return true (redraw) if anything changed */
    bool pointerLeave();
    /** A held key's repeat. @return true if it acted (outputs, a redraw) */
    bool tick(double now);
    /** @return the next repeat (+infinity when no key is held) */
    double nextWake() const { return repeat_.nextAt(); }

    /** @return the outputs since the last call, oldest first */
    std::vector<KeyboardOutput> takeOutput();

    /**
     * Finishes the syllable being composed (the owner's field loses the caret, the keyboard closes): it comes out as
     * a Text output.
     */
    void commit();
    /** Forgets the composition and Shift (a new field). */
    void reset();
    /**
     * The field the keys go into, shown in the preview line before the composition (a profile name being edited).
     * Without one (the browser's page) the preview line shows only the composition and a hint.
     * @param caret where typing goes (bytes into text; npos: its end)
     */
    void setField(const std::string& text, bool show, size_t caret = std::string::npos);
    /** A title on the handle bar (what is being typed into). */
    void setTitle(const std::string& title) { title_ = title; }

    void setLayout(Layout layout);
    Layout layout() const { return layout_; }
    bool shift() const { return shift_ || capsLock_; }
    bool capsLock() const { return capsLock_; }
    /** @return the syllable being composed (UTF-8) */
    std::string preedit() const { return composer_.preedit(); }

    /** @return the last picture: premultiplied ARGB32 (cairo's), kWidth × kHeight × 4 */
    const uint8_t* pixels() const;
    bool writePng(const std::string& path) const;
    /** @return the keys as last drawn */
    const std::vector<Key>& keys() const { return keys_; }
    /** @return the centre of a key as last drawn (tests). False if it isn't there */
    bool keyCenter(const std::string& id, double& x, double& y) const;
    /** @return the key at a point (tests), "" for none */
    std::string keyAt(double x, double y) const;

private:
    const FontSet& fonts_;
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
    std::vector<Key> keys_;
    Layout layout_ = Layout::Hangul;
    Layout lettersLayout_ = Layout::Hangul;  ///< where 한/영 and the symbols key go back to
    bool shift_ = false;
    bool capsLock_ = false;
    double shiftTappedAt_ = -1e9;
    hangul::Composer composer_;
    std::vector<KeyboardOutput> output_;
    std::string field_;
    size_t fieldCaret_ = 0;  ///< bytes into field_
    bool showField_ = false;
    std::string title_;  ///< "" until setTitle: tr(Tx::KeyboardTitle)
    double px_ = -1, py_ = -1;
    std::string pressedId_;
    HoldRepeat repeat_;

    const Key* keyAtPoint(double x, double y) const;
    /** Acts for one key press (or a repeat). */
    void act(const std::string& id, double now, bool repeat);
    void emit(KeyboardOutput::Kind kind, const std::string& text = {});
    void emitText(const std::string& text);
    static bool repeats(const std::string& id);
    /** @return the character a character key types now (Shift applied) */
    std::string typed(const std::string& id) const;
    void drawKey(const Pen& pen, const Key& key, bool special);
};
