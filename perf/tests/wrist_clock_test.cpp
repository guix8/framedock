// 手首への固定・時計・設定パネルのテスト（CTest から動かす。SteamVR は要らない）。
// 計算（位置と向き・フェード・やり直し）、時計の文字列、設定ファイル、設定パネルのボタンの当たり判定と見た目を確かめる。
#include "clock.h"
#include "config.h"
#include "draw.h"
#include "grid_renderer.h"
#include "placement.h"
#include "settings_panel.h"
#include "theme.h"

#include <cairo.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

/** 条件が偽なら、式と行番号を持った例外を投げる。 */
#define CHECK(expr)                                                                                  \
    do {                                                                                             \
        if (!(expr)) throw std::runtime_error(std::string(#expr) + " (line " + std::to_string(__LINE__) + ")"); \
    } while (false)

/**
 * 2 つの数がほぼ同じか確かめる。
 * @param actual 実際の値
 * @param expected 期待する値
 * @param epsilon 許す差
 * @param line 呼んだ行（失敗したときの表示用）
 */
void nearAt(double actual, double expected, double epsilon, int line) {
    if (std::fabs(actual - expected) < epsilon) return;
    char text[160];
    std::snprintf(text, sizeof(text), "near: %.6f != %.6f (line %d)", actual, expected, line);
    throw std::runtime_error(text);
}
#define NEAR(actual, expected) nearAt((actual), (expected), 1e-4, __LINE__)

/**
 * 頭のパネルと同じ回転（panelRotation）と位置から、テスト用の変換を作る。
 * @param yaw 左右（度）
 * @param pitch 上下（度）
 * @param roll 回転（度）
 * @param x 位置 x（m）
 * @param y 位置 y（m）
 * @param z 位置 z（m）
 * @return 変換
 */
vr::HmdMatrix34_t makeTransform(double yaw, double pitch, double roll, double x, double y, double z) {
    double r[3][3];
    panelRotation(yaw, pitch, roll, r);
    vr::HmdMatrix34_t m {};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) m.m[row][col] = static_cast<float>(r[row][col]);
    }
    m.m[0][3] = static_cast<float>(x);
    m.m[1][3] = static_cast<float>(y);
    m.m[2][3] = static_cast<float>(z);
    return m;
}

