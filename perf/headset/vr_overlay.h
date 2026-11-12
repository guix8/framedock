// OpenVR との接続、頭か手首（コントローラー）に固定する性能パネル、ダッシュボードの設定パネル、フレーム時間の取得。
#pragma once

#include "config.h"
#include "dock.h"
#include "dock_loop.h"
#include "dock_math.h"
#include "history.h"
#include "placement.h"
#include "vk_texture.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

/** ダッシュボードの設定パネルへのポインター操作（座標は画像の左上が原点の px）。 */
struct PointerInput {
    enum class Type { Move, Down, Up, Leave, Scroll };  // Scroll: VREvent_ScrollSmooth（レイアウトの編集の列）
    Type type;
    double x = 0.0;
    double y = 0.0;
    double scrollDy = 0.0;  ///< Scroll の ydelta
    double scrollDx = 0.0;  ///< Scroll's xdelta (the browser panel)
    uint32_t device = 0xffffffff;  ///< the event's trackedDeviceIndex: the controller whose laser it is (or invalid)
    bool secondary = false;  ///< Down / Up of the laser mouse's right button (the browser panel only)
};

/**
 * 設定パネル（ダッシュボードのこのアプリのタブ）が見えているかの材料。
 */
struct SettingsVisibility {
    bool dashboardVisible = false;  ///< IVROverlay::IsDashboardVisible()（ダッシュボード全体が開いている）
    bool activeTab = false;         ///< IVROverlay::IsActiveDashboardOverlay(設定パネル)（このアプリのタブが選ばれている）
    bool overlayVisible = false;    ///< IVROverlay::IsOverlayVisible(設定パネル)

    /**
     * 実際に見えているか。ダッシュボード全体が開いているだけでは見えている扱いにしない。
     * @return 3 つとも true なら true
     */
    bool visible() const { return dashboardVisible && activeTab && overlayVisible; }
};

/** pollEvents() の結果。 */
struct VrEvents {
    bool quit = false;                  ///< SteamVR から終了を求められた（VREvent_Quit。SteamVR 自体が終わる）
    bool closeRequested = false;        ///< ダッシュボードのアイコンの「閉じる」が押された（VREvent_OverlayClosed）
    std::vector<PointerInput> pointer;  ///< 設定パネルへの操作（届いた順）
    std::vector<PointerInput> hudPointer;   ///< the laser on the HUD (in-game interaction), HUD texture px
    std::vector<PointerInput> dockPointer;  ///< the laser on the Dock panel, its texture px
    std::vector<PointerInput> browserPointer;   ///< the laser on the browser panel, its texture px
    std::vector<PointerInput> keyboardPointer;  ///< the laser on the VR keyboard, its texture px
    bool keyboardDone = false;          ///< Done on the keyboard showKeyboard() opened; keyboardText has the text
    std::string keyboardText;
    bool keyboardClosed = false;        ///< the keyboard closed without Done
    int roomView = -1;                  ///< VREvent_RoomViewShown (1) / Hidden (0), the last of them; -1 = none
    bool sceneAppChanged = false;       ///< VREvent_SceneApplicationChanged arrived: re-query the scene app

    /** イベントを捨て、次の読み取りに配列の容量を再利用する。 */
    void clear() {
        quit = closeRequested = keyboardDone = keyboardClosed = sceneAppChanged = false;
        roomView = -1;
        pointer.clear();
        hudPointer.clear();
        dockPointer.clear();
        browserPointer.clear();
        keyboardPointer.clear();
        keyboardText.clear();
    }
};

/** The app drawing the scene (IVRApplications). */
struct SceneApp {
    uint32_t pid = 0;  ///< 0 = none (the compositor is showing)
    std::string key;   ///< GetApplicationKeyByProcessId, or "process.<comm>" for a process with no app key
    std::string name;  ///< VRApplicationProperty_Name_String, may be empty
};

