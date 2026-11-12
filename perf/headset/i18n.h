// 画面に出す文言の表（日本語・英語・韓国語）。描画コードの中に文言を書かず、ここから引く。
// ログや --print の出力はここには入れない。性能パネルのウィジェットの文言は core/render/widget_text にある。
#pragma once

#include "widget_text.h"  // the widgets' strings (core/render)

#include <string>

/** 表示の言語。config.json's "language": "auto" (Steam's language) or a fixed one. */
enum class Language { Ja, En, Ko };

/**
 * Steam's client language in registry.vdf's text: the "language" value under Registry / HKCU / Software / Valve /
 * Steam (keys compared without case), e.g. "koreana", "japanese", "english".
 * @return the value, "" if there is none
 */
std::string steamLanguageFromVdf(const std::string& text);

/**
 * What "auto" stands for. A Steam language that could be read decides: "koreana" -> Ko, "japanese" -> Ja, any other
 * -> En. Without one (registry.vdf missing or without it) the locale does, as POSIX looks it up: the first set of
 * LC_ALL, LC_MESSAGES, LANG (ko* -> Ko, ja* -> Ja); else En.
 * @param steam Steam's language ("" = unknown)
 * @param lcAll, lcMessages, lang the variables (nullptr = unset)
 */
Language autoLanguage(const std::string& steam, const char* lcAll, const char* lcMessages, const char* lang);

/**
 * "auto" for a home folder and the process's locale variables: registry.vdf from ~/.local/share/Steam, then ~/.steam
 * (the first that has the language), then autoLanguage.
 * @param home the home folder ("" = none)
 */
Language detectAutoLanguage(const std::string& home);

/** @return detectAutoLanguage($HOME), worked out once per run (the app looks again at its next start) */
Language systemLanguage();

/**
 * @param language the UI language
 * @return the same language for the widgets' strings (core/render/widget_text.h)
 */
inline frame_perf_overlay::WidgetLanguage widgetLanguage(Language language) {
    switch (language) {
        case Language::Ja: return frame_perf_overlay::WidgetLanguage::Ja;
        case Language::Ko: return frame_perf_overlay::WidgetLanguage::Ko;
        case Language::En: break;
    }
    return frame_perf_overlay::WidgetLanguage::En;
}

/**
 * パネルと設定パネルの文言一式。
 */
