#include "gaze_feed.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>
#include <ctime>
#include <fstream>
#include <system_error>

#include <openvr.h>

#include "../third_party/vkbasalt/src/frame_gaze.hpp"
#include "auto_profile.h"
#include "eye_server.h"
#include "gaze_fit.h"
#include "gaze_map.h"

namespace framecolor {

namespace {

constexpr float kRaisedRate = 90.0f;
constexpr float kDefaultRate = 15.0f;  // the Frame's default (frame_hmd_additional.vrsettings)
// The rate is raised only while a PC stream runs: a standalone game can flicker black with any raised minimum
// (Eleven Table Tennis at 60 and 90 Hz, fine at the default, 2026-11-13); streamed games were tested at 90 Hz
// (Titanfall 2). Checked this often while following
constexpr auto kStreamCheck = std::chrono::seconds(1);
// Rewrite an unchanged point this often: the layer drops points older than 200 ms
constexpr auto kHeartbeat = std::chrono::milliseconds(100);

std::uint64_t monotonicNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(now.tv_nsec);
}

framegaze::Record* mapRecord() {
    const int fd = ::open(framegaze::path(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return nullptr;
    if (::ftruncate(fd, sizeof(framegaze::Record)) != 0) {
        ::close(fd);
        return nullptr;
    }
    void* memory = ::mmap(nullptr, sizeof(framegaze::Record), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (memory == MAP_FAILED) return nullptr;
    auto* record = static_cast<framegaze::Record*>(memory);
    if (record->magic != framegaze::kMagic || record->version != framegaze::kVersion) framegaze::initialize(*record);
    framegaze::recover(*record);
    framegaze::write(*record, {});  // invalid until a point arrives
    return record;
}

// The gaze fit's dot: an overlay fixed to the headset 0.9 m ahead (frameeyeosc's distance: nearer than the dashboard,
// so it shows over it), white while the eyes settle on it, green while measured, orange when measured again
class DotOverlay {
public:
    enum class Shade { Settle, Measure, Again, Probe };
    ~DotOverlay() {
        if (handle_ != vr::k_ulOverlayHandleInvalid && vr::VROverlay()) vr::VROverlay()->DestroyOverlay(handle_);
    }
    void show(const GazeTarget& target, Shade shade, float distanceM = kDistanceM) {
        if (!create()) return;
        vr::IVROverlay* overlay = vr::VROverlay();
        const bool probe = shade == Shade::Probe;
        if (distanceM != distance_ || probe != probeSize_) {
            // The same size to the eye at any distance (the probe a little bigger, to be found on recordings)
            overlay->SetOverlayWidthInMeters(handle_, kWidthM * distanceM / kDistanceM * (probe ? 1.6f : 1.0f));
            distance_ = distanceM;
            probeSize_ = probe;
            shown_ = false;  // show again with the new size
        }
        if (!shown_ || shade != shade_) {
            const auto pixels = image(shade);
            overlay->SetOverlayRaw(handle_, const_cast<std::uint8_t*>(pixels.data()), kPx, kPx, 4);
            shade_ = shade;
        }
        if (!shown_ || target.xDeg != target_.xDeg || target.yDeg != target_.yDeg) {
            vr::HmdMatrix34_t m = transform(target, distance_);
            overlay->SetOverlayTransformTrackedDeviceRelative(handle_, vr::k_unTrackedDeviceIndex_Hmd, &m);
            target_ = target;
        }
        if (!shown_) overlay->ShowOverlay(handle_);
        shown_ = true;
    }
    void hide() {
        if (shown_ && handle_ != vr::k_ulOverlayHandleInvalid && vr::VROverlay()) vr::VROverlay()->HideOverlay(handle_);
        shown_ = false;
    }

private:
    static constexpr int kPx = 64;
    static constexpr float kDistanceM = 0.9f, kWidthM = 0.035f;  // about 2.2 deg across, the white core under 1 deg
    bool create() {
        if (handle_ != vr::k_ulOverlayHandleInvalid) return true;
        if (failed_ || !vr::VROverlay()) return false;
        const vr::EVROverlayError error =
            vr::VROverlay()->CreateOverlay("framedock.gazefit", "Frame Dock gaze fit", &handle_);
        if (error != vr::VROverlayError_None) {
            std::fprintf(stderr, "[gaze] fit dot overlay: %s\n", vr::VROverlay()->GetOverlayErrorNameFromEnum(error));
            handle_ = vr::k_ulOverlayHandleInvalid;
            failed_ = true;
            return false;
        }
        vr::VROverlay()->SetOverlayWidthInMeters(handle_, kWidthM);
        vr::VROverlay()->SetOverlaySortOrder(handle_, 1000);
        return true;
    }
    static std::vector<std::uint8_t> image(Shade shade) {
        const std::uint8_t core[3] = {static_cast<std::uint8_t>(shade == Shade::Probe ? 0 : shade == Shade::Measure ? 90 : 255),
                                      static_cast<std::uint8_t>(shade == Shade::Again ? 150 : 255),
                                      static_cast<std::uint8_t>(shade == Shade::Probe ? 0 : shade == Shade::Settle ? 255 : 90)};
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kPx) * kPx * 4, 0);
        const double c = kPx / 2.0, inner = kPx * (shade == Shade::Probe ? 0.30 : 0.18), outer = kPx * 0.45;
        for (int y = 0; y < kPx; ++y) {
            for (int x = 0; x < kPx; ++x) {
                const double d = std::hypot(x + 0.5 - c, y + 0.5 - c);
                const double alpha = std::clamp(outer - d + 0.5, 0.0, 1.0), lit = std::clamp(inner - d + 0.5, 0.0, 1.0);
                std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * kPx + x) * 4];
                for (int i = 0; i < 3; ++i) p[i] = static_cast<std::uint8_t>(std::lround(13 + lit * (core[i] - 13)));
                p[3] = static_cast<std::uint8_t>(std::lround(alpha * 240));
            }
        }
        return rgba;
    }
    static vr::HmdMatrix34_t transform(const GazeTarget& target, float distanceM) {
        constexpr float kRad = 3.14159265f / 180.0f;
        float d[3] = {std::tan(target.xDeg * kRad), std::tan(target.yDeg * kRad), -1.0f};
        const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (float& v : d) v /= length;
        // The overlay's +Z towards the eyes, its x level
        const float z[3] = {-d[0], -d[1], -d[2]};
        float x[3] = {z[2], 0.0f, -z[0]};  // (0, 1, 0) x z
        const float xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
        x[0] /= xl;
        x[2] /= xl;
        const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
        vr::HmdMatrix34_t m{};
        for (int r = 0; r < 3; ++r) {
            m.m[r][0] = x[r];
            m.m[r][1] = y[r];
            m.m[r][2] = z[r];
            m.m[r][3] = d[r] * distanceM;
        }
        return m;
    }
    vr::VROverlayHandle_t handle_ = vr::k_ulOverlayHandleInvalid;
    bool failed_ = false, shown_ = false;
    float distance_ = kDistanceM;
    bool probeSize_ = false;
    Shade shade_ = Shade::Settle;
    GazeTarget target_;
};

