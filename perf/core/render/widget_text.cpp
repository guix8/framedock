#include "widget_text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace frame_perf_overlay {

namespace {

const WidgetText kJapanese = {
    WidgetLanguage::Ja,
    "",  // 日本語は理由の文だけで分かるので前に何も付けない
    // Titles
    "フレーム",
    "CPU",
    "GPU・メモリ",
    "電力",
    "電池",
    "温度",
    "通信",
    "その他",
    // Frame
    "計測待ち",
    "SteamVR 未接続",
    "再投影 ",
    "落ち ",
    // CPU
    "（最大コア ",
    "）",
    // GPU & RAM
    "負荷",
    "クロック ",
    "メモリ ",
    // Power
    "各ch計 ",
    // Battery
    "実際の残量の %。Steam の表示はこれより高め",
    "電池へ",
    "電池から",
    "充電中",
    "放電中",
    "満充電",
    "給電中",
    "待機",
    // Temp
    "℃ CPU",
    "℃",
    "ファン ",
    "熱で制限中 ",
    "CPU・GPU",
    "DDR",
    "電池",
    "画面",
    "排気",
    "放熱",
    // Network
    "直通",
    "Wi-Fi",
    "未接続",
    "リンク ",
    // General
    "コントローラー",
    "左手 ",
    "右手 ",
    "現在時刻",
    // The full rows and the process lists
    "CPU 上位", "メモリ上位", "  最大 ", "本来 ", "コア 1 個あたり", "使用メモリ", "起動時間", "前面のアプリ",
    "なし（SteamVR ホーム）", "Frame Dock", "hide_rules.json にエラー。前のルールを使用中",
    // Simple templates
    "シンプル A", "シンプル B", "再投影", "RAM", "正常", "半分に制限", "充電完了",
    // Controls
    "色補正", "比較", "元の画面", "サウンド", "ミュート", "パススルー", "オフ", "モノクロ", "カラー",
    "プロファイル", "使用中: ", "もう一度押すと切り替えます", "ブラウザ", "ブラウザを開く", "ブラウザを閉じる",
    "Chrome オフ", "Chrome オン", "ゲーム中の操作がオフです", "確認中…", "オン",
    "録画", "録画開始", "停止", "保存中…", "録画が止まりました", "framecorder で録画中です",
};

const WidgetText kEnglish = {
    WidgetLanguage::En,
    "N/A: ",
    // Titles
    "Frame",
    "CPU",
    "GPU & RAM",
    "Power",
    "Battery",
    "Temp",
    "Network",
    "General",
    // Frame
    "Measuring",
    "No SteamVR",
    "Reproj ",
    "Dropped ",
    // CPU
    " (max core ",
    ")",
    // GPU & RAM
    "load",
    "Clock ",
    "RAM ",
    // Power
    "Rails ",
    // Battery
    "Real gauge %. Steam shows a higher number",
    "Into battery",
    "From battery",
    "Charging",
    "On battery",
    "Full",
    "Plugged",
    "Idle",
    // Temp
    "°C CPU",
    "°C",
    "Fan ",
    "Thermal limit ",
    "CPU+GPU",
    "DDR",
    "Battery",
    "Display",
    "Exhaust",
    "Heatsink",
    // Network
    "Dongle",
    "Wi-Fi",
    "Not connected",
    "Link ",
    // General
    "Controllers",
    "L ",
    "R ",
    "Time",
    // The full rows and the process lists
    "CPU items", "RAM items", " ms  ·  max ", "capped from ", "CPU % of one core", "RSS", "Uptime", "App",
    "none (SteamVR home)", "Frame Dock", "hide_rules.json has an error, using the last rules",
    // Simple templates
    "Simple A", "Simple B", "Reproj", "RAM", "OK", "Half rate", "Full",
    // Controls
    "Color", "Compare", "Original", "Sound", "Mute", "Passthrough", "Off", "Mono", "Color",
    "Profiles", "In use: ", "Press again to switch", "Browser", "Open browser", "Close browser",
    "Chrome off", "Chrome on", "In-game controls are off", "Checking…", "On",
    "Record", "Start recording", "Stop", "Saving…", "Recording stopped", "framecorder is already recording",
};

const WidgetText kKorean = {
    WidgetLanguage::Ko,
    "",  // like Japanese: the reason reads on its own
    // Titles
    "프레임",
    "CPU",
    "GPU·메모리",
    "전력",
    "배터리",
    "온도",
    "네트워크",
    "기타",
    // Frame
    "측정 대기",
    "SteamVR 미연결",
    "재투영 ",
    "드롭 ",
    // CPU
    " (최대 코어 ",
    ")",
    // GPU & RAM
    "부하",
    "클럭 ",
    "메모리 ",
    // Power
    "레일 합 ",
    // Battery
    "실제 잔량 %. Steam 표시는 이보다 높음",
    "배터리로",
    "배터리에서",
    "충전 중",
    "배터리 사용 중",
    "완충",
    "전원 연결",
    "대기",
    // Temp
    "°C CPU",
    "°C",
    "팬 ",
    "발열 제한 ",
    "CPU·GPU",
    "DDR",
    "배터리",
    "화면",
    "배기",
    "방열판",
    // Network
    "동글",
    "Wi-Fi",
    "미연결",
    "링크 ",
    // General
    "컨트롤러",
    "왼손 ",
    "오른손 ",
    "현재 시각",
    // The full rows and the process lists
    "CPU 상위", "메모리 상위", "  최대 ", "원래 ", "코어 1개 기준", "사용 중 메모리", "켜진 시간", "앞의 앱",
    "없음 (SteamVR 홈)", "Frame Dock", "hide_rules.json 오류. 이전 규칙 사용 중",
    // Simple templates
    "간단 A", "간단 B", "재투영", "RAM", "정상", "절반 제한", "충전 완료",
    // Controls
    "색 보정", "비교", "원본", "소리", "음소거", "패스스루", "끔", "흑백", "컬러",
    "프로필", "사용 중: ", "한 번 더 누르면 바꿔요", "브라우저", "브라우저 열기", "브라우저 닫기",
    "Chrome 꺼짐", "Chrome 켜짐", "게임 중 조작이 꺼져 있어요", "확인 중…", "켬",
    "녹화", "녹화 시작", "정지", "저장 중…", "녹화가 멈췄어요", "framecorder가 이미 녹화 중이에요",
};

}  // namespace

