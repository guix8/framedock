// What the dashboard tab's Profiles page shows (main.cpp builds it from the ProfileStore and the config).
// Kept apart so the settings panel doesn't pull in the store.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * What a profile action would throw away: live changes that aren't saved and that the action replaces with something
 * else (ProfileStore::useWouldLose / resetWouldLose). Use / Reset on the Profiles page ask for a second tap when any.
 */
struct LiveLoss {
    bool layout = false;     ///< the live layout's unsaved changes
    bool placement = false;  ///< the live placement's unsaved changes
    bool any() const { return layout || placement; }
    bool operator==(const LiveLoss& o) const { return layout == o.layout && placement == o.placement; }
    bool operator!=(const LiveLoss& o) const { return !(*this == o); }
};

/** The saved profiles and the live layout's state against them. */
struct ProfilesUiState {
    struct Item {
        std::string id;    ///< "profile-3" (pass it back through SettingsPanel::selectedProfileId())
        std::string name;
        bool layoutOnly = false;  ///< no placement in it: Use leaves the placement as it is
        LiveLoss useLoses {};     ///< what Use on this profile would throw away (the quick switch doesn't ask yet)
        bool operator==(const Item& o) const {
            return id == o.id && name == o.name && layoutOnly == o.layoutOnly && useLoses == o.useLoses;
        }
    };
    std::vector<Item> items;  ///< by id number
    /** Profile files that couldn't be read ("profile-1.json"): shown, left as they are, their numbers not reused */
    std::vector<std::string> unreadable;
    std::string activeId;     ///< the active profile, "" = none
    /**
     * The "unsaved changes" chip: the layout (or, for an active profile with a placement, the placement) differs from
     * the active profile, or (no active profile) it isn't saved in any profile. The second tap of Use / Reset follows
     * useLoses / resetLoses instead, which also see a placement a layout-only profile doesn't hold.
     */
    bool unsaved = false;
    LiveLoss resetLoses {};  ///< what Reset (layout -> default, placement untouched) would throw away: the layout only
    size_t maxProfiles = 8;
    std::string status;     ///< the last action's result ("Saved to Gaming"), "" = nothing yet
    uint64_t revision = 0;  ///< bumps whenever anything above changes (redraw)

    /** @return true if everything but the revision is the same (no redraw needed) */
    bool sameAs(const ProfilesUiState& o) const {
        return items == o.items && unreadable == o.unreadable && activeId == o.activeId && unsaved == o.unsaved &&
               resetLoses == o.resetLoses && maxProfiles == o.maxProfiles && status == o.status;
    }
};