/**
 * OpenVR のオーバーレイアプリとしての接続をまとめたクラス。
 * SteamVR の設定は変えない（オーバーレイを作って出すだけ）。
 * 画像は SetOverlayRaw ではなく Vulkan の画像を SetOverlayTexture で渡す。
 */
class VrOverlay {
public:
    /** connect() の結果。 */
    enum class ConnectResult {
        Ok,          ///< つながった
        NotRunning,  ///< SteamVR が起動していない（待って再試行する）
        Error,       ///< それ以外の失敗
    };

    VrOverlay();
    ~VrOverlay();
    VrOverlay(const VrOverlay&) = delete;
    VrOverlay& operator=(const VrOverlay&) = delete;

    /**
     * SteamVR が起動していればオーバーレイアプリとしてつなぎ、性能パネル・Vulkan・設定パネルを用意する。
     * 起動していないときに SteamVR を立ち上げてしまわないよう、先に Background 型で有無を確かめる。
     * @param panelWidth 性能パネルの画像の幅（px）
     * @param panelHeight 性能パネルの画像の高さ（px）
     * @param settingsWidth 設定パネルの画像の幅（px）
     * @param settingsHeight 設定パネルの画像の高さ（px）
     * @param message 失敗したときの理由
     * @return 接続結果
     */
    ConnectResult connect(int panelWidth, int panelHeight, int settingsWidth, int settingsHeight,
                          std::string& message);

    /**
     * 診断用: Background 型でだけつなぐ（オーバーレイも Vulkan も作らないので、常駐しているインスタンスと
     * ぶつからない。SteamVR が起動していなければ起動させずに失敗する）。
     * @param message 失敗したときの理由
     * @return つながったら true
     */
    bool connectReadOnly(std::string& message);

    /**
     * 安全な順番で片付ける: 隠す → テクスチャを外す → オーバーレイを消す → コンポジタの数フレーム分待つ
     * → VR_Shutdown → Vulkan の画像とデバイスを壊す。各 API の戻り値はログに出す。
     */
    void shutdown();

    /**
     * 固定先・位置・向き・幅・透明度・表示の有無を性能パネルに反映する。
     * @param config 反映する設定
     */
    void applyConfig(const Config& config);

    /**
     * 性能パネルの置き場所と見え方を反映する（性能パネルの描画とは別の周期で呼ぶ）。
     * 手首のときはコントローラーと頭の姿勢を読んで、面が目から外れる角度に合わせて薄くし、トラッキングが外れたらすぐ隠す。
     * 変わったところだけ OpenVR に渡し、断られた操作は次の呼び出しでやり直す。次に呼ぶ時刻は nextPlacementAt() で分かる。
     * @param config 今の設定
     * @param now 今の時刻（秒、単調増加）
     */
    void updatePlacement(const Config& config, double now);

    /**
     * 次に updatePlacement() を呼ぶ時刻。手首のときはフェードの途中なら 1/30 秒後、見えきっている・消えきっているなら
     * 0.1 秒後。頭のときや隠しているときは、やり直しが要るときだけ（それ以外は無限大）。
     * @return 時刻（秒、単調増加）
     */
    double nextPlacementAt() const { return nextPlacementAt_; }

    /**
     * Whether the panel is really shown: connected, and OpenVR took the last ShowOverlay. False while it's hidden by
     * the setting, by the wrist fade or lost tracking, or at alpha 0 (updatePlacement keeps checking, so it comes
     * back by itself).
     */
    bool panelShown() const { return connected_ && panelHandle_ != 0 && presentation_.shown(); }

    /**
     * --verbose のとき true にする。applyConfig のたびに、少し後の pollEvents で
     * パネルの向きの確かめ（logFacingCheck）をログに出す。
     * @param verbose 出すなら true
     */
    void setVerbose(bool verbose) { verbose_ = verbose; }

