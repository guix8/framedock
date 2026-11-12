// Decoding Chrome's screencast JPEGs into pixels the panel can show (libturbojpeg, the system's libjpeg-turbo).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/** A decoded picture: BGRA bytes (cairo's ARGB32 in memory on a little-endian machine), alpha 255, rows packed. */
struct BgraFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;  ///< width × height × 4
    uint64_t sequence = 0;        ///< counts frames (the panel uploads only new ones)
    /// the page area the picture shows, in CSS px (the screencast's deviceWidth / deviceHeight; 0: the picture's size)
    double cssWidth = 0, cssHeight = 0;
    bool empty() const { return width <= 0 || height <= 0; }
};

/** Gives the frame's pixel memory back (Chrome stopped: a 1080p frame is 8 MB); its sequence stays. */
inline void releasePixels(BgraFrame& frame) {
    std::vector<uint8_t>().swap(frame.pixels);
    frame.width = frame.height = 0;
}

class JpegDecoder {
public:
    JpegDecoder();
    ~JpegDecoder();
    JpegDecoder(const JpegDecoder&) = delete;
    JpegDecoder& operator=(const JpegDecoder&) = delete;

    /**
     * Decodes a JPEG. The frame's buffer is reused when the size stays the same.
     * @param maxWidth,maxHeight larger pictures are an error (a frame never is: Chrome scales to the screencast size)
     * @return false on a broken JPEG or no decoder (error)
     */
    bool decode(const uint8_t* data, size_t size, BgraFrame& frame, std::string& error, int maxWidth = 4096,
                int maxHeight = 4096);

    /** Test helper: encodes BGRA pixels as a JPEG (quality 1–100). @return false on failure */
    static bool encode(const BgraFrame& frame, int quality, std::string& jpeg);

private:
    void* handle_ = nullptr;  ///< tjhandle
};
