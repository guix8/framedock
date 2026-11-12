// OpenVR との接続とオーバーレイの実装。
#include "vr_overlay.h"

#include "dock_manifest.h"
#include "log_limiter.h"
#include "openvr.h"
#include "placement.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

namespace {

/**
 * OpenVR's internal mailbox interface "IVRMailbox_001" (not in openvr.h; the vtable order read from vrclient.so,
 * SteamVR 2.15, room_view.h): RegisterMailbox(name, &handle), UndoRegisterMailbox(handle), SendMessage(handle,
 * address, message), ReadMessage(...). The calls return 0 on success. Called through its vtable's slots: declared
 * as a C++ class with no implementation in this program, LTO turned the calls into __cxa_pure_virtual.
 */
constexpr const char* kMailboxVersion = "IVRMailbox_001";
using MailboxRegisterFn = int (*)(void* self, const char* name, uint64_t* handle);
using MailboxUndoFn = int (*)(void* self, uint64_t handle);
using MailboxSendFn = int (*)(void* self, uint64_t handle, const char* address, const char* message);
/** @return the interface's slot (0 RegisterMailbox, 1 UndoRegisterMailbox, 2 SendMessage) */
template <typename Fn>
Fn mailboxSlot(void* mailbox, int slot) {
    return reinterpret_cast<Fn>((*static_cast<void***>(mailbox))[slot]);
}
/**
 * @return true if the interface's first slots (the four methods) are functions in SteamVR's vrclient.so, as read
 * there: a SteamVR that changed the interface but kept its name fails here instead of calling something else
 */
bool mailboxLooksRight(void* mailbox, std::string& error) {
    for (int slot = 0; slot < 4; ++slot) {
        void* fn = (*static_cast<void***>(mailbox))[slot];
        Dl_info info {};
        if (fn == nullptr || ::dladdr(fn, &info) == 0 || info.dli_fname == nullptr ||
            std::strstr(info.dli_fname, "vrclient") == nullptr) {
            error = std::string(kMailboxVersion) + "'s slot " + std::to_string(slot) + " isn't in vrclient (" +
                    (info.dli_fname != nullptr ? info.dli_fname : "nowhere") + ")";
            return false;
        }
    }
    return true;
}
/** Ours: "framedock_<pid>" (the app and a `framedock --room-view` beside it never share one) */
constexpr const char* kMailboxPrefix = "framedock_";

// Frame Dock's own keys, so it never collides with a Frame Perf Overlay or FrameColor left running
constexpr const char* kOverlayKey = "framedock.hud";
constexpr const char* kOverlayName = "Frame Dock HUD";
constexpr const char* kDashboardKey = "framedock.dashboard";
constexpr const char* kDashboardName = "Frame Dock";
constexpr const char* kDockKey = "framedock.dock";
constexpr const char* kDockName = "Frame Dock panel";
constexpr const char* kBrowserKey = "framedock.browser";
constexpr const char* kBrowserName = "Frame Dock browser";
constexpr const char* kKeyboardKey = "framedock.keyboard";
constexpr const char* kKeyboardName = "Frame Dock keyboard";
constexpr const char* kNoticeKey = "framedock.notice";
constexpr const char* kNoticeName = "Frame Dock notice";
constexpr const char* kCursorKey = "framedock.cursor";
constexpr const char* kCursorName = "Frame Dock cursor";
constexpr int kCursorPx = 64;

const char* auxName(VrOverlay::Aux which) {
    return which == VrOverlay::Aux::Browser ? "browser" : which == VrOverlay::Aux::Keyboard ? "keyboard" : "notice";
}
// Poses read this recently (updatePlacement's) are good enough for the Dock's aim check
constexpr double kPoseReuseSec = 0.02;
// A controller's laser tip, asked again after this long (a found one rarely changes; a missing one may load later)
constexpr double kTipFoundRecheckSec = 30.0;
constexpr double kTipMissingRecheckSec = 5.0;
// 設定パネルの 1px あたりの大きさ（m）。1200px を 2.8m で出していたときと同じ（前の 1024px / 2.4m ともほぼ同じ）。
// 幅（m）は画像の幅（px）から決める
constexpr float kDashboardMetersPerPx = 2.8f / 1200.0f;
constexpr uint32_t kMaxTimings = 256;
// 終了時、オーバーレイを消してから VR_Shutdown まで待つ時間（90Hz で約 36 フレーム）
constexpr int kShutdownWaitMs = 400;
// --verbose の向きの確かめを、変換を渡してから何秒後にするか
constexpr double kFacingCheckDelaySec = 0.5;
// 性能パネルの透明度・表示・変換を断られたとき、やり直すまでの秒数
constexpr double kPlacementRetrySec = 0.1;
// 手首に固定しているコントローラーの番号（役割から引く）を聞き直す間隔（秒）
constexpr double kWristRoleCheckSec = 1.0;

/**
 * 単調増加の時計で今の時刻を秒で返す。
 * @return 秒
 */
double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/**
 * オーバーレイのエラー名を返す。
 * @param error エラー
 * @return 名前（例: VROverlayError_None）
 */
const char* overlayErrorName(vr::EVROverlayError error) {
    return vr::VROverlay()->GetOverlayErrorNameFromEnum(error);
}

/**
 * オーバーレイのエラーを、成功以外なら標準エラーに出す。
 * @param what 何をしたときか
 * @param error エラー
 * @return 成功なら true
 */
/** The failures of OpenVR and Vulkan calls: a call retried every 0.1 s logs once a minute, not ten times a second */
LogLimiter& failureLog() {
    static LogLimiter log;
    return log;
}

/** Logs a failure through failureLog(). @return true if it was logged */
bool logFailure(const char* key, const std::string& line, const std::string& message) {
    int suppressed = 0;
    if (!failureLog().allow(key, message, nowSeconds(), suppressed)) return false;
    if (suppressed > 0) {
        std::fprintf(stderr, "%s (%d more like it in the last minute)\n", line.c_str(), suppressed);
    } else {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    return true;
}

bool checkOverlay(const char* what, vr::EVROverlayError error) {
    if (error == vr::VROverlayError_None) {
        failureLog().clear(what);
        return true;
    }
    const std::string name = overlayErrorName(error);
    logFailure(what, std::string("[VR] ") + what + " に失敗: " + name, name);
    return false;
}

/**
 * PID からプロセス名（/proc/<pid>/comm）を引く（診断の表示用）。
 * @param pid プロセス ID
 * @return 名前。分からなければ "?"
 */
std::string processName(uint32_t pid) {
    if (pid == 0) return "-";
    const std::string path = "/proc/" + std::to_string(pid) + "/comm";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "?";
    char name[64] = {};
    const ssize_t n = ::read(fd, name, sizeof(name) - 1);
    ::close(fd);
    std::string text(name, n > 0 ? static_cast<size_t>(n) : 0);
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

/**
 * 終了処理の 1 手順の結果を、成功でも失敗でもログに出す（あとで順番と結果を確かめるため）。
 * @param what 何をしたか
 * @param error 戻り値
 */
void logShutdownStep(const char* what, vr::EVROverlayError error) {
    std::fprintf(stderr, "[VR] 終了処理 %s -> %s\n", what, overlayErrorName(error));
}

}  // namespace

VrOverlay::VrOverlay() : timingBuffer_(sizeof(vr::Compositor_FrameTiming) * kMaxTimings) {}

VrOverlay::~VrOverlay() {
    shutdown();
}

int VrOverlay::findVrserverPid() {
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return -1;
    int found = -1;
    while (const dirent* entry = ::readdir(proc)) {
        const char* name = entry->d_name;
        if (name[0] < '0' || name[0] > '9') continue;
        const std::string commPath = std::string("/proc/") + name + "/comm";
        const int fd = ::open(commPath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char comm[64] = {};
        const ssize_t n = ::read(fd, comm, sizeof(comm) - 1);
        ::close(fd);
        if (n > 0 && std::strncmp(comm, "vrserver\n", 9) == 0) {
            found = std::atoi(name);
            break;
        }
    }
    ::closedir(proc);
    return found;
}

VrOverlay::ConnectResult VrOverlay::connect(int panelWidth, int panelHeight, int settingsWidth, int settingsHeight,
                                            std::string& message) {
    if (connected_) return ConnectResult::Ok;

    // 1) Background 型で「SteamVR が動いているか」だけ確かめる（動いていなければ起動させない）
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return error == vr::VRInitError_Init_NoServerForBackgroundApp ? ConnectResult::NotRunning
                                                                        : ConnectResult::Error;
    }
    vr::VR_Shutdown();

    // 2) オーバーレイ型でつなぎ直す
    vr::VR_Init(&error, vr::VRApplication_Overlay);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return ConnectResult::Error;
    }
    connected_ = true;
    // SteamVR Input's manifest has to come before the first UpdateActionState / PollNextEvent
    if (gripWanted_) setupGrip();

    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError overlayError = vr::VROverlay()->CreateOverlay(kOverlayKey, kOverlayName, &handle);
    if (overlayError != vr::VROverlayError_None) {
        message = std::string("CreateOverlay: ") + overlayErrorName(overlayError);
        shutdown();
        return ConnectResult::Error;
    }
    panelHandle_ = handle;
    // 置き場所と見え方の状態は、つなぎ直すたびに最初から（新しいオーバーレイには何も渡していない）
    attachedDevice_ = vr::k_unTrackedDeviceIndexInvalid;
    transformDirty_ = true;
    wristFade_ = WristFadeState {};
    presentation_ = PanelPresentationState {};
    nextPlacementAt_ = std::numeric_limits<double>::infinity();
    wristDevice_ = vr::k_unTrackedDeviceIndexInvalid;
    wristDeviceCheckedAt_ = -1.0;
    hudInput_ = hudInteractive_ = dockInteractive_ = hudGrabbed_ = false;
    polling_ = dockloop::EventPolling {};
    posesAt_ = -1.0;
    rolesAt_ = -1.0;
    for (TipCache& tip : tips_) tip = TipCache {};
    panelTextureHeight_ = panelHeight;
    panelTexture_ = std::make_unique<OverlayTexture>();
    retiredPanel_.clear();
    pendingPanel_.reset();
    panelTextureWidth_ = panelWidth;
    // cairo's premultiplied ARGB32 goes up as it is (B8G8R8A8 + IsPremultiplied), which saves the per-redraw RGBA
    // conversion. FRAME_PERF_RGBA_UPLOAD=1 brings back the non-premultiplied RGBA path
    const char* rgbaEnv = std::getenv("FRAME_PERF_RGBA_UPLOAD");
    panelBgra_ = rgbaEnv == nullptr || rgbaEnv[0] == '\0' || rgbaEnv[0] == '0';
    checkOverlay("SetOverlayFlag(IsPremultiplied)",
                 vr::VROverlay()->SetOverlayFlag(panelHandle_, vr::VROverlayFlags_IsPremultiplied, panelBgra_));

    // 3) Vulkan と性能パネルのテクスチャ
    std::string vkMessage;
    if (!vulkan_.init(vkMessage) || !panelTexture_->create(vulkan_, panelWidth, panelHeight, vkMessage, panelBgra_)) {
        message = "Vulkan の準備に失敗: " + vkMessage;
        shutdown();
        return ConnectResult::Error;
    }

    // 4) ダッシュボードの設定パネル（失敗しても性能パネルは動かす）
    createDashboard(settingsWidth, settingsHeight);
    // 5) The Dock panel (hidden until the HUD is clicked in a game). A failure leaves the rest working
    createDock();
    // 6) The browser panel and its keyboard (hidden until opened from the Dock)
    createAux();

    vrserverPid_ = findVrserverPid();
    nextVrserverLookup_ = 0;
    lastFrameIndex_ = 0;
    haveFrameIndex_ = false;
    hzCheckedAt_ = -1.0;
    return ConnectResult::Ok;
}

bool VrOverlay::connectReadOnly(std::string& message) {
    if (connected_) return true;
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return false;
    }
    if (vr::VRCompositor() == nullptr || vr::VRSystem() == nullptr) {
        message = "Background 型では VRCompositor / VRSystem が使えません";
        vr::VR_Shutdown();
        return false;
    }
    connected_ = true;
    readOnly_ = true;
    hzCheckedAt_ = -1.0;
    return true;
}

