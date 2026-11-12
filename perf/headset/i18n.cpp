// 画面に出す文言の表の中身。
#include "i18n.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

/** @return text in lower case (ASCII), for registry.vdf's keys */
std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

/** @return the file's text ("" if it can't be read) */
std::string readText(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

const UiText kJapanese = {
    // 設定パネル
    "Frame Dock", "パネル", "。", "性能パネル表示中", "性能パネル非表示",
    "表示", "位置", "移動: パネルをレーザーで指し、トリガーを 0.6 秒押したまま動かします。動かした場所は保存されます", "大きさ", "透明度",
    "自動起動", "自動起動の切り替えは次回の SteamVR 起動から効きます",
    "自動起動のユニットが入っていないので切り替えられません（README の「インストール」を見てね）", "自動起動を切り替え中…",
    "自動起動の切り替えに失敗しました（ログを見てね）",
    "オン", "オフ",
    "左下", "中央下", "右下", "左上", "右上",
    "配置をすべて既定に", "アプリを終了", "もう一度押すと終了",
    "もう一度押すと既定に戻す",
    "もう一度押すと、配置（固定先・位置と向き・大きさ・透明度・手首・時計）がすべて既定に戻ります",
    "もう一度",
    "もう一度押すと性能パネルとドックが消えます。戻すには SteamVR ダッシュボードの Frame Dock タブで表示をオンに（Frame Dock をもう一度起動しても戻ります）",
    "手首で隠す", "手首を返してパネルが斜めになると、%.0f° から薄くなり %.0f° で消えます",
    "パネルが手首に付いているときだけ使います",
    "変更はすぐ反映され、設定ファイルに保存されます",
    // 固定先・手首・時計
    "固定先", "頭", "左手", "右手", "手首の標準の位置",
    "傾けると消す", "消える角度", "%.0f〜%.0f°",
    "時計", "12h", "24h", "現在時刻",
    // ダッシュボードのページ・レイアウトの編集
    "レイアウト",
    "配置",
    "レイアウト",
    "ドラッグで移動、押すと設定を変えられます",
    "新しい列",
    "ここに落とすと外します",
    "ウィジェット",
    "通信",
    "左のウィジェットを押すと変えられます",
    "設定はありません",
    "外す",
    "保存しました",
    "ほかで変わりました。もう一度やってね",
    "レイアウトを保存できませんでした",
    "小さく表示",
    "行数", "同じ名前は 1 行に", "システムを隠す",
    // プロファイルのページ
    "プロファイル",
    "プロファイル",
    "%zu / %zu",
    "まだありません。「新しく作る」で今のレイアウトと配置を保存します",
    "使用中",
    "未保存の変更あり",
    "使う",
    "上書き保存",
    "名前を変える",
    "削除",
    "新しく作る",
    "既定に戻す",
    "もう一度押すと、レイアウトと配置の変更は消えます",
    "もう一度押すと、レイアウトの変更は消えます",
    "もう一度押すと、配置の変更は消えます",
    "もう一度押すと削除",
    "もう一度押すと上書き",
    "今のレイアウトと配置はどのプロファイルにも保存されていません",
    "プロファイルにはレイアウトと配置（固定先・位置と向き・大きさ・透明度・手首・時計）が入ります。\n「レイアウトのみ」のプロファイルは配置を変えません",
    "プロファイルの名前",
    "レイアウトのみ",
    "プロファイル",
    "プロファイルはまだありません",
    "読めないファイル（そのまま残します）: ",
    "保存していない変更があります。もう一度押すと切り替えます",
    // 色補正のページ
    "色補正",
    // ドックのページ
    "ドック",
    "ゲーム中の操作",
    "使う",
    "もう一度押すと、ゲーム中にドックを開けなくなります。戻すには SteamVR ダッシュボードの Frame Dock タブのドックでオンに",
    "指す手",
    "自動",
    "自動: 性能パネルのない方の手（頭のときは両手）\n0.25 秒指し続けるとレーザーが付きます。付いている間、そのコントローラーの入力はトリガーだけでなくすべてゲームに届きません",
    "ドックパネル",
    "大きさ",
    "自動で閉じる",
    "%d 秒",
    "閉じない",
    "標準の位置に戻す",
    "位置",
    "ドックパネルは性能パネルと同じ所（頭・手首）に付き、位置は別に覚えます。動かすには上の取っ手を押し続けます",
    "使い方",
    "指す: 0.25 秒指すと性能パネルに枠が出て、トリガーで押せます\n押す: ドックを開く。もう一度押すと閉じる\n移動: 性能パネルはトリガーを 0.6 秒押し続ける、ドックパネルは上の取っ手を押し続ける\nほかを指すと、コントローラーはゲームに戻ります",
    // 言語のボタン
    "自動",
};

const UiText kEnglish = {
    // Settings panel
    "Frame Dock", "Panel", ". ", "Perf panel shown", "Perf panel hidden",
    "Show", "Position", "To move it: point the laser at the panel and hold the trigger for 0.6 s, then move. Where you leave it is saved", "Size", "Opacity",
    "Autostart", "Autostart changes apply from the next SteamVR start",
    "The autostart unit isn't installed, so it can't be switched (see Install in the README)",
    "Switching autostart…", "Couldn't switch autostart (see the log)",
    "On", "Off",
    "Bottom L", "Bottom C", "Bottom R", "Top L", "Top R",
    "Reset placement", "Quit app", "Press again to quit",
    "Press again to reset",
    "Press again: the whole placement (attach point, position and facing, size, opacity, wrist, clock) goes back to the defaults",
    "Again",
    "Press again: the performance panel and the dock go away. To get them back, turn Show on in the Frame Dock tab of the SteamVR dashboard (or start Frame Dock again)",
    "Hide on the wrist", "Turn your wrist so the panel tilts away: it starts to fade at %.0f° and is gone at %.0f°",
    "Only used while the panel is on a wrist",
    "Changes apply at once and are saved",
    // Attachment, wrist and clock
    "Attach to", "Head", "L hand", "R hand", "Default wrist position",
    "Fade on tilt", "Fade angle", "%.0f–%.0f°",
    "Clock", "12h", "24h", "Local time",
    // Dashboard pages and the layout editor
    "Layout", "Placement",
    "Layout", "Drag a widget to move it, press it to change its settings", "New column", "Drop here to remove",
    "Widgets", "Network",
    "Press a widget on the left to change it", "No settings", "Remove",
    "Saved", "Changed meanwhile, try again", "Couldn't save the layout",
    "Compact",
    "Rows", "One row per name", "Hide system",
    // The Profiles page
    "Profiles", "Profiles", "%zu / %zu", "No profiles yet. New profile saves the layout and placement you have now",
    "Active", "unsaved changes",
    "Use", "Save", "Rename", "Delete", "New profile", "Reset layout",
    "Press again: the layout's and placement's changes are lost", "Press again: the layout's changes are lost",
    "Press again: the placement's changes are lost", "Press again to delete", "Press again to save",
    "The layout and placement you have now aren't saved in a profile",
    "A profile holds the layout and the placement (attach point, position and facing, size, opacity, wrist, clock).\n"
    "\"Layout only\" ones leave the placement as it is",
    "Profile name",
    "layout only",
    "Profiles",
    "No profiles yet",
    "Can't be read (left as they are): ",
    "There are unsaved changes. Press again to switch",
    // The Color page
    "Color",
    // The Dock page
    "Dock",
    "In-game use",
    "Use",
    "Press again: the dock won't open in games. To get it back, turn it on in the Dock page of the Frame Dock tab on the SteamVR dashboard",
    "Pointing hand",
    "Auto",
    "Auto: the hand without the performance panel (either hand when it's on your head)\nThe laser comes on after pointing for 0.25 s. While it is on, the game gets none of that controller's input, not just the trigger",
    "Dock panel",
    "Size",
    "Collapse after",
    "%d s",
    "Never",
    "Default place",
    "Place",
    "The dock panel rides where the performance panel does (head or wrist) and keeps its own place. To move it, hold the trigger on its top bar",
    "How to use",
    "Point: after 0.25 s the performance panel gets a border, and the trigger presses it\nPress: opens the dock, press again to collapse it\nMove: the performance panel, hold the trigger 0.6 s; the dock panel, hold its top bar\nPoint elsewhere and the game has the controller back",
    // The language button
    "Auto",
};

const UiText kKorean = {
    // Settings panel
    "Frame Dock", "패널", ". ", "성능 패널 표시 중", "성능 패널 숨김",
    "표시", "위치", "옮기기: 패널을 레이저로 가리키고 트리거를 0.6초 누른 채 움직여요. 옮긴 자리는 저장돼요", "크기", "불투명도",
    "자동 시작", "자동 시작 변경은 다음 SteamVR 시작부터 적용돼요",
    "자동 시작 유닛이 설치되지 않아서 바꿀 수 없어요(README의 \"설치\" 참고)",
    "자동 시작을 바꾸는 중…", "자동 시작을 바꾸지 못했어요(로그를 확인해 주세요)",
    "켬", "끔",
    "왼쪽 아래", "가운데 아래", "오른쪽 아래", "왼쪽 위", "오른쪽 위",
    "배치 전부 기본값으로", "앱 종료", "한 번 더 누르면 종료",
    "한 번 더 누르면 기본값으로",
    "한 번 더 누르면 배치(붙는 곳, 위치·방향, 크기, 불투명도, 손목, 시계)가 전부 기본값으로 돌아가요",
    "한 번 더",
    "한 번 더 누르면 성능 패널과 도크가 사라져요. 다시 켜려면 SteamVR 대시보드의 Frame Dock 탭에서 표시를 켜세요(Frame Dock을 다시 실행해도 돼요)",
    "손목에서 숨기기", "손목을 돌려 패널이 비스듬해지면 %.0f°부터 흐려지고 %.0f°에서 사라져요",
    "패널이 손목에 붙어 있을 때만 써요",
    "변경은 바로 적용되고 저장돼요",
    // Attachment, wrist and clock
    "붙는 곳", "머리", "왼손", "오른손", "손목 기본 위치",
    "기울이면 숨김", "숨김 각도", "%.0f–%.0f°",
    "시계", "12h", "24h", "현재 시각",
    // Dashboard pages and the layout editor
    "레이아웃", "배치",
    "레이아웃", "끌어서 옮기고, 누르면 설정을 바꿀 수 있어요", "새 열", "여기에 놓으면 제거",
    "위젯", "네트워크",
    "왼쪽 위젯을 누르면 바꿀 수 있어요", "설정 없음", "제거",
    "저장했어요", "그사이 바뀌었어요. 다시 해 주세요", "레이아웃을 저장하지 못했어요",
    "작게 표시",
    "줄 수", "같은 이름은 한 줄로", "시스템 숨기기",
    // The Profiles page
    "프로필", "프로필", "%zu / %zu", "아직 없어요. \"새로 만들기\"로 지금 레이아웃과 배치를 저장해요",
    "사용 중", "저장하지 않은 변경",
    "사용", "덮어쓰기", "이름 바꾸기", "삭제", "새로 만들기", "레이아웃만 기본값으로",
    "한 번 더 누르세요: 레이아웃·배치 변경이 사라져요", "한 번 더 누르세요: 레이아웃 변경이 사라져요",
    "한 번 더 누르세요: 배치 변경이 사라져요", "한 번 더 누르면 삭제", "한 번 더 누르면 덮어쓰기",
    "지금 레이아웃과 배치는 어느 프로필에도 저장되지 않았어요",
    "프로필에는 레이아웃과 배치(붙는 곳, 위치·방향, 크기, 불투명도, 손목, 시계)가 들어가요.\n\"레이아웃만\" 프로필은 배치를 바꾸지 않아요",
    "프로필 이름",
    "레이아웃만",
    "프로필",
    "저장된 프로필이 없어요",
    "읽지 못한 파일(그대로 둬요): ",
    "저장하지 않은 변경이 있어요. 한 번 더 누르면 바꿔요",
    // The Color page
    "색 보정",
    // The Dock page
    "도크",
    "게임 중 조작",
    "사용",
    "한 번 더 누르면 게임 중에 도크를 열 수 없어요. 다시 켜려면 SteamVR 대시보드의 Frame Dock 탭 → 도크에서 켜세요",
    "가리키는 손",
    "자동",
    "자동: 성능 패널이 없는 손(머리에 있으면 양손)\n0.25초 가리키고 있으면 레이저가 켜져요. 레이저가 켜진 동안에는 그 컨트롤러의 입력이 트리거뿐 아니라 모두 게임에 가지 않아요",
    "도크 패널",
    "크기",
    "자동 접기",
    "%d초",
    "안 함",
    "기본 위치로",
    "위치",
    "도크 패널은 성능 패널과 같은 곳(머리·손목)에 붙고 위치는 따로 기억해요. 옮기려면 위쪽 손잡이를 누르고 있으세요",
    "사용법",
    "가리키기: 0.25초 가리키면 성능 패널에 테두리가 생기고 트리거로 누를 수 있어요\n누르기: 도크가 열려요. 한 번 더 누르면 접혀요\n옮기기: 성능 패널은 트리거를 0.6초 누르고 있기, 도크 패널은 위쪽 손잡이를 누르고 있기\n다른 곳을 가리키면 컨트롤러가 다시 게임으로 가요",
    // The language button
    "자동",
};

}  // namespace

