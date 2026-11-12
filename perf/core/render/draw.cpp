// 描画の共通部品の実装。
#include "draw.h"

#include "json.h"   // isValidUtf8 / makeValidUtf8
#include "theme.h"  // the segmented pill's colours

#include <cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

/**
 * cairo の決まりに沿って、フォントが要らなくなったら FT_Face を閉じるための後始末。
 * @param face FT_Face
 */
void destroyFtFace(void* face) {
    FT_Done_Face(static_cast<FT_Face>(face));
}

const cairo_user_data_key_t kFtFaceKey {};

}  // namespace

FontSet::FontSet() {
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) == 0) ftLibrary_ = library;
}

FontSet::~FontSet() {
    release();
    // FT_Face は cairo が持っている間は生きている必要があるので、FT_Library は閉じない（終了時に OS が片付ける）
}

cairo_font_face_t* FontSet::createFace(const std::string& path, bool bold) {
    FT_Face face = nullptr;
    if (ftLibrary_ != nullptr && FT_New_Face(static_cast<FT_Library>(ftLibrary_), path.c_str(), 0, &face) == 0) {
        cairo_font_face_t* cairoFace = cairo_ft_font_face_create_for_ft_face(face, 0);
        if (cairo_font_face_set_user_data(cairoFace, &kFtFaceKey, face, destroyFtFace) == CAIRO_STATUS_SUCCESS) {
            return cairoFace;
        }
        cairo_font_face_destroy(cairoFace);
        FT_Done_Face(face);
    }
    std::fprintf(stderr, "[描画] フォント %s を読めないので Noto Sans CJK JP を探して使います\n", path.c_str());
    return cairo_toy_font_face_create("Noto Sans CJK JP", CAIRO_FONT_SLANT_NORMAL,
                                      bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
}

void FontSet::release() {
    // cairo_t が参照を持っていれば、そちらが手放すまでフォントは生きている
    if (regular_ != nullptr) cairo_font_face_destroy(regular_);
    if (bold_ != nullptr) cairo_font_face_destroy(bold_);
    regular_ = nullptr;
    bold_ = nullptr;
}

void FontSet::load(const std::string& regularPath, const std::string& boldPath) {
    if (regular_ != nullptr && regularPath == regularPath_ && boldPath == boldPath_) return;
    release();
    regular_ = createFace(regularPath, false);
    bold_ = createFace(boldPath, true);
    regularPath_ = regularPath;
    boldPath_ = boldPath;
}

namespace {

/**
 * 壊れた UTF-8 を描かない: cairo は一度 CAIRO_STATUS_INVALID_STRING になるとその cairo_t で二度と描けないので、
 * 渡す前に U+FFFD に置き換える。正しい文字列（ほとんど全部）は確かめるだけで、そのまま返す。
 * @param text 文字列
 * @param storage 置き換えたときの置き場所
 * @return 正しい UTF-8 の文字列
 */
const char* validUtf8(const char* text, std::string& storage) {
    if (isValidUtf8(text, std::strlen(text))) return text;
    storage = text;
    makeValidUtf8(storage);
    return storage.c_str();
}

}  // namespace

double Pen::measure(const char* text, double size, bool isBold) const {
    std::string fixed;
    text = validUtf8(text, fixed);
    cairo_set_font_face(cr, isBold ? fonts->bold() : fonts->regular());
    cairo_set_font_size(cr, size);
    cairo_text_extents_t extents;
    cairo_text_extents(cr, text, &extents);
    return extents.x_advance;
}

double Pen::text(double x, double y, const char* text, double size, Color c, bool isBold,
                 bool alignRight) const {
    std::string fixed;
    text = validUtf8(text, fixed);
    color(c);
    if (alignRight) {
        const double width = measure(text, size, isBold);
        cairo_move_to(cr, x - width, y);
        cairo_show_text(cr, text);
        return width;
    }
    // 左揃えは先に幅を測らず、描いたあとの現在位置から幅を出す（文字の組み立てを 1 回で済ませる）
    cairo_set_font_face(cr, isBold ? fonts->bold() : fonts->regular());
    cairo_set_font_size(cr, size);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);
    double endX = x;
    double endY = y;
    cairo_get_current_point(cr, &endX, &endY);
    return endX - x;
}

void Pen::roundedRect(double x, double y, double w, double h, double r) const {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void Pen::dot(double x, double y, Color c) const {
    color(c);
    cairo_arc(cr, x, y, 4.0, 0, 2 * M_PI);
    cairo_fill(cr);
}

double fitSize(const Pen& pen, const char* text, double size, double minSize, double maxWidth, bool bold) {
    while (size > minSize && pen.measure(text, size, bold) > maxWidth) size -= 1;
    return size;
}

size_t utf8Prefix(const std::string& text, size_t maxChars) {
    size_t chars = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        // A character starts at every byte that isn't a continuation byte (10xxxxxx)
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            if (chars == maxChars) return i;
            ++chars;
        }
    }
    return text.size();
}

