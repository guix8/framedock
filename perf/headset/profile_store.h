// Profiles. A profile is a named copy of the layout (columns, widget order, every per-widget setting, in
// core/layout's own JSON) and of the placement (config.h's Placement: attachment, head position and rotation, both
// wrist poses, size, opacity, wrist fade, clock), one file each in ~/.config/frame-perf-overlay/profiles/ (next to
// config.json), written atomically. A file without "placement" (made before profiles had one, or a built-in template)
// is a layout-only profile: using it leaves the placement as it is. Show / hide and the other app settings aren't part
// of a profile. Files are only read at start and written on a button press.
#pragma once

#include "config.h"  // Placement
#include "layout.h"
#include "profiles_ui.h"  // LiveLoss

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

/** One saved profile. */
struct Profile {
    std::string id;      ///< "profile-3": the file name without .json; stays the same through renames
    std::string name;    ///< shown on the tab ("프로필 3" / "Profile 3" until renamed)
    frame_perf_overlay::Layout layout;  ///< its version field is meaningless (Use gives the live layout a new version)
    std::optional<Placement> placement;  ///< none: a layout-only profile (Use keeps the live placement)
};

/** A built-in profile Frame Dock adds once (seedTemplates). */
struct ProfileTemplate {
    std::string id;    ///< stable, for config.json's "templates_seeded": "simple_a", "simple_b", "basic", "detailed"
    std::string name;  ///< "간단 A"
    frame_perf_overlay::Layout layout;
};

/**
 * @return the built-in templates, named in uiLanguage(): "간단 A" (the Simple A card alone), "간단 B" (the small fps
 *         tile alone), "기본" (defaultLayout()) and "상세" (two columns of full cards and CPU 상위), in that order
 */
std::vector<ProfileTemplate> builtInTemplates();

class ProfileStore;

/**
 * Adds each built-in template not in seeded yet as a layout-only profile (no placement, so Use keeps where the HUD
 * is), after the profiles there are (ProfileStore::create's atEnd), unless a profile already has its name or all 8
 * are used: then it is skipped for good. Never overwrites or renames anything. Every template added or skipped goes
 * into seeded (config.json's "templates_seeded"), so a template the user deleted stays deleted; one whose file
 * couldn't be written stays out of it and is tried again on the next start.
 * @param seeded the templates offered before (in) and now (out)
 * @param created the names added
 * @param errors why a template couldn't be written
 * @return true if seeded changed (save the config)
 */
bool seedTemplates(ProfileStore& store, std::vector<std::string>& seeded, std::vector<std::string>& created,
                   std::vector<std::string>& errors);

/** The profiles folder. Single-threaded (the main loop). */
class ProfileStore {
public:
    static constexpr size_t kMaxProfiles = 8;
    static constexpr size_t kMaxNameBytes = 31;  ///< a longer name is cut (on a UTF-8 boundary)

    /** @param dir the folder (created on the first write) */
    explicit ProfileStore(std::string dir);

    /**
     * Reads every profile-N.json in the folder (N 1-8). A file that can't be read (or isn't a profile) is skipped
     * with a warning and listed in unreadable(); it is never written over or deleted, and its number isn't reused.
     * The others still load.
     */
    void load(std::vector<std::string>& warnings);

    /** @return the profiles, by id number */
    const std::vector<Profile>& profiles() const { return profiles_; }
    /** @return the profile with this id, nullptr if none */
    const Profile* find(const std::string& id) const;
    /** @return the ids whose file load() couldn't read ("profile-1"), by id number. Left as they are */
    const std::vector<std::string>& unreadable() const { return unreadable_; }
    /** @return true while there's room for another: a number with no profile and no file on disk */
    bool canCreate() const { return freeNumber() != 0; }

    /**
     * Whether Use / Reset would lose what's live ("unsaved changes"): it differs from the active profile, or, with no
     * active profile, it isn't saved in any. Compares widgets (not the version), so it follows every edit (the
     * editor's, a profile's or a file reload's), and, for a profile that has one and when livePlacement is given, the
     * placement as it would be saved.
     * @param live the live layout
     * @param activeId config.activeProfile ("" or an id that's gone = no active profile)
     * @param livePlacement the live placement (null: compare layouts only)
     */
    bool unsaved(const frame_perf_overlay::Layout& live, const std::string& activeId,
                 const Placement* livePlacement = nullptr) const;