void VrOverlay::createDashboard(int width, int height) {
    vr::IVROverlay* overlay = vr::VROverlay();
    vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t thumbnail = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError error = overlay->CreateDashboardOverlay(kDashboardKey, kDashboardName, &main, &thumbnail);
    std::fprintf(stderr, "[VR] CreateDashboardOverlay -> %s\n", overlayErrorName(error));
    if (error != vr::VROverlayError_None) return;
    dashboardHandle_ = main;
    thumbnailHandle_ = thumbnail;
    settingsHeight_ = height;

    checkOverlay("SetOverlayWidthInMeters(設定)", overlay->SetOverlayWidthInMeters(main, kDashboardMetersPerPx * static_cast<float>(width)));
    checkOverlay("SetOverlayInputMethod(設定)", overlay->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse));
    // マウス座標を画像の px にそろえる
    const vr::HmdVector2_t scale = {{static_cast<float>(width), static_cast<float>(height)}};
    checkOverlay("SetOverlayMouseScale(設定)", overlay->SetOverlayMouseScale(main, &scale));
    // ダッシュボードの下のアイコンにホバーしたとき「閉じる」を出す。SteamVR の dashboard は
    // このフラグがあるオーバーレイに閉じるボタンを出し、押されると VREvent_OverlayClosed を送ってくる。
    const vr::EVROverlayError closeError =
        overlay->SetOverlayFlag(main, vr::VROverlayFlags_EnableControlBarClose, true);
    // 立ったかどうかを vrserver から読み返して確かめる（ログに残す）
    bool closeEnabled = false;
    const vr::EVROverlayError readError =
        overlay->GetOverlayFlag(main, vr::VROverlayFlags_EnableControlBarClose, &closeEnabled);
    std::fprintf(stderr, "[VR] SetOverlayFlag(EnableControlBarClose) -> %s（読み返し: %s, %s）\n",
                 overlayErrorName(closeError), overlayErrorName(readError), closeEnabled ? "true" : "false");
    // Thumbstick / trackpad scrolling over the tab (VREvent_ScrollSmooth; needs the Mouse input method)
    checkOverlay("SetOverlayFlag(SendVRSmoothScrollEvents)",
                 overlay->SetOverlayFlag(main, vr::VROverlayFlags_SendVRSmoothScrollEvents, true));

    // cairo's premultiplied BGRA as it is, like the other panels (no RGBA conversion, 4.7 MB, per redraw)
    checkOverlay("SetOverlayFlag(設定, IsPremultiplied)",
                 overlay->SetOverlayFlag(main, vr::VROverlayFlags_IsPremultiplied, true));
    std::string message;
    if (!settingsTexture_.create(vulkan_, width, height, message, true)) {
        std::fprintf(stderr, "[Vulkan] 設定パネルのテクスチャを作れません: %s\n", message.c_str());
    }
}

void VrOverlay::createDock() {
    vr::IVROverlay* overlay = vr::VROverlay();
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError error = overlay->CreateOverlay(kDockKey, kDockName, &handle);
    std::fprintf(stderr, "[dock] CreateOverlay(Dock panel) -> %s\n", overlayErrorName(error));
    if (error != vr::VROverlayError_None) return;
    dockHandle_ = handle;
    dockShown_ = false;
    dockHiddenAt_ = -1;
    dockPlacement_ = DockPlacement::None;
    dockTextureWidth_ = dockTextureHeight_ = 0;
    // cairo's premultiplied pixels as they are, the laser as a mouse, scrolling for the Layout page's lanes, drawn
    // over the HUD where they overlap
    checkOverlay("SetOverlayFlag(Dock, IsPremultiplied)",
                 overlay->SetOverlayFlag(handle, vr::VROverlayFlags_IsPremultiplied, true));
    checkOverlay("SetOverlayInputMethod(Dock)", overlay->SetOverlayInputMethod(handle, vr::VROverlayInputMethod_Mouse));
    checkOverlay("SetOverlayFlag(Dock, SendVRSmoothScrollEvents)",
                 overlay->SetOverlayFlag(handle, vr::VROverlayFlags_SendVRSmoothScrollEvents, true));
    checkOverlay("SetOverlaySortOrder(Dock)", overlay->SetOverlaySortOrder(handle, 1));
    checkOverlay("HideOverlay(Dock)", overlay->HideOverlay(handle));
}

void VrOverlay::createAux() {
    vr::IVROverlay* overlay = vr::VROverlay();
    const struct {
        const char* key;
        const char* name;
        uint32_t sortOrder;
    } specs[3] = {{kBrowserKey, kBrowserName, 2}, {kKeyboardKey, kKeyboardName, 3}, {kNoticeKey, kNoticeName, 4}};
    for (int i = 0; i < 3; ++i) {
        AuxOverlay& a = aux_[i];
        a = AuxOverlay {};
        vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
        const vr::EVROverlayError error = overlay->CreateOverlay(specs[i].key, specs[i].name, &handle);
        std::fprintf(stderr, "[browser] CreateOverlay(%s) -> %s\n", specs[i].name, overlayErrorName(error));
        if (error != vr::VROverlayError_None) continue;
        a.handle = handle;
        // cairo's premultiplied pixels as they are, the laser as a mouse, the thumbstick's scrolling (the page), in
        // front of the HUD and the Dock panel. The notice takes no input: the laser passes through it
        checkOverlay("SetOverlayFlag(browser, IsPremultiplied)",
                     overlay->SetOverlayFlag(handle, vr::VROverlayFlags_IsPremultiplied, true));
        if (i == static_cast<int>(Aux::Notice)) {
            checkOverlay("SetOverlaySortOrder(notice)", overlay->SetOverlaySortOrder(handle, specs[i].sortOrder));
            checkOverlay("HideOverlay(notice)", overlay->HideOverlay(handle));
            continue;
        }
        checkOverlay("SetOverlayInputMethod(browser)", overlay->SetOverlayInputMethod(handle, vr::VROverlayInputMethod_Mouse));
        checkOverlay("SetOverlayFlag(browser, SendVRSmoothScrollEvents)",
                     overlay->SetOverlayFlag(handle, vr::VROverlayFlags_SendVRSmoothScrollEvents, true));
        checkOverlay("SetOverlaySortOrder(browser)", overlay->SetOverlaySortOrder(handle, specs[i].sortOrder));
        checkOverlay("HideOverlay(browser)", overlay->HideOverlay(handle));
    }
    createCursor();
}

void VrOverlay::createCursor() {
    // The laser's cursor on the browser panel and the keyboard: a small white dot in a dark ring (kCursorWidthM)
    // instead of SteamVR's generic blob, which covers a link or a key on a panel this size. SteamVR draws it where the
    // laser hits; the overlay itself is never shown by us
    vr::IVROverlay* overlay = vr::VROverlay();
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError error = overlay->CreateOverlay(kCursorKey, kCursorName, &handle);
    std::fprintf(stderr, "[browser] CreateOverlay(%s) -> %s\n", kCursorName, overlayErrorName(error));
    if (error != vr::VROverlayError_None) return;
    cursorHandle_ = handle;
    std::vector<uint8_t> rgba(static_cast<size_t>(kCursorPx) * kCursorPx * 4, 0);
    const double c = kCursorPx / 2.0, inner = kCursorPx * 0.27, outer = kCursorPx * 0.45;
    for (int y = 0; y < kCursorPx; ++y) {
        for (int x = 0; x < kCursorPx; ++x) {
            const double d = std::hypot(x + 0.5 - c, y + 0.5 - c);
            const double alpha = std::clamp(outer - d + 0.5, 0.0, 1.0);  // the edge antialiased over a pixel
            const double white = std::clamp(inner - d + 0.5, 0.0, 1.0);
            uint8_t* p = &rgba[(static_cast<size_t>(y) * kCursorPx + x) * 4];
            p[0] = p[1] = p[2] = static_cast<uint8_t>(std::lround(13 + white * (255 - 13)));  // kBg's dark to white
            p[3] = static_cast<uint8_t>(std::lround(alpha * 230));
        }
    }
    checkOverlay("SetOverlayRaw(cursor)", overlay->SetOverlayRaw(handle, rgba.data(), kCursorPx, kCursorPx, 4));
    checkOverlay("SetOverlayWidthInMeters(cursor)",
                 overlay->SetOverlayWidthInMeters(handle, static_cast<float>(kCursorWidthM)));
    const vr::HmdVector2_t hotspot = {{0.5f, 0.5f}};
    checkOverlay("SetOverlayTransformCursor(cursor)", overlay->SetOverlayTransformCursor(handle, &hotspot));
    for (const Aux which : {Aux::Browser, Aux::Keyboard}) {
        const AuxOverlay& a = aux_[static_cast<int>(which)];
        if (a.handle != 0) checkOverlay("SetOverlayCursor(browser)", overlay->SetOverlayCursor(a.handle, handle));
    }
}

bool VrOverlay::submitAux(Aux which, const uint8_t* pixels, int width, int height, double widthM) {
    AuxOverlay& a = aux_[static_cast<int>(which)];
    if (!connected_ || a.handle == 0) return false;
    vr::IVROverlay* overlay = vr::VROverlay();
    std::string message;
    const char* key = which == Aux::Browser    ? "browser texture"
                      : which == Aux::Keyboard ? "keyboard texture"
                                               : "notice texture";
    buryIdle();
    const double now = nowSeconds();
    pruneRetired(now);
    if (a.texture && a.texture->broken()) bury(a.texture, key);
    if (!a.texture || width != a.textureWidth || height != a.textureHeight) {
        auto next = std::make_unique<OverlayTexture>();
        if (!next->create(vulkan_, width, height, message, true)) {
            logFailure(key, "[browser] can't make a " + std::to_string(width) + "x" + std::to_string(height) +
                                " texture: " + message, message);
            return false;
        }
        if (a.retired) graveyard_.push_back(std::move(a.retired));  // as the Dock's
        a.retired = std::move(a.texture);
        a.retiredAt = now;
        a.texture = std::move(next);
        a.textureWidth = width;
        a.textureHeight = height;
        const vr::HmdVector2_t scale = {{static_cast<float>(width), static_cast<float>(height)}};
        checkOverlay("SetOverlayMouseScale(browser)", overlay->SetOverlayMouseScale(a.handle, &scale));
        a.widthM = -1;
    }
    if (std::fabs(widthM - a.widthM) > 1e-6) {
        if (checkOverlay("SetOverlayWidthInMeters(browser)",
                         overlay->SetOverlayWidthInMeters(a.handle, static_cast<float>(widthM)))) {
            a.widthM = widthM;
        }
    }
    const bool ok = a.texture->update(a.handle, pixels, message);
    if (ok) {
        failureLog().clear(key);
    } else {
        logFailure(key, std::string("[browser] can't send the ") + auxName(which) + ": " + message, message);
    }
    return ok;
}