namespace {

/** @return the length in bytes of the UTF-8 character at text[i] */
size_t utf8Length(const std::string& text, size_t i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    const size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    return std::min(n, text.size() - i);
}

/** @return true if text[i] starts a character that mustn't begin a line (Japanese closing punctuation) */
bool closesLine(const std::string& text, size_t i) {
    for (const char* mark : {"、", "。", "）", "」", "』", "・", "ー", ")", ",", "."}) {
        if (text.compare(i, std::strlen(mark), mark) == 0) return true;
    }
    return false;
}

}  // namespace

std::vector<std::string> wrapText(const Pen& pen, const std::string& text, double size, double maxWidth, bool bold) {
    const auto fits = [&](const std::string& line) { return pen.measure(line, size, bold) <= maxWidth; };
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = std::min(text.find('\n', start), text.size());
        const std::string paragraph = text.substr(start, end - start);
        std::string line;
        size_t pos = 0;
        while (pos < paragraph.size()) {
            const size_t space = std::min(paragraph.find(' ', pos), paragraph.size());
            std::string rest = paragraph.substr(pos, space - pos);
            pos = space + 1;
            const std::string next = line.empty() ? rest : line + " " + rest;
            if (fits(next)) {
                line = next;
                continue;
            }
            // Japanese (kana and kanji, U+3000-U+9FFF: no spaces between words) fills the line; other words move
            const unsigned char first = static_cast<unsigned char>(rest.empty() ? 0 : rest[0]);
            const bool cjk = first >= 0xE3 && first <= 0xE9;
            if (!line.empty() && fits(rest) && !cjk) {
                lines.push_back(line);
                line = rest;
                continue;
            }
            // Japanese, or too wide even on a line of its own: as many characters as fit, then the next line
            while (!rest.empty()) {
                const std::string lead = line.empty() ? std::string() : line + " ";
                size_t i = 0;
                while (i < rest.size() && fits(lead + rest.substr(0, i + utf8Length(rest, i)))) {
                    i += utf8Length(rest, i);
                }
                if (i == rest.size()) {
                    line = lead + rest;
                    break;
                }
                if (i == 0 && !line.empty()) {
                    lines.push_back(line);
                    line.clear();
                    continue;
                }
                if (i == 0) i = utf8Length(rest, 0);  // not even one character fits: it goes alone
                while (i < rest.size() && closesLine(rest, i)) i += utf8Length(rest, i);
                lines.push_back(lead + rest.substr(0, i));
                line.clear();
                rest = rest.substr(i);
            }
        }
        lines.push_back(line);
        start = end + 1;
    }
    return lines;
}


std::string ellipsize(const Pen& pen, const std::string& text, double size, double maxWidth, bool bold) {
    const size_t limit = utf8Prefix(text, kEllipsizeMaxChars);
    if (limit == text.size() && pen.measure(text, size, bold) <= maxWidth) return text;
    // Character starts within the first kEllipsizeMaxChars characters: the candidate cuts
    std::vector<size_t> starts;
    starts.reserve(kEllipsizeMaxChars + 1);
    for (size_t i = 0; i < limit; ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) starts.push_back(i);
    }
    starts.push_back(limit);
    // The longest prefix (k characters) that still fits with "…": fits(k) is monotonic in k
    const auto fits = [&](size_t k) { return pen.measure(text.substr(0, starts[k]) + "…", size, bold) <= maxWidth; };
    size_t lo = 0, hi = starts.size() - 1;  // fits(lo) is assumed (nothing + "…"); find the largest that fits
    if (limit < text.size() && fits(hi)) return text.substr(0, starts[hi]) + "…";
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (fits(mid)) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return text.substr(0, starts[lo]) + "…";
}

double centerBaseline(double top, double h, double size) {
    return top + h / 2 + size * 0.36;
}

void textCentered(const Pen& pen, double cx, double baseline, const char* text, double size, Color c, bool bold) {
    pen.text(cx - pen.measure(text, size, bold) / 2, baseline, text, size, c, bold);
}

void drawCard(const Pen& pen, double x, double y, double w, double h, double r, Color fill, Color border,
              double borderWidth) {
    cairo_t* cr = pen.cr;
    // 影（下にずらした黒を 2 段重ねる）
    cairo_set_source_rgba(cr, 0, 0, 0, 0.22);
    pen.roundedRect(x - 2, y + 5, w + 4, h + 4, r + 2);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.30);
    pen.roundedRect(x, y + 2, w, h + 1, r);
    cairo_fill(cr);
    pen.color(fill);
    pen.roundedRect(x, y, w, h, r);
    cairo_fill(cr);
    if (borderWidth > 0) strokeRounded(pen, x, y, w, h, r, border, borderWidth);
    // 内側のハイライト（上の縁だけ少し明るく）
    cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
    cairo_set_line_width(cr, 1);
    const double inset = borderWidth + 0.5;
    cairo_move_to(cr, x + r, y + inset);
    cairo_line_to(cr, x + w - r, y + inset);
    cairo_stroke(cr);
}

