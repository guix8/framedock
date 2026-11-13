// SteamVR ダッシュボードに出す設定パネル（描画・ボタンの当たり判定・操作の反映）。
#pragma once

#include "autostart.h"
#include "color_i18n.h"  // src/: framecolor::Language
#include "config.h"
#include "hold_repeat.h"
#include "layout_editor.h"  // the Layout page
#include "profiles_ui.h"    // the Profiles page

#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

class DevicePage;
class FontSet;
struct Pen;
namespace framecolor {
class ColorPage;
}
struct UiText;
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

/** 設定パネルのボタンが表す操作。 */
enum class SettingsAction {
    None,
    ShowOn,
    ShowOff,
    AttachHead,   ///< 固定先を頭に
    AttachLeft,   ///< 固定先を左手（のコントローラー）に
    AttachRight,  ///< 固定先を右手（のコントローラー）に
    PresetLeftBottom,
    PresetCenterBottom,
    PresetRightBottom,
    PresetLeftTop,
    PresetRightTop,
    PresetWrist,  ///< 選んでいる手首の位置と向きを標準（WristPose の既定値）に戻す
    FadeOn,       ///< 手首のとき、傾けると消す
    FadeOff,      ///< 手首のとき、傾けても消さない
    FadeAngleDown,  ///< 消える角度を 5° 小さく（35° まで）
    FadeAngleUp,    ///< 消える角度を 5° 大きく（90° まで）
    SizeDown,
    SizeUp,
    AlphaDown,
    AlphaUp,
    ClockOff,     ///< 時計を出さない
    Clock12,      ///< 時計を 12 時間制に
    Clock24,      ///< 時計を 24 時間制に
    Reset,  ///< the Placement page: all of it back to the defaults (only comes back on the second tap)
    Quit,  ///< アプリを終了（2 回目の押下で確定したときだけ返る）
    AutostartOn,   ///< 自動起動を有効に（systemctl --user enable）
    AutostartOff,  ///< 自動起動を無効に（systemctl --user disable）
    // Dashboard pages
    PageLayout,      ///< show the layout editor (handled inside the panel)
    PagePlacement,   ///< show the placement cards (handled inside the panel)
    PageColor,       ///< show the Color page (FrameColor; handled inside the panel)
    // The Profiles page. The caller does them on selectedProfileId(); Delete, Use / Reset when they would throw away
    // unsaved changes (ProfilesUiState's useLoses / resetLoses), and Save when the live layout differs from the
    // profile (unsaved), only come back on the second tap
    PageProfiles,     ///< show the Profiles page (handled inside the panel)
    ProfileRow0,      ///< select row 0-7 (ProfileRow0 + i; handled inside the panel)
    ProfileRow1,
    ProfileRow2,
    ProfileRow3,
    ProfileRow4,
    ProfileRow5,
    ProfileRow6,
    ProfileRow7,
    ProfileNew,       ///< save the live layout as a new profile
    ProfileUse,       ///< load the selected profile as the live layout
    ProfileSave,      ///< overwrite the active profile with the live layout
    ProfileRename,    ///< SteamVR's keyboard for the selected profile's name
    ProfileDelete,    ///< delete the selected profile
    ProfileReset,     ///< live layout -> the built-in default (profiles untouched)
    // The footer's quick switch: one tap uses the profile, two when that would throw away unsaved changes (its item's
    // useLoses; the first tap arms it and says so on the footer). The caller does it on quickProfileId()
    QuickProfile0,    ///< the first profile in the strip (QuickProfile0 + i)
    QuickProfile1,
    QuickProfile2,
    QuickProfile3,
    QuickProfile4,
    QuickProfile5,
    // The Dock page (in-game interaction; config.dock)
    PageDock,         ///< show the Dock page (handled inside the panel)
    DockOn,           ///< the HUD takes the laser in games
    DockOff,
    DockHandAuto,     ///< the pointing hand: the one without the HUD
    DockHandLeft,
    DockHandRight,
    DockSizeDown,     ///< the Dock panel 5 cm narrower
    DockSizeUp,
    DockTimeoutDown,  ///< collapse sooner (kDockCollapseSteps)
    DockTimeoutUp,    ///< later, then never
    DockResetPlace,   ///< the Dock panel's offsets (head, both wrists) back to the defaults
    // The Browser page (the browser panel; config.browser)
    PageBrowser,          ///< show the Browser page (handled inside the panel)
    BrowserFpsDown,       ///< kBrowserFpsSteps
    BrowserFpsUp,
    Browser720p,          ///< the page at 1280×720 (kBrowser720p)
    Browser1080p,         ///< 1920×1080: sharper, smaller text, more CPU
    BrowserHeadless,      ///< Chrome without a window (the next start)
    BrowserWindow,        ///< a normal window far off screen (the next start)
    BrowserKeepAudioOn,   ///< hidden: the page keeps playing
    BrowserKeepAudioOff,  ///< hidden: its media pauses
    BrowserRemovedKeepOn,  ///< the headset off: the video keeps playing
    BrowserRemovedPause,   ///< the headset off: the video pauses (the default)
    BrowserIdleDown,      ///< kBrowserIdleSteps
    BrowserIdleUp,
    BrowserAnchorAuto,
    BrowserAnchorLeft,
    BrowserAnchorRight,
    BrowserAnchorHead,
    BrowserResetPlace,    ///< the panel's offsets (head, both wrists) and size back to the defaults
    BrowserHomeFromPage,  ///< the page showing now becomes the home page (the caller: it knows the URL)
    BrowserStopChrome,    ///< close Chrome now (the caller; a second tap, the first arms it)
    BrowserStartChrome,   ///< in its place while Chrome isn't running: start it and show the panel (the caller)
    // The 기기 page (sound, passthrough, the PC's account; DevicePage does its controls)
    PageDevice,           ///< show the 기기 page (handled inside the panel)
    // The header's language button: auto -> ko -> en -> ja -> auto (nextLanguageChoice), saved at once
    LanguageCycle,
    // The footer's bottom line, right: the daily new version check (config.updateCheck)
    UpdateCheckOn,
    UpdateCheckOff,
};