struct UiText {
    // ---- 設定パネル ----
    const char* settingsTitle;
    const char* cardPanel;              ///< 左のカードの見出し（パネル）
    const char* sentenceBreak;          ///< 最下行で 2 つの文をつなぐ区切り（。/ . ）
    const char* panelShown;
    const char* panelHidden;
    const char* rowShow;
    const char* rowPosition;
    const char* moveHint;               ///< the Placement page: how to move the panel (the laser)
    const char* rowSize;
    const char* rowOpacity;
    const char* rowAutostart;           ///< 自動起動
    const char* autostartNextLaunch;    ///< 次回の SteamVR 起動から
    const char* autostartNotInstalled;  ///< 未インストール
    const char* autostartBusy;          ///< 切り替え中
    const char* autostartFailed;        ///< 切り替えに失敗
    const char* on;
    const char* off;
    const char* bottomLeft;
    const char* bottomCenter;
    const char* bottomRight;
    const char* topLeft;
    const char* topRight;
    const char* reset;          ///< the Placement page's reset: everything on it (resetDisplaySettings)
    const char* quit;           ///< アプリを終了
    const char* quitConfirm;    ///< もう一度押すと終了
    const char* resetConfirm;   ///< reset armed (the button)
    const char* resetConfirmNote;    ///< reset armed (the bottom line): what goes back to the defaults
    const char* offConfirm;          ///< on the Dock's full view, 表示 / 게임 중 조작 끔 armed (the segment; short)
    const char* showOffConfirmNote;  ///< 表示 끔 armed on the Dock (the bottom line): what goes, how to get it back
    const char* cardWristHide;  ///< the Placement page's right card: hide the panel on the wrist
    const char* wristHideNote;  ///< what tilting does (%.0f twice: where it starts to fade, where it is gone)
    const char* wristHideHeadNote;  ///< on the head: the card isn't used
    const char* footer;         ///< 変更はすぐ反映される旨
    // ---- 固定先・手首・時計 ----
    const char* rowAttachment;    ///< 位置のカードの「固定先」
    const char* attachHead;       ///< 固定先: 頭
    const char* attachLeft;       ///< 固定先: 左手（のコントローラー）
    const char* attachRight;      ///< 固定先: 右手（のコントローラー）
    const char* wristPreset;      ///< 手首のときの位置のボタン（手首の標準の位置と向きに戻す）
    const char* rowWristFade;     ///< 傾けると消す
    const char* rowFadeAngle;     ///< 消える角度
    const char* fadeRangeFormat;  ///< 消える角度の値（%.0f が 2 つ: 薄くなり始める角度、消えきる角度）
    const char* rowClock;         ///< 時計
    const char* clock12;          ///< 12 時間制のボタン
    const char* clock24;          ///< 24 時間制のボタン
    const char* clockLabel;       ///< 性能パネルの時計の見出し
    // ---- Dashboard pages and the layout editor ----
    const char* pageLayout;          ///< page switch: the layout editor
    const char* pagePlacement;       ///< page switch: attach point, position, size, ...
    const char* editorTitle;         ///< layout card title
    const char* editorHint;          ///< layout card hint
    const char* editorNewColumn;     ///< the empty lane on the right
    const char* editorDropToRemove;  ///< palette card while a widget is dragged over it
    const char* paletteTitle;        ///< palette card title
    const char* paletteNetwork;      ///< palette chip for Network (the card's title names the link)
    const char* selectedNone;        ///< no widget selected
    const char* noSettings;          ///< the selected widget has no settings
    const char* removeWidget;        ///< remove the selected widget
    const char* layoutSaved;         ///< status after an applied edit
    const char* layoutStale;         ///< status after a rejected (stale) edit
    const char* layoutSaveFailed;    ///< status after a failed save
    const char* rowCompact;          ///< the short card (off adds the extra rows: Frame, CPU, Battery, Temp, General)
    const char* rowItems;            ///< CPU / RAM items: rows shown
    const char* rowSumSameName;      ///< one row per process name
    const char* rowHideSystem;       ///< leave out SteamOS / Steam / SteamVR (hide_rules.json)
    // ---- The Profiles page ----
    const char* pageProfiles;        ///< page switch
    const char* cardProfiles;        ///< the list's title
    const char* profileCountFormat;  ///< "%zu / %zu" profiles used
    const char* profileNone;         ///< no profiles yet
    const char* profileActive;       ///< chip on the active profile
    const char* profileUnsaved;      ///< chip: the live layout differs from it
    const char* profileUse;
    const char* profileSave;         ///< overwrite the active profile
    const char* profileRename;
    const char* profileDelete;
    const char* profileNew;
    const char* profileReset;        ///< live layout -> the built-in default
    const char* profileTapAgain;     ///< Use armed: the live layout's and placement's changes would be lost
    const char* profileTapAgainLayout;     ///< Use / Reset armed: the live layout's changes would be lost
    const char* profileTapAgainPlacement;  ///< Use armed: the live placement's changes would be lost
    const char* profileDeleteAgain;  ///< Delete armed
    const char* profileSaveAgain;    ///< Save (overwrite) armed: the live layout differs from the profile
    const char* profileNotSaved;     ///< no active profile and the live layout isn't saved in any
    const char* profileHint;         ///< what a profile holds (two lines, split at '\n')
    const char* profileKeyboard;     ///< the keyboard's description when renaming
    const char* profileLayoutOnly;   ///< chip on a profile without placement (Use leaves the placement as it is)
    const char* rowQuickProfiles;    ///< the footer's quick switch
    const char* quickNone;           ///< the quick switch with no profiles
    const char* profileUnreadable;   ///< before the file names of profiles that couldn't be read (left as they are)
    const char* quickTapAgain;       ///< a quick-switch button armed: unsaved changes would be lost, tap it again
    // ---- The Color page (its own strings are in src/color_i18n) ----
    const char* pageColor;           ///< page switch: display colour and sharpness (FrameColor)
    // ---- The Dock page (in-game interaction) ----
    const char* pageDock;            ///< page switch
    const char* cardDockInGame;      ///< card: in-game interaction
    const char* rowDockUse;          ///< on / off
    const char* dockOffConfirmNote;  ///< 끔 armed on the Dock (the bottom line): what goes, how to get it back
    const char* rowDockHand;         ///< which hand points
    const char* dockHandAuto;        ///< the hand without the HUD
    const char* dockHandHint;        ///< under the hand row: what auto means (lines split at '\n')
    const char* cardDockPanel;       ///< card: the Dock panel
    const char* rowDockSize;         ///< its width
    const char* rowDockTimeout;      ///< collapse after
    const char* dockTimeoutFormat;   ///< "%d초" (seconds)
    const char* dockTimeoutNever;    ///< never collapses by itself
    const char* dockResetPlace;      ///< the Dock panel's offsets back to the defaults
    const char* rowDockPlace;        ///< the row of dockResetPlace
    const char* dockPanelHint;       ///< under the Dock panel card's rows: where it sits, how to move it
    const char* cardDockHelp;        ///< card: how to use it
    const char* dockHelp;            ///< lines split at '\n'
    // ---- The header's language button ----
    const char* languageAuto;        ///< its first word while it follows Steam ("자동")
};

