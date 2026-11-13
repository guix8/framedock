// 設定（~/.config/frame-perf-overlay/config.json）の読み込みと変更の監視。
#pragma once

#include "i18n.h"
#include "json.h"        // JsonValue (placementFromJson)
#include "layout.h"      // the widget grid (core/)
#include "thresholds.h"  // in core/, so the widgets share it

#include <ctime>
#include <string>
#include <vector>

/** 性能パネルをどこに固定するか。 */
enum class Attachment {
    Head,        ///< 頭（HMD）
    LeftWrist,   ///< 左手のコントローラー
    RightWrist,  ///< 右手のコントローラー
};

/**
 * 手首（コントローラー）に固定するときの位置と向き。左右の手で別々に持つ。
 * 位置はコントローラーの座標（SteamVR が返すコントローラーの姿勢の軸、m）。レーザーで動かしたパネルの置き場所が
 * ここに入る。
 * 向きは頭のパネルと同じ約束（パネル自身の軸で yaw → pitch → roll、正で面が右・上・面を見て反時計回り）で、
 * 手首の上に寝かせた向きを pitch −90 として数える（wristTransform のコメントを参照）。
 */
struct WristPose {
    double x = 0.0;       ///< コントローラーの x（m、−0.5〜0.5）
    double y = 0.05;      ///< コントローラーの y（m、−0.5〜0.5）
    double z = 0.08;      ///< コントローラーの z（m、−0.5〜0.5）
    double pitch = -90.0; ///< 上下の傾き（度、−180〜0）。−90 で手首の上に寝かせた向き。増やすと面がパネルの上側へ向く
    double yaw = 0.0;     ///< 左右の向き（度、−180〜180）。正で面がパネルの右側へ向く
    double roll = 0.0;    ///< 画面内の回転（度、−180〜180）。正で面を見て反時計回り

    /**
     * どれか 1 つでも違うか。
     * @param other 比べる相手
     * @return 違えば true
     */
    bool operator!=(const WristPose& other) const {
        return x != other.x || y != other.y || z != other.z || pitch != other.pitch || yaw != other.yaw ||
               roll != other.roll;
    }
};

/**
 * A pose relative to the head (the Dock panel's head offset): position in HMD axes (m) and the head HUD's yaw / pitch
 * / roll convention (panelRotation).
 */
struct HeadPose {
    double x = 0.0;
    double y = -0.10;
    double z = -0.55;
    double yaw = 0.0;
    double pitch = 10.0;  ///< a little up, toward the eyes from below
    double roll = 0.0;
    bool operator!=(const HeadPose& o) const {
        return x != o.x || y != o.y || z != o.z || yaw != o.yaw || pitch != o.pitch || roll != o.roll;
    }
};

/** Which controller's laser works the HUD and the Dock panel in a game. */
enum class DockHand {
    Auto,   ///< the hand without the HUD (either hand when the HUD is on the head)
    Left,
    Right,
};

/** The Dock panel's default pose on a wrist: over the hand, 9 cm up, tilted 25° from flat toward the user. */
inline WristPose defaultDockWrist() {
    WristPose pose;
    pose.x = 0.0;
    pose.y = 0.09;
    pose.z = -0.10;
    pose.pitch = -115.0;
    return pose;
}

/**
 * In-game interaction ("dock" in config.json): point at the HUD with the other hand's laser, click it for the Dock
 * panel. Not part of a profile's placement.
 */
struct DockSettings {
    bool enabled = true;                    ///< "enabled": the HUD takes the laser in games
    DockHand pointerHand = DockHand::Auto;  ///< "pointer_hand": "auto" | "left" | "right"
    double widthM = 0.30;                   ///< "width_m": the Dock panel's quick view width (0.15–0.60 m)
    int collapseSec = 20;                   ///< "collapse_sec": collapse after this long untouched (0 = never)
    bool gripInput = true;                  ///< "grip_input": read the grip through SteamVR Input (IVRInput)
    HeadPose head;                          ///< "head": the Dock's pose relative to the head (HUD on the head)
    WristPose leftWrist = defaultDockWrist();   ///< "left_wrist": relative to the left controller
    WristPose rightWrist = defaultDockWrist();  ///< "right_wrist"
};

/** Where the browser panel hangs. */
enum class BrowserAnchor {
    Auto,        ///< the HUD's wrist (the head when the HUD is on the head)
    LeftWrist,
    RightWrist,
    Head,
};