void VrOverlay::bury(std::unique_ptr<OverlayTexture>& texture, const char* name) {
    // Its upload never finished: a new texture takes over, this one waits until the GPU lets it go
    std::fprintf(stderr, "[Vulkan] The %s's last upload is stuck on the GPU: making a new texture\n", name);
    graveyard_.push_back(std::move(texture));
}

void VrOverlay::buryIdle() {
    for (auto it = graveyard_.begin(); it != graveyard_.end();) {
        it = (*it)->destroyIfIdle() ? graveyard_.erase(it) : std::next(it);
    }
}

void VrOverlay::showAux(Aux which, bool show) {
    AuxOverlay& a = aux_[static_cast<int>(which)];
    if (!connected_ || a.handle == 0 || show == a.shown) return;
    vr::IVROverlay* overlay = vr::VROverlay();
    if (show) a.hiddenAt = -1;  // meant to show (even if ShowOverlay fails): its texture isn't idle
    if (checkOverlay(show ? "ShowOverlay(browser)" : "HideOverlay(browser)",
                     show ? overlay->ShowOverlay(a.handle) : overlay->HideOverlay(a.handle))) {
        a.shown = show;
        if (!show) a.hiddenAt = nowSeconds();
    }
    if (!show && which != Aux::Notice) {
        setInteractive(which == Aux::Browser ? DockTarget::Browser : DockTarget::Keyboard, false);
    }
}

void VrOverlay::setAuxAlpha(Aux which, double alpha) {
    AuxOverlay& a = aux_[static_cast<int>(which)];
    if (!connected_ || a.handle == 0 || std::fabs(alpha - a.alpha) < 1e-3) return;
    if (checkOverlay("SetOverlayAlpha(aux)", vr::VROverlay()->SetOverlayAlpha(a.handle, static_cast<float>(alpha)))) {
        a.alpha = alpha;
    }
}

bool VrOverlay::placeAuxRelative(AuxOverlay& a, const char* name, uint32_t device, const dockmath::Mat34& local) {
    if (!connected_ || a.handle == 0 || device >= vr::k_unMaxTrackedDeviceCount) return false;
    if (a.placement == DockPlacement::Relative && a.device == device && std::memcmp(&a.transform, &local, sizeof(local)) == 0) {
        return true;
    }
    if (checkOverlay(name, vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(a.handle, device, &local))) {
        a.placement = DockPlacement::Relative;
        a.device = device;
        a.transform = local;
        return true;
    }
    return false;
}

void VrOverlay::placeAuxAbsolute(AuxOverlay& a, const char* name, const dockmath::Mat34& world) {
    if (!connected_ || a.handle == 0) return;
    if (a.placement == DockPlacement::Absolute && std::memcmp(&a.transform, &world, sizeof(world)) == 0) return;
    if (checkOverlay(name, vr::VROverlay()->SetOverlayTransformAbsolute(a.handle, vr::TrackingUniverseStanding, &world))) {
        a.placement = DockPlacement::Absolute;
        a.device = vr::k_unTrackedDeviceIndexInvalid;
        a.transform = world;
    }
}

bool VrOverlay::placeBrowserRelative(uint32_t device, const dockmath::Mat34& local,
                                     const dockmath::Mat34& keyboardInBrowser) {
    AuxOverlay& browser = aux_[static_cast<int>(Aux::Browser)];
    AuxOverlay& keyboard = aux_[static_cast<int>(Aux::Keyboard)];
    const bool placed = placeAuxRelative(browser, "SetOverlayTransformTrackedDeviceRelative(browser)", device, local);
    if (placed) {
        placeAuxRelative(keyboard, "SetOverlayTransformTrackedDeviceRelative(keyboard)", device,
                         dockmath::multiply(local, keyboardInBrowser));
    }
    return placed;
}

bool VrOverlay::placeNotice(const dockmath::Mat34& local) {
    return placeAuxRelative(aux_[static_cast<int>(Aux::Notice)], "SetOverlayTransformTrackedDeviceRelative(notice)",
                            vr::k_unTrackedDeviceIndex_Hmd, local);
}

void VrOverlay::placeBrowserAbsolute(const dockmath::Mat34& world, const dockmath::Mat34& keyboardInBrowser) {
    placeAuxAbsolute(aux_[static_cast<int>(Aux::Browser)], "SetOverlayTransformAbsolute(browser)", world);
    placeAuxAbsolute(aux_[static_cast<int>(Aux::Keyboard)], "SetOverlayTransformAbsolute(keyboard)",
                     dockmath::multiply(world, keyboardInBrowser));
}

bool VrOverlay::auxWorld(Aux which, dockmath::Mat34& world) const {
    const AuxOverlay& a = aux_[static_cast<int>(which)];
    if (!connected_ || a.handle == 0 || !a.shown) return false;
    if (a.placement == DockPlacement::Absolute) {
        world = a.transform;
        return true;
    }
    if (a.placement != DockPlacement::Relative || a.device >= vr::k_unMaxTrackedDeviceCount || posesAt_ < 0) return false;
    const vr::TrackedDevicePose_t& p = poses_[a.device];
    if (!p.bDeviceIsConnected || !p.bPoseIsValid) return false;
    world = dockmath::multiply(p.mDeviceToAbsoluteTracking, a.transform);
    return true;
}

void VrOverlay::setupGrip() {
    gripSet_ = gripAction_ = leftHand_ = rightHand_ = 0;
    int written = 0;
    std::string error;
    const std::string manifest = dockinput::writeManifest(dockinput::defaultManifestDir(), written, error);
    if (manifest.empty()) {
        std::fprintf(stderr, "[dock] No grip (SteamVR Input): can't write the action manifest: %s\n", error.c_str());
        return;
    }
    vr::IVRInput* input = vr::VRInput();
    if (input == nullptr) {
        std::fprintf(stderr, "[dock] No grip: SteamVR Input isn't there\n");
        return;
    }
    const vr::EVRInputError e = input->SetActionManifestPath(manifest.c_str());
    vr::VRActionSetHandle_t set = vr::k_ulInvalidActionSetHandle;
    vr::VRActionHandle_t action = vr::k_ulInvalidActionHandle;
    vr::VRInputValueHandle_t left = vr::k_ulInvalidInputValueHandle, right = vr::k_ulInvalidInputValueHandle;
    const bool ok = e == vr::VRInputError_None &&
                    input->GetActionSetHandle(dockinput::kActionSet, &set) == vr::VRInputError_None &&
                    input->GetActionHandle(dockinput::kGrabAction, &action) == vr::VRInputError_None &&
                    input->GetInputSourceHandle("/user/hand/left", &left) == vr::VRInputError_None &&
                    input->GetInputSourceHandle("/user/hand/right", &right) == vr::VRInputError_None;
    std::fprintf(stderr, "[dock] SteamVR Input manifest %s (%d file(s) written): SetActionManifestPath -> %d%s\n",
                 manifest.c_str(), written, static_cast<int>(e), ok ? ", grip ready" : ", no grip");
    if (!ok) return;
    gripSet_ = set;
    gripAction_ = action;
    leftHand_ = left;
    rightHand_ = right;
}

bool VrOverlay::readGrip(bool leftHand, bool& down) {
    down = false;
    if (!connected_ || gripAction_ == 0) return false;
    vr::IVRInput* input = vr::VRInput();
    // Active for this read only, at normal priority: the game keeps getting the grip
    vr::VRActiveActionSet_t active {};
    active.ulActionSet = gripSet_;
    active.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    active.nPriority = 0;
    if (input->UpdateActionState(&active, sizeof(active), 1) != vr::VRInputError_None) return false;
    vr::InputDigitalActionData_t data {};
    if (input->GetDigitalActionData(gripAction_, &data, sizeof(data), leftHand ? leftHand_ : rightHand_) !=
            vr::VRInputError_None ||
        !data.bActive) {
        return false;
    }
    down = data.bState;
    return true;
}

bool VrOverlay::readTrigger(uint32_t device, bool& down) {
    down = false;
    if (!connected_ || device >= vr::k_unMaxTrackedDeviceCount) return false;
    vr::IVRSystem* system = vr::VRSystem();
    if (system->GetTrackedDeviceClass(device) != vr::TrackedDeviceClass_Controller) return false;
    vr::VRControllerState_t state {};
    if (!system->GetControllerState(device, &state, sizeof(state))) return false;
    down = (state.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0;
    return true;
}

void VrOverlay::readPoses(double now) {
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses_, vr::k_unMaxTrackedDeviceCount);
    posesAt_ = now;
}

dockmath::Mat34 VrOverlay::tipOffset(uint32_t device, double now) {
    // Where SteamVR's laser comes from: the render model's "tip" component, not the controller's pose (on the Frame's
    // controllers it points about 40° below the pose's −z). GetComponentState, not ...ForDevicePath: without an input
    // source the latter fails for every component while a game runs (Frametop, screens/vr.cpp TipOffset)
    TipCache& tip = tips_[device];
    const double recheck = tip.found ? kTipFoundRecheckSec : kTipMissingRecheckSec;
    if (now - tip.checkedAt < recheck) return tip.found ? tip.offset : dockmath::identity();
    tip.checkedAt = now;
    char model[256] = "";
    vr::VRSystem()->GetStringTrackedDeviceProperty(device, vr::Prop_RenderModelName_String, model, sizeof(model));
    vr::RenderModel_ControllerMode_State_t mode {};
    vr::RenderModel_ComponentState_t state {};
    vr::VRControllerState_t buttons {};
    const bool found = model[0] != '\0' && vr::VRRenderModels() != nullptr &&
                       vr::VRRenderModels()->GetComponentState(model, vr::k_pch_Controller_Component_Tip, &buttons,
                                                               &mode, &state);
    if (found != tip.found || model != tip.model) {
        std::fprintf(stderr, "[dock] Laser of device %u (%s): %s\n", device, model[0] ? model : "no model",
                     found ? "from the render model's tip" : "along the pose (no tip)");
    }
    tip.model = model;
    tip.found = found;
    if (found) tip.offset = state.mTrackingToComponentLocal;
    return found ? tip.offset : dockmath::identity();
}