    /**
     * たまったイベントを処理する。SteamVR の終了（VREvent_Quit）には AcknowledgeQuit_Exiting で応える。
     * @param includeSystem false なら設定パネルのイベント（マウス・閉じる）だけ見る（設定パネルが見えている間の
     *                      細かい周期用）。true なら SteamVR 全体と性能パネルのイベントも見る
     * @param result 終了要求と設定パネルへの操作。前の結果は消し、配列の容量は再利用する
     */
    void pollEvents(VrEvents& result, bool includeSystem = true, bool includeDock = false, bool includeDashboard = true);
    /**
     * The dashboard's state as the loop read it (each wake while a panel of ours can take its laser): while open,
     * every overlay of ours is polled each wake, and for dockloop::kEventDrainSec after it closed.
     */
    void noteDashboardOpen(bool open, double now) { polling_.setDashboardOpen(open, now); }
    /** @return true if this wake should poll our overlays anyway (the dashboard open, a drain after the laser left) */
    bool overlayEventsWanted(double now) const { return polling_.wanted(now); }
    /** @return true while a drain runs: the loop wakes often enough to deliver an Up or Leave left behind */
    bool overlayEventsDraining(double now) const { return polling_.draining(now); }

    /** @return true once the Vulkan device was lost (VK_ERROR_DEVICE_LOST): the app exits so systemd restarts it */
    bool deviceLost() const { return vulkan_.lost(); }
    /**
     * Frees the textures of the Dock panel, the browser panel and the keyboard once hidden for kIdleTextureSec (their
     * next submit makes new ones), and destroys stuck textures the GPU has let go of. Call it now and then (2 Hz).
     */
    void releaseIdleTextures(double now);
    static constexpr double kIdleTextureSec = dockloop::kIdleTextureSec;

    // ---- The Dock (in-game interaction) ----
    /**
     * Reads the poses the Dock needs: the HMD and both hands, with each hand's laser (the render model's "tip"
     * component, cached per device and model; Frametop's LaserPose). Reuses the poses updatePlacement read within the
     * last 20 ms instead of asking again.
     */
    DockPoses readDockPoses(double now);
    /**
     * The HUD's pose in the room as of the last readDockPoses (its anchor's pose × its offset, or the grabbing
     * controller's × the grab).
     * @return false if it isn't placed or its anchor isn't tracked
     */
    bool hudWorld(dockmath::Mat34& world) const;
    /** @return the HUD's size in the room (m): width_m × its columns, and the height from its texture */
    double hudWidthM() const { return configWidthM_ * panelColumns_; }
    double hudHeightM() const {
        return panelTextureWidth_ > 0 ? hudWidthM() * panelTextureHeight_ / panelTextureWidth_ : 0.0;
    }
    /**
     * Whether the HUD takes laser input (the Mouse input method with its mouse scale at the texture size, so SteamVR's
     * hit area is the panel). Only while a hand points at it: otherwise None, so the dashboard's laser passes through
     * it as before.
     */
    void setHudInput(bool on);
    /**
     * Turns SteamVR's laser on for a panel (VROverlayFlags_MakeOverlaysInteractiveIfVisible) while a hand points at
     * it, off when it points away: the game has that controller back. Only changes are sent.
     */
    void setInteractive(DockTarget target, bool on);
    /** Carries the HUD with a controller (relative transform; SteamVR keeps it there) until releaseHud(). */
    void grabHud(uint32_t device, const dockmath::Mat34& offset);
    /** Back to the config's placement (call applyConfig after changing it). */
    void releaseHud();
    /** @return true if the Dock panel overlay exists */
    bool hasDock() const { return dockHandle_ != 0; }
    /**
     * The Dock panel's picture. A new size (quick ↔ full) gets a new texture (the old one is freed on the next submit)
     * and a new width and mouse scale.
     * @param pixels cairo's premultiplied ARGB32
     * @param widthM the overlay's width (m)
     */
    bool submitDock(const uint8_t* pixels, int width, int height, double widthM);
    void showDock(bool show);
    /**
     * Attaches the Dock panel to a device (its anchor, or the grabbing controller). Only changes are sent.
     * @return false if it isn't placed there: no such device (a wrist's controller off), or SteamVR refused
     */
    bool placeDockRelative(uint32_t device, const dockmath::Mat34& local);
    /** Freezes the Dock panel in the room (the fine-adjust session). */
    void placeDockAbsolute(const dockmath::Mat34& world);
    /** @return the Dock panel's pose in the room as of the last readDockPoses (false: not placed / not tracked) */
    bool dockWorld(dockmath::Mat34& world) const;
    double dockWidthM() const { return dockWidthM_; }
    double dockHeightM() const { return dockTextureWidth_ > 0 ? dockWidthM_ * dockTextureHeight_ / dockTextureWidth_ : 0; }
    /** @return the device index the HUD hangs on for this config (HMD, or the wrist's controller) */
    uint32_t anchorDevice(const Config& config, double now);
    /** @return the device index of an attachment (the HMD, or that wrist's controller) */
    uint32_t anchorDeviceFor(Attachment attachment, double now);