/** The browser panel's default pose on a wrist: above the hand, standing up toward the user like a held tablet. */
inline WristPose defaultBrowserWrist() {
    WristPose pose;
    pose.x = 0.0;
    pose.y = 0.17;
    pose.z = -0.06;
    pose.pitch = -140.0;
    return pose;
}

/** The browser panel's default pose relative to the head: in front, a little low. */
inline HeadPose defaultBrowserHead() {
    HeadPose pose;
    pose.x = 0.0;
    pose.y = -0.08;
    pose.z = -0.75;
    pose.pitch = 5.0;
    return pose;
}

/** Steps of the idle shutdown on the Browser page (minutes; 0 = never). */
constexpr int kBrowserIdleSteps[] = {5, 10, 15, 30, 60, 0};
/** The Browser page's two resolutions (CSS px = texture px of the page) */
constexpr int kBrowser720p[2] = {1280, 720};
constexpr int kBrowser1080p[2] = {1920, 1080};
/** The page's zoom steps (%; the 보기 list's − / +), as Chrome's own */
constexpr int kBrowserZoomSteps[] = {50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200};

/** The page's shape (the 보기 list's 비율): the same number of pixels as width × height, another aspect ratio */
enum class BrowserAspect { Wide, Standard, Square, Tall };  ///< 16:9 | 4:3 | 1:1 | 9:16
/** Frame rate caps the Browser page steps through. */
constexpr int kBrowserFpsSteps[] = {10, 15, 24, 30, 45, 60};

/**
 * The browser panel ("browser" in config.json): Chrome over the DevTools protocol, shown on its own overlay. Not part
 * of a profile's placement.
 */
struct BrowserSettings {
    std::string chromeCommand = "flatpak run com.google.Chrome";  ///< "chrome_command"
    bool headless = true;                    ///< "mode": "headless" (no window) or "window" (a window far off screen)
    /// "transport": "pipe" (--remote-debugging-pipe; nothing listens) or "port" (a DevTools port on 127.0.0.1 that any
    /// local process can reach). The pipe falls back to the port for the rest of the run if it gets no answer
    bool transportPipe = true;
    std::string home = "https://www.google.com/";  ///< "home"
    int fps = 30;                            ///< "fps": the screencast's frame rate at most (5–60)
    int viewportWidth = 1280;                ///< "width" / "height": the 16:9 size (720p / 1080p) the page's pixels follow
    int viewportHeight = 720;
    BrowserAspect aspect = BrowserAspect::Wide;  ///< "aspect": "16:9" | "4:3" | "1:1" | "9:16"
    bool mobile = false;                     ///< "mobile": the mobile version of sites (a phone's user agent)
    int zoomPct = 100;                       ///< "zoom_pct": the page's zoom (kBrowserZoomSteps)
    int jpegQuality = 70;                    ///< "jpeg_quality" (30–95)
    bool keepAudio = true;                   ///< "keep_audio": hidden, the page keeps playing (false: paused)
    bool pauseWhenRemoved = true;            ///< "pause_when_removed": the headset off pauses the page's video
    int idleShutdownMin = 10;                ///< "idle_shutdown_min": hidden and silent this long → Chrome closes (0 = never)
    double widthM = 0.45;                    ///< "width_m": the panel's width in the room (0.25–1.50 m)
    BrowserAnchor anchor = BrowserAnchor::Auto;  ///< "anchor": "auto" | "left_wrist" | "right_wrist" | "head"
    std::vector<std::string> extraArgs;      ///< "extra_args": more Chrome switches (experiments: GPU, video decode)
    HeadPose head = defaultBrowserHead();    ///< "head": its pose relative to the head
    WristPose leftWrist = defaultBrowserWrist();   ///< "left_wrist"
    WristPose rightWrist = defaultBrowserWrist();  ///< "right_wrist"

    bool operator==(const BrowserSettings& o) const {
        return chromeCommand == o.chromeCommand && headless == o.headless && transportPipe == o.transportPipe &&
               home == o.home &&
               fps == o.fps && viewportWidth == o.viewportWidth &&
               viewportHeight == o.viewportHeight && aspect == o.aspect && mobile == o.mobile &&
               zoomPct == o.zoomPct && jpegQuality == o.jpegQuality && keepAudio == o.keepAudio &&
               pauseWhenRemoved == o.pauseWhenRemoved &&
               idleShutdownMin == o.idleShutdownMin && widthM == o.widthM && anchor == o.anchor &&
               extraArgs == o.extraArgs && !(head != o.head) && !(leftWrist != o.leftWrist) &&
               !(rightWrist != o.rightWrist);
    }
    bool operator!=(const BrowserSettings& o) const { return !(*this == o); }
    /** The page's size in CSS px (= texture px): width × height's pixels in the aspect's shape (even numbers) */
    void pageSize(int& width, int& height) const;
};

