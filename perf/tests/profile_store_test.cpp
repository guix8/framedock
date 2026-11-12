// Layout profiles. ProfileStore (round trip, the 8 cap, rename / delete, atomic writes, unsaved tracking) and
// its use with LayoutAuthority (what a PC's LAYOUT_SET and the Reset button do). POSIX only; works in a temp dir and
// never touches ~/.config.
#include "config.h"
#include "json.h"
#include "layout.h"
#include "layout_authority.h"
#include "layout_edit.h"
#include "profile_store.h"

#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using frame_perf_overlay::Layout;
using frame_perf_overlay::WidgetInstance;
using W = frame_perf_overlay::WidgetType;

std::string makeTempDir() {
    char tmpl[] = "/tmp/frame-perf-overlay-profiles-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    return dir != nullptr ? dir : "";
}

std::vector<std::string> listDir(const std::string& dir) {
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") names.push_back(n);
        }
        closedir(d);
    }
    return names;
}

bool hasTmp(const std::string& dir) {
    for (const std::string& n : listDir(dir)) {
        if (n.find(".tmp") != std::string::npos) return true;
    }
    return false;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

void spit(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

bool exists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

void rmTree(const std::string& dir) {
    for (const std::string& n : listDir(dir)) std::remove((dir + "/" + n).c_str());
    rmdir(dir.c_str());
}

WidgetInstance inst(W type) {
    WidgetInstance w;
    w.type = type;
    return w;
}

Layout make(const std::vector<std::vector<W>>& columns) {
    Layout layout;
    for (const std::vector<W>& types : columns) {
        frame_perf_overlay::LayoutColumn column;
        for (const W t : types) column.widgets.push_back(inst(t));
        layout.columns.push_back(column);
    }
    return layout;
}

/** A layout that is different from every other `make({{...}})` below by its column count. */
Layout variant(size_t n) {
    // one column each, every widget at most once (a layout file repeats none)
    const W types[] = {W::Frame, W::Cpu, W::GpuRam, W::Power, W::Battery, W::Temp, W::Network, W::General};
    Layout layout;
    for (size_t i = 0; i < n; ++i) layout.columns.push_back({{inst(types[i % 8])}});
    return layout;
}

/** Every widget type, in two columns, with the per-widget setting layout.h has (Compact) switched on. */
Layout fullLayout() {
    Layout layout;
    layout.version = 7;
    frame_perf_overlay::LayoutColumn a;
    for (const W t : {W::Cpu, W::GpuRam, W::Network}) a.widgets.push_back(inst(t));
    layout.columns.push_back(a);
    frame_perf_overlay::LayoutColumn b;
    for (const W t : {W::Frame, W::Power, W::Battery, W::Temp, W::General}) {
        WidgetInstance w = inst(t);
        if (frame_perf_overlay::hasCompactView(t)) w.compact = true;  // a type without the switch keeps the default
        b.widgets.push_back(w);
    }
    layout.columns.push_back(b);
    return layout;
}

// 1. toJson -> fromJson
void roundTrip(const Layout& layout, const char* what) {
    Profile in;
    in.id = "profile-1";
    in.name = std::string("Round ") + what;
    in.layout = layout;
    Profile out;
    std::vector<std::string> warnings;
    std::string error;
    const bool ok = ProfileStore::fromJson(ProfileStore::toJson(in), out, warnings, error);
    if (!ok) std::printf("  (%s: %s)\n", what, error.c_str());
    CHECK(ok);
    CHECK(out.layout.sameWidgets(in.layout));
    CHECK(out.name == in.name);
    if (!warnings.empty()) std::printf("  (%s warning: %s)\n", what, warnings.front().c_str());
    CHECK(warnings.empty());
}

void testRoundTrip() {
    roundTrip(frame_perf_overlay::defaultLayout(), "default");
    const Layout full = fullLayout();
    roundTrip(full, "full");
    // The full layout really carries the settings (so a round trip that dropped them would fail above)
    CHECK(full.columns[1].widgets[2].compact && full.columns[1].widgets[3].compact);

    // an old file (no compact / name keys) still loads with defaults
    const std::string old =
        "{ \"layout\": { \"version\": 3, \"columns\": [ [ {\"type\":\"frame\"}, {\"type\":\"battery\"} ] ] } }";
    Profile p;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(ProfileStore::fromJson(old, p, warnings, error));
    CHECK(p.name.empty());  // load() names it "프로필 N"
    CHECK(p.layout.columns.size() == 1 && p.layout.columns[0].widgets.size() == 2);
    const WidgetInstance& items = p.layout.columns[0].widgets[1];
    CHECK(items.type == W::Battery && !items.compact);
    CHECK(p.layout.columns[0].widgets[0].compact);  // a missing key: Frame compact, Battery / Temp full

    // Not a profile at all
    CHECK(!ProfileStore::fromJson("{ \"name\": \"x\" }", p, warnings, error));
    CHECK(!ProfileStore::fromJson("not json", p, warnings, error));
    CHECK(!ProfileStore::fromJson("[1,2]", p, warnings, error));
    // A name with quotes / backslash / control chars survives (jsonString escapes; cleanName keeps ", \ and drops control)
    Profile q;
    q.id = "profile-1";
    q.name = "a\"b\\c";
    q.layout = make({{W::Frame}});
    Profile r;
    CHECK(ProfileStore::fromJson(ProfileStore::toJson(q), r, warnings, error));
    CHECK(r.name == q.name);
}

// 2. the 8 cap
void testCap() {
    const std::string dir = makeTempDir();
    ProfileStore store(dir);
    std::vector<std::string> warnings;
    store.load(warnings);
    CHECK(store.profiles().empty() && store.canCreate());
    for (size_t n = 1; n <= 8; ++n) {
        std::string id, error;
        CHECK(store.create(variant(n), id, error));
        CHECK(id == "profile-" + std::to_string(n));
        CHECK(store.find(id) != nullptr && store.find(id)->name == "프로필 " + std::to_string(n));
        CHECK(exists(dir + "/" + id + ".json"));
    }
    CHECK(store.profiles().size() == 8 && !store.canCreate());
    const std::vector<std::string> before = listDir(dir);
    std::string id = "untouched", error;
    CHECK(!store.create(variant(9), id, error));
    CHECK(error == "프로필은 8개까지 만들 수 있어요" && !store.lastErrorDetail().empty());  // Korean on the tab
    CHECK(id == "untouched");
    CHECK(store.profiles().size() == 8);
    CHECK(listDir(dir).size() == before.size());  // nothing written (and no stray temp)

    // delete 3, the next create reuses it
    CHECK(store.remove("profile-3", error));
    CHECK(store.canCreate() && store.find("profile-3") == nullptr);
    CHECK(store.create(variant(3), id, error));
    CHECK(id == "profile-3" && store.find(id)->name == "프로필 3");
    for (size_t i = 0; i < store.profiles().size(); ++i) {  // kept sorted by id number
        CHECK(store.profiles()[i].id == "profile-" + std::to_string(i + 1));
    }

    // a fresh store sees all 8, in order
    ProfileStore again(dir);
    again.load(warnings);
    CHECK(again.profiles().size() == 8);
    CHECK(again.profiles()[2].layout.sameWidgets(variant(3)));
    CHECK(warnings.empty());
    rmTree(dir);
}

// 3. rename / delete
void testRenameDelete() {
    const std::string dir = makeTempDir();
    std::vector<std::string> warnings;
    std::string id, error;
    {
        ProfileStore store(dir);
        store.load(warnings);
        CHECK(store.create(make({{W::Frame}}), id, error));
        const uint64_t rev = store.revision();
        CHECK(store.rename(id, "Gaming", error));
        CHECK(store.revision() > rev);
    }
    {
        ProfileStore fresh(dir);
        fresh.load(warnings);
        CHECK(fresh.find("profile-1") != nullptr && fresh.find("profile-1")->name == "Gaming");
    }

    ProfileStore store(dir);
    store.load(warnings);
    CHECK(store.rename(id, "   padded \t ", error) && store.find(id)->name == "padded");
    // empty / blank = unchanged, and still "success"
    CHECK(store.rename(id, "", error) && store.find(id)->name == "padded");
    CHECK(store.rename(id, "  \n ", error) && store.find(id)->name == "padded");
    // control chars inside become spaces
    CHECK(store.rename(id, "a\nb", error) && store.find(id)->name == "a b");
    // 31 bytes max
    const std::string longName(60, 'x');
    CHECK(store.rename(id, longName, error) && store.find(id)->name == std::string(31, 'x'));
    // multi-byte: 20 x "é" (2 bytes) = 40 bytes -> 15 whole characters = 30 bytes, not 31 with half a character
    std::string accents;
    for (int i = 0; i < 20; ++i) accents += "\xC3\xA9";
    CHECK(store.rename(id, accents, error));
    CHECK(store.find(id)->name.size() == 30);
    CHECK(store.find(id)->name.substr(0, 2) == "\xC3\xA9");
    // exactly 31 ASCII bytes stays whole; 32 is cut
    CHECK(ProfileStore::cleanName(std::string(31, 'y')).size() == 31);
    CHECK(ProfileStore::cleanName(std::string(32, 'y')).size() == 31);
    // 3-byte chars: 11 x "€" = 33 bytes -> 10 chars = 30 bytes
    std::string euros;
    for (int i = 0; i < 11; ++i) euros += "\xE2\x82\xAC";
    CHECK(ProfileStore::cleanName(euros).size() == 30);
    // the saved file has the cut name too
    {
        ProfileStore fresh(dir);
        fresh.load(warnings);
        CHECK(fresh.find(id)->name.size() == 30);
    }
    CHECK(!store.rename("profile-6", "x", error) && !error.empty());

    // delete
    CHECK(!store.remove("profile-6", error) && !error.empty());
    CHECK(!store.remove("nonsense", error));
    CHECK(exists(store.pathOf(id)));
    CHECK(store.remove(id, error));
    CHECK(!exists(store.pathOf(id)));
    CHECK(store.profiles().empty() && store.find(id) == nullptr);
    CHECK(!store.remove(id, error));  // already gone
    rmTree(dir);
}

// 4. atomic writes, garbage files, strangers
void testFilesInDir() {
    const std::string dir = makeTempDir();
    std::vector<std::string> warnings;
    std::string id, error;
    ProfileStore store(dir);
    store.load(warnings);
    CHECK(store.create(make({{W::Frame}}), id, error));
    CHECK(!hasTmp(dir));
    CHECK(store.save(id, make({{W::Power}}), error));
    CHECK(!hasTmp(dir));
    CHECK(store.rename(id, "Renamed", error));
    CHECK(!hasTmp(dir));
    CHECK(listDir(dir).size() == 1);
    CHECK(!store.save("profile-2", make({{W::Power}}), error) && !error.empty());  // nothing to overwrite
    CHECK(!exists(dir + "/profile-2.json"));

    spit(dir + "/profile-5.json", "this is { not json");
    spit(dir + "/profile-6.json", "{ \"name\": \"no layout\" }");
    spit(dir + "/notes.txt", "hi");
    spit(dir + "/profile-9.json", slurp(store.pathOf(id)));  // 9 is out of range
    spit(dir + "/profile-0.json", slurp(store.pathOf(id)));
    spit(dir + "/profile-1.json.bak", slurp(store.pathOf(id)));

    ProfileStore fresh(dir);
    std::vector<std::string> warn;
    fresh.load(warn);
    CHECK(fresh.profiles().size() == 1 && fresh.profiles()[0].id == "profile-1");
    CHECK(fresh.profiles()[0].name == "Renamed");
    CHECK(fresh.find("profile-5") == nullptr && fresh.find("profile-6") == nullptr);
    CHECK(warn.size() == 2);
    bool mention5 = false, mention6 = false, mention9 = false, mentionNotes = false;
    for (const std::string& w : warn) {
        mention5 = mention5 || w.find("profile-5") != std::string::npos;
        mention6 = mention6 || w.find("profile-6") != std::string::npos;
        mention9 = mention9 || w.find("profile-9") != std::string::npos;
        mentionNotes = mentionNotes || w.find("notes") != std::string::npos;
    }
    CHECK(mention5 && mention6 && !mention9 && !mentionNotes);
    // The skipped garbage file is left alone, and creating takes the lowest free number (2), not 5
    CHECK(slurp(dir + "/profile-5.json") == "this is { not json");
    CHECK(fresh.create(make({{W::Battery}}), id, error) && id == "profile-2");
    CHECK(!hasTmp(dir));
    // A folder that doesn't exist yet: load is fine (empty), the first create makes it (or fails with an error)
    ProfileStore missing(dir + "/sub/profiles");
    std::vector<std::string> w2;
    missing.load(w2);
    CHECK(missing.profiles().empty() && w2.empty());
    rmTree(dir);
}

// 5. unsaved tracking, with LayoutAuthority as the PC's LAYOUT_SET reaches it
void testUnsaved() {
    const std::string dir = makeTempDir();
    std::vector<std::string> warnings;
    std::string id, error;
    ProfileStore store(dir);
    store.load(warnings);

    Config config;
    config.layout = make({{W::Frame, W::Cpu}, {W::Power}});
    int saves = 0;
    LayoutAuthority auth(
        config, [&](const Config&, std::string&) { return ++saves > 0; }, [](const Layout&) {});

    // no profiles at all: the live layout is in none of them
    CHECK(store.unsaved(auth.layout(), ""));
    CHECK(store.create(auth.layout(), id, error));
    config.activeProfile = id;
    CHECK(!store.unsaved(auth.layout(), id));

    // an edit through trySetLayout (a PC's LAYOUT_SET)
    Layout edited = auth.layout();
    edited.columns[0].widgets.push_back(inst(W::Temp));
    const auto r = auth.trySetLayout(auth.version(), edited);
    CHECK(r.applied());
    CHECK(store.unsaved(auth.layout(), id));
    // an edit that only bumps the version is not a change
    Layout same = auth.layout();
    CHECK(auth.trySetLayout(auth.version(), same).applied());
    CHECK(store.unsaved(auth.layout(), id));  // still the Temp edit
    // Save -> clean again
    CHECK(store.save(id, auth.layout(), error));
    CHECK(!store.unsaved(auth.layout(), id));
    // a pure version bump after saving stays clean
    CHECK(auth.trySetLayout(auth.version(), auth.layout()).applied());
    CHECK(!store.unsaved(auth.layout(), id));

    // no / missing active id: unsaved unless some profile equals the live layout
    CHECK(!store.unsaved(auth.layout(), ""));
    CHECK(!store.unsaved(auth.layout(), "profile-8"));  // gone id behaves like none
    CHECK(!store.unsaved(auth.layout(), "garbage"));
    Layout other = make({{W::Battery}});
    CHECK(auth.trySetLayout(auth.version(), other).applied());
    CHECK(store.unsaved(auth.layout(), ""));
    CHECK(store.unsaved(auth.layout(), "profile-8"));
    CHECK(store.unsaved(auth.layout(), id));
    // equal to a profile that is NOT the active one: active id decides (still unsaved against the active one)
    std::string id2;
    CHECK(store.create(other, id2, error));
    CHECK(!store.unsaved(auth.layout(), ""));       // matches profile-2
    CHECK(store.unsaved(auth.layout(), id));        // active = profile-1, which differs
    CHECK(!store.unsaved(auth.layout(), id2));
    rmTree(dir);
}

// A profile saved with every widget removed comes back empty (through the editor's path: LayoutAuthority)
void testEmptyProfile() {
    const std::string dir = makeTempDir();
    std::vector<std::string> warnings;
    std::string id, error;
    ProfileStore store(dir);
    store.load(warnings);
    Config config;
    config.layout = make({{W::Frame, W::Cpu}, {W::Power}});
    LayoutAuthority auth(config, [](const Config&, std::string&) { return true; }, [](const Layout&) {});
    CHECK(store.create(auth.layout(), id, error));

    Layout emptied = auth.layout();
    while (!emptied.columns.empty()) CHECK(frame_perf_overlay::removeWidget(emptied, {0, 0}));
    CHECK(auth.trySetLayout(auth.version(), emptied).applied());
    CHECK(auth.layout().columns.empty());
    CHECK(store.save(id, auth.layout(), error));
    CHECK(!store.unsaved(auth.layout(), id));
    std::string id2;
    CHECK(store.create(auth.layout(), id2, error));  // a new profile from the empty layout too

    ProfileStore fresh(dir);
    warnings.clear();
    fresh.load(warnings);
    CHECK(fresh.profiles().size() == 2);
    CHECK(fresh.find(id) != nullptr && fresh.find(id)->layout.columns.empty());
    CHECK(fresh.find(id2) != nullptr && fresh.find(id2)->layout.columns.empty());
    CHECK(warnings.empty());
    rmTree(dir);
}

// 6. Reset leaves the profile files alone
void testResetLeavesProfiles() {
    const std::string dir = makeTempDir();
    std::vector<std::string> warnings;
    std::string id, error;
    ProfileStore store(dir);
    store.load(warnings);
    Config config;
    config.layout = fullLayout();
    LayoutAuthority auth(config, [](const Config&, std::string&) { return true; }, [](const Layout&) {});
    CHECK(store.create(auth.layout(), id, error));
    config.activeProfile = id;
    std::string id2;
    CHECK(store.create(make({{W::Frame}}), id2, error));
    const std::string a = slurp(store.pathOf("profile-1")), b = slurp(store.pathOf("profile-2"));
    CHECK(!a.empty() && !b.empty());
    CHECK(!store.unsaved(auth.layout(), id));

    const auto r = auth.trySetLayout(auth.version(), frame_perf_overlay::defaultLayout());
    CHECK(r.applied());
    CHECK(auth.layout().sameWidgets(frame_perf_overlay::defaultLayout()));
    CHECK(slurp(store.pathOf("profile-1")) == a);
    CHECK(slurp(store.pathOf("profile-2")) == b);
    CHECK(store.unsaved(auth.layout(), id));
    CHECK(store.find(id)->layout.sameWidgets(fullLayout()));
    CHECK(config.activeProfile == id);  // Reset doesn't deselect it
    // a stale base is refused and leaves the live layout (Use / Reset sent a stale version)
    CHECK(auth.trySetLayout(auth.version() - 1, fullLayout()).outcome == LayoutAuthority::Outcome::Stale);
    CHECK(auth.layout().sameWidgets(frame_perf_overlay::defaultLayout()));
    rmTree(dir);
}

// 7. Profiles carry the placement: round trip, old files without it, Save / New, unsaved, Use (useLayout)
void testPlacement() {
    // A placement unlike the defaults, with values the dashboard makes (mm, 0.1°, 5 % steps)
    Config moved;
    moved.attachment = Attachment::RightWrist;
    moved.posX = 0.18;
    moved.posY = -0.2;
    moved.posZ = -0.75;
    moved.yawDeg = -12.3;
    moved.pitchDeg = 7;
    moved.rollDeg = 3;
    moved.leftWrist = {0.01, 0.06, 0.07, -80, 5, -4};
    moved.rightWrist = {-0.02, 0.04, 0.1, -100, -6.5, 8};
    moved.wristFade = false;
    moved.wristFadeEndDeg = 60;
    moved.clockFormat = 12;
    moved.widthM = 0.26;
    moved.alpha = 0.65;
    const Placement placement = placementOf(moved);
    CHECK(!samePlacement(placement, placementOf(Config())));

    // Round trip through the file's JSON
    Profile in;
    in.id = "profile-1";
    in.name = "Placed";
    in.layout = make({{W::Frame}});
    in.placement = placement;
    Profile out;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(ProfileStore::fromJson(ProfileStore::toJson(in), out, warnings, error) && warnings.empty());
    CHECK(out.placement.has_value() && samePlacement(*out.placement, placement));
    if (out.placement) {
        Config back;
        applyPlacement(*out.placement, back);
        CHECK(back.attachment == Attachment::RightWrist && back.posZ == -0.75 && back.yawDeg == -12.3);
        CHECK(back.rightWrist.yaw == -6.5 && !back.wristFade && back.wristFadeEndDeg == 60);
        CHECK(back.clockFormat == 12 && back.widthM == 0.26 && back.alpha == 0.65);
        CHECK(back.visible && back.layout.sameWidgets(frame_perf_overlay::defaultLayout()));  // not part of it
    }
    // The placement members are config.json's (a profile's "placement" can be pasted from config.json)
    JsonValue root;
    CHECK(parseJson(ProfileStore::toJson(in), root, error));
    const JsonValue* json = root.get("placement");
    CHECK(json != nullptr && json->get("width_m") != nullptr && json->get("left_wrist") != nullptr &&
          json->get("visible") == nullptr && json->get("layout") == nullptr);

    // An old file (no "placement") is layout-only; a broken "placement" is skipped with a warning; a partial one
    // keeps the defaults for what's missing
    const std::string layoutOnly = "{ \"name\": \"Old\", \"layout\": { \"version\": 1, \"columns\": [ [ {\"type\":\"frame\"} ] ] } }";
    Profile old;
    warnings.clear();
    CHECK(ProfileStore::fromJson(layoutOnly, old, warnings, error) && warnings.empty() && !old.placement);
    Profile broken;
    const std::string brokenText = layoutOnly.substr(0, layoutOnly.size() - 2) + ", \"placement\": 5 }";
    CHECK(ProfileStore::fromJson(brokenText, broken, warnings, error) && !broken.placement && warnings.size() == 1);
    Profile partial;
    warnings.clear();
    const std::string partialText = layoutOnly.substr(0, layoutOnly.size() - 2) +
                                    ", \"placement\": { \"width_m\": 0.3, \"attachment\": \"left_wrist\", \"x\": 1 } }";
    CHECK(ProfileStore::fromJson(partialText, partial, warnings, error) && partial.placement && warnings.size() == 1);
    if (partial.placement) {
        CHECK(partial.placement->widthM == 0.3 && partial.placement->attachment == Attachment::LeftWrist);
        CHECK(partial.placement->alpha == Config().alpha && partial.placement->posZ == Config().posZ);
    }

    // The store: New with the placement, Save keeps / replaces it, a layout-only profile stays layout-only
    const std::string dir = makeTempDir();
    ProfileStore store(dir);
    store.load(warnings);
    std::string id, plain;
    CHECK(store.create(make({{W::Frame}}), id, error, placement));
    CHECK(store.create(make({{W::Power}}), plain, error, std::nullopt, "  간단 A  "));
    CHECK(store.find(plain) != nullptr && store.find(plain)->name == "간단 A" && !store.find(plain)->placement);
    CHECK(store.findByName("간단 A") == store.find(plain) && store.findByName("nope") == nullptr);
    CHECK(store.save(id, make({{W::Frame}, {W::Cpu}}), error));  // no placement given: it keeps its own
    CHECK(store.find(id)->placement && samePlacement(*store.find(id)->placement, placement));
    ProfileStore fresh(dir);
    warnings.clear();
    fresh.load(warnings);
    CHECK(warnings.empty() && fresh.find(id)->placement && !fresh.find(plain)->placement);
    CHECK(fresh.find(plain)->name == "간단 A");
    // The old file format on disk loads too
    spit(store.pathOf("profile-3"), layoutOnly);
    fresh.load(warnings);
    CHECK(fresh.find("profile-3") != nullptr && !fresh.find("profile-3")->placement);

    // Unsaved: the placement counts for a profile that has one, and not for a layout-only one
    Config config;
    config.layout = make({{W::Frame}, {W::Cpu}});
    applyPlacement(placement, config);
    Placement live = placementOf(config);
    CHECK(!store.unsaved(config.layout, id, &live));
    config.widthM = 0.28;  // the size − / ＋
    live = placementOf(config);
    CHECK(store.unsaved(config.layout, id, &live));
    CHECK(!store.unsaved(config.layout, id));  // layouts only
    CHECK(store.save(id, config.layout, error, live));
    CHECK(!store.unsaved(config.layout, id, &live));
    config.layout = make({{W::Power}});
    config.posX = 0.0;
    live = placementOf(config);
    CHECK(!store.unsaved(config.layout, plain, &live));  // layout-only: the placement doesn't matter
    CHECK(!store.unsaved(config.layout, "", &live));     // no active profile: the layout-only one matches

    // Use: useLayout takes the layout, the active profile and the placement in one save; a layout-only profile
    // keeps the live placement; a failed save changes nothing
    Config liveConfig;
    liveConfig.visible = false;
    liveConfig.layout = make({{W::Battery}});
    bool failSave = false;
    std::vector<Config> saved;
    LayoutAuthority auth(
        liveConfig,
        [&](const Config& c, std::string& e) {
            if (failSave) {
                e = "disk full";
                return false;
            }
            saved.push_back(c);
            return true;
        },
        [](const Layout&) {});
    const Profile* placed = store.find(id);
    auto r = auth.useLayout(placed->layout, placed->id, placed->placement);
    CHECK(r.applied() && saved.size() == 1);
    CHECK(liveConfig.layout.sameWidgets(placed->layout) && liveConfig.activeProfile == id);
    CHECK(samePlacement(placementOf(liveConfig), *placed->placement) && !liveConfig.visible);
    CHECK(samePlacement(placementOf(saved.back()), *placed->placement) && saved.back().activeProfile == id);
    const Placement before = placementOf(liveConfig);
    const Profile* only = store.find(plain);
    r = auth.useLayout(only->layout, only->id, only->placement);
    CHECK(r.applied() && liveConfig.layout.sameWidgets(only->layout) && liveConfig.activeProfile == plain);
    CHECK(samePlacement(placementOf(liveConfig), before));
    failSave = true;
    const uint32_t version = liveConfig.layout.version;
    r = auth.useLayout(placed->layout, placed->id, placed->placement);
    CHECK(r.outcome == LayoutAuthority::Outcome::SaveFailed && r.error == "disk full");
    CHECK(liveConfig.layout.version == version && liveConfig.activeProfile == plain &&
          samePlacement(placementOf(liveConfig), before) && liveConfig.layout.sameWidgets(only->layout));
    rmTree(dir);
}

// 8. The built-in templates: each added once as a layout-only profile (by its id in "templates_seeded"), after the
// profiles there are, never over the user's own
void testSeedTemplates() {
    const std::string dir = makeTempDir();
    std::vector<std::string> warnings, created, errors, seeded;
    std::string id, error;
    ProfileStore store(dir);
    store.load(warnings);
    CHECK(seedTemplates(store, seeded, created, errors) && errors.empty());
    CHECK(created.size() == 4 && created[0] == "간단 A" && created[1] == "간단 B" && created[2] == "기본" &&
          created[3] == "상세");
    CHECK((seeded == std::vector<std::string> {"simple_a", "simple_b", "basic", "detailed"}));
    const Profile* a = store.findByName("간단 A");
    const Profile* b = store.findByName("간단 B");
    const Profile* basic = store.findByName("기본");
    const Profile* detailed = store.findByName("상세");
    CHECK(a != nullptr && b != nullptr && !a->placement && !b->placement);
    CHECK(a != nullptr && a->layout.sameWidgets(make({{W::SimpleA}})));
    CHECK(b != nullptr && b->layout.sameWidgets(make({{W::SimpleB}})));
    // 기본 is the default layout; 상세 the approved mockup's (detailed.json), both without a placement
    CHECK(basic != nullptr && !basic->placement && basic->layout.sameWidgets(frame_perf_overlay::defaultLayout()));
    frame_perf_overlay::Layout mockup;
    CHECK(frame_perf_overlay::parseLayoutText(
        "{\"version\":1,\"columns\":[[{\"type\":\"frame\",\"compact\":false},{\"type\":\"cpu\",\"compact\":false},"
        "{\"type\":\"temp\",\"compact\":false},{\"type\":\"network\"}],[{\"type\":\"gpu_ram\"},{\"type\":\"power\"},"
        "{\"type\":\"battery\",\"compact\":false},{\"type\":\"cpu_items\",\"max_items\":5,\"sum_same_name\":true,"
        "\"hide_system\":true},{\"type\":\"general\",\"compact\":false}]]}",
        mockup, warnings, error));
    CHECK(detailed != nullptr && !detailed->placement && detailed->layout.sameWidgets(mockup));
    // In the list (number order): 간단 A, 간단 B, 기본, 상세
    CHECK(store.profiles().size() == 4 && store.profiles()[2].name == "기본" && store.profiles()[3].name == "상세");
    // Again: all four offered, nothing is added or rewritten
    const std::string fileA = slurp(store.pathOf(a->id));
    created.clear();
    CHECK(!seedTemplates(store, seeded, created, errors) && created.empty() && store.profiles().size() == 4);
    CHECK(slurp(store.pathOf(a->id)) == fileA);
    // A template the user deleted doesn't come back
    CHECK(store.remove(store.findByName("기본")->id, error));
    CHECK(!seedTemplates(store, seeded, created, errors) && created.empty() && store.findByName("기본") == nullptr);
    rmTree(dir);

    // An older config.json's "templates_seeded": true (간단 A and 간단 B offered): only 기본 and 상세 are added, after
    // the user's profiles (number 2 was deleted: they still go last), and 간단 A, which they deleted, stays away
    const std::string dirLegacy = makeTempDir();
    ProfileStore legacy(dirLegacy);
    legacy.load(warnings);
    for (int n = 0; n < 3; ++n) CHECK(legacy.create(variant(static_cast<size_t>(n + 1)), id, error));
    CHECK(legacy.remove("profile-2", error));
    Config old;
    {
        const std::string configPath = dirLegacy + "/config.json";
        spit(configPath, "{ \"templates_seeded\": true }");
        warnings.clear();
        CHECK(loadConfig(configPath, old, warnings, error) && warnings.empty());
        CHECK((old.templatesSeeded == std::vector<std::string> {"simple_a", "simple_b"}));
    }
    created.clear();
    CHECK(seedTemplates(legacy, old.templatesSeeded, created, errors) && errors.empty());
    CHECK(created.size() == 2 && created[0] == "기본" && created[1] == "상세");
    CHECK(legacy.findByName("기본")->id == "profile-4" && legacy.findByName("상세")->id == "profile-5");
    CHECK(legacy.findByName("간단 A") == nullptr && legacy.findByName("간단 B") == nullptr);
    CHECK(old.templatesSeeded.size() == 4);
    rmTree(dirLegacy);

    // A profile of the user's named "간단 A" stays as it is; it counts as offered
    const std::string dir2 = makeTempDir();
    ProfileStore mine(dir2);
    mine.load(warnings);
    CHECK(mine.create(make({{W::Temp}}), id, error, placementOf(Config()), "간단 A"));
    created.clear();
    seeded.clear();
    CHECK(seedTemplates(mine, seeded, created, errors) && created.size() == 3 && created[0] == "간단 B");
    CHECK(mine.find(id)->layout.sameWidgets(make({{W::Temp}})) && mine.find(id)->placement && seeded.size() == 4);
    rmTree(dir2);

    // No room: nothing added, not a failure, and skipped for good (deleting one later doesn't bring them in)
    const std::string dir3 = makeTempDir();
    ProfileStore full(dir3);
    full.load(warnings);
    for (size_t n = 1; n <= ProfileStore::kMaxProfiles; ++n) CHECK(full.create(variant(n), id, error));
    created.clear();
    seeded.clear();
    CHECK(seedTemplates(full, seeded, created, errors) && created.empty() && errors.empty() && seeded.size() == 4);
    CHECK(full.remove("profile-8", error));
    CHECK(!seedTemplates(full, seeded, created, errors) && created.empty());
    // Room for two: 간단 A and 간단 B go in, 기본 and 상세 are skipped
    const std::string dir4 = makeTempDir();
    ProfileStore almost(dir4);
    almost.load(warnings);
    for (size_t n = 1; n <= ProfileStore::kMaxProfiles - 2; ++n) CHECK(almost.create(variant(n), id, error));
    created.clear();
    seeded.clear();
    CHECK(seedTemplates(almost, seeded, created, errors) && created.size() == 2 && created[1] == "간단 B");
    CHECK(seeded.size() == 4 && !almost.canCreate());
    rmTree(dir3);
    rmTree(dir4);
}

// 9. A profile file that can't be read keeps its number: never written over by New or the template seeding
void testUnreadableKept() {
    const std::string dir = makeTempDir();
    std::vector<std::string> warnings, created, errors, seeded = {"basic", "detailed"};
    std::string id, error;
    // A hand edit with a typo in profile-1.json, and a good profile-3.json
    const std::string typo = "{ \"name\": \"Mine\", \"layout\": { \"columns\": [ [ {\"type\":\"frame\"} ] ] ";
    spit(dir + "/profile-1.json", typo);
    {
        Profile p3;
        p3.name = "Three";
        p3.layout = make({{W::Power}});
        spit(dir + "/profile-3.json", ProfileStore::toJson(p3));
    }
    // And a stray huge profile-7.json: not read whole, kept as it is
    spit(dir + "/profile-7.json", "{\"name\": \"" + std::string(kMaxConfigBytes + 5, 'x') + "\"}");
    ProfileStore store(dir);
    warnings.clear();
    store.load(warnings);
    CHECK(store.profiles().size() == 1 && store.profiles()[0].id == "profile-3");
    CHECK(store.unreadable().size() == 2 && store.unreadable()[0] == "profile-1" && store.unreadable()[1] == "profile-7");
    CHECK(warnings.size() == 2 && warnings[0].find("profile-1") != std::string::npos &&
          warnings[1].find("larger than") != std::string::npos);
    ::unlink((dir + "/profile-7.json").c_str());
    // The seeding (here 간단 A and 간단 B) goes after the last number in use, profile-1.json stays as it was
    CHECK(seedTemplates(store, seeded, created, errors) && errors.empty() && created.size() == 2);
    CHECK(store.findByName("간단 A") != nullptr && store.findByName("간단 A")->id == "profile-4");
    CHECK(store.findByName("간단 B") != nullptr && store.findByName("간단 B")->id == "profile-5");
    CHECK(slurp(dir + "/profile-1.json") == typo);
    // Nothing deletes or saves it either (it isn't a profile the store knows)
    CHECK(!store.remove("profile-1", error) && !store.save("profile-1", make({{W::Cpu}}), error));
    CHECK(slurp(dir + "/profile-1.json") == typo);

    // A file that appeared after load() (written by hand meanwhile) isn't written over either
    spit(dir + "/profile-6.json", "{}");
    CHECK(store.create(make({{W::Battery}}), id, error) && id == "profile-2");
    CHECK(slurp(dir + "/profile-6.json") == "{}");

    // The unreadable file counts against the 8: 6 profiles + profile-1 + profile-6 = 8, so no room
    CHECK(store.create(make({{W::Temp}}), id, error) && id == "profile-7");
    CHECK(store.create(make({{W::Network}}), id, error) && id == "profile-8");
    CHECK(store.profiles().size() == 6 && !store.canCreate());
    id = "untouched";
    CHECK(!store.create(make({{W::General}}), id, error) && !error.empty() && id == "untouched");
    created.clear();
    seeded.clear();
    CHECK(seedTemplates(store, seeded, created, errors) && created.empty() && errors.empty());  // no room: no failure
    CHECK(slurp(dir + "/profile-1.json") == typo && slurp(dir + "/profile-6.json") == "{}");

    // Fixed by hand: the next load reads it again
    Profile fixed;
    fixed.name = "Mine";
    fixed.layout = make({{W::Frame}});
    spit(dir + "/profile-1.json", ProfileStore::toJson(fixed));
    ProfileStore again(dir);
    warnings.clear();
    again.load(warnings);
    CHECK(again.find("profile-1") != nullptr && again.find("profile-1")->name == "Mine");
    CHECK(again.unreadable().size() == 1 && again.unreadable()[0] == "profile-6");
    rmTree(dir);
}

// 10. What Use / Reset would throw away, per target: a moved HUD under a layout-only profile counts for a target with a
// placement; Reset only ever counts the layout
void testLossPerTarget() {
    const std::string dir = makeTempDir();
    std::vector<std::string> warnings;
    std::string a, p, p2, error;
    ProfileStore store(dir);
    store.load(warnings);
    Config base;
    const Placement pl1 = placementOf(base);
    Config moved = base;
    moved.widthM += 0.1;
    const Placement pl2 = placementOf(moved);
    Config other = base;
    other.alpha = 0.5;
    const Placement pl3 = placementOf(other);
    CHECK(!samePlacement(pl1, pl2) && !samePlacement(pl1, pl3) && !samePlacement(pl2, pl3));
    const Layout la = make({{W::SimpleA}});
    const Layout lp = make({{W::Frame, W::Cpu}});
    const Layout lp2 = make({{W::Power}});
    CHECK(store.create(la, a, error, std::nullopt, "간단 A"));
    CHECK(store.create(lp, p, error, pl1, "Desk"));
    CHECK(store.create(lp2, p2, error, pl3, "Bed"));

    // Active 간단 A (layout-only), the HUD moved: the chip stays off (the profile holds no placement)...
    CHECK(!store.unsaved(la, a, &pl2));
    CHECK(store.layoutSaved(la, a) && !store.placementSaved(pl2, a));
    // ...but Use on Desk would lose the moved HUD, Use on 간단 A nothing, Reset nothing
    CHECK(store.useWouldLose(la, pl2, a, p) == (LiveLoss {false, true}));
    CHECK(!store.useWouldLose(la, pl2, a, a).any());
    CHECK(!store.resetWouldLose(la, a).any());
    // Not moved, or moved to just where Desk has it: nothing is lost
    CHECK(!store.useWouldLose(la, pl1, a, p).placement);
    // An edited layout too: both for Desk, the layout for Reset and for 간단 A itself
    const Layout edited = make({{W::SimpleA, W::Battery}});
    CHECK(store.useWouldLose(edited, pl2, a, p) == (LiveLoss {true, true}));
    CHECK(store.useWouldLose(edited, pl2, a, a) == (LiveLoss {true, false}));
    CHECK(store.resetWouldLose(edited, a) == (LiveLoss {true, false}));
    CHECK(!store.resetWouldLose(frame_perf_overlay::defaultLayout(), a).any());  // already the default

    // No active profile: a placement isn't saved anywhere, the layout is when some profile has it
    CHECK(store.useWouldLose(la, pl2, "", p) == (LiveLoss {false, true}));
    CHECK(store.useWouldLose(edited, pl2, "", p) == (LiveLoss {true, true}));
    CHECK(store.useWouldLose(la, pl2, "profile-8", p) == (LiveLoss {false, true}));  // a gone id is none

    // Active Desk (with a placement): the placement counts when it differs from Desk's
    CHECK(!store.useWouldLose(lp, pl1, p, p2).any());                       // saved in Desk
    CHECK(store.useWouldLose(lp, pl2, p, p2) == (LiveLoss {false, true}));  // moved since
    CHECK(store.useWouldLose(lp, pl2, p, p) == (LiveLoss {false, true}));   // Use on Desk itself puts it back
    CHECK(!store.useWouldLose(lp, pl2, p, a).any());                        // 간단 A keeps the placement
    CHECK(!store.resetWouldLose(lp, p).any());                              // Reset: the layout is saved

    // A target that doesn't exist: nothing to ask about
    CHECK(!store.useWouldLose(edited, pl2, a, "profile-7").any());
    rmTree(dir);
}

// 11. The errors the tab shows are short Korean ones; the OS's reason is kept for the log (lastErrorDetail)
void testKoreanErrors() {
    const std::string dir = makeTempDir();
    std::vector<std::string> warnings;
    std::string id, error;
    const auto korean = [](const std::string& text) {
        for (const char c : text) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return false;  // no English words
        }
        return !text.empty();
    };
    // A folder that can't be made (a file is in the way): the write fails
    spit(dir + "/blocker", "x");
    ProfileStore blocked(dir + "/blocker/profiles");
    blocked.load(warnings);
    CHECK(!blocked.create(make({{W::Frame}}), id, error));
    CHECK(error == "파일을 쓰지 못했어요" && korean(error));
    CHECK(blocked.lastErrorDetail().find("blocker") != std::string::npos);  // the path, for the log

    ProfileStore store(dir);
    store.load(warnings);
    CHECK(!store.save("profile-4", make({{W::Frame}}), error) && error == "그 프로필이 없어요" && korean(error));
    CHECK(!store.rename("profile-4", "x", error) && korean(error));
    CHECK(!store.remove("profile-4", error) && korean(error));
    CHECK(store.lastErrorDetail().find("profile-4") != std::string::npos);
    // A success clears the detail; the default name is Korean too
    CHECK(store.create(make({{W::Frame}}), id, error) && store.lastErrorDetail().empty());
    CHECK(store.find(id)->name == "프로필 1");
    // A file without a name loads as "프로필 N"
    spit(dir + "/profile-2.json", "{ \"layout\": { \"columns\": [ [ {\"type\":\"cpu\"} ] ] } }");
    ProfileStore again(dir);
    again.load(warnings);
    CHECK(again.find("profile-2") != nullptr && again.find("profile-2")->name == "프로필 2");
    rmTree(dir);  // (the blocker file too)
}

}  // namespace

int main() {
    testRoundTrip();
    testCap();
    testRenameDelete();
    testFilesInDir();
    testUnsaved();
    testEmptyProfile();
    testResetLeavesProfiles();
    testPlacement();
    testSeedTemplates();
    testUnreadableKept();
    testLossPerTarget();
    testKoreanErrors();
    if (failures == 0) std::printf("profile-store-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
