#pragma once
// Gaze fit for gaze-following sharpening (2026-11-09). Dots on a 3 x 3 grid (20 deg left / right, 15 deg up / down,
// the corners too) fixed to the headset; each dot's gaze is measured as the tracker gives it, and the fit maps the
// tracker's gaze back onto the grid: inside each quarter, the four measured corners are mapped onto the four true ones
// (bilinear, so every dot comes out exactly where it was shown; beyond the grid, the edge quarter's map goes on, up
// to 1.5 quarters further). The fit is measured on the fused gaze; it is applied to each eye's gaze too (that only
// matters when one eye stands in for the fused gaze, see GazeSmoother).
// Why not frameeyeosc's eye fit alone (straight ahead + an up gain, https://github.com/sasaken1102r/frameeyeosc,
// docs/eye-fit.md, MIT): on 2026-11-09 one Steam Frame read 20 deg right as 25-26 deg three fits in a row while 20 deg
// left read about right, which no offset and up gain can take out. Once fitted, straight ahead is measured again each
// time the headset is put on (one dot) and the whole grid moves with it, frameeyeosc's re-centre.
// Dot measuring (DotMeasure) keeps frameeyeosc's rules. Pure logic, tested off the headset.
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace framecolor {

/** One dot of a fit, in degrees (x right, y up). */
struct GazeTarget {
    float xDeg = 0, yDeg = 0;
};

/** The fit: the tracker's gaze at each grid dot (gaze units, 1.0 = 45 deg), and the re-centre shift since. */
struct GazeFit {
    // Row major: rows 15, 0, -15 deg up; columns -20, 0, 20 deg right
    std::array<std::array<float, 2>, 9> measured{};
    float shiftX = 0, shiftY = 0;  // the tracker's straight ahead moved this much since the fit (re-centre)
    bool fitted = false;
};

/** The grid's true angles (degrees) of row r, column c. */
GazeTarget gridTarget(int row, int column);

/** The tracker's gaze -> where the eyes look, by the fit (unchanged without one). Clamped to -1..1. */
std::array<float, 2> correctGaze(const std::array<float, 2>& angles, const GazeFit& fit);

/** Straight ahead first, then up, down, left, right, and the corners (9 dots). */
const std::vector<GazeTarget>& fullFitTargets();
/** Straight ahead only (put on again). */
const std::vector<GazeTarget>& recenterTargets();

/** What one dot measured. */
struct DotResult {
    bool ok = false;
    int received = 0, usable = 0;
    std::array<float, 2> mean{};  // gaze units, the fused gaze as the tracker gives it (no fit)
    float spreadDeg = 0;          // RMS distance from the mean
};

/**
 * Collects one dot's samples while it is measured. A sample counts when both eyes read at least 0.5 open. The dot is
 * good when at least 60% of the samples that came in were usable (at least 12, never more than 45 needed) and the
 * spread is at most 2.7 deg (frameeyeosc's rules).
 */
class DotMeasure {
public:
    void add(const std::array<float, 2>& angles, const std::array<float, 2>& openness);
    DotResult result() const;
    void clear() { samples_.clear(); received_ = 0; }

private:
    std::vector<std::array<float, 2>> samples_;
    int received_ = 0;
};

/**
 * The fit from a full fit's dots (fullFitTargets order). Nothing if a dot was not steady or the measured grid is
 * folded (each row must read left to right and each column top to bottom, at least 5 deg apart, and every quarter
 * must keep its corners in order), which a dot looked at wrongly gives.
 */
std::optional<GazeFit> fitFromDots(const std::vector<DotResult>& dots);
/**
 * Straight ahead measured again: the grid moves with it. Nothing without a fit, for a dot not steady, or when straight
 * ahead moved more than 6 deg (a dot looked at wrongly: the fit stays as it was).
 */
std::optional<GazeFit> recenterFromDot(const DotResult& ahead, const GazeFit& before);

/** One line per dot: the measured angle and how far it is from the dot without and with `fit` (degrees). */
std::string describeDots(const std::vector<GazeTarget>& targets, const std::vector<DotResult>& dots, const GazeFit& fit);

/** gaze-fit.txt ("v2", the 9 measured points, the shift). A missing, old or broken file: no fit. */
GazeFit loadGazeFit(const std::filesystem::path& file);
bool saveGazeFit(const std::filesystem::path& file, const GazeFit& fit);

}  // namespace framecolor
