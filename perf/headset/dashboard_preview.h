// Made-up state for pictures of the dashboard tab without SteamVR: --dump-settings-png and the tests that render the
// README's images (dock-view). Nothing here reads or writes the user's files.
#pragma once

#include "color_model.h"  // src/
#include "device_model.h"
#include "i18n.h"
#include "profiles_ui.h"

/**
 * The Color page's state for a picture: a custom preset, rules for standalone and the scene app, and a message. With
 * busy, the states with the longest labels: auto compare showing the original, save mode, reset armed, whole-image
 * sharpening.
 */
void previewColorState(framecolor::ColorModel& model, framecolor::Language language, bool busy);

/**
 * Made-up profiles: the two built-in templates (layout only) and three of the user's, "Racing" active.
 * @param profilesPage the Profiles page's picture: unsaved changes and a result line too
 */
ProfilesUiState previewProfiles(Language language, bool profilesPage);

/** The variants of the 기기 page's made-up state (--preview-device). */
extern const char* const kPreviewDeviceVariants[];
extern const size_t kPreviewDeviceVariantCount;

/**
 * The 기기 page's state for a picture (no backend: nothing is read or sent). Made-up accounts and outputs.
 * @param variant "" (two outputs, colour passthrough, the PC on another account), "single" (one output, muted,
 *        passthrough off, the PC ready), "unavailable" (Camera Switch not running, sound and the PC not read yet),
 *        "sync-pc-off" (the PC off: the button wakes it), "sync-switching" (a run that woke the PC, switching),
 *        "sync-confirm" (a game runs on the PC: the question line), "sync-error" (the app key refused)
 * @return false if the variant is unknown (the state is the default one then)
 */
bool previewDeviceState(DeviceModel& model, const std::string& variant);
