// The sound backend's pure parts. See audio_sinks.h.
#include "audio_sinks.h"

#include <cmath>
#include <limits>

namespace audio {

namespace {

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool has(const std::string& s, const char* part) {
    return s.find(part) != std::string::npos;
}

std::string prop(const SinkInfo& sink, const char* key) {
    const auto it = sink.props.find(key);
    return it != sink.props.end() ? it->second : std::string();
}

/** SteamOS's loopback over the platform speaker (spatial-node-config.lua: alsa_loopback_device.stereo.<raw>). */
bool isSpeakerLoopback(const SinkInfo& sink) {
    return startsWith(sink.name, "alsa_loopback_device.") && has(sink.name, ".alsa_output.platform-");
}

bool isRawSpeaker(const SinkInfo& sink) {
    return startsWith(sink.name, "alsa_output.platform-");
}

/** Any of SteamOS's loopbacks over a card's sink (the speaker's, or since the 0.5.7 beta a USB card's too). */
bool isCardLoopback(const SinkInfo& sink) {
    return startsWith(sink.name, "alsa_loopback_device.") && has(sink.name, ".alsa_output.");
}

bool hasSpeakerLoopback(const std::vector<SinkInfo>& sinks) {
    for (const SinkInfo& s : sinks) {
        if (isSpeakerLoopback(s)) return true;
    }
    return false;
}

}  // namespace

SinkKind classify(const SinkInfo& sink, bool speakerLoopback) {
    if (isSpeakerLoopback(sink)) return SinkKind::Speaker;
    if (isRawSpeaker(sink)) return speakerLoopback ? SinkKind::Hidden : SinkKind::Speaker;
    // A USB card's loopback (the 0.5.7 beta, 2026-11-05: alsa.loopback, node.virtual false): it is that card
    if (isCardLoopback(sink)) return SinkKind::Output;
    // Filters, echo cancelling, spatializers, other loopbacks, null sinks: not something to listen on
    if (prop(sink, "node.virtual") == "true" || prop(sink, "alsa.loopback") == "true") return SinkKind::Hidden;
    if (!prop(sink, "filter.smart").empty() || !prop(sink, "steamos.mic_filter").empty()) return SinkKind::Hidden;
    if (has(sink.name, "filter-chain") || has(sink.name, "echo_cancel") || has(sink.name, "spatializ") ||
        has(sink.name, ".monitor")) {
        return SinkKind::Hidden;
    }
    const std::string api = prop(sink, "device.api");
    return api == "alsa" || api == "bluez5" ? SinkKind::Output : SinkKind::Hidden;
}

std::string deviceKey(const std::string& name) {
    for (const char* prefix : {"alsa_output.", "alsa_input.", "bluez_output.", "bluez_input."}) {
        const size_t at = name.find(prefix);
        if (at == std::string::npos) continue;
        const size_t start = at + std::string(prefix).size();
        const size_t end = name.find('.', start);
        return name.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
    return "";
}

std::string micFor(const std::string& sink, const std::vector<std::string>& sources, const std::string& speakerSink) {
    const auto onHardware = [&](const std::string& key) -> std::string {
        if (key.empty()) return "";
        std::string found;
        for (const std::string& source : sources) {
            if (has(source, ".monitor") || !has(source, "_input.") || deviceKey(source) != key) continue;
            if (startsWith(source, "alsa_loopback_device.")) return source;
            if (found.empty()) found = source;
        }
        return found;
    };
    // Bluetooth: the headset's microphone on purpose. A Bluetooth microphone would make WirePlumber switch the
    // earphones to the headset profile (HSP/HFP: mono, low quality) whenever anything records
    // (bluetooth.autoswitch-to-headset-profile, on by default in WirePlumber 0.5.14); its source is also named
    // bluez_input.AA:BB:… while the sink is bluez_output.AA_BB_….N
    const std::string own = startsWith(sink, "bluez_output.") ? std::string() : onHardware(deviceKey(sink));
    return !own.empty() ? own : onHardware(deviceKey(speakerSink));
}

AudioState stateFrom(const std::vector<SinkInfo>& sinks, const std::string& defaultSink) {
    AudioState state;
    state.known = true;
    const bool loopback = hasSpeakerLoopback(sinks);
    const SinkInfo* current = nullptr;
    const SinkInfo* rawDefault = nullptr;
    // The cards a loopback stands for: their raw sinks (if a client can see them) are hidden, a raw default maps to it
    std::vector<const SinkInfo*> loopbacks;
    for (const SinkInfo& s : sinks) {
        if (isCardLoopback(s)) loopbacks.push_back(&s);
    }
    // By the whole raw name (alsa_loopback_device.[stereo.]<raw>): a card with two sinks (analog-stereo, mono-chat)
    // keeps one row each
    const auto loopbackOver = [&](const SinkInfo& raw) -> const SinkInfo* {
        if (!startsWith(raw.name, "alsa_output.")) return nullptr;
        const std::string tail = "." + raw.name;
        for (const SinkInfo* l : loopbacks) {
            if (l->name.size() > tail.size() && l->name.compare(l->name.size() - tail.size(), tail.size(), tail) == 0) {
                return l;
            }
        }
        return nullptr;
    };
    for (const SinkInfo& s : sinks) {
        const SinkKind kind = classify(s, loopback);
        if (s.name == defaultSink) rawDefault = &s;
        if (kind == SinkKind::Hidden || loopbackOver(s) != nullptr) continue;
        AudioOutput output;
        output.id = s.name;
        output.name = s.description;
        output.builtin = kind == SinkKind::Speaker;
        state.outputs.push_back(output);
        if (s.name == defaultSink) current = &s;
    }
    // A default the page hides: a card's raw sink stands for its loopback; anything else (a filter made default by
    // hand) selects nothing, but its volume is still the one that plays
    if (current == nullptr && rawDefault != nullptr) current = loopbackOver(*rawDefault);
    if (current != nullptr) state.defaultId = current->name;
    const SinkInfo* level = current != nullptr ? current : rawDefault;
    if (level != nullptr) {
        state.volume = percentOf(level->volume);
        state.muted = level->muted;
    }
    return state;
}

int percentOf(uint32_t volume) {
    const double percent = std::round(static_cast<double>(volume) * 100.0 / kVolumeNorm);
    return percent > 100 ? 100 : static_cast<int>(percent);
}

uint32_t volumeOf(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return static_cast<uint32_t>(std::lround(static_cast<double>(kVolumeNorm) * percent / 100.0));
}

int VolumeThrottle::push(int percent, bool final, double now) {
    if (final || now - lastSent_ >= kMinIntervalSec) {
        waiting_ = -1;
        lastSent_ = now;
        return percent;
    }
    waiting_ = percent;
    return -1;
}

int VolumeThrottle::sendDue(double now) {
    if (waiting_ < 0 || now < lastSent_ + kMinIntervalSec) return -1;
    const int value = waiting_;
    waiting_ = -1;
    lastSent_ = now;
    return value;
}

double VolumeThrottle::nextDue() const {
    return waiting_ < 0 ? std::numeric_limits<double>::infinity() : lastSent_ + kMinIntervalSec;
}

}  // namespace audio
