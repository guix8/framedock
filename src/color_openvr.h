// The Color page's OpenVR access in the running app (SteamVR settings, the scene app, PC streaming).
#pragma once

#include "color_model.h"

namespace framecolor {

/** ColorHost on Frame Dock's OpenVR connection. Every call checks that the interface is there. */
class OpenVrColorHost : public ColorHost {
public:
    double getSetting(const char* key, double fallback) override;
    std::string setSetting(const char* key, double value) override;
    DisplayContext context(bool withStreaming) override;
    std::string appName(const std::string& key) override;

private:
    StreamingProbe streaming_;  ///< vrlink, by its cached PID (a full /proc scan only every few seconds)
};

}  // namespace framecolor