/** 位置と向きの計算・フェード・トラッキングの確かめ。 */
void geometry() {
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] {};
    CHECK(!wristTrackingValid(vr::k_unTrackedDeviceIndexInvalid, poses));
    CHECK(!wristTrackingValid(1, poses));
    poses[0].bDeviceIsConnected = poses[0].bPoseIsValid = true;
    poses[1].bDeviceIsConnected = poses[1].bPoseIsValid = true;
    CHECK(wristTrackingValid(1, poses));
    poses[1].bPoseIsValid = false;
    CHECK(!wristTrackingValid(1, poses));
    poses[1].bPoseIsValid = true;
    poses[0].bDeviceIsConnected = false;
    CHECK(!wristTrackingValid(1, poses));

    // 面が頭を向いていれば 1、45°（75 − 30）までは 1、60° で半分、75° 以上で 0
    const auto head = makeTransform(0, 0, 0, 0, 0, 0);
    NEAR(wristFacingAlpha(makeTransform(0, 0, 0, 0, 0, -0.5), head, 75), 1);
    NEAR(wristFacingAlpha(makeTransform(45, 0, 0, 0, 0, -0.5), head, 75), 1);
    NEAR(wristFacingAlpha(makeTransform(60, 0, 0, 0, 0, -0.5), head, 75), 0.5);
    NEAR(wristFacingAlpha(makeTransform(90, 0, 0, 0, 0, -0.5), head, 75), 0);
    NEAR(wristFacingAlpha(makeTransform(180, 0, 0, 0, 0, -0.5), head, 75), 0);
    NEAR(wristFacingAlpha(head, head, 75), 0);  // 頭と重なっている
    // 全体を動かしても（コントローラーと頭が一緒に動いても）同じ
    const auto world = makeTransform(41, 17, -22, 2, 3, 4);
    NEAR(wristFacingAlpha(composeTransform(world, makeTransform(60, 0, 0, 0, 0, -0.5)), composeTransform(world, head), 75),
         0.5);

    // 手首の既定（pitch −90）: 面がコントローラーの +y、パネルの上側がコントローラーの −z、右側が +x
    const auto wrist = wristTransform(WristPose());
    NEAR(wrist.m[0][2], 0);
    NEAR(wrist.m[1][2], 1);
    NEAR(wrist.m[2][2], 0);
    NEAR(wrist.m[2][1], -1);
    NEAR(wrist.m[0][0], 1);
    NEAR(wrist.m[0][3], 0);
    NEAR(wrist.m[1][3], 0.05);
    NEAR(wrist.m[2][3], 0.08);
    // 手首の pitch −180 で、面がコントローラーの +z を向き、上側が +y
    WristPose standing;
    standing.pitch = -180;
    const auto stand = wristTransform(standing);
    NEAR(stand.m[2][2], 1);
    NEAR(stand.m[1][1], 1);

    // どんな向きでも、手首の回転は「寝かせた向き（pitch −90）を基準にした、頭のパネルと同じ回転」そのもの:
    // wristTransform(yaw, pitch, roll) = 基準 · panelRotation(yaw, pitch + 90, roll)
    const auto base = wristTransform(WristPose());
    const double samples[][3] = {{-30, -70, 12}, {140, -165, -80}, {5, -10, 179}, {-90, -90, 45}};
    for (const auto& sample : samples) {
        WristPose pose;
        pose.x = pose.y = pose.z = 0;
        pose.yaw = sample[0];
        pose.pitch = sample[1];
        pose.roll = sample[2];
        const auto wristRot = wristTransform(pose);
        const auto headRot = makeTransform(sample[0], sample[1] + 90, sample[2], 0, 0, 0);
        const auto expected = composeTransform(base, headRot);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) NEAR(wristRot.m[row][col], expected.m[row][col]);
        }
    }

    // フェードは時間でなめらかに（呼ぶ間隔によらない）
    double alpha = 0;
    for (int i = 0; i < 60; ++i) {
        const double next = smoothWristAlpha(alpha, 1, 1.0 / 60);
        CHECK(next >= alpha && next <= 1);
        alpha = next;
    }
    NEAR(alpha, smoothWristAlpha(0, 1, 1));
    CHECK(smoothWristAlpha(1, 0, 0.2) < 0.1);
}

/** CPU を抑えるための確かめる間隔と、粗い間隔のあとでもいきなり変わらないこと。 */
void pollScheduling() {
    NEAR(wristPollInterval(1, 1), kWristPollSteadySec);      // 見えきっている
    NEAR(wristPollInterval(0, 0), kWristPollSteadySec);      // 消えきっている
    NEAR(wristPollInterval(0.3, 1), kWristPollFadeSec);      // フェードの途中
    NEAR(wristPollInterval(0.6, 0.6), kWristPollSteadySec);  // held still inside the fade band: arrived, steady rate
    NEAR(wristPollInterval(0.6, 0.615), kWristPollFadeSec);   // still on its way
    // The alpha sent to SetOverlayAlpha moves in steps (no 30 tiny changes a second)
    NEAR(quantizeAlpha(0.0), 0.0);
    NEAR(quantizeAlpha(1.0), 1.0);
    NEAR(quantizeAlpha(-0.1), 0.0);
    NEAR(quantizeAlpha(0.503), 0.5);
    NEAR(quantizeAlpha(0.511), 0.52);
    CHECK(quantizeAlpha(0.6001) == quantizeAlpha(0.6049));
    CHECK(kWristPollFadeSec >= 1.0 / 30.0 - 1e-9);           // 細かいときでも 30Hz まで
    CHECK(kWristPollSteadySec <= 0.1 + 1e-9);                // トラッキングが外れたら 0.1 秒以内に気づく

    WristFadeState fade;
    fade.update(true, true, 1, 1, 10.0);
    for (int i = 1; i <= 60; ++i) fade.update(true, true, 1, 1, 10.0 + i * kWristPollFadeSec);
    CHECK(fade.alpha() > 0.99);
    // 見えきって 0.1 秒おきになったところで急に目標が 0 になっても、1 回で 1/30 秒ぶんしか進めない
    const double t = 10.0 + 60 * kWristPollFadeSec + kWristPollSteadySec;
    const double first = fade.update(true, true, 1, 0, t);
    CHECK(first > 0.6 && first < 0.7);  // 0.1 秒ぶん進めると 0.29 まで落ちてしまう
    // トラッキングが外れたら、フェードを待たずにすぐ 0
    NEAR(fade.update(true, false, 1, 1, t + 0.1), 0);
}

