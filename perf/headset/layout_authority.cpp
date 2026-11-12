#include "layout_authority.h"

#include <cstdio>
#include <utility>

LayoutAuthority::LayoutAuthority(Config& config, SaveFn save, ApplyFn apply)
    : config_(config), save_(std::move(save)), apply_(std::move(apply)) {}

LayoutAuthority::Result LayoutAuthority::trySetLayout(uint32_t baseVersion, const frame_perf_overlay::Layout& layout) {
    if (baseVersion != config_.layout.version) {
        Result result;
        result.outcome = Outcome::Stale;
        result.version = config_.layout.version;
        return result;
    }
    return commit(config_, layout);
}

LayoutAuthority::Result LayoutAuthority::useLayout(const frame_perf_overlay::Layout& target,
                                                   const std::string& activeProfile,
                                                   const std::optional<Placement>& placement) {
    Config next = config_;
    next.activeProfile = activeProfile;
    if (placement) applyPlacement(*placement, next);
    return commit(next, target);
}

LayoutAuthority::Result LayoutAuthority::commit(const Config& base, const frame_perf_overlay::Layout& layout) {
    Result result;
    const uint32_t current = config_.layout.version;
    // Save first, from a copy: a failed save leaves the live config and the file as they were
    Config next = base;
    next.layout = layout;
    next.layout.version = current + 1;
    if (save_ && !save_(next, result.error)) {
        std::fprintf(stderr, "[layout] Edit on version %u not applied: saving failed: %s\n", current,
                     result.error.c_str());
        result.outcome = Outcome::SaveFailed;
        result.version = current;
        return result;
    }
    config_ = next;  // the layout, and what useLayout changed (active profile, placement)
    if (apply_) apply_(config_.layout);
    result.outcome = Outcome::Applied;
    result.version = config_.layout.version;
    std::fprintf(stderr, "[layout] Applied an edit: version %u -> %u\n", current, result.version);
    return result;
}
