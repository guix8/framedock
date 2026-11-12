// The VR keyboard's Hangul 2-set composition (browser/hangul.*): syllables, double vowels and finals, a final moving
// to the next syllable, backspace one jamo at a time.
#include "hangul.h"

#include <cstdio>
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

#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        const std::string a_ = (a), b_ = (b);                                                             \
        if (a_ != b_) {                                                                                   \
            std::printf("FAIL line %d: %s == %s (\"%s\" vs \"%s\")\n", __LINE__, #a, #b, a_.c_str(), b_.c_str()); \
            ++failures;                                                                                   \
        }                                                                                                 \
    } while (false)

/** Types the keys; returns everything finished so far plus what is still composing. */
struct Typed {
    std::string committed;
    std::string preedit;
    std::string all() const { return committed + preedit; }
};

Typed type(hangul::Composer& composer, const std::u32string& keys) {
    Typed t;
    for (const char32_t k : keys) t.committed += composer.input(k);
    t.preedit = composer.preedit();
    return t;
}

Typed type(const std::u32string& keys) {
    hangul::Composer composer;
    return type(composer, keys);
}

std::string word(const std::u32string& keys) { return type(keys).all(); }

void testSyllables() {
    CHECK_EQ(word(U"ㄱㅏ"), "가");
    CHECK_EQ(type(U"ㄱㅏ").preedit, "가");
    CHECK_EQ(type(U"ㄱㅏ").committed, "");
    CHECK_EQ(word(U"ㅎㅏㄴㄱㅡㄹ"), "한글");
    CHECK_EQ(type(U"ㅎㅏㄴㄱㅡㄹ").committed, "한");
    CHECK_EQ(type(U"ㅎㅏㄴㄱㅡㄹ").preedit, "글");
    CHECK_EQ(word(U"ㄱㅗㅇㅑㅇㅇㅣ"), "고양이");
    CHECK_EQ(word(U"ㅇㅏㄴㄴㅕㅇㅎㅏㅅㅔㅇㅛ"), "안녕하세요");
    CHECK_EQ(word(U"ㄲㅗㅊ"), "꽃");
    CHECK_EQ(word(U"ㅃㅏㅇ"), "빵");
    CHECK_EQ(word(U"ㅇㅠㅌㅠㅂㅡ"), "유튜브");
    CHECK_EQ(word(U"ㄴㅔㅇㅣㅂㅓ"), "네이버");
    CHECK_EQ(word(U"ㅎㅐㅆㄷㅏ"), "했다");
    CHECK_EQ(word(U"ㅇㅖㅃㅡㄷㅏ"), "예쁘다");
    CHECK_EQ(word(U"ㅇㅒㄱㅣ"), "얘기");
}

void testFinalMovesOn() {
    // A vowel after a final takes it as its initial
    CHECK_EQ(word(U"ㄱㅏㅂㅏ"), "가바");
    CHECK_EQ(word(U"ㅇㅣㅆㅓ"), "이써");  // ㅆ as a final moves whole
    CHECK_EQ(word(U"ㄲㅏㄲㅏ"), "까까");
    // A double final splits: the second part moves on
    CHECK_EQ(word(U"ㄱㅏㅂㅅㅏ"), "갑사");
    CHECK_EQ(word(U"ㅇㅓㅂㅅㅓ"), "업서");
    CHECK_EQ(word(U"ㅅㅏㄹㅁㅏ"), "살마");
    CHECK_EQ(word(U"ㄷㅏㄹㄱㅣ"), "달기");
    // A consonant that doesn't join the final starts the next syllable
    CHECK_EQ(word(U"ㄷㅏㄹㄱㅇㅣ"), "닭이");
    CHECK_EQ(word(U"ㅇㅓㅂㅅㅇㅓ"), "없어");
    CHECK_EQ(word(U"ㅁㅏㄴㅎㅇㅣ"), "많이");
}

