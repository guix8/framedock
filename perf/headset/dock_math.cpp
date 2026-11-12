// The Dock's geometry. See dock_math.h.
#include "dock_math.h"

#include "placement.h"

#include <algorithm>
#include <cmath>

namespace dockmath {

namespace {

constexpr double kDeg = M_PI / 180.0;

double roundMm(double meters) {
    return std::round(meters * 1000.0) / 1000.0 + 0.0;  // + 0.0: no "-0" in the file
}

double roundTenth(double degrees) {
    return std::round(degrees * 10.0) / 10.0 + 0.0;
}

/** Wraps an angle into (−180, 180]. */
double wrapDegrees(double degrees) {
    while (degrees > 180.0) degrees -= 360.0;
    while (degrees <= -180.0) degrees += 360.0;
    return degrees;
}

void rotationOf(const Mat34& m, double r[3][3]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) r[i][j] = m.m[i][j];
    }
}

}  // namespace

Mat34 identity() {
    Mat34 m {};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f;
    return m;
}

Mat34 inverseRigid(const Mat34& m) {
    Mat34 out {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out.m[i][j] = m.m[j][i];
    }
    for (int i = 0; i < 3; ++i) {
        double t = 0;
        for (int k = 0; k < 3; ++k) t -= static_cast<double>(m.m[k][i]) * m.m[k][3];
        out.m[i][3] = static_cast<float>(t);
    }
    return out;
}

Mat34 multiply(const Mat34& a, const Mat34& b) {
    return composeTransform(a, b);
}

Mat34 headTransform(const HeadPose& pose) {
    double r[3][3];
    panelRotation(pose.yaw, pose.pitch, pose.roll, r);
    Mat34 m {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m.m[i][j] = static_cast<float>(r[i][j]);
    }
    m.m[0][3] = static_cast<float>(pose.x);
    m.m[1][3] = static_cast<float>(pose.y);
    m.m[2][3] = static_cast<float>(pose.z);
    return m;
}

void yawPitchRoll(const double r[3][3], double& yawDeg, double& pitchDeg, double& rollDeg) {
    // panelRotation: third column (sy·cp, sp, cy·cp), second row (cp·sr, cp·cr, sp)
    const double sp = std::clamp(r[1][2], -1.0, 1.0);
    pitchDeg = std::asin(sp) / kDeg;
    const double cp = std::sqrt(std::max(0.0, 1.0 - sp * sp));
    if (cp > 1e-6) {
        yawDeg = std::atan2(r[0][2], r[2][2]) / kDeg;
        rollDeg = std::atan2(r[1][0], r[1][1]) / kDeg;
    } else {
        // Facing straight up or down: roll and yaw turn about the same axis. Keep roll 0, give yaw the turn
        // (first column with roll 0: (cy, 0, −sy))
        rollDeg = 0.0;
        yawDeg = std::atan2(-r[2][0], r[0][0]) / kDeg;
    }
}

HeadPose headPoseFrom(const Mat34& local) {
    double r[3][3];
    rotationOf(local, r);
    HeadPose pose;
    yawPitchRoll(r, pose.yaw, pose.pitch, pose.roll);
    pose.yaw = roundTenth(wrapDegrees(pose.yaw));
    pose.pitch = roundTenth(std::clamp(pose.pitch, -90.0, 90.0));
    pose.roll = roundTenth(wrapDegrees(pose.roll));
    pose.x = roundMm(std::clamp<double>(local.m[0][3], -5.0, 5.0));
    pose.y = roundMm(std::clamp<double>(local.m[1][3], -5.0, 5.0));
    pose.z = roundMm(std::clamp<double>(local.m[2][3], -5.0, 5.0));
    return pose;
}

WristPose wristPoseFrom(const Mat34& local) {
    // wristTransform: M = Rx(−90°) · panelRotation(yaw, pitch + 90, roll). Undo the base: P = Rx(90°) · M, whose rows
    // are M's first row, minus its third, and its second
    double p[3][3];
    for (int j = 0; j < 3; ++j) {
        p[0][j] = local.m[0][j];
        p[1][j] = -static_cast<double>(local.m[2][j]);
        p[2][j] = local.m[1][j];
    }
    double yaw = 0, pitch = 0, roll = 0;
    yawPitchRoll(p, yaw, pitch, roll);
    WristPose pose;
    pose.yaw = roundTenth(wrapDegrees(yaw));
    pose.pitch = roundTenth(std::clamp(pitch - 90.0, -180.0, 0.0));
    pose.roll = roundTenth(wrapDegrees(roll));
    pose.x = roundMm(std::clamp<double>(local.m[0][3], -0.5, 0.5));
    pose.y = roundMm(std::clamp<double>(local.m[1][3], -0.5, 0.5));
    pose.z = roundMm(std::clamp<double>(local.m[2][3], -0.5, 0.5));
    return pose;
}

