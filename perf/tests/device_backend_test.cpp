// The 기기 page's backends without a sound server, a bus, SteamVR or a PC: which sinks are outputs (recorded
// `pactl list sinks` data), the volume in percent and the drag's throttle, Camera Switch's availability from
// GetNameOwner / NameOwnerChanged, the Frame's Steam accounts (a loginusers.vdf fixture, checked against
// frame_steam.py), Frame Sync's settings, cache file and status reading, and DeviceBackend on fake services
// (visibility, actions, the poll's timing, passthrough's composition, the game question's timeout).
#include "audio_sinks.h"
#include "camera_switch.h"
#include "device_backend.h"
#include "device_model.h"
#include "frame_steam.h"
#include "frame_sync_agent.h"

#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#ifndef FIXTURES
#error "FIXTURES (tests/fixtures/device) must be defined"
#endif

namespace {

int failures = 0;

#define CHECK(expr)                                             \
    do {                                                        \
        if (!(expr)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
            ++failures;                                         \
        }                                                       \
    } while (false)

const double kInf = std::numeric_limits<double>::infinity();

std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void writeAll(const std::string& path, const std::string& text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

/** A fresh folder under the build tree's temp (removed by the caller). */
std::string tempDir(const char* name) {
    const std::string dir = std::filesystem::temp_directory_path().string() + "/framedock-device-test-" +
                            std::to_string(::getpid()) + "-" + name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// ---- Sinks: `pactl list sinks` as recorded ----

std::vector<audio::SinkInfo> parsePactl(const std::string& text) {
    std::vector<audio::SinkInfo> sinks;
    std::istringstream in(text);
    std::string line;
    bool props = false;
    while (std::getline(in, line)) {
        if (line.rfind("Sink #", 0) == 0) {
            sinks.emplace_back();
            props = false;
            continue;
        }
        if (sinks.empty()) continue;
        audio::SinkInfo& s = sinks.back();
        const size_t start = line.find_first_not_of('\t');
        if (start == std::string::npos) continue;
        const std::string body = line.substr(start);
        if (start == 1) props = body == "Properties:";
        if (start == 1 && body.rfind("Name: ", 0) == 0) s.name = body.substr(6);
        if (start == 1 && body.rfind("Description: ", 0) == 0) s.description = body.substr(13);
        if (start == 1 && body.rfind("Mute: ", 0) == 0) s.muted = body.substr(6) == "yes";
        if (start == 1 && body.rfind("Volume: ", 0) == 0) {
            const size_t colon = body.find(": ", 8);  // "front-left: 37651 / ..."
            s.volume = static_cast<uint32_t>(std::strtoul(body.c_str() + colon + 2, nullptr, 10));
        }
        if (start == 2 && props) {
            const size_t eq = body.find(" = \"");
            if (eq != std::string::npos) s.props[body.substr(0, eq)] = body.substr(eq + 4, body.size() - eq - 5);
        }
    }
    return sinks;
}

const audio::SinkInfo* find(const std::vector<audio::SinkInfo>& sinks, const std::string& name) {
    for (const auto& s : sinks) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

const char* kLoopback = "alsa_loopback_device.stereo.alsa_output.platform-sound.HiFi__Speaker__sink";
const char* kRaw = "alsa_output.platform-sound.HiFi__Speaker__sink";
const char* kWireless = "alsa_output.usb-Generic_Wireless_Headset_0000000000000000-00.analog-stereo";
const char* kBluez = "bluez_output.00_11_22_33_44_55.1";

void testSinks() {
    const auto sinks = parsePactl(readAll(std::string(FIXTURES) + "/pactl-sinks.txt"));
    CHECK(sinks.size() == 7);
    using audio::SinkKind;
    const auto kind = [&](const char* name) { return audio::classify(*find(sinks, name), true); };
    CHECK(find(sinks, kLoopback) && kind(kLoopback) == SinkKind::Speaker);
    CHECK(find(sinks, kWireless) && kind(kWireless) == SinkKind::Output);
    CHECK(kind(kRaw) == SinkKind::Hidden);  // the loopback stands for it
    CHECK(audio::classify(*find(sinks, kRaw), false) == SinkKind::Speaker);
    CHECK(kind("filter-chain-sink") == SinkKind::Hidden);
    CHECK(kind("echo_cancel_sink") == SinkKind::Hidden);
    CHECK(kind("null-sink-test") == SinkKind::Hidden);
    CHECK(kind(kBluez) == SinkKind::Output);

    // The page's state: the server's order, the speaker marked, the default's volume
    AudioState state = audio::stateFrom(sinks, kWireless);
    CHECK(state.known);
    CHECK(state.outputs.size() == 3);
    CHECK(state.outputs[0].id == kLoopback && state.outputs[0].builtin);
    CHECK(outputLabel(state.outputs[0]) == "헤드셋 스피커");
    CHECK(state.outputs[1].id == kWireless && !state.outputs[1].builtin &&
          state.outputs[1].name == "Wireless Headset Analog Stereo");
    CHECK(state.outputs[2].id == kBluez && state.outputs[2].name == "Test Earbuds");
    CHECK(state.defaultId == kWireless && state.volume == 37 && !state.muted);  // 24373 / 65536
    state = audio::stateFrom(sinks, kRaw);  // set as default by something that sees the raw sink
    CHECK(state.defaultId == kLoopback && state.volume == 57);
    state = audio::stateFrom(sinks, kBluez);
    CHECK(state.defaultId == kBluez && state.volume == 50 && state.muted);
    state = audio::stateFrom(sinks, "filter-chain-sink");  // a hidden default: nothing selected, its level shown
    CHECK(state.defaultId.empty() && state.volume == 100);
    state = audio::stateFrom({}, "");
    CHECK(state.known && state.outputs.empty() && state.defaultId.empty());

    // Percent as pactl shows it
    CHECK(audio::percentOf(37651) == 57 && audio::percentOf(0) == 0 && audio::percentOf(0x10000) == 100);
    CHECK(audio::percentOf(0x18000) == 100);  // above 100 % shows as 100
    CHECK(audio::volumeOf(100) == 0x10000 && audio::volumeOf(0) == 0 && audio::volumeOf(150) == 0x10000);
    for (int p = 0; p <= 100; ++p) CHECK(audio::percentOf(audio::volumeOf(p)) == p);
}

void testSinksBeta() {
    // The 0.5.7 beta: the USB headset behind its own loopback (alsa.loopback, node.virtual false), the raw one hidden
    auto sinks = parsePactl(readAll(std::string(FIXTURES) + "/pactl-sinks-beta.txt"));
    CHECK(sinks.size() == 2);
    const std::string usb =
        "alsa_loopback_device.alsa_output.usb-Generic_Wireless_Headset_0000000000000000-00.analog-stereo";
    CHECK(find(sinks, usb) && audio::classify(*find(sinks, usb), true) == audio::SinkKind::Output);
    AudioState state = audio::stateFrom(sinks, usb);
    CHECK(state.outputs.size() == 2);
    CHECK(state.outputs[0].id == kLoopback && state.outputs[0].builtin);
    CHECK(state.outputs[1].id == usb && !state.outputs[1].builtin &&
          state.outputs[1].name == "Wireless Headset Analog Stereo");
    CHECK(state.defaultId == usb);
    CHECK(audio::deviceKey(usb) == "usb-Generic_Wireless_Headset_0000000000000000-00");
    // A client that can see the raw sinks too: one row per card, and a raw default stands for its loopback
    audio::SinkInfo raw = *find(sinks, usb);
    raw.name = kWireless;
    raw.props.erase("alsa.loopback");
    sinks.push_back(raw);
    state = audio::stateFrom(sinks, kWireless);
    CHECK(state.outputs.size() == 2);
    CHECK(state.defaultId == usb);
    // A second sink on the same card without a loopback (a headset's chat channel): its own row, its own default
    audio::SinkInfo chat = raw;
    chat.name = "alsa_output.usb-Generic_Wireless_Headset_0000000000000000-00.mono-chat";
    sinks.push_back(chat);
    state = audio::stateFrom(sinks, chat.name);
    CHECK(state.outputs.size() == 3);
    CHECK(state.outputs[2].id == chat.name && state.defaultId == chat.name);
    // Its microphone: the loopback over the card's input
    const std::vector<std::string> sources = {
        "alsa_loopback_device.alsa_input.platform-sound.HiFi__Mic__source",
        "alsa_loopback_device.alsa_output.usb-Generic_Wireless_Headset_0000000000000000-00.analog-stereo.monitor",
        "alsa_loopback_device.alsa_input.usb-Generic_Wireless_Headset_0000000000000000-00.mono-fallback",
    };
    CHECK(audio::micFor(usb, sources, kLoopback) ==
          "alsa_loopback_device.alsa_input.usb-Generic_Wireless_Headset_0000000000000000-00.mono-fallback");
}

void testMicFor() {
    // The sources on this Frame (pactl list sources short, 2026-10-30) and a Bluetooth headset's
    const std::vector<std::string> sources = {
        "alsa_loopback_device.alsa_input.platform-sound.HiFi__Mic__source",
        "alsa_input.platform-sound.HiFi__Mic__source",  // the raw one, if a client could see it
        "alsa_output.usb-Generic_Wireless_Headset_0000000000000000-00.analog-stereo",  // not a source: no input
        "alsa_input.usb-Generic_Wireless_Headset_0000000000000000-00.mono-fallback",
        "bluez_input.00_11_22_33_44_55.0",
        "echo_cancel_source",
    };
    CHECK(audio::deviceKey(kLoopback) == "platform-sound" && audio::deviceKey(kRaw) == "platform-sound");
    CHECK(audio::deviceKey(kWireless) == "usb-Generic_Wireless_Headset_0000000000000000-00");
    CHECK(audio::deviceKey(kBluez) == "00_11_22_33_44_55" && audio::deviceKey("echo_cancel_source").empty());
    // Each output's own microphone (SteamOS's loopback over the headset's first)
    CHECK(audio::micFor(kWireless, sources, kLoopback) ==
          "alsa_input.usb-Generic_Wireless_Headset_0000000000000000-00.mono-fallback");
    CHECK(audio::micFor(kLoopback, sources, kLoopback) ==
          "alsa_loopback_device.alsa_input.platform-sound.HiFi__Mic__source");
    // Bluetooth: the headset's on purpose (a Bluetooth microphone switches the earphones to the call profile)
    CHECK(audio::micFor(kBluez, sources, kLoopback) ==
          "alsa_loopback_device.alsa_input.platform-sound.HiFi__Mic__source");
    // An output without a microphone: the headset's; no headset speaker known either: leave it
    const char* speakerOnly = "bluez_output.66_77_88_99_AA_BB.1";
    CHECK(audio::micFor(speakerOnly, sources, kLoopback) ==
          "alsa_loopback_device.alsa_input.platform-sound.HiFi__Mic__source");
    CHECK(audio::micFor(speakerOnly, sources, "").empty());
    // Monitors never count
    CHECK(audio::micFor(kWireless, {std::string(kWireless) + ".monitor"}, "").empty());
}

void testThrottle() {
    audio::VolumeThrottle t;
    CHECK(t.nextDue() == kInf);
    CHECK(t.push(10, false, 100.0) == 10);  // the first goes at once
    CHECK(t.push(11, false, 100.01) == -1);
    CHECK(t.push(12, false, 100.02) == -1);
    CHECK(t.nextDue() == 100.0 + audio::VolumeThrottle::kMinIntervalSec);
    CHECK(t.sendDue(100.05) == -1);
    CHECK(t.sendDue(100.07) == 12);  // only the last waiting value
    CHECK(t.nextDue() == kInf && t.sendDue(101) == -1);
    CHECK(t.push(20, false, 100.08) == -1);
    CHECK(t.push(21, true, 100.09) == 21);  // the release goes at once and drops the waiting one
    CHECK(t.nextDue() == kInf);
    CHECK(t.push(22, false, 100.10) == -1);  // still within the interval of the release
    // At most 15 a second over a long drag
    audio::VolumeThrottle u;
    int sent = 0;
    for (int i = 0; i < 600; ++i) {  // 6 s of 100 Hz moves
        const double now = 10 + i * 0.01;
        if (u.push(i % 101, false, now) >= 0) ++sent;
        if (u.sendDue(now) >= 0) ++sent;
    }
    CHECK(sent <= 6 * 15 + 1 && sent >= 6 * 14);
}

void testNameOwner() {
    NameOwnerTracker t;
    bool available = true;
    CHECK(!t.take(available));
    t.answer(false);  // NameHasNoOwner
    CHECK(t.take(available) && !available);
    CHECK(!t.take(available));
    t.changed(":1.42");  // Camera Switch started
    CHECK(t.take(available) && available);
    t.changed(":1.43");  // a new owner: still running, no change
    CHECK(!t.take(available));
    t.changed("");  // stopped
    CHECK(t.take(available) && !available);
    NameOwnerTracker u;
    u.changed(":1.5");  // the signal before the answer, then the answer (newer) agrees
    u.answer(true);
    CHECK(u.take(available) && available && !u.take(available));
}

// ---- The Frame's Steam ----

void testFrameSteam() {
    const std::string steamDir = std::string(FIXTURES) + "/steam";
    const auto users = framesync::parseLoginUsers(framesync::readLoginUsers(steamDir));
    // What frame_steam.py gives for the same file (checked 2026-10-30): the nested block is skipped
    CHECK(users.size() == 4);
    const auto accounts = framesync::frameAccounts(users);
    CHECK(accounts.size() == 3);
    if (accounts.size() == 3) {
        CHECK(accounts[0].steamId == "76561190000000001" && accounts[0].accountName == "alpha_acct" &&
              accounts[0].personaName == "Alpha {the} \"Great\"");
        CHECK(accounts[1].steamId == "76561190000000002" && accounts[1].personaName == "bravo_acct");
        CHECK(accounts[2].steamId == "76561190000000005" && accounts[2].personaName == "찰리 \\ 백슬래시");
    }
    CHECK(framesync::currentSteamId(users) == "76561190000000002");
    CHECK(framesync::avatarPath(steamDir, "76561190000000001") ==
          steamDir + "/config/avatarcache/76561190000000001.png");
    CHECK(framesync::avatarPath(steamDir, "76561190000000002").empty());  // no file
    CHECK(framesync::avatarPath(steamDir, "../../etc/passwd").empty());
    CHECK(framesync::validSteamId("76561198000000000") && !framesync::validSteamId("7656119800000000") &&
          !framesync::validSteamId("12345678901234567"));
    CHECK(framesync::readLoginUsers("/nonexistent").empty());
    // Edge cases of the regexes
    CHECK(framesync::parseLoginUsers("\"76561190000000009\" {\"AccountName\" \"unterminated}").empty());
    const auto odd = framesync::parseLoginUsers("\"76561190000000009\"{\"AccountName\"\"x\" \"K\"\t\"a\\\"b\"}");
    // frame_steam.py's regexes give {"x": "K"} here (checked with Python): no space after a key, no field
    CHECK(odd.size() == 1 && odd[0].fields.size() == 1 && odd[0].fields.count("x") == 1 &&
          odd[0].fields.at("x") == "K");
}

// ---- Frame Sync's files and status ----

// Whether Frame Sync is set up at all (the 기기 page leaves it out otherwise): looked at, not read
void testSyncPresent() {
    const std::string dir = tempDir("sync-present");
    CHECK(!framesync::settingsPresent(dir + "/none"));
    writeAll(dir + "/cfg/config.json", "{}");
    CHECK(!framesync::settingsPresent(dir + "/cfg"));  // no key
    writeAll(dir + "/cfg/token", "");
    CHECK(!framesync::settingsPresent(dir + "/cfg"));  // an empty key
    writeAll(dir + "/cfg/token", "fake-key\n");
    CHECK(framesync::settingsPresent(dir + "/cfg"));
    std::filesystem::remove(dir + "/cfg/config.json");
    CHECK(!framesync::settingsPresent(dir + "/cfg"));  // a key without its config
    std::filesystem::remove_all(dir);
}

void testSyncFiles() {
    const std::string dir = tempDir("sync-files");
    // Settings: all three fields and a non-empty key, else nothing
    writeAll(dir + "/cfg/config.json",
             R"({"agentUrl": "http://192.168.0.2:8765/", "pcMac": "01:23:45:67:89:ab", "broadcast": "192.168.0.255"})");
    CHECK(!framesync::loadSettings(dir + "/cfg"));  // no token
    writeAll(dir + "/cfg/token", "  fake-token\n");
    auto settings = framesync::loadSettings(dir + "/cfg");
    CHECK(settings && settings->token == "fake-token" && settings->agentUrl == "http://192.168.0.2:8765/" &&
          settings->broadcast == "192.168.0.255");
    writeAll(dir + "/cfg/token", "\n");
    CHECK(!framesync::loadSettings(dir + "/cfg"));
    writeAll(dir + "/cfg/token", "k");
    writeAll(dir + "/cfg/config.json", R"({"agentUrl": "http://x", "pcMac": "", "broadcast": "b"})");
    CHECK(!framesync::loadSettings(dir + "/cfg"));
    writeAll(dir + "/cfg/config.json", "[1, 2]");
    CHECK(!framesync::loadSettings(dir + "/cfg"));
    CHECK(!framesync::loadSettings(dir + "/missing"));

    // The cache: written as Python's json.dump(..., ensure_ascii=False), extra fields kept
    JsonValue accounts;
    std::string error;
    CHECK(parseJson(R"([{"steamId": "76561190000000001", "personaName": "한글 \"이름\"", "accountName": "a",)"
                    R"( "hasAvatar": true, "extra": [1, null]}, {"personaName": "no id"}])",
                    accounts, error));
    const std::string file = dir + "/cache/pc-accounts.json";
    bool written = false;
    CHECK(framesync::saveCachedPcAccounts(file, accounts, error, &written) && written);
    // The same list again (every poll): the file is left alone
    const auto stamp = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    std::filesystem::last_write_time(file, stamp);
    CHECK(framesync::saveCachedPcAccounts(file, accounts, error, &written) && !written);
    CHECK(std::filesystem::last_write_time(file) == stamp);
    CHECK(readAll(file) == R"([{"steamId": "76561190000000001", "personaName": "한글 \"이름\"", "accountName": "a", )"
                           R"("hasAvatar": true, "extra": [1, null]}, {"personaName": "no id"}])");
    auto cached = framesync::loadCachedPcAccounts(file);
    CHECK(cached && cached->size() == 1 && (*cached)[0].personaName == "한글 \"이름\"" && (*cached)[0].hasAvatar);
    writeAll(file, "{not json");
    CHECK(!framesync::loadCachedPcAccounts(file));
    CHECK(!framesync::loadCachedPcAccounts(dir + "/none.json"));

    // /status as flow.py reads it
    JsonValue status;
    CHECK(parseJson(R"({"steam": {"running": true, "steamId": "76561190000000001", "gameAppId": 620980},)"
                    R"( "streaming": false, "users": [{"name": "u"}]})",
                    status, error));
    framesync::PcStatus s = framesync::statusFrom(status);
    CHECK(s.steamRunning && s.steamId == "76561190000000001" && s.gameAppId == "620980" && !s.streaming && s.hasUsers);
    CHECK(parseJson(R"({"steam": {"running": false, "gameAppId": 0}, "streaming": 1, "users": []})", status, error));
    s = framesync::statusFrom(status);
    CHECK(!s.steamRunning && s.steamId.empty() && s.gameAppId.empty() && s.streaming && !s.hasUsers);
    CHECK(parseJson(R"({"steam": null})", status, error));
    s = framesync::statusFrom(status);
    CHECK(!s.steamRunning && !s.hasUsers);

    // The remembered colour
    const std::string colorFile = dir + "/state/passthrough-color";
    CHECK(!readRememberedColor(colorFile));
    CHECK(writeRememberedColor(colorFile, true, error) && readRememberedColor(colorFile) == std::optional<bool>(true));
    CHECK(writeRememberedColor(colorFile, false, error) && readRememberedColor(colorFile) == std::optional<bool>(false));
    writeAll(colorFile, "purple\n");
    CHECK(!readRememberedColor(colorFile));
    std::filesystem::remove_all(dir);
}

// ---- DeviceBackend on fakes ----

struct FakeAudio : AudioService {
    std::vector<std::string> calls;
    bool wanted = false;
    std::optional<AudioState> next;
    void setWanted(bool w) override {
        wanted = w;
        calls.push_back(w ? "want" : "unwant");
    }
    bool take(AudioState& out) override {
        if (!next) return false;
        out = *next;
        next.reset();
        return true;
    }
    void selectOutput(const std::string& id) override { calls.push_back("select " + id); }
    void setVolume(int p) override { calls.push_back("volume " + std::to_string(p)); }
    void setMuted(bool m) override { calls.push_back(m ? "mute" : "unmute"); }
};

struct FakeCamera : CameraSwitchService {
    std::vector<std::string>* log;
    bool wanted = false;
    std::optional<bool> next;
    explicit FakeCamera(std::vector<std::string>* l) : log(l) {}
    void setWanted(bool w) override { wanted = w; }
    bool takeAvailable(bool& a) override {
        if (!next) return false;
        a = *next;
        next.reset();
        return true;
    }
    void setColor(bool c) override { log->push_back(c ? "color" : "mono"); }
};

struct FakeRoomView : RoomViewControl {
    std::vector<std::string>* log;
    std::optional<bool> result;  ///< the next outcome to report
    bool setsExactly = false;  ///< the openvr route (sets, no toggle)
    explicit FakeRoomView(std::vector<std::string>* l) : log(l) {}
    void setEnabled(bool on) override { log->push_back(on ? "view on" : "view off"); }
    bool exact() const override { return setsExactly; }
    bool takeResult(bool& ok) override {
        if (!result) return false;
        ok = *result;
        result.reset();
        return true;
    }
};

struct FakeSync : SyncService {
    std::vector<std::string> calls;
    std::vector<SyncEvent> pending;
    void setup() override { calls.push_back("setup"); }
    void refresh() override { calls.push_back("refresh"); }
    void startSync(const std::string& id) override { calls.push_back("sync " + id); }
    void answerGame(bool yes) override { calls.push_back(yes ? "answer yes" : "answer no"); }
    void shutdownPc() override { calls.push_back("shutdown"); }
    std::vector<SyncEvent> take() override {
        std::vector<SyncEvent> t;
        t.swap(pending);
        return t;
    }
    int count(const std::string& call) const {
        int n = 0;
        for (const auto& c : calls) n += c == call;
        return n;
    }
};

void testBackendSound() {
    DeviceModel model;
    auto audioOwned = std::make_unique<FakeAudio>();
    FakeAudio& audio = *audioOwned;
    DeviceBackend backend(model, std::move(audioOwned), nullptr, nullptr, nullptr);
    double now = 1000;
    CHECK(backend.nextWake() == kInf);
    CHECK(audio.calls.empty());  // nothing shown: nothing watched
    backend.setVisible(true, false, now);
    CHECK(audio.wanted);
    AudioState reading;
    reading.known = true;
    reading.outputs = {{"spk", "Built-in", true}, {"usb", "USB", false}};
    reading.defaultId = "spk";
    reading.volume = 40;
    audio.next = reading;
    const unsigned rev = model.revision();
    CHECK(backend.tick(now) && model.revision() != rev);
    CHECK(model.audio.known && model.audio.volume == 40 && model.audio.defaultId == "spk");

    // Picking an output and muting go straight through
    model.selectOutput("usb");
    model.setMuted(true);
    backend.tick(now);
    CHECK(audio.calls.size() >= 3 && audio.calls[1] == "select usb" && audio.calls[2] == "mute");

    // A drag: throttled, the knob not moved by readings, the release's value always sent
    audio.calls.clear();
    model.dragVolume(50);
    backend.tick(now);
    model.dragVolume(55);
    backend.tick(now + 0.01);
    model.dragVolume(60);
    backend.tick(now + 0.02);
    CHECK(audio.calls == std::vector<std::string>({"volume 50"}));
    CHECK(std::fabs(backend.nextWake() - (now + audio::VolumeThrottle::kMinIntervalSec)) < 1e-9);
    reading.volume = 50;
    audio.next = reading;  // the server's echo of 50 while the knob is at 60
    backend.tick(now + 0.03);
    CHECK(model.audio.volume == 60);
    backend.tick(now + 0.07);
    CHECK(audio.calls.back() == "volume 60");
    model.dragVolume(70);
    backend.tick(now + 0.08);
    model.releaseVolume();
    backend.tick(now + 0.09);
    CHECK(audio.calls.back() == "volume 70");
    // The released value is held while an older reading arrives (no jump back to 60 for a moment)...
    CHECK(std::fabs(backend.nextWake() - (now + 0.09 + DeviceBackend::kVolumeHoldSec)) < 1e-9);
    reading.volume = 60;
    audio.next = reading;
    backend.tick(now + 0.1);
    CHECK(model.audio.volume == 70);
    // ...until the reading of it comes; then readings show as they are
    reading.volume = 70;
    audio.next = reading;
    backend.tick(now + 0.15);
    CHECK(model.audio.volume == 70 && backend.nextWake() == kInf);
    reading.volume = 65;  // Steam changed it
    audio.next = reading;
    backend.tick(now + 0.2);
    CHECK(model.audio.volume == 65);
    // A release whose value never comes back (the server kept 65): the reading shows once the hold runs out
    model.dragVolume(80);
    backend.tick(now + 1);
    model.releaseVolume();
    backend.tick(now + 1.01);
    CHECK(model.audio.volume == 80);
    reading.volume = 65;
    audio.next = reading;
    backend.tick(now + 1.1);
    CHECK(model.audio.volume == 80);
    backend.tick(now + 1.01 + DeviceBackend::kVolumeHoldSec);
    CHECK(model.audio.volume == 65 && backend.nextWake() == kInf);

    backend.setVisible(false, false, now + 1);
    CHECK(!audio.wanted);
}

void testBackendPassthrough() {
    DeviceModel model;
    std::vector<std::string> log;
    auto cameraOwned = std::make_unique<FakeCamera>(&log);
    FakeCamera& camera = *cameraOwned;
    auto viewOwned = std::make_unique<FakeRoomView>(&log);
    FakeRoomView& view = *viewOwned;
    DeviceBackend backend(model, nullptr, std::move(cameraOwned), std::move(viewOwned), nullptr);
    const double now = 50;
    CHECK(!model.passthrough.roomViewKnown && !model.passthrough.cameraSwitchAvailable);
    backend.setVisible(true, false, now);
    CHECK(camera.wanted);

    // Without Camera Switch no colour to pick (the model refuses 컬러; 켬 is on with the colour as it is)
    model.setPassthrough(PassthroughMode::Color);
    backend.tick(now);
    CHECK(log.empty());
    // The view isn't known: the command goes out (SteamVR only takes a toggle); the page shows the wish meanwhile
    model.setPassthrough(PassthroughMode::Off);
    backend.tick(now);
    CHECK(log == std::vector<std::string>({"view off"}));
    CHECK(model.passthrough.roomViewKnown && model.passthrough.mode == PassthroughMode::Off);
    CHECK(!model.passthrough.roomViewConfirmed &&
          backend.nextWake() == now + DeviceBackend::kRoomViewSendSec + DeviceBackend::kRoomViewEventSec);
    // The toggle turned it on (it was off): once more, then SteamVR's word
    backend.roomViewEvent(true);
    backend.tick(now);
    CHECK(log == std::vector<std::string>({"view off", "view off"}));
    backend.roomViewEvent(false);
    backend.tick(now);
    CHECK(log.size() == 2 && model.passthrough.mode == PassthroughMode::Off && model.passthrough.roomViewConfirmed);
    CHECK(backend.nextWake() == kInf);

    // Camera Switch starts: 흑백 = the colour off, then the view on
    camera.next = true;
    backend.tick(now);
    CHECK(model.passthrough.cameraSwitchAvailable);
    log.clear();
    model.setPassthrough(PassthroughMode::Mono);
    backend.tick(now);
    CHECK(log == std::vector<std::string>({"mono", "view on"}));
    CHECK(model.passthrough.mode == PassthroughMode::Mono && model.passthrough.color == ColorSource::Remembered);
    CHECK(backend.rememberedColor() == std::optional<bool>(false));
    CHECK(!model.passthrough.roomViewConfirmed);  // what we asked for, not SteamVR's word
    // 컬러 before SteamVR answered: the colour; no second toggle while one is out
    log.clear();
    model.setPassthrough(PassthroughMode::Color);
    backend.tick(now);
    CHECK(log == std::vector<std::string>({"color"}));
    CHECK(model.passthrough.mode == PassthroughMode::Color);
    view.result = true;  // vrcmd ran
    backend.roomViewEvent(true);
    backend.tick(now);
    CHECK(log.size() == 1 && model.passthrough.mode == PassthroughMode::Color && model.passthrough.roomViewConfirmed);
    // The same button again: the colour can't be read, so it goes out again; the view is on already
    log.clear();
    model.setPassthrough(PassthroughMode::Color);
    backend.tick(now);
    CHECK(log == std::vector<std::string>({"color"}));

    // 끔, and vrcmd failed: the wish is dropped, the page shows SteamVR's last word
    log.clear();
    model.setPassthrough(PassthroughMode::Off);
    backend.tick(now);
    CHECK(log == std::vector<std::string>({"view off"}) && model.passthrough.mode == PassthroughMode::Off);
    view.result = false;
    CHECK(backend.tick(now));
    CHECK(model.passthrough.mode == PassthroughMode::Color && model.passthrough.roomViewConfirmed);
    // 끔 went out but no event came: unknown (nothing selected)
    model.setPassthrough(PassthroughMode::Off);
    backend.tick(now + 1);
    backend.tick(now + 1 + DeviceBackend::kRoomViewEventSec);  // vrcmd still running: still waiting
    CHECK(model.passthrough.roomViewKnown);
    view.result = true;
    backend.tick(now + 5);
    CHECK(model.passthrough.roomViewKnown && backend.nextWake() == now + 5 + DeviceBackend::kRoomViewEventSec);
    backend.tick(now + 5 + DeviceBackend::kRoomViewEventSec);
    CHECK(!model.passthrough.roomViewKnown && !model.passthrough.roomViewConfirmed);

    // SteamVR's events: hidden (the user's double tap)
    backend.roomViewEvent(false);
    CHECK(model.passthrough.mode == PassthroughMode::Off && model.passthrough.roomViewConfirmed);
    // 끔 while SteamVR says it is off: nothing to do
    log.clear();
    model.setPassthrough(PassthroughMode::Off);
    backend.tick(now + 10);
    CHECK(log.empty());
    // A view that won't come on: kRoomViewTries commands, then it is left as SteamVR says
    model.setPassthrough(PassthroughMode::Color);
    backend.tick(now + 10);
    for (int i = 0; i < DeviceBackend::kRoomViewTries; ++i) {
        backend.roomViewEvent(false);
        backend.tick(now + 10);
    }
    CHECK(log == std::vector<std::string>({"color", "view on", "view on", "view on"}));
    CHECK(model.passthrough.mode == PassthroughMode::Off && model.passthrough.roomViewConfirmed);
    // Shown again by SteamVR (its own toggle): in the remembered colour
    backend.roomViewEvent(true);
    CHECK(model.passthrough.mode == PassthroughMode::Color && model.passthrough.roomViewConfirmed);
    // Camera Switch stops: the colour stays as remembered, 흑백 / 컬러 can't be picked
    camera.next = false;
    backend.tick(now);
    CHECK(!model.passthrough.cameraSwitchAvailable && model.passthrough.mode == PassthroughMode::Color);

    // A fresh start: a remembered colour, the view's state unknown until SteamVR says
    DeviceModel model2;
    DeviceBackend backend2(model2, nullptr, nullptr, nullptr, nullptr);
    CHECK(model2.passthrough.color == ColorSource::Unknown);
    backend2.setRememberedColor(true);
    CHECK(!model2.passthrough.roomViewKnown && model2.passthrough.color == ColorSource::Remembered);
    backend2.roomViewEvent(true);
    CHECK(model2.passthrough.roomViewKnown && model2.passthrough.mode == PassthroughMode::Color);

    // A control that sets the view (the openvr route): no event after it went out = it already was so
    DeviceModel model3;
    std::vector<std::string> log3;
    auto view3Owned = std::make_unique<FakeRoomView>(&log3);
    FakeRoomView& view3 = *view3Owned;
    view3.setsExactly = true;
    DeviceBackend backend3(model3, nullptr, nullptr, std::move(view3Owned), nullptr);
    backend3.setVisible(true, false, now);
    model3.setPassthrough(PassthroughMode::Off);
    backend3.tick(now);
    CHECK(log3 == std::vector<std::string>({"view off"}));
    view3.result = true;
    backend3.tick(now);
    CHECK(backend3.nextWake() == now + DeviceBackend::kRoomViewEventSec);
    backend3.tick(now + DeviceBackend::kRoomViewEventSec);
    // Taken as off, but not SteamVR's word (its own 룸 뷰 button may change it without telling)
    CHECK(model3.passthrough.mode == PassthroughMode::Off && !model3.passthrough.roomViewConfirmed);
    CHECK(backend3.nextWake() == kInf);
    // 끔 again: sent again
    model3.setPassthrough(PassthroughMode::Off);
    backend3.tick(now + 5);
    CHECK(log3 == std::vector<std::string>({"view off", "view off"}));
    view3.result = true;
    backend3.tick(now + 5);
    backend3.tick(now + 5 + DeviceBackend::kRoomViewEventSec);
    log3.erase(log3.begin());
    // 켬: the set message, then SteamVR's word
    model3.setPassthrough(PassthroughMode::Mono);
    backend3.tick(now + 5);
    CHECK(log3 == std::vector<std::string>({"view off", "view on"}));
    view3.result = true;
    backend3.roomViewEvent(true);
    backend3.tick(now + 5);
    CHECK(model3.passthrough.mode == PassthroughMode::Mono && model3.passthrough.roomViewConfirmed);
    // 끔 while 켬 is still out with no word back: 켬 counts as done, then 끔 goes out
    backend3.roomViewEvent(false);  // hidden by SteamVR's double tap
    log3.clear();
    model3.setPassthrough(PassthroughMode::Mono);
    backend3.tick(now + 20);
    CHECK(log3 == std::vector<std::string>({"view on"}));
    view3.result = true;
    backend3.tick(now + 20);
    model3.setPassthrough(PassthroughMode::Off);
    backend3.tick(now + 21);
    CHECK(log3.size() == 1);  // one out at a time
    backend3.tick(now + 20 + DeviceBackend::kRoomViewEventSec);
    CHECK(log3 == std::vector<std::string>({"view on", "view off"}));
    CHECK(!model3.passthrough.roomViewConfirmed && model3.passthrough.mode == PassthroughMode::Off);
}

SyncEvent setupEvent(bool configured) {
    SyncEvent e;
    e.kind = SyncEvent::Kind::Setup;
    e.configured = configured;
    e.frameId = "76561190000000001";
    e.frameAccounts = {{"76561190000000001", "a", "Alpha"}, {"76561190000000002", "b", "Bravo"}};
    e.pcAccounts = std::vector<framesync::PcAccount>{{"76561190000000001", "Alpha", "a", false}};
    e.avatars["76561190000000001"] = "/tmp/a.png";
    return e;
}

void testBackendSync() {
    DeviceModel model;
    auto syncOwned = std::make_unique<FakeSync>();
    FakeSync& sync = *syncOwned;
    DeviceBackend backend(model, nullptr, nullptr, nullptr, std::move(syncOwned));
    double now = 0;
    // Hidden: nothing asked, no wake
    backend.tick(now);
    CHECK(sync.calls.empty() && backend.nextWake() == kInf);
    // Only the quick view (sound): still nothing for the PC
    backend.setVisible(true, false, now);
    backend.tick(now);
    CHECK(sync.calls.empty());
    // The page: settings first, the poll once they are read
    backend.setVisible(true, true, now);
    backend.tick(now);
    CHECK(sync.calls == std::vector<std::string>({"setup"}));
    CHECK(backend.nextWake() == kInf);  // waits for the setup's answer
    sync.pending.push_back(setupEvent(true));
    backend.tick(now += 0.05);
    CHECK(sync.count("refresh") == 1);
    CHECK(model.sync.frameId == "76561190000000001" && model.sync.frameAccounts.size() == 2 &&
          model.sync.pcAccounts && model.sync.avatars.count("76561190000000001") == 1);
    CHECK(backend.nextWake() == kInf);  // the poll is out
    SyncEvent refresh;
    refresh.kind = SyncEvent::Kind::Refresh;
    refresh.reach = framesync::Reach::Off;
    sync.pending.push_back(refresh);
    CHECK(backend.tick(now += 1));
    CHECK(model.sync.reach == framesync::Reach::Off);
    CHECK(std::fabs(backend.nextWake() - 5.05) < 1e-9);  // every kRefreshSec from the last poll
    // The same answer again: nothing to redraw
    sync.pending.push_back(refresh);
    CHECK(!backend.tick(now));
    // New avatars or a changed account list do redraw
    SyncEvent withAvatar = refresh;
    withAvatar.avatars["76561190000000002"] = "/tmp/b.png";
    sync.pending.push_back(withAvatar);
    CHECK(backend.tick(now));
    sync.pending.push_back(withAvatar);
    CHECK(!backend.tick(now));
    SyncEvent withAccounts = refresh;
    withAccounts.pcAccounts = std::vector<framesync::PcAccount>{{"76561190000000001", "Alpha2", "a", false}};
    sync.pending.push_back(withAccounts);
    CHECK(backend.tick(now));
    sync.pending.push_back(withAccounts);
    CHECK(!backend.tick(now));
    backend.tick(now = 4);
    CHECK(sync.count("refresh") == 1);
    backend.tick(now = 5.05);
    CHECK(sync.count("refresh") == 2);
    // A poll that takes long isn't doubled
    backend.tick(now = 10.1);
    CHECK(sync.count("refresh") == 2);
    sync.pending.push_back(refresh);
    backend.tick(now = 10.2);
    CHECK(sync.count("refresh") == 2 && std::fabs(backend.nextWake() - 15.1) < 1e-9);

    // A sync: no polls while it runs; the game question; its timeout answers 취소; the end polls again
    model.startSync("76561190000000001", false);
    backend.tick(now = 11);
    CHECK(sync.calls.back() == "sync 76561190000000001" && model.sync.busy);
    CHECK(backend.nextWake() == kInf);
    SyncEvent ask;
    ask.kind = SyncEvent::Kind::AskGame;
    sync.pending.push_back(ask);
    backend.tick(now = 12);
    CHECK(model.sync.question == SyncState::Question::Game);
    CHECK(backend.nextWake() == 12 + framesync::kAskTimeoutSec);
    backend.tick(now = 50);
    CHECK(sync.count("refresh") == 2);  // busy: no poll
    backend.tick(now = 12 + framesync::kAskTimeoutSec);
    CHECK(sync.calls.back() == "answer no" && model.sync.question == SyncState::Question::None);
    SyncEvent done;
    done.kind = SyncEvent::Kind::Finished;
    done.ok = false;
    done.message = tr(Tx::SyncCancelled);
    sync.pending.push_back(done);
    backend.tick(now = 133);
    CHECK(!model.sync.busy && model.sync.message == tr(Tx::SyncCancelled) && model.sync.messageError);
    CHECK(sync.count("refresh") == 3);  // at once after a run

    // The PC's 끄기 and its answer
    model.sync.reach = framesync::Reach::On;
    model.askShutdown();
    model.answer(true);
    backend.tick(now = 134);
    CHECK(sync.calls.back() == "shutdown");
    SyncEvent msg;
    msg.kind = SyncEvent::Kind::Message;
    msg.message = "집 PC를 20초 뒤에 꺼요.";
    sync.pending.push_back(msg);
    backend.tick(now = 135);
    CHECK(model.sync.message == "집 PC를 20초 뒤에 꺼요." && !model.sync.messageError);

    // Hidden: no more polls; shown again: the settings are read again
    backend.setVisible(false, false, now = 140);
    const int polls = sync.count("refresh");
    backend.tick(now = 200);
    CHECK(sync.count("refresh") == polls && backend.nextWake() == kInf);
    backend.setVisible(false, true, now = 300);
    CHECK(sync.count("setup") == 2);

    // Not configured: no polls at all
    DeviceModel model2;
    auto sync2Owned = std::make_unique<FakeSync>();
    FakeSync& sync2 = *sync2Owned;
    DeviceBackend backend2(model2, nullptr, nullptr, nullptr, std::move(sync2Owned));
    backend2.setVisible(false, true, 0);
    sync2.pending.push_back(setupEvent(false));
    backend2.tick(1);
    CHECK(!model2.sync.configured && sync2.count("refresh") == 0 && backend2.nextWake() == kInf);
    CHECK(syncScreen(model2.sync).bigText == tr(Tx::SyncNoSettings));
}

}  // namespace

int main() {
    testSinks();
    testSinksBeta();
    testMicFor();
    testThrottle();
    testNameOwner();
    testFrameSteam();
    testSyncPresent();
    testSyncFiles();
    testBackendSound();
    testBackendPassthrough();
    testBackendSync();
    if (failures > 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("device-backend: all passed\n");
    return 0;
}
