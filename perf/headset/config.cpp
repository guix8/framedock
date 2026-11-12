// 設定の読み込みと変更の監視の実装。
#include "config.h"

#include "json.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <string_view>

namespace {

/** A number for config.json: never nan / inf (they aren't JSON; the file would be unreadable next start) */
double fin(double value) { return std::isfinite(value) ? value : 0.0; }

/**
 * オブジェクトのメンバーのうち、知っているキー以外を warnings に入れる。
 * @param object 調べるオブジェクト
 * @param known 知っているキーの一覧
 * @param prefix 表示用のキーの前置き（"thresholds." など）
 * @param warnings 追加先
 */
void warnUnknownKeys(const JsonValue& object, std::initializer_list<std::string_view> known, const std::string& prefix,
                     std::vector<std::string>& warnings) {
    for (const auto& member : object.members) {
        if (std::find(known.begin(), known.end(), member.first) == known.end()) {
            warnings.push_back("知らないキーです: " + prefix + member.first);
        }
    }
}

/**
 * 数値のキーを読んで範囲に収める。キーが無ければ何もしない。
 * @param object 読むオブジェクト
 * @param key キー
 * @param minValue 最小値
 * @param maxValue 最大値
 * @param target 書き込み先
 * @param warnings 型違い・範囲外のときの追加先
 */
void readNumber(const JsonValue& object, const char* key, double minValue, double maxValue, double& target,
                std::vector<std::string>& warnings) {
    const JsonValue* value = object.get(key);
    if (value == nullptr) return;
    if (!value->isNumber()) {
        warnings.push_back(std::string(key) + " は数値で書いてください");
        return;
    }
    const double clamped = std::clamp(value->number, minValue, maxValue);
    if (clamped != value->number) {
        char text[32];
        std::snprintf(text, sizeof(text), "%g", clamped);
        warnings.push_back(std::string(key) + " は範囲外なので " + text + " にしました");
    }
    target = clamped;
}

/**
 * 整数のキーを読んで範囲に収める。キーが無ければ何もしない。
 * @param object 読むオブジェクト
 * @param key キー
 * @param minValue 最小値
 * @param maxValue 最大値
 * @param target 書き込み先
 * @param warnings 型違い・範囲外のときの追加先
 */
void readInt(const JsonValue& object, const char* key, int minValue, int maxValue, int& target,
             std::vector<std::string>& warnings) {
    double value = target;
    readNumber(object, key, minValue, maxValue, value, warnings);
    target = static_cast<int>(value);
}

/**
 * 真偽値のキーを読む。キーが無ければ何もしない。
 * @param object 読むオブジェクト
 * @param key キー
 * @param target 書き込み先
 * @param warnings 型違いのときの追加先
 */
void readBool(const JsonValue& object, const char* key, bool& target, std::vector<std::string>& warnings) {
    const JsonValue* value = object.get(key);
    if (value == nullptr) return;
    if (!value->isBool()) {
        warnings.push_back(std::string(key) + " は true か false で書いてください");
        return;
    }
    target = value->boolean;
}

/**
 * 文字列のキーを読む。キーが無ければ何もしない。
 * @param object 読むオブジェクト
 * @param key キー
 * @param target 書き込み先
 * @param warnings 型違いのときの追加先
 */
void readString(const JsonValue& object, const char* key, std::string& target, std::vector<std::string>& warnings) {
    const JsonValue* value = object.get(key);
    if (value == nullptr) return;
    if (!value->isString()) {
        warnings.push_back(std::string(key) + " は文字列で書いてください");
        return;
    }
    target = value->text;
}

/**
 * thresholds オブジェクトを読む。
 * @param object thresholds の中身
 * @param th 書き込み先
 * @param warnings 追加先
 */
struct ThresholdField {
    const char* key;
    double Thresholds::*member;
    double min, max;
};

constexpr ThresholdField kThresholdFields[] = {
    {"fps_warn_ratio", &Thresholds::fpsWarnRatio, 0, 1}, {"fps_crit_ratio", &Thresholds::fpsCritRatio, 0, 1},
    {"frame_warn_ratio", &Thresholds::frameWarnRatio, 0.1, 10}, {"frame_crit_ratio", &Thresholds::frameCritRatio, 0.1, 10},
    {"reproj_warn_pct", &Thresholds::reprojWarnPct, 0, 100}, {"reproj_crit_pct", &Thresholds::reprojCritPct, 0, 100},
    {"temp_warn_c", &Thresholds::tempWarnC, 0, 150}, {"temp_crit_c", &Thresholds::tempCritC, 0, 150},
    {"battery_warn_pct", &Thresholds::batteryWarnPct, 0, 100}, {"battery_crit_pct", &Thresholds::batteryCritPct, 0, 100},
    {"power_warn_w", &Thresholds::powerWarnW, 0, 100}, {"power_crit_w", &Thresholds::powerCritW, 0, 100},
    {"cpu_warn_pct", &Thresholds::cpuWarnPct, 0, 100}, {"cpu_crit_pct", &Thresholds::cpuCritPct, 0, 100},
    {"gpu_warn_pct", &Thresholds::gpuWarnPct, 0, 100}, {"gpu_crit_pct", &Thresholds::gpuCritPct, 0, 100},
    {"wifi_warn_dbm", &Thresholds::wifiWarnDbm, -120, 0}, {"wifi_crit_dbm", &Thresholds::wifiCritDbm, -120, 0},
    {"controller_warn_pct", &Thresholds::controllerWarnPct, 0, 100},
    {"controller_crit_pct", &Thresholds::controllerCritPct, 0, 100},
};

void readThresholds(const JsonValue& object, Thresholds& th, std::vector<std::string>& warnings) {
    for (const auto& member : object.members) {
        const auto known = std::find_if(std::begin(kThresholdFields), std::end(kThresholdFields),
                                      [&](const ThresholdField& field) { return member.first == field.key; });
        if (known == std::end(kThresholdFields)) warnings.push_back("知らないキーです: thresholds." + member.first);
    }
    for (const auto& field : kThresholdFields) readNumber(object, field.key, field.min, field.max, th.*field.member, warnings);
}

/**
 * 手首の位置と向き（left_wrist / right_wrist）を読む。キーが無ければ何もしない。
 * @param root 設定ファイルの一番外側
 * @param key "left_wrist" か "right_wrist"
 * @param pose 書き込み先
 * @param warnings 追加先
 */
void readWristPose(const JsonValue& root, const char* key, WristPose& pose, std::vector<std::string>& warnings) {
    const JsonValue* object = root.get(key);
    if (object == nullptr) return;
    if (!object->isObject()) {
        warnings.push_back(std::string(key) + " は {\"x\":..,\"y\":..,\"z\":..,\"pitch\":..,\"yaw\":..,\"roll\":..} で書いてください");
        return;
    }
    warnUnknownKeys(*object, {"x", "y", "z", "pitch", "yaw", "roll"}, std::string(key) + ".", warnings);
    readNumber(*object, "x", -0.5, 0.5, pose.x, warnings);
    readNumber(*object, "y", -0.5, 0.5, pose.y, warnings);
    readNumber(*object, "z", -0.5, 0.5, pose.z, warnings);
    readNumber(*object, "pitch", -180.0, 0.0, pose.pitch, warnings);
    readNumber(*object, "yaw", -180.0, 180.0, pose.yaw, warnings);
    readNumber(*object, "roll", -180.0, 180.0, pose.roll, warnings);
}

/**
 * The placement's keys (Placement: attachment, position, rotation, both wrists, the wrist fade, the clock, width_m,
 * alpha), from config.json or a profile's "placement". Keys that are missing leave config as it is.
 * @param root the object
 * @param config 書き込み先
 * @param warnings 追加先
 */
void readPlacementKeys(const JsonValue& root, Config& config, std::vector<std::string>& warnings) {
    std::string attachment = attachmentName(config.attachment);
    readString(root, "attachment", attachment, warnings);
    if (attachment == "left_wrist") {
        config.attachment = Attachment::LeftWrist;
    } else if (attachment == "right_wrist") {
        config.attachment = Attachment::RightWrist;
    } else if (attachment != "head") {
        warnings.push_back("attachment は \"head\" か \"left_wrist\" か \"right_wrist\" で書いてください");
    }
    if (const JsonValue* position = root.get("position")) {
        if (position->isObject()) {
            warnUnknownKeys(*position, {"x", "y", "z"}, "position.", warnings);
            readNumber(*position, "x", -5.0, 5.0, config.posX, warnings);
            readNumber(*position, "y", -5.0, 5.0, config.posY, warnings);
            readNumber(*position, "z", -5.0, 5.0, config.posZ, warnings);
        } else {
            warnings.push_back("position は {\"x\":..,\"y\":..,\"z\":..} で書いてください");
        }
    }
    if (const JsonValue* rotation = root.get("rotation")) {
        if (rotation->isObject()) {
            warnUnknownKeys(*rotation, {"yaw", "pitch", "roll"}, "rotation.", warnings);
            readNumber(*rotation, "yaw", -180.0, 180.0, config.yawDeg, warnings);
            readNumber(*rotation, "pitch", -90.0, 90.0, config.pitchDeg, warnings);
            readNumber(*rotation, "roll", -180.0, 180.0, config.rollDeg, warnings);
        } else {
            warnings.push_back("rotation は {\"yaw\":..,\"pitch\":..,\"roll\":..} で書いてください（度）");
        }
    }
    readWristPose(root, "left_wrist", config.leftWrist, warnings);
    readWristPose(root, "right_wrist", config.rightWrist, warnings);
    readBool(root, "wrist_fade", config.wristFade, warnings);
    readNumber(root, "wrist_fade_end_deg", 35.0, 90.0, config.wristFadeEndDeg, warnings);
    if (const JsonValue* clock = root.get("clock_format")) {
        if (clock->isNumber() && (clock->number == 0 || clock->number == 12 || clock->number == 24)) {
            config.clockFormat = static_cast<int>(clock->number);
        } else {
            warnings.push_back("clock_format は 0（出さない）か 12 か 24 で書いてください");
        }
    }
    readNumber(root, "width_m", 0.03, 2.0, config.widthM, warnings);
    readNumber(root, "alpha", 0.0, 1.0, config.alpha, warnings);
}

/**
 * The "dock" object (in-game interaction and the Dock panel). Missing keys keep the defaults.
 * @param root config.json's outermost object
 * @param dock 書き込み先
 * @param warnings 追加先
 */
void readDock(const JsonValue& root, DockSettings& dock, std::vector<std::string>& warnings) {
    const JsonValue* object = root.get("dock");
    if (object == nullptr) return;
    if (!object->isObject()) {
        warnings.push_back("dock は { ... } で書いてください");
        return;
    }
    warnUnknownKeys(*object,
                    {"enabled", "pointer_hand", "width_m", "collapse_sec", "grip_input", "head", "left_wrist",
                     "right_wrist"},
                    "dock.", warnings);
    readBool(*object, "enabled", dock.enabled, warnings);
    std::string hand = dockHandName(dock.pointerHand);
    readString(*object, "pointer_hand", hand, warnings);
    if (hand == "left") {
        dock.pointerHand = DockHand::Left;
    } else if (hand == "right") {
        dock.pointerHand = DockHand::Right;
    } else if (hand == "auto") {
        dock.pointerHand = DockHand::Auto;
    } else {
        warnings.push_back("dock.pointer_hand は \"auto\" か \"left\" か \"right\" で書いてください");
    }
    readNumber(*object, "width_m", 0.15, 0.60, dock.widthM, warnings);
    readInt(*object, "collapse_sec", 0, 600, dock.collapseSec, warnings);
    readBool(*object, "grip_input", dock.gripInput, warnings);
    if (const JsonValue* head = object->get("head")) {
        if (head->isObject()) {
            warnUnknownKeys(*head, {"x", "y", "z", "yaw", "pitch", "roll"}, "dock.head.", warnings);
            readNumber(*head, "x", -5.0, 5.0, dock.head.x, warnings);
            readNumber(*head, "y", -5.0, 5.0, dock.head.y, warnings);
            readNumber(*head, "z", -5.0, 5.0, dock.head.z, warnings);
            readNumber(*head, "yaw", -180.0, 180.0, dock.head.yaw, warnings);
            readNumber(*head, "pitch", -90.0, 90.0, dock.head.pitch, warnings);
            readNumber(*head, "roll", -180.0, 180.0, dock.head.roll, warnings);
        } else {
            warnings.push_back("dock.head は {\"x\":..,\"y\":..,\"z\":..,\"yaw\":..,\"pitch\":..,\"roll\":..} で書いてください");
        }
    }
    readWristPose(*object, "left_wrist", dock.leftWrist, warnings);
    readWristPose(*object, "right_wrist", dock.rightWrist, warnings);
}

/** A pose relative to the head ({"x","y","z","yaw","pitch","roll"}) under key. */
void readHeadPose(const JsonValue& object, const char* key, const std::string& prefix, HeadPose& pose,
                  std::vector<std::string>& warnings) {
    const JsonValue* head = object.get(key);
    if (head == nullptr) return;
    if (!head->isObject()) {
        warnings.push_back(prefix + key + " は {\"x\":..,\"y\":..,\"z\":..,\"yaw\":..,\"pitch\":..,\"roll\":..} で書いてください");
        return;
    }
    warnUnknownKeys(*head, {"x", "y", "z", "yaw", "pitch", "roll"}, prefix + key + ".", warnings);
    readNumber(*head, "x", -5.0, 5.0, pose.x, warnings);
    readNumber(*head, "y", -5.0, 5.0, pose.y, warnings);
    readNumber(*head, "z", -5.0, 5.0, pose.z, warnings);
    readNumber(*head, "yaw", -180.0, 180.0, pose.yaw, warnings);
    readNumber(*head, "pitch", -90.0, 90.0, pose.pitch, warnings);
    readNumber(*head, "roll", -180.0, 180.0, pose.roll, warnings);
}

/**
 * The "browser" object (the browser panel). Missing keys keep the defaults.
 */
void readBrowser(const JsonValue& root, BrowserSettings& browser, std::vector<std::string>& warnings) {
    const JsonValue* object = root.get("browser");
    if (object == nullptr) return;
    if (!object->isObject()) {
        warnings.push_back("browser は { ... } で書いてください");
        return;
    }
    warnUnknownKeys(*object,
                    {"chrome_command", "mode", "transport", "home", "bookmarks", "fps", "width", "height", "aspect",
                     "mobile", "zoom_pct", "jpeg_quality",
                     "keep_audio", "pause_when_removed", "idle_shutdown_min", "width_m", "anchor", "extra_args", "head", "left_wrist",
                     "right_wrist"},
                    "browser.", warnings);
    readString(*object, "chrome_command", browser.chromeCommand, warnings);
    std::string mode = browser.headless ? "headless" : "window";
    readString(*object, "mode", mode, warnings);
    if (mode == "headless" || mode == "window") {
        browser.headless = mode == "headless";
    } else {
        warnings.push_back("browser.mode は \"headless\" か \"window\" で書いてください");
    }
    std::string transport = browser.transportPipe ? "pipe" : "port";
    readString(*object, "transport", transport, warnings);
    if (transport == "pipe" || transport == "port") {
        browser.transportPipe = transport == "pipe";
    } else {
        warnings.push_back("browser.transport は \"pipe\" か \"port\" で書いてください");
    }
    readString(*object, "home", browser.home, warnings);
    // "bookmarks" (the bar's old quick links) is still accepted above and ignored: 즐겨찾기 took their place
    readInt(*object, "fps", 5, 60, browser.fps, warnings);
    readInt(*object, "width", 640, 1920, browser.viewportWidth, warnings);
    readInt(*object, "height", 360, 1080, browser.viewportHeight, warnings);
    std::string aspect = browserAspectName(browser.aspect);
    readString(*object, "aspect", aspect, warnings);
    bool aspectKnown = false;
    for (const BrowserAspect a : {BrowserAspect::Wide, BrowserAspect::Standard, BrowserAspect::Square, BrowserAspect::Tall}) {
        if (aspect == browserAspectName(a)) {
            browser.aspect = a;
            aspectKnown = true;
        }
    }
    if (!aspectKnown) warnings.push_back("browser.aspect は \"16:9\" か \"4:3\" か \"1:1\" か \"9:16\" で書いてください");
    readBool(*object, "mobile", browser.mobile, warnings);
    readInt(*object, "zoom_pct", kBrowserZoomSteps[0], kBrowserZoomSteps[std::size(kBrowserZoomSteps) - 1],
            browser.zoomPct, warnings);
    readInt(*object, "jpeg_quality", 30, 95, browser.jpegQuality, warnings);
    readBool(*object, "keep_audio", browser.keepAudio, warnings);
    readBool(*object, "pause_when_removed", browser.pauseWhenRemoved, warnings);
    readInt(*object, "idle_shutdown_min", 0, 1440, browser.idleShutdownMin, warnings);
    readNumber(*object, "width_m", 0.25, 1.5, browser.widthM, warnings);
    std::string anchor = browserAnchorName(browser.anchor);
    readString(*object, "anchor", anchor, warnings);
    if (anchor == "auto") {
        browser.anchor = BrowserAnchor::Auto;
    } else if (anchor == "left_wrist") {
        browser.anchor = BrowserAnchor::LeftWrist;
    } else if (anchor == "right_wrist") {
        browser.anchor = BrowserAnchor::RightWrist;
    } else if (anchor == "head") {
        browser.anchor = BrowserAnchor::Head;
    } else {
        warnings.push_back("browser.anchor は \"auto\" か \"left_wrist\" か \"right_wrist\" か \"head\" で書いてください");
    }
    if (const JsonValue* extra = object->get("extra_args")) {
        if (extra->type == JsonValue::Type::Array) {
            browser.extraArgs.clear();
            for (const JsonValue& a : extra->items) {
                if (a.isString() && !a.text.empty()) browser.extraArgs.push_back(a.text);
            }
        } else {
            warnings.push_back("browser.extra_args は [\"--switch\", ...] で書いてください");
        }
    }
    readHeadPose(*object, "head", "browser.", browser.head, warnings);
    readWristPose(*object, "left_wrist", browser.leftWrist, warnings);
    readWristPose(*object, "right_wrist", browser.rightWrist, warnings);
}

/** The "browser" member as saveConfig writes it (no trailing comma). */
std::string browserToJson(const BrowserSettings& b) {
    char line[512];
    std::string out = "  \"browser\": {\n";
    out += "    \"chrome_command\": " + jsonString(b.chromeCommand) + ",\n";
    out += std::string("    \"mode\": \"") + (b.headless ? "headless" : "window") + "\",\n";
    out += std::string("    \"transport\": \"") + (b.transportPipe ? "pipe" : "port") + "\",\n";
    out += "    \"home\": " + jsonString(b.home) + ",\n";
    std::snprintf(line, sizeof(line),
                  "    \"fps\": %d,\n    \"width\": %d,\n    \"height\": %d,\n    \"aspect\": \"%s\",\n"
                  "    \"mobile\": %s,\n    \"zoom_pct\": %d,\n    \"jpeg_quality\": %d,\n"
                  "    \"keep_audio\": %s,\n    \"pause_when_removed\": %s,\n    \"idle_shutdown_min\": %d,\n"
                  "    \"width_m\": %.4g,\n    \"anchor\": \"%s\",\n",
                  b.fps, b.viewportWidth, b.viewportHeight, browserAspectName(b.aspect), b.mobile ? "true" : "false",
                  b.zoomPct, b.jpegQuality, b.keepAudio ? "true" : "false",
                  b.pauseWhenRemoved ? "true" : "false", b.idleShutdownMin, fin(b.widthM), browserAnchorName(b.anchor));
    out += line;
    out += "    \"extra_args\": [";
    for (size_t i = 0; i < b.extraArgs.size(); ++i) out += (i == 0 ? "" : ", ") + jsonString(b.extraArgs[i]);
    out += "],\n";
    std::snprintf(line, sizeof(line),
                  "    \"head\": { \"x\": %.4g, \"y\": %.4g, \"z\": %.4g, \"yaw\": %.4g, \"pitch\": %.4g, \"roll\": %.4g },\n",
                  fin(b.head.x), fin(b.head.y), fin(b.head.z), fin(b.head.yaw), fin(b.head.pitch), fin(b.head.roll));
    out += line;
    for (const char* key : {"left_wrist", "right_wrist"}) {
        const WristPose& w = key[0] == 'l' ? b.leftWrist : b.rightWrist;
        std::snprintf(line, sizeof(line),
                      "    \"%s\": { \"x\": %.4g, \"y\": %.4g, \"z\": %.4g, \"pitch\": %.4g, \"yaw\": %.4g, \"roll\": %.4g }%s\n",
                      key, fin(w.x), fin(w.y), fin(w.z), fin(w.pitch), fin(w.yaw), fin(w.roll), key[0] == 'l' ? "," : "");
        out += line;
    }
    out += "  }";
    return out;
}

/**
 * The "dock" member as saveConfig writes it (no trailing comma).
 * @param dock the settings
 */
std::string dockToJson(const DockSettings& dock) {
    char line[512];
    std::string out = "  \"dock\": {\n";
    std::snprintf(line, sizeof(line),
                  "    \"enabled\": %s,\n    \"pointer_hand\": \"%s\",\n    \"width_m\": %.4g,\n"
                  "    \"collapse_sec\": %d,\n    \"grip_input\": %s,\n",
                  dock.enabled ? "true" : "false", dockHandName(dock.pointerHand), fin(dock.widthM), dock.collapseSec,
                  dock.gripInput ? "true" : "false");
    out += line;
    std::snprintf(line, sizeof(line),
                  "    \"head\": { \"x\": %.4g, \"y\": %.4g, \"z\": %.4g, \"yaw\": %.4g, \"pitch\": %.4g, \"roll\": %.4g },\n",
                  fin(dock.head.x), fin(dock.head.y), fin(dock.head.z), fin(dock.head.yaw), fin(dock.head.pitch),
                  fin(dock.head.roll));
    out += line;
    for (const char* key : {"left_wrist", "right_wrist"}) {
        const WristPose& w = key[0] == 'l' ? dock.leftWrist : dock.rightWrist;
        std::snprintf(line, sizeof(line),
                      "    \"%s\": { \"x\": %.4g, \"y\": %.4g, \"z\": %.4g, \"pitch\": %.4g, \"yaw\": %.4g, \"roll\": %.4g }%s\n",
                      key, fin(w.x), fin(w.y), fin(w.z), fin(w.pitch), fin(w.yaw), fin(w.roll), key[0] == 'l' ? "," : "");
        out += line;
    }
    out += "  }";
    return out;
}

}  // namespace