void setHudPlacementFrom(const Mat34& local, Config& config) {
    if (config.attachment != Attachment::Head) {
        selectedWrist(config) = wristPoseFrom(local);
        return;
    }
    const HeadPose pose = headPoseFrom(local);
    config.posX = pose.x;
    config.posY = pose.y;
    config.posZ = pose.z;
    config.yawDeg = pose.yaw;
    config.pitchDeg = pose.pitch;
    config.rollDeg = pose.roll;
}

Ray rayFromPose(const Mat34& pose) {
    Ray ray;
    double length = 0;
    for (int i = 0; i < 3; ++i) {
        ray.origin[i] = pose.m[i][3];
        ray.dir[i] = -static_cast<double>(pose.m[i][2]);
        length += ray.dir[i] * ray.dir[i];
    }
    length = std::sqrt(length);
    if (length > 1e-9) {
        for (double& d : ray.dir) d /= length;
    }
    return ray;
}

PanelHit hitPanel(const Mat34& panel, const Ray& ray) {
    // Into the panel's frame: o' = Rᵀ(o − t), d' = Rᵀd; then the plane z' = 0
    double o[3], d[3];
    for (int i = 0; i < 3; ++i) {
        o[i] = 0;
        d[i] = 0;
        for (int k = 0; k < 3; ++k) {
            o[i] += static_cast<double>(panel.m[k][i]) * (ray.origin[k] - panel.m[k][3]);
            d[i] += static_cast<double>(panel.m[k][i]) * ray.dir[k];
        }
    }
    PanelHit hit;
    if (std::fabs(d[2]) < 1e-9) return hit;
    const double t = -o[2] / d[2];
    if (!(t > 0) || !std::isfinite(t)) return hit;
    hit.hit = true;
    hit.u = o[0] + d[0] * t;
    hit.v = o[1] + d[1] * t;
    hit.distance = t;
    return hit;
}

bool onPanel(const PanelHit& hit, double width, double height, double margin) {
    return hit.hit && std::fabs(hit.u) <= width / 2 + margin && std::fabs(hit.v) <= height / 2 + margin;
}

bool hitPixels(const PanelHit& hit, double width, double height, int textureWidth, int textureHeight, double& x,
               double& y) {
    if (!hit.hit || width <= 0 || height <= 0) return false;
    x = (hit.u / width + 0.5) * textureWidth;
    y = (0.5 - hit.v / height) * textureHeight;
    return x >= 0 && y >= 0 && x <= textureWidth && y <= textureHeight;
}

Mat34 grabOffset(const Mat34& controllerWorld, const Mat34& panelWorld) {
    return multiply(inverseRigid(controllerWorld), panelWorld);
}

Mat34 localAfterGrab(const Mat34& anchorWorld, const Mat34& controllerWorld, const Mat34& grab) {
    return multiply(inverseRigid(anchorWorld), multiply(controllerWorld, grab));
}

double distance(const Mat34& a, const Mat34& b) {
    double sum = 0;
    for (int i = 0; i < 3; ++i) {
        const double d = static_cast<double>(a.m[i][3]) - b.m[i][3];
        sum += d * d;
    }
    return std::sqrt(sum);
}

double angleBetweenDeg(const Mat34& a, const Mat34& b) {
    // The rotation from a to b, R = Raᵀ·Rb: cos θ from its trace, sin θ from its skew part (atan2 keeps small angles
    // precise, where acos of a float trace would not be)
    double r[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[i][j] = 0;
            for (int k = 0; k < 3; ++k) r[i][j] += static_cast<double>(a.m[k][i]) * b.m[k][j];
        }
    }
    const double c = (r[0][0] + r[1][1] + r[2][2] - 1.0) / 2.0;
    const double sx = (r[2][1] - r[1][2]) / 2.0, sy = (r[0][2] - r[2][0]) / 2.0, sz = (r[1][0] - r[0][1]) / 2.0;
    return std::atan2(std::sqrt(sx * sx + sy * sy + sz * sz), c) / kDeg;
}

bool movedByGrab(const Mat34& before, const Mat34& after) {
    return distance(before, after) >= kGrabDeadZoneM || angleBetweenDeg(before, after) >= kGrabDeadZoneDeg;
}

}  // namespace dockmath
