// SteamVR Input's action manifest for the Dock's grab (the grip). One action set, /actions/dock, with one boolean,
// /actions/dock/in/grab, bound by default to the grip of the Frame's controllers (and of a few other controllers, so
// it does something on a PC headset too). The main loop activates the set only while a laser is on a Frame Dock panel
// or a grab runs, at normal priority: it never takes the grip from the game (that would need SteamVR's experimental
// global action set priority, which we don't use).
//
// The files are written at start into a folder of ours (XDG data: ~/.local/share/framedock/input), only when their
// content differs, so the install scripts don't need to know about them.
#pragma once

#include <string>
#include <vector>

namespace dockinput {

constexpr const char* kActionSet = "/actions/dock";
constexpr const char* kGrabAction = "/actions/dock/in/grab";
constexpr const char* kManifestName = "actions.json";

/** One file of the manifest: its name in the folder and its JSON. */
struct ManifestFile {
    std::string name;
    std::string json;
};

/** @return actions.json and its default binding files */
std::vector<ManifestFile> manifestFiles();

/** @return the folder: $XDG_DATA_HOME/framedock/input, or ~/.local/share/framedock/input */
std::string defaultManifestDir();

/**
 * Writes the manifest files into dir (made if missing), each only when its content differs (atomic: a temporary file
 * renamed over it).
 * @param written how many files were (re)written
 * @param error why it failed
 * @return the path of actions.json, or "" on failure
 */
std::string writeManifest(const std::string& dir, int& written, std::string& error);

}  // namespace dockinput