/** Which page the dashboard tab shows. */
enum class SettingsPage { Layout, Placement, Profiles, Color, Dock, Browser, Device };

/** What the Browser page shows about the running Chrome (the caller fills it in). */
struct BrowserPageInfo {
    std::string url;      ///< the page showing now ("" = none)
    std::string status;   ///< e.g. "실행 중", "꺼짐" (browserloop::statusLine)
    bool running = false; ///< Chrome runs (its stop button can be pressed)
    bool sameAs(const BrowserPageInfo& o) const { return url == o.url && status == o.status && running == o.running; }
};

/** The Color page's language for the app's language (one setting for the whole app). */
framecolor::Language colorLanguage(Language language);

/**
 * 操作を設定に反映する（Quit と None は何もしない）。位置のボタンは、今の固定先（頭・左手・右手）の値を動かす。
 * @param action 操作
 * @param config 書き換える設定
 * @return 設定が変わったら true
 */
bool applySettingsAction(SettingsAction action, Config& config);

/**
 * 設定パネルの画像を描き、レーザーポインターの位置からボタンを判定する係。
 */
class SettingsPanel {
public:
    /** 「アプリを終了」を 1 回押してから、確定の 2 回目を待つ秒数。 */
    static constexpr double kQuitConfirmSec = 3.0;

    /**
     * @param fonts 使うフォント（このオブジェクトより長く生きていること）
     */
    explicit SettingsPanel(const FontSet& fonts);
    ~SettingsPanel();
    SettingsPanel(const SettingsPanel&) = delete;
    SettingsPanel& operator=(const SettingsPanel&) = delete;

    /**
     * 今の設定と、ポインターが乗っている・押しているボタンの状態でパネルを描く。
     * @param config 今の設定（言語もここから）
     * @param autostart 自動起動の今の状態
     */
    void render(const Config& config, const AutostartStatus& autostart,
                const ProfilesUiState& profiles = ProfilesUiState());

    /**
     * Hands the editor the current layout. Call before each batch of pointer events, so a press works on the
     * layout the user sees (a file reload may have changed it since the last draw).
     * @return true if it differed from what the tab shows (redraw it)
     */
    bool syncLayout(const frame_perf_overlay::Layout& layout) { return editor_.sync(layout); }

    /**
     * ポインターが動いた。
     * @param x 左端からの px
     * @param y 上端からの px
     * @return 乗っているボタンが変わった（描き直しが要る）なら true
     */
    bool pointerMove(double x, double y);

    /**
     * Smooth scroll on the tab (VREvent_ScrollSmooth), over the editor's lanes.
     * @param dy the event's ydelta
     * @return true if a redraw is needed
     */
    bool scroll(double dy);

