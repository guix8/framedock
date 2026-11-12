#include "color_i18n.h"

namespace framecolor {

namespace {

Translations createEnglish() {
    Translations t{};

    // Language Metadata
    t.code = "en";

    // Preset Buttons
    t.builtinPresets = {"Standard", "Vivid", "Night"};
    t.emptySlotSuffix = " (Empty)";
    t.save = "Save";
    t.selectSlot = "Select Slot";
    t.reset = "Reset";
    t.resetConfirm = "Confirm?";

    // Adjustment Controls & Sliders
    t.panelBrightness = "Panel Brightness";
    t.adjustmentChannel = "Adjustment Channel";
    t.channelNames = {"All", "Red", "Green", "Blue"};
    t.brightness = "Brightness";
    t.saturation = "Saturation";
    t.contrast = "Contrast";
    t.gamma = "Gamma";
    t.sharpness = "Sharpness";
    t.compare = "Hold to Compare";
    t.compareHeld = "Original";
    t.autoCompare = "Auto Compare";
    t.autoCompareShowingOriginal = "Auto: Original";
    t.autoCompareShowingCorrected = "Auto: Corrected";
    t.autoCompareOn = "Auto compare: switches every 3 s for 60 s. Hide the dashboard to watch (not with this panel's X)";
    t.autoCompareOff = "Auto compare off";
    t.autoCompareEnded = "Auto compare finished";
    t.sharpenCenter = "Center only";
    t.sharpenGaze = "Follow gaze";
    t.gazeFit = "Fit";
    t.gazeFitRunning = "%d/%d";
    t.gazeFitHint[0] = "Look at the green dot only.";
    t.gazeFitHint[1] = "The tint centred on it means the fit in use is right.";
    t.gazeFitReviewHint[0] = "New fit in use. The tint goes in %d s.";
    t.gazeFitReviewHint[1] = "Look around to check it. Fit again: the tint should centre on each dot.";
    t.gazeFitFailed = "The fit did not finish. The fit in use is unchanged.";
    t.gazeFitFailReason[0] = "No eye-tracking data came in. Try again in a moment.";
    t.gazeFitFailReason[1] = "The lens data could not be read. Try again in a moment.";
    t.gazeFitFailReason[2] = "The gaze was unsteady at one dot three times. Keep looking at the green dot.";
    t.gazeFitFailReason[3] = "The dots did not read in order: one was probably missed. Try again.";
    t.sharpenWhole = "Whole image";
    t.presets = "Presets";
    t.autoPreset = "Auto Preset";
    t.standalone = "Standalone";
    t.streaming = "PC Streaming";
    t.thisApp = "This App";
    t.autoOff = "Off";
    t.noApp = "no app";
    t.nowPrefix = "Now: ";
    t.sharpenPausedStreaming = "whole-image sharpening paused while streaming";
    t.autoApplied = "Auto preset: ";
    t.autoRestored = "Restored the settings from before the auto preset";
    t.ruleSaved = "Rule saved. It applies when the mode or app changes";
    t.layerOff = "The Vulkan correction is off, so only panel brightness works. It can only be turned on from a PC or a "
                 "terminal (framedockctl enable)";
    t.layerReload = "The new color correction applies when SteamVR restarts";

    // Status & Feedback Messages
    t.errorSaveSettings = "Failed to save settings: ";
    t.applied = "Applied";
    t.errorSaveAdvanced = "Failed to save advanced settings";
    t.errorSavePreset = "Failed to save preset";
    t.slotEmpty = "This slot is empty";
    t.presetApplied = " applied";
    t.promptSaveSlot = "Select slot P1-P3 to save";
    t.saveCanceled = "Save canceled";
    t.savedToSlotPrefix = "Saved to P";
    t.savedToSlotSuffix = "";

    return t;
}

Translations createJapanese() {
    Translations t{};

    // Language Metadata
    t.code = "ja";

    // Preset Buttons
    t.builtinPresets = {"標準", "鮮やか", "夜間"};
    t.emptySlotSuffix = "（空）";
    t.save = "保存";
    t.selectSlot = "保存先を選択";
    t.reset = "リセット";
    t.resetConfirm = "確認？";

    // Adjustment Controls & Sliders
    t.panelBrightness = "パネル輝度";
    t.adjustmentChannel = "調整チャンネル";
    t.channelNames = {"全体", "赤", "緑", "青"};
    t.brightness = "明るさ";
    t.saturation = "彩度";
    t.contrast = "コントラスト";
    t.gamma = "ガンマ";
    t.sharpness = "シャープネス";
    t.compare = "長押しで比較";
    t.compareHeld = "補正なし";
    t.autoCompare = "自動比較";
    t.autoCompareShowingOriginal = "自動：補正なし";
    t.autoCompareShowingCorrected = "自動：補正あり";
    t.autoCompareOn = "自動比較：3秒ごとに60秒間切り替えます．ダッシュボードを隠して確認してください（パネルの×は使わない）";
    t.autoCompareOff = "自動比較をオフにしました";
    t.autoCompareEnded = "自動比較が終わりました";
    t.sharpenCenter = "中央のみ";
    t.sharpenGaze = "視線追従";
    t.gazeFit = "補正";
    t.gazeFitRunning = "%d/%d";
    t.gazeFitHint[0] = "緑の点だけを見てください。";
    t.gazeFitHint[1] = "色の付いた範囲の中心が点に来れば、今の補正は合っています。";
    t.gazeFitReviewHint[0] = "新しい補正を適用しました。色は %d 秒後に消えます。";
    t.gazeFitReviewHint[1] = "見回して確かめてください。もう一度補正すると色の中心が各点に来るはずです。";
    t.gazeFitFailed = "補正できませんでした。今の補正はそのままです。";
    t.gazeFitFailReason[0] = "視線のデータが届きませんでした。少し待ってからもう一度どうぞ。";
    t.gazeFitFailReason[1] = "レンズの情報を読めませんでした。少し待ってからもう一度どうぞ。";
    t.gazeFitFailReason[2] = "ある点で視線が 3 回とも揺れました。緑の点を見続けてください。";
    t.gazeFitFailReason[3] = "点が順番どおりに読めませんでした。1 つ見誤ったようです。もう一度どうぞ。";
    t.sharpenWhole = "画面全体";
    t.presets = "プリセット";
    t.autoPreset = "自動プリセット";
    t.standalone = "単体";
    t.streaming = "PCストリーミング";
    t.thisApp = "このアプリ";
    t.autoOff = "オフ";
    t.noApp = "アプリなし";
    t.nowPrefix = "現在: ";
    t.sharpenPausedStreaming = "ストリーミング中は全体シャープ停止";
    t.autoApplied = "自動プリセット: ";
    t.autoRestored = "自動プリセット前の設定に戻しました";
    t.ruleSaved = "規則を保存しました．モードかアプリが変わると適用します";
    t.layerOff = "Vulkan の補正がオフなので，使えるのはパネル輝度だけです．オンにするのは PC かターミナルから"
                 "（framedockctl enable）";
    t.layerReload = "新しい色補正は SteamVR の再起動後に適用されます";

    // Status & Feedback Messages
    t.errorSaveSettings = "設定の保存に失敗: ";
    t.applied = "適用しました";
    t.errorSaveAdvanced = "詳細設定を保存できません";
    t.errorSavePreset = "プリセットを保存できません";
    t.slotEmpty = "このスロットは未保存です";
    t.presetApplied = "を適用しました";
    t.promptSaveSlot = "保存先 P1〜P3 を選んでください";
    t.saveCanceled = "保存をキャンセルしました";
    t.savedToSlotPrefix = "P";
    t.savedToSlotSuffix = " に保存しました";

    return t;
}

Translations createKorean() {
    Translations t{};

    // Language Metadata
    t.code = "ko";

    // Preset Buttons
    t.builtinPresets = {"표준", "비비드", "야간"};
    t.emptySlotSuffix = " (비어 있음)";
    t.save = "저장";
    t.selectSlot = "저장할 슬롯 선택";
    t.reset = "초기화";
    t.resetConfirm = "한 번 더 누르면 초기화";

    // Adjustment Controls & Sliders
    t.panelBrightness = "디스플레이 밝기";
    t.adjustmentChannel = "조정 채널";
    t.channelNames = {"전체", "빨강", "초록", "파랑"};
    t.brightness = "밝기";
    t.saturation = "채도";
    t.contrast = "대비";
    t.gamma = "감마";
    t.sharpness = "선명도";
    t.compare = "길게 눌러 비교";
    t.compareHeld = "원본";
    t.autoCompare = "자동 비교";
    t.autoCompareShowingOriginal = "자동: 원본";
    t.autoCompareShowingCorrected = "자동: 보정";
    t.autoCompareOn = "자동 비교: 60초 동안 3초마다 바뀌어요. 대시보드를 숨기고 확인하세요(패널의 X는 누르지 마세요)";
    t.autoCompareOff = "자동 비교를 껐어요";
    t.autoCompareEnded = "자동 비교가 끝났어요";
    t.sharpenCenter = "중앙만";
    t.sharpenGaze = "시선 따라";
    t.gazeFit = "보정";
    t.gazeFitRunning = "%d/%d";
    t.gazeFitHint[0] = "초록 점만 보세요.";
    t.gazeFitHint[1] = "색칠된 영역의 가운데가 점에 오면 지금 보정이 맞는 것입니다.";
    t.gazeFitReviewHint[0] = "새 보정을 적용했습니다. 색칠은 %d초 뒤 사라집니다.";
    t.gazeFitReviewHint[1] = "둘러보며 확인하세요. 다시 보정하면 색칠의 가운데가 각 점에 와야 정상입니다.";
    t.gazeFitFailed = "보정하지 못했습니다. 보정 값은 그대로입니다.";
    t.gazeFitFailReason[0] = "시선 추적 값이 들어오지 않았습니다. 잠시 뒤 다시 해 보세요.";
    t.gazeFitFailReason[1] = "렌즈 정보를 읽지 못했습니다. 잠시 뒤 다시 해 보세요.";
    t.gazeFitFailReason[2] = "한 점에서 시선이 3번 모두 흔들렸습니다. 초록 점을 계속 보세요.";
    t.gazeFitFailReason[3] = "점들이 순서대로 읽히지 않았습니다. 한 점을 잘못 본 것 같습니다. 다시 해 보세요.";
    t.sharpenWhole = "화면 전체";
    t.presets = "프리셋";
    t.autoPreset = "자동 프리셋";
    t.standalone = "독립형";
    t.streaming = "PC 스트리밍";
    t.thisApp = "이 앱";
    t.autoOff = "끔";
    t.noApp = "앱 없음";
    t.nowPrefix = "현재: ";
    t.sharpenPausedStreaming = "스트리밍 중엔 화면 전체 샤픈 쉼";
    t.autoApplied = "자동 프리셋: ";
    t.autoRestored = "자동 프리셋 전의 설정으로 되돌렸어요";
    t.ruleSaved = "규칙을 저장했어요. 모드나 앱이 바뀌면 적용돼요";
    t.layerOff = "Vulkan 보정이 꺼져 있어서 디스플레이 밝기만 바꿀 수 있어요. 켜는 건 PC나 터미널에서 해야 해요"
                 "(framedockctl enable)";
    t.layerReload = "새 색 보정은 SteamVR을 다시 시작하면 적용돼요";

    // Status & Feedback Messages
    t.errorSaveSettings = "설정을 저장하지 못했어요: ";
    t.applied = "적용했어요";
    t.errorSaveAdvanced = "고급 설정을 저장하지 못했어요";
    t.errorSavePreset = "프리셋을 저장하지 못했어요";
    t.slotEmpty = "이 슬롯은 비어 있어요";
    t.presetApplied = " 적용했어요";
    t.promptSaveSlot = "저장할 슬롯 P1~P3을 고르세요";
    t.saveCanceled = "저장을 취소했어요";
    t.savedToSlotPrefix = "P";
    t.savedToSlotSuffix = "에 저장했어요";

    return t;
}

const Translations kTranslationsEn = createEnglish();
const Translations kTranslationsJa = createJapanese();
const Translations kTranslationsKo = createKorean();

const std::vector<Language> kLanguages = {
    Language::English,
    Language::Japanese,
    Language::Korean,
};

}  // namespace

std::string Translations::formatSavedToSlot(int slotNumber) const {
    return std::string(savedToSlotPrefix) + std::to_string(slotNumber) + savedToSlotSuffix;
}

std::string Translations::formatPresetApplied(const std::string& presetName) const {
    return presetName + presetApplied;
}

const std::vector<Language>& availableLanguages() {
    return kLanguages;
}

const Translations& getTranslations(Language lang) {
    switch (lang) {
        case Language::Japanese:
            return kTranslationsJa;
        case Language::Korean:
            return kTranslationsKo;
        case Language::English:
        default:
            return kTranslationsEn;
    }
}

std::string languageCode(Language lang) {
    switch (lang) {
        case Language::Japanese:
            return "ja";
        case Language::Korean:
            return "ko";
        case Language::English:
        default:
            return "en";
    }
}

}  // namespace framecolor