const WidgetText& widgetText(WidgetLanguage language) {
    switch (language) {
        case WidgetLanguage::En: return kEnglish;
        case WidgetLanguage::Ko: return kKorean;
        case WidgetLanguage::Ja: break;
    }
    return kJapanese;
}

const char* reasonLabel(const WidgetText& t, Reason reason) {
    if (t.language == WidgetLanguage::En) return reasonText(reason);
    if (t.language == WidgetLanguage::Ko) {
        switch (reason) {
            case Reason::None: return "정상";
            case Reason::NotSampledYet: return "읽기 대기";
            case Reason::SensorMissing: return "센서 없음";
            case Reason::ReadFailed: return "읽기 실패";
            case Reason::NoSteamVr: return "SteamVR 미연결";
            case Reason::NoFrames: return "프레임 대기";
            case Reason::NotConnected: return "미연결";
            case Reason::Paused: return "일시 정지 (보는 사람 없음)";
            case Reason::InvalidReading: return "의미 없는 값";
        }
        return "알 수 없음";
    }
    switch (reason) {
        case Reason::None: return "正常";
        case Reason::NotSampledYet: return "読み取り待ち";
        case Reason::SensorMissing: return "センサーなし";
        case Reason::ReadFailed: return "読み取り失敗";
        case Reason::NoSteamVr: return "SteamVR 未接続";
        case Reason::NoFrames: return "フレーム待ち";
        case Reason::NotConnected: return "未接続";
        case Reason::Paused: return "一時停止中（だれも見ていない）";
        case Reason::InvalidReading: return "意味のない値";
    }
    return "不明";
}