    // ---- The browser panel and its keyboard (two more overlays, made at connect, hidden until used) ----
    /** Notice: the low-battery line in front of the eyes (no input; hangs off the headset). */
    enum class Aux { Browser, Keyboard, Notice };
    bool hasAux(Aux which) const { return aux_[static_cast<int>(which)].handle != 0; }
    /**
     * A picture for one of them. A new size gets a new texture (the old one is freed kRetiredKeepSec later), its mouse scale
     * and width.
     * @param pixels cairo's premultiplied ARGB32
     */
    bool submitAux(Aux which, const uint8_t* pixels, int width, int height, double widthM);
    void showAux(Aux which, bool show);
    bool auxShown(Aux which) const { return aux_[static_cast<int>(which)].shown; }
    /** Its opacity (0..1; the notice's fade). Only changes are sent. */
    void setAuxAlpha(Aux which, double alpha);
    /**
     * Attaches the browser panel to a device at local, and the keyboard under it (keyboardInBrowser: the keyboard
     * relative to the browser panel). Only changes are sent.
     * @return false if no such device or SteamVR refused
     */
    bool placeBrowserRelative(uint32_t device, const dockmath::Mat34& local, const dockmath::Mat34& keyboardInBrowser);
    /** Hangs the notice off the headset at local. Only changes are sent. */
    bool placeNotice(const dockmath::Mat34& local);
    /** Fixes both in the room (고정, or a grab released while fixed). */
    void placeBrowserAbsolute(const dockmath::Mat34& world, const dockmath::Mat34& keyboardInBrowser);
    /** @return its pose in the room as of the last readDockPoses (false: not shown, not placed or not tracked) */
    bool auxWorld(Aux which, dockmath::Mat34& world) const;
    double auxWidthM(Aux which) const { return aux_[static_cast<int>(which)].widthM; }
    double auxHeightM(Aux which) const {
        const AuxOverlay& a = aux_[static_cast<int>(which)];
        return a.textureWidth > 0 ? a.widthM * a.textureHeight / a.textureWidth : 0.0;
    }

    /** Whether connect() sets up SteamVR Input for the grip (config.dock.grip_input; read at connect only). */
    void setGripWanted(bool wanted) { gripWanted_ = wanted; }
    /** @return true if SteamVR Input took the action manifest (otherwise the trigger-hold grabs still work) */
    bool gripAvailable() const { return gripAction_ != 0; }
    /**
     * Reads one hand's grip through the Dock's action set (made active for this read only; normal priority, so the
     * game keeps the grip too).
     * @return false if it can't be read now (no SteamVR Input, an error, or SteamVR has the action inactive, which it
     *         may do while its laser has the focus): not a release
     */
    bool readGrip(bool leftHand, bool& down);
    /**
     * The trigger of a controller (the one whose laser pressed a button), from IVRSystem::GetControllerState.
     * @param device the tracked device index from the press's event (or the aiming hand's controller)
     * @return false if it can't be read (no such controller, no answer). An app without input focus (or one that uses
     *         the action system) may get an all-zero state: a "released" that only counts once it read pressed.
     */
    bool readTrigger(uint32_t device, bool& down);

