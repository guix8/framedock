// The Dock's geometry (in-game interaction): a laser ray against a panel, the grab that carries a panel rigidly with a
// controller, and turning a carried panel's pose back into the offsets config.json keeps (the HUD's head position /
// wrist pose, the Dock panel's own offsets). Pure math, no OpenVR calls, so the tests can run it.
//
// Ray against a flat overlay and the "the laser starts at the render model's tip" idea come from Frametop
// (https://github.com/Frametop/frametop, MIT, screens/vr.cpp RayOnSurface / LaserPose); see THIRD_PARTY_LICENSES.md.
#pragma once

#include "config.h"
#include "openvr.h"

namespace dockmath {

using Mat34 = vr::HmdMatrix34_t;

/** @return the identity pose */
Mat34 identity();

/**
 * Inverse of a rigid pose (rotation + translation): [Rᵀ | −Rᵀt].
 * @param m a pose whose 3×3 part is a rotation
 */
Mat34 inverseRigid(const Mat34& m);

/** @return a · b (both 3×4, the fourth row (0, 0, 0, 1) implied) */
Mat34 multiply(const Mat34& a, const Mat34& b);

/** A pose relative to the head: position (m, HMD axes) and the HUD's yaw / pitch / roll convention (panelRotation). */
Mat34 headTransform(const HeadPose& pose);

/**
 * Inverse of panelRotation: R = Ry(yaw) · Rx(−pitch) · Rz(roll) → the angles (degrees). Pitch comes out in [−90, 90];
 * at ±90 (gimbal lock) roll is 0 and yaw takes the whole turn about the vertical.
 */
void yawPitchRoll(const double r[3][3], double& yawDeg, double& pitchDeg, double& rollDeg);

/** A head-relative pose back to HeadPose (rounded to 1 mm and 0.1°, positions clamped to ±5 m like config.json). */
HeadPose headPoseFrom(const Mat34& local);

/**
 * A controller-relative pose back to WristPose, the inverse of wristTransform (rounded to 1 mm and 0.1°; positions
 * clamped to ±0.5 m, the range config.json allows).
 */
WristPose wristPoseFrom(const Mat34& local);

/**
 * Writes a HUD pose relative to its anchor into the config: position and rotation for the head, the selected wrist's
 * WristPose for a wrist. Everything else stays.
 * @param local the HUD's pose relative to the anchor (HMD or controller)
 * @param config the config whose attachment says which one
 */
void setHudPlacementFrom(const Mat34& local, Config& config);

/** A laser: origin and unit direction, standing-universe coordinates. */
struct Ray {
    double origin[3] = {0, 0, 0};
    double dir[3] = {0, 0, -1};
};

/** @return the ray along a pose's −z axis (SteamVR's forward), from its origin */
Ray rayFromPose(const Mat34& pose);

/** Where a ray meets a flat panel's plane. */
struct PanelHit {
    bool hit = false;     ///< the plane is ahead of the ray (not parallel, not behind)
    double u = 0, v = 0;  ///< metres from the panel's centre along its +x (right) and +y (up)
    double distance = 0;  ///< metres along the ray
};

/**
 * Intersects a ray with the plane of a panel (the panel's local z = 0; OpenVR overlays face +z). Either face counts.
 * @param panel the panel's pose in the same space as the ray
 */
PanelHit hitPanel(const Mat34& panel, const Ray& ray);

/**
 * @param width panel width (m)
 * @param height panel height (m): width × texture height / texture width (the mouse scale is the texture size)
 * @param margin extra metres on every side (a wider one to keep the laser than to take it)
 * @return true if the hit is on the panel (with the margin)
 */
bool onPanel(const PanelHit& hit, double width, double height, double margin);

/**
 * The hit in texture pixels (left-top origin), like SteamVR's mouse events with the mouse scale set to the texture.
 * @return false if it's off the texture
 */
bool hitPixels(const PanelHit& hit, double width, double height, int textureWidth, int textureHeight, double& x,
               double& y);

/**
 * The grab: a panel follows the grabbing controller rigidly. This is the panel's pose relative to the controller at
 * the moment of the grab (controller⁻¹ · panel); SteamVR then keeps it there itself
 * (SetOverlayTransformTrackedDeviceRelative), with no lag from us.
 */
Mat34 grabOffset(const Mat34& controllerWorld, const Mat34& panelWorld);

/**
 * Where a released panel sits relative to its anchor (the HMD or the anchor wrist's controller):
 * anchor⁻¹ · controller · grab, from the poses at the release.
 */
Mat34 localAfterGrab(const Mat34& anchorWorld, const Mat34& controllerWorld, const Mat34& grab);

/** @return the distance between two poses' origins (m) */
double distance(const Mat34& a, const Mat34& b);

/** @return the angle between two poses' rotations (degrees, 0–180) */
double angleBetweenDeg(const Mat34& a, const Mat34& b);

/** A grab released this close to where it started is no move (a slow click, a grip let go in place) */
constexpr double kGrabDeadZoneM = 0.005;
constexpr double kGrabDeadZoneDeg = 1.0;

/**
 * @param before the panel's pose in the room when the grab started
 * @param after its pose at the release
 * @return true if it moved kGrabDeadZoneM or more, or turned kGrabDeadZoneDeg or more
 */
bool movedByGrab(const Mat34& before, const Mat34& after);

}  // namespace dockmath

/** The poses the Dock needs, read with one GetDeviceToAbsoluteTrackingPose (standing universe). */
struct DockPoses {
    struct Device {
        uint32_t index = 0xffffffff;  ///< vr::k_unTrackedDeviceIndexInvalid when there's none
        bool valid = false;           ///< connected and its pose is valid
        dockmath::Mat34 pose {};
        dockmath::Mat34 laser {};     ///< where SteamVR's laser starts and points: the render model's tip
    };
    Device hmd, left, right;
    const Device& hand(bool leftHand) const { return leftHand ? left : right; }
    /** @return the device the HUD (and the Dock panel) hangs on: the HMD, or the wrist's controller */
    const Device& anchor(Attachment attachment) const {
        return attachment == Attachment::Head ? hmd : attachment == Attachment::LeftWrist ? left : right;
    }
};
