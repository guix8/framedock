// Made-up state for pictures of the dashboard tab. See dashboard_preview.h.
#include "dashboard_preview.h"

void previewColorState(framecolor::ColorModel& model, framecolor::Language language, bool busy) {
    framecolor::ColorState& ui = model.state;
    ui.language = language;
    ui.vulkanLayerActive = true;
    ui.values.sharpen = 0.35;
    ui.values.panel = 0.95;
    ui.values.saturation = {1.12, 1.12, 1.12};
    ui.values.gamma = {1.05, 1.05, 1.05};
    ui.custom[0].valid = true;
    ui.contextKnown = true;
    ui.context = {false, "steam.app.546560"};
    ui.appName = "Half-Life: Alyx";
    ui.autoRules.standalone = 1;
    ui.autoRules.apps["steam.app.546560"] = 3;
    ui.message = std::string(model.tr().autoApplied) + "P1";
    if (busy) {
        ui.channel = 1;
        ui.autoCompare = true;
        ui.autoOriginal = true;
        ui.saveMode = true;
        ui.resetConfirm = true;
        ui.sharpenWhole = true;
        ui.context = {true, ""};
        ui.appName.clear();
        ui.message = model.tr().autoCompareOn;
    }
}

ProfilesUiState previewProfiles(Language language, bool profilesPage) {
    ProfilesUiState profiles;
    const frame_perf_overlay::WidgetText& t = frame_perf_overlay::widgetText(widgetLanguage(language));
    profiles.items = {{"profile-1", t.simpleA, true},
                      {"profile-2", t.simpleB, true},
                      {"profile-3", "Standalone", false},
                      {"profile-4", "Racing", false},
                      {"profile-5", "Minimal", false}};
    profiles.activeId = "profile-4";
    if (profilesPage) {
        profiles.unsaved = true;
        profiles.status = profileStatusText(language, "saved-to", "Racing", "");
    }
    return profiles;
}

const char* const kPreviewDeviceVariants[] = {"",           "single",       "unavailable", "sync-pc-off",
                                              "sync-switching", "sync-confirm", "sync-error"};
const size_t kPreviewDeviceVariantCount = sizeof(kPreviewDeviceVariants) / sizeof(kPreviewDeviceVariants[0]);

bool previewDeviceState(DeviceModel& model, const std::string& variant) {
    model = DeviceModel();
    // Sound: the headset's speakers and a wireless headset
    AudioState audio;
    audio.known = true;
    audio.outputs = {{"alsa_output.platform-sound.HiFi__Speaker__sink", "", true},
                     {"bluez_output.Wireless_Headset.1", "Wireless Headset", false}};
    audio.defaultId = audio.outputs[1].id;
    audio.volume = 45;
    model.setAudio(audio);
    model.passthrough = {PassthroughMode::Color, true, true};

    // Frame Sync: five accounts on the Frame, the PC knows them all and runs Steam as another one
    SyncState& sync = model.sync;
    sync.frameAccounts = {{"76561198000000001", "alex", "Alex"},
                          {"76561198000000002", "sam", "Sam"},
                          {"76561198000000003", "guest", "Player 1"},
                          {"76561198000000004", "robin", "Robin"},
                          {"76561198000000005", "kai", "Kai"}};
    sync.frameId = "76561198000000001";
    sync.pcAccounts = std::vector<framesync::PcAccount> {{"76561198000000001", "Alex", "alex", true},
                                                         {"76561198000000002", "Sam", "sam", true},
                                                         {"76561198000000003", "Player 1", "guest", false},
                                                         {"76561198000000004", "Robin", "robin", true},
                                                         {"76561198000000005", "Kai", "kai", true}};
    framesync::PcStatus status;
    status.steamRunning = true;
    status.steamId = "76561198000000002";
    status.hasUsers = true;
    model.applyRefresh(status, framesync::Reach::On, framesync::ErrorKind::None, "");

    if (variant.empty()) return true;
    if (variant == "single") {
        audio.outputs.resize(1);
        audio.defaultId = audio.outputs[0].id;
        audio.volume = 70;
        audio.muted = true;
        model.setAudio(audio);
        model.passthrough = {PassthroughMode::Off, true, true};
        status.steamId = sync.frameId;
        status.streaming = true;
        status.gameAppId = "546560";
        model.applyRefresh(status, framesync::Reach::On, framesync::ErrorKind::None, "");
        return true;
    }
    if (variant == "unavailable") {
        model.setAudio(AudioState());
        model.passthrough = {PassthroughMode::Off, false, false};
        model.applyRefresh(std::nullopt, framesync::Reach::Unknown, framesync::ErrorKind::None, "");
        return true;
    }
    if (variant == "sync-pc-off") {
        model.applyRefresh(std::nullopt, framesync::Reach::Off, framesync::ErrorKind::None, "");
        return true;
    }
    if (variant == "sync-switching" || variant == "sync-confirm") {
        model.applyRefresh(std::nullopt, framesync::Reach::Off, framesync::ErrorKind::None, "");
        model.startSync(sync.frameId, false);
        model.syncProgress(framesync::Step::Waking);
        model.syncProgress(framesync::Step::WaitingLogin);
        model.syncProgress(framesync::Step::Switching);
        if (variant == "sync-confirm") model.askGame(0);
        model.takeActions();
        return true;
    }
    if (variant == "sync-error") {
        model.applyRefresh(std::nullopt, framesync::Reach::On, framesync::ErrorKind::Auth, tr(Tx::SyncAuth));
        return true;
    }
    return false;
}