    /**
     * 接続時に見つけた vrserver のプロセスがまだ生きているか: /proc/<pid>/comm still says vrserver (a reused PID
     * doesn't count). Not found at connect: looked for again every kVrserverLookupSec.
     * @return 生きている（または確かめられない）なら true
     */
    bool steamVrAlive(double now);
    static constexpr double kVrserverLookupSec = 5.0;

    /**
     * 設定パネル（ダッシュボードのこのアプリのタブ）が今見えているかの材料を読む（IPC 3 回）。
     * @return 材料。visible() で判定する
     */
    SettingsVisibility settingsVisibility() const;

    /**
     * SteamVR のダッシュボードが開いているか（--verbose の診断用）。
     * @return 開いていれば true
     */
    bool dashboardOpen() const;

    /**
     * HMD activity level (IVRSystem::GetTrackedDeviceActivityLevel for the HMD, one IPC). Off-head reads 3
     * (k_EDeviceActivityLevel_Standby): nobody sees the panel then, so its sources pause.
     * @return the EDeviceActivityLevel value, or -1 (unknown) when not connected
     */
    int hmdActivityLevel() const;

    /** @return 設定パネル（ダッシュボード）を作れていれば true */
    bool hasSettings() const { return dashboardHandle_ != 0; }

    /**
     * ダッシュボードのサムネイル画像を送る（接続直後に 1 回）。
     * @param rgba 非乗算済み RGBA
     * @param size 一辺の px
     * @return 送れたら true
     */
    bool submitThumbnail(const uint8_t* rgba, int size);

    /**
     * 前回の呼び出しからのフレーム時間を集計する。
     * @return 集計結果（取れなければ valid = false）
     */
    FrameStats readFrameStats();

    /**
     * 左右のコントローラーの電池を読む（呼ぶのは 10 秒おき程度で十分）。
     * @return 左右の電池。つながっていない側は present = false
     */
    ControllerStatus readControllers();

    /**
     * Reads the current scene app (GetCurrentSceneProcessId, then GetApplicationKeyByProcessId and the name).
     * @param app the result
     * @return false when not connected
     */
    bool readSceneApp(SceneApp& app) const;

    /**
     * 診断用: 新しく届いたフレームの生のタイミング（フレーム番号・表示回数・再投影の印など）を
     * 0.5 秒おきに標準出力へ書き出す。
     * @param seconds 書き出す秒数
     * @param stop 途中でやめる印（SIGINT などで 1 になる）
     */
    void dumpFrameTimings(double seconds, const volatile int& stop);

    /**
     * 性能パネルの画像を送る。When the size differs from the current texture (the layout changed), a new texture is
     * made; it takes over once its picture went up (until then the old one stays as it was), and the old one is kept
     * alive kRetiredKeepSec, since the compositor may still read it. The overlay's centre moves down by half of
     * extraBottomPx only after a picture went up.
     * @param pixels width × height: cairo's premultiplied ARGB32 bytes when panelTakesCairoPixels(), else
     *               non-premultiplied RGBA 8 bit
     * @param width texture width (px)
     * @param height texture height (px)
     * @param widthColumns the panel's width in grid columns (GridGeometry::widthInColumns): the overlay is
     *                     width_m × this wide (width_m is one full column's width)
     * @param extraBottomPx the last px rows are under the HUD's own picture (the video controls): the overlay is
     *                      moved down by half of them, so its own picture stays where the config puts it
     * @return 送れたら true
     */
    bool submitPanel(const uint8_t* pixels, int width, int height, double widthColumns, int extraBottomPx = 0);
    /**
     * The config's HUD transform for an overlay transform (relative to its anchor) of the HUD as drawn now: with
     * px under its own picture (submitPanel's extraBottomPx) the overlay's centre is that much lower.
     */
    dockmath::Mat34 hudBaseFrom(const dockmath::Mat34& overlayLocal) const;

