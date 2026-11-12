#include "eye_server.h"

#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>

namespace framecolor {

namespace {

constexpr const char* kPath = "/dev/shm/eye-server.mmap";
// Header: u32 version, u32 initialized, 48-byte glibc pthread_mutex_t at 0x08, u32 sequence at 0x38,
// u32 metadata_requested at 0x3c (frameeyeosc's layout)
constexpr std::size_t kMutexOffset = 0x08, kSequenceOffset = 0x38, kRequestOffset = 0x3c;
// Packed record: u32 producer_state @0, u8 flag @4, f64 sample_time @5, f32[2][3] gaze_direction @0x0d
constexpr std::size_t kStateOffset = 0x00, kTimeOffset = 0x05, kGazeOffset = 0x0d, kFixationOffset = 0x3d,
                      kOpennessOffset = 0x79, kRecordSize = 0xebc;
static_assert(sizeof(pthread_mutex_t) <= 0x30, "the mutex slot in the header is 48 bytes");

struct Abi {
    int version;
    std::size_t size, recordOffset;
};
constexpr Abi kKnown[] = {{4, 0x4f21a, 0x152}, {5, 0x4f21f, 0x157}};

volatile std::uint32_t* word(unsigned char* base, std::size_t offset) {
    return reinterpret_cast<volatile std::uint32_t*>(base + offset);
}

class Lock {
public:
    explicit Lock(pthread_mutex_t* mutex) : mutex_(mutex) {
        const int code = pthread_mutex_lock(mutex_);
        if (code == EOWNERDEAD) ok_ = pthread_mutex_consistent(mutex_) == 0;  // the eye tracker died holding it
        else ok_ = code == 0;
        if (!ok_ && code == EOWNERDEAD) pthread_mutex_unlock(mutex_);
    }
    ~Lock() {
        if (ok_) pthread_mutex_unlock(mutex_);
    }
    bool ok() const { return ok_; }

private:
    pthread_mutex_t* mutex_;
    bool ok_ = false;
};

}  // namespace

EyeServer::~EyeServer() { close(); }

void EyeServer::close() {
    if (map_) ::munmap(map_, length_);
    map_ = nullptr;
    length_ = recordOffset_ = 0;
    version_ = 0;
}

bool EyeServer::open() {
    close();
    const int fd = ::open(kPath, O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(kRequestOffset + 4)) {
        ::close(fd);
        return false;
    }
    const auto length = static_cast<std::size_t>(st.st_size);
    void* memory = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (memory == MAP_FAILED) return false;
    auto* base = static_cast<unsigned char*>(memory);
    const std::uint32_t version = *word(base, 0), initialized = *word(base, 4);
    for (const Abi& abi : kKnown) {
        if (static_cast<std::uint32_t>(abi.version) == version && length >= abi.size && initialized == 1 &&
            abi.recordOffset + kRecordSize <= length) {
            map_ = base;
            length_ = length;
            recordOffset_ = abi.recordOffset;
            version_ = abi.version;
            return true;
        }
    }
    ::munmap(memory, length);
    return false;
}

std::optional<EyeServerSample> EyeServer::next(int timeoutMs) {
    if (!map_) return std::nullopt;
    auto* mutex = reinterpret_cast<pthread_mutex_t*>(map_ + kMutexOffset);
    std::uint32_t sequence = 0;
    {
        Lock lock(mutex);
        if (!lock.ok()) return std::nullopt;
        sequence = *word(map_, kSequenceOffset);
        *word(map_, kRequestOffset) = 1;  // ask the eye tracker to publish the next sample
    }
    timespec timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000000L};
    syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(map_ + kSequenceOffset), FUTEX_WAIT, sequence, &timeout,
            nullptr, 0);  // EAGAIN (already changed), EINTR and ETIMEDOUT all just mean: look now
    Lock lock(mutex);
    if (!lock.ok() || *word(map_, kSequenceOffset) == sequence) return std::nullopt;
    const unsigned char* record = map_ + recordOffset_;
    std::uint32_t state = 0;
    std::memcpy(&state, record + kStateOffset, sizeof(state));
    if (state != 1) return std::nullopt;
    EyeServerSample sample;
    std::memcpy(&sample.sampleTime, record + kTimeOffset, sizeof(sample.sampleTime));
    std::memcpy(sample.gaze.data(), record + kGazeOffset, sizeof(sample.gaze));
    std::memcpy(sample.fixation.data(), record + kFixationOffset, sizeof(sample.fixation));
    std::memcpy(sample.openness.data(), record + kOpennessOffset, sizeof(sample.openness));
    // Anything not finite: no sample (frameeyeosc skips it too; a long run of them reads as tracking lost)
    for (const auto& eye : sample.gaze)
        for (float value : eye)
            if (!std::isfinite(value)) return std::nullopt;
    for (float value : sample.fixation)
        if (!std::isfinite(value)) return std::nullopt;
    for (float value : sample.openness)
        if (!std::isfinite(value)) return std::nullopt;
    if (!std::isfinite(sample.sampleTime)) return std::nullopt;
    return sample;
}

}  // namespace framecolor