    /** Takes the editor's queued edit (base version + edited layout). @return false if none */
    bool takeLayoutCommit(uint32_t& baseVersion, frame_perf_overlay::Layout& layout) {
        return editor_.takeCommit(baseVersion, layout);
    }

    /** How the edit taken last went (shown in the editor). */
    void layoutCommitResult(LayoutEditor::CommitOutcome outcome) { editor_.commitResult(outcome); }

    /** @return true while the editor holds a press or a drag (poll the pointer at the fast rate) */
    bool editorBusy() const { return editor_.busy(); }
    /** The Layout page offers 녹화 (framecorder is installed). */
    void setRecordOffered(bool offered) { editor_.setRecordOffered(offered); }

    /** Switches the page (tests, --dump-settings-png). Lets go of a held button. */
    void showPage(SettingsPage page) {
        if (page != page_) stopRepeat();
        page_ = page;
    }

    /**
     * Whether an action repeats while its button is held: the − / ＋ of size, opacity and the fade angle.
     */
    static bool repeatsWhileHeld(SettingsAction action);

    /**
     * The held button's next repeat. pointerDown returned the action once already; call this from the main loop
     * (at nextRepeatAt() or later) and apply what it returns like a press.
     * @param now monotonic seconds (the clock pointerDown got)
     * @return the held action when a repeat is due, else None
     */
    SettingsAction takeRepeat(double now) {
        takeDeviceRepeat(now);  // the 기기 page's held − / ＋ (it changes the shared DeviceModel itself)
        if (repeat_.due(now)) return repeatAction_;
        if (!repeat_.active()) repeatAction_ = SettingsAction::None;  // the safety stop
        return SettingsAction::None;
    }
    /**
     * takeRepeat, but when a repeat is due it first asks stillVisible() (the dashboard tab as SteamVR has it now, not
     * as of the last 2 Hz check): a hidden tab stops the repeat at once.
     */
    SettingsAction takeRepeat(double now, const std::function<bool()>& stillVisible) {
        if (now < nextRepeatAt() - 1e-6) return SettingsAction::None;
        if (!stillVisible()) {
            stopRepeat();
            stopDeviceRepeat();
            return SettingsAction::None;
        }
        return takeRepeat(now);
    }
    /** A pointer event came in: a held button's safety stop (HoldRepeat::kIdleStopSec) counts from now. */
    void pointerSeen(double now) {
        repeat_.touch(now);
        deviceSeen(now);
    }
    /** @return when the held button repeats next (+infinity when none is held): wake up then */
    double nextRepeatAt() const { return std::min(repeat_.nextAt(), deviceRepeatAt()); }
    /** @return true while a repeating button is held (poll fast, hold the config save back) */
    bool repeating() const { return repeat_.active(); }

    /**
     * The Color page (FrameColor). Without one the page stays empty. The page outlives this panel.
     * @param page the page, or null
     */
    void setColorPage(framecolor::ColorPage* page) { colorPage_ = page; }
    /**
     * The 기기 page (sound, passthrough, the PC's account). Without one the page stays empty. The page outlives this
     * panel; the dashboard tab and the Dock's full view each have their own on one shared DeviceModel.
     */
    void setDevicePage(DevicePage* page) { devicePage_ = page; }
    /** The Browser page's line about Chrome. @return true if it changed (redraw) */
    bool setBrowserInfo(const BrowserPageInfo& info) {
        if (info.sameAs(browserInfo_)) return false;
        browserInfo_ = info;
        // 끄기 ↔ 켜기 in the same place (not laid out again: the header and footer buttons are only placed by render)
        const SettingsAction chrome = info.running ? SettingsAction::BrowserStopChrome : SettingsAction::BrowserStartChrome;
        for (Button& b : buttons_) {
            if (b.action == SettingsAction::BrowserStopChrome || b.action == SettingsAction::BrowserStartChrome) {
                b.action = chrome;
            }
        }
        return true;
    }
    /** @return the page shown */
    SettingsPage page() const { return page_; }
    /**
     * A newer release (update_check: "v0.1.1"; "" = none), said on the footer's bottom line. Only the dashboard
     * tab's own panel gets one (no notice on the wrist). @return true if it changed (redraw)
     */
    bool setUpdateNotice(const std::string& tag) {
        if (tag == updateTag_) return false;
        updateTag_ = tag;
        return true;
    }
    /**
     * This panel is the Dock's full view: 표시 끔 and 게임 중 조작 끔 take a second tap there (they take the Dock away,
     * and only the dashboard tab brings it back). The dashboard tab's own panel leaves this off.
     */
    void setInDock(bool inDock) { inDock_ = inDock; }

