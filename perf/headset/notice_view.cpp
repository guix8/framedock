#include "notice_view.h"

#include "app_text.h"
#include "draw.h"
#include "theme.h"

#include <cairo.h>

#include <algorithm>

NoticeView::NoticeView(const FontSet& fonts) : fonts_(fonts) {
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kWidth, kHeight);
    cr_ = cairo_create(surface_);
}

NoticeView::~NoticeView() {
    if (cr_ != nullptr) cairo_destroy(cr_);
    if (surface_ != nullptr) cairo_surface_destroy(surface_);
}

void NoticeView::render(const std::string& text, Level level) {
    const Pen pen {cr_, &fonts_};
    const Color accent = level == Level::Danger ? kDanger : kWarn;
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr_);
    cairo_restore(cr_);
    const double r = kHeight / 2.0;
    pen.color(kCard, kBgAlpha);  // the game shows through
    pen.roundedRect(2, 2, kWidth - 4, kHeight - 4, r - 2);
    cairo_fill(cr_);
    strokeRounded(pen, 2, 2, kWidth - 4, kHeight - 4, r - 2, accent, 4);
    // A disc in the level's colour with "!" on it, then the line
    drawDisc(cr_, r, r, 26, accent);
    textCentered(pen, r, centerBaseline(r - 26, 52, 34), "!", 34, kOnAccent, true);
    const double left = r + 44;
    const double room = kWidth - left - 40;
    const double size = fitSize(pen, text, 36, 24, room, true);
    pen.text(left, centerBaseline(0, kHeight, size), ellipsize(pen, text, size, room, true), size, kText, true);
    cairo_surface_flush(surface_);
}

const uint8_t* NoticeView::pixels() const {
    return cairo_image_surface_get_data(surface_);
}

bool NoticeView::writePng(const std::string& path) const {
    return cairo_surface_write_to_png(surface_, path.c_str()) == CAIRO_STATUS_SUCCESS;
}

GuideView::GuideView(const FontSet& fonts) : fonts_(fonts) {
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kWidth, kHeight);
    cr_ = cairo_create(surface_);
}

GuideView::~GuideView() {
    if (cr_ != nullptr) cairo_destroy(cr_);
    if (surface_ != nullptr) cairo_surface_destroy(surface_);
}

void GuideView::render() {
    const Pen pen {cr_, &fonts_};
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr_);
    cairo_restore(cr_);
    // The approved mockup (lang_firstrun.py): the card, an accent border, the title, four rows, the footer
    pen.color(kCard, kBgAlpha);
    pen.roundedRect(0, 0, kWidth, kHeight, 36);
    cairo_fill(cr_);
    strokeRounded(pen, 2, 2, kWidth - 4, kHeight - 4, 34, kAccent, 4);
    const double left = 44, room = kWidth - 2 * left;
    const std::string title = tr(Tx::GuideTitle);
    pen.text(left, 70, ellipsize(pen, title, 38, room, true), 38, kText, true);
    const Tx rows[4][2] = {{Tx::GuideWristLabel, Tx::GuideWrist},
                           {Tx::GuidePressLabel, Tx::GuidePress},
                           {Tx::GuideMoveLabel, Tx::GuideMove},
                           {Tx::GuideSettingsLabel, Tx::GuideSettings}};
    double labelW = 0;
    for (const auto& row : rows) labelW = std::max(labelW, pen.measure(tr(row[0]), 26, true));
    labelW += 30;
    const double textX = 74 + labelW, textRoom = kWidth - 40 - textX;
    double y = 130;
    for (const auto& row : rows) {
        drawDisc(cr_, 54, y - 9, 6, kAccent);
        pen.text(74, y, tr(row[0]), 26, kAccentText, true);
        const std::string line = tr(row[1]);
        const double size = fitSize(pen, line, 26, 20, textRoom, false);
        pen.text(textX, y, ellipsize(pen, line, size, textRoom, false), size, kText);
        y += 54;
    }
    const std::string footer = tr(Tx::GuideFooter);
    const double footerSize = fitSize(pen, footer, 21, 16, room, false);
    pen.text(left, kHeight - 30, ellipsize(pen, footer, footerSize, room, false), footerSize, kTextMuted);
    cairo_surface_flush(surface_);
}

const uint8_t* GuideView::pixels() const {
    return cairo_image_surface_get_data(surface_);
}

bool GuideView::writePng(const std::string& path) const {
    return cairo_surface_write_to_png(surface_, path.c_str()) == CAIRO_STATUS_SUCCESS;
}