/** @return "auto" / "left_wrist" / "right_wrist" / "head" */
const char* browserAnchorName(BrowserAnchor anchor);
/** @return "16:9" / "4:3" / "1:1" / "9:16" */
const char* browserAspectName(BrowserAspect aspect);

/**
 * アプリの設定一式。ファイルが無いときはこの既定値で動く。
 */
struct Config {
    bool visible = true;            ///< false でパネルを隠す（読み取りと描画も止める）
    bool updateCheck = true;        ///< "update_check": once a day, ask GitHub for the latest version (version_notice.h)
    /// "language": "auto" (the default: Steam's language, systemLanguage()) or "ko" / "en" / "ja". loadConfig
    /// resolves auto into language; a Config made here is Korean until then (tests, previews)
    bool languageAuto = true;
    Language language = Language::Ko;  ///< the language shown
    Attachment attachment = Attachment::Head;  ///< パネルの固定先（頭・左手・右手）
    double posX = -0.15;            ///< HMD 基準の位置（m）。右が +x
    double posY = -0.12;            ///< 上が +y
    double posZ = -0.50;            ///< 前が -z
    double yawDeg = 0.0;            ///< 左右の向き（度）。正でパネルの面が右（+x）を向く（-180〜180）
    double pitchDeg = 0.0;          ///< 上下の傾き（度）。正でパネルの面が上（+y）を向く（-90〜90）
    double rollDeg = 0.0;           ///< 画面内の回転（度）。正で面の側から見て反時計回り（-180〜180）
    WristPose leftWrist;            ///< 左手に固定するときの位置と向き
    WristPose rightWrist;           ///< 右手に固定するときの位置と向き
    bool wristFade = true;          ///< 手首のとき、パネルの面が目から外れるほど薄くして消す
    double wristFadeEndDeg = 75.0;  ///< 消えきる角度（度、35〜90）。その 30° 手前から薄くなり始める
    int clockFormat = 24;           ///< 性能パネルの時計（0 = 出さない、12 = 12 時間制、24 = 24 時間制。本体の時刻）
    double widthM = 0.20;           ///< 1 列（512 px）の幅（m）。パネルの幅は列の数 × これ（既定のレイアウトは 2 列）
    double alpha = 0.9;             ///< パネル全体の不透明度（0〜1）
    int updateIntervalMs = 500;     ///< 更新間隔（ms）
    int graphSeconds = 30;          ///< グラフに出す秒数
    std::string fontPath = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc";
    std::string boldFontPath = "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc";
    Thresholds thresholds;
    frame_perf_overlay::Layout layout = frame_perf_overlay::defaultLayout();  ///< the widget grid ("layout" key)
    bool layoutInFile = false;  ///< the file had a usable "layout" (false: first run, from before the grid, unreadable)
    bool layoutRejected = false;  ///< the file had an unusable "layout" (the next save keeps the file as .bad)
    std::string activeProfile;  ///< the profile last used / saved ("active_profile", e.g. "profile-2"); "" = none
    /// "templates_seeded": the built-in profiles (ProfileTemplate::id: "simple_a", "simple_b", "basic", "detailed")
    /// offered once each (seedTemplates); a deleted one isn't added again. An older file's `true` stands for
    /// legacySeededTemplates() (간단 A and 간단 B), `false` for none
    std::vector<std::string> templatesSeeded;
    /// "first_run_guide_done": the first-run guide (the card in front of the eyes) is not shown any more: the HUD was
    /// pressed once. A file without it (from before the guide) counts as done; only freshConfig() starts without
    bool firstRunGuideDone = true;
    DockSettings dock;  ///< "dock": in-game interaction and the Dock panel
    BrowserSettings browser;  ///< "browser": the browser panel
};

/**
 * The config of a fresh install (no config.json yet): the defaults, with the HUD on the left wrist (WristPose's
 * default pose: over the back of the hand, facing up, so turning the wrist brings it into view), the language
 * following Steam (systemLanguage()) and the first-run guide still to show.
 */
/** Where a fresh install (freshConfig) and "Reset placement" put the performance panel. */
constexpr Attachment kFreshAttachment = Attachment::LeftWrist;

