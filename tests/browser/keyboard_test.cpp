// The VR keyboard (browser/vr_keyboard.*): typing Korean through the composer with the laser, English with Shift,
// symbols, backspace one jamo at a time, keys repeating while held, the hit areas, and its pictures
// ($FRAMEDOCK_PNG_DIR/keyboard-*.png when set).
#include "draw.h"
#include "vr_keyboard.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

int failures = 0;

#define CHECK(expr)                                             \
    do {                                                        \
        if (!(expr)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
            ++failures;                                         \
        }                                                       \
    } while (false)

using Kind = KeyboardOutput::Kind;

const char* kFont = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc";
const char* kBold = "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc";

/** Presses and releases a key by its id with the laser. */
bool tap(VrKeyboard& k, const std::string& id, double now = 0) {
    double x = 0, y = 0;
    if (!k.keyCenter(id, x, y)) {
        std::printf("  no key %s\n", id.c_str());
        return false;
    }
    k.pointerMove(x, y, now);
    const bool pressed = k.pointerDown(x, y, now) == VrKeyboard::Press::Key;
    k.pointerUp();
    return pressed;
}

void typeAll(VrKeyboard& k, const std::u32string& keys) {
    for (const char32_t c : keys) tap(k, hangul::utf8(c));
}

/** Everything inserted (Text outputs joined), with the other outputs as [name] */
std::string outputs(VrKeyboard& k) {
    std::string out;
    for (const KeyboardOutput& o : k.takeOutput()) {
        switch (o.kind) {
            case Kind::Text: out += o.text; break;
            case Kind::Backspace: out += "[bs]"; break;
            case Kind::Enter: out += "[enter]"; break;
            case Kind::Left: out += "[left]"; break;
            case Kind::Right: out += "[right]"; break;
            case Kind::Clear: out += "[clear]"; break;
            case Kind::Close: out += "[close]"; break;
        }
    }
    return out;
}

std::string pngPath(const std::string& name) {
    const char* dir = std::getenv("FRAMEDOCK_PNG_DIR");
    if (dir == nullptr || *dir == '\0') return {};
    std::filesystem::create_directories(dir);
    return std::string(dir) + "/" + name;
}

void testKorean(FontSet& fonts) {
    VrKeyboard k(fonts);
    CHECK(k.layout() == VrKeyboard::Layout::Hangul);
    typeAll(k, U"ㄱㅗㅇㅑㅇㅇㅣ");
    CHECK(outputs(k) == "고양");     // finished syllables go out at once
    CHECK(k.preedit() == "이");      // the last one is still composing
    tap(k, "enter");
    CHECK(outputs(k) == "이[enter]");  // enter finishes it first
    CHECK(k.preedit().empty());
    // Shift gives the doubled consonants and ㅒ ㅖ, once
    tap(k, "shift");
    CHECK(k.shift());
    tap(k, "ㄱ");  // ㄲ
    CHECK(!k.shift());
    typeAll(k, U"ㅗㅊ");
    CHECK(k.preedit() == "꽃");
    tap(k, "space");
    CHECK(outputs(k) == "꽃 ");
    // Backspace takes back a jamo while composing, then deletes in the field
    typeAll(k, U"ㄱㅏㅂㅅ");
    CHECK(k.preedit() == "값");
    tap(k, "backspace");
    CHECK(k.preedit() == "갑");
    CHECK(outputs(k).empty());
    tap(k, "backspace");
    tap(k, "backspace");
    tap(k, "backspace");
    CHECK(k.preedit().empty());
    CHECK(outputs(k).empty());
    tap(k, "backspace");
    CHECK(outputs(k) == "[bs]");
    // A number or a symbol finishes the syllable
    typeAll(k, U"ㅎㅏ");
    tap(k, "1");
    CHECK(outputs(k) == "하1");
    // ← → finish the syllable before moving the caret
    typeAll(k, U"ㄴㅏ");
    tap(k, "left");
    CHECK(outputs(k) == "나[left]");
    // 지우기 drops the composition and clears the field
    typeAll(k, U"ㄷㅏ");
    tap(k, "clear");
    CHECK(outputs(k) == "[clear]");
    CHECK(k.preedit().empty());
    // Close finishes the syllable
    typeAll(k, U"ㄹㅏ");
    tap(k, "close");
    CHECK(outputs(k) == "라[close]");
}