/** 断られた操作のやり直しと、固定先を変えたとき・戻ったときのフェードの始まり。 */
void placementRecovery() {
    WristFadeState fade;
    NEAR(fade.update(false, true, 0, 1, 1), 1);    // 頭はすぐ 1
    NEAR(fade.update(true, true, 1, 1, 61), 0);    // 手首に替えたら 0 から（60 秒前からの時間で飛ばない）
    const double firstStep = fade.update(true, true, 1, 1, 61 + 1.0 / 60);
    CHECK(firstStep > 0 && firstStep < 0.25);
    NEAR(fade.update(true, true, 2, 1, 100), 0);   // しばらくしてから別の手に替えた
    CHECK(fade.update(true, true, 2, 1, 100.1) > 0);
    NEAR(fade.update(true, false, 2, 1, 101), 0);  // トラッキングが外れた・表示オフ
    NEAR(fade.update(true, true, 2, 1, 150), 0);   // 戻ったときも 0 から
    CHECK(fade.update(true, true, 2, 1, 150.1) > 0);
    NEAR(fade.update(false, true, 0, 1, 151), 1);  // 頭はすぐ

    /** OpenVR の代わり（断る回数を決められる）。 */
    struct FakeOverlay {
        double alpha = 0;
        bool shown = false;
        int failAlpha = 0, failShow = 0, failHide = 0;
        int alphaCalls = 0, showCalls = 0, hideCalls = 0;
        /**
         * 透明度を受け取る。
         * @param value 透明度
         * @return 受け付けたら true
         */
        bool setAlpha(double value) {
            ++alphaCalls;
            if (failAlpha > 0) {
                --failAlpha;
                return false;
            }
            alpha = value;
            return true;
        }
        /**
         * 表示・非表示を受け取る。
         * @param show 表示なら true
         * @return 受け付けたら true
         */
        bool setVisible(bool show) {
            int& failures = show ? failShow : failHide;
            if (show) ++showCalls; else ++hideCalls;
            if (failures > 0) {
                --failures;
                return false;
            }
            shown = show;
            return true;
        }
    } backend;
    PanelPresentationState state;
    /**
     * 透明度を反映する。
     */
    const auto apply = [&](double alpha) {
        return state.apply(alpha, [&](double value) { return backend.setAlpha(value); },
                           [&](bool show) { return backend.setVisible(show); });
    };
    backend.failShow = 1;
    CHECK(apply(0.9));
    CHECK(!backend.shown);
    CHECK(!apply(0.9));
    CHECK(backend.shown && backend.showCalls == 2);
    for (int i = 0; i < 120; ++i) CHECK(!apply(0.9));
    CHECK(backend.showCalls == 2 && backend.alphaCalls == 1);  // うまくいったものは何度も渡さない

    backend.failHide = 1;
    CHECK(apply(0));
    CHECK(backend.shown);
    NEAR(backend.alpha, 0);
    CHECK(!apply(0));
    CHECK(!backend.shown && backend.hideCalls == 2);

    backend.failAlpha = 1;
    CHECK(apply(0.9));
    CHECK(!backend.shown);  // 透明度を断られたら、古い透明度のまま出さない
    CHECK(!apply(0.9));
    CHECK(backend.shown);
    NEAR(backend.alpha, 0.9);

    backend.failAlpha = 1;
    CHECK(apply(0));
    CHECK(!backend.shown);  // 透明度 0 を断られても、隠すのは試す
    CHECK(!apply(0));
    NEAR(backend.alpha, 0);

    backend.failShow = 100;
    for (int i = 0; i < 100; ++i) CHECK(apply(0.9));  // 断られ続けても覚えた状態は壊れない
    CHECK(!backend.shown);
    CHECK(!apply(0.9));
    CHECK(backend.shown);
    backend.failAlpha = 1;
    CHECK(apply(0.4));
    NEAR(backend.alpha, 0.9);
    CHECK(!apply(0.9));  // 目標が、最後に受け付けられた値に戻った
    NEAR(backend.alpha, 0.9);
}