std::string simpleThrottleText(const WidgetText& t, int throttled) {
    if (throttled <= 0) return t.simpleOk;
    if (throttled == 1) return t.simpleHalf;
    const std::string fraction = "1/" + std::to_string(throttled + 1);
    if (t.language == WidgetLanguage::En) return fraction + " rate";
    if (t.language == WidgetLanguage::Ko) return fraction + " 제한";
    return fraction + "に制限";
}

std::string recordElapsedText(int seconds) {
    const int s = std::max(0, seconds);
    char buffer[32];
    if (s >= 3600) {
        std::snprintf(buffer, sizeof(buffer), "● %d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    } else {
        std::snprintf(buffer, sizeof(buffer), "● %d:%02d", s / 60, s % 60);
    }
    return buffer;
}

std::string remainText(const WidgetText& t, double minutes) {
    if (!(minutes > 0)) return "";
    const long total = std::lround(minutes);
    const std::string h = std::to_string(total / 60);
    const std::string m = std::to_string(total % 60);
    if (t.language == WidgetLanguage::En) {
        if (total < 60) return "~" + m + " min";
        return "~" + h + " h" + (total % 60 != 0 ? " " + m + " min" : "");
    }
    if (t.language == WidgetLanguage::Ko) {
        if (total < 60) return "약 " + m + "분";
        return "약 " + h + "시간" + (total % 60 != 0 ? " " + m + "분" : "");
    }
    if (total < 60) return "約" + m + "分";
    return "約" + h + "時間" + (total % 60 != 0 ? m + "分" : "");
}

std::string remainTextShort(const WidgetText& t, double minutes) {
    if (!(minutes >= 60)) return remainText(t, minutes);
    // Hours to the half, whole hours from 10 h up ("약 16.5시간" doesn't fit Simple B at 100 %)
    const long halves = minutes >= 570 ? std::lround(minutes / 60) * 2 : std::lround(minutes / 30);
    const std::string h = std::to_string(halves / 2) + (halves % 2 != 0 ? ".5" : "");
    if (t.language == WidgetLanguage::En) return "~" + h + " h";
    if (t.language == WidgetLanguage::Ko) return "약 " + h + "시간";
    return "約" + h + "時間";
}

std::string uptimeText(const WidgetText& t, double seconds) {
    if (std::isnan(seconds) || seconds < 0) return "--";
    const long minutes = static_cast<long>(seconds) / 60;
    const std::string d = std::to_string(minutes / 1440);
    const std::string h = std::to_string(minutes / 60 % 24);
    const std::string hours = std::to_string(minutes / 60);
    const std::string m = std::to_string(minutes % 60);
    if (t.language == WidgetLanguage::En) {
        if (minutes >= 1440) return d + "d " + h + "h";
        if (minutes >= 60) return hours + "h " + m + "m";
        return m + "m";
    }
    if (t.language == WidgetLanguage::Ko) {
        if (minutes >= 1440) return d + "일 " + h + "시간";
        if (minutes >= 60) return hours + "시간 " + m + "분";
        return m + "분";
    }
    if (minutes >= 1440) return d + "日" + h + "時間";
    if (minutes >= 60) return hours + "時間" + m + "分";
    return m + "分";
}

std::string throttleBadgeText(const WidgetText& t, double hz, int throttled) {
    char hzText[16];
    std::snprintf(hzText, sizeof(hzText), "%.0f", hz);
    const std::string fraction = "1/" + std::to_string(throttled + 1);
    if (t.language == WidgetLanguage::En) return "Throttled to " + fraction + " of " + hzText + "Hz (reproj)";
    if (t.language == WidgetLanguage::Ko) {
        return std::string(hzText) + "Hz의 " + (throttled == 1 ? std::string("절반으로") : fraction + "로") + " 제한 (재투영)";
    }
    return std::string(hzText) + "Hz の" + (throttled == 1 ? std::string("半分") : fraction) + "に制限（再投影）";
}

}  // namespace frame_perf_overlay
