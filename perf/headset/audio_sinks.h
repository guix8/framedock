// The 기기 page's sound backend, the parts without libpulse: which sinks are outputs (and which one is the headset's
// speaker), the volume in percent, and the volume drag's throttle. pulse_audio.cpp feeds them what the server says;
// the tests feed them recorded `pactl list sinks` data.
//
// What counts as an output on SteamOS (2026-10-30, SteamOS 0.4.5 / PipeWire 1.6.8 / WirePlumber 0.5.14):
// - The speaker's real sink, alsa_output.platform-sound.HiFi__Speaker__sink, is hidden from ordinary clients
//   (/etc/wireplumber/wireplumber.conf.d/10-access.conf gives it no permissions). WirePlumber's
//   /etc/wireplumber/scripts/spatial-node-config.lua puts a loopback in front of it,
//   alsa_loopback_device.stereo.<that name> (".71." in surround mode), with priority.session one above the speaker:
//   that loopback is "Built-in Audio Playback" for Pulse clients, the default this Frame had before the USB dongle
//   (WirePlumber's default-nodes history) and what the spatializer treats as the speaker. It is the headset speaker.
// - Filters (filter-chain-sink, echo_cancel_sink, the spatializers), other loopbacks and null sinks are virtual:
//   hidden. A real device sink (device.api alsa or bluez5: USB, Bluetooth) is an output.
// - Since the 0.5.7 beta (2026-11-05) a USB card gets a loopback too,
//   alsa_loopback_device.alsa_output.<card>.<profile>, and its raw sink is hidden from clients like the speaker's:
//   the loopback is that output. A raw sink a client can still see next to its loopback is hidden (one row per
//   sink), and as the default it maps to that loopback.
#pragma once

#include "device_model.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace audio {

/** One sink as the sound server describes it. */
struct SinkInfo {
    std::string name;
    std::string description;
    std::map<std::string, std::string> props;  ///< its property list (node.virtual, device.api, ...)
    uint32_t volume = 0;                       ///< the loudest channel (pa_volume_t: 0x10000 = 100 %)
    bool muted = false;
};

enum class SinkKind { Hidden, Output, Speaker };

/**
 * What a sink is on the page.
 * @param speakerLoopback whether the list has SteamOS's loopback over the speaker (then the raw speaker sink, if a
 *        client can see it at all, is hidden: the loopback stands for it)
 */
SinkKind classify(const SinkInfo& sink, bool speakerLoopback);

/** @return the page's state: the outputs in the server's order, the default (a hidden default maps to its output) */
AudioState stateFrom(const std::vector<SinkInfo>& sinks, const std::string& defaultSink);

/**
 * The part of a node name that names the hardware: what follows alsa_output. / alsa_input. / bluez_output. /
 * bluez_input., up to the next '.' ("platform-sound" for the headset, "usb-Generic_Wireless_Headset_…-00" for a
 * USB dongle, a Bluetooth address). "" for anything else (filters, echo cancelling, null sinks).
 */
std::string deviceKey(const std::string& name);

/**
 * The microphone that goes with an output, so switching the output switches the microphone too (the user's wish,
 * 2026-10-30): a source on the same hardware as sink (SteamOS's loopback over it first, like the speaker's);
 * when that hardware has none (a speaker without a microphone) or it is Bluetooth (see audio_sinks.cpp), the
 * headset's own microphone (speakerSink's hardware). Only inputs count (alsa_input. / bluez_input.), never monitors.
 * @param sources the server's sources, by name, in its order
 * @return the source's name, or "" (none fits: leave the default microphone as it is)
 */
std::string micFor(const std::string& sink, const std::vector<std::string>& sources, const std::string& speakerSink);

/** pa_volume_t's 100 % */
constexpr uint32_t kVolumeNorm = 0x10000;
/** @return a volume in percent (Pulse's linear-in-the-cubic scale, as pactl and Steam show it), rounded, 0-100 */
int percentOf(uint32_t volume);
/** @return the volume for percent (0-100) */
uint32_t volumeOf(int percent);

/**
 * The volume drag: values go out at most every kMinIntervalSec while dragging; the last one (and the release's
 * final value) always goes out. Pure: the caller asks what is due.
 */
class VolumeThrottle {
public:
    static constexpr double kMinIntervalSec = 1.0 / 15;

    /**
     * A new value from the slider.
     * @return the value to send now, or -1 (it waits: sendDue() gives it when its time comes)
     */
    int push(int percent, bool final, double now);
    /** @return the waiting value if its time came (it is then sent), else -1 */
    int sendDue(double now);
    /** @return when sendDue() has something (+infinity: nothing waits) */
    double nextDue() const;

private:
    double lastSent_ = -1e300;
    int waiting_ = -1;
};

}  // namespace audio
