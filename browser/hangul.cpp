// Hangul 2-set composition. See hangul.h.
#include "hangul.h"

namespace hangul {
namespace {

// The tables of Unicode's syllable formula: U+AC00 + (initial × 21 + vowel) × 28 + final
constexpr char32_t kInitials[19] = {U'ㄱ', U'ㄲ', U'ㄴ', U'ㄷ', U'ㄸ', U'ㄹ', U'ㅁ', U'ㅂ', U'ㅃ', U'ㅅ',
                                    U'ㅆ', U'ㅇ', U'ㅈ', U'ㅉ', U'ㅊ', U'ㅋ', U'ㅌ', U'ㅍ', U'ㅎ'};
constexpr char32_t kVowels[21] = {U'ㅏ', U'ㅐ', U'ㅑ', U'ㅒ', U'ㅓ', U'ㅔ', U'ㅕ', U'ㅖ', U'ㅗ', U'ㅘ', U'ㅙ',
                                  U'ㅚ', U'ㅛ', U'ㅜ', U'ㅝ', U'ㅞ', U'ㅟ', U'ㅠ', U'ㅡ', U'ㅢ', U'ㅣ'};
// Index 0 is "no final"
constexpr char32_t kFinals[28] = {0,     U'ㄱ', U'ㄲ', U'ㄳ', U'ㄴ', U'ㄵ', U'ㄶ', U'ㄷ', U'ㄹ', U'ㄺ',
                                  U'ㄻ', U'ㄼ', U'ㄽ', U'ㄾ', U'ㄿ', U'ㅀ', U'ㅁ', U'ㅂ', U'ㅄ', U'ㅅ',
                                  U'ㅆ', U'ㅇ', U'ㅈ', U'ㅊ', U'ㅋ', U'ㅌ', U'ㅍ', U'ㅎ'};

struct Pair {
    char32_t first, second, combined;
};
// Vowels typed one after the other that make one vowel
constexpr Pair kVowelPairs[] = {
    {U'ㅗ', U'ㅏ', U'ㅘ'}, {U'ㅗ', U'ㅐ', U'ㅙ'}, {U'ㅗ', U'ㅣ', U'ㅚ'}, {U'ㅜ', U'ㅓ', U'ㅝ'},
    {U'ㅜ', U'ㅔ', U'ㅞ'}, {U'ㅜ', U'ㅣ', U'ㅟ'}, {U'ㅡ', U'ㅣ', U'ㅢ'},
};
// Finals typed one after the other that make a double final (겹받침)
constexpr Pair kFinalPairs[] = {
    {U'ㄱ', U'ㅅ', U'ㄳ'}, {U'ㄴ', U'ㅈ', U'ㄵ'}, {U'ㄴ', U'ㅎ', U'ㄶ'}, {U'ㄹ', U'ㄱ', U'ㄺ'},
    {U'ㄹ', U'ㅁ', U'ㄻ'}, {U'ㄹ', U'ㅂ', U'ㄼ'}, {U'ㄹ', U'ㅅ', U'ㄽ'}, {U'ㄹ', U'ㅌ', U'ㄾ'},
    {U'ㄹ', U'ㅍ', U'ㄿ'}, {U'ㄹ', U'ㅎ', U'ㅀ'}, {U'ㅂ', U'ㅅ', U'ㅄ'},
};

template <size_t N>
int indexOf(const char32_t (&table)[N], char32_t c) {
    for (size_t i = 0; i < N; ++i) {
        if (table[i] == c && c != 0) return static_cast<int>(i);
    }
    return -1;
}

char32_t combine(const Pair* pairs, size_t n, char32_t a, char32_t b) {
    for (size_t i = 0; i < n; ++i) {
        if (pairs[i].first == a && pairs[i].second == b) return pairs[i].combined;
    }
    return 0;
}

/** @return the two finals a double final is made of (false: it is a single one) */
bool splitFinal(char32_t final, char32_t& first, char32_t& second) {
    for (const Pair& p : kFinalPairs) {
        if (p.combined == final) {
            first = p.first;
            second = p.second;
            return true;
        }
    }
    return false;
}

}  // namespace

bool isConsonant(char32_t c) { return c >= U'ㄱ' && c <= U'ㅎ'; }
bool isVowel(char32_t c) { return c >= U'ㅏ' && c <= U'ㅣ'; }
bool isJamo(char32_t c) { return isConsonant(c) || isVowel(c); }

std::string utf8(char32_t c) {
    std::string out;
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    return out;
}

std::u32string decodeUtf8(const std::string& text) {
    std::u32string out;
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char b = static_cast<unsigned char>(text[i]);
        const int extra =
            b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
        if (extra < 0) {
            out += U'�';
            ++i;
            continue;
        }
        char32_t c = extra == 0 ? b : b & (0x3F >> extra);
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= text.size() || (static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        if (!ok) {
            out += U'�';
            ++i;
            continue;
        }
        out += c;
        i += static_cast<size_t>(extra) + 1;
    }
    return out;
}

std::string dropLastChar(const std::string& text) {
    if (text.empty()) return text;
    size_t end = text.size() - 1;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

std::string Composer::text(const Syllable& s) {
    if (s.initial >= 0 && s.vowel >= 0) {
        return utf8(0xAC00 + (static_cast<char32_t>(s.initial) * 21 + static_cast<char32_t>(s.vowel)) * 28 +
                    static_cast<char32_t>(s.final));
    }
    if (s.initial >= 0) return utf8(kInitials[s.initial]);
    if (s.vowel >= 0) return utf8(kVowels[s.vowel]);
    return {};
}

std::string Composer::preedit() const { return text(current_); }

std::string Composer::startNew(const Syllable& next) {
    std::string done = text(current_);
    current_ = next;
    history_.clear();
    if (!current_.empty()) {
        // Backspace in the new syllable steps back through what it was made of: an initial carried over from the
        // last syllable first stands alone (이써 → 이ㅆ → 이)
        if (current_.initial >= 0 && current_.vowel >= 0) {
            history_.push_back(Syllable {});
            Syllable initialOnly;
            initialOnly.initial = current_.initial;
            history_.push_back(initialOnly);
        } else {
            history_.push_back(Syllable {});
        }
    }
    return done;
}

std::string Composer::input(char32_t jamo) {
    if (!isJamo(jamo)) return commit() + utf8(jamo);
    const Syllable before = current_;
    Syllable& s = current_;
    if (isConsonant(jamo)) {
        const int initial = indexOf(kInitials, jamo);
        const int final = indexOf(kFinals, jamo);
        if (s.empty()) {
            s.initial = initial;
        } else if (s.vowel < 0) {
            // A consonant on a lone consonant: the first stands as it is
            Syllable next;
            next.initial = initial;
            return startNew(next);
        } else if (s.initial < 0) {
            // A lone vowel takes no final
            Syllable next;
            next.initial = initial;
            return startNew(next);
        } else if (s.final == 0) {
            if (final <= 0) {  // ㄸ ㅃ ㅉ are never finals: a new syllable
                Syllable next;
                next.initial = initial;
                return startNew(next);
            }
            s.final = final;
        } else {
            const char32_t combined = combine(kFinalPairs, sizeof(kFinalPairs) / sizeof(kFinalPairs[0]),
                                              kFinals[s.final], jamo);
            if (combined == 0) {
                Syllable next;
                next.initial = initial;
                return startNew(next);
            }
            s.final = indexOf(kFinals, combined);
        }
        history_.push_back(before);
        return {};
    }
    // A vowel
    const int vowel = indexOf(kVowels, jamo);
    if (s.empty()) {
        s.vowel = vowel;
    } else if (s.vowel < 0) {
        s.vowel = vowel;  // ㄱ + ㅏ → 가
    } else if (s.final != 0) {
        // The final moves over as the next syllable's initial (갑 + ㅏ → 가바); a double final splits (값 + ㅏ → 갑사)
        char32_t keep = 0, carry = kFinals[s.final];
        char32_t first = 0, second = 0;
        if (splitFinal(carry, first, second)) {
            keep = first;
            carry = second;
        }
        s.final = keep == 0 ? 0 : indexOf(kFinals, keep);
        Syllable next;
        next.initial = indexOf(kInitials, carry);
        next.vowel = vowel;
        return startNew(next);
    } else {
        const char32_t combined =
            combine(kVowelPairs, sizeof(kVowelPairs) / sizeof(kVowelPairs[0]), kVowels[s.vowel], jamo);
        if (combined == 0) {
            Syllable next;
            next.vowel = vowel;
            return startNew(next);
        }
        s.vowel = indexOf(kVowels, combined);
    }
    history_.push_back(before);
    return {};
}

bool Composer::backspace() {
    if (history_.empty()) return false;
    current_ = history_.back();
    history_.pop_back();
    if (current_.empty()) history_.clear();
    return true;
}

std::string Composer::commit() {
    std::string done = text(current_);
    reset();
    return done;
}

void Composer::reset() {
    current_ = Syllable {};
    history_.clear();
}

}  // namespace hangul