// One fit in progress: the dots, the one shown, and what each measured
struct FitRun {
    bool recenter = false;
    std::vector<GazeTarget> targets;
    std::vector<DotResult> results;
    std::size_t index = 0;
    int attempt = 0;
    bool measuring = false;
    std::chrono::steady_clock::time_point phaseEnd;
    DotMeasure measure;
};
// frameeyeosc: each dot 2.5 s, the last 1.7 s measured; a dot measured again up to three times
constexpr std::chrono::milliseconds kSettle{800}, kSettleAgain{500}, kMeasure{1700}, kRecenterAfterWear{3000},
    kWearPoll{250};
constexpr int kAttempts = 3;
// After a fit: the tint stays this long to look around with the new fit
constexpr std::chrono::seconds kReview{10};
// What a gaze-debug file made for a fit holds (one left by a crash is removed at the next start; the user's own stays)
constexpr const char* kTintMark = "framedock gaze fit";

bool isOurTint(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::string line;
    return std::getline(in, line) && line == kTintMark;
}

}  // namespace

GazeFeed::GazeFeed(std::filesystem::path configDir)
    : marker_(configDir / "gaze-rate-owned"), fitFile_(configDir / "gaze-fit.txt"),
      renderTestFile_(configDir / "gaze-render-test"), tintFile_(configDir / "gaze-debug") {
    // A previous run that stopped without giving the rate back (crash, kill): put the default back now
    std::error_code ec;
    if (std::filesystem::exists(marker_, ec)) {
        rateRaised_ = true;
        setRate(false);
    }
    if (isOurTint(tintFile_)) std::filesystem::remove(tintFile_, ec);  // a fit's tint left by a crash
    record_ = mapRecord();
    if (!record_) std::fprintf(stderr, "[gaze] %s unavailable: gaze-following sharpening stays at the lens centre\n",
                               framegaze::path());
    running_ = true;
    worker_ = std::thread([this] { run(); });
}