void testEnglishAndSymbols(FontSet& fonts) {
    VrKeyboard k(fonts);
    typeAll(k, U"ㅎㅏ");
    tap(k, "lang");  // switching finishes the syllable
    CHECK(k.layout() == VrKeyboard::Layout::English);
    CHECK(outputs(k) == "하");
    tap(k, "shift");
    tap(k, "h");
    tap(k, "i");
    CHECK(outputs(k) == "Hi");
    // Shift twice quickly: locked
    tap(k, "shift", 10.0);
    tap(k, "shift", 10.2);
    CHECK(k.capsLock());
    tap(k, "o");
    tap(k, "k");
    CHECK(outputs(k) == "OK");
    CHECK(k.capsLock());
    tap(k, "shift", 11.0);
    CHECK(!k.shift());
    tap(k, "@");
    tap(k, "/");
    CHECK(outputs(k) == "@/");
    // Symbols, and back to the letters it came from
    tap(k, "symbols");
    CHECK(k.layout() == VrKeyboard::Layout::Symbols);
    double x, y;
    CHECK(k.keyCenter("symbols", x, y));
    for (const char* s : {"!", "#", "$", "%", "^", "&", "*", "(", ")", "=", "+", "[", "]", "{", "}", ";", ":", "'",
                          "\"", "<", ">", "\\", "|", "`", "~", ",", ".", "?", "_", "-", "/", "@"}) {
        CHECK(tap(k, s));
    }
    CHECK(outputs(k) == "!#$%^&*()=+[]{};:'\"<>\\|`~,.?_-/@");
    tap(k, "symbols");
    CHECK(k.layout() == VrKeyboard::Layout::English);
    tap(k, "lang");
    CHECK(k.layout() == VrKeyboard::Layout::Hangul);
    tap(k, "symbols");
    tap(k, "lang");  // 한/영 from the symbols goes back to the letters
    CHECK(k.layout() == VrKeyboard::Layout::Hangul);
}

void testRepeat(FontSet& fonts) {
    VrKeyboard k(fonts);
    double x, y;
    // Backspace held: once at the press, again 0.4 s later, then every 0.1 s
    CHECK(k.keyCenter("backspace", x, y));
    k.pointerMove(x, y, 0);
    CHECK(k.pointerDown(x, y, 0) == VrKeyboard::Press::Key);
    CHECK(outputs(k) == "[bs]");
    CHECK(!k.tick(0.3));
    CHECK(k.nextWake() > 0.39 && k.nextWake() < 0.41);
    CHECK(k.tick(0.4));
    CHECK(k.tick(0.5));
    CHECK(outputs(k) == "[bs][bs]");
    k.pointerMove(x + 2, y, 0.55);  // a small wobble on the key keeps it going
    CHECK(k.tick(0.6));
    CHECK(outputs(k) == "[bs]");
    k.pointerUp();
    CHECK(!k.tick(0.7));
    CHECK(std::isinf(k.nextWake()));
    // A held jamo repeats: ㅋㅋㅋ
    CHECK(k.keyCenter("ㅋ", x, y));
    k.pointerDown(x, y, 1.0);
    k.tick(1.4);
    k.tick(1.5);
    k.pointerUp();
    tap(k, "space");
    CHECK(outputs(k) == "ㅋㅋㅋ ");
    // Sliding off the key stops it; so does the laser leaving
    CHECK(k.keyCenter("space", x, y));
    k.pointerDown(x, y, 2.0);
    double ox, oy;
    CHECK(k.keyCenter("enter", ox, oy));
    k.pointerMove(ox, oy, 2.2);
    CHECK(!k.tick(2.5));
    k.pointerUp();
    CHECK(k.keyCenter("left", x, y));
    k.pointerDown(x, y, 3.0);
    k.pointerLeave();
    CHECK(!k.tick(3.5));
    outputs(k);
    // Modifier keys don't repeat
    CHECK(k.keyCenter("lang", x, y));
    k.pointerDown(x, y, 4.0);
    CHECK(!k.tick(4.5));
    k.pointerUp();
    // A Shift used by a held key keeps its character while it repeats
    k.setLayout(VrKeyboard::Layout::English);
    tap(k, "shift");
    CHECK(k.keyCenter("a", x, y));
    k.pointerDown(x, y, 5.0);
    k.tick(5.4);
    k.pointerUp();
    CHECK(outputs(k) == "AA");
    tap(k, "a");
    CHECK(outputs(k) == "a");
}

