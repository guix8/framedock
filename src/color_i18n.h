// The Color page's strings (FrameColor's dashboard), in English, Japanese and Korean.
// The page follows the app's language setting (Perf's config.json), so there is no language file here.
#pragma once

#include <array>
#include <string>
#include <vector>

namespace framecolor {

enum class Language {
    English = 0,
    Japanese = 1,
    Korean = 2,
};

struct Translations {
    const char* code;         // ISO language code

    std::array<const char*, 3> builtinPresets;  // Preset names: {Standard, Vivid, Night}
    const char* emptySlotSuffix;                // Suffix for unassigned custom slots (e.g. " (Empty)")
    const char* save;                           // Save button label (default mode)
    const char* selectSlot;                     // Save button label (when waiting for P1-P3 selection)
    const char* reset;                          // Reset button label (restores standard preset)
    const char* resetConfirm;                   // Reset button label when confirming (e.g. "Confirm?")

    const char* panelBrightness;                // Hardware panel brightness slider label
    const char* adjustmentChannel;              // Adjustment channel selector section header
    std::array<const char*, 4> channelNames;    // Channel options: {All, Red, Green, Blue}
    const char* brightness;                     // Color gain / brightness slider label
    const char* saturation;                     // Saturation slider label
    const char* contrast;                       // Contrast slider label
    const char* gamma;                          // Gamma slider label
    const char* sharpness;                      // Sharpness slider label
    const char* compare;                        // Compare button label (hold to see the original)
    const char* compareHeld;                    // Compare button label while held
    const char* autoCompare;                    // Auto compare toggle label
    const char* autoCompareShowingOriginal;     // Auto compare button while the original image is shown
    const char* autoCompareShowingCorrected;    // Auto compare button while the corrected image is shown
    const char* autoCompareOn;                  // Shown when auto compare starts (mentions 3 s and 60 s)
    const char* autoCompareOff;                 // Shown when auto compare is turned off
    const char* autoCompareEnded;               // Shown when auto compare stops by itself
    const char* sharpenCenter;                  // Sharpen area button: centre of each eye
    const char* sharpenWhole;                   // Sharpen area button: whole image
    const char* sharpenGaze;                    // Sharpen area button: around where the eyes look
    const char* gazeFit;                        // Sharpness row button (follow gaze): start a gaze fit (short)
    const char* gazeFitRunning;                 // ... while it runs: printf with the dot and the dots (a tap stops)
    const char* gazeFitHint[2];                 // Two lines in place of the sharpness track while a fit runs
    const char* gazeFitReviewHint[2];           // ... for a few seconds after a fit is done (the first: printf, seconds left)
    const char* gazeFitFailed;                  // ... after a fit failed: the first line (nothing changed)
    const char* gazeFitFailReason[4];           // ... the second: no gaze, no lens data, a dot unsteady, dots out of order
    const char* presets;                        // Presets card title
    const char* autoPreset;                     // Auto preset section header
    const char* standalone;                     // Standalone mode rule label
    const char* streaming;                      // PC streaming mode rule label
    const char* thisApp;                        // Current app rule label
    const char* autoOff;                        // Rule value when no preset is set
    const char* noApp;                          // Shown when no app is detected
    const char* nowPrefix;                      // Prefix for the current display context
    const char* sharpenPausedStreaming;         // Added to the "Now:" line while PC streaming pauses whole-image sharpening
    const char* autoApplied;                    // Prefix for the auto preset message
    const char* autoRestored;                   // Shown when leaving all auto preset rules
    const char* ruleSaved;                      // Shown after a rule button changes a rule
    const char* layerOff;                       // Shown when the Vulkan layer is not enabled
    const char* layerReload;                    // Shown when vrcompositor still runs a layer replaced on disk

    const char* errorSaveSettings;  // Error prefix when saving SteamVR analogGain setting fails
    const char* applied;            // Success message when a setting is applied
    const char* errorSaveAdvanced;  // Error message when writing current.txt fails
    const char* errorSavePreset;    // Error message when writing presets.txt fails
    const char* slotEmpty;          // Error message when clicking an unassigned custom preset
    const char* presetApplied;      // Suffix after the preset's name when a preset button applies it
    const char* promptSaveSlot;     // Prompt shown when entering save mode
    const char* saveCanceled;       // Notice shown when save mode is cancelled
    const char* savedToSlotPrefix;  // Prefix for slot save confirmation (e.g. "Saved to P")
    const char* savedToSlotSuffix;  // Suffix for slot save confirmation (e.g. " に保存しました")

    // Format a confirmation message when saving to a custom preset slot (e.g. P1, P2, P3).
    std::string formatSavedToSlot(int slotNumber) const;
    // Format the message after a preset button applied a preset (e.g. "Vivid applied").
    std::string formatPresetApplied(const std::string& presetName) const;
};

/** Get the list of all available languages. */
const std::vector<Language>& availableLanguages();

/** Get translation table for the specified language. */
const Translations& getTranslations(Language lang);

/** Get canonical string code for a language ("en", "ja", "ko"). */
std::string languageCode(Language lang);

}  // namespace framecolor