    /** Most profiles the footer's quick switch shows (the first ones in the list). */
    static constexpr size_t kQuickProfiles = 6;

    /**
     * @param action QuickProfile0..QuickProfile5
     * @return the profile that quick-switch button stood for when last drawn ("" = none)
     */
    std::string quickProfileId(SettingsAction action) const;

    /** @return the profile selected on the Profiles page ("" = none) */
    const std::string& selectedProfileId() const { return selectedProfile_; }
    /** Selects a profile (tests, --dump-settings-png) */
    void selectProfile(const std::string& id) { selectedProfile_ = id; }

    /**
     * The profiles' state right now. Call before each pointerDown: Use / Reset ask for a second tap on what they would
     * throw away now (the selected item's useLoses, resetLoses), not on the state the page was last drawn with (a
     * file reload may have come in since). What's drawn (rows, quick switch) stays until the next render.
     */
    void setProfilesState(const ProfilesUiState& profiles) { lastProfiles_ = profiles; }

    /** How long a profile action's result stays on the footer's bottom line (an error: kActionErrorSec). */
    static constexpr double kActionStatusSec = 5.0;
    static constexpr double kActionErrorSec = 10.0;

    /**
     * A profile action's result (the Profiles page's buttons, the footer's quick switch, a rename) on the footer's
     * bottom line, on every page, for kActionStatusSec (an error kActionErrorSec; tick() takes it away). Below
     * autostart's messages and a Color page error, above the Color page's other messages and the usual note.
     * @param text the message ("" = take the current one away)
     * @param error drawn red
     * @param now monotonic seconds
     */
    void showActionStatus(const std::string& text, bool error, double now);
    /** @return the footer's bottom line as last drawn (tests) */
    const std::string& footerNote() const { return footerNote_; }

    /** The centre of a button as last laid out. @return false if it isn't on the tab */
    bool buttonCenter(SettingsAction action, double& x, double& y) const {
        const Button* b = findButton(action);
        if (b == nullptr) return false;
        x = b->x + b->w / 2;
        y = b->y + b->h / 2;
        return true;
    }

    /** @return the editor (tests) */
    const LayoutEditor& editor() const { return editor_; }

    /**
     * ボタンが押された。「アプリを終了」は 1 回目で確認の表示に変わり、kQuitConfirmSec 秒以内の 2 回目で Quit を返す。
     * @param x 左端からの px
     * @param y 上端からの px
     * @param now 現在時刻（秒、単調増加）
     * @return 押されたボタンの操作（ボタンの外・終了の 1 回目なら None）
     */
    SettingsAction pointerDown(double x, double y, double now);

    /**
     * ボタンが離された。Also ends an editor drag (a drop may queue an edit: takeLayoutCommit).
     * @param x 左端からの px
     * @param y 上端からの px
     * @return 押している表示を消した（描き直しが要る）なら true
     */
    bool pointerUp(double x, double y);

    /**
     * ポインターがパネルから外れた。
     * @return 描き直しが要るなら true
     */
    bool pointerLeave();

    /**
     * 時間で変わる表示（終了の確認の期限切れ、プロファイルの操作の結果の表示の期限切れ）を進める。
     * @param now 現在時刻（秒、単調増加）
     * @return 描き直しが要るなら true
     */
    bool tick(double now);

    /**
     * 描いた画像を OpenVR 用の非乗算済み RGBA にして返す。
     * @return width() * height() * 4 バイト
     */
    const std::vector<uint8_t>& toRgba();

    /** @return cairo's premultiplied ARGB32 bytes as drawn (width() * height() * 4; for the B8G8R8A8 texture) */
    const uint8_t* pixels() const;

    /**
     * 描いた画像を PNG で保存する。
     * @param path 保存先
     * @return 保存できたら true
     */
    bool writePng(const std::string& path) const;

    /**
     * 見た目の確認用に「もう一度押すと終了」の状態にする（--dump-settings-png 用）。
     */
    void armQuitForPreview();

    /** @return the last render (cairo's premultiplied ARGB32), for the Dock panel's full view */
    cairo_surface_t* surface() const { return surface_; }

    /** @return 画像の幅（px） */
    int width() const;
    /** @return 画像の高さ（px） */
    int height() const;

private:
    /** ボタン 1 個。文言は描くときに言語の表から引く。 */
    struct Button {
        SettingsAction action;
        double x, y, w, h;
    };