    /**
     * @return true if the live layout is saved: it has the active profile's widgets, or, with no active profile
     *         ("" or gone), some profile's
     */
    bool layoutSaved(const frame_perf_overlay::Layout& live, const std::string& activeId) const;
    /**
     * @return true if the live placement is saved in the active profile (equal as saved). A layout-only active
     *         profile, or none, holds no placement: false
     */
    bool placementSaved(const Placement& livePlacement, const std::string& activeId) const;
    /**
     * What Use on target would throw away. The layout: when it isn't saved (layoutSaved) and target's differs. The
     * placement: only when target carries one that differs from the live placement and the live one isn't saved in
     * the active profile (placementSaved; a layout-only or no active profile never holds it). Per target, so it also
     * catches a moved HUD under a layout-only profile ("간단 A") that Use on a profile with a placement would lose.
     * The Profiles page's Use asks for a second tap on it; the footer's quick switch can use it too.
     * @return nothing lost if target doesn't exist
     */
    LiveLoss useWouldLose(const frame_perf_overlay::Layout& live, const Placement& livePlacement,
                          const std::string& activeId, const std::string& targetId) const;
    /**
     * What Reset (layout -> the built-in default; the placement stays) would throw away: the layout only, when it
     * isn't saved and isn't the default already.
     */
    LiveLoss resetWouldLose(const frame_perf_overlay::Layout& live, const std::string& activeId) const;

    /**
     * Saves a new profile, named name or "프로필 N" (in uiLanguage()) with N the lowest free number: one that no profile
     * has and no file uses (an unreadable profile-N.json, or one that appeared since load(), is never written over).
     * @param id the new profile's id
     * @param placement its placement (none: a layout-only profile)
     * @param name its name ("" = "프로필 N"; cleaned like a rename)
     * @param atEnd the number after the highest one in use (profiles and files) instead, so it comes last in the list;
     *        the lowest free one when that would be past 8
     * @return false with error if all 8 are used or the file can't be written
     */
    bool create(const frame_perf_overlay::Layout& layout, std::string& id, std::string& error,
                const std::optional<Placement>& placement = std::nullopt, const std::string& name = "",
                bool atEnd = false);
    /**
     * Overwrites a profile's layout and, when given, its placement (none: the profile keeps what it had).
     * @return false with error if it doesn't exist or can't be written
     */
    bool save(const std::string& id, const frame_perf_overlay::Layout& layout, std::string& error,
              const std::optional<Placement>& placement = std::nullopt);
    /** @return the first profile with this name, nullptr if none */
    const Profile* findByName(const std::string& name) const;
    /** Renames a profile (trimmed, cut to kMaxNameBytes; empty = unchanged). @return false with error as above */
    bool rename(const std::string& id, const std::string& name, std::string& error);
    /** Deletes a profile's file. @return false with error if it doesn't exist or can't be deleted */
    bool remove(const std::string& id, std::string& error);

    /**
     * The last failure's detail for the log (the OS's reason, the path; English), "" after a success. The error
     * the calls return is a short summary for the tab, in uiLanguage().
     */
    const std::string& lastErrorDetail() const { return detail_; }

    const std::string& dir() const { return dir_; }
    /** @return the file of a profile id */
    std::string pathOf(const std::string& id) const;
    /** @return bumps on every change (redraw) */
    uint64_t revision() const { return revision_; }

    /** @return name trimmed and cut to kMaxNameBytes without splitting a UTF-8 character */
    static std::string cleanName(const std::string& name);
    /** @return the file's JSON: { "name": ..., "layout": <layoutToJson>[, "placement": <placementToJson>] } */
    static std::string toJson(const Profile& profile);
    /**
     * Reads a profile file's text. A "placement" that isn't an object is skipped with a warning (the profile is then
     * layout-only).
     * @return false with error if it isn't a profile
     */
    static bool fromJson(const std::string& text, Profile& out, std::vector<std::string>& warnings, std::string& error);

private:
    std::string dir_;
    std::vector<Profile> profiles_;
    std::vector<std::string> unreadable_;
    std::string detail_;  ///< lastErrorDetail()
    uint64_t revision_ = 0;

    /** Sets error (uiLanguage(), for the tab) and the detail (for the log). @return false */
    bool fail(std::string& error, const std::string& message, const std::string& detail);

    /** @return the lowest N with no profile and no profile-N.json on disk, 0 if none */
    size_t freeNumber() const;

    bool write(const Profile& profile, std::string& error);
    Profile* findMutable(const std::string& id);
};