/** 時計の文字列。 */
void clockCases() {
    std::tm t {};
    CHECK(formatClock(t, 12) == "12:00 AM");
    CHECK(formatClock(t, 24) == "00:00");
    t.tm_hour = 12;
    t.tm_min = 5;
    CHECK(formatClock(t, 12) == "12:05 PM");
    t.tm_hour = 23;
    t.tm_min = 59;
    CHECK(formatClock(t, 12) == "11:59 PM");
    CHECK(formatClock(t, 24) == "23:59");
    CHECK(formatClock(t, 0).empty());
}

/**
 * 設定の操作と、設定ファイルの読み書き。
 * @param file 一時的な設定ファイルのパス
 */
void configuration(const std::string& file) {
    Config c;
    std::vector<std::string> warnings;
    std::string error;
    {
        std::ofstream out(file);
        out << R"({"visible":false,"position":{"x":0.2,"y":-0.1,"z":-0.7}})";
    }
    CHECK(loadConfig(file, c, warnings, error));
    CHECK(c.attachment == Attachment::Head && !c.visible);
    NEAR(c.posX, 0.2);
    CHECK(c.clockFormat == 24 && c.wristFade);
    NEAR(c.wristFadeEndDeg, 75);

    const WristPose standard;
    CHECK(!(c.leftWrist != standard) && !(c.rightWrist != standard));

    // 손목 값은 손마다 따로 (레이저로 옮긴 자리): 붙는 곳을 바꿔도 다른 손의 값은 그대로
    CHECK(applySettingsAction(SettingsAction::AttachLeft, c));
    c.leftWrist.x = 0.01;
    c.leftWrist.pitch = -91;
    const WristPose left = c.leftWrist;
    CHECK(applySettingsAction(SettingsAction::AttachRight, c));
    c.rightWrist.z = 0.09;
    c.rightWrist.yaw = 5;
    CHECK(!(c.leftWrist != left));

    // 手首の標準の位置: 選んでいる手だけ既定に戻す
    CHECK(applySettingsAction(SettingsAction::PresetWrist, c));
    CHECK(!(c.rightWrist != standard));
    CHECK(!(c.leftWrist != left));
    CHECK(!applySettingsAction(SettingsAction::PresetWrist, c));  // もう標準なので変わらない

    // 保存して読み直す（新しいキーも含めて、知らないキーの警告なし）
    c.updateCheck = false;
    c.clockFormat = 12;
    c.wristFade = false;
    c.wristFadeEndDeg = 80;
    c.pitchDeg = 5;  // the head's facing (the laser sets it) round-trips too
    c.yawDeg = 5;
    c.rollDeg = 1;
    CHECK(saveConfig(file, c, error));
    Config roundTrip;
    warnings.clear();
    CHECK(loadConfig(file, roundTrip, warnings, error));
    CHECK(warnings.empty());
    CHECK(roundTrip.attachment == Attachment::RightWrist && roundTrip.clockFormat == 12);
    CHECK(!(roundTrip.leftWrist != c.leftWrist) && !(roundTrip.rightWrist != c.rightWrist));
    CHECK(!roundTrip.wristFade);
    NEAR(roundTrip.wristFadeEndDeg, 80);
    CHECK(!roundTrip.updateCheck);
    NEAR(roundTrip.pitchDeg, 5);
    NEAR(roundTrip.yawDeg, 5);
    NEAR(roundTrip.rollDeg, 1);
    NEAR(roundTrip.posZ, c.posZ);

    // 範囲外・型違いは警告して丸める（手首の pitch は −180〜0）
    {
        std::ofstream out(file);
        out << R"({"attachment":"bad","clock_format":12.5,"left_wrist":{"x":9,"pitch":-300},)"
            << R"("right_wrist":{"pitch":20,"roll":400,"w":1},"wrist_fade_end_deg":5})";
    }
    warnings.clear();
    CHECK(loadConfig(file, c, warnings, error));
    CHECK(warnings.size() >= 7);
    CHECK(c.attachment == Attachment::Head && c.clockFormat == 24);
    NEAR(c.leftWrist.x, 0.5);
    NEAR(c.leftWrist.pitch, -180);
    NEAR(c.rightWrist.pitch, 0);
    NEAR(c.rightWrist.roll, 180);
    NEAR(c.wristFadeEndDeg, 35);

    // 手首の位置は ±50cm、pitch は −180〜0、消える角度は 35〜90° で止まる
    CHECK(applySettingsAction(SettingsAction::AttachLeft, c));
    NEAR(c.leftWrist.x, 0.5);
    NEAR(c.leftWrist.pitch, -180);
    CHECK(applySettingsAction(SettingsAction::AttachRight, c));
    NEAR(c.rightWrist.pitch, 0);
    CHECK(!applySettingsAction(SettingsAction::FadeAngleDown, c));
    c.wristFadeEndDeg = 90;
    CHECK(!applySettingsAction(SettingsAction::FadeAngleUp, c));
    CHECK(applySettingsAction(SettingsAction::FadeAngleDown, c));
    NEAR(c.wristFadeEndDeg, 85);
    CHECK(!applySettingsAction(SettingsAction::Clock24, c));  // もう 24h
    CHECK(applySettingsAction(SettingsAction::Clock12, c) && c.clockFormat == 12);
    CHECK(applySettingsAction(SettingsAction::ClockOff, c) && c.clockFormat == 0);

    // 壊れた JSON は前の設定のまま
    {
        std::ofstream out(file);
        out << "{invalid";
    }
    const WristPose before = c.leftWrist;
    CHECK(!loadConfig(file, c, warnings, error));
    CHECK(!(before != c.leftWrist));

    // 既定に戻す: 固定先・両手・傾けると消す・時計も戻す
    resetDisplaySettings(c);
    // The attachment goes back to where a fresh install puts it (the left wrist), not Config's head default
    CHECK(c.attachment == kFreshAttachment && c.attachment == Attachment::LeftWrist && c.clockFormat == 24 && c.wristFade);
    CHECK(!(c.leftWrist != standard) && !(c.rightWrist != standard));
    NEAR(c.pitchDeg, 0);
    NEAR(c.yawDeg, 0);
    NEAR(c.rollDeg, 0);
}