const UiText& uiText(Language language) {
    switch (language) {
        case Language::En: return kEnglish;
        case Language::Ko: return kKorean;
        case Language::Ja: break;
    }
    return kJapanese;
}

std::string profileStatusText(Language language, const std::string& what, const std::string& name,
                              const std::string& error) {
    if (language == Language::Ko) {
        // error: a short Korean reason (ProfileStore's, or the caller's); the OS's detail only goes to the log
        const std::string why = error.empty() ? std::string() : ": " + error;
        if (what == "saved-as") return "저장했어요: " + name;
        if (what == "new-failed") return "새 프로필을 저장하지 못했어요" + why;
        if (what == "using") return "사용 중: " + name;
        if (what == "use-failed") return "'" + name + "'을(를) 사용하지 못했어요" + why;
        if (what == "saved-to") return "덮어썼어요: " + name;
        if (what == "save-failed") return "'" + name + "'을(를) 저장하지 못했어요" + why;
        if (what == "keyboard-failed") return "키보드를 열지 못했어요" + why;
        if (what == "deleted") return "삭제했어요: " + name;
        if (what == "delete-failed") return "'" + name + "'을(를) 삭제하지 못했어요" + why;
        if (what == "reset") return "레이아웃을 기본값으로 되돌렸어요";
        if (what == "reset-failed") return "기본값으로 되돌리지 못했어요" + why;
        if (what == "renamed") return "이름을 바꿨어요: " + name;
        if (what == "rename-failed") return "이름을 바꾸지 못했어요" + why;
        return "그 프로필은 더 이상 없어요";
    }
    // error: a short reason in the same language (ProfileStore's, or the caller's); the OS's detail only goes to the
    // log
    const bool en = language == Language::En;
    const std::string why = error.empty() ? std::string() : ": " + error;
    if (what == "saved-as") return en ? "Saved as " + name : name + " として保存しました";
    if (what == "new-failed") return (en ? "Couldn't save a new profile" : "新しいプロファイルを保存できませんでした") + why;
    if (what == "using") return en ? "Using " + name : name + " を使っています";
    if (what == "use-failed") return (en ? "Couldn't use " + name : name + " を使えませんでした") + why;
    if (what == "saved-to") return en ? "Saved to " + name : name + " に上書き保存しました";
    if (what == "save-failed") return (en ? "Couldn't save " + name : name + " を保存できませんでした") + why;
    if (what == "keyboard-failed") return (en ? "The keyboard didn't open" : "キーボードを開けませんでした") + why;
    if (what == "deleted") return en ? "Deleted " + name : name + " を削除しました";
    if (what == "delete-failed") return (en ? "Couldn't delete " + name : name + " を削除できませんでした") + why;
    if (what == "reset") return en ? "Layout reset to the default" : "レイアウトを既定に戻しました";
    if (what == "reset-failed") return (en ? "Couldn't reset" : "既定に戻せませんでした") + why;
    if (what == "renamed") return en ? "Renamed to " + name : "名前を " + name + " に変えました";
    if (what == "rename-failed") return (en ? "Couldn't rename" : "名前を変えられませんでした") + why;
    return en ? "That profile isn't there any more" : "そのプロファイルはもうありません";
}