DockPoses VrOverlay::readDockPoses(double now) {
    DockPoses result;
    if (!connected_ || readOnly_) return result;
    if (posesAt_ < 0 || now - posesAt_ > kPoseReuseSec || now < posesAt_) readPoses(now);
    if (rolesAt_ < 0 || now - rolesAt_ >= kWristRoleCheckSec || now < rolesAt_) {
        leftRole_ = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
        rightRole_ = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        rolesAt_ = now;
    }
    const auto fill = [&](DockPoses::Device& d, uint32_t index, bool laser) {
        d.index = index;
        if (index >= vr::k_unMaxTrackedDeviceCount) return;
        const vr::TrackedDevicePose_t& p = poses_[index];
        d.valid = p.bDeviceIsConnected && p.bPoseIsValid;
        if (!d.valid) return;
        d.pose = p.mDeviceToAbsoluteTracking;
        d.laser = laser ? dockmath::multiply(d.pose, tipOffset(index, now)) : d.pose;
    };
    fill(result.hmd, vr::k_unTrackedDeviceIndex_Hmd, false);
    fill(result.left, leftRole_, true);
    fill(result.right, rightRole_, true);
    return result;
}

bool VrOverlay::hudWorld(dockmath::Mat34& world) const {
    const uint32_t device = hudGrabbed_ ? hudGrabDevice_ : attachedDevice_;
    if (!connected_ || device >= vr::k_unMaxTrackedDeviceCount || posesAt_ < 0) return false;
    const vr::TrackedDevicePose_t& p = poses_[device];
    if (!p.bDeviceIsConnected || !p.bPoseIsValid) return false;
    world = dockmath::multiply(p.mDeviceToAbsoluteTracking, hudGrabbed_ ? hudGrabOffset_ : panelTransform_);
    return true;
}

bool VrOverlay::dockWorld(dockmath::Mat34& world) const {
    if (!connected_ || dockHandle_ == 0 || !dockShown_) return false;
    if (dockPlacement_ == DockPlacement::Absolute) {
        world = dockTransform_;
        return true;
    }
    if (dockPlacement_ != DockPlacement::Relative || dockDevice_ >= vr::k_unMaxTrackedDeviceCount || posesAt_ < 0) {
        return false;
    }
    const vr::TrackedDevicePose_t& p = poses_[dockDevice_];
    if (!p.bDeviceIsConnected || !p.bPoseIsValid) return false;
    world = dockmath::multiply(p.mDeviceToAbsoluteTracking, dockTransform_);
    return true;
}

uint32_t VrOverlay::anchorDevice(const Config& config, double now) { return anchorDeviceFor(config.attachment, now); }

uint32_t VrOverlay::anchorDeviceFor(Attachment attachment, double now) {
    if (attachment == Attachment::Head) return vr::k_unTrackedDeviceIndex_Hmd;
    return wristDeviceIndex(attachment == Attachment::LeftWrist, now);
}

void VrOverlay::setHudInput(bool on) {
    if (!connected_ || panelHandle_ == 0 || on == hudInput_) return;
    vr::IVROverlay* overlay = vr::VROverlay();
    if (!checkOverlay("SetOverlayInputMethod(HUD)",
                      overlay->SetOverlayInputMethod(panelHandle_, on ? vr::VROverlayInputMethod_Mouse
                                                                      : vr::VROverlayInputMethod_None))) {
        return;
    }
    hudInput_ = on;
    if (on && panelTextureWidth_ > 0) {
        // SteamVR's laser and ComputeOverlayIntersection take the panel's height from the mouse scale (Frametop's
        // finding): the texture's size makes it exactly the panel
        const vr::HmdVector2_t scale = {{static_cast<float>(panelTextureWidth_), static_cast<float>(panelTextureHeight_)}};
        checkOverlay("SetOverlayMouseScale(HUD)", overlay->SetOverlayMouseScale(panelHandle_, &scale));
    }
    if (!on) setInteractive(DockTarget::Hud, false);
}

void VrOverlay::setInteractive(DockTarget target, bool on) {
    if (!connected_) return;
    if (target == DockTarget::Browser || target == DockTarget::Keyboard) {
        AuxOverlay& a = aux_[target == DockTarget::Browser ? 0 : 1];
        if (a.handle == 0 || a.interactive == on) return;
        if (checkOverlay("SetOverlayFlag(browser, MakeOverlaysInteractiveIfVisible)",
                         vr::VROverlay()->SetOverlayFlag(a.handle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible,
                                                         on))) {
            a.interactive = on;
            polling_.setInteractive(target == DockTarget::Browser ? dockloop::EventPolling::Browser
                                                                  : dockloop::EventPolling::Keyboard,
                                    on, nowSeconds());
        }
        return;
    }
    const bool hud = target == DockTarget::Hud;
    const uint64_t handle = hud ? panelHandle_ : dockHandle_;
    bool& current = hud ? hudInteractive_ : dockInteractive_;
    if (handle == 0 || target == DockTarget::None || current == on) return;
    if (hud && on && !hudInput_) return;
    if (checkOverlay("SetOverlayFlag(MakeOverlaysInteractiveIfVisible)",
                     vr::VROverlay()->SetOverlayFlag(handle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, on))) {
        current = on;
        // Taken away: its events are still polled a moment (the Up or Leave SteamVR sent last)
        polling_.setInteractive(hud ? dockloop::EventPolling::Hud : dockloop::EventPolling::Dock, on, nowSeconds());
    }
}

void VrOverlay::grabHud(uint32_t device, const dockmath::Mat34& offset) {
    if (!connected_ || panelHandle_ == 0) return;
    hudGrabbed_ = true;
    hudGrabDevice_ = device;
    hudGrabOffset_ = offset;
    // At once: SteamVR carries it from here, rigidly, with no lag from us
    if (checkOverlay("SetOverlayTransformTrackedDeviceRelative(HUD grab)",
                     vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(panelHandle_, device, &hudGrabOffset_))) {
        attachedDevice_ = device;
        transformDirty_ = false;
    } else {
        transformDirty_ = true;
    }
    nextPlacementAt_ = std::min(nextPlacementAt_, nowSeconds());
}

void VrOverlay::releaseHud() {
    if (!hudGrabbed_) return;
    hudGrabbed_ = false;
    transformDirty_ = true;
    nextPlacementAt_ = std::min(nextPlacementAt_, nowSeconds());
}

bool VrOverlay::submitDock(const uint8_t* pixels, int width, int height, double widthM) {
    if (!connected_ || dockHandle_ == 0) return false;
    vr::IVROverlay* overlay = vr::VROverlay();
    std::string message;
    buryIdle();
    const double now = nowSeconds();
    pruneRetired(now);
    if (dockTexture_ && dockTexture_->broken()) bury(dockTexture_, "Dock panel");
    if (!dockTexture_ || width != dockTextureWidth_ || height != dockTextureHeight_) {
        auto next = std::make_unique<OverlayTexture>();
        if (!next->create(vulkan_, width, height, message, true)) {
            logFailure("dock texture", "[dock] can't make a " + std::to_string(width) + "x" + std::to_string(height) +
                                           " texture: " + message, message);
            return false;
        }
        // The old one is kept kRetiredKeepSec (SteamVR may still show it); one retired already goes on its upload
        if (retiredDockTexture_) graveyard_.push_back(std::move(retiredDockTexture_));
        retiredDockTexture_ = std::move(dockTexture_);
        retiredDockAt_ = now;
        dockTexture_ = std::move(next);
        dockTextureWidth_ = width;
        dockTextureHeight_ = height;
        const vr::HmdVector2_t scale = {{static_cast<float>(width), static_cast<float>(height)}};
        checkOverlay("SetOverlayMouseScale(Dock)", overlay->SetOverlayMouseScale(dockHandle_, &scale));
        dockWidthM_ = -1;  // set below
    }
    if (std::fabs(widthM - dockWidthM_) > 1e-6) {
        if (checkOverlay("SetOverlayWidthInMeters(Dock)",
                         overlay->SetOverlayWidthInMeters(dockHandle_, static_cast<float>(widthM)))) {
            dockWidthM_ = widthM;
        }
    }
    const bool ok = dockTexture_->update(dockHandle_, pixels, message);
    if (ok) {
        failureLog().clear("dock texture");
    } else {
        logFailure("dock texture", "[dock] can't send the Dock panel: " + message, message);
    }
    return ok;
}

void VrOverlay::showDock(bool show) {
    if (!connected_ || dockHandle_ == 0 || show == dockShown_) return;
    vr::IVROverlay* overlay = vr::VROverlay();
    if (show) dockHiddenAt_ = -1;  // meant to show (even if ShowOverlay fails): its texture isn't idle
    if (checkOverlay(show ? "ShowOverlay(Dock)" : "HideOverlay(Dock)",
                     show ? overlay->ShowOverlay(dockHandle_) : overlay->HideOverlay(dockHandle_))) {
        dockShown_ = show;
        if (!show) dockHiddenAt_ = nowSeconds();
    }
    if (!show) setInteractive(DockTarget::Dock, false);
}

bool VrOverlay::placeDockRelative(uint32_t device, const dockmath::Mat34& local) {
    if (!connected_ || dockHandle_ == 0 || device >= vr::k_unMaxTrackedDeviceCount) return false;
    if (dockPlacement_ == DockPlacement::Relative && dockDevice_ == device &&
        std::memcmp(&dockTransform_, &local, sizeof(local)) == 0) {
        return true;
    }
    if (checkOverlay("SetOverlayTransformTrackedDeviceRelative(Dock)",
                     vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(dockHandle_, device, &local))) {
        dockPlacement_ = DockPlacement::Relative;
        dockDevice_ = device;
        dockTransform_ = local;
        return true;
    }
    return false;
}

void VrOverlay::placeDockAbsolute(const dockmath::Mat34& world) {
    if (!connected_ || dockHandle_ == 0) return;
    if (checkOverlay("SetOverlayTransformAbsolute(Dock)",
                     vr::VROverlay()->SetOverlayTransformAbsolute(dockHandle_, vr::TrackingUniverseStanding, &world))) {
        dockPlacement_ = DockPlacement::Absolute;
        dockDevice_ = vr::k_unTrackedDeviceIndexInvalid;
        dockTransform_ = world;
    }
}

bool VrOverlay::sendMailbox(const std::string& target, const std::string& message, std::string& error) {
    if (!connected_) {
        error = "not connected to SteamVR";
        return false;
    }
    if (mailbox_ == nullptr) {
        vr::EVRInitError initError = vr::VRInitError_None;
        void* found = vr::VR_GetGenericInterface(kMailboxVersion, &initError);
        if (found == nullptr) {
            error = std::string(kMailboxVersion) + ": " + vr::VR_GetVRInitErrorAsEnglishDescription(initError);
            return false;
        }
        if (!mailboxLooksRight(found, error)) return false;
        uint64_t handle = 0;
        const std::string name = kMailboxPrefix + std::to_string(::getpid());
        const int registered = mailboxSlot<MailboxRegisterFn>(found, 0)(found, name.c_str(), &handle);
        if (registered != 0) {
            error = "RegisterMailbox: " + std::to_string(registered);
            return false;
        }
        mailbox_ = found;
        mailboxHandle_ = handle;
    }
    const int sent = mailboxSlot<MailboxSendFn>(mailbox_, 2)(mailbox_, mailboxHandle_, target.c_str(), message.c_str());
    if (sent != 0) {
        error = "SendMessage: " + std::to_string(sent);
        return false;
    }
    return true;
}

