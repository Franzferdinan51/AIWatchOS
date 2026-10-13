// Band-based dirty flushing (plan 1.6; HermesGadget's Ui::render band
// model brought to pixel data). The frame is split into kBands
// horizontal bands; after each render the shell hashes every band and
// flushes ONLY the bands whose pixels changed since the last flushed
// frame, through the HAL's row-range flush. A full-frame flush is
// ~410,820 bytes (~80 ms on the QSPI wire) against a 40 ms frame
// budget; a static clock face drops to zero flush traffic per frame.
//
// Hashing is FNV-1a over the band's pixels read as 32-bit words
// (410 px/row = 205 words exactly): ~103k xor-multiply ops per frame
// in PSRAM-resident data, well under a millisecond on the S3 — and the
// framebuffer reads are the same ones the flush would have made anyway
// for changed bands.
#pragma once

#include <cstdint>

namespace aiwatchos {

// FNV-1a over framebuffer rows [y0, y1). Change detection only — the
// value is never trusted for anything but equality.
uint32_t band_hash(const uint16_t* pixels, int width, int y0, int y1);

class BandFlush {
 public:
    static constexpr int kBands = 4;

    // Row range [y0, y1) covered by band `band` for a frame of `height`.
    static void band_range(int band, int height, int& y0, int& y1);

    // Hash all bands of the current frame; returns the bitmask of bands
    // that differ from the last committed frame (all bands when nothing
    // has been committed yet). The hashes are kept pending until
    // commit() marks bands as flushed.
    uint8_t scan(const uint16_t* pixels, int width, int height);

    // Mark the bands in `mask` as flushed (adopts their pending hashes).
    void commit(uint8_t mask);

    // Force the next scan() to report every band (first frame, wake).
    void invalidate() { valid_ = false; }

 private:
    uint32_t committed_[kBands] = {};
    uint32_t pending_[kBands] = {};
    bool valid_ = false;
};

}  // namespace aiwatchos