void testHitAreas(FontSet& fonts) {
    VrKeyboard k(fonts);
    double x, y;
    CHECK(k.keyCenter("handle", x, y));
    CHECK(k.pointerDown(x, y, 0) == VrKeyboard::Press::Handle);
    k.pointerUp();
    CHECK(k.pointerDown(3, VrKeyboard::kHeight - 3, 0) == VrKeyboard::Press::None);  // the margin
    k.pointerUp();
    for (const VrKeyboard::Layout layout :
         {VrKeyboard::Layout::Hangul, VrKeyboard::Layout::English, VrKeyboard::Layout::Symbols}) {
        k.setLayout(layout);
        const std::vector<VrKeyboard::Key>& keys = k.keys();
        CHECK(keys.size() >= 50);
        for (size_t i = 0; i < keys.size(); ++i) {
            const VrKeyboard::Key& a = keys[i];
            CHECK(a.x >= 0 && a.y >= 0 && a.x + a.w <= VrKeyboard::kWidth && a.y + a.h <= VrKeyboard::kHeight);
            CHECK(k.keyAt(a.x + a.w / 2, a.y + a.h / 2) == a.id);
            if (a.id != "handle") CHECK(a.w >= 85 && a.h >= 48);  // big enough for a laser at arm's length
            for (size_t j = i + 1; j < keys.size(); ++j) {
                const VrKeyboard::Key& b = keys[j];
                const bool overlap = a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
                if (overlap) std::printf("  overlap %s / %s\n", a.id.c_str(), b.id.c_str());
                CHECK(!overlap);
            }
        }
        // Every row ends at the same right edge
        double right = 0;
        for (const VrKeyboard::Key& key : keys) {
            if (key.id != "handle" && key.id != "close") right = std::max(right, key.x + key.w);
        }
        CHECK(std::abs(right - (VrKeyboard::kWidth - 24)) < 1);
    }
}

void testField(FontSet& fonts) {
    VrKeyboard k(fonts);
    k.setField("프로필 ", true);
    typeAll(k, U"ㅎㅏㄴ");
    tap(k, "space");
    CHECK(outputs(k) == "한 ");
    tap(k, "backspace");
    CHECK(outputs(k) == "[bs]");
    k.reset();
    CHECK(k.preedit().empty());
}

void testPictures(FontSet& fonts) {
    VrKeyboard k(fonts);
    k.setTitle("키보드 · 브라우저");
    typeAll(k, U"ㄱㅗㅇㅑㅇ");
    k.takeOutput();
    double x, y;
    k.keyCenter("ㅇ", x, y);
    k.pointerMove(x, y, 0);  // hovered
    k.render();
    const std::string hangulPng = pngPath("keyboard-hangul.png");
    if (!hangulPng.empty()) CHECK(k.writePng(hangulPng));
    // The picture is opaque where the panel is (premultiplied: alpha 255 in the middle)
    const uint8_t* p = k.pixels();
    CHECK(p[(300 * VrKeyboard::kWidth + 640) * 4 + 3] == 255);

    VrKeyboard e(fonts);
    e.setLayout(VrKeyboard::Layout::English);
    tap(e, "shift");
    e.render();
    const std::string englishPng = pngPath("keyboard-english.png");
    if (!englishPng.empty()) CHECK(e.writePng(englishPng));

    VrKeyboard s(fonts);
    s.setLayout(VrKeyboard::Layout::Symbols);
    const std::string symbolsPng = pngPath("keyboard-symbols.png");
    if (!symbolsPng.empty()) CHECK(s.writePng(symbolsPng));

    VrKeyboard r(fonts);
    r.setTitle("프로필 이름");
    r.setField("레이싱 ", true);
    typeAll(r, U"ㅅㅔㅌ");
    r.render();
    const std::string renamePng = pngPath("keyboard-rename.png");
    if (!renamePng.empty()) CHECK(r.writePng(renamePng));
}

}  // namespace

int main() {
    FontSet fonts;
    fonts.load(kFont, kBold);
    testKorean(fonts);
    testEnglishAndSymbols(fonts);
    testRepeat(fonts);
    testHitAreas(fonts);
    testField(fonts);
    testPictures(fonts);
    if (failures == 0) std::printf("keyboard: all passed\n");
    return failures == 0 ? 0 : 1;
}