    /**
     * @return true if submitPanel takes cairo's premultiplied ARGB32 bytes as they are (B8G8R8A8 + IsPremultiplied, no
     *         conversion). FRAME_PERF_RGBA_UPLOAD=1 brings back the RGBA conversion (surfaceToRgba)
     */
    bool panelTakesCairoPixels() const { return panelBgra_; }

    /**
     * 設定パネルの画像を送る。
     * @param pixels cairo's premultiplied ARGB32 bytes (B8G8R8A8 + IsPremultiplied, like the other panels)
     * @return 送れたら true
     */
    bool submitSettings(const uint8_t* pixels);

    /**
     * SteamVR's keyboard for the dashboard tab (profile names). The text comes back in VrEvents::keyboardText when
     * Done is pressed.
     * @param description shown above the keys
     * @param existing the text it starts with
     * @param maxChars longest text allowed
     * @param error why it didn't open
     * @return true if the keyboard is up
     */
    bool showKeyboard(const std::string& description, const std::string& existing, uint32_t maxChars,
                      std::string& error, bool forDock = false);

    /**
     * Sends message to one of SteamVR's internal mailboxes over this connection (OpenVR's internal IVRMailbox_001,
     * room_view.h). Our own mailbox ("framedock_<pid>") is registered the first time and dropped at shutdown().
     * @return false (and why) when not connected or SteamVR refused it
     */
    bool sendMailbox(const std::string& target, const std::string& message, std::string& error);

private:
    bool connected_ = false;
    void* mailbox_ = nullptr;     ///< IVRMailbox_001 (vr_overlay.cpp), for this connection
    uint64_t mailboxHandle_ = 0;  ///< ours, registered on it
    uint32_t attachedDevice_ = 0xffffffff;  ///< 変換を渡してある機器（0xffffffff = vr::k_unTrackedDeviceIndexInvalid = まだ）
    bool transformDirty_ = true;            ///< 変換を渡し直す必要がある（設定が変わった・前回断られた）
    vr::HmdMatrix34_t panelTransform_ {};    ///< 設定変更時に作る、機器基準の位置と向き (moved down by panelExtraPx_)
    vr::HmdMatrix34_t basePanelTransform_ {};  ///< the config's transform: where the HUD's own picture is centred
    int panelExtraPx_ = 0;  ///< px under the HUD's own picture (the video controls): the overlay grows downward
    /** @return half the extra rows' height (m): how far the overlay's centre is below the picture's */
    double panelShiftM() const;
    /** @return basePanelTransform_ moved down by panelShiftM() along the panel's own y */
    vr::HmdMatrix34_t shiftedPanelTransform() const;
    WristFadeState wristFade_;              ///< 手首のときの見せる割合（フェード）
    PanelPresentationState presentation_;   ///< OpenVR に反映できた透明度と表示・非表示
    double nextPlacementAt_ = std::numeric_limits<double>::infinity();  ///< 次に updatePlacement を呼ぶ時刻
    uint32_t wristDevice_ = 0xffffffff;     ///< 手首に固定しているコントローラーの番号（覚えておいて、ときどき聞き直す）
    bool wristDeviceLeft_ = true;           ///< wristDevice_ が左手の番号か
    double wristDeviceCheckedAt_ = -1.0;    ///< wristDevice_ を聞いた時刻（まだなら負）
    int panelTextureHeight_ = 0;    ///< 性能パネルのテクスチャの高さ（px。レイアウトで変わる）
    uint64_t panelHandle_ = 0;      ///< vr::VROverlayHandle_t（性能パネル）
    uint64_t dashboardHandle_ = 0;  ///< ダッシュボードの設定パネル
    uint64_t thumbnailHandle_ = 0;  ///< ダッシュボードのサムネイル
    int settingsHeight_ = 0;
    int vrserverPid_ = -1;
    double nextVrserverLookup_ = 0;
    uint32_t lastFrameIndex_ = 0;
    bool haveFrameIndex_ = false;   ///< 前回の readFrameStats でフレームを受け取った
    double displayHz_ = 0.0;
    double hzCheckedAt_ = -1.0;
    bool readOnly_ = false;         ///< connectReadOnly でつないだ（オーバーレイも Vulkan もない）
    bool verbose_ = false;          ///< applyConfig のたびに向きの確かめをログに出す
    double facingCheckAt_ = 0.0;    ///< 向きの確かめをする時刻（0 なら予定なし）
    Config facingCheckConfig_;      ///< 向きの確かめに使う設定（最後に applyConfig したもの）
    std::vector<uint8_t> timingBuffer_;  ///< Compositor_FrameTiming の配列用
    std::vector<std::unique_ptr<OverlayTexture>> graveyard_;  ///< stuck textures, destroyed once the GPU lets go
    /** A texture whose upload is stuck (OverlayTexture::broken): moved to the graveyard; the caller makes a new one */
    void bury(std::unique_ptr<OverlayTexture>& texture, const char* name);
    /** Destroys what in the graveyard the GPU has finished with (never waits) */
    void buryIdle();