const char* languageCode(Language language) {
    switch (language) {
        case Language::En: return "en";
        case Language::Ko: return "ko";
        case Language::Ja: break;
    }
    return "ja";
}

bool parseLanguage(const std::string& code, Language& language) {
    if (code == "ja") {
        language = Language::Ja;
        return true;
    }
    if (code == "en") {
        language = Language::En;
        return true;
    }
    if (code == "ko") {
        language = Language::Ko;
        return true;
    }
    return false;
}

std::string steamLanguageFromVdf(const std::string& text) {
    // Tokens: quoted strings (\" and \\ escaped), { and }. A key followed by { opens a section; by a string, a value
    std::vector<std::string> path;
    std::string pending;  // the last key read, waiting for its value or section
    bool haveKey = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '{') {
            path.push_back(haveKey ? lower(pending) : std::string());
            haveKey = false;
        } else if (c == '}') {
            if (!path.empty()) path.pop_back();
            haveKey = false;
        } else if (c == '"') {
            std::string token;
            for (++i; i < text.size() && text[i] != '"'; ++i) {
                if (text[i] == '\\' && i + 1 < text.size()) ++i;
                token += text[i];
            }
            if (!haveKey) {
                pending = token;
                haveKey = true;
                continue;
            }
            haveKey = false;
            const std::vector<std::string> steam = {"registry", "hkcu", "software", "valve", "steam"};
            if (path == steam && lower(pending) == "language") return token;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;  // a comment
        }
    }
    return "";
}

