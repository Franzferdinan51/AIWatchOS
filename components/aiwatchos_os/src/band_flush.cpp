// Band-based dirty flushing — see band_flush.hpp.
#include "aiwatchos/band_flush.hpp"

#include <cstring>

namespace aiwatchos {

uint32_t band_hash(const uint16_t* pixels, int width, int y0, int y1) {
    // 410 is even, so rows are a whole number of 32-bit words; guard the
    // general case by hashing the tail pixel as a half-word.
    uint32_t h = 2166136261u;
    const uint32_t* words = reinterpret_cast<const uint32_t*>(pixels);
    const int words_per_row = width / 2;
    for (int y = y0; y < y1; ++y) {
        const uint32_t* row = words + y * words_per_row;
        for (int i = 0; i < words_per_row; ++i) {
            h = (h ^ row[i]) * 16777619u;
        }
        if (width & 1) {
            h = (h ^ pixels[y * width + width - 1]) * 16777619u;
        }
    }
    return h;
}

void BandFlush::band_range(int band, int height, int& y0, int& y1) {
    y0 = height * band / kBands;
    y1 = height * (band + 1) / kBands;
}

uint8_t BandFlush::scan(const uint16_t* pixels, int width, int height) {
    uint8_t mask = 0;
    for (int b = 0; b < kBands; ++b) {
        int y0, y1;
        band_range(b, height, y0, y1);
        pending_[b] = band_hash(pixels, width, y0, y1);
        if (!valid_ || pending_[b] != committed_[b]) {
            mask |= static_cast<uint8_t>(1u << b);
        }
    }
    return mask;
}

void BandFlush::commit(uint8_t mask) {
    for (int b = 0; b < kBands; ++b) {
        if (mask & (1u << b)) committed_[b] = pending_[b];
    }
    valid_ = true;
}

}  // namespace aiwatchos