void VrOverlay::shutdown() {
    if (!connected_) return;
    if (mailbox_ != nullptr) mailboxSlot<MailboxUndoFn>(mailbox_, 1)(mailbox_, mailboxHandle_);
    mailbox_ = nullptr;
    mailboxHandle_ = 0;
    if (readOnly_) {
        // オーバーレイも Vulkan も作っていないので、閉じるだけ
        vr::VR_Shutdown();
        connected_ = false;
        readOnly_ = false;
        return;
    }
    vr::IVROverlay* overlay = vr::VROverlay();
    std::fprintf(stderr, "[VR] 終了処理を始めます\n");

    // 1) 性能パネルを隠す
    if (panelHandle_ != 0) logShutdownStep("HideOverlay(パネル)", overlay->HideOverlay(panelHandle_));
    if (dockHandle_ != 0) logShutdownStep("HideOverlay(Dock)", overlay->HideOverlay(dockHandle_));
    for (AuxOverlay& a : aux_) {
        if (a.handle != 0) logShutdownStep("HideOverlay(browser)", overlay->HideOverlay(a.handle));
    }
    // 2) テクスチャを外す（コンポジタがこちらの画像を参照しないようにする）
    if (panelHandle_ != 0) logShutdownStep("ClearOverlayTexture(パネル)", overlay->ClearOverlayTexture(panelHandle_));
    if (dockHandle_ != 0) logShutdownStep("ClearOverlayTexture(Dock)", overlay->ClearOverlayTexture(dockHandle_));
    for (AuxOverlay& a : aux_) {
        if (a.handle != 0) logShutdownStep("ClearOverlayTexture(browser)", overlay->ClearOverlayTexture(a.handle));
    }
    if (dashboardHandle_ != 0) {
        logShutdownStep("ClearOverlayTexture(設定)", overlay->ClearOverlayTexture(dashboardHandle_));
        logShutdownStep("ClearOverlayTexture(サムネイル)", overlay->ClearOverlayTexture(thumbnailHandle_));
    }
    // 3) オーバーレイを消す（サムネイルは設定パネルと一緒に消える）
    if (panelHandle_ != 0) logShutdownStep("DestroyOverlay(パネル)", overlay->DestroyOverlay(panelHandle_));
    if (dashboardHandle_ != 0) logShutdownStep("DestroyOverlay(設定)", overlay->DestroyOverlay(dashboardHandle_));
    if (dockHandle_ != 0) logShutdownStep("DestroyOverlay(Dock)", overlay->DestroyOverlay(dockHandle_));
    if (cursorHandle_ != 0) logShutdownStep("DestroyOverlay(cursor)", overlay->DestroyOverlay(cursorHandle_));
    cursorHandle_ = 0;
    for (AuxOverlay& a : aux_) {
        if (a.handle != 0) logShutdownStep("DestroyOverlay(browser)", overlay->DestroyOverlay(a.handle));
        a.handle = 0;
        a.shown = a.interactive = false;
        a.placement = DockPlacement::None;
    }
    polling_ = dockloop::EventPolling {};
    dockHandle_ = 0;
    dockShown_ = false;
    dockPlacement_ = DockPlacement::None;
    gripSet_ = gripAction_ = leftHand_ = rightHand_ = 0;
    panelHandle_ = 0;
    dashboardHandle_ = 0;
    thumbnailHandle_ = 0;

    // 4) コンポジタが数フレーム回って、外したテクスチャを手放すのを待つ
    std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownWaitMs));
    std::fprintf(stderr, "[VR] 終了処理 %dms 待ちました\n", kShutdownWaitMs);

    // 5) OpenVR を閉じる（Vulkan の画像を壊すのはこの後、という OpenVR の決まり）
    vr::VR_Shutdown();
    connected_ = false;
    std::fprintf(stderr, "[VR] 終了処理 VR_Shutdown 済み\n");

    // 6) Vulkan の画像とデバイスを壊す
    thumbnailTexture_.destroy();
    settingsTexture_.destroy();
    if (retiredDockTexture_) retiredDockTexture_->destroy();
    if (dockTexture_) dockTexture_->destroy();
    for (AuxOverlay& a : aux_) {
        if (a.retired) a.retired->destroy();
        if (a.texture) a.texture->destroy();
        a.retired.reset();
        a.texture.reset();
        a.textureWidth = a.textureHeight = 0;
    }
    retiredDockTexture_.reset();
    dockTexture_.reset();
    dockTextureWidth_ = dockTextureHeight_ = 0;
    for (RetiredTexture& r : retiredPanel_) r.texture->destroy();
    if (pendingPanel_) pendingPanel_->destroy();
    if (panelTexture_) panelTexture_->destroy();
    retiredPanel_.clear();
    pendingPanel_.reset();
    panelTexture_.reset();
    for (auto& t : graveyard_) t->destroy();  // a texture still stuck is left alive (destroy() won't block for long)
    graveyard_.clear();
    vulkan_.destroy();
    std::fprintf(stderr, "[VR] 終了処理 Vulkan を片付けました\n");
}

void VrOverlay::applyConfig(const Config& config) {
    if (!connected_ || panelHandle_ == 0) return;
    // width_m is one column's width, so the text keeps its size whatever the layout
    configWidthM_ = config.widthM;
    checkOverlay("SetOverlayWidthInMeters", vr::VROverlay()->SetOverlayWidthInMeters(
                                                panelHandle_, static_cast<float>(config.widthM * panelColumns_)));
    basePanelTransform_ = attachmentTransform(config);
    panelTransform_ = shiftedPanelTransform();
    transformDirty_ = true;
    updatePlacement(config, nowSeconds());
    // 変換を渡した直後に交差を聞くと、SteamVR はまだ前の変換で答える（実機で確かめた）ので、少し待ってから聞く。
    // 確かめるのは頭に固定しているときだけ（頭の中心から見た向きを計算するため）
    facingCheckAt_ = 0;
    if (verbose_ && config.attachment == Attachment::Head) {
        facingCheckConfig_ = config;
        facingCheckAt_ = nowSeconds() + kFacingCheckDelaySec;
    }
}