const char* browserAspectName(BrowserAspect aspect) {
    switch (aspect) {
        case BrowserAspect::Standard: return "4:3";
        case BrowserAspect::Square: return "1:1";
        case BrowserAspect::Tall: return "9:16";
        case BrowserAspect::Wide: break;
    }
    return "16:9";
}

void BrowserSettings::pageSize(int& width, int& height) const {
    const double ratio = aspect == BrowserAspect::Standard ? 4.0 / 3 : aspect == BrowserAspect::Square ? 1.0
                         : aspect == BrowserAspect::Tall   ? 9.0 / 16
                                                           : 0;
    if (ratio == 0) {
        width = viewportWidth;
        height = viewportHeight;
        return;
    }
    const double pixels = static_cast<double>(viewportWidth) * viewportHeight;
    width = 2 * static_cast<int>(std::lround(std::sqrt(pixels * ratio) / 2));
    height = 2 * static_cast<int>(std::lround(std::sqrt(pixels / ratio) / 2));
}

const char* browserAnchorName(BrowserAnchor anchor) {
    switch (anchor) {
        case BrowserAnchor::LeftWrist: return "left_wrist";
        case BrowserAnchor::RightWrist: return "right_wrist";
        case BrowserAnchor::Head: return "head";
        case BrowserAnchor::Auto: break;
    }
    return "auto";
}

