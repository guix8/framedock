#include "color_openvr.h"

#include "openvr.h"

#include <chrono>

namespace framecolor {

namespace {

// OpenVR: the app key of the process that owns the scene, or "" if it has none.
std::string currentAppKey() {
    vr::IVRCompositor* compositor = vr::VRCompositor();
    vr::IVRApplications* applications = vr::VRApplications();
    if (compositor == nullptr || applications == nullptr) return {};
    const uint32_t pid = compositor->GetCurrentSceneFocusProcess();
    if (pid == 0) return {};
    char key[vr::k_unMaxApplicationKeyLength] = {};
    if (applications->GetApplicationKeyByProcessId(pid, key, sizeof(key)) != vr::VRApplicationError_None) return {};
    return key;
}

}  // namespace

double OpenVrColorHost::getSetting(const char* key, double fallback) {
    if (vr::VRSettings() == nullptr) return fallback;
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const float value = vr::VRSettings()->GetFloat("steamvr", key, &error);
    return error == vr::VRSettingsError_None ? value : fallback;
}

std::string OpenVrColorHost::setSetting(const char* key, double value) {
    if (vr::VRSettings() == nullptr) return "no VRSettings";
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->SetFloat("steamvr", key, static_cast<float>(value), &error);
    if (error == vr::VRSettingsError_None) return {};
    return vr::VRSettings()->GetSettingsErrorNameFromEnum(error);
}

DisplayContext OpenVrColorHost::context(bool withStreaming) {
    // isStreaming() reads every process's cmdline in /proc: only when it is wanted
    // Cached: the vrlink found last is checked by its PID; /proc is scanned only every few seconds while none is
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return {withStreaming && streaming_.streaming(now), currentAppKey()};
}

std::string OpenVrColorHost::appName(const std::string& key) {
    if (key.empty() || vr::VRApplications() == nullptr) return {};
    char name[256] = {};
    vr::EVRApplicationError error = vr::VRApplicationError_None;
    vr::VRApplications()->GetApplicationPropertyString(key.c_str(), vr::VRApplicationProperty_Name_String, name,
                                                       sizeof(name), &error);
    return error == vr::VRApplicationError_None && name[0] != '\0' ? name : key;
}

}  // namespace framecolor