void testDoubleFinals() {
    CHECK_EQ(word(U"ㄱㅏㅂㅅ"), "값");
    CHECK_EQ(word(U"ㄷㅏㄹㄱ"), "닭");
    CHECK_EQ(word(U"ㅅㅏㄹㅁ"), "삶");
    CHECK_EQ(word(U"ㅇㅏㄴㅈ"), "앉");
    CHECK_EQ(word(U"ㅇㅣㄹㄱ"), "읽");
    CHECK_EQ(word(U"ㅇㅕㄹㅂ"), "엷");
    CHECK_EQ(word(U"ㄱㅗㄹㅅ"), "곬");
    CHECK_EQ(word(U"ㅎㅏㄹㅌ"), "핥");
    CHECK_EQ(word(U"ㅇㅡㄹㅍ"), "읊");
    CHECK_EQ(word(U"ㅇㅏㄹㅎ"), "앓");
    CHECK_EQ(word(U"ㅁㅏㄱㅅ"), "맋");
    CHECK_EQ(word(U"ㄱㅏㄱㅅ"), "갃");
    // No third consonant joins a double final
    CHECK_EQ(word(U"ㄱㅏㄱㅅㅅ"), "갃ㅅ");
    // ㄸ ㅃ ㅉ are never finals
    CHECK_EQ(word(U"ㅇㅏㄸㅏ"), "아따");
    CHECK_EQ(type(U"ㅇㅏㄸ").committed, "아");
    CHECK_EQ(type(U"ㅇㅏㄸ").preedit, "ㄸ");
    CHECK_EQ(word(U"ㅇㅏㅃ"), "아ㅃ");
    CHECK_EQ(word(U"ㅇㅏㅉ"), "아ㅉ");
    // ㄲ and ㅆ are
    CHECK_EQ(word(U"ㄲㅏㄲ"), "깎");
    CHECK_EQ(word(U"ㅇㅣㅆ"), "있");
}

void testDoubleVowels() {
    CHECK_EQ(word(U"ㄱㅗㅏ"), "과");
    CHECK_EQ(word(U"ㅇㅗㅐ"), "왜");
    CHECK_EQ(word(U"ㅎㅗㅣ"), "회");
    CHECK_EQ(word(U"ㅇㅜㅓㄴ"), "원");
    CHECK_EQ(word(U"ㄱㅜㅔ"), "궤");
    CHECK_EQ(word(U"ㅇㅜㅣ"), "위");
    CHECK_EQ(word(U"ㅇㅡㅣㅅㅏ"), "의사");
    CHECK_EQ(word(U"ㅗㅏ"), "ㅘ");  // a lone vowel combines too
    CHECK_EQ(word(U"ㅜㅣ"), "ㅟ");
    // Vowels that don't combine
    CHECK_EQ(word(U"ㄱㅏㅏ"), "가ㅏ");
    CHECK_EQ(word(U"ㅏㅏ"), "ㅏㅏ");
    CHECK_EQ(word(U"ㄱㅗㅓ"), "고ㅓ");
}

void testLoneJamo() {
    CHECK_EQ(word(U"ㄱ"), "ㄱ");
    CHECK_EQ(word(U"ㄱㄱ"), "ㄱㄱ");
    CHECK_EQ(type(U"ㄱㄱ").committed, "ㄱ");
    CHECK_EQ(word(U"ㅂㅂㅂ"), "ㅂㅂㅂ");
    CHECK_EQ(word(U"ㅏ"), "ㅏ");
    CHECK_EQ(word(U"ㅏㄱ"), "ㅏㄱ");  // a lone vowel takes no final
    CHECK_EQ(word(U"ㅏㄱㅏ"), "ㅏ가");
    CHECK_EQ(word(U"ㅋㅋㅋ"), "ㅋㅋㅋ");
    CHECK_EQ(word(U"ㅎㅎ"), "ㅎㅎ");
}

void testOtherKeys() {
    hangul::Composer c;
    CHECK_EQ(c.input(U'ㄱ'), "");
    CHECK_EQ(c.input(U'ㅏ'), "");
    CHECK_EQ(c.input(U' '), "가 ");  // a space finishes the syllable and comes after it
    CHECK(!c.composing());
    CHECK_EQ(c.input(U'a'), "a");
    CHECK_EQ(c.input(U'ㅎ'), "");
    CHECK_EQ(c.input(U'?'), "ㅎ?");
    CHECK_EQ(c.input(U'1'), "1");
    CHECK(!c.composing());
}