Config freshConfig();

/** Collapse timeouts the Dock page steps through (seconds; 0 = never). */
constexpr int kDockCollapseSteps[] = {10, 20, 30, 60, 120, 0};

/** @return "auto" / "left" / "right" */
const char* dockHandName(DockHand hand);

/**
 * The placement a profile keeps: where the HUD sits and how it looks. Everything the Placement page sets except
 * show / hide; not the layout, the thresholds or anything else in config.json.
 */
struct Placement {
    Attachment attachment = Attachment::Head;  // the defaults are Config's
    double posX = -0.15, posY = -0.12, posZ = -0.50;
    double yawDeg = 0.0, pitchDeg = 0.0, rollDeg = 0.0;
    WristPose leftWrist;
    WristPose rightWrist;
    bool wristFade = true;
    double wristFadeEndDeg = 75.0;
    int clockFormat = 24;
    double widthM = 0.20;
    double alpha = 0.9;
};

/** @return the config's placement */
Placement placementOf(const Config& config);

/** Sets the config's placement (the rest of the config stays). */
void applyPlacement(const Placement& placement, Config& config);

/**
 * The placement as JSON members with config.json's keys and number format ("attachment", "position", "rotation",
 * "left_wrist", "right_wrist", "wrist_fade", "wrist_fade_end_deg", "clock_format", "width_m", "alpha"), one per line,
 * joined by ",\n", no trailing comma. saveConfig writes these same lines.
 * @param placement the placement
 * @param indent spaces before each line
 */
std::string placementMembers(const Placement& placement, int indent);

/** @return `{ <placementMembers> }`, the lines indented by indent + 2 and the closing brace by indent */
std::string placementToJson(const Placement& placement, int indent);

/**
 * Reads a placement object (placementToJson's). Missing keys keep Config's defaults; out-of-range values are clamped
 * and unknown keys noted, as in config.json.
 * @return false (out unchanged) if value isn't an object
 */
bool placementFromJson(const JsonValue& value, Placement& out, std::vector<std::string>& warnings, std::string& error);

/** @return true if the two placements are the same as saved (compared as placementMembers writes them) */
bool samePlacement(const Placement& a, const Placement& b);

/**
 * 既定の設定ファイルの場所を返す（$XDG_CONFIG_HOME か ~/.config の下）。
 * @return 設定ファイルのパス
 */
std::string defaultConfigPath();

/**
 * 設定ファイルに書く固定先の名前。
 * @param attachment 固定先
 * @return "head" / "left_wrist" / "right_wrist"
 */
const char* attachmentName(Attachment attachment);

/**
 * 今の固定先の手首の位置と向き（固定先が頭のときは左手のもの。呼ぶ側で頭かどうかを見ること）。
 * @param config 設定
 * @return 左手か右手の WristPose
 */
WristPose& selectedWrist(Config& config);

/**
 * 今の固定先の手首の位置と向き（読むだけ）。
 * @param config 設定
 * @return 左手か右手の WristPose
 */
const WristPose& selectedWrist(const Config& config);

/** config.json (and a profile file) is read up to this size; a larger one is refused, not read whole */
constexpr size_t kMaxConfigBytes = 4u << 20;

/**
 * 設定ファイルを読む。ファイルが無いときは freshConfig() を返して成功扱いにする。"language": "auto" は systemLanguage() に。
 * 値の範囲外は丸め、知らないキーは warnings に入れる。
 * @param path 設定ファイルのパス
 * @param out 読んだ設定の書き込み先（失敗時は変更しない）
 * @param warnings 気づいたこと（知らないキー、範囲外の値など）の追加先
 * @param error 読めなかったときの理由
 * @return 成功（またはファイルが無い）なら true
 */
bool loadConfig(const std::string& path, Config& out, std::vector<std::string>& warnings, std::string& error);

/**
 * 設定をファイルに保存する（同じフォルダに一時ファイルを書いてから置き換える）。
 * フォルダが無ければ作る。書くのはこの設定ファイルだけ。
 * @param path 設定ファイルのパス
 * @param config 保存する設定
 * @param error 失敗したときの理由
 * @return 保存できたら true
 */
bool saveConfig(const std::string& path, const Config& config, std::string& error);

/**
 * 表示まわり（表示の有無・固定先・位置・向き・両手の位置と向き・傾けると消す・時計・幅・透明度）だけ既定値に戻す。
 * 言語・しきい値・フォントはそのまま。
 * @param config 書き換える設定
 */