    // The Dock
    uint64_t dockHandle_ = 0;          ///< vr::VROverlayHandle_t of the Dock panel
    std::unique_ptr<OverlayTexture> dockTexture_, retiredDockTexture_;
    double retiredDockAt_ = 0;
    int dockTextureWidth_ = 0, dockTextureHeight_ = 0;
    double dockWidthM_ = 0.30;
    bool dockShown_ = false;
    double dockHiddenAt_ = -1;  ///< when a HideOverlay succeeded; −1: not hidden by one (never freed idle)
    enum class DockPlacement { None, Relative, Absolute };
    DockPlacement dockPlacement_ = DockPlacement::None;
    uint32_t dockDevice_ = 0xffffffff;
    dockmath::Mat34 dockTransform_ {};  ///< relative to dockDevice_, or absolute
    bool hudInput_ = false;
    bool hudInteractive_ = false, dockInteractive_ = false;
    dockloop::EventPolling polling_;  ///< which overlays a wake polls (interactive, the dashboard, a drain)
    bool hudGrabbed_ = false;
    uint32_t hudGrabDevice_ = 0xffffffff;
    dockmath::Mat34 hudGrabOffset_ {};
    vr::TrackedDevicePose_t poses_[vr::k_unMaxTrackedDeviceCount] {};  ///< the last pose read (updatePlacement)
    double posesAt_ = -1.0;
    struct TipCache {
        std::string model;
        dockmath::Mat34 offset {};
        bool found = false;
        double checkedAt = -1e9;
    };
    TipCache tips_[vr::k_unMaxTrackedDeviceCount];
    bool gripWanted_ = true;
    uint64_t gripSet_ = 0, gripAction_ = 0, leftHand_ = 0, rightHand_ = 0;  ///< SteamVR Input handles
    uint32_t leftRole_ = 0xffffffff, rightRole_ = 0xffffffff;
    double rolesAt_ = -1.0;

    /** Creates the Dock panel overlay (hidden) after the HUD. A failure leaves the HUD working. */
    void createDock();