    const FontSet& fonts_;
    std::string updateTag_;  ///< setUpdateNotice
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
    std::vector<uint8_t> rgba_;
    std::vector<Button> buttons_;
    /** Which set of card buttons is laid out (was wristLayout_, a bool). */
    enum class ButtonMode { Layout, Head, Wrist, Profiles, Color, Dock, Browser, Device };
    ButtonMode mode_ = ButtonMode::Head;  ///< 今のボタンの配置（固定先・ページが変わったら render() で置き直す）
    SettingsPage page_ = SettingsPage::Placement;  ///< The tab opens on Placement, the first page
    LayoutEditor editor_;                       ///< the Layout page
    framecolor::ColorPage* colorPage_ = nullptr;  ///< the Color page (not owned)
    DevicePage* devicePage_ = nullptr;            ///< the 기기 page (not owned)
    BrowserPageInfo browserInfo_;                ///< the Browser page's line about Chrome
    SettingsAction hover_ = SettingsAction::None;
    bool autostartInstalled_ = true;  ///< 最後に描いたときユニットがあったか（無ければ自動起動のボタンは押せない）
    SettingsAction pressed_ = SettingsAction::None;
    bool quitArmed_ = false;
    double quitArmedUntil_ = 0.0;
    // The Profiles page
    std::string selectedProfile_;                    ///< "" = none
    std::vector<std::string> profileRowIds_;         ///< the ids of the rows as last drawn
    std::vector<std::string> quickIds_;              ///< the ids of the footer's quick switch as last drawn
    ProfilesUiState lastProfiles_;                   ///< as last drawn (the two-tap rule needs `unsaved`)
    /// Waiting for its second tap: the Profiles page's Use / Reset / Delete / Save, a quick switch, the Placement
    /// page's Reset, and on the Dock (inDock_) 표시 끔 / 게임 중 조작 끔
    SettingsAction profileArmed_ = SettingsAction::None;
    bool inDock_ = false;  ///< the Dock panel's full view (setInDock)
    std::string quickArmedId_;                       ///< the profile the armed quick-switch button stood for
    double profileArmedUntil_ = 0.0;
    std::vector<SettingsAction> profileDisabled_;    ///< Profiles page buttons drawn disabled (not hittable)
    std::string actionStatus_;         ///< showActionStatus: the message, "" = none
    bool actionStatusError_ = false;
    double actionStatusUntil_ = 0.0;
    std::string footerNote_;           ///< the bottom line as last drawn
    HoldRepeat repeat_;                ///< the held button's timing (repeatsWhileHeld)
    SettingsAction repeatAction_ = SettingsAction::None;  ///< the held button

    /** The 기기 page's held − / ＋ (DevicePage is only declared here). */
    void takeDeviceRepeat(double now);
    double deviceRepeatAt() const;
    void deviceSeen(double now);
    void stopDeviceRepeat();

    /** Lets go of the held button (release, leave, page switch, a new press). */
    void stopRepeat() {
        repeat_.stop();
        repeatAction_ = SettingsAction::None;
    }

    /**
     * 3 つのカードのボタンの配置を作る（起動時と、固定先が頭と手首のあいだで変わったとき。下の段と更新の帯は
     * render() のたびに置き直す）。手首のときは、位置のボタンが「手首の標準の位置」だけになり、向きのカードの
     * いちばん下の段が「自分に向ける / 正面向き」から「傾けると消す」「消える角度」に変わる。
     */
    void layoutButtons();

    /**
     * 座標にあるボタンを探す。
     * @param x 左端からの px
     * @param y 上端からの px
     * @return ボタンの操作。無ければ None
     */
    SettingsAction hitTest(double x, double y) const;

    /**
     * ボタンの文言を言語の表から引く。
     * @param action ボタンの操作
     * @param text 言語の表
     * @return 文言
     */
    std::string labelOf(SettingsAction action, const UiText& text) const;

    /**
     * 操作からボタンを探す。
     * @param action 操作
     * @return ボタン。無ければ nullptr
     */
    const Button* findButton(SettingsAction action) const;

    /**
     * 下の段・更新の帯のボタンを置く（文言の幅が言語や状態で変わるので、描くたびに位置を決め直す）。
     * 既にあるボタンは位置を置き直すだけ、無ければ足す。
     * @param action 操作
     * @param x 左
     * @param y 上
     * @param w 幅
     * @param h 高さ
     */
    void placeFooterButton(SettingsAction action, double x, double y, double w, double h);