uint32_t VrOverlay::wristDeviceIndex(bool left, double now) {
    // 役割（左手・右手）の番号を聞くのは IPC なので、毎回は聞かない。手を替えたときと 1 秒ごと（コントローラーが
    // つなぎ直されて番号が変わっても 1 秒以内に追いつく）だけ聞き直す。隠すのは姿勢で決めるので、ここは待たない
    if (left != wristDeviceLeft_ || wristDeviceCheckedAt_ < 0 || now - wristDeviceCheckedAt_ >= kWristRoleCheckSec) {
        wristDevice_ = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(
            left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        wristDeviceLeft_ = left;
        wristDeviceCheckedAt_ = now;
    }
    return wristDevice_;
}

void VrOverlay::updatePlacement(const Config& config, double now) {
    if (!connected_ || panelHandle_ == 0) {
        nextPlacementAt_ = std::numeric_limits<double>::infinity();
        return;
    }
    vr::IVROverlay* overlay = vr::VROverlay();
    // Carried by a controller (the Dock's grab): on that controller, fully shown, no wrist fade
    const bool grabbed = hudGrabbed_;
    const bool wrist = !grabbed && config.attachment != Attachment::Head;
    vr::TrackedDeviceIndex_t device = vr::k_unTrackedDeviceIndex_Hmd;
    bool tracked = true;
    double target = 1.0;
    if (grabbed && config.visible) {
        device = hudGrabDevice_;
        readPoses(now);
        tracked = wristTrackingValid(device, poses_);
    } else if (wrist && config.visible) {
        // コントローラー基準の変換。見せる割合は、コントローラーと頭の今の姿勢から、面が目の方を向いている度合いで決める
        device = wristDeviceIndex(config.attachment == Attachment::LeftWrist, now);
        readPoses(now);  // kept for the Dock's aim check (readDockPoses)
        tracked = wristTrackingValid(device, poses_);
        if (tracked && config.wristFade) {
            target = wristFacingAlpha(composeTransform(poses_[device].mDeviceToAbsoluteTracking, panelTransform_),
                                      poses_[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking,
                                      config.wristFadeEndDeg);
        }
    }
    const vr::HmdMatrix34_t& transform = grabbed ? hudGrabOffset_ : panelTransform_;
    bool retry = false;
    if (tracked && config.visible && (transformDirty_ || attachedDevice_ != device)) {
        const vr::EVROverlayError error = overlay->SetOverlayTransformTrackedDeviceRelative(panelHandle_, device, &transform);
        if (checkOverlay("SetOverlayTransformTrackedDeviceRelative", error)) {
            attachedDevice_ = device;
            transformDirty_ = false;
        } else {
            // 置き場所を渡せなかったパネルは出さない（前の場所に残さない）。次の呼び出しでやり直す
            tracked = false;
            retry = true;
        }
    }
    // トラッキングが外れたら、フェードを待たずにすぐ隠す（止まったコントローラーの位置にパネルを浮かせたままにしない）
    const double shown = wristFade_.update(wrist, tracked && config.visible, device, target, now);
    retry |= presentation_.apply(
        config.alpha * quantizeAlpha(shown),  // the fade in steps; the opacity setting itself as it is
        [&](double value) {
            return checkOverlay("SetOverlayAlpha", overlay->SetOverlayAlpha(panelHandle_, static_cast<float>(value)));
        },
        [&](bool show) {
            return checkOverlay(show ? "ShowOverlay" : "HideOverlay",
                                show ? overlay->ShowOverlay(panelHandle_) : overlay->HideOverlay(panelHandle_));
        });
    // 次に確かめる時刻: 断られた操作があれば少し後にやり直す。手首を出している間はフェードの途中なら細かく、
    // 見えきっている・消えきっているなら粗く（トラッキングが外れたのに気づくのもこの間隔）。頭で問題なければ呼ばない
    double interval = std::numeric_limits<double>::infinity();
    if (wrist && config.visible) interval = wristPollInterval(shown, tracked ? target : 0.0);
    if (grabbed) interval = std::min(interval, kWristPollSteadySec);  // notice a lost controller
    if (retry) interval = std::min(interval, kPlacementRetrySec);
    nextPlacementAt_ = now + interval;
}

void VrOverlay::logFacingCheck(const Config& config) const {
    vr::TrackedDevicePose_t pose {};
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &pose, 1);
    if (!pose.bPoseIsValid) {
        std::fprintf(stderr, "[向き] HMD の姿勢が取れないので確かめられません\n");
        return;
    }
    const vr::HmdMatrix34_t& hmd = pose.mDeviceToAbsoluteTracking;
    double r[3][3];
    panelRotation(config, r);
    const double pos[3] = {config.posX, config.posY, config.posZ};
    const double distance = std::sqrt(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
    std::fprintf(stderr,
                 "[向き] 位置 (%.3f, %.3f, %.3f) yaw %.1f pitch %.1f roll %.1f | 計算した表の向き (%.3f, %.3f, %.3f)・"
                 "頭への向きとの内積 %.4f | 計算したパネルの +x 軸 (%.3f, %.3f, %.3f)\n",
                 pos[0], pos[1], pos[2], config.yawDeg, config.pitchDeg, config.rollDeg, r[0][2], r[1][2], r[2][2],
                 distance > 0 ? -(r[0][2] * pos[0] + r[1][2] * pos[1] + r[2][2] * pos[2]) / distance : 0.0, r[0][0],
                 r[1][0], r[2][0]);

    // 狙う点（HMD 基準）: 中心、パネル自身の +x に 2cm、+y に 2cm
    const char* names[3] = {"中心", "パネルの+x 2cm", "パネルの+y 2cm"};
    const double offsets[3][2] = {{0.0, 0.0}, {0.02, 0.0}, {0.0, 0.02}};
    for (int i = 0; i < 3; ++i) {
        double target[3];
        for (int k = 0; k < 3; ++k) target[k] = pos[k] + r[k][0] * offsets[i][0] + r[k][1] * offsets[i][1];
        const double length = std::sqrt(target[0] * target[0] + target[1] * target[1] + target[2] * target[2]);
        if (length <= 0) continue;
        // HMD 基準 → 世界（Standing）: 向きは回転だけ、始点は HMD の位置
        vr::VROverlayIntersectionParams_t params {};
        for (int k = 0; k < 3; ++k) {
            params.vSource.v[k] = hmd.m[k][3];
            params.vDirection.v[k] = static_cast<float>(
                (hmd.m[k][0] * target[0] + hmd.m[k][1] * target[1] + hmd.m[k][2] * target[2]) / length);
        }
        params.eOrigin = vr::TrackingUniverseStanding;
        vr::VROverlayIntersectionResults_t hit {};
        if (!vr::VROverlay()->ComputeOverlayIntersection(panelHandle_, &params, &hit)) {
            std::fprintf(stderr, "[向き] %s: 当たりなし\n", names[i]);
            continue;
        }
        // UV は u が右へ、v が上へ増える。どちらも幅で割った値（縦も幅で割る。実機で確かめた）。
        // 法線を HMD 基準に戻す（回転の転置を掛ける）
        double normal[3];
        for (int k = 0; k < 3; ++k) {
            normal[k] = hmd.m[0][k] * hit.vNormal.v[0] + hmd.m[1][k] * hit.vNormal.v[1] + hmd.m[2][k] * hit.vNormal.v[2];
        }
        const double dotComputed = normal[0] * r[0][2] + normal[1] * r[1][2] + normal[2] * r[2][2];
        std::fprintf(stderr,
                     "[向き] %s: UV (%.3f, %.3f)（予想 %.3f, %.3f）距離 %.3fm（予想 %.3f）"
                     "SteamVR の法線 (HMD 基準) (%.3f, %.3f, %.3f)・計算した表の向きとの内積 %.4f\n",
                     names[i], hit.vUVs.v[0], hit.vUVs.v[1], 0.5 + offsets[i][0] / config.widthM,
                     0.5 + offsets[i][1] / config.widthM, hit.fDistance, length, normal[0], normal[1], normal[2], dotComputed);
    }
}

namespace {

/**
 * A mouse event on one of our overlays as a PointerInput (y from the top: SteamVR's mouse y starts at the bottom).
 * @param secondary the right button too (PointerInput::secondary): the Frame's laser mouse sends it for a thumbstick
 *        click (its vrcompositor bindings, /actions/lasermouse/in/rightclick)
 * @return false if it isn't a pointer event
 */
bool pointerFromEvent(const vr::VREvent_t& event, double height, PointerInput& out, bool secondary = false) {
    const double x = event.data.mouse.x;
    const double y = height - event.data.mouse.y;
    const bool right = secondary && event.data.mouse.button == vr::VRMouseButton_Right;
    switch (event.eventType) {
        case vr::VREvent_MouseMove: out = {PointerInput::Type::Move, x, y}; return true;
        case vr::VREvent_MouseButtonDown:
            if (event.data.mouse.button != vr::VRMouseButton_Left && !right) return false;
            out = {PointerInput::Type::Down, x, y};
            out.device = event.trackedDeviceIndex;
            out.secondary = right;
            return true;
        case vr::VREvent_MouseButtonUp:
            if (event.data.mouse.button != vr::VRMouseButton_Left && !right) return false;
            out = {PointerInput::Type::Up, x, y};
            out.secondary = right;
            return true;
        case vr::VREvent_FocusLeave: out = {PointerInput::Type::Leave, 0, 0}; return true;
        case vr::VREvent_ScrollSmooth:
            out = {PointerInput::Type::Scroll, 0, 0};
            out.scrollDy = event.data.scroll.ydelta;
            out.scrollDx = event.data.scroll.xdelta;
            return true;
        default: return false;
    }
}

}  // namespace

void VrOverlay::pollEvents(VrEvents& result, bool includeSystem, bool includeDock, bool includeDashboard) {
    result.clear();
    if (!connected_) return;
    vr::VREvent_t event {};
    if (includeSystem) {
        while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_Quit) result.quit = true;
            // The room view (passthrough). openvr.h says "for scene apps only": whether an overlay app gets them is
            // to be seen on the headset
            if (event.eventType == vr::VREvent_RoomViewShown) result.roomView = 1;
            if (event.eventType == vr::VREvent_RoomViewHidden) result.roomView = 0;
            if (event.eventType == vr::VREvent_SceneApplicationChanged) result.sceneAppChanged = true;
        }
    }
    if (includeSystem || includeDock) {
        // Every overlay on the full (2 Hz) poll; between, only those SteamVR's laser can be on now (interactive, all
        // of them while the dashboard is open) and for a moment after (dockloop::EventPolling): the others have no
        // pointer events to give, and anything else they have waits for the next full poll
        const bool all = includeSystem;
        const double now = nowSeconds();
        // The HUD: image failures, and the laser on it (in-game interaction)
        PointerInput input {PointerInput::Type::Move};
        while ((hudInput_ || polling_.polls(dockloop::EventPolling::Hud, all, now)) &&
               vr::VROverlay()->PollNextOverlayEvent(panelHandle_, &event, sizeof(event))) {
            if (event.eventType == vr::VREvent_ImageFailed) std::fprintf(stderr, "[VR] 画像の読み込みに失敗しました\n");
            if (hudInput_ && pointerFromEvent(event, panelTextureHeight_, input) &&
                input.type != PointerInput::Type::Scroll) {
                result.hudPointer.push_back(input);
            }
        }
        if (dockHandle_ != 0 && polling_.polls(dockloop::EventPolling::Dock, all, now)) {
            while (vr::VROverlay()->PollNextOverlayEvent(dockHandle_, &event, sizeof(event))) {
                if (pointerFromEvent(event, dockTextureHeight_, input)) result.dockPointer.push_back(input);
                if (event.eventType == vr::VREvent_KeyboardDone) {  // a profile renamed from the Dock's full view
                    char text[256] = {};
                    vr::VROverlay()->GetKeyboardText(text, sizeof(text));
                    result.keyboardDone = true;
                    result.keyboardText = text;
                }
                if (event.eventType == vr::VREvent_KeyboardClosed) result.keyboardClosed = true;
            }
        }
        // The browser panel and its keyboard
        for (int i = 0; i < 2; ++i) {
            const AuxOverlay& a = aux_[i];
            if (a.handle == 0 ||
                !polling_.polls(i == 0 ? dockloop::EventPolling::Browser : dockloop::EventPolling::Keyboard, all, now)) {
                continue;
            }
            std::vector<PointerInput>& out = i == 0 ? result.browserPointer : result.keyboardPointer;
            while (vr::VROverlay()->PollNextOverlayEvent(a.handle, &event, sizeof(event))) {
                // The browser panel takes the right button too (its context menu); the keyboard doesn't
                if (pointerFromEvent(event, a.textureHeight, input, i == 0)) out.push_back(input);
            }
        }
    }
    if (dashboardHandle_ != 0 && (includeSystem || includeDashboard)) {
        while (vr::VROverlay()->PollNextOverlayEvent(dashboardHandle_, &event, sizeof(event))) {
            // マウス座標は左下が原点なので、上が原点になるよう反転する
            const double x = event.data.mouse.x;
            const double y = settingsHeight_ - event.data.mouse.y;
            switch (event.eventType) {
                case vr::VREvent_MouseMove: result.pointer.push_back({PointerInput::Type::Move, x, y}); break;
                case vr::VREvent_MouseButtonDown:
                    if (event.data.mouse.button == vr::VRMouseButton_Left) {
                        PointerInput down {PointerInput::Type::Down, x, y};
                        down.device = event.trackedDeviceIndex;
                        result.pointer.push_back(down);
                    }
                    break;
                case vr::VREvent_MouseButtonUp:
                    if (event.data.mouse.button == vr::VRMouseButton_Left) {
                        result.pointer.push_back({PointerInput::Type::Up, x, y});
                    }
                    break;
                case vr::VREvent_FocusLeave: result.pointer.push_back({PointerInput::Type::Leave, 0, 0}); break;
                case vr::VREvent_ScrollSmooth: {
                    PointerInput scroll {PointerInput::Type::Scroll, 0, 0};
                    scroll.scrollDy = event.data.scroll.ydelta;
                    result.pointer.push_back(scroll);
                    break;
                }
                case vr::VREvent_KeyboardDone: {  // a profile name
                    char text[256] = {};
                    vr::VROverlay()->GetKeyboardText(text, sizeof(text));
                    result.keyboardDone = true;
                    result.keyboardText = text;
                    break;
                }
                case vr::VREvent_KeyboardClosed: result.keyboardClosed = true; break;
                // ダッシュボードのアイコンにホバーしたときの「閉じる」（VROverlayFlags_EnableControlBarClose）。
                // フラグを立てたこのオーバーレイにだけ VREvent_OverlayClosed が届く。
                // SteamVR 自体の終了（VRSystem 側の VREvent_Quit）とは別のイベント
                case vr::VREvent_OverlayClosed:
                    std::fprintf(stderr, "[VR] ダッシュボードの「閉じる」が押されました\n");
                    result.closeRequested = true;
                    break;
                default: break;
            }
        }
    }
    if (includeSystem && facingCheckAt_ > 0 && nowSeconds() >= facingCheckAt_) {
        facingCheckAt_ = 0;
        logFacingCheck(facingCheckConfig_);
    }
    if (result.quit) {
        std::fprintf(stderr, "[VR] SteamVR から終了の知らせが来ました\n");
        vr::VRSystem()->AcknowledgeQuit_Exiting();
    }
}

int VrOverlay::hmdActivityLevel() const {
    if (!connected_) return vr::k_EDeviceActivityLevel_Unknown;
    return vr::VRSystem()->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
}

