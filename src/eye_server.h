#pragma once
// SteamVR's private eye tracking shared memory on the Steam Frame (/dev/shm/eye-server.mmap), read the way
// frameeyeosc does (https://github.com/konsti219/frameeyeosc, MIT; ABI versions 4 and 5 only): lock the robust
// pthread mutex in the header, note the sequence, ask for a sample, wait on the sequence with a futex, read the packed
// record. Not a public format: any other version, size or state is refused and the caller falls back to
// IVRSystem::GetEyeTrackedFoveationCenter. 2026-11-08: SteamOS 0.5.8 / SteamVR 1794004177 had version 5.
#include <array>
#include <cstddef>
#include <optional>

namespace framecolor {

struct EyeServerSample {
    std::array<std::array<float, 3>, 2> gaze{};  // left, right; head-relative, -Z forward (after stereo fusion)
    std::array<float, 3> fixation{};              // where both eyes meet, head-relative, metres, -Z forward
    std::array<float, 2> openness{};              // left, right eye openness (0 shut .. 1 open)
    double sampleTime = 0;                        // seconds (frameeyeosc times its filters with it)
};

class EyeServer {
public:
    EyeServer() = default;
    ~EyeServer();
    EyeServer(const EyeServer&) = delete;
    EyeServer& operator=(const EyeServer&) = delete;
    /** @return true if the file is there, initialised and of a known version (it can be called again later) */
    bool open();
    bool isOpen() const { return map_ != nullptr; }
    int version() const { return version_; }
    /** Wait up to timeoutMs for the next sample. Nothing: no new sample, or the tracker reports no valid gaze. */
    std::optional<EyeServerSample> next(int timeoutMs);
    void close();

private:
    unsigned char* map_ = nullptr;
    std::size_t length_ = 0, recordOffset_ = 0;
    int version_ = 0;
};

}  // namespace framecolor