GazeFeed::~GazeFeed() {
    stop();
    if (record_) ::munmap(record_, sizeof(framegaze::Record));
}

void GazeFeed::stop() {
    if (running_.exchange(false) && worker_.joinable()) worker_.join();
    if (record_) framegaze::write(*record_, {});
    setRate(false);
    setTint(false);
    reviewing_ = false;
}

void GazeFeed::setTint(bool on) {
    std::error_code ec;
    if (on && !tintOwned_) {
        if (std::filesystem::exists(tintFile_, ec)) return;  // the user's own debug view: left as it is
        std::ofstream(tintFile_) << kTintMark << "\n";
        tintOwned_ = true;
    } else if (!on && tintOwned_) {
        if (isOurTint(tintFile_)) std::filesystem::remove(tintFile_, ec);
        tintOwned_ = false;
    }
}

bool GazeFeed::setRate(bool raised) {
    if (raised == rateRaised_ || vr::VRSettings() == nullptr) return raised == rateRaised_;
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    std::error_code ec;
    if (raised) {
        // The value before ours goes into the marker, so a user's own minimum comes back (not just the default)
        vr::EVRSettingsError readError = vr::VRSettingsError_None;
        const float previous = vr::VRSettings()->GetFloat("steamvr", "eyeTrackingRateMin", &readError);
        std::ofstream(marker_) << (readError == vr::VRSettingsError_None ? previous : kDefaultRate) << "\n";
        vr::VRSettings()->SetFloat("steamvr", "eyeTrackingRateMin", kRaisedRate, &error);
    } else {
        float previous = kDefaultRate;
        std::ifstream(marker_) >> previous;
        if (!(previous > 0.0f) || previous == kRaisedRate || previous == kDefaultRate)
            vr::VRSettings()->RemoveKeyInSection("steamvr", "eyeTrackingRateMin", &error);  // the default again
        else
            vr::VRSettings()->SetFloat("steamvr", "eyeTrackingRateMin", previous, &error);
    }
    if (error != vr::VRSettingsError_None) {
        std::fprintf(stderr, "[gaze] eye tracking rate %s failed: %s\n", raised ? "raise" : "restore",
                     vr::VRSettings()->GetSettingsErrorNameFromEnum(error));
        if (raised) std::filesystem::remove(marker_, ec);
        return false;
    }
    if (!raised) std::filesystem::remove(marker_, ec);
    rateRaised_ = raised;
    std::fprintf(stderr, "[gaze] eye tracking minimum rate %s\n", raised ? "raised to 90 Hz" : "back to default");
    return true;
}