bool VrOverlay::steamVrAlive(double now) {
    if (vrserverPid_ <= 0) {
        // Not found at connect (it may have been starting): looked for again, every few seconds (a /proc scan)
        if (now < nextVrserverLookup_) return true;  // 確かめられないときは生きている扱い
        nextVrserverLookup_ = now + kVrserverLookupSec;
        vrserverPid_ = findVrserverPid();
        if (vrserverPid_ > 0) std::fprintf(stderr, "[VR] vrserver is PID %d\n", vrserverPid_);
        return true;
    }
    // By its name, not only kill(pid, 0): a PID reused by another process after vrserver went isn't vrserver
    const std::string path = "/proc/" + std::to_string(vrserverPid_) + "/comm";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno != ENOENT && errno != ESRCH;
    char comm[32] = {};
    const ssize_t n = ::read(fd, comm, sizeof(comm) - 1);
    ::close(fd);
    return n <= 0 || std::strncmp(comm, "vrserver\n", 9) == 0;
}

void VrOverlay::pruneRetired(double now) {
    // Kept kRetiredKeepSec after it was replaced (SteamVR may still show it), then freed if its upload is done, else
    // left to the graveyard; never waited for here
    const auto drop = [&](std::unique_ptr<OverlayTexture>& texture) {
        if (texture && !texture->destroyIfIdle()) graveyard_.push_back(std::move(texture));
        texture.reset();
    };
    for (auto it = retiredPanel_.begin(); it != retiredPanel_.end();) {
        if (now - it->at < kRetiredKeepSec) {
            ++it;
            continue;
        }
        drop(it->texture);
        it = retiredPanel_.erase(it);
    }
    if (retiredDockTexture_ && now - retiredDockAt_ >= kRetiredKeepSec) drop(retiredDockTexture_);
    for (AuxOverlay& a : aux_) {
        if (a.retired && now - a.retiredAt >= kRetiredKeepSec) drop(a.retired);
    }
}

void VrOverlay::releaseIdleTextures(double now) {
    buryIdle();
    if (connected_) pruneRetired(now);  // also while nothing is sent (the HUD hidden, unchanged)
    // A panel hidden this long (closed, collapsed) gives its GPU memory back; the next submit makes a new texture
    const auto release = [&](uint64_t handle, std::unique_ptr<OverlayTexture>& texture,
                             std::unique_ptr<OverlayTexture>& retired, int& w, int& h, const char* name) {
        if (!texture) return;
        vr::VROverlay()->ClearOverlayTexture(handle);
        if (retired) retired->destroy();
        retired.reset();
        if (!texture->destroyIfIdle()) graveyard_.push_back(std::move(texture));
        texture.reset();
        w = h = 0;
        std::fprintf(stderr, "[VR] The %s's texture freed (hidden for %.0f s)\n", name, kIdleTextureSec);
    };
    if (!connected_) return;
    if (dockHandle_ != 0 && dockloop::idleTextureDue(dockShown_, dockTexture_ != nullptr, dockHiddenAt_, now)) {
        release(dockHandle_, dockTexture_, retiredDockTexture_, dockTextureWidth_, dockTextureHeight_, "Dock panel");
    }
    for (int i = 0; i < 3; ++i) {
        AuxOverlay& a = aux_[i];
        if (a.handle != 0 && dockloop::idleTextureDue(a.shown, a.texture != nullptr, a.hiddenAt, now)) {
            release(a.handle, a.texture, a.retired, a.textureWidth, a.textureHeight,
                    i == 0 ? "browser panel" : i == 1 ? "keyboard" : "notice");
        }
    }
}

bool VrOverlay::dashboardOpen() const {
    return connected_ && !readOnly_ && vr::VROverlay()->IsDashboardVisible();
}

SettingsVisibility VrOverlay::settingsVisibility() const {
    SettingsVisibility result;
    if (!connected_ || readOnly_ || dashboardHandle_ == 0) return result;
    vr::IVROverlay* overlay = vr::VROverlay();
    result.dashboardVisible = overlay->IsDashboardVisible();
    // ダッシュボードが閉じていれば残りは聞かない（IPC を減らす）
    if (!result.dashboardVisible) return result;
    result.activeTab = overlay->IsActiveDashboardOverlay(dashboardHandle_);
    result.overlayVisible = overlay->IsOverlayVisible(dashboardHandle_);
    return result;
}

bool VrOverlay::submitThumbnail(const uint8_t* rgba, int size) {
    if (!connected_ || thumbnailHandle_ == 0) return false;
    std::string message;
    if (!thumbnailTexture_.ready() && !thumbnailTexture_.create(vulkan_, size, size, message)) {
        std::fprintf(stderr, "[Vulkan] サムネイルのテクスチャを作れません: %s\n", message.c_str());
        return false;
    }
    if (!thumbnailTexture_.update(thumbnailHandle_, rgba, message)) {
        std::fprintf(stderr, "[VR] サムネイルを送れません: %s\n", message.c_str());
        return false;
    }
    return true;
}

double VrOverlay::displayHz(double now) {
    if (hzCheckedAt_ < 0 || now - hzCheckedAt_ > 5.0) {
        hzCheckedAt_ = now;
        vr::ETrackedPropertyError error = vr::TrackedProp_Success;
        const float hz = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                                        vr::Prop_DisplayFrequency_Float, &error);
        displayHz_ = (error == vr::TrackedProp_Success && hz > 1.0f) ? hz : 0.0;
    }
    return displayHz_;
}

namespace {

/** 直近 1 秒ぶんのフレームタイミングから数えた値。 */
struct WindowStats {
    double fps = std::numeric_limits<double>::quiet_NaN();  ///< アプリの新しく表示されたフレーム（毎秒）
    double reprojectedPct = 0.0;  ///< 表示回数のうち、同じフレームの 2 回目以降（再投影で埋めたコマ）の %
    uint32_t dropped = 0;         ///< m_nNumDroppedFrames の合計
    int throttledFrames = 0;      ///< VRCompositor_ThrottleMask の段数（最新の確定したフレーム）
};

/**
 * 直近 1 秒ぶんのフレームタイミングを数える。1 件 = アプリの新しいフレーム 1 枚（m_nFrameIndex は重ならず、
 * 同じフレームを何回表示したかは m_nNumFramePresents に入る）なので:
 * - fps: 1 回以上表示されたフレームの数を、その時刻（m_flSystemTimeInSeconds、vsync にそろった時刻）の幅で割る
 * - 再投影: （表示回数の合計 − 新しいフレームの数）÷ 表示回数の合計。2 回ずつ表示なら 50%
 * いちばん新しい 1 件はまだ表示回数が決まっていないことがあるので数えない。
 * @param timings 古い順のフレームタイミング
 * @param count 件数
 * @return 数えた値（数えられなければ fps は NaN）
 */
WindowStats measureWindow(const vr::Compositor_FrameTiming* timings, uint32_t count) {
    WindowStats result;
    if (count < 3) return result;
    const uint32_t last = count - 2;  // 最新の 1 件は除く
    const double end = timings[last].m_flSystemTimeInSeconds;
    int shown = 0;
    uint32_t presents = 0;
    double first = end;
    for (int i = static_cast<int>(last); i >= 0; --i) {
        const vr::Compositor_FrameTiming& t = timings[i];
        if (t.m_flSystemTimeInSeconds <= end - 1.0) break;
        result.dropped += t.m_nNumDroppedFrames;
        if (t.m_nNumFramePresents == 0) continue;  // 一度も表示されなかった（目に届いていない）
        ++shown;
        presents += t.m_nNumFramePresents;
        first = t.m_flSystemTimeInSeconds;
    }
    // 1 秒に 2 枚未満（読み込み中など）は、数えた枚数をそのまま毎秒の値にする
    result.fps = (shown < 2 || end <= first) ? shown : (shown - 1) / (end - first);
    result.reprojectedPct = presents > 0 ? 100.0 * (presents - shown) / presents : 0.0;
    result.throttledFrames = static_cast<int>(VR_COMPOSITOR_NUMBER_OF_THROTTLED_FRAMES(timings[last]));
    return result;
}

}  // namespace

FrameStats VrOverlay::readFrameStats() {
    FrameStats stats;
    if (!connected_) return stats;

    stats.displayHz = displayHz(nowSeconds());
    stats.targetMs = stats.displayHz > 0 ? 1000.0 / stats.displayHz : 0.0;

    // 直近のフレームを古い順にもらい、前回より新しいものだけ集計する
    auto* timings = reinterpret_cast<vr::Compositor_FrameTiming*>(timingBuffer_.data());
    std::memset(timings, 0, timingBuffer_.size());
    timings[0].m_nSize = sizeof(vr::Compositor_FrameTiming);
    const uint32_t count = vr::VRCompositor()->GetFrameTimings(timings, kMaxTimings);

    double gpuSum = 0.0;
    double cpuSum = 0.0;
    uint32_t newest = lastFrameIndex_;
    for (uint32_t i = 0; i < count; ++i) {
        const vr::Compositor_FrameTiming& t = timings[i];
        if (lastFrameIndex_ != 0 && t.m_nFrameIndex <= lastFrameIndex_) continue;
        const double gpu = t.m_flTotalRenderGpuMs;
        // アプリの CPU 時間（姿勢を受け取ってから 2 回目の Submit まで）+ コンポジタの CPU 時間
        const double appCpu = std::max(0.0, static_cast<double>(t.m_flNewFrameReadyMs - t.m_flNewPosesReadyMs));
        const double cpu = appCpu + t.m_flCompositorRenderCpuMs;
        gpuSum += gpu;
        cpuSum += cpu;
        stats.gpuMaxMs = std::max(stats.gpuMaxMs, gpu);
        stats.cpuMaxMs = std::max(stats.cpuMaxMs, cpu);
        ++stats.frames;
        newest = std::max(newest, t.m_nFrameIndex);
    }
    const bool firstRead = !haveFrameIndex_;
    lastFrameIndex_ = newest;
    haveFrameIndex_ = count > 0;
    stats.valid = count > 0;
    if (stats.frames > 0) {
        stats.gpuMs = gpuSum / stats.frames;
        stats.cpuMs = cpuSum / stats.frames;
        stats.haveTimes = true;
    }
    if (count > 0) {
        // fps・再投影・落ち・抑えている段数は、同じ直近 1 秒のフレームタイミングから出す
        // （以前の累積統計 GetCumulativeStats の差分はやめた。数える元をそろえるため）
        const WindowStats window = measureWindow(timings, count);
        // 前回から新しいフレームが 1 つも来ていなければ、アプリは止まっている（0 fps）
        stats.appFps = (!firstRead && stats.frames == 0) ? 0.0 : window.fps;
        stats.reprojectedPct = window.reprojectedPct;
        stats.dropped = window.dropped;
        stats.throttledFrames = window.throttledFrames;
        if (stats.haveTimes) stats.gpuBusyPct = estimateGpuBusyPct(stats.gpuMs, stats.appFps);
    }
    return stats;
}

