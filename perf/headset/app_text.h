// Frame Dock's own UI strings in Korean, English and Japanese, and the language the app shows now. The older tables
// stay where they were: the dashboard's settings pages (i18n.h's UiText), the widgets (core/render/widget_text.h), the
// Color page (src/color_i18n.h). Everything Frame Dock added (the Dock panel, the browser panel and its keyboard, the
// 기기 page, notices, the first-run guide) looks its words up here. Logs stay out of it.
//
// The language is one setting for the whole app (Config::language, resolved from "auto" at start): main sets it here
// with setUiLanguage at start and whenever it changes, and every view and message reads uiLanguage(). Any thread may
// read it (the browser's and Frame Sync's threads word their messages with it).
#pragma once

#include "i18n.h"  // Language

#include <initializer_list>
#include <string>

/** @return the language the app shows now (Korean until main sets it) */
Language uiLanguage();
/** Sets the language the app shows (main: at start and when the setting changes; tests). */
void setUiLanguage(Language language);

/** One string of the table (app_text.cpp keeps the rows in this order). "{0}", "{1}": filled in by trf. */
enum class Tx {
    // ---- The dashboard header and its pages ----
    PageBrowser,
    PageDevice,
    // ---- The Browser page ----
    BrowserCardPicture,
    BrowserRowFps,
    BrowserRowResolution,
    BrowserRowMode,
    BrowserRowClosedSound,
    BrowserRowRemovedVideo,
    BrowserHeadless,
    BrowserWindow,
    BrowserKeep,          ///< sound when closed / video when the headset is off: keeps playing
    BrowserPause,         ///< ... pauses
    BrowserRowIdle,
    BrowserRowPlace,
    BrowserRowNow,
    BrowserIdleMinutes,   ///< "{0}분"
    BrowserIdleHint,
    BrowserGuideTitle,
    BrowserGuideTap,
    BrowserGuideSelect,
    BrowserGuideMenu,
    BrowserGuideThumbstick,
    BrowserGuideMove,
    BrowserGuidePinned,
    BrowserHome,          ///< "홈: {0}"
    BrowserHomeFromPage,
    BrowserStopChrome,
    BrowserStopChromeAgain,  ///< 끄기 armed (the button)
    BrowserStartChrome,
    BrowserNeedsDock,     ///< Chrome 켜기 while in-game interaction is off
    PaletteControls,      ///< the Layout page's palette: the controls' group
    BrowserPageNote,      ///< the footer's line on the Browser page
    // ---- Chrome's state (the Browser page's line) ----
    ChromeOff,
    ChromeStarting,
    ChromeRunning,        ///< "실행 중 (PID {0})"
    ChromeRunningPipe,
    ChromeRunningPort,
    ChromeClosing,
    ChromeRestarting,     ///< "다시 시작하는 중 ({0}번째)"
    ChromeFailed,         ///< "실패: {0}"
    // ---- The Dock panel ----
    DockHandleHint,
    DockFullTitle,
    DockAdjustHint,
    DockDeviceTitle,
    DockQuitChrome,
    DockQuitAgain,
    DockFull,
    DockQuick,
    DockCollapse,
    DockDone,
    DockUsing,            ///< "사용 중: {0}"
    DockUnsaved,
    DockNoActive,
    DockAwayHint,
    DockHeadset,
    DockLeftHand,
    DockRightHand,
    DockOff,              ///< a controller that isn't on
    // ---- The 기기 page and the Dock's 소리 · 패스스루 card ----
    DeviceSound,
    DeviceOutput,
    DeviceVolume,
    DeviceMute,
    DeviceChecking,
    DeviceNoOutputs,
    DeviceOnlyOutput,
    DeviceSpeakers,       ///< the headset's own speakers
    DevicePassthrough,
    DevicePassView,       ///< the passthrough row's title on the page
    DevicePassOff,
    DevicePassMono,
    DevicePassColor,
    DevicePassOn,
    DevicePassNote1,
    DevicePassNote2,
    DevicePassNoCameraSwitch,
    DevicePassUnknown,
    DevicePassRemembered,
    DevicePassColorUnknown,
    // ---- PC 계정 맞추기 (Frame Sync) ----
    SyncTitle,
    SyncShutdown,
    SyncCancel,
    SyncOthers,
    SyncChoose,
    SyncNoOthers,
    SyncMore,             ///< "외 {0}개"
    SyncNoSettings,
    SyncPcWaking,
    SyncStreaming,
    SyncInGame,
    SyncAskGame,
    SyncContinue,
    SyncAskShutdown,
    SyncTurnOff,
    SyncSwitchedFromList,
    SyncUnreachable,
    SyncAuth,
    SyncLogin,
    SyncResult,
    SyncCancelled,
    SyncAgentHttp,        ///< "집 PC 에이전트 오류(HTTP {0})"
    SyncUnexpected,       ///< "예상하지 못한 오류: {0}"
    SyncStripAuth,
    SyncStripError,
    SyncStripChecking,
    SyncStripOn,
    SyncStripOff,
    SyncBigError,
    SyncBigNoFrameAccount,
    SyncBigNotOnPc,       ///< "{0} 계정이 ..."
    SyncBigWake,          ///< "... {0}로 스팀 켜기"
    SyncBigSwitch,
    SyncBigReady,
    SyncStepWaking,
    SyncStepLogin,
    SyncStepSwitching,
    SyncStepDone,
    SyncQuitting,
    SyncShutdownSoon,
    SyncShutdownFailed,
    // ---- The browser panel ----
    BrowserTitle,
    BrowserClose,
    BrowserQuit,
    BrowserAgain,
    BrowserDragScroll,
    BrowserDragSelect,
    BrowserUnpin,
    BrowserPin,
    BrowserCancel,
    BrowserOk,
    BrowserAddressHint,
    BrowserTranslate,
    BrowserView,
    BrowserFavorites,
    BrowserKeyboard,
    BrowserLoading,
    BrowserFavoritesEmpty,
    BrowserFavoritesFull,
    BrowserFavoritesUnsaved,
    BrowserViewSite,
    BrowserViewDesktop,
    BrowserViewMobile,
    BrowserViewZoom,
    BrowserViewZoomReset,
    BrowserViewAspect,
    BrowserViewAspectNote,
    BrowserMenuCopy,
    BrowserMenuPaste,
    BrowserMenuSelectAll,
    BrowserMenuOpenLink,
    BrowserDialogLeave,
    BrowserDialogPrompt,  ///< "페이지의 질문 (확인은 기본값으로 답함): {0}"
    BrowserDialogAsk,     ///< "페이지의 질문: {0}"
    BrowserStatusOff,
    BrowserStatusStarting,
    BrowserStatusConnecting,
    BrowserStatusClosing,
    BrowserStatusRestarting,  ///< "... ({0}번째)"
    BrowserStatusFailed,
    BrowserStatusRetry,
    BrowserKeyboardAddress,
    BrowserKeyboardPage,
    BrowserThreadFailed,  ///< "...: {0}"
    // ---- Chrome's errors (the panel's message) ----
    ChromeNoProfile,
    ChromeStartFailed,    ///< "...: {0}"
    ChromeExited,         ///< "Chrome이 종료됐어요 ({0})"
    ChromePipeClosed,
    ChromeNotAnswering,
    ChromeDevToolsFailed,
    ChromeDevToolsLost,
    ChromeDevToolsSilent,
    ChromePageFailed,
    ChromePageFailedWhy,  ///< "...: {0}"
    ChromeAttachFailed,
    ChromeAttachFailedWhy,
    ChromeCrashBlank,     ///< "...: {0}"
    ChromeCrashHome,
    ChromePageHung,
    // ---- The VR keyboard ----
    KeyboardTitle,
    KeyboardHandleHint,
    KeyboardClose,
    KeyboardHintHangul,
    KeyboardHintOther,
    KeyboardComposing,
    KeyboardHangul,       ///< the Hangul layout's name (the space bar, the badge)
    KeyboardEnglish,
    KeyboardSymbols,      ///< the space bar on the symbols layout
    KeyboardSymbolsBadge,
    KeyboardClear,
    KeyboardLatinKey,     ///< the second half of 한/영
    // ---- The HUD's video controls ----
    MediaPlaying,
    MediaPaused,
    MediaPausedByRemoval,
    MediaBack,
    MediaForward,
    MediaPause,
    MediaPlay,
    MediaMute,
    MediaBrowser,
    // ---- Profiles' errors and names (ProfileStore) ----
    ProfileDefaultName,   ///< "프로필 {0}"
    ProfileBasic,         ///< the built-in template "기본"
    ProfileDetailed,      ///< the built-in template "상세"
    ProfileWriteFailed,
    ProfileTooMany,       ///< "...{0}개까지..."
    ProfileMissing,
    ProfileDeleteFailed,
    ProfileConfigSaveFailed,
    // ---- Notices in front of the eyes ----
    NoticeHeadsetLow,              ///< "헤드셋 배터리 {0} 남았어요"
    NoticeHeadsetLowTime,          ///< "헤드셋 배터리 {0} · {1} 남았어요"
    NoticeHeadsetCritical,
    NoticeHeadsetCriticalTime,
    NoticeLeftControllerLow,       ///< "{0}": the %
    NoticeRightControllerLow,
    // ---- The first-run guide ----
    GuideTitle,
    GuideWristLabel,
    GuideWrist,
    GuidePressLabel,
    GuidePress,
    GuideMoveLabel,
    GuideMove,
    GuideSettingsLabel,
    GuideSettings,
    GuideFooter,
    // ---- 녹화 (framecorder, when installed) ----
    DockRecord,                    ///< the Dock's button (recording: the elapsed time instead)
    DockClip,                      ///< next to it while framecorder's clips are on
    // ---- The new version notice (version_notice.h) ----
    UpdateCheckRow,                ///< the footer's toggle
    UpdateAvailable,               ///< "{0}": the tag ("v0.1.1"), "{1}": the Releases page
    Count
};

/** @return the string in uiLanguage() */
const char* tr(Tx key);
/** @return the string in a language */
const char* tr(Tx key, Language language);
/** @return tr(key) with "{0}", "{1}", ... replaced by args (an index past them stays as it is) */
std::string trf(Tx key, std::initializer_list<std::string> args);
/** @return pattern with "{0}", "{1}", ... replaced by args */
std::string formatText(const std::string& pattern, std::initializer_list<std::string> args);

/**
 * The table's own check (tests): every row is at its key's place and has a non-empty string in each language.
 * @param problem the first row that isn't (its key's number and why)
 */
bool textTableComplete(std::string& problem);