/**
 * 性能パネルの高さ: 時計を出すと General の時計の行ぶん高く、出さない（0）と 1 行（22 px）詰まる。
 * 既定のレイアウトは 1 列なので、幅は 512 のまま。
 * @param directory PNG を書く一時フォルダ
 */
void panelHeights(const std::string& directory) {
    Config c;
    FontSet fonts;
    fonts.load(c.fontPath, c.boldFontPath);
    frame_perf_overlay::GridRenderer renderer(fonts);
    // 時計の行の分だけ高さが変わるのを確かめるので、既定のウィジェットを全部 1 列に並べて描く
    frame_perf_overlay::Layout oneColumn = c.layout;
    for (size_t i = 1; i < oneColumn.columns.size(); ++i) {
        for (const auto& w : oneColumn.columns[i].widgets) oneColumn.columns[0].widgets.push_back(w);
    }
    oneColumn.columns.resize(1);
    renderer.setLayout(oneColumn);
    frame_perf_overlay::MetricStore store;
    int withClock = 0;
    for (const int format : {24, 12, 0}) {
        frame_perf_overlay::WidgetContext ctx;
        ctx.store = &store;
        ctx.now = 1.0;
        ctx.clockFormat = format;
        renderer.updateGeometry(ctx);
        renderer.render(ctx);
        const int width = renderer.geometry().textureWidth;
        const int height = renderer.geometry().textureHeight;
        if (format == 24) withClock = height;
        const int expected = format != 0 ? withClock : withClock - 22;
        CHECK(width == 512 && height == expected);
        CHECK(renderer.toRgba().size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        // PNG は見える部分の高さで書き出す
        const std::string path = directory + "/panel-" + std::to_string(format) + ".png";
        CHECK(renderer.writePng(path));
        cairo_surface_t* png = cairo_image_surface_create_from_png(path.c_str());
        CHECK(cairo_image_surface_get_width(png) == 512 && cairo_image_surface_get_height(png) == expected);
        cairo_surface_destroy(png);
    }
}

/** ボタン 1 つの期待する場所（設定パネルの画像の px）。 */
struct ExpectedButton {
    SettingsAction action;
    double x, y, w, h;
};

/**
 * 画像の 1 画素の色（RGB）。
 * @param rgba 設定パネルの非乗算済み RGBA
 * @param width 画像の幅
 * @param x 左端からの px
 * @param y 上端からの px
 * @return 0xRRGGBB
 */
unsigned pixel(const std::vector<uint8_t>& rgba, int width, double x, double y) {
    const size_t i = (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4;
    return (static_cast<unsigned>(rgba[i]) << 16) | (static_cast<unsigned>(rgba[i + 1]) << 8) | rgba[i + 2];
}

/**
 * 色を 0xRRGGBB にする。
 * @param c 色
 * @return 0xRRGGBB
 */
unsigned hex(Color c) {
    return (static_cast<unsigned>(std::lround(c.r * 255)) << 16) | (static_cast<unsigned>(std::lround(c.g * 255)) << 8) |
           static_cast<unsigned>(std::lround(c.b * 255));
}

/**
 * どのボタンも、真ん中を押すとその操作になり、その場所にボタンの形が描かれていて（地がカードの色でない）、
 * すぐ上はカードの地のまま（ボタンの外）であることを確かめる。当たり判定と見た目のずれを見つけるため。
 * @param panel 設定パネル（描いたあと）
 * @param buttons 期待するボタン
 * @param now 押した時刻
 */
void checkButtons(SettingsPanel& panel, const std::vector<ExpectedButton>& buttons, double now) {
    const std::vector<uint8_t> rgba = panel.toRgba();
    for (const auto& b : buttons) {
        const std::string where = " at (" + std::to_string(static_cast<int>(b.x)) + ", " + std::to_string(static_cast<int>(b.y)) + ")";
        // 見た目: 左の縁の少し内側はボタン（地の色かアクセント）、すぐ上と下はカードの地
        const unsigned inside = pixel(rgba, panel.width(), b.x + 3, b.y + b.h / 2);
        if (inside == hex(kCard) || inside == hex(kBg)) throw std::runtime_error("button not drawn" + where);
        if (pixel(rgba, panel.width(), b.x + b.w / 2, b.y - 4) != hex(kCard)) throw std::runtime_error("above not card" + where);
        if (pixel(rgba, panel.width(), b.x + b.w / 2, b.y + b.h + 4) != hex(kCard)) throw std::runtime_error("below not card" + where);
        // 当たり判定: 真ん中と、縁の少し内側。Reset only comes back on its second tap (the first arms it)
        const auto tap = [&](double x, double y) {
            const SettingsAction got = panel.pointerDown(x, y, now);
            return got == SettingsAction::None && b.action == SettingsAction::Reset ? panel.pointerDown(x, y, now) : got;
        };
        const SettingsAction centre = tap(b.x + b.w / 2, b.y + b.h / 2);
        if (centre != b.action) throw std::runtime_error("hit test" + where);
        if (tap(b.x + 2, b.y + 2) != centre) throw std::runtime_error("hit edge" + where);
        if (centre != SettingsAction::None && tap(b.x + b.w / 2, b.y - 3) == centre) {
            throw std::runtime_error("hit above" + where);
        }
    }
    panel.pointerLeave();
}

/**
 * 設定パネル: 別の画面は無く、固定先のセグメントと位置のボタン、手首のときだけ「手首で隠す」のボタン
 * （細かい位置と向きはレーザーで動かすので、十字・向きのボタンは無い）。
 * @param directory PNG を書く一時フォルダ
 */
void controls(const std::string& directory) {
    Config c;
    c.language = Language::En;
    FontSet fonts;
    fonts.load(c.fontPath, c.boldFontPath);
    SettingsPanel panel(fonts);
    const AutostartStatus service {AutostartStatus::State::Enabled, false, false};
    CHECK(panel.width() == 1600 && panel.height() <= 760);

    // どの固定先でも同じ場所にあるボタン
    const std::vector<ExpectedButton> common = {
        // パネル
        {SettingsAction::ShowOn, 168, 188, 130, 68}, {SettingsAction::ShowOff, 298, 188, 130, 68},
        {SettingsAction::SizeDown, 168, 267, 84, 68}, {SettingsAction::SizeUp, 372, 267, 84, 68},
        {SettingsAction::AlphaDown, 168, 346, 84, 68}, {SettingsAction::AlphaUp, 372, 346, 84, 68},
        {SettingsAction::ClockOff, 168, 425, 96, 68}, {SettingsAction::Clock12, 264, 425, 96, 68},
        {SettingsAction::Clock24, 360, 425, 96, 68}, {SettingsAction::Reset, 168, 504, 288, 68},
        // 位置: 固定先
        {SettingsAction::AttachHead, 630, 188, 410.0 / 3, 68},
        {SettingsAction::AttachLeft, 630 + 410.0 / 3, 188, 410.0 / 3, 68},
        {SettingsAction::AttachRight, 630 + 820.0 / 3, 188, 410.0 / 3, 68},
    };
    const std::vector<ExpectedButton> headOnly = {
        {SettingsAction::PresetLeftTop, 520, 267, 166, 68}, {SettingsAction::PresetRightTop, 874, 267, 166, 68},
        {SettingsAction::PresetLeftBottom, 520, 346, 166, 68}, {SettingsAction::PresetCenterBottom, 697, 346, 166, 68},
        {SettingsAction::PresetRightBottom, 874, 346, 166, 68},
    };
    const std::vector<ExpectedButton> wristOnly = {
        {SettingsAction::PresetWrist, 520, 267, 520, 68},
        {SettingsAction::FadeOn, 1292, 188, 130, 68}, {SettingsAction::FadeOff, 1422, 188, 130, 68},
        {SettingsAction::FadeAngleDown, 1264, 267, 84, 68}, {SettingsAction::FadeAngleUp, 1468, 267, 84, 68},
    };

    for (const Language language : {Language::En, Language::Ja}) {
        c = Config();
        c.language = language;
        const std::string code = language == Language::En ? "en" : "ja";
        // 頭
        panel.render(c, service, {});
        checkButtons(panel, common, 1);
        checkButtons(panel, headOnly, 1);
        CHECK(panel.pointerDown(780, 301, 1) == SettingsAction::None);   // 上の段の真ん中は空き
        CHECK(panel.pointerDown(1350, 222, 1) == SettingsAction::None);  // 頭のとき「手首で隠す」は押せない
        CHECK(panel.pointerDown(700, 459, 1) == SettingsAction::None);   // 位置の下は説明だけ
        panel.pointerLeave();
        panel.render(c, service, {});
        CHECK(panel.writePng(directory + "/head-" + code + ".png"));
        // 左手・右手: 位置のボタンが手首の標準の位置になり、「手首で隠す」が押せる
        for (const SettingsAction attach : {SettingsAction::AttachLeft, SettingsAction::AttachRight}) {
            CHECK(panel.pointerDown(attach == SettingsAction::AttachLeft ? 835 : 972, 222, 2) == attach);
            CHECK(applySettingsAction(attach, c));
            panel.pointerLeave();
            panel.render(c, service, {});
            checkButtons(panel, common, 3);
            checkButtons(panel, wristOnly, 3);
            CHECK(panel.pointerDown(1200, 459, 3) == SettingsAction::None);  // 説明だけ
            CHECK(panel.pointerDown(600, 380, 3) == SettingsAction::None);   // 頭の位置のボタンは無い
            panel.pointerLeave();
            panel.render(c, service, {});
            CHECK(panel.writePng(directory + "/" + (attach == SettingsAction::AttachLeft ? "left-" : "right-") + code + ".png"));
        }
        // 頭に戻すと、頭のボタンに戻る
        CHECK(applySettingsAction(SettingsAction::AttachHead, c));
        panel.render(c, service, {});
        checkButtons(panel, headOnly, 4);
    }

    // The header row (title and page switch) does not take the attachment, clock or facing buttons
    c.attachment = Attachment::LeftWrist;
    panel.render(c, service, {});
    CHECK(panel.pointerDown(960, 222, 6) == SettingsAction::AttachRight);
    CHECK(panel.pointerDown(400, 459, 6) == SettingsAction::Clock24);
    CHECK(panel.pointerDown(1340, 222, 6) == SettingsAction::FadeOn);
    panel.pointerLeave();
}

}  // namespace

/**
 * テストを全部動かす。
 * @return 0 = 全部通った、1 = 失敗、2 = 一時フォルダを作れない
 */
int main() {
    char directory[] = "/tmp/frame-overlay-test-XXXXXX";
    if (mkdtemp(directory) == nullptr) return 2;
    try {
        geometry();
        pollScheduling();
        placementRecovery();
        clockCases();
        configuration(std::string(directory) + "/config.json");
        panelHeights(directory);
        controls(directory);
        std::cout << "Geometry, rotation convention, panel-axis nudges, polling, placement recovery, clock, config, panel heights and UI controls passed.\n";
        std::filesystem::remove_all(directory);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << " (artifacts: " << directory << ")\n";
        return 1;
    }
    return 0;
}