void strokeRounded(const Pen& pen, double x, double y, double w, double h, double r, Color c, double width) {
    pen.color(c);
    cairo_set_line_width(pen.cr, width);
    pen.roundedRect(x + width / 2, y + width / 2, w - width, h - width, std::max(0.0, r - width / 2));
    cairo_stroke(pen.cr);
}

void drawCheck(cairo_t* cr, double cx, double cy, double s, Color c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_set_line_width(cr, s * 0.16);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, cx - s * 0.36, cy + s * 0.02);
    cairo_line_to(cr, cx - s * 0.10, cy + s * 0.28);
    cairo_line_to(cr, cx + s * 0.38, cy - s * 0.26);
    cairo_stroke(cr);
}

void drawDisc(cairo_t* cr, double cx, double cy, double r, Color c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_fill(cr);
}

double pillPartLeft(double x, double w, size_t n, size_t i, double* hitLeft, double* hitRight) {
    const double part = (w - kPillInset * 2) / static_cast<double>(n);
    const double left = x + kPillInset + part * static_cast<double>(i);
    if (hitLeft != nullptr) *hitLeft = i == 0 ? x : left;
    if (hitRight != nullptr) *hitRight = i + 1 == n ? x + w : left + part;
    return left;
}

void drawSegmentedPill(const Pen& pen, double x, double y, double w, double h, const std::vector<PillPart>& parts,
                       int selected, bool usable, double size) {
    if (parts.empty()) return;
    cairo_t* cr = pen.cr;
    const double r = h / 2;
    // 地のピル。押せるときは枠（3:1 以上）で部品の形を見せる。押せないときは枠なし
    pen.color(kControl);
    pen.roundedRect(x, y, w, h, r);
    cairo_fill(cr);
    if (usable) strokeRounded(pen, x, y, w, h, r, kBorder, 2);

    const double inset = kPillInset;
    const double part = (w - inset * 2) / static_cast<double>(parts.size());
    for (size_t i = 0; i < parts.size(); ++i) {
        const double sx = pillPartLeft(x, w, parts.size(), i);
        const double sy = y + inset;
        const double sh = h - inset * 2;
        const bool pressed = usable && parts[i].pressed;
        const bool hovered = usable && parts[i].hovered;
        const bool isSelected = selected == static_cast<int>(i);
        if (isSelected) {
            // 選択中の部分: アクセントの塗り（押している間は少し濃く）＋ ✓ ＋ 太字。押せないときは薄く残す
            pen.color(pressed ? kAccentPressed : kAccent, usable ? 1.0 : 0.35);
            pen.roundedRect(sx, sy, part, sh, sh / 2);
            cairo_fill(cr);
        } else if (pressed || hovered) {
            pen.color(kControlHover);
            pen.roundedRect(sx, sy, part, sh, sh / 2);
            cairo_fill(cr);
        }
        const std::string& label = parts[i].label;
        const Color textColor = !usable ? kTextDisabled : (isSelected ? kOnAccent : kText);
        const double checkW = isSelected ? size * 0.9 : 0;
        const double labelSize = fitSize(pen, label, size, size * 0.7, part - 20 - checkW, isSelected);
        const double textW = pen.measure(label, labelSize, isSelected) + checkW;
        const double tx = sx + (part - textW) / 2;
        if (isSelected) drawCheck(cr, tx + checkW * 0.4, sy + sh / 2, size * 0.72, usable ? kOnAccent : kTextDisabled);
        pen.text(tx + checkW, centerBaseline(sy, sh, labelSize), label, labelSize, textColor, isSelected);
    }
}

void drawSlider(const Pen& pen, double trackX, double trackW, double cy, double fraction, Color fill, bool usable,
                bool active, bool ring) {
    cairo_t* cr = pen.cr;
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, 10);
    pen.color(usable ? kBorder : kControlHover);
    cairo_move_to(cr, trackX, cy);
    cairo_line_to(cr, trackX + trackW, cy);
    cairo_stroke(cr);
    fraction = fraction < 0 ? 0 : (fraction > 1 ? 1 : fraction);
    const double knobX = trackX + fraction * trackW;
    if (usable) {
        pen.color(fill);
        cairo_move_to(cr, trackX, cy);
        cairo_line_to(cr, knobX, cy);
        cairo_stroke(cr);
    }
    const double radius = active ? 18 : 15;
    drawDisc(cr, knobX, cy, radius, usable ? fill : kTextDisabled);
    if (usable && ring) {
        // A ring shows which knob the laser holds or would grab
        pen.color(kText);
        cairo_set_line_width(cr, 2.5);
        cairo_new_sub_path(cr);
        cairo_arc(cr, knobX, cy, radius, 0, 2 * M_PI);
        cairo_stroke(cr);
    }
}

