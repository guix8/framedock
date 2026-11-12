// 描画の共通部品（色・フォント・文字や角丸の描画・OpenVR 用の画素変換）。
#pragma once

#include <cairo.h>

#include <cstdint>
#include <string>
#include <vector>

/** RGB の色（0〜1）。 */
struct Color {
    double r, g, b;
};

// 色の定数は theme.h にまとめてある（コントラスト比の確認と同じ定義を使うため）

/**
 * 本文用と太字用の 2 つのフォント。一度読み込んで使い回す。
 */
class FontSet {
public:
    FontSet();
    ~FontSet();
    FontSet(const FontSet&) = delete;
    FontSet& operator=(const FontSet&) = delete;

    /**
     * フォントを読み込む（前と同じパスなら何もしない）。読めなければ fontconfig の "Noto Sans CJK JP" を使う。
     * @param regularPath 本文用フォントのパス（.ttc は 0 番 = 日本語）
     * @param boldPath 太字用フォントのパス
     */
    void load(const std::string& regularPath, const std::string& boldPath);

    /** @return 本文用フォント */
    cairo_font_face_t* regular() const { return regular_; }
    /** @return 太字用フォント */
    cairo_font_face_t* bold() const { return bold_; }

private:
    void* ftLibrary_ = nullptr;  ///< FT_Library
    cairo_font_face_t* regular_ = nullptr;
    cairo_font_face_t* bold_ = nullptr;
    std::string regularPath_;
    std::string boldPath_;

    /**
     * フォントファイルから cairo のフォントを作る。
     * @param path フォントファイル
     * @param bold 代わりのフォントを太字にするか
     * @return 作ったフォント
     */
    cairo_font_face_t* createFace(const std::string& path, bool bold);

    /** 読み込んだフォントを手放す。 */
    void release();
};

/**
 * 描画中に使う道具（cairo とフォント）をまとめたもの。
 */
struct Pen {
    cairo_t* cr;
    const FontSet* fonts;

    /**
     * 色を設定する。
     * @param c 色
     * @param alpha 不透明度
     */
    void color(Color c, double alpha = 1.0) const { cairo_set_source_rgba(cr, c.r, c.g, c.b, alpha); }

    /**
     * 文字列の幅を測る。
     * @param text UTF-8 の文字列
     * @param size 文字の大きさ（px）
     * @param isBold 太字か
     * @return 幅（px）
     */
    double measure(const char* text, double size, bool isBold = false) const;
    double measure(const std::string& text, double size, bool isBold = false) const {
        return measure(text.c_str(), size, isBold);
    }

    /**
     * 文字を描く。
     * @param x 左端（alignRight なら右端）
     * @param y ベースライン
     * @param text UTF-8 の文字列
     * @param size 文字の大きさ（px）
     * @param c 色
     * @param isBold 太字にするか
     * @param alignRight 右揃えにするか
     * @return 描いた文字列の幅（px）
     */
    double text(double x, double y, const char* text, double size, Color c, bool isBold = false,
                bool alignRight = false) const;
    double text(double x, double y, const std::string& text, double size, Color c, bool isBold = false,
                bool alignRight = false) const {
        return this->text(x, y, text.c_str(), size, c, isBold, alignRight);
    }

    /**
     * 角の丸い四角の道筋を作る。
     * @param x 左
     * @param y 上
     * @param w 幅
     * @param h 高さ
     * @param r 角の半径
     */
    void roundedRect(double x, double y, double w, double h, double r) const;

    /**
     * 小さな色つきの丸（凡例用）を描く。
     * @param x 中心 x
     * @param y 中心 y
     * @param c 色
     */
    void dot(double x, double y, Color c) const;
};

/**
 * 収まる幅になるまで文字を小さくした大きさを返す。
 * @param pen 描画の道具
 * @param text 文字列
 * @param size 最初の大きさ
 * @param minSize これより小さくしない
 * @param maxWidth 収めたい幅（px）
 * @param bold 太字か
 * @return 文字の大きさ
 */
double fitSize(const Pen& pen, const char* text, double size, double minSize, double maxWidth, bool bold);
inline double fitSize(const Pen& pen, const std::string& text, double size, double minSize, double maxWidth, bool bold) {
    return fitSize(pen, text.c_str(), size, minSize, maxWidth, bold);
}

/** At most this many characters are measured: longer text (a page's URL, title or dialog) is cut first. */
constexpr size_t kEllipsizeMaxChars = 256;

/**
 * Cuts text with "…" (on a UTF-8 character boundary) so that it fits maxWidth. Bounded work whatever the text:
 * it is first cut to kEllipsizeMaxChars characters (a page controls its URL, title and dialog text, and measuring a
 * megabyte at each step froze the main loop), then the cut is found by a binary search (O(log n) measurements).
 * @param size the size it's drawn at
 * @param maxWidth the room (px)
 * @return text when it fits, else a shorter one ending in "…" (just "…" when nothing else fits)
 */
std::string ellipsize(const Pen& pen, const std::string& text, double size, double maxWidth, bool bold);

/**
 * Splits text at '\n' and then between words so each line fits maxWidth. A word wider than a line (Japanese, which
 * has no spaces) is broken between characters, closing punctuation (、。）」) kept on the line before.
 * @return the lines (a paragraph that is empty gives an empty line)
 */