namespace {

/**
 * 役割（左手・右手）のコントローラーの電池を読む。
 * @param role 役割
 * @return 電池の状態
 */
ControllerBattery readControllerBattery(vr::ETrackedControllerRole role) {
    ControllerBattery battery;
    vr::IVRSystem* system = vr::VRSystem();
    const vr::TrackedDeviceIndex_t index = system->GetTrackedDeviceIndexForControllerRole(role);
    if (index == vr::k_unTrackedDeviceIndexInvalid || !system->IsTrackedDeviceConnected(index)) return battery;
    battery.present = true;
    vr::ETrackedPropertyError error = vr::TrackedProp_Success;
    const float fraction = system->GetFloatTrackedDeviceProperty(index, vr::Prop_DeviceBatteryPercentage_Float, &error);
    if (error == vr::TrackedProp_Success) battery.pct = std::clamp(fraction * 100.0, 0.0, 100.0);  // 0〜1 で返る
    error = vr::TrackedProp_Success;
    const bool charging = system->GetBoolTrackedDeviceProperty(index, vr::Prop_DeviceIsCharging_Bool, &error);
    battery.charging = error == vr::TrackedProp_Success && charging;
    return battery;
}

}  // namespace

bool VrOverlay::readSceneApp(SceneApp& app) const {
    app = SceneApp();
    if (!connected_ || vr::VRApplications() == nullptr) return false;
    vr::IVRApplications* apps = vr::VRApplications();
    app.pid = apps->GetCurrentSceneProcessId();
    if (app.pid == 0) return true;
    char key[vr::k_unMaxApplicationKeyLength] = {};
    if (apps->GetApplicationKeyByProcessId(app.pid, key, sizeof(key)) == vr::VRApplicationError_None && key[0] != '\0') {
        app.key = key;
        char name[256] = {};
        vr::EVRApplicationError error = vr::VRApplicationError_None;
        apps->GetApplicationPropertyString(key, vr::VRApplicationProperty_Name_String, name, sizeof(name), &error);
        if (error == vr::VRApplicationError_None) app.name = name;
        return true;
    }
    // A scene process SteamVR has no app key for (e.g. a sideloaded app): name it by its process
    const std::string comm = processName(app.pid);
    if (!comm.empty() && comm != "?") {
        app.key = "process." + comm;
        app.name = comm;
    }
    return true;
}

ControllerStatus VrOverlay::readControllers() {
    ControllerStatus status;
    if (!connected_) return status;
    status.left = readControllerBattery(vr::TrackedControllerRole_LeftHand);
    status.right = readControllerBattery(vr::TrackedControllerRole_RightHand);
    return status;
}

void VrOverlay::dumpFrameTimings(double seconds, const volatile int& stop) {
    if (!connected_) return;
    auto* timings = reinterpret_cast<vr::Compositor_FrameTiming*>(timingBuffer_.data());
    uint32_t last = 0;
    const double start = nowSeconds();
    std::printf("# t(s) index presents mispresented dropped reprojFlags systemTime(s) newFrameReady(ms) gpu(ms)\n");
    while (!stop && nowSeconds() - start < seconds) {
        std::memset(timings, 0, timingBuffer_.size());
        timings[0].m_nSize = sizeof(vr::Compositor_FrameTiming);
        const uint32_t count = vr::VRCompositor()->GetFrameTimings(timings, kMaxTimings);
        const uint32_t focus = vr::VRCompositor()->GetCurrentSceneFocusProcess();
        const uint32_t renderer = vr::VRCompositor()->GetLastFrameRenderer();
        const WindowStats window = measureWindow(timings, count);
        std::printf("# t=%.2f 返ってきた件数=%u 表示 %.1fHz アプリ %.1ffps 再投影 %.1f%% 落ち %u 抑え %d 段 "
                    "シーンのプロセス=%u(%s) 最後に描いたプロセス=%u(%s)\n",
                    nowSeconds() - start, count, displayHz(nowSeconds()), window.fps, window.reprojectedPct,
                    window.dropped, window.throttledFrames, focus, processName(focus).c_str(), renderer,
                    processName(renderer).c_str());
        for (uint32_t i = 0; i < count; ++i) {
            const vr::Compositor_FrameTiming& t = timings[i];
            if (last != 0 && t.m_nFrameIndex <= last) continue;
            if (last == 0 && i + 12 < count) continue;  // 初回は最後の 12 件だけ
            std::printf("%.2f %u %u %u %u 0x%x %.4f %.2f %.2f\n", nowSeconds() - start, t.m_nFrameIndex,
                        t.m_nNumFramePresents, t.m_nNumMisPresented, t.m_nNumDroppedFrames, t.m_nReprojectionFlags,
                        t.m_flSystemTimeInSeconds, t.m_flNewFrameReadyMs, t.m_flTotalRenderGpuMs);
        }
        if (count > 0) last = std::max(last, timings[count - 1].m_nFrameIndex);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

double VrOverlay::panelShiftM() const {
    return panelTextureWidth_ > 0 ? hudWidthM() * panelExtraPx_ / panelTextureWidth_ / 2 : 0.0;
}

vr::HmdMatrix34_t VrOverlay::shiftedPanelTransform() const {
    // Down along the panel's own y (its second column) by half the extra rows: the top keeps its place
    vr::HmdMatrix34_t m = basePanelTransform_;
    const double shift = panelShiftM();
    for (int row = 0; row < 3; ++row) m.m[row][3] -= static_cast<float>(m.m[row][1] * shift);
    return m;
}

dockmath::Mat34 VrOverlay::hudBaseFrom(const dockmath::Mat34& overlayLocal) const {
    dockmath::Mat34 m = overlayLocal;
    const double shift = panelShiftM();
    for (int row = 0; row < 3; ++row) m.m[row][3] += static_cast<float>(m.m[row][1] * shift);
    return m;
}

bool VrOverlay::submitPanel(const uint8_t* pixels, int width, int height, double widthColumns, int extraBottomPx) {
    if (!connected_ || panelHandle_ == 0) return false;
    std::string message;
    // A texture of an earlier size has been off-screen for kRetiredKeepSec: safe to free now (the video controls can
    // change the size twice within a second, so the next submit alone is no guarantee)
    buryIdle();
    const double now = nowSeconds();
    pruneRetired(now);
    if (panelTexture_ && panelTexture_->broken()) {
        bury(panelTexture_, "HUD");
        panelTextureWidth_ = panelTextureHeight_ = 0;  // a new one below
    }
    // A new size: a new texture (pendingPanel_), which takes over (size, laser hit area, width) only once its picture
    // went up. Until then the old one stays in place as SteamVR shows it. A pending one is used again by the next try
    // at its size: an upload stuck on the GPU then fails at once (busy) rather than blocking for a new texture each
    // time, and is buried once broken()
    const bool resize = !panelTexture_ || width != panelTextureWidth_ || height != panelTextureHeight_;
    if (pendingPanel_ && (pendingPanel_->broken() || !resize || width != pendingWidth_ || height != pendingHeight_)) {
        if (pendingPanel_->broken()) {
            bury(pendingPanel_, "HUD");
        } else {
            graveyard_.push_back(std::move(pendingPanel_));  // never shown: freed once its upload is done
        }
    }
    if (resize && !pendingPanel_) {
        auto next = std::make_unique<OverlayTexture>();
        if (!next->create(vulkan_, width, height, message, panelBgra_)) {
            logFailure("panel texture", "[VR] can't make a " + std::to_string(width) + "x" + std::to_string(height) +
                                            " panel texture: " + message, message);
            return false;
        }
        pendingPanel_ = std::move(next);
        pendingWidth_ = width;
        pendingHeight_ = height;
    }
    const bool ok = (resize ? pendingPanel_ : panelTexture_)->update(panelHandle_, pixels, message);
    if (resize && ok) {
        std::fprintf(stderr, "[VR] panel texture %dx%d -> %dx%d (%.2f columns wide)\n", panelTextureWidth_,
                     panelTextureHeight_, width, height, widthColumns);
        if (panelTexture_) retiredPanel_.push_back({std::move(panelTexture_), now});
        if (retiredPanel_.size() > kMaxRetired) {  // many sizes within kRetiredKeepSec: the oldest goes on its upload
            graveyard_.push_back(std::move(retiredPanel_.front().texture));
            retiredPanel_.erase(retiredPanel_.begin());
        }
        panelTexture_ = std::move(pendingPanel_);
        panelTextureWidth_ = width;
        panelTextureHeight_ = height;
        if (hudInput_) {  // the laser's hit area follows the new size
            const vr::HmdVector2_t scale = {{static_cast<float>(width), static_cast<float>(height)}};
            checkOverlay("SetOverlayMouseScale(HUD)", vr::VROverlay()->SetOverlayMouseScale(panelHandle_, &scale));
        }
    }
    // The size (or the rows under the HUD's own picture) changed: the width, then the centre moves so the picture
    // doesn't. Sent right after the new picture, so the HUD doesn't jump for a frame (a carried HUD keeps its grab
    // until let go); not after a failed one (the old picture still shows as it was)
    bool moved = false;
    if (ok) {
        if (widthColumns != panelColumns_) {
            panelColumns_ = std::max(0.1, widthColumns);
            checkOverlay("SetOverlayWidthInMeters", vr::VROverlay()->SetOverlayWidthInMeters(
                                                        panelHandle_, static_cast<float>(configWidthM_ * panelColumns_)));
        }
        panelExtraPx_ = extraBottomPx;
        const vr::HmdMatrix34_t shifted = shiftedPanelTransform();
        moved = std::memcmp(&shifted, &panelTransform_, sizeof(shifted)) != 0;
        if (moved) {
            panelTransform_ = shifted;
            transformDirty_ = true;
            nextPlacementAt_ = std::min(nextPlacementAt_, now);
        }
    }
    if (moved && !hudGrabbed_ && attachedDevice_ < vr::k_unMaxTrackedDeviceCount &&
        checkOverlay("SetOverlayTransformTrackedDeviceRelative(HUD resized)",
                     vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(panelHandle_, attachedDevice_,
                                                                              &panelTransform_))) {
        transformDirty_ = false;
    }
    // 同じエラーを毎回出さない
    if (ok) {
        failureLog().clear("panel texture");
    } else {
        logFailure("panel texture", "[VR] パネルを送れません: " + message, message);
    }
    return ok;
}

bool VrOverlay::showKeyboard(const std::string& description, const std::string& existing, uint32_t maxChars,
                             std::string& error, bool forDock) {
    const uint64_t handle = forDock && dockHandle_ != 0 ? dockHandle_ : dashboardHandle_;
    if (!connected_ || handle == 0) {
        error = "no dashboard tab";
        return false;
    }
    const vr::EVROverlayError e = vr::VROverlay()->ShowKeyboardForOverlay(
        handle, vr::k_EGamepadTextInputModeNormal, vr::k_EGamepadTextInputLineModeSingleLine,
        vr::KeyboardFlag_Modal, description.c_str(), maxChars, existing.c_str(), 0);
    if (e != vr::VROverlayError_None) {
        error = overlayErrorName(e);
        return false;
    }
    return true;
}

bool VrOverlay::submitSettings(const uint8_t* pixels) {
    if (!connected_ || dashboardHandle_ == 0 || !settingsTexture_.ready()) return false;
    std::string message;
    const bool ok = settingsTexture_.update(dashboardHandle_, pixels, message);
    if (ok) {
        failureLog().clear("settings texture");
    } else {
        logFailure("settings texture", "[VR] 設定パネルを送れません: " + message, message);
    }
    return ok;
}