const char* dockHandName(DockHand hand) {
    switch (hand) {
        case DockHand::Left: return "left";
        case DockHand::Right: return "right";
        case DockHand::Auto: break;
    }
    return "auto";
}

const char* attachmentName(Attachment attachment) {
    switch (attachment) {
        case Attachment::LeftWrist: return "left_wrist";
        case Attachment::RightWrist: return "right_wrist";
        case Attachment::Head: break;
    }
    return "head";
}

WristPose& selectedWrist(Config& config) {
    return config.attachment == Attachment::RightWrist ? config.rightWrist : config.leftWrist;
}

const WristPose& selectedWrist(const Config& config) {
    return config.attachment == Attachment::RightWrist ? config.rightWrist : config.leftWrist;
}

std::string defaultConfigPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg != nullptr && xdg[0] != '\0') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = std::string(home != nullptr ? home : ".") + "/.config";
    }
    return base + "/frame-perf-overlay/config.json";
}

Config freshConfig() {
    Config config;
    config.attachment = kFreshAttachment;
    config.language = systemLanguage();
    config.firstRunGuideDone = false;
    return config;
}

bool loadConfig(const std::string& path, Config& out, std::vector<std::string>& warnings, std::string& error) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        out = freshConfig();  // ファイルが無いとき: a fresh install
        return true;
    }
    std::string source;
    char buffer[4096];
    size_t count;
    bool tooLarge = false;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) != 0) {
        source.append(buffer, count);
        if (source.size() > kMaxConfigBytes) {
            tooLarge = true;  // not a config file anyone wrote by hand: don't read (or parse) all of it
            break;
        }
    }
    const bool readFailed = std::ferror(file) != 0;
    std::fclose(file);
    if (tooLarge) {
        error = "ファイルが大きすぎます (" + std::to_string(kMaxConfigBytes >> 20) + " MiB まで): " + path;
        return false;
    }
    if (readFailed) {
        error = "読み込みに失敗しました: " + path;
        return false;
    }

    JsonValue root;
    if (!parseJson(source, root, error)) return false;
    if (!root.isObject()) {
        error = "一番外側は { ... } にしてください";
        return false;
    }

    Config config;  // 書かれていない項目は既定値
    warnUnknownKeys(root,
                    {"visible", "update_check", "language", "attachment", "position", "rotation", "left_wrist",
                     "right_wrist", "wrist_fade", "wrist_fade_end_deg", "clock_format", "width_m", "alpha",
                     "update_interval_ms", "graph_seconds", "font", "font_bold", "thresholds", "layout",
                     "active_profile", "language_migrated", "templates_seeded", "first_run_guide_done", "dock",
                     "browser"},
                    "", warnings);
    readBool(root, "visible", config.visible, warnings);
    readBool(root, "update_check", config.updateCheck, warnings);
    readString(root, "active_profile", config.activeProfile, warnings);
    if (const JsonValue* seeded = root.get("templates_seeded")) {
        // A list of template ids; an older Frame Dock's true: the two it had then (simple_a, simple_b)
        if (seeded->type == JsonValue::Type::Bool) {
            if (seeded->boolean) config.templatesSeeded = {"simple_a", "simple_b"};
        } else if (seeded->type == JsonValue::Type::Array) {
            for (const JsonValue& item : seeded->items) {
                if (item.isString()) config.templatesSeeded.push_back(item.text);
            }
        } else {
            warnings.push_back("templates_seeded は [\"simple_a\", ...] で書いてください");
        }
    }
    readBool(root, "first_run_guide_done", config.firstRunGuideDone, warnings);
    // "language": "auto" or a fixed one ("language_migrated", an older Frame Dock's, is known and ignored)
    if (const JsonValue* language = root.get("language")) {
        if (language->isString() && language->text == "auto") {
            config.languageAuto = true;
        } else if (language->isString() && parseLanguage(language->text, config.language)) {
            config.languageAuto = false;
        } else {
            warnings.push_back("language は \"auto\" / \"ko\" / \"en\" / \"ja\" のどれかにしてください（auto にしました）");
        }
    }
    if (config.languageAuto) config.language = systemLanguage();
    readPlacementKeys(root, config, warnings);
    readDock(root, config.dock, warnings);
    readBrowser(root, config.browser, warnings);
    readInt(root, "update_interval_ms", 100, 5000, config.updateIntervalMs, warnings);
    readInt(root, "graph_seconds", 5, 300, config.graphSeconds, warnings);
    readString(root, "font", config.fontPath, warnings);
    readString(root, "font_bold", config.boldFontPath, warnings);
    if (const JsonValue* thresholds = root.get("thresholds")) {
        if (thresholds->isObject()) {
            readThresholds(*thresholds, config.thresholds, warnings);
        } else {
            warnings.push_back("thresholds は { ... } で書いてください");
        }
    }
    // The widget grid. A broken layout falls back to the default one; the rest of the file still counts. A file
    // without one (from before the grid) gets the default too: two columns, the fixed panel's readings
    if (const JsonValue* layout = root.get("layout")) {
        std::string layoutError;
        if (frame_perf_overlay::layoutFromJson(*layout, config.layout, warnings, layoutError)) {
            config.layoutInFile = true;
        } else {
            config.layoutRejected = true;
            warnings.push_back(layoutError + ": using the default layout");
        }
    }
    out = config;
    return true;
}