    /** The browser panel or the keyboard overlay. */
    struct AuxOverlay {
        uint64_t handle = 0;
        std::unique_ptr<OverlayTexture> texture, retired;
        double retiredAt = 0;  ///< retired replaced then (freed kRetiredKeepSec later)
        int textureWidth = 0, textureHeight = 0;
        double widthM = 0.4;
        bool shown = false;
        double alpha = 1;      ///< as last set (setAuxAlpha)
        double hiddenAt = -1;  ///< when a HideOverlay succeeded; −1: not hidden by one
        bool interactive = false;
        DockPlacement placement = DockPlacement::None;
        uint32_t device = 0xffffffff;
        dockmath::Mat34 transform {};  ///< relative to device, or absolute
    };
    AuxOverlay aux_[3];
    /** Creates the browser panel and keyboard overlays (hidden). A failure leaves the rest working. */
    void createAux();
    /** The laser's small cursor for the browser panel and the keyboard (SetOverlayCursor). A failure keeps SteamVR's. */
    void createCursor();
    uint64_t cursorHandle_ = 0;
    static constexpr double kCursorWidthM = 0.007;  ///< the dot's overlay: ~6 mm across, its white ~4 mm
    bool placeAuxRelative(AuxOverlay& a, const char* name, uint32_t device, const dockmath::Mat34& local);
    void placeAuxAbsolute(AuxOverlay& a, const char* name, const dockmath::Mat34& world);
    /** The action manifest for the grip (SteamVR Input), before the first PollNextEvent. */
    void setupGrip();
    /** @return the laser offset (tip) of a device, cached */
    dockmath::Mat34 tipOffset(uint32_t device, double now);
    /** Reads every device's pose into poses_ */
    void readPoses(double now);

    VulkanContext vulkan_;
    std::unique_ptr<OverlayTexture> panelTexture_;         ///< recreated when the layout changes its size
    struct RetiredTexture {
        std::unique_ptr<OverlayTexture> texture;
        double at;  ///< replaced then
    };
    std::vector<RetiredTexture> retiredPanel_;  ///< earlier sizes, each destroyed kRetiredKeepSec after it was replaced
    static constexpr double kRetiredKeepSec = 1.0;  ///< a retired texture is kept this long (SteamVR may still show it)
    static constexpr size_t kMaxRetired = 4;
    std::unique_ptr<OverlayTexture> pendingPanel_;  ///< a new size whose picture hasn't gone up yet (see submitPanel)
    /** Frees the retired textures (HUD, Dock, browser, keyboard, notice) kept kRetiredKeepSec, without waiting */
    void pruneRetired(double now);
    int pendingWidth_ = 0, pendingHeight_ = 0;
    int panelTextureWidth_ = 0;
    double panelColumns_ = 1.0;   ///< the overlay is width_m × this wide (the panel's width in full columns)
    bool panelBgra_ = true;       ///< the panel texture is B8G8R8A8 + premultiplied
    double configWidthM_ = 0.20;  ///< width_m from the last applyConfig
    OverlayTexture settingsTexture_;
    OverlayTexture thumbnailTexture_;

    /**
     * 手首に固定するコントローラーの番号を返す（役割から引いた番号を覚えておき、手を替えたときと 1 秒ごとだけ
     * OpenVR に聞き直す）。
     * @param left 左手なら true
     * @param now 今の時刻（秒、単調増加）
     * @return 番号（見つからなければ vr::k_unTrackedDeviceIndexInvalid）
     */
    uint32_t wristDeviceIndex(bool left, double now);

    /**
     * ダッシュボードの設定パネルとサムネイルを作る。失敗しても性能パネルは動かす。
     * @param width 設定パネルの幅（px）
     * @param height 設定パネルの高さ（px）
     */
    void createDashboard(int width, int height);

    /**
     * 診断用（--verbose）: 今の HMD の姿勢から、頭の中心 → パネル上の 3 点（中心・パネル自身の +x 方向に 2cm・
     * +y 方向に 2cm）へ光線を飛ばし、SteamVR が計算した当たり（ComputeOverlayIntersection の UV・法線・距離）を
     * ログに出す。法線は HMD 基準に直して、こちらで計算した表の向き R·(0,0,1) と、頭への向き −pos / |pos| との
     * 内積も出す（1 に近ければ表が頭を向いている）。
     * @param config 反映した設定
     */
    void logFacingCheck(const Config& config) const;

    /**
     * /proc を一度だけ走査して vrserver の PID を探す。
     * @return 見つかった PID。無ければ -1
     */
    static int findVrserverPid();

    /**
     * HMD のリフレッシュレートを取る（5 秒に 1 回だけ問い合わせる）。
     * @param now 現在時刻（秒）
     * @return Hz。取れなければ 0
     */
    double displayHz(double now);
};