double sliderFraction(double x, double trackX, double trackW) {
    if (!(trackW > 0)) return 0;
    const double f = (x - trackX) / trackW;
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

void surfaceToRgba(cairo_surface_t* surface, std::vector<uint8_t>& out) {
    // 乗算済みを戻す計算（c * 255 / a）を前もって表にしておく（割り算を画素ごとにしない）。
    // 表は初回にメモリ上で作るので、実行ファイルには 64 KiB の表を持たない
    static const auto table = [] {
        std::vector<uint8_t> values(256 * 256);
        for (unsigned a = 1; a < 256; ++a)
            for (unsigned c = 0; c < 256; ++c)
                values[a * 256 + c] = static_cast<uint8_t>(std::min(255u, (c * 255 + a / 2) / a));
        return values;
    }();

    cairo_surface_flush(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const uint8_t* data = cairo_image_surface_get_data(surface);
    out.resize(static_cast<size_t>(width) * height * 4);
    uint8_t* dst = out.data();
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(data + static_cast<size_t>(y) * stride);
        int x = 0;
        // cairo は乗算済みの ARGB（ネイティブエンディアンの 32bit）なので、戻して RGBA に並べ替える。
        // 透明度は 4 画素まとめて見る（全部不透明・全部透明なら表を引かない。半透明の画像でも遅くしない）
        for (; x <= width - 4; x += 4) {
            const uint32_t p0 = row[x], p1 = row[x + 1], p2 = row[x + 2], p3 = row[x + 3];
            if (((p0 & p1 & p2 & p3) >> 24) == 255) {
                dst[0] = static_cast<uint8_t>(p0 >> 16);
                dst[1] = static_cast<uint8_t>(p0 >> 8);
                dst[2] = static_cast<uint8_t>(p0);
                dst[3] = 255;
                dst[4] = static_cast<uint8_t>(p1 >> 16);
                dst[5] = static_cast<uint8_t>(p1 >> 8);
                dst[6] = static_cast<uint8_t>(p1);
                dst[7] = 255;
                dst[8] = static_cast<uint8_t>(p2 >> 16);
                dst[9] = static_cast<uint8_t>(p2 >> 8);
                dst[10] = static_cast<uint8_t>(p2);
                dst[11] = 255;
                dst[12] = static_cast<uint8_t>(p3 >> 16);
                dst[13] = static_cast<uint8_t>(p3 >> 8);
                dst[14] = static_cast<uint8_t>(p3);
                dst[15] = 255;
            } else if (((p0 | p1 | p2 | p3) >> 24) == 0) {
                std::fill_n(dst, 16, uint8_t{0});
            } else {
                const uint8_t* line0 = table.data() + (p0 >> 24) * 256;
                dst[0] = line0[(p0 >> 16) & 255];
                dst[1] = line0[(p0 >> 8) & 255];
                dst[2] = line0[p0 & 255];
                dst[3] = static_cast<uint8_t>(p0 >> 24);
                const uint8_t* line1 = table.data() + (p1 >> 24) * 256;
                dst[4] = line1[(p1 >> 16) & 255];
                dst[5] = line1[(p1 >> 8) & 255];
                dst[6] = line1[p1 & 255];
                dst[7] = static_cast<uint8_t>(p1 >> 24);
                const uint8_t* line2 = table.data() + (p2 >> 24) * 256;
                dst[8] = line2[(p2 >> 16) & 255];
                dst[9] = line2[(p2 >> 8) & 255];
                dst[10] = line2[p2 & 255];
                dst[11] = static_cast<uint8_t>(p2 >> 24);
                const uint8_t* line3 = table.data() + (p3 >> 24) * 256;
                dst[12] = line3[(p3 >> 16) & 255];
                dst[13] = line3[(p3 >> 8) & 255];
                dst[14] = line3[p3 & 255];
                dst[15] = static_cast<uint8_t>(p3 >> 24);
            }
            dst += 16;
        }
        for (; x < width; ++x) {
            const uint32_t p = row[x], a = p >> 24;
            const uint8_t* line = table.data() + a * 256;
            dst[0] = line[(p >> 16) & 255];
            dst[1] = line[(p >> 8) & 255];
            dst[2] = line[p & 255];
            dst[3] = static_cast<uint8_t>(a);
            dst += 4;
        }
    }
}