namespace {

/**
 * フォルダを親から順に作る（mkdir -p と同じ）。
 * @param dir 作るフォルダ
 * @return できた（元からあった）なら true
 */
bool makeDirectories(const std::string& dir) {
    for (size_t pos = 1; pos <= dir.size(); ++pos) {
        if (pos != dir.size() && dir[pos] != '/') continue;
        const std::string part = dir.substr(0, pos);
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

}  // namespace

bool saveConfig(const std::string& path, const Config& config, std::string& error) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !makeDirectories(path.substr(0, slash))) {
        error = "フォルダを作れません: " + std::string(std::strerror(errno));
        return false;
    }
    // 一時ファイルに直接書く。文字列全体を組み立てるコピーやストリームのロケールは要らない。
    const std::string temp = path + ".tmp";
    FILE* file = std::fopen(temp.c_str(), "wb");
    if (file == nullptr) {
        error = "書き込めません: " + temp;
        return false;
    }
    const auto boolean = [](bool value) { return value ? "true" : "false"; };
    std::fprintf(file,
        "{\n"
        "  \"visible\": %s,\n"
        "  \"update_check\": %s,\n"
        "  \"language\": \"%s\",\n"
        "%s,\n",
        boolean(config.visible), boolean(config.updateCheck),
        config.languageAuto ? "auto" : languageCode(config.language), placementMembers(placementOf(config), 2).c_str());
    std::fprintf(file,
        "  \"update_interval_ms\": %d,\n"
        "  \"graph_seconds\": %d,\n"
        "  \"font\": %s,\n"
        "  \"font_bold\": %s,\n"
        "  \"thresholds\": {\n",
        config.updateIntervalMs, config.graphSeconds, jsonString(config.fontPath).c_str(), jsonString(config.boldFontPath).c_str());
    const auto* last = std::end(kThresholdFields) - 1;
    for (const auto& field : kThresholdFields) {
        std::fprintf(file, "    \"%s\": %.4g%s\n", field.key, fin(config.thresholds.*field.member),
                     &field == last ? "" : ",");
    }
    std::string seeded = "[";
    for (size_t i = 0; i < config.templatesSeeded.size(); ++i) {
        seeded += (i == 0 ? "" : ", ") + jsonString(config.templatesSeeded[i]);
    }
    seeded += "]";
    std::fprintf(file,
                 "  },\n  \"active_profile\": %s,\n  \"templates_seeded\": %s,\n  \"first_run_guide_done\": %s,\n"
                 "%s,\n%s,\n  \"layout\": %s\n}\n",
                 jsonString(config.activeProfile).c_str(), seeded.c_str(),
                 boolean(config.firstRunGuideDone),
                 dockToJson(config.dock).c_str(), browserToJson(config.browser).c_str(),
                 frame_perf_overlay::layoutToJson(config.layout, 2).c_str());
    // The data must be on disk before the rename makes it the config (a power cut right after could leave an
    // empty file otherwise)
    const bool writeFailed = std::ferror(file) != 0 || std::fflush(file) != 0 || ::fsync(::fileno(file)) != 0;
    const bool closeFailed = std::fclose(file) != 0;
    if (writeFailed || closeFailed) {
        error = "書き込みに失敗しました: " + temp;
        ::unlink(temp.c_str());
        return false;
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        error = "置き換えに失敗しました: " + std::string(std::strerror(errno));
        ::unlink(temp.c_str());
        return false;
    }
    return true;
}