void testBackspace() {
    {
        hangul::Composer c;
        type(c, U"ㄱㅏㅂㅅ");
        CHECK_EQ(c.preedit(), "값");
        CHECK(c.backspace());
        CHECK_EQ(c.preedit(), "갑");
        CHECK(c.backspace());
        CHECK_EQ(c.preedit(), "가");
        CHECK(c.backspace());
        CHECK_EQ(c.preedit(), "ㄱ");
        CHECK(c.backspace());
        CHECK_EQ(c.preedit(), "");
        CHECK(!c.composing());
        CHECK(!c.backspace());  // nothing left: the caller deletes in the field
    }
    {
        hangul::Composer c;
        type(c, U"ㄱㅗㅏ");
        CHECK(c.backspace());
        CHECK_EQ(c.preedit(), "고");
        CHECK_EQ(c.input(U'ㅐ'), "");
        CHECK_EQ(c.preedit(), "괘");
    }
    {
        // After the final moved on: the new syllable steps back to its lone initial, then to nothing
        hangul::Composer c;
        const Typed t = type(c, U"ㅇㅣㅆㅓ");
        CHECK_EQ(t.committed, "이");
        CHECK_EQ(t.preedit, "써");
        CHECK(c.backspace());
        CHECK_EQ(c.preedit(), "ㅆ");
        CHECK(c.backspace());
        CHECK_EQ(c.preedit(), "");
        CHECK(!c.backspace());
    }
    {
        // Typing again after a backspace composes from there
        hangul::Composer c;
        type(c, U"ㄷㅏㄹㄱ");
        CHECK(c.backspace());
        CHECK_EQ(c.preedit(), "달");
        CHECK_EQ(c.input(U'ㅁ'), "");
        CHECK_EQ(c.preedit(), "닮");
    }
    {
        hangul::Composer c;
        CHECK(!c.backspace());
        type(c, U"ㅏ");
        CHECK(c.backspace());
        CHECK(!c.composing());
    }
}

void testCommitReset() {
    hangul::Composer c;
    type(c, U"ㅎㅏㄴ");
    CHECK_EQ(c.commit(), "한");
    CHECK(!c.composing());
    CHECK_EQ(c.preedit(), "");
    CHECK_EQ(c.commit(), "");
    type(c, U"ㄱㅡㄹ");
    c.reset();
    CHECK(!c.composing());
    CHECK_EQ(c.commit(), "");
    // After a commit the next jamo starts fresh (no final carried over)
    type(c, U"ㄱㅏㄴ");
    c.commit();
    CHECK_EQ(type(c, U"ㅏ").preedit, "ㅏ");
}

void testUtf8Helpers() {
    CHECK_EQ(hangul::utf8(U'A'), "A");
    CHECK_EQ(hangul::utf8(U'é'), "é");
    CHECK_EQ(hangul::utf8(U'한'), "한");
    CHECK_EQ(hangul::utf8(U'😀'), "😀");
    CHECK(hangul::decodeUtf8("한a글") == std::u32string(U"한a글"));
    CHECK(hangul::decodeUtf8(std::string("a\xff" "b")) == std::u32string(U"a�b"));
    CHECK(hangul::decodeUtf8(std::string("\xed\x95")) == std::u32string(U"��"));  // cut short
    CHECK_EQ(hangul::dropLastChar("한글"), "한");
    CHECK_EQ(hangul::dropLastChar("ab"), "a");
    CHECK_EQ(hangul::dropLastChar("a😀"), "a");
    CHECK_EQ(hangul::dropLastChar(""), "");
    CHECK(hangul::isJamo(U'ㄱ') && hangul::isJamo(U'ㅣ') && !hangul::isJamo(U'가') && !hangul::isJamo(U'a'));
    CHECK(hangul::isConsonant(U'ㅎ') && !hangul::isConsonant(U'ㅏ'));
    CHECK(hangul::isVowel(U'ㅢ') && !hangul::isVowel(U'ㄹ'));
}

}  // namespace

int main() {
    testSyllables();
    testFinalMovesOn();
    testDoubleFinals();
    testDoubleVowels();
    testLoneJamo();
    testOtherKeys();
    testBackspace();
    testCommitReset();
    testUtf8Helpers();
    if (failures == 0) std::printf("hangul: all passed\n");
    return failures == 0 ? 0 : 1;
}