void resetDisplaySettings(Config& config);

/**
 * 回転行列を作る。パネル自身の軸で見て yaw → pitch → roll の順に回す（内因的な Y-X-Z の順）。
 * 行列では R = Ry(yaw) · Rx(−pitch) · Rz(roll)（右手系、列ベクトル）。pitch だけ符号を反転しているのは、
 * pitch を「正で面が上を向く」にそろえるため（X 軸まわりの右手の回転は正で面が下を向く）。
 * 回転なしのとき、パネルの表（+z 側）は親の +z を向く。頭のパネルと手首のパネル（wristTransform）で共通に使う。
 * @param yawDeg 左右の向き（度）。正で面が +x へ
 * @param pitchDeg 上下の傾き（度）。正で面が +y へ
 * @param rollDeg 画面内の回転（度）。正で面を見て反時計回り
 * @param r 書き込み先（r[行][列]）
 */
void panelRotation(double yawDeg, double pitchDeg, double rollDeg, double r[3][3]);

/**
 * 頭のパネルの回転行列（HMD 基準）を作る。性能パネルの変換は「位置 × この回転」。
 * 回転なしのとき、パネルの表（+z 側）は頭の方（HMD の +z = 後ろ）を向く。
 * @param config 設定（yawDeg / pitchDeg / rollDeg を使う）
 * @param r 書き込み先（r[行][列]）
 */
void panelRotation(const Config& config, double r[3][3]);

/**
 * 今の位置のまま、パネルの表が頭（HMD 基準の原点）を向くように yaw と pitch を決める。roll は今の値のまま残す
 * （ユーザーが合わせた傾きを消さない。Rz(roll) は (0,0,1) を動かさないので、roll があっても表の向きは同じ）。
 * 表の向き（法線）は R·(0,0,1) = (sin yaw·cos pitch, sin pitch, cos yaw·cos pitch) なので、
 * これが「パネルから原点への向き」−pos / |pos| と一致するように
 * yaw = atan2(−x, −z)、pitch = atan2(−y, √(x² + z²)) とする（0.1° 単位に丸める）。
 * @param config 書き換える設定
 */
void faceHead(Config& config);


/**
 * 設定ファイルの更新時刻を見て、変わっていたら読み直すための小さな監視役。
 */
class ConfigWatcher {
public:
    /**
     * @param path 監視する設定ファイルのパス
     */
    explicit ConfigWatcher(std::string path);

    /**
     * 最初の読み込み。ファイルが無い・壊れているときは既定値を使う。
     * @return 読み込んだ設定
     */
    Config loadInitial();

    /**
     * 更新時刻が変わっていたら読み直す。stat を 1 回呼ぶだけなので毎回呼んでよい。
     * @param config 現在の設定。変わったときだけ書き換える
     * @return 設定を読み直して書き換えたら true
     */
    bool reloadIfChanged(Config& config);

    /**
     * 自分で設定ファイルを書いた直後に呼ぶ。今のファイルの状態を覚えて、次の reloadIfChanged で読み直さないようにする。
     */
    void noteSaved();

    /** @return 監視しているパス */
    const std::string& path() const { return path_; }

    /** @return true if the file exists but the last load couldn't parse it (so don't overwrite it) */
    bool lastLoadFailed() const { return loadFailed_; }

    /**
     * Saves the config (saveConfig) and notes it (noteSaved). If the file on disk couldn't be parsed, or its "layout"
     * was rejected (the default layout replaced it), it's first renamed to `<path>.bad` (replacing an older one), so
     * the user's text survives and the save still persists.
     * Every save of the running app goes through here.
     * @param config what to save
     * @param error why it failed
     * @return true if saved
     */
    bool save(const Config& config, std::string& error);

private:
    std::string path_;
    bool existed_ = false;
    struct timespec mtime_ {};
    off_t size_ = 0;
    mutable bool loadFailed_ = false;
    mutable bool layoutRejected_ = false;  ///< the last load had a "layout" key that couldn't be used

    /**
     * 今のファイルの状態（有無・更新時刻・サイズ）を取る。
     * @param exists ファイルがあるか
     * @param mtime 更新時刻
     * @param size サイズ
     */
    void statFile(bool& exists, struct timespec& mtime, off_t& size) const;

    /**
     * 読み込んでログを出す共通処理。
     * @param config 書き込み先（失敗時は変えない）
     * @return 読めたら true
     */
    bool loadAndLog(Config& config) const;
};