Placement placementOf(const Config& config) {
    Placement p;
    p.attachment = config.attachment;
    p.posX = config.posX;
    p.posY = config.posY;
    p.posZ = config.posZ;
    p.yawDeg = config.yawDeg;
    p.pitchDeg = config.pitchDeg;
    p.rollDeg = config.rollDeg;
    p.leftWrist = config.leftWrist;
    p.rightWrist = config.rightWrist;
    p.wristFade = config.wristFade;
    p.wristFadeEndDeg = config.wristFadeEndDeg;
    p.clockFormat = config.clockFormat;
    p.widthM = config.widthM;
    p.alpha = config.alpha;
    return p;
}

void applyPlacement(const Placement& p, Config& config) {
    config.attachment = p.attachment;
    config.posX = p.posX;
    config.posY = p.posY;
    config.posZ = p.posZ;
    config.yawDeg = p.yawDeg;
    config.pitchDeg = p.pitchDeg;
    config.rollDeg = p.rollDeg;
    config.leftWrist = p.leftWrist;
    config.rightWrist = p.rightWrist;
    config.wristFade = p.wristFade;
    config.wristFadeEndDeg = p.wristFadeEndDeg;
    config.clockFormat = p.clockFormat;
    config.widthM = p.widthM;
    config.alpha = p.alpha;
}

