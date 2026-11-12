// Hangul composition for the VR keyboard's 두벌식 (2-set) layout: jamo keys in, syllables out. Pure (no drawing, no
// I/O) so the tests can run every case.
//
// The usual 2-set automaton: an initial consonant (초성), a vowel (중성) and an optional final consonant (종성) make a
// syllable. A vowel after a final consonant takes it over as its own initial (갑 + ㅏ → 가바; a double final splits:
// 값 + ㅏ → 갑사). Vowels combine (ㅗ+ㅏ → ㅘ, …) and finals combine (ㄱ+ㅅ → ㄳ, …) as typed. Shift gives ㄲ ㄸ ㅃ
// ㅆ ㅉ ㅒ ㅖ (the keyboard sends those jamo directly). Backspace takes back the last jamo typed into the syllable
// being composed (값 → 갑 → 가 → ㄱ), not the whole syllable.
#pragma once

#include <string>
#include <vector>

namespace hangul {

/** @return true for a Hangul compatibility jamo the 2-set keys produce (U+3131 ㄱ … U+3163 ㅣ) */
bool isJamo(char32_t c);
/** @return true for a consonant jamo (ㄱ … ㅎ) */
bool isConsonant(char32_t c);
/** @return true for a vowel jamo (ㅏ … ㅣ) */
bool isVowel(char32_t c);

/** @return c as UTF-8 */
std::string utf8(char32_t c);
/** @return the code points of a UTF-8 string (invalid bytes become U+FFFD) */
std::u32string decodeUtf8(const std::string& text);
/** @return text without its last code point (whole UTF-8 sequences) */
std::string dropLastChar(const std::string& text);

class Composer {
public:
    /**
     * One jamo key.
     * @param jamo a compatibility jamo (isJamo); anything else commits what is being composed and comes back as is
     * @return text that is final now (a finished syllable, possibly followed by more), "" if all of it still composes
     */
    std::string input(char32_t jamo);
    /**
     * Backspace: takes back the last jamo of the syllable being composed.
     * @return false if nothing was being composed (the caller deletes a character in the field instead)
     */
    bool backspace();
    /** @return what is being composed, finished (UTF-8), and forgets it */
    std::string commit();
    /** Forgets what is being composed (지우기). */
    void reset();
    /** @return the syllable (or lone jamo) being composed, UTF-8; "" when nothing is */
    std::string preedit() const;
    bool composing() const { return !history_.empty(); }

private:
    /** One syllable in the making: indices into the 초성/중성/종성 tables (-1 none; final 0 = none). */
    struct Syllable {
        int initial = -1;
        int vowel = -1;
        int final = 0;
        bool empty() const { return initial < 0 && vowel < 0; }
    };
    Syllable current_;
    std::vector<Syllable> history_;  ///< the states before each jamo of this syllable (backspace pops)

    static std::string text(const Syllable& s);
    /** Starts a new syllable with these parts, after finishing the old one. */
    std::string startNew(const Syllable& next);
};

}  // namespace hangul
