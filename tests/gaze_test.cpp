// Gaze-following sharpening, off the headset: the distortion grid inversion (gaze_map) and the shared record the
// layer reads (frame_gaze.hpp).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../third_party/vkbasalt/src/frame_gaze.hpp"
#include "gaze_map.h"

namespace {
int failures = 0, checks = 0;
#define CHECK(condition)                                                              \
    do {                                                                              \
        ++checks;                                                                     \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                             \
    } while (0)

// A barrel-like map shaped like the Steam Frame's ComputeDistortion (panel centre -> render centre, the render
// coordinate grows faster towards the edges; 2026-11-08 probe: panel 0.25 -> render ~0.32 on the centre row)
bool barrel(float u, float v, framecolor::Uv& out) {
    const float x = u - 0.5f, y = v - 0.5f, r2 = x * x + y * y;
    const float k = 1.0f + 1.4f * r2;
    out = {0.5f + x * k, 0.5f + y * k};
    return true;
}
bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }
}  // namespace

int main() {
    using framecolor::DistortionGrid;
    using framecolor::Uv;

    // NDC to render UV: x right, y up -> u right, v down
    {
        const Uv centre = framecolor::ndcToRenderUv(0, 0);
        CHECK(near(centre.u, 0.5f, 1e-6f) && near(centre.v, 0.5f, 1e-6f));
        const Uv upperLeft = framecolor::ndcToRenderUv(-1, 1);
        CHECK(near(upperLeft.u, 0.0f, 1e-6f) && near(upperLeft.v, 0.0f, 1e-6f));
    }

    // Gaze direction -> projection NDC, with the Steam Frame's left-eye projection (2026-11-08 probe)
    {
        framecolor::EyeView left;
        left.left = -1.677f; left.right = 1.206f; left.top = -1.727f; left.bottom = 1.176f;
        const auto ahead = framecolor::directionToNdc({{0, 0, -1}}, left);
        // GetEyeTrackedFoveationCenter gave (0.1634, 0.1898) for this eye when it had no gaze (straight ahead)
        CHECK(ahead && near(ahead->u, 0.1634f, 2e-3f) && near(ahead->v, 0.1898f, 2e-3f));
        const auto right = framecolor::directionToNdc({{0.5f, 0, -1}}, left), up = framecolor::directionToNdc({{0, 0.5f, -1}}, left);
        CHECK(right && right->u > ahead->u && near(right->v, ahead->v, 1e-5f));  // looking right: x grows
        CHECK(up && up->v > ahead->v && near(up->u, ahead->u, 1e-5f));          // looking up: y grows
        CHECK(!framecolor::directionToNdc({{0, 0, 1}}, left));                  // behind the eye
        CHECK(!framecolor::directionToNdc({{NAN, 0, -1}}, left));
        framecolor::EyeView turned = left;  // an eye turned 90 degrees to the right: head forward is its left
        turned.eyeToHead = {{{{0, 0, -1}}, {{0, 1, 0}}, {{1, 0, 0}}}};
        const auto fromTurned = framecolor::directionToNdc({{1, 0, -1}}, turned);  // 45 deg right of head = its forward-ish
        CHECK(fromTurned.has_value());
    }

    // frameeyeosc's gaze angles and their inverse
    {
        const auto ahead = framecolor::gazeAngles({{0, 0, -2}});
        CHECK(near(ahead[0], 0, 1e-6f) && near(ahead[1], 0, 1e-6f));
        const auto rightUp = framecolor::gazeAngles({{1, -1, -1}});  // 45 deg right, 45 deg down
        CHECK(near(rightUp[0], 1, 1e-5f) && near(rightUp[1], -1, 1e-5f));
        const auto back = framecolor::gazeAngles(framecolor::anglesToDirection(0.3f, -0.2f));
        CHECK(near(back[0], 0.3f, 1e-5f) && near(back[1], -0.2f, 1e-5f));
    }

    // 1-euro (frameeyeosc's gaze defaults) and the deadzone
    {
        framecolor::OneEuro filter;
        const float dt = 1.0f / 90.0f;
        CHECK(near(filter.filter(0.2f, dt), 0.2f, 1e-6f));
        float resting = 0.2f;
        for (int i = 1; i <= 90; ++i) resting = filter.filter(i % 2 ? 0.21f : 0.19f, dt);  // jitter at rest
        CHECK(std::fabs(resting - 0.2f) < 0.002f);  // almost all of the +-0.01 jitter is gone
        float moved = resting;
        for (int i = 1; i <= 18; ++i) moved = filter.filter(0.6f, dt);  // a saccade: 200 ms later
        CHECK(moved > 0.45f);  // it has caught up most of the way
        framecolor::Deadzone deadzone;
        CHECK(near(deadzone.apply(0.1f), 0.1f, 1e-6f));
        CHECK(near(deadzone.apply(0.103f), 0.1f, 1e-6f));    // within 0.005: stays
        CHECK(near(deadzone.apply(0.12f), 0.115f, 1e-6f));   // beyond: follows, 0.005 behind
    }

    // The gaze path: blinks hold, dropouts vanish, a gap starts over, far down x holds, a wrong one-eye reading
    {
        const auto at = [](double t, float x, float y, float l = 1, float r = 1) {
            const auto d = framecolor::anglesToDirection(x, y);
            return framecolor::GazeReading{t, d, {{d, d}}, {{l, r}}};
        };
        framecolor::GazeSmoother smoother;
        double t = 10;
        std::optional<std::array<float, 2>> out;
        for (int i = 0; i < 90; ++i, t += 1 / 90.0) out = smoother.step(at(t, 0.2f, 0.1f));
        CHECK(out && near((*out)[0], 0.2f, 1e-4f) && near((*out)[1], 0.1f, 1e-4f));
        // A blink with the gaze jumping meanwhile: held where it was
        for (int i = 0; i < 20; ++i, t += 1 / 90.0) out = smoother.step(at(t, -0.5f, 0.6f, 0.1f, 0.1f));
        CHECK(out && near((*out)[0], 0.2f, 1e-4f) && near((*out)[1], 0.1f, 1e-4f));
        // A one-sample dropout of the gaze: the median drops it
        out = smoother.step(at(t, 0.2f, 0.1f)); t += 1 / 90.0;
        out = smoother.step(at(t, 0.2f, 0.1f)); t += 1 / 90.0;
        out = smoother.step(at(t, 0.9f, -0.9f)); t += 1 / 90.0;
        CHECK(out && near((*out)[0], 0.2f, 2e-3f) && near((*out)[1], 0.1f, 2e-3f));
        // A gap over 250 ms: starts over at the new gaze
        t += 1.0;
        out = smoother.step(at(t, -0.4f, 0.0f));
        CHECK(out && near((*out)[0], -0.4f, 1e-5f) && near((*out)[1], 0.0f, 1e-5f));
        // 40 degrees down (past 24 + 10): x is the one from before looking down, whatever the tracker says now (y
        // stops one deadzone width, 0.005, short of a target it falls towards)
        for (int i = 0; i < 180; ++i, t += 1 / 90.0) out = smoother.step(at(t, 0.5f, -40.0f / 45.0f));
        CHECK(out && near((*out)[0], -0.4f, 2e-3f) && near((*out)[1], -40.0f / 45.0f + 0.005f, 1e-3f));
        // Starting that low after a gap: x is straight ahead, not the one from before the gap
        t += 1.0;
        for (int i = 0; i < 90; ++i, t += 1 / 90.0) out = smoother.step(at(t, 0.5f, -40.0f / 45.0f));
        CHECK(out && near((*out)[0], 0.0f, 2e-3f));
        // One eye read shut alone for over a second is taken as wrong: the gaze moves again
        smoother.reset();
        for (int i = 0; i < 180; ++i, t += 1 / 90.0) out = smoother.step(at(t, i < 90 ? 0.0f : 0.3f, 0.0f, 0.0f, 1.0f));
        CHECK(out && (*out)[0] > 0.25f);
        // ... and the other eye's own gaze stands in for the fused one
        smoother.reset();
        for (int i = 0; i < 270; ++i, t += 1 / 90.0) {
            auto reading = at(t, 0.0f, 0.0f, 0.0f, 1.0f);
            reading.gaze[1] = framecolor::anglesToDirection(-0.3f, 0.1f);
            out = smoother.step(reading);
        }
        CHECK(out && near((*out)[0], -0.3f, 0.01f) && near((*out)[1], 0.1f, 0.01f));
        // Anything not finite: skipped
        CHECK(!smoother.step({t, {{NAN, 0, -1}}, {{{{0, 0, -1}}, {{0, 0, -1}}}}, {{1, 1}}}));
        CHECK(!smoother.step({t, {{0, 0, -1}}, {{{{0, 0, -1}}, {{0, 0, -1}}}}, {{NAN, 1}}}));
    }

    // Grid build and inversion
    {
        DistortionGrid grid;
        CHECK(!grid.ready() && !grid.inverse({0.5f, 0.5f}));
        CHECK(!grid.build([](float, float, Uv&) { return false; }) && !grid.ready());  // no distortion data
        CHECK(grid.build(barrel) && grid.ready());
        for (float u = 0.15f; u <= 0.86f; u += 0.07f) {
            for (float v = 0.15f; v <= 0.86f; v += 0.07f) {
                Uv render;
                barrel(u, v, render);
                const auto panel = grid.inverse(render);
                CHECK(panel.has_value());
                if (panel) CHECK(near(panel->u, u, 3e-3f) && near(panel->v, v, 3e-3f));
            }
        }
        const auto centre = grid.inverse({0.5f, 0.5f});
        CHECK(centre && near(centre->u, 0.5f, 1e-3f) && near(centre->v, 0.5f, 1e-3f));
        // A render point no panel pixel shows (beyond the barrel's reach at the panel edge) has no panel point
        Uv corner;
        barrel(1.0f, 1.0f, corner);
        CHECK(!grid.inverse({corner.u + 0.2f, corner.v + 0.2f}));
        CHECK(!grid.inverse({NAN, 0.5f}));
    }

    // The shared record
    {
        framegaze::Record record{};
        framegaze::Sample out;
        CHECK(!framegaze::read(record, out));  // not initialised: no magic
        framegaze::initialize(record);
        CHECK(framegaze::read(record, out) && !out.valid);
        framegaze::Sample in;
        in.valid = true;
        in.leftU = 0.4f; in.leftV = 0.45f; in.rightU = 0.6f; in.rightV = 0.55f; in.writtenNs = 123;
        framegaze::write(record, in);
        CHECK(record.sequence.load() == 2);
        CHECK(framegaze::read(record, out) && out.valid && near(out.leftU, 0.4f, 1e-6f) && near(out.rightV, 0.55f, 1e-6f) &&
              out.writtenNs == 123);
        // A write in progress (odd sequence) is not read; the previous sample stays
        record.sequence.store(3);
        framegaze::Sample kept = out;
        CHECK(!framegaze::read(record, kept) && kept.valid && near(kept.leftU, 0.4f, 1e-6f));
        record.sequence.store(4);
        // A point outside an eye's half is not used
        in.leftU = 1.5f;
        framegaze::write(record, in);
        CHECK(framegaze::read(record, out) && !out.valid);
        // A writer that died mid-write left the sequence odd: recover() makes it readable again
        record.sequence.store(7);
        CHECK(!framegaze::read(record, out));
        framegaze::recover(record);
        CHECK(record.sequence.load() == 8);
        in.leftU = 0.3f;
        framegaze::write(record, in);
        CHECK(framegaze::read(record, out) && out.valid && near(out.leftU, 0.3f, 1e-6f));
        framegaze::recover(record);  // an even sequence stays
        CHECK(record.sequence.load() == 10);
        // Another format is ignored
        record.version = 99;
        CHECK(!framegaze::read(record, out));
    }

    // Gaze fit: dots (frameeyeosc's rules), the 9-dot grid fit, the correction, re-centre, the file
    {
        using framecolor::DotMeasure;
        const float u = 1.0f / 45.0f;  // a degree in gaze units
        const auto dot = [&](float xDeg, float yDeg, float jitterDeg) {
            DotMeasure m;
            for (int i = 0; i < 150; ++i)
                m.add({{(xDeg + (i % 2 ? jitterDeg : -jitterDeg)) * u, yDeg * u}}, {{0.9f, 0.9f}});
            return m.result();
        };
        const auto steady = dot(2, 3, 0.5f);
        CHECK(steady.ok && steady.usable == 150 && near(steady.mean[0], 2 * u, 1e-5f) && near(steady.spreadDeg, 0.5f, 1e-3f));
        CHECK(!dot(0, 0, 3.0f).ok);  // spread over 2.7 deg
        DotMeasure shut;  // eyes shut most of the time: too few usable samples
        for (int i = 0; i < 150; ++i) shut.add({{0, 0}}, {{i < 20 ? 0.9f : 0.1f, 0.9f}});
        CHECK(!shut.result().ok && shut.result().usable == 20);
        DotMeasure few;  // 12 usable of 15 came in: enough (60%, at least 12)
        for (int i = 0; i < 15; ++i) few.add({{0, 0}}, {{i < 12 ? 0.9f : 0.1f, 0.9f}});
        CHECK(few.result().ok);

        // A tracker like the 2026-11-09 one: straight ahead off by (1.4, 0.8), right reads 28% wide, up 20% short, a
        // slight tilt. Inside each quarter that is affine, so the grid fit takes it out exactly
        const auto tracker = [](float xDeg, float yDeg) {
            const float x = 1.4f + xDeg * (xDeg > 0 ? 1.28f : 1.03f) + 0.05f * yDeg;
            const float y = 0.8f + yDeg * (yDeg > 0 ? 0.8f : 1.0f) - 0.04f * xDeg;
            return std::array<float, 2>{{x, y}};
        };
        const auto& targets = framecolor::fullFitTargets();
        CHECK(targets.size() == 9 && targets[0].xDeg == 0 && targets[0].yDeg == 0);
        std::vector<framecolor::DotResult> dots;
        for (const auto& t : targets) {
            const auto r = tracker(t.xDeg, t.yDeg);
            dots.push_back(dot(r[0], r[1], 0.3f));
        }
        const auto fit = framecolor::fitFromDots(dots);
        CHECK(fit && fit->fitted);
        const auto seen = [&](const framecolor::GazeFit& f, float xDeg, float yDeg, float shiftX = 0, float shiftY = 0) {
            const auto r = tracker(xDeg, yDeg);
            const auto c = framecolor::correctGaze({{(r[0] + shiftX) * u, (r[1] + shiftY) * u}}, f);
            return std::array<float, 2>{{c[0] * 45.0f, c[1] * 45.0f}};
        };
        bool allDots = true;
        for (const auto& t : targets) {
            const auto c = seen(*fit, t.xDeg, t.yDeg);
            allDots = allDots && near(c[0], t.xDeg, 0.01f) && near(c[1], t.yDeg, 0.01f);
        }
        CHECK(allDots);  // every dot where it was shown
        const auto between = seen(*fit, 10, 7.5f), lowerLeft = seen(*fit, -12, -9);
        CHECK(near(between[0], 10, 0.05f) && near(between[1], 7.5f, 0.05f));
        CHECK(near(lowerLeft[0], -12, 0.05f) && near(lowerLeft[1], -9, 0.05f));
        const auto beyond = seen(*fit, 28, 4);  // past the grid: the edge quarter's map goes on
        CHECK(near(beyond[0], 28, 0.1f) && near(beyond[1], 4, 0.1f));
        // Not a fit: a dot not steady, or two dots read the wrong way round (one looked at wrongly)
        auto bad = dots;
        bad[3] = dot(0, 0, 3.0f);
        CHECK(!framecolor::fitFromDots(bad));
        bad = dots;
        std::swap(bad[3], bad[4]);
        CHECK(!framecolor::fitFromDots(bad));
        bad = dots;
        bad.pop_back();
        CHECK(!framecolor::fitFromDots(bad));
        // Put on again: the tracker's straight ahead moved (+2, -1.5); one dot there moves the whole grid with it
        const auto r0 = tracker(0, 0);
        const auto movedOr = framecolor::recenterFromDot(dot(r0[0] + 2, r0[1] - 1.5f, 0.3f), *fit);
        CHECK(movedOr.has_value());
        const framecolor::GazeFit moved = movedOr.value_or(*fit);
        CHECK(near(moved.shiftX, 2 * u, 1e-4f) && near(moved.shiftY, -1.5f * u, 1e-4f));
        CHECK(!framecolor::recenterFromDot(dot(r0[0] + 9, r0[1], 0.3f), *fit));  // 9 deg: looked at wrongly
        const auto afterRight = seen(moved, 20, 0, 2, -1.5f), afterBetween = seen(moved, -10, 7.5f, 2, -1.5f);
        CHECK(near(afterRight[0], 20, 0.02f) && near(afterRight[1], 0, 0.02f));
        CHECK(near(afterBetween[0], -10, 0.05f) && near(afterBetween[1], 7.5f, 0.05f));
        CHECK(!framecolor::recenterFromDot(dot(1, 1, 0.3f), framecolor::GazeFit{}));  // nothing to move
        // Past the grid diagonally (the 2026-11-09 review's cases): the right quarter, and no jumps from one quarter to
        // the next. A grid with real tilt and per-dot noise, then a sweep in small steps all around it
        std::vector<framecolor::DotResult> noisy;
        const float wobble[9][2] = {{0.6f, -0.4f}, {-0.8f, 0.5f}, {0.3f, 0.9f}, {-0.7f, -0.6f}, {0.9f, 0.2f},
                                    {-0.5f, 0.8f}, {0.4f, -0.9f}, {-0.9f, -0.3f}, {0.7f, 0.6f}};
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const auto r = tracker(targets[i].xDeg, targets[i].yDeg);
            noisy.push_back(dot(r[0] + wobble[i][0], r[1] + wobble[i][1], 0.3f));
        }
        const auto noisyFit = framecolor::fitFromDots(noisy);
        CHECK(noisyFit.has_value());
        if (noisyFit) {
            float worstJump = 0;
            bool signsRight = true;
            for (float y = -32; y <= 32; y += 4) {
                std::array<float, 2> previous{};
                for (float x = -36; x <= 36; x += 0.25f) {
                    const auto c = framecolor::correctGaze({{x * u, y * u}}, *noisyFit);
                    if (x > -36) worstJump = std::max(worstJump, std::hypot(c[0] - previous[0], c[1] - previous[1]) * 45.0f);
                    previous = c;
                    if (std::fabs(y) >= 20 && (c[1] > 0) != (y > 0)) signsRight = false;  // far up stays up, down down
                }
            }
            for (float x = -36; x <= 36; x += 4) {
                std::array<float, 2> previous{};
                for (float y = -32; y <= 32; y += 0.25f) {
                    const auto c = framecolor::correctGaze({{x * u, y * u}}, *noisyFit);
                    if (y > -32) worstJump = std::max(worstJump, std::hypot(c[0] - previous[0], c[1] - previous[1]) * 45.0f);
                    previous = c;
                }
            }
            CHECK(signsRight);
            CHECK(worstJump < 0.6f);  // a 0.25 deg step never moves the result much more than its own size
            const auto corner = framecolor::correctGaze({{-30.6f * u, 23.3f * u}}, *noisyFit);
            CHECK(corner[0] < 0 && corner[1] > 0);  // up-left stays up-left
        }
        // Noisier dots (up to 2.3 deg off, still steady): far past the grid the extended maps fold over; the result
        // still goes on without jumps and keeps its direction
        std::vector<framecolor::DotResult> rough;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const auto r = tracker(targets[i].xDeg, targets[i].yDeg);
            rough.push_back(dot(r[0] + 2.5f * wobble[i][0], r[1] + 2.5f * wobble[i][1], 0.3f));
        }
        const auto roughFit = framecolor::fitFromDots(rough);
        CHECK(roughFit.has_value());
        if (roughFit) {
            float worstJump = 0;
            bool directionKept = true;
            for (float angle = 0; angle < 360; angle += 3) {
                const float dx = std::cos(angle * 3.14159265f / 180), dy = std::sin(angle * 3.14159265f / 180);
                std::array<float, 2> previous{};
                for (float radius = 0; radius <= 50; radius += 0.25f) {
                    const auto c = framecolor::correctGaze({{(1.4f + radius * dx) * u, (0.8f + radius * dy) * u}}, *roughFit);
                    if (radius > 0) worstJump = std::max(worstJump, std::hypot(c[0] - previous[0], c[1] - previous[1]) * 45.0f);
                    previous = c;
                    if (radius >= 25 && std::fabs(dx) > 0.5f && (c[0] > 0) != (dx > 0)) directionKept = false;
                    if (radius >= 25 && std::fabs(dy) > 0.5f && (c[1] > 0) != (dy > 0)) directionKept = false;
                }
            }
            CHECK(worstJump < 1.0f);
            CHECK(directionKept);
        }
        // No fit: the gaze as it is
        const auto none = framecolor::correctGaze({{0.1f, -0.2f}}, framecolor::GazeFit{});
        CHECK(near(none[0], 0.1f, 1e-7f) && near(none[1], -0.2f, 1e-7f));
        CHECK(framecolor::describeDots(targets, dots, *fit).find("fitted off 0.0 deg") != std::string::npos);
        // The file
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "framedock-gaze-fit-test.txt";
        CHECK(framecolor::saveGazeFit(file, moved));
        const auto loaded = framecolor::loadGazeFit(file);
        CHECK(loaded.fitted && near(loaded.shiftX, moved.shiftX, 1e-6f) && near(loaded.measured[8][1], moved.measured[8][1], 1e-6f));
        { std::ofstream(file) << "-0.0255785 0.0517935 1.42154 1\n1.42154\n"; }  // the 5-dot fit from before
        CHECK(!framecolor::loadGazeFit(file).fitted);
        { std::ofstream(file) << "nonsense\n"; }
        CHECK(!framecolor::loadGazeFit(file).fitted);
        auto folded = *fit;
        std::swap(folded.measured[3], folded.measured[5]);
        CHECK(framecolor::saveGazeFit(file, folded) && !framecolor::loadGazeFit(file).fitted);  // not believed
        std::filesystem::remove(file);
        // Applied in the gaze path: the tracker's straight ahead reads as straight ahead
        framecolor::GazeSmoother smoother;
        smoother.setFit(*fit);
        std::optional<std::array<float, 2>> out;
        double t = 50;
        for (int i = 0; i < 90; ++i, t += 1 / 90.0) {
            const auto d = framecolor::anglesToDirection(r0[0] * u, r0[1] * u);
            out = smoother.step({t, d, {{d, d}}, {{1, 1}}});
        }
        CHECK(out && near((*out)[0], 0, 1e-3f) && near((*out)[1], 0, 1e-3f));
    }

    // Drawing fit: the built-in one, applied and read back from a file
    {
        const auto fit = framecolor::builtInDrawFit();
        CHECK(fit.valid);
        // 2026-11-10 measurement: the path's UV for straight ahead lay above and inside of SteamVR's own drawing
        const auto centre = framecolor::applyDrawFit(fit, 0, {0.5f, 0.5f});
        CHECK(std::fabs(centre.u - 0.5f) < 0.15f && std::fabs(centre.v - 0.5f) < 0.15f);
        CHECK(near(framecolor::applyDrawFit(framecolor::DrawFit{}, 1, {0.3f, 0.7f}).u, 0.3f, 1e-7f));  // none: as is
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "framedock-draw-fit-test.txt";
        {
            std::ofstream out(file);
            out << "v1\n";
            for (const auto& eye : fit.c) {
                for (float v : eye) out << v << " ";
                out << "\n";
            }
        }
        const auto loaded = framecolor::loadDrawFit(file.string());
        CHECK(loaded.valid && near(loaded.c[1][6], fit.c[1][6], 1e-5f));
        { std::ofstream(file) << "v1\n5 0 0 0 0 0 0 0 1 0 0 0\n0 1 0 0 0 0 0 0 1 0 0 0\n"; }  // centre moved far: no
        CHECK(!framecolor::loadDrawFit(file.string()).valid);
        { std::ofstream(file) << "nonsense\n"; }
        CHECK(!framecolor::loadDrawFit(file.string()).valid);
        std::filesystem::remove(file);
    }

    std::printf("gaze: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
