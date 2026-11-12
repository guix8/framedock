#include "profile_store.h"

#include "app_text.h"
#include "atomic_file.h"
#include "json.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

constexpr const char* kPrefix = "profile-";

/** @return N of "profile-N", 0 if the id isn't one */
int idNumber(const std::string& id) {
    if (id.rfind(kPrefix, 0) != 0 || id.size() == 8 || id.size() > 10) return 0;
    int n = 0;
    for (size_t i = 8; i < id.size(); ++i) {
        if (id[i] < '0' || id[i] > '9') return 0;
        n = n * 10 + (id[i] - '0');
    }
    return n >= 1 && n <= static_cast<int>(ProfileStore::kMaxProfiles) ? n : 0;
}

/** @return true if something (a file we can read or not) is at path */
bool onDisk(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

}  // namespace

ProfileStore::ProfileStore(std::string dir) : dir_(std::move(dir)) {}

std::string ProfileStore::pathOf(const std::string& id) const {
    return dir_ + "/" + id + ".json";
}

std::string ProfileStore::cleanName(const std::string& name) {
    const size_t a = name.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    std::string out = name.substr(a, name.find_last_not_of(" \t\r\n") - a + 1);
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';  // a pasted newline or tab
    }
    if (out.size() > kMaxNameBytes) {
        size_t cut = kMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;  // not inside a character
        out.resize(cut);
    }
    return out;
}

std::string ProfileStore::toJson(const Profile& profile) {
    std::string out = "{\n  \"name\": " + jsonString(profile.name) +
                      ",\n  \"layout\": " + frame_perf_overlay::layoutToJson(profile.layout, 2);
    if (profile.placement) out += ",\n  \"placement\": " + placementToJson(*profile.placement, 2);
    return out + "\n}\n";
}

bool ProfileStore::fromJson(const std::string& text, Profile& out, std::vector<std::string>& warnings,
                            std::string& error) {
    JsonValue root;
    if (!parseJson(text, root, error)) return false;
    const JsonValue* name = root.get("name");
    const JsonValue* layout = root.get("layout");
    if (layout == nullptr) {
        error = "no \"layout\"";
        return false;
    }
    Profile p;
    if (!frame_perf_overlay::layoutFromJson(*layout, p.layout, warnings, error)) return false;
    p.name = name != nullptr && name->isString() ? cleanName(name->text) : "";
    if (const JsonValue* placement = root.get("placement")) {
        Placement read;
        std::string placementError;
        if (placementFromJson(*placement, read, warnings, placementError)) {
            p.placement = read;
        } else {
            warnings.push_back(placementError + ": the profile keeps only its layout");
        }
    }
    out.name = p.name;
    out.layout = p.layout;
    out.placement = p.placement;
    return true;
}

void ProfileStore::load(std::vector<std::string>& warnings) {
    profiles_.clear();
    unreadable_.clear();
    for (size_t n = 1; n <= kMaxProfiles; ++n) {
        Profile p;
        p.id = kPrefix + std::to_string(n);
        if (!onDisk(pathOf(p.id))) continue;
        // A file that is there but can't be read or isn't a profile (a typo in a hand edit) keeps its id: it's left
        // as it is, and create() never writes over it
        std::ifstream file(pathOf(p.id), std::ios::binary);
        if (!file) {
            unreadable_.push_back(p.id);
            warnings.push_back(pathOf(p.id) + ": can't be opened (left as it is, its number isn't reused)");
            continue;
        }
        // At most kMaxConfigBytes (+1 to tell a larger file): a stray huge file isn't read whole
        std::string text;
        char chunk[65536];
        while (text.size() <= kMaxConfigBytes && file.read(chunk, sizeof(chunk)).gcount() > 0) {
            text.append(chunk, static_cast<size_t>(file.gcount()));
        }
        if (text.size() > kMaxConfigBytes) {
            unreadable_.push_back(p.id);
            warnings.push_back(pathOf(p.id) + ": larger than " + std::to_string(kMaxConfigBytes >> 20) +
                               " MiB (skipped and left as it is, its number isn't reused)");
            continue;
        }
        std::string error;
        std::vector<std::string> notes;
        if (!fromJson(text, p, notes, error)) {
            unreadable_.push_back(p.id);
            warnings.push_back(pathOf(p.id) + ": " + error + " (skipped and left as it is, its number isn't reused)");
            continue;
        }
        for (const std::string& w : notes) warnings.push_back(pathOf(p.id) + ": " + w);
        if (p.name.empty()) p.name = trf(Tx::ProfileDefaultName, {std::to_string(n)});
        profiles_.push_back(p);
    }
    ++revision_;
}