std::string placementMembers(const Placement& p, int indent) {
    const std::string pad(static_cast<size_t>(std::max(0, indent)), ' ');
    char line[256];
    std::string out;
    const auto add = [&](const char* text) {
        if (!out.empty()) out += ",\n";
        out += pad;
        out += text;
    };
    std::snprintf(line, sizeof(line), "\"attachment\": \"%s\"", attachmentName(p.attachment));
    add(line);
    std::snprintf(line, sizeof(line), "\"position\": { \"x\": %.4g, \"y\": %.4g, \"z\": %.4g }", fin(p.posX), fin(p.posY),
                  fin(p.posZ));
    add(line);
    std::snprintf(line, sizeof(line), "\"rotation\": { \"yaw\": %.4g, \"pitch\": %.4g, \"roll\": %.4g }", fin(p.yawDeg),
                  fin(p.pitchDeg), fin(p.rollDeg));
    add(line);
    for (const char* key : {"left_wrist", "right_wrist"}) {
        const WristPose& w = key[0] == 'l' ? p.leftWrist : p.rightWrist;
        std::snprintf(line, sizeof(line),
                      "\"%s\": { \"x\": %.4g, \"y\": %.4g, \"z\": %.4g, \"pitch\": %.4g, \"yaw\": %.4g, \"roll\": %.4g }", key,
                      fin(w.x), fin(w.y), fin(w.z), fin(w.pitch), fin(w.yaw), fin(w.roll));
        add(line);
    }
    std::snprintf(line, sizeof(line), "\"wrist_fade\": %s", p.wristFade ? "true" : "false");
    add(line);
    std::snprintf(line, sizeof(line), "\"wrist_fade_end_deg\": %.4g", fin(p.wristFadeEndDeg));
    add(line);
    std::snprintf(line, sizeof(line), "\"clock_format\": %d", p.clockFormat);
    add(line);
    std::snprintf(line, sizeof(line), "\"width_m\": %.4g", fin(p.widthM));
    add(line);
    std::snprintf(line, sizeof(line), "\"alpha\": %.4g", fin(p.alpha));
    add(line);
    return out;
}