/**
 * 言語の文言の表を返す。
 * @param language 言語
 * @return 文言の表（プログラムの終わりまで有効）
 */
const UiText& uiText(Language language);

/**
 * プロファイルのページの、操作の結果の文言。
 * @param language 言語
 * @param what "saved-as" / "new-failed" / "using" / "use-failed" / "saved-to" / "save-failed" / "keyboard-failed" /
 *             "deleted" / "delete-failed" / "reset" / "reset-failed" / "renamed" / "rename-failed" / "gone"
 * @param name プロファイルの名前（使わない文言もある）
 * @param error 失敗の理由（失敗のときだけ使う。韓国語では短い韓国語の理由。空なら理由を付けない）
 * @return 文言
 * @example
 * profileStatusText(Language::Ja, "saved-to", "Racing", "") // "Racing に上書き保存しました"
 */
std::string profileStatusText(Language language, const std::string& what, const std::string& name,
                              const std::string& error);

/**
 * 設定ファイルに書く言語の名前。
 * @param language 言語
 * @return "ja" / "en" / "ko"
 */
const char* languageCode(Language language);

/**
 * 設定ファイルの言語の名前を読む。
 * @param code "ja" / "en" / "ko"
 * @param language 読めたときの書き込み先
 * @return 知っている名前なら true
 */
bool parseLanguage(const std::string& code, Language& language);

/**
 * The language button's next choice: auto -> ko -> en -> ja -> auto (auto shows systemLanguage()).
 * @param automatic set to whether the next one follows Steam
 * @param language set to the language shown next
 */
void nextLanguageChoice(bool& automatic, Language& language);

/**
 * The language button's words, always in the language they name: "자동 · 한국어", "Auto · English", "自動 · 日本語" while
 * it follows Steam, else the language's own name ("한국어", "English", "日本語").
 * @param automatic it follows Steam
 * @param language the language shown (auto: the one it resolved to)
 */
std::string languageButtonText(bool automatic, Language language);

/** @return a language's name in itself: "한국어", "English", "日本語" */
const char* languageName(Language language);