void GazeFeed::run() {
    DistortionGrid grids[2];
    bool gridFailed = false, wasEnabled = false;
    auto gridRetry = std::chrono::steady_clock::time_point{};
    StreamingProbe streaming;
    auto nextStreamCheck = std::chrono::steady_clock::time_point{};
    bool streamingNow = false, lastWantRaised = false;
    vr::HmdVector2_t last[2]{{{-9, -9}}, {{-9, -9}}};
    bool lastValid = false;
    auto lastWrite = std::chrono::steady_clock::time_point{};
    EyeServer eyeServer;
    EyeView views[2];
    float eyeX[2] = {-0.034f, 0.034f};  // each eye's sideways offset in the head (GetEyeToHeadTransform)
    GazeSmoother smoother;
    int timedSamples = 0;
    double firstSampleTime = 0;
    auto eyeServerRetry = std::chrono::steady_clock::time_point{}, lastEyeSample = std::chrono::steady_clock::time_point{};
    bool fallbackNoted = false;
    std::optional<GazeTarget> renderTest;
    float renderTestDistance = 0;  // > 0: a far probe dot (drawing fit measurements) instead of the fit's dot
    bool renderTestLogged = false, renderTestVerify = false;
    bool renderShown = false;  // the render test's dot is up  // verify: the probe and the drawn centre both
    DrawFit drawFit = loadDrawFit((fitFile_.parent_path() / "gaze-draw-fit.txt").string());
    if (!drawFit.valid) drawFit = builtInDrawFit();  // the one measured on a Steam Frame (2026-11-10)
    std::filesystem::file_time_type drawFitTime{};
    auto nextDrawFitCheck = std::chrono::steady_clock::time_point{};

    auto nextRenderTestCheck = std::chrono::steady_clock::time_point{};
    GazeFit fit = loadGazeFit(fitFile_);
    smoother.setFit(fit);
    fitted_ = fit.fitted;
    if (fit.fitted)
        std::fprintf(stderr, "[gaze] fit: 9-dot grid, straight ahead read (%.1f, %.1f) deg, moved (%.1f, %.1f) since\n",
                     fit.measured[4][0] * 45.0f, fit.measured[4][1] * 45.0f, fit.shiftX * 45.0f, fit.shiftY * 45.0f);
    else if (std::error_code ec; std::filesystem::exists(fitFile_, ec))
        std::fprintf(stderr, "[gaze] %s is not a 9-dot fit (or not believed): fit again\n", fitFile_.c_str());
    std::optional<FitRun> fitRun;
    DotOverlay dot;
    std::optional<bool> worn;
    auto nextWearPoll = std::chrono::steady_clock::time_point{};
    std::optional<std::chrono::steady_clock::time_point> recenterAt, reviewUntil;
    const auto endFit = [&](FitOutcome outcome, const char* why, FitFailure failure = FitFailure::None) {
        dot.hide();
        if (fitRun)
            std::fprintf(stderr, "[gaze] %s %s\n", fitRun->recenter ? "re-centre" : "fit", why);
        const bool shown = fitRun && !fitRun->recenter;  // the automatic re-centre only logs (nobody pressed anything)
        if (shown && outcome == FitOutcome::Done) {
            reviewUntil = std::chrono::steady_clock::now() + kReview;
            reviewEnd_ = reviewUntil->time_since_epoch().count();
            reviewing_ = true;  // before the step goes to -1 below: the page never sees a fit with neither
        }
        fitRun.reset();
        if (shown) fitFailure_ = static_cast<int>(failure);
        fitStep_ = -1;
        if (shown) fitOutcome_ = static_cast<int>(outcome);
    };
    const auto startFit = [&](bool recenter) {
        fitRun.emplace();
        fitRun->recenter = recenter;
        fitRun->targets = recenter ? recenterTargets() : fullFitTargets();
        fitRun->phaseEnd = std::chrono::steady_clock::now() + kSettle;
        if (!recenter) {
            reviewUntil.reset();
            fitSteps_ = static_cast<int>(fitRun->targets.size());
            fitStep_ = 0;
            fitOutcome_ = static_cast<int>(FitOutcome::None);
        }
        std::fprintf(stderr, "[gaze] %s started\n", recenter ? "re-centre" : "fit");
    };
    // The dot shown, and the step to the next phase when its time is up
    const auto stepFit = [&](bool eyeServerOpen, bool noGrid) {
        if (!fitRun) return;
        const auto now = std::chrono::steady_clock::now();
        if (!eyeServerOpen) return endFit(FitOutcome::Failed, "failed: no eye-server gaze", FitFailure::NoGaze);
        if (noGrid)
            return endFit(FitOutcome::Failed, "failed: no distortion grid (the gaze is not read)", FitFailure::NoGrid);
        if (fitRun->recenter && vr::VROverlay() && vr::VROverlay()->IsDashboardVisible())
            return endFit(FitOutcome::Stopped, "stopped: the dashboard opened");
        FitRun& current = *fitRun;
        if (now >= current.phaseEnd) {
            if (!current.measuring) {
                current.measuring = true;
                current.measure.clear();
                current.phaseEnd = now + kMeasure;
            } else {
                const DotResult result = current.measure.result();
                current.measuring = false;
                if (!result.ok && ++current.attempt < kAttempts) {
                    std::fprintf(stderr, "[gaze] dot %zu measured again (spread %.1f deg, %d/%d usable)\n", current.index,
                                 result.spreadDeg, result.usable, result.received);
                    current.phaseEnd = now + kSettleAgain;
                } else if (!result.ok) {
                    current.results.push_back(result);
                    std::fprintf(stderr, "%s", describeDots(current.targets, current.results, fit).c_str());
                    return endFit(FitOutcome::Failed, "failed: the gaze was not steady (nothing changed)",
                                  FitFailure::Unsteady);
                } else {
                    current.results.push_back(result);
                    current.attempt = 0;
                    if (++current.index == current.targets.size()) {
                        const std::optional<GazeFit> next = current.recenter ? recenterFromDot(current.results[0], fit)
                                                                       : fitFromDots(current.results);
                        if (!next) {
                            std::fprintf(stderr, "%s", describeDots(current.targets, current.results, fit).c_str());
                            return endFit(FitOutcome::Failed, current.recenter
                                                                  ? "failed: straight ahead moved over 6 deg (kept as it was)"
                                                                  : "failed: the dots did not read in order (one looked at wrongly?)",
                                          FitFailure::OutOfOrder);
                        }
                        fit = *next;
                        smoother.setFit(fit);
                        fitted_ = fit.fitted;
                        if (!saveGazeFit(fitFile_, fit)) std::fprintf(stderr, "[gaze] %s not saved\n", fitFile_.c_str());
                        std::fprintf(stderr, "[gaze] fit: straight ahead read (%.1f, %.1f) deg, moved (%.1f, %.1f) since\n%s",
                                     fit.measured[4][0] * 45.0f, fit.measured[4][1] * 45.0f, fit.shiftX * 45.0f,
                                     fit.shiftY * 45.0f, describeDots(current.targets, current.results, fit).c_str());
                        return endFit(FitOutcome::Done, "done");
                    }
                    if (!current.recenter) fitStep_ = static_cast<int>(current.index);
                    current.phaseEnd = now + kSettle;
                }
            }
        }
        dot.show(current.targets[current.index], current.measuring ? DotOverlay::Shade::Measure
                                         : current.attempt > 0 ? DotOverlay::Shade::Again
                                                           : DotOverlay::Shade::Settle);
    };
    while (running_) {
        const int request = fitRequest_.exchange(kNoRequest);
        switch (request) {
            case kStartFit:
                if (fitRun && fitRun->recenter) endFit(FitOutcome::Stopped, "stopped for a full fit");
                if (fitRun) break;
                startFit(false);
                break;
            case kStopFit:
                if (fitRun) endFit(FitOutcome::Stopped, "stopped");
                break;
            default: break;
        }
        // Put on again (a fit is in use and the gaze is followed): straight ahead once more, with the dashboard closed
        if (fit.fitted && enabled_.load() && vr::VRSystem() != nullptr && std::chrono::steady_clock::now() >= nextWearPoll) {
            nextWearPoll = std::chrono::steady_clock::now() + kWearPoll;
            const auto level = vr::VRSystem()->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
            // The app's own rule (main.cpp hmdInUse): off the head the Frame reads Standby (proximity sensor);
            // Idle only means it has not moved for a while
            const bool onHead = level != vr::k_EDeviceActivityLevel_Standby;
            if (worn && !*worn && onHead) recenterAt = std::chrono::steady_clock::now() + kRecenterAfterWear;
            if (!onHead) recenterAt.reset();
            worn = onHead;
            if (recenterAt && std::chrono::steady_clock::now() >= *recenterAt && !fitRun && eyeServer.isOpen() &&
                !gridFailed && vr::VROverlay() && !vr::VROverlay()->IsDashboardVisible()) {
                recenterAt.reset();
                startFit(true);
            }
        }
        const bool enabled = (enabled_.load() || fitRun.has_value()) && record_ != nullptr && vr::VRSystem() != nullptr;
        // The tint: through a full fit (drawn with the fit in use) and a few seconds after one (with the new fit)
        if (reviewUntil && (!enabled || std::chrono::steady_clock::now() >= *reviewUntil)) reviewUntil.reset();
        reviewing_ = reviewUntil.has_value();
        setTint((enabled && fitRun && !fitRun->recenter) || reviewUntil.has_value());
        if (!enabled) {
            if (fitRun) endFit(FitOutcome::Stopped, "stopped");
            if (renderShown) {
                dot.hide();
                renderShown = false;
            }
            if (wasEnabled) {
                framegaze::write(*record_, {});
                setRate(false);
                std::fprintf(stderr, "[gaze] following off\n");
            }
            wasEnabled = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (!wasEnabled) {
            smoother.reset();  // nothing carried over from before it was off
            std::fprintf(stderr, "[gaze] following on\n");
            wasEnabled = true;
            nextStreamCheck = {};  // the rate is decided right away
        }
        bool rateCheck = false;
        if (const auto now = std::chrono::steady_clock::now(); now >= nextStreamCheck) {
            nextStreamCheck = now + kStreamCheck;
            streamingNow = streaming.streaming(std::chrono::duration<double>(now.time_since_epoch()).count());
            rateCheck = true;
        }
        // Also through a full fit the user started (on the panel, not mid-game): its 1.7 s per dot would get about a
        // sixth of the samples at the default. The automatic recenter after putting the headset on runs mid-game, so
        // it stays at the default when standalone. Set when the wanted rate changes, else once a second (a failed
        // change is retried then, not on every pass)
        const bool wantRaised = streamingNow || (fitRun && !fitRun->recenter);
        if (rateCheck || wantRaised != lastWantRaised) {
            setRate(wantRaised);
            lastWantRaised = wantRaised;
        }
        if (gridFailed && std::chrono::steady_clock::now() >= gridRetry) gridFailed = false;  // try again
        if (!gridFailed && (!grids[0].ready() || !grids[1].ready())) {
            for (int eye = 0; eye < 2; ++eye) {
                const bool built = grids[eye].build([eye](float u, float v, Uv& out) {
                    vr::DistortionCoordinates_t coordinates{};
                    if (!vr::VRSystem()->ComputeDistortion(static_cast<vr::EVREye>(eye), u, v, &coordinates)) return false;
                    out = {coordinates.rfGreen[0], coordinates.rfGreen[1]};
                    return true;
                });
                if (!built) gridFailed = true;
            }
            if (gridFailed) gridRetry = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            std::fprintf(stderr, "[gaze] distortion grid %s\n", gridFailed ? "unavailable (lens centre stays)" : "ready");
        }
        // Gaze source: the eye tracker's own shared memory when it is a known format (raw per-eye directions, every
        // tracker sample, waited on with a futex), else SteamVR's foveation centre (2026-11-08: that one came filtered
        // for foveated rendering: the horizontal swing was a third of the vertical one and it lagged)
        if (!eyeServer.isOpen() && std::chrono::steady_clock::now() >= eyeServerRetry) {
            if (eyeServer.open()) {
                smoother.reset();
                for (int eye = 0; eye < 2; ++eye) {
                    vr::VRSystem()->GetProjectionRaw(static_cast<vr::EVREye>(eye), &views[eye].left, &views[eye].right,
                                                     &views[eye].top, &views[eye].bottom);
                    const vr::HmdMatrix34_t m = vr::VRSystem()->GetEyeToHeadTransform(static_cast<vr::EVREye>(eye));
                    for (int row = 0; row < 3; ++row)
                        for (int column = 0; column < 3; ++column) views[eye].eyeToHead[row][column] = m.m[row][column];
                    eyeX[eye] = m.m[0][3];
                }
                std::fprintf(stderr, "[gaze] source: eye-server shared memory (version %d)\n", eyeServer.version());
            } else {
                eyeServerRetry = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                if (!fallbackNoted) std::fprintf(stderr, "[gaze] source: SteamVR foveation centre (no known eye-server)\n");
                fallbackNoted = true;
            }
        }
        stepFit(eyeServer.isOpen(), gridFailed);
        // The render test file, read a few times a second (its dot and the sharpening centre on one fixed direction)
        if (std::chrono::steady_clock::now() >= nextRenderTestCheck) {
            nextRenderTestCheck = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            std::optional<GazeTarget> wanted;
            std::ifstream in(renderTestFile_);
            GazeTarget t;
            float distance = 0;
            if (in >> t.xDeg >> t.yDeg && std::fabs(t.xDeg) <= 40 && std::fabs(t.yDeg) <= 40) {
                wanted = t;
                if (!(in >> distance) || !(distance >= 0.5f && distance <= 50.0f)) distance = 0;
                std::string mode;
                renderTestVerify = distance > 0 && (in >> mode) && mode == "verify";
            }
            if (renderTestDistance != distance) renderTestLogged = false;
            renderTestDistance = distance;
            if (wanted.has_value() != renderTest.has_value() ||
                (wanted && (wanted->xDeg != renderTest->xDeg || wanted->yDeg != renderTest->yDeg))) {
                renderTestLogged = false;
                if (wanted) std::fprintf(stderr, "[gaze] render test at (%.1f, %.1f) deg\n", wanted->xDeg, wanted->yDeg);
                else std::fprintf(stderr, "[gaze] render test off\n");
                if (!wanted && !fitRun) dot.hide();
            }
            renderTest = wanted;
        }
        // The drawing fit file, picked up again when it changes (measured and written from the PC)
        if (std::chrono::steady_clock::now() >= nextDrawFitCheck) {
            nextDrawFitCheck = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            std::error_code ec;
            const auto path = fitFile_.parent_path() / "gaze-draw-fit.txt";
            const auto time = std::filesystem::last_write_time(path, ec);
            if (ec && drawFitTime != std::filesystem::file_time_type{}) {
                drawFitTime = {};
                drawFit = builtInDrawFit();
                std::fprintf(stderr, "[gaze] drawing fit file removed: the built-in one\n");
            } else if (!ec && time != drawFitTime) {
                drawFitTime = time;
                drawFit = loadDrawFit(path.string());
                std::fprintf(stderr, "[gaze] drawing fit %s\n", drawFit.valid ? "loaded" : "not believed: the built-in one");
                if (!drawFit.valid) drawFit = builtInDrawFit();
            }
        }
        vr::HmdVector2_t ndc[2]{};
        bool tracked = false;
        // Whether this pass is the render test's (only then does it touch the dot, the log and the published point)
        const bool renderThis = renderTest && !fitRun && !gridFailed && eyeServer.isOpen();
        if (renderShown && !renderThis && !fitRun) {
            dot.hide();
            renderShown = false;
        }
        if (renderThis) {
            renderShown = true;
            if (renderTestDistance > 0) dot.show(*renderTest, DotOverlay::Shade::Probe, renderTestDistance);
            else dot.show(*renderTest, DotOverlay::Shade::Settle);
            auto direction = anglesToDirection(renderTest->xDeg / 45.0f, renderTest->yDeg / 45.0f);
            // A probe at a distance: each eye's own direction to it (the path is measured against what each eye sees)
            auto towards = [&](int eye) {
                if (!(renderTestDistance > 0)) return direction;
                const float n = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
                return std::array<float, 3>{{direction[0] / n * renderTestDistance - eyeX[eye],
                                             direction[1] / n * renderTestDistance, direction[2] / n * renderTestDistance}};
            };
            const auto left = directionToNdc(towards(0), views[0]), right = directionToNdc(towards(1), views[1]);
            if (left && right) {
                ndc[0] = {{left->u, left->v}};
                ndc[1] = {{right->u, right->v}};
                tracked = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else if (gridFailed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } else if (eyeServer.isOpen()) {
            const auto eyes = eyeServer.next(20);
            if (eyes) {
                // The fit measures the fused gaze as the tracker gives it (frameeyeosc averages the tracker's gaze)
                if (fitRun && fitRun->measuring) fitRun->measure.add(gazeAngles(eyes->fixation), eyes->openness);
                // frameeyeosc's gaze path (GazeSmoother): one gaze for both eyes, the fused fixation point's
                // direction from the head, smoothed and held through blinks. Both eyes then look along the same
                // direction, as frameeyeosc sends it (each eye's own gaze is too unsteady to use)
                const auto gaze = smoother.step({eyes->sampleTime, eyes->fixation, eyes->gaze, eyes->openness});
                if (gaze) {
                    const auto direction = anglesToDirection((*gaze)[0], (*gaze)[1]);
                    const auto left = directionToNdc(direction, views[0]), right = directionToNdc(direction, views[1]);
                    if (left && right) {
                        ndc[0] = {{left->u, left->v}};
                        ndc[1] = {{right->u, right->v}};
                        tracked = true;
                    }
                }
                if (timedSamples < 90 && eyes->sampleTime > 0) {  // check once that sample_time is in seconds
                    if (timedSamples == 0) firstSampleTime = eyes->sampleTime;
                    if (++timedSamples == 90)
                        std::fprintf(stderr, "[gaze] eye-server sample interval %.2f ms (sample_time units)\n",
                                     (eyes->sampleTime - firstSampleTime) / 89.0 * 1000.0);
                }
                lastEyeSample = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - lastEyeSample > std::chrono::milliseconds(150)) {
                tracked = false;  // no sample for a while (headset off, tracker restarting): the lens centre
            } else {
                continue;  // between samples: keep the last point, the heartbeat rewrites it
            }
        } else {
            tracked = vr::VRSystem()->GetEyeTrackedFoveationCenter(&ndc[0], &ndc[1]);
        }
        const auto now = std::chrono::steady_clock::now();
        const bool changed = tracked != lastValid || ndc[0].v[0] != last[0].v[0] || ndc[0].v[1] != last[0].v[1] ||
                             ndc[1].v[0] != last[1].v[0] || ndc[1].v[1] != last[1].v[1];
        if (changed || now - lastWrite >= kHeartbeat) {
            framegaze::Sample sample;
            if (tracked) {
                const auto left = grids[0].inverse(ndcToRenderUv(ndc[0].v[0], ndc[0].v[1]));
                const auto right = grids[1].inverse(ndcToRenderUv(ndc[1].v[0], ndc[1].v[1]));
                // An eye whose point is off its panel (near the edge) keeps the lens centre; the other still follows.
                // The image the layer gets is ComputeDistortion's panel UV turned 180 degrees in each eye's half
                // (2026-11-08 on the headset: a ring at panel (0.25, 0.5) showed on the right, (0.5, 0.25) below; the
                // left half still goes to the left eye)
                if (left || right) {
                    sample.valid = true;
                    const Uv l = left ? Uv{1.0f - left->u, 1.0f - left->v} : Uv{}, r = right ? Uv{1.0f - right->u, 1.0f - right->v} : Uv{};
                    if (renderThis && !renderTestLogged) {
                        // The path's own UV for this direction (before the drawing fit): what the fit is measured on
                        std::fprintf(stderr, "[gaze] render test path uv L %.5f %.5f R %.5f %.5f\n", l.u, l.v, r.u, r.v);
                        renderTestLogged = true;
                    }
                    // A probe measurement draws nothing of its own (the probe dot alone is measured)
                    const bool probe = renderThis && renderTestDistance > 0 && !renderTestVerify;
                    // An eye whose fitted point leaves its panel keeps the lens centre (the layer drops a sample
                    // with any point outside 0..1, which would stop both eyes)
                    const auto inside = [](Uv p) { return p.u >= 0.0f && p.u <= 1.0f && p.v >= 0.0f && p.v <= 1.0f; };
                    if (left && !probe) {
                        const Uv f = applyDrawFit(drawFit, 0, l);
                        if (inside(f)) { sample.leftU = f.u; sample.leftV = f.v; }
                    }
                    if (right && !probe) {
                        const Uv f = applyDrawFit(drawFit, 1, r);
                        if (inside(f)) { sample.rightU = f.u; sample.rightV = f.v; }
                    }
                    if (probe) sample.valid = false;
                }
            }
            sample.writtenNs = monotonicNs();
            framegaze::write(*record_, sample);
            last[0] = ndc[0]; last[1] = ndc[1]; lastValid = tracked; lastWrite = now;
        }
        if (!eyeServer.isOpen()) std::this_thread::sleep_for(std::chrono::milliseconds(2));  // eye-server waits itself
    }
}

}  // namespace framecolor