std::string placementToJson(const Placement& placement, int indent) {
    return "{\n" + placementMembers(placement, indent + 2) + "\n" +
           std::string(static_cast<size_t>(std::max(0, indent)), ' ') + "}";
}

bool placementFromJson(const JsonValue& value, Placement& out, std::vector<std::string>& warnings, std::string& error) {
    if (!value.isObject()) {
        error = "placement isn't an object";
        return false;
    }
    warnUnknownKeys(value,
                    {"attachment", "position", "rotation", "left_wrist", "right_wrist", "wrist_fade",
                     "wrist_fade_end_deg", "clock_format", "width_m", "alpha"},
                    "placement.", warnings);
    Config config;  // missing keys: the defaults
    readPlacementKeys(value, config, warnings);
    out = placementOf(config);
    return true;
}

bool samePlacement(const Placement& a, const Placement& b) {
    return placementMembers(a, 0) == placementMembers(b, 0);
}

void resetDisplaySettings(Config& config) {
    const Config defaults;
    config.visible = defaults.visible;
    config.attachment = kFreshAttachment;  // where a fresh install puts the panel, not Config's head default
    config.posX = defaults.posX;
    config.posY = defaults.posY;
    config.posZ = defaults.posZ;
    config.yawDeg = defaults.yawDeg;
    config.pitchDeg = defaults.pitchDeg;
    config.rollDeg = defaults.rollDeg;
    config.leftWrist = defaults.leftWrist;
    config.rightWrist = defaults.rightWrist;
    config.wristFade = defaults.wristFade;
    config.wristFadeEndDeg = defaults.wristFadeEndDeg;
    config.clockFormat = defaults.clockFormat;
    config.widthM = defaults.widthM;
    config.alpha = defaults.alpha;
}

