#pragma once
// FrameColor gaze hand-over (2026-11-08): Frame Dock writes where each eye looks, in the coordinates of the image
// this layer sees (each eye's half of vrcompositor's output, 0..1; on the Steam Frame that is ComputeDistortion's
// panel UV turned 180 degrees), into a small shared memory file; the layer
// reads it once per present and moves the sharpening centre there. A seqlock: the writer makes `sequence` odd, writes,
// makes it even; a reader that sees an odd or changed sequence keeps its previous sample. Header-only, no OpenVR.
#include <atomic>
#include <cstdint>
#include <cstring>

namespace framegaze {

constexpr std::uint32_t kMagic = 0x5a474346u;  // "FCGZ"
constexpr std::uint32_t kVersion = 1;

struct Record {
    std::uint32_t magic;
    std::uint32_t version;
    std::atomic<std::uint32_t> sequence;
    std::uint32_t valid;        // 1: both eyes' points below are current
    float leftU, leftV;         // left eye, its own half of the image
    float rightU, rightV;       // right eye
    std::uint64_t writtenNs;    // CLOCK_MONOTONIC of the write
};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "the seqlock needs a lock-free counter");

struct Sample {
    bool valid = false;
    float leftU = 0.5f, leftV = 0.5f, rightU = 0.5f, rightV = 0.5f;
    std::uint64_t writtenNs = 0;
};

inline void initialize(Record& record) noexcept {
    record.magic = kMagic;
    record.version = kVersion;
    record.sequence.store(0, std::memory_order_relaxed);
    record.valid = 0;
    record.leftU = record.leftV = record.rightU = record.rightV = 0.5f;
    record.writtenNs = 0;
}

// A writer that died between the two stores of write() leaves the sequence odd: make it even again before writing
// (only the single writer, Frame Dock, calls this, when it maps the file)
inline void recover(Record& record) noexcept {
    const std::uint32_t sequence = record.sequence.load(std::memory_order_relaxed);
    if (sequence & 1u) record.sequence.store(sequence + 1, std::memory_order_release);
}

inline void write(Record& record, const Sample& sample) noexcept {
    const std::uint32_t start = record.sequence.load(std::memory_order_relaxed);
    record.sequence.store(start + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    record.valid = sample.valid ? 1u : 0u;
    record.leftU = sample.leftU;
    record.leftV = sample.leftV;
    record.rightU = sample.rightU;
    record.rightV = sample.rightV;
    record.writtenNs = sample.writtenNs;
    std::atomic_thread_fence(std::memory_order_release);
    record.sequence.store(start + 2, std::memory_order_release);
}

// @return true and the sample if a complete, current-format record was read; false leaves `out` as it was
inline bool read(const Record& record, Sample& out) noexcept {
    if (record.magic != kMagic || record.version != kVersion) return false;
    const std::uint32_t before = record.sequence.load(std::memory_order_acquire);
    if (before & 1u) return false;
    Sample next;
    next.valid = record.valid == 1u;
    next.leftU = record.leftU;
    next.leftV = record.leftV;
    next.rightU = record.rightU;
    next.rightV = record.rightV;
    next.writtenNs = record.writtenNs;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (record.sequence.load(std::memory_order_relaxed) != before) return false;
    const auto inUnit = [](float value) { return value >= 0.0f && value <= 1.0f; };
    if (next.valid && !(inUnit(next.leftU) && inUnit(next.leftV) && inUnit(next.rightU) && inUnit(next.rightV)))
        next.valid = false;
    out = next;
    return true;
}

// The file both sides use (per user, in /dev/shm)
inline const char* path() noexcept { return "/dev/shm/framecolor-gaze"; }

}  // namespace framegaze