    /**
     * セグメント切り替え（丸い枠のピルの中で、選択中がアクセントの塗り ＋ ✓）を描く。2 択でも 3 択でも使う。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param actions 左から順の操作（ボタンとして置いてあること）
     * @param selected 選択中の番号（0 から。-1 = どれでもない）
     * @param usable 押せるか（押せないときは枠なし・薄い文字）
     */
    void drawSegmented(const Pen& pen, const UiText& text, std::initializer_list<SettingsAction> actions, int selected,
                       bool usable) const;

    /**
     * 1 つのボタン（位置・− / ＋・既定に戻す）を描く。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param action 操作
     * @param selected 選ばれている（位置のボタン）ならアクセントの塗り ＋ ✓
     * @param size 文字の大きさ（入りきらなければ小さくする）
     * @param check selected のとき ✓ を付けるか（false なら塗りだけの強調ボタン。更新の帯の［更新する］）
     */
    void drawButton(const Pen& pen, const UiText& text, SettingsAction action, bool selected, double size,
                    bool check = true) const;

    /**
     * 見出しと、右上の状態のピル（● パネル表示中 / ○ パネル非表示）を描く。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     * @return 状態のピルの左端の x（言語のボタンはこの左に置く）
     */
    double drawHeader(const Pen& pen, const UiText& text, const Config& config) const;

    /**
     * The language button between the page switch and the status pill (a globe, "자동 · 한국어" / "English"). Places
     * its button, so not const. Not on the Color page: its compare buttons have that place.
     */
    void drawLanguageButton(const Pen& pen, const Config& config, double left, double right);

    /**
     * The page switch (Placement | Layout | Profiles | Color | Dock | Browser | Device) next to the title. Places its buttons, so not const.
     * @return its right edge
     */
    double drawPageSwitch(const Pen& pen, const UiText& text, double left);

    /**
     * @return what Use / Reset / a quick-switch button would throw away (the selected profile's useLoses, resetLoses,
     *         the button's profile's useLoses)
     */
    LiveLoss armedLoss(SettingsAction action) const;

    /** The Dock page: in-game interaction, the Dock panel, how to use it. */
    void drawDockPage(const Pen& pen, const UiText& text, const Config& config) const;
    /** The Browser page: the picture, Chrome, home and bookmarks (its words are app_text's). */
    void drawBrowserPage(const Pen& pen, const UiText& text, const Config& config) const;

    /** The Profiles page: the list (left) and the selected profile's actions (right). Places its buttons. */
    void drawProfilesPage(const Pen& pen, const UiText& text, const ProfilesUiState& profiles);
    /** A Profiles page button; a disabled one is drawn faded and isn't hittable */
    void drawProfileButton(const Pen& pen, const UiText& text, SettingsAction action, double x, double y, double w,
                           bool enabled, bool primary);

    /**
     * 左のカード「パネル」（表示・大きさ・透明度・時計・既定に戻す）を描く。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     */
    void drawPanelCard(const Pen& pen, const UiText& text, const Config& config) const;

    /**
     * 真ん中のカード「位置」（固定先・位置のボタン・レーザーでの動かし方）を描く。細かい位置と向きはレーザーで
     * 動かすので、ここにはボタンが無い。頭のときは位置のボタンが 5 つ（左上・右上・左下・中央下・右下）、手首のときは
     * 「手首の標準の位置」の 1 つ。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     */
    void drawPositionCard(const Pen& pen, const UiText& text, const Config& config) const;

    /**
     * 右のカード「手首で隠す」（傾けると消す・消える角度・その説明）を描く。頭のときは使わないので、薄く描いて押せない。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     */
    void drawFacingCard(const Pen& pen, const UiText& text, const Config& config) const;

    /**
     * 下の段（プロファイルの切り替え・自動起動・終了）と、いちばん下の 1 行の説明を描く。lastProfiles_ を使う。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param autostart 自動起動の今の状態
     */
    void drawFooter(const Pen& pen, const UiText& text, const AutostartStatus& autostart, bool updateCheck);

};

/**
 * The dashboard thumbnail (also the launcher icon), the same in every language: Perf's graph with FrameColor's RGB discs and "Dock".
 * @param fonts 使うフォント
 * @param size 一辺の px
 * @param rgba 非乗算済み RGBA の書き込み先
 * @param pngPath 空でなければ PNG にも保存する
 */
void renderThumbnail(const FontSet& fonts, int size, std::vector<uint8_t>& rgba, const std::string& pngPath = "");
