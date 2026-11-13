// The size the game in front renders each eye at, for the Frame card (2026-11-13). SteamVR's recommended size isn't
// it: Eleven Table Tennis renders 2400x2400 where SteamVR recommends 1728x1728.
//
// - A PC stream (vrlink runs): only the PC knows. Frame Dock's optional PC helper (pc-helper/framedock-pc.cs) answers
//   "GET /render" on port 27463 with {"v":1,"app":"<key>","width":W,"height":H}, or {"v":1,"app":null} while no
//   streamed game runs. The PC's address is the peer of vrlink.txt's latest "SVLDataLinkTransferUDP::SetPeer <here> ->
//   <peer>" lines; the one that answered is asked first next time.
// - Standalone: the layer (third_party/vkbasalt, vrcompositor only) writes "W H <unix seconds>" to
//   $XDG_RUNTIME_DIR/framecolor/render-size when a game's eye textures are created. Taken only while a scene app runs
//   and only if written after that app's process started, so a previous game's size isn't shown.
//
// RenderSizeReader blocks for up to a few hundred ms (the LAN request): AsyncSampler's thread calls it.
#pragma once

#include "auto_profile.h"

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

/** One reading; source is a frame_perf_overlay::RenderSource value (0 = nothing known). */
struct Reading {
    int width = 0;
    int height = 0;
    uint32_t source = 0;
};

/** Reads the render size (AsyncSampler's thread, one at a time). */
class RenderSizeReader {
public:
    /** After a PC that didn't answer, ask again only this much later (a firewall drops, so each try waits). */
    static constexpr double kRetrySec = 15.0;
    static constexpr long kTimeoutMs = 400;
    static constexpr int kMaxPeers = 3;  ///< addresses tried per read

    /**
     * Sets up libcurl: construct on the main thread.
     * @param home $HOME (vrlink.txt is under it)
     * @param runtimeDir $XDG_RUNTIME_DIR (the layer's file)
     * @param procRoot /proc
     */
    RenderSizeReader(std::string home, std::string runtimeDir, std::string procRoot = "/proc");

    /**
     * @param now monotonic seconds
     * @param scenePid the OpenVR scene app's process (0: none, or no SteamVR)
     */
    Reading read(double now, int scenePid);

private:
    std::string vrlinkLog_;
    std::string layerFile_;
    std::string procRoot_;
    framecolor::StreamingProbe streaming_;
    std::string peer_;           ///< the PC that answered last
    double nextPcTry_ = -1e300;  ///< after a PC that didn't answer

    Reading readPc(double now);
    Reading readLayer(int scenePid) const;
};

}  // namespace render_size
