#pragma once
// Gaze-following sharpening (2026-11-08): a worker thread that, while enabled, reads SteamVR's eye-tracked
// foveation centre, maps it to the layer's per-eye panel UV (gaze_map) and publishes it in /dev/shm for the Vulkan
// layer (third_party/vkbasalt/src/frame_gaze.hpp). While enabled it raises steamvr/eyeTrackingRateMin to 90 Hz (the
// default 15 Hz gives a point every ~67 ms); it removes that key again when disabled, on exit, and at the next start
// after a crash (a marker file says the key is ours).
// Gaze fit (2026-11-09, gaze_fit.h): on request it shows dots fixed to the headset 0.9 m ahead (an overlay of its own,
// driven from this thread) and measures the gaze at each; a fit is saved to ~/.config/framecolor/gaze-fit.txt and
// applied. Once there is a fit, straight ahead is measured again (one dot) about 3 s after the headset is put on while
// the gaze is followed and the dashboard is closed.
// While a full fit runs and for 10 s after one is done, the sharpened area is tinted (the layer's
// debug view, a gaze-debug file this class makes and removes): drawn with the fit in use, so each dot shows how well
// that fit matches (2026-11-10, in place of a separate check: a second fit's tint should centre on each dot; its red centre dot is
// off unless ~/.config/framecolor/gaze-debug-dot exists).
// Render test (2026-11-10, for the drawing path alone, no eye tracking): while ~/.config/framecolor/gaze-render-test
// holds "x y" (degrees right and up from the head), the fit's dot is shown there and the sharpening centre is put on
// that same direction through the whole drawing path; with gaze-debug and gaze-debug-dot on, the red centre dot should sit on the dot.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

namespace framegaze { struct Record; }

namespace framecolor {

class GazeFeed {
public:
    /** @param configDir ~/.config/framecolor (the ownership marker lives there) */
    explicit GazeFeed(std::filesystem::path configDir);
    ~GazeFeed();
    GazeFeed(const GazeFeed&) = delete;
    GazeFeed& operator=(const GazeFeed&) = delete;
    /** Called by the main loop with ColorModel::gazeFollowActive(); cheap, the work is on the worker thread. */
    void setEnabled(bool enabled) { enabled_.store(enabled); }
    /** Stop the worker, mark the gaze invalid and give the eye tracking rate back (before VR_Shutdown). */
    void stop();
    /** Start a full gaze fit, or stop the one running. */
    void toggleFit() { fitRequest_.store(fitStep_.load() >= 0 ? kStopFit : kStartFit); }

    enum class FitOutcome { None, Done, Failed, Stopped };
    /** Why a fit failed (the page says it): no eye-server gaze, no distortion grid, a dot never steady, dots out of order. */
    enum class FitFailure { None, NoGaze, NoGrid, Unsteady, OutOfOrder };
    struct FitStatus {
        int step = -1, steps = 0;  // the dot being shown (from 0), -1: no fit running
        FitOutcome outcome = FitOutcome::None;
        bool fitted = false;       // a full fit is in use
        bool reviewing = false;    // a fit just done: the tint stays a few seconds to look around with it
        int reviewLeft = 0;        // ... this many seconds more (rounded up)
        FitFailure failure = FitFailure::None;  // with outcome Failed
    };
    FitStatus fitStatus() const {
        FitStatus status{fitStep_.load(), fitSteps_.load(), static_cast<FitOutcome>(fitOutcome_.load()), fitted_.load(),
                         reviewing_.load(), 0, static_cast<FitFailure>(fitFailure_.load())};
        if (status.reviewing) {
            const auto left = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(reviewEnd_.load())) -
                              std::chrono::steady_clock::now();
            status.reviewLeft = static_cast<int>(std::max<long long>(
                1, (std::chrono::duration_cast<std::chrono::milliseconds>(left).count() + 999) / 1000));
        }
        return status;
    }
    /** Forget the outcome shown (the UI shows it until then). */
    void clearFitOutcome() { fitOutcome_.store(static_cast<int>(FitOutcome::None)); }

private:
    void run();
    bool setRate(bool raised);
    void setTint(bool on);
    static constexpr int kNoRequest = 0, kStartFit = 1, kStopFit = 2;
    std::filesystem::path marker_, fitFile_, renderTestFile_, tintFile_;
    std::atomic<int> fitRequest_{kNoRequest}, fitStep_{-1}, fitSteps_{0}, fitOutcome_{0}, fitFailure_{0};
    std::atomic<std::chrono::steady_clock::rep> reviewEnd_{0};  // when the review ends (steady_clock ticks)
    std::atomic<bool> fitted_{false}, reviewing_{false};
    bool tintOwned_ = false;  // the gaze-debug file is ours (made for a fit: removed again)
    framegaze::Record* record_ = nullptr;
    std::atomic<bool> enabled_{false}, running_{false};
    bool rateRaised_ = false;
    std::thread worker_;
};

}  // namespace framecolor