Language autoLanguage(const std::string& steam, const char* lcAll, const char* lcMessages, const char* lang) {
    if (!steam.empty()) {
        const std::string name = lower(steam);
        if (name == "koreana" || name == "korean") return Language::Ko;  // "koreana" is Steam's name for Korean
        if (name == "japanese") return Language::Ja;
        return Language::En;
    }
    for (const char* value : {lcAll, lcMessages, lang}) {
        if (value == nullptr || value[0] == '\0') continue;
        if (value[0] == 'k' && value[1] == 'o') return Language::Ko;
        if (value[0] == 'j' && value[1] == 'a') return Language::Ja;
        return Language::En;
    }
    return Language::En;
}

Language detectAutoLanguage(const std::string& home) {
    std::string steam;
    if (!home.empty()) {
        for (const char* file : {"/.local/share/Steam/registry.vdf", "/.steam/registry.vdf"}) {
            steam = steamLanguageFromVdf(readText(home + file));
            if (!steam.empty()) break;
        }
    }
    return autoLanguage(steam, std::getenv("LC_ALL"), std::getenv("LC_MESSAGES"), std::getenv("LANG"));
}

Language systemLanguage() {
    static const Language detected = [] {
        const char* home = std::getenv("HOME");
        return detectAutoLanguage(home != nullptr ? home : "");
    }();
    return detected;
}

void nextLanguageChoice(bool& automatic, Language& language) {
    if (automatic) {
        automatic = false;
        language = Language::Ko;
    } else if (language == Language::Ko) {
        language = Language::En;
    } else if (language == Language::En) {
        language = Language::Ja;
    } else {
        automatic = true;
        language = systemLanguage();
    }
}

const char* languageName(Language language) {
    switch (language) {
        case Language::En: return "English";
        case Language::Ja: return "日本語";
        case Language::Ko: break;
    }
    return "한국어";
}

std::string languageButtonText(bool automatic, Language language) {
    if (!automatic) return languageName(language);
    return std::string(uiText(language).languageAuto) + " · " + languageName(language);
}