size_t ProfileStore::freeNumber() const {
    for (size_t n = 1; n <= kMaxProfiles; ++n) {
        const std::string id = kPrefix + std::to_string(n);
        // Not a loaded profile, and no file at all: an unreadable file, or one that appeared since load(), stays
        if (find(id) == nullptr && !onDisk(pathOf(id))) return n;
    }
    return 0;
}

const Profile* ProfileStore::find(const std::string& id) const {
    for (const Profile& p : profiles_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const Profile* ProfileStore::findByName(const std::string& name) const {
    for (const Profile& p : profiles_) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

bool ProfileStore::unsaved(const frame_perf_overlay::Layout& live, const std::string& activeId,
                           const Placement* livePlacement) const {
    const auto same = [&](const Profile& p) {
        if (!live.sameWidgets(p.layout)) return false;
        return livePlacement == nullptr || !p.placement || samePlacement(*livePlacement, *p.placement);
    };
    if (const Profile* active = find(activeId)) return !same(*active);
    return std::none_of(profiles_.begin(), profiles_.end(), same);
}

bool ProfileStore::layoutSaved(const frame_perf_overlay::Layout& live, const std::string& activeId) const {
    if (const Profile* active = find(activeId)) return live.sameWidgets(active->layout);
    return std::any_of(profiles_.begin(), profiles_.end(),
                       [&](const Profile& p) { return live.sameWidgets(p.layout); });
}

bool ProfileStore::placementSaved(const Placement& livePlacement, const std::string& activeId) const {
    const Profile* active = find(activeId);
    return active != nullptr && active->placement && samePlacement(livePlacement, *active->placement);
}

LiveLoss ProfileStore::useWouldLose(const frame_perf_overlay::Layout& live, const Placement& livePlacement,
                                    const std::string& activeId, const std::string& targetId) const {
    LiveLoss loss;
    const Profile* target = find(targetId);
    if (target == nullptr) return loss;
    loss.layout = !live.sameWidgets(target->layout) && !layoutSaved(live, activeId);
    loss.placement = target->placement && !samePlacement(livePlacement, *target->placement) &&
                     !placementSaved(livePlacement, activeId);
    return loss;
}

LiveLoss ProfileStore::resetWouldLose(const frame_perf_overlay::Layout& live, const std::string& activeId) const {
    LiveLoss loss;
    loss.layout = !live.sameWidgets(frame_perf_overlay::defaultLayout()) && !layoutSaved(live, activeId);
    return loss;
}

Profile* ProfileStore::findMutable(const std::string& id) {
    for (Profile& p : profiles_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

bool ProfileStore::write(const Profile& profile, std::string& error) {
    if (frame_perf_overlay::atomicWriteFile(pathOf(profile.id), toJson(profile), detail_)) return true;
    error = tr(Tx::ProfileWriteFailed);  // the OS's reason (English, strerror) goes to the log: lastErrorDetail()
    return false;
}

bool ProfileStore::fail(std::string& error, const std::string& message, const std::string& detail) {
    error = message;
    detail_ = detail;
    return false;
}

bool ProfileStore::create(const frame_perf_overlay::Layout& layout, std::string& id, std::string& error,
                          const std::optional<Placement>& placement, const std::string& name, bool atEnd) {
    detail_.clear();
    size_t n = freeNumber();
    if (atEnd && n != 0) {
        // After the highest number a profile or a file has (the list is in number order)
        size_t last = 0;
        for (size_t k = 1; k <= kMaxProfiles; ++k) {
            const std::string taken = kPrefix + std::to_string(k);
            if (find(taken) != nullptr || onDisk(pathOf(taken))) last = k;
        }
        if (last < kMaxProfiles) n = last + 1;
    }
    if (n == 0) {
        return fail(error, trf(Tx::ProfileTooMany, {std::to_string(kMaxProfiles)}),
                    "all " + std::to_string(kMaxProfiles) + " profile numbers are used (or taken by unreadable files)");
    }
    Profile p;
    p.id = kPrefix + std::to_string(n);
    p.name = cleanName(name);
    if (p.name.empty()) p.name = trf(Tx::ProfileDefaultName, {std::to_string(n)});
    p.layout = layout;
    p.placement = placement;
    if (!write(p, error)) return false;
    profiles_.push_back(p);
    std::sort(profiles_.begin(), profiles_.end(),
              [](const Profile& a, const Profile& b) { return idNumber(a.id) < idNumber(b.id); });
    id = p.id;
    ++revision_;
    return true;
}

bool ProfileStore::save(const std::string& id, const frame_perf_overlay::Layout& layout, std::string& error,
                        const std::optional<Placement>& placement) {
    Profile* p = findMutable(id);
    detail_.clear();
    if (p == nullptr) return fail(error, tr(Tx::ProfileMissing), "no profile " + id);
    Profile next = *p;
    next.layout = layout;
    if (placement) next.placement = placement;
    if (!write(next, error)) return false;
    *p = next;
    ++revision_;
    return true;
}

bool ProfileStore::rename(const std::string& id, const std::string& name, std::string& error) {
    Profile* p = findMutable(id);
    detail_.clear();
    if (p == nullptr) return fail(error, tr(Tx::ProfileMissing), "no profile " + id);
    const std::string clean = cleanName(name);
    if (clean.empty() || clean == p->name) return true;
    Profile next = *p;
    next.name = clean;
    if (!write(next, error)) return false;
    *p = next;
    ++revision_;
    return true;
}

bool ProfileStore::remove(const std::string& id, std::string& error) {
    const auto it = std::find_if(profiles_.begin(), profiles_.end(), [&](const Profile& p) { return p.id == id; });
    detail_.clear();
    if (it == profiles_.end()) return fail(error, tr(Tx::ProfileMissing), "no profile " + id);
    if (std::remove(pathOf(id).c_str()) != 0) {
        const int why = errno;
        if (onDisk(pathOf(id))) return fail(error, tr(Tx::ProfileDeleteFailed), pathOf(id) + ": " + std::strerror(why));
    }
    profiles_.erase(it);
    ++revision_;
    return true;
}

std::vector<ProfileTemplate> builtInTemplates() {
    using frame_perf_overlay::WidgetType;
    using frame_perf_overlay::WidgetInstance;
    const auto widget = [](WidgetType type, bool compact = true) {
        WidgetInstance w;
        w.type = type;
        w.compact = compact;
        return w;
    };
    const auto single = [&](WidgetType type) {
        frame_perf_overlay::Layout layout;
        layout.columns.push_back({{widget(type)}});
        return layout;
    };
    // 상세: every card with its extra rows, and CPU 상위 (5 rows, one per name, the system left out)
    frame_perf_overlay::Layout detailed;
    WidgetInstance cpuItems = widget(WidgetType::CpuItems);
    cpuItems.maxItems = 5;
    cpuItems.sumSameName = true;
    cpuItems.hideSystem = true;
    detailed.columns.push_back({{widget(WidgetType::Frame, false), widget(WidgetType::Cpu, false),
                                 widget(WidgetType::Temp, false), widget(WidgetType::Network)}});
    detailed.columns.push_back({{widget(WidgetType::GpuRam), widget(WidgetType::Power),
                                 widget(WidgetType::Battery, false), cpuItems, widget(WidgetType::General, false)}});
    // Named in the UI language they are first added in (the widgets' own titles)
    const frame_perf_overlay::WidgetText& t = frame_perf_overlay::widgetText(widgetLanguage(uiLanguage()));
    return {{"simple_a", t.simpleA, single(WidgetType::SimpleA)},
            {"simple_b", t.simpleB, single(WidgetType::SimpleB)},
            {"basic", tr(Tx::ProfileBasic), frame_perf_overlay::defaultLayout()},
            {"detailed", tr(Tx::ProfileDetailed), detailed}};
}

bool seedTemplates(ProfileStore& store, std::vector<std::string>& seeded, std::vector<std::string>& created,
                   std::vector<std::string>& errors) {
    bool changed = false;
    for (const ProfileTemplate& t : builtInTemplates()) {
        if (std::find(seeded.begin(), seeded.end(), t.id) != seeded.end()) continue;  // offered before
        // The user's own name wins, and no room is no room: skipped for good, like one offered
        if (store.findByName(t.name) == nullptr && store.canCreate()) {
            std::string id, error;
            if (!store.create(t.layout, id, error, std::nullopt, t.name, true)) {
                errors.push_back(t.name + ": " + error + " (" + store.lastErrorDetail() + ")");
                continue;  // tried again on the next start
            }
            created.push_back(t.name);
        }
        seeded.push_back(t.id);
        changed = true;
    }
    return changed;
}
