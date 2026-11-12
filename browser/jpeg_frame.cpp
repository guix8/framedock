// JPEG decoding with libturbojpeg. See jpeg_frame.h.
#include "jpeg_frame.h"

#include <turbojpeg.h>

JpegDecoder::JpegDecoder() : handle_(tjInitDecompress()) {}

JpegDecoder::~JpegDecoder() {
    if (handle_ != nullptr) tjDestroy(static_cast<tjhandle>(handle_));
}

bool JpegDecoder::decode(const uint8_t* data, size_t size, BgraFrame& frame, std::string& error, int maxWidth,
                         int maxHeight) {
    if (handle_ == nullptr) {
        error = "no JPEG decoder";
        return false;
    }
    tjhandle tj = static_cast<tjhandle>(handle_);
    int width = 0, height = 0, subsampling = 0, colorspace = 0;
    if (size < 4 || tjDecompressHeader3(tj, data, static_cast<unsigned long>(size), &width, &height, &subsampling, &colorspace) != 0) {
        error = std::string("JPEG header: ") + tjGetErrorStr2(tj);
        return false;
    }
    if (width <= 0 || height <= 0 || width > maxWidth || height > maxHeight) {
        error = "JPEG size " + std::to_string(width) + "x" + std::to_string(height) + " out of range";
        return false;
    }
    if (frame.width != width || frame.height != height ||
        frame.pixels.size() != static_cast<size_t>(width) * height * 4) {
        frame.pixels.assign(static_cast<size_t>(width) * height * 4, 0);
        frame.width = width;
        frame.height = height;
    }
    // TJFLAG_FASTDCT, TJFLAG_FASTUPSAMPLE (chroma by pixel replication): the screencast is already lossy; speed
    // matters more on the headset's cores
    // Chrome's frames are whole: any complaint (even a warning about a cut-short picture) drops the frame
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8 ||
        tjDecompress2(tj, data, static_cast<unsigned long>(size), frame.pixels.data(), width, width * 4, height,
                      TJPF_BGRA, TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE) != 0) {
        error = std::string("JPEG: ") + (size < 4 || data[0] != 0xFF || data[1] != 0xD8 ? "no JPEG start" : tjGetErrorStr2(tj));
        return false;
    }
    ++frame.sequence;
    return true;
}

bool JpegDecoder::encode(const BgraFrame& frame, int quality, std::string& jpeg) {
    tjhandle tj = tjInitCompress();
    if (tj == nullptr) return false;
    unsigned char* out = nullptr;
    unsigned long outSize = 0;
    const bool ok = tjCompress2(tj, frame.pixels.data(), frame.width, frame.width * 4, frame.height, TJPF_BGRA, &out,
                                &outSize, TJSAMP_420, quality, 0) == 0;
    if (ok) jpeg.assign(reinterpret_cast<const char*>(out), outSize);
    if (out != nullptr) tjFree(out);
    tjDestroy(tj);
    return ok;
}