namespace {

constexpr double kDegToRad = M_PI / 180.0;

/**
 * 0.1° 単位に丸める（小数の誤差を設定ファイルに残さない）。
 * @param degrees 角度（度）
 * @return 丸めた値
 */
double roundTenthDegree(double degrees) {
    return std::round(degrees * 10.0) / 10.0 + 0.0;  // + 0.0 で −0 を 0 にする（ファイルに "-0" と書かない）
}

/**
 * faceHead() の yaw と pitch を計算する（丸める前の値）。
 * @param config 設定（位置だけ使う）
 * @param yaw yaw の書き込み先（度）
 * @param pitch pitch の書き込み先（度）
 */
void headFacingAngles(const Config& config, double& yaw, double& pitch) {
    // 原点と同じ位置のときは atan2(0, 0) = 0 になり、正面向きのまま
    yaw = std::atan2(-config.posX, -config.posZ) / kDegToRad;
    pitch = std::atan2(-config.posY, std::hypot(config.posX, config.posZ)) / kDegToRad;
}

}  // namespace

void panelRotation(double yawDeg, double pitchDeg, double rollDeg, double r[3][3]) {
    const double cy = std::cos(yawDeg * kDegToRad), sy = std::sin(yawDeg * kDegToRad);
    const double cp = std::cos(pitchDeg * kDegToRad), sp = std::sin(pitchDeg * kDegToRad);
    const double cr = std::cos(rollDeg * kDegToRad), sr = std::sin(rollDeg * kDegToRad);
    // Ry(yaw) = [cy 0 sy; 0 1 0; −sy 0 cy]、Rx(−pitch) = [1 0 0; 0 cp sp; 0 −sp cp]、Rz(roll) = [cr −sr 0; sr cr 0; 0 0 1]
    // を掛けた R = Ry · Rx(−pitch) · Rz。3 列目（パネルの表の向き）は (sy·cp, sp, cy·cp)
    r[0][0] = cy * cr - sy * sp * sr;
    r[0][1] = -cy * sr - sy * sp * cr;
    r[0][2] = sy * cp;
    r[1][0] = cp * sr;
    r[1][1] = cp * cr;
    r[1][2] = sp;
    r[2][0] = -sy * cr - cy * sp * sr;
    r[2][1] = sy * sr - cy * sp * cr;
    r[2][2] = cy * cp;
}

void panelRotation(const Config& config, double r[3][3]) {
    panelRotation(config.yawDeg, config.pitchDeg, config.rollDeg, r);
}

void faceHead(Config& config) {
    double yaw = 0.0;
    double pitch = 0.0;
    headFacingAngles(config, yaw, pitch);
    config.yawDeg = roundTenthDegree(yaw);
    config.pitchDeg = roundTenthDegree(pitch);
}

bool ConfigWatcher::save(const Config& config, std::string& error) {
    if (loadFailed_ || layoutRejected_) {
        const std::string bad = path_ + ".bad";
        if (std::rename(path_.c_str(), bad.c_str()) == 0) {
            std::fprintf(stderr, "[設定] %s %s %s として残し、新しく保存します\n", path_.c_str(),
                         loadFailed_ ? "を読めなかったので" : "の layout を使えなかったので", bad.c_str());
        } else if (errno != ENOENT) {
            error = "読めなかった設定を " + bad + " として残せません: " + std::strerror(errno);
            return false;
        }
        loadFailed_ = false;
        layoutRejected_ = false;
    }
    if (!saveConfig(path_, config, error)) return false;
    noteSaved();
    return true;
}

void ConfigWatcher::noteSaved() {
    statFile(existed_, mtime_, size_);
}

ConfigWatcher::ConfigWatcher(std::string path) : path_(std::move(path)) {}

void ConfigWatcher::statFile(bool& exists, struct timespec& mtime, off_t& size) const {
    struct stat st {};
    exists = ::stat(path_.c_str(), &st) == 0;
    mtime = exists ? st.st_mtim : timespec{};
    size = exists ? st.st_size : 0;
}

bool ConfigWatcher::loadAndLog(Config& config) const {
    std::vector<std::string> warnings;
    std::string error;
    Config loaded;
    loadFailed_ = false;
    if (!loadConfig(path_, loaded, warnings, error)) {
        loadFailed_ = true;
        std::fprintf(stderr, "[設定] %s を読めませんでした: %s（前の設定のまま）\n", path_.c_str(), error.c_str());
        return false;
    }
    for (const auto& warning : warnings) std::fprintf(stderr, "[設定] %s\n", warning.c_str());
    layoutRejected_ = loaded.layoutRejected;
    config = loaded;
    return true;
}

Config ConfigWatcher::loadInitial() {
    statFile(existed_, mtime_, size_);
    // No file: a fresh install (left wrist, auto language, the first-run guide), as loadConfig gives
    Config config = existed_ ? Config() : freshConfig();
    if (existed_) {
        if (loadAndLog(config)) std::fprintf(stderr, "[設定] %s を読みました\n", path_.c_str());
    } else {
        std::fprintf(stderr, "[設定] %s が無いので既定値で動きます\n", path_.c_str());
    }
    return config;
}

bool ConfigWatcher::reloadIfChanged(Config& config) {
    bool exists = false;
    struct timespec mtime {};
    off_t size = 0;
    statFile(exists, mtime, size);
    const bool changed = exists != existed_ || size != size_ || mtime.tv_sec != mtime_.tv_sec ||
                         mtime.tv_nsec != mtime_.tv_nsec;
    if (!changed) return false;
    existed_ = exists;
    mtime_ = mtime;
    size_ = size;
    if (!exists) {
        std::fprintf(stderr, "[設定] %s が消えたので既定値に戻します\n", path_.c_str());
        config = freshConfig();
        return true;
    }
    if (!loadAndLog(config)) return false;
    std::fprintf(stderr, "[設定] 変更を読み直しました\n");
    return true;
}
