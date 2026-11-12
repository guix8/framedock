#pragma once
// Gaze-following sharpening (2026-11-08): from a gaze direction or SteamVR's eye-tracked foveation centre (projection
// NDC per eye) to the per-eye panel UV of the image the Vulkan layer sees (after lens distortion). Pure math, no OpenVR, so it is
// tested off the headset. The distortion is sampled once (IVRSystem::ComputeDistortion, panel UV -> render UV) into a
// grid; a gaze sample is inverted against that grid with Newton steps, without further SteamVR calls.
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "gaze_fit.h"

namespace framecolor {

struct Uv {
    float u = 0.5f, v = 0.5f;
};

/** Projection NDC (x right, y up, -1..1) -> render-target UV (0..1, v down). */
Uv ndcToRenderUv(float x, float y);

/** One eye's view: IVRSystem::GetProjectionRaw tangents and the rotation of GetEyeToHeadTransform (eye -> head). */
struct EyeView {
    float left = -1, right = 1, top = -1, bottom = 1;
    std::array<std::array<float, 3>, 3> eyeToHead{{{{1, 0, 0}}, {{0, 1, 0}}, {{0, 0, 1}}}};
};

/**
 * frameeyeosc's gaze angles (https://github.com/konsti219/frameeyeosc, MIT, gaze_angles): a head-relative direction
 * (x right, y up, -Z forward) -> sideways and up/down angles, atan2(x, -z) and atan2(y, -z), in units of 45 degrees
 * (1.0 = 45), clamped to -1..1.
 */
std::array<float, 2> gazeAngles(const std::array<float, 3>& direction);
/** The head-relative direction (-Z forward) with those two angles (the inverse of gazeAngles inside -1..1). */
std::array<float, 3> anglesToDirection(float x, float y);

/**
 * 1-euro filter (Casiez et al., CHI 2012) for one coordinate, as frameeyeosc has it: smooths the tracker's jitter
 * while the eye rests and follows quickly when it moves (the cut-off rises with speed). Defaults: frameeyeosc's gaze
 * (--gaze-min-cutoff 0.3, --gaze-beta 1.5, --gaze-d-cutoff 0.5), for values in units of 45 degrees.
 */
class OneEuro {
public:
    OneEuro(float minCutoffHz = 0.3f, float beta = 1.5f, float derivativeCutoffHz = 0.5f)
        : minCutoff_(minCutoffHz), beta_(beta), derivativeCutoff_(derivativeCutoffHz) {}
    float filter(float value, float dt);
    void reset() { started_ = false; velocity_ = 0; }

private:
    float minCutoff_, beta_, derivativeCutoff_;
    bool started_ = false;
    float value_ = 0, velocity_ = 0;
};

/** frameeyeosc's backlash deadzone: the output moves only once the input is more than `width` away from it. */
class Deadzone {
public:
    explicit Deadzone(float width = 0.005f) : width_(width) {}
    float apply(float value);
    void reset() { started_ = false; }

private:
    float width_, value_ = 0;
    bool started_ = false;
};

/** One eye-server sample, as the gaze smoother takes it. */
struct GazeReading {
    double time = 0;                        // the record's sample_time (seconds)
    std::array<float, 3> fixation{};        // fused fixation point, head-relative, -Z forward
    std::array<std::array<float, 3>, 2> gaze{{{{0, 0, -1}}, {{0, 0, -1}}}};  // left, right eye directions (-Z forward)
    std::array<float, 2> openness{{1, 1}};  // left, right (Frame openness 0..1)
};

/**
 * frameeyeosc's gaze path (src/main.rs, process and Smoother, default settings, no eye fit), one gaze shared by both
 * eyes: the angles of the fused fixation point (each eye's own gaze wobbles on its own: L/R changes correlate ~0.35),
 * the sideways angle faded to its last value from 24 to 34 degrees down (the Frame's x jumps there), a 3-sample median
 * (drops one-sample dropouts), held while either eye reads below 0.5 open (the gaze jumps as the eyes reopen; an eye
 * read alone below it for over a second is taken as a wrong reading instead, and the other eye's own gaze stands in
 * for the fused one), else 1-euro and a 0.005 (0.225 deg) deadzone. A gap over 250 ms between samples starts it all
 * over. A sample with anything not finite is skipped (frameeyeosc: data.is_finite()).
 */
class GazeSmoother {
public:
    /** @return the gaze to show (gazeAngles units), or nothing before the first usable sample */
    std::optional<std::array<float, 2>> step(const GazeReading& reading);
    void reset();
    /** The eye fit applied to every sample from now on (frameeyeosc correct_gaze, after hold_down_x). */
    void setFit(const GazeFit& fit) { fit_ = fit; }

private:
    GazeFit fit_;
    std::optional<double> lastTime_, lowTime_;
    std::optional<std::array<float, 3>> downHoldX_;  // left, right, fused
    std::array<std::optional<std::array<float, 8>>, 2> recent_{};
    std::array<double, 2> alone_{};
    std::array<int, 2> up_{};
    std::optional<std::array<float, 2>> last_;
    OneEuro filters_[2];
    Deadzone deadzones_[2];
};

/**
 * A head-relative gaze direction (x right, y up, -Z forward, as the eye-server record gives it) -> that eye's projection
 * NDC, with GetEyeTrackedFoveationCenter's convention (2026-11-08 on the headset: straight ahead came back as
 * -(l+r)/(r-l), -(t+b)/(b-t)). @return nothing for a direction not in front of the eye
 */
std::optional<Uv> directionToNdc(const std::array<float, 3>& head, const EyeView& view);

/**
 * Drawing fit (2026-11-10): the panel UV this file's path gives for a direction, mapped onto where SteamVR itself
 * draws that direction (measured with head-locked dots on the raw panels, analysis gaze_draw_fit.py), per eye a
 * quadratic in (u, v): u' = c0 + c1 u + c2 v + c3 u^2 + c4 u v + c5 v^2, v' likewise with c6..c11. Measured because
 * the path (projection, ComputeDistortion inverse, the 180 degree turn) put the sharpening centre about 5 deg off.
 */
struct DrawFit {
    bool valid = false;
    std::array<std::array<float, 12>, 2> c{};
};
Uv applyDrawFit(const DrawFit& fit, int eye, Uv uv);
/** gaze-draw-fit.txt: "v1" then one line of 12 numbers per eye (left, right). Missing or broken: not valid. */
DrawFit loadDrawFit(const std::string& file);
/**
 * The drawing fit measured on a Steam Frame on 2026-11-10 (35 directions per eye, 2.8 px mean after the fit, verified
 * at 10-13 px), used when there is no gaze-draw-fit.txt: the error it takes out comes from this path, not from one
 * headset. A file measured on a headset replaces it.
 */
DrawFit builtInDrawFit();

class DistortionGrid {
public:
    /** Samples per side; the edges of the panel are included. */
    static constexpr int kSize = 33;
    /** @param forward panel UV -> render UV for one eye (ComputeDistortion's green channel); false: not available */
    bool build(const std::function<bool(float, float, Uv&)>& forward);
    bool ready() const { return ready_; }
    /** The forward map interpolated from the grid (panel UV in 0..1). */
    Uv forward(Uv panel) const;
    /** @return the panel UV whose render UV is `render`, or nothing if it is outside the panel or doesn't converge */
    std::optional<Uv> inverse(Uv render) const;

private:
    std::vector<Uv> samples_;  // kSize * kSize, row j (v) major
    bool ready_ = false;
};

}  // namespace framecolor
