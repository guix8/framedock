// The size the game in front renders each eye at, for the Frame card (2026-11-13). SteamVR's recommended size isn't
// it: Eleven Table Tennis renders 2400x2400 where SteamVR recommends 1728x1728.
//
// - A PC stream (vrlink runs): only the PC knows. Frame Dock's optional PC helper (pc-helper/framedock-pc.cs) answers
//   "GET /render" on port 27463 with {"v":1,"app":"<key>","width":W,"height":H}, or {"v":1,"app":null} while no
//   streamed game runs. The PC's address is the peer of vrlink.txt's latest "SVLDataLinkTransferUDP::SetPeer <here> ->
//   <peer>" lines; the one that answered is asked first next time.
//   2026-11-15: the helper of Frame Dock 0.1.3 on also answers "GET /v2/state": {"v":2,"render":<the /render body>,
//   "vr":bool,"hz":..,"fps":..,"gpuMs":..,...}, any value null while unknown. That is asked first; an older helper
//   says 404 there and is then asked "/render" (and "/v2/state" again every kStateProbeSec, in case it was updated).
//   The request's TCP connect time is the headset-to-PC round trip: the "ping".
//   Its "last" sums up the stream session that ended last on the PC (SteamVR writes that when a stream ends). The
//   helper is asked only during a stream, so once one has ended it is asked a few more times (AfterStream) for that
//   session's summary. Otherwise nothing is asked while standalone.
// - Standalone: the layer (third_party/vkbasalt, vrcompositor only) writes "W H <unix seconds>" to
//   $XDG_RUNTIME_DIR/framecolor/render-size when a game's eye textures are created. Taken only while a scene app runs
//   and only if written after that app's process started, so a previous game's size isn't shown.
//
// RenderSizeReader blocks for up to a few hundred ms (the LAN request): AsyncSampler's thread calls it.
#pragma once

#include "auto_profile.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace render_size {

/** The PC helper's port (pc-helper/framedock-pc.cs) */
constexpr int kHelperPort = 27463;

/** What the helper said. */
enum class HelperReply { Invalid, NoGame, Game };

/**
 * Parses the helper's JSON, defensively: version 1 only, sizes 1-16384.
 * @param body the response body
 * @param width,height set for Game
 * @return Game with a size, NoGame for "app":null, Invalid for anything else
 */
HelperReply parseHelperReply(const std::string& body, int& width, int& height);

/**
 * The PC addresses in vrlink.txt: the peer (after "->") of each SetPeer line, newest line first, each address once.
 * @param logText the log (or its tail)
 * @return IPv4 addresses
 */
std::vector<std::string> vrlinkPeers(const std::string& logText);

/**
 * Parses the layer's file, "W H <unix seconds>\n".
 * @return false for anything else (sizes 1-16384)
 */
bool parseLayerFile(const std::string& text, int& width, int& height, int64_t& writtenUnix);

/**
 * When a process started, in Unix seconds.
 * @param statText /proc/<pid>/stat (field 22: start time in clock ticks after boot)
 * @param uptimeS /proc/uptime's first number (since boot, sleep included, like the start time)
 * @param nowUnix the wall clock now
 * @param ticksPerSec sysconf(_SC_CLK_TCK)
 * @return NaN when statText can't be parsed
 */
double processStartUnix(const std::string& statText, double uptimeS, double nowUnix, long ticksPerSec);

/**
 * The staleness rule: the layer's size belongs to the scene app only if written after it started. The file has whole
 * seconds and the start time is an estimate (clock ticks against uptime), hence kStartSlackS.
 * @param writtenUnix the file's time
 * @param startUnix processStartUnix() of the scene app (NaN: never current)
 */
bool layerSizeIsCurrent(int64_t writtenUnix, double startUnix);
constexpr double kStartSlackS = 2.0;

/** The helper's "last": a stream session that has ended, as SteamVR on the PC counted its frames. */
struct LastSession {
    static constexpr size_t kMaxCauses = 3;
    int64_t endedUnix = 0;              ///< when, Unix seconds on the PC's clock; 0: none
    int64_t frames = 0;
    double linkBadPct = std::nan("");   ///< frames the viewer saw late or not at all, % of all
    double pcMissedPct = std::nan("");  ///< frames the PC had no new picture to send for, % of all
    std::vector<std::string> causes;    ///< SteamVR's names for what it blames for the link's bad frames, the most
                                        ///< frequent first, kMaxCauses at most ("DecodeSlow")
};

/** The helper's /v2/state values about the stream's source. NaN: null, missing, or out of range. */
struct PcState {
    bool vr = false;  ///< the helper is attached to SteamVR
    double hz = std::nan("");
    double fps = std::nan("");        ///< the game's fps on the PC
    double missedPct = std::nan("");  ///< 1 - fps / hz, the helper's
    double gpuMs = std::nan("");
    double gpuMaxMs = std::nan("");
    double cpuMs = std::nan("");
    double cpuMaxMs = std::nan("");
    double cpuPct = std::nan("");  ///< the PC's CPU load
    double gpuPct = std::nan("");
    double gpuTempC = std::nan("");
    double vramUsedMb = std::nan("");
    double vramTotalMb = std::nan("");
    double linkMbit = std::nan("");
    LastSession last;  ///< the session that ended last (endedUnix 0: none, or not understood)
};

/** The frame values (fps ... cpuMaxMs) are dropped when the helper read them longer ago than this ("ageMs"). */
constexpr double kMaxFrameAgeMs = 3000.0;

/**
 * Parses the helper's /v2/state body, defensively: version 2 only, "render" by parseHelperReply's rules, every other
 * value on its own (a wrong type or a value out of range is NaN, not a failure).
 * @param body the response body
 * @param width,height set for Game
 * @param pc set unless Invalid
 * @return as parseHelperReply, for "render"
 */