std::vector<std::string> wrapText(const Pen& pen, const std::string& text, double size, double maxWidth,
                                  bool bold = false);

/** @return the byte offset where the first maxChars UTF-8 characters end (text.size() if it has fewer) */
size_t utf8Prefix(const std::string& text, size_t maxChars);

/**
 * 文字を、ある高さの帯の縦の真ん中に置いたときのベースライン。
 * @param top 帯の上
 * @param h 帯の高さ
 * @param size 文字の大きさ
 * @return ベースラインの y
 */
double centerBaseline(double top, double h, double size);

/**
 * 文字を横の真ん中にそろえて描く。
 * @param pen 描画の道具
 * @param cx 真ん中の x
 * @param baseline ベースライン
 * @param text 文字列
 * @param size 大きさ
 * @param c 色
 * @param bold 太字か
 */
void textCentered(const Pen& pen, double cx, double baseline, const char* text, double size, Color c, bool bold);
inline void textCentered(const Pen& pen, double cx, double baseline, const std::string& text, double size, Color c, bool bold) {
    textCentered(pen, cx, baseline, text.c_str(), size, c, bold);
}

/**
 * カードを描く: 重ねた影で浮かせ、塗り、（あれば）枠、内側の上の縁の 1px のハイライト。
 * @param pen 描画の道具
 * @param x 左
 * @param y 上
 * @param w 幅
 * @param h 高さ
 * @param r 角の半径
 * @param fill 塗り
 * @param border 枠の色
 * @param borderWidth 枠の太さ（0 なら描かない）
 */
void drawCard(const Pen& pen, double x, double y, double w, double h, double r, Color fill, Color border,
              double borderWidth);

/**
 * 枠の線を描く（角丸）。
 * @param pen 描画の道具
 * @param x 左
 * @param y 上
 * @param w 幅
 * @param h 高さ
 * @param r 角の半径
 * @param c 色
 * @param width 線の太さ
 */
void strokeRounded(const Pen& pen, double x, double y, double w, double h, double r, Color c, double width);

/**
 * ✓ を線で描く（フォントに頼らない）。
 * @param cr cairo
 * @param cx 真ん中の x
 * @param cy 真ん中の y
 * @param s 大きさ（px）
 * @param c 色
 */
void drawCheck(cairo_t* cr, double cx, double cy, double s, Color c);

/**
 * 塗りつぶしの丸を描く。
 * @param cr cairo
 * @param cx 真ん中の x
 * @param cy 真ん中の y
 * @param r 半径
 * @param c 色
 */
void drawDisc(cairo_t* cr, double cx, double cy, double r, Color c);

/** One part of a segmented pill. */
struct PillPart {
    std::string label;
    bool pressed = false;  ///< held down (the selected part fills darker)
    bool hovered = false;
};

/**
 * The dashboard's segmented pill (Autostart On | Off, Compact, ...), shared by the dashboard and the layout editor
 * so every toggle looks the same: a bordered pill, the parts splitting its width evenly (that's also each part's
 * hit area), the selected part filled in the accent with a ✓.
 * @param parts left to right
 * @param selected index of the selected part (-1 = none)
 * @param usable false = drawn faded, no border
 * @param size label size (shrinks to fit)
 */
void drawSegmentedPill(const Pen& pen, double x, double y, double w, double h, const std::vector<PillPart>& parts,
                       int selected, bool usable, double size = 24);

/** The pill's rim: the parts are drawn this far inside its edge. */
constexpr double kPillInset = 5;

/**
 * Where drawSegmentedPill draws part i of n in a pill at x, w wide: [left, left + width). Hit areas that match what's
 * drawn use these edges between parts and the pill's own edges outside (the rim belongs to the end parts).
 * @param hitLeft, hitRight the part's hit area (optional)
 * @return the drawn part's left edge; width = (w - 2 * kPillInset) / n
 */
double pillPartLeft(double x, double w, size_t n, size_t i, double* hitLeft = nullptr, double* hitRight = nullptr);

/** How far either side of a slider's track its hit area reaches (the ends are easy to grab with a laser). */
constexpr double kSliderReach = 20;

/**
 * The dashboard's slider (the Color page's, the 기기 page's volume): a 10 px round track, the part up to the knob in
 * fill, the knob (larger while held), a ring while held or pointed at. The caller draws the title and the value.
 * @param fraction the knob's place on the track, 0-1 (clamped)
 * @param usable false = grey, no fill
 * @param active the laser holds the knob
 * @param ring draw the ring (held, or the laser is over the hit area)
 */
void drawSlider(const Pen& pen, double trackX, double trackW, double cy, double fraction, Color fill, bool usable,
                bool active, bool ring);

/** @return the fraction 0-1 of a slider's track at x (clamped; the hit area reaches kSliderReach past the ends) */
double sliderFraction(double x, double trackX, double trackW);

/**
 * cairo の画像（乗算済み ARGB）を OpenVR 用の非乗算済み RGBA に変換する。
 * @param surface cairo の画像（CAIRO_FORMAT_ARGB32）
 * @param out 書き込み先（幅 * 高さ * 4 バイトに合わせる）
 */
void surfaceToRgba(cairo_surface_t* surface, std::vector<uint8_t>& out);
