// SteamVR Input's action manifest for the Dock's grab. See dock_manifest.h.
#include "dock_manifest.h"

#include "atomic_file.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

namespace dockinput {

namespace {

/** A grip source for both hands: Frame and Index-style grips are analog triggers / force sensors with a click. */
std::string gripSources(const char* mode, const char* input) {
    std::ostringstream out;
    for (const char* hand : {"left", "right"}) {
        out << (hand[0] == 'r' ? ",\n" : "") << "        {\n"
            << "          \"path\": \"/user/hand/" << hand << "/input/grip\",\n"
            << "          \"mode\": \"" << mode << "\",\n"
            << "          \"inputs\": { \"" << input << "\": { \"output\": \"" << kGrabAction << "\" } }\n"
            << "        }";
    }
    return out.str();
}

std::string bindingFile(const char* controllerType, const char* mode, const char* input) {
    std::ostringstream out;
    out << "{\n"
        << "  \"action_manifest_version\": 0,\n"
        << "  \"controller_type\": \"" << controllerType << "\",\n"
        << "  \"description\": \"Frame Dock: hold the grip while pointing at a Frame Dock panel to move it\",\n"
        << "  \"name\": \"Frame Dock default\",\n"
        << "  \"bindings\": {\n"
        << "    \"" << kActionSet << "\": {\n"
        << "      \"sources\": [\n"
        << gripSources(mode, input) << "\n"
        << "      ]\n"
        << "    }\n"
        << "  }\n"
        << "}\n";
    return out.str();
}

struct Controller {
    const char* type;
    const char* mode;
    const char* input;
};

// The Frame's controllers first; the others so a PC headset gets a default too
constexpr Controller kControllers[] = {
    {"frame_controller", "trigger", "click"},
    {"knuckles", "force_sensor", "grab"},
    {"oculus_touch", "trigger", "click"},
    {"vive_controller", "button", "click"},
    {"generic", "button", "click"},
};

}  // namespace

std::vector<ManifestFile> manifestFiles() {
    std::vector<ManifestFile> files;
    std::ostringstream manifest;
    manifest << "{\n  \"default_bindings\": [\n";
    bool first = true;
    for (const Controller& c : kControllers) {
        manifest << (first ? "" : ",\n") << "    { \"controller_type\": \"" << c.type
                 << "\", \"binding_url\": \"bindings_" << c.type << ".json\" }";
        first = false;
        files.push_back({std::string("bindings_") + c.type + ".json", bindingFile(c.type, c.mode, c.input)});
    }
    manifest << "\n  ],\n"
             << "  \"actions\": [\n"
             << "    { \"name\": \"" << kGrabAction << "\", \"type\": \"boolean\", \"requirement\": \"optional\" }\n"
             << "  ],\n"
             << "  \"action_sets\": [\n"
             << "    { \"name\": \"" << kActionSet << "\", \"usage\": \"single\" }\n"
             << "  ],\n"
             << "  \"localization\": [\n"
             << "    { \"language_tag\": \"en_US\", \"" << kActionSet << "\": \"Frame Dock\", \"" << kGrabAction
             << "\": \"Move a Frame Dock panel (hold while pointing at it)\" },\n"
             << "    { \"language_tag\": \"ko_KR\", \"" << kActionSet << "\": \"Frame Dock\", \"" << kGrabAction
             << "\": \"Frame Dock 패널 옮기기 (가리킨 채 누르고 있기)\" },\n"
             << "    { \"language_tag\": \"ja_JP\", \"" << kActionSet << "\": \"Frame Dock\", \"" << kGrabAction
             << "\": \"Frame Dock のパネルを動かす（指したまま押し続ける）\" }\n"
             << "  ]\n"
             << "}\n";
    files.insert(files.begin(), {kManifestName, manifest.str()});
    return files;
}

std::string defaultManifestDir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base;
    if (xdg != nullptr && xdg[0] == '/') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = std::string(home != nullptr ? home : ".") + "/.local/share";
    }
    return base + "/framedock/input";
}

std::string writeManifest(const std::string& dir, int& written, std::string& error) {
    written = 0;
    if (!frame_perf_overlay::makeDirectories(dir)) {
        error = "can't make " + dir;
        return "";
    }
    for (const ManifestFile& file : manifestFiles()) {
        const std::string path = dir + "/" + file.name;
        std::ifstream in(path, std::ios::binary);
        if (in.is_open()) {
            const std::string current((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (current == file.json) continue;  // unchanged: leave it (SteamVR may be reading it)
        }
        if (!frame_perf_overlay::atomicWriteFile(path, file.json, error)) return "";
        ++written;
    }
    return dir + "/" + kManifestName;
}

}  // namespace dockinput