HelperReply parseHelperState(const std::string& body, int& width, int& height, PcState& pc);

/** Which helper a peer runs: /v2/state is asked unless that peer said 404 to it less than kStateProbeSec ago. */
class HelperVersion {
public:
    static constexpr double kStateProbeSec = 60.0;
    /** @return whether to ask the peer /v2/state (false: /render straight away, it is an old helper) */
    bool askState(const std::string& peer, double now) const { return peer != oldPeer_ || now >= probeAt_; }
    /**
     * The peer answered /v2/state.
     * @param status the HTTP status (0 = no answer)
     * @return true for 404: an old helper, ask it /render
     */
    bool stateAnswered(const std::string& peer, long status, double now);

private:
    std::string oldPeer_;  ///< the peer that said 404 to /v2/state
    double probeAt_ = 0.0;
};

/**
 * When to ask the helper for the summary of the stream that just ended. During a stream its "last" is the session
 * before; SteamVR writes the stream's own when it ends. So once the stream is seen to end the helper is asked again,
 * every kEverySec for kWindowSec at most, until it names a later session than it did when the stream began. (Its own
 * times are compared with each other: the headset's clock and the PC's needn't agree.) Only after a stream whose
 * helper answered /v2/state.
 */
class AfterStream {
public:
    static constexpr double kWindowSec = 90.0;
    static constexpr double kEverySec = 5.0;
    /** A stream begins: nothing is known of it. */
    void started();
    /** During the stream the helper's "last" ended then (0: it has none). The first such answer counts. */
    void during(int64_t endedUnix);
    /** The stream was seen to end. */
    void ended(double now);
    /** Standalone: @return whether to ask the helper now (the next time is then kEverySec later) */
    bool ask(double now);
    /** @return whether a "last" that ended then is the stream's own session (later than the one before it) */
    bool isNew(int64_t endedUnix) const { return known_ && endedUnix > before_; }
    /** The summary is here, or this helper won't give one: nothing more is asked. */
    void done() { until_ = -1e300; }

private:
    bool known_ = false;     ///< a helper answered /v2/state during the stream
    int64_t before_ = 0;     ///< its "last" then: the session before this stream (0: none)
    double until_ = -1e300;  ///< asking ends then (-1e300: not asking)
    double nextAt_ = 0.0;
};

/** The last kSamples connect times; their median rides out one slow connect (Wi-Fi power save, a busy PC). */
class PingMedian {
public:
    static constexpr size_t kSamples = 5;
    /** @param ms NaN is skipped */
    void add(double ms);
    void clear() { samples_.clear(); }
    /** @return NaN while empty */
    double median() const;

private:
    std::vector<double> samples_;  ///< oldest first
};

/** One reading; source is a frame_perf_overlay::RenderSource value (0 = nothing known). */
struct Reading {
    int width = 0;
    int height = 0;
    uint32_t source = 0;
    uint32_t stream = 0;           ///< a frame_perf_overlay::StreamState value
    PcState pc;                    ///< for StreamState::Helper
    double pingMs = std::nan("");  ///< while a helper answers (old or new)
    LastSession last;              ///< while not streaming: the stream that ended last (endedUnix 0: not known)
};

/** Reads the render size (AsyncSampler's thread, one at a time). */
class RenderSizeReader {
public:
    /** After a PC that didn't answer, ask again only this much later (a firewall drops, so each try waits). */
    static constexpr double kRetrySec = 15.0;
    static constexpr long kTimeoutMs = 400;
    static constexpr int kMaxPeers = 3;  ///< addresses tried per read
    /** A helper that answered keeps its last values this long while requests fail (then kRetrySec applies). */
    static constexpr double kHoldSec = 3.0;

    /**
     * Sets up libcurl: construct on the main thread.
     * @param home $HOME (vrlink.txt is under it)
     * @param runtimeDir $XDG_RUNTIME_DIR (the layer's file)
     * @param procRoot /proc
     * @param port the helper's port (tests: a fake helper's)
     */
    RenderSizeReader(std::string home, std::string runtimeDir, std::string procRoot = "/proc", int port = kHelperPort);

    /**
     * @param now monotonic seconds
     * @param scenePid the OpenVR scene app's process (0: none, or no SteamVR)
     */
    Reading read(double now, int scenePid);

private:
    std::string vrlinkLog_;
    std::string layerFile_;
    std::string procRoot_;
    int port_;
    framecolor::StreamingProbe streaming_;
    std::string peer_;           ///< the PC that answered last
    double nextPcTry_ = -1e300;  ///< after a PC that didn't answer
    HelperVersion version_;
    PingMedian ping_;
    bool lastSeen_ = false;  ///< StreamingProbe's last answer
    bool stable_ = false;    ///< ... once it said the same twice: what read() goes by
    Reading good_;           ///< the helper's last answer, shown for kHoldSec while it doesn't answer
    double goodAt_ = 0;
    bool haveGood_ = false;
    bool wasStreaming_ = false;  ///< the read before this one was a stream's
    AfterStream after_;
    std::string statePeer_;  ///< the PC that answered /v2/state during the stream: asked for its summary afterwards
    LastSession summary_;    ///< the summary of the stream that ended last; kept until the next stream starts

    Reading readPc(double now);
    /** Asks one address. @return false when no helper answered there (r untouched) */
    bool askPeer(const std::string& peer, double now, Reading& r);
    Reading readLayer(int scenePid) const;
    /** Asks statePeer_ for the ended stream's summary (AfterStream says when). */
    void askLast();
};

}  // namespace render_size
