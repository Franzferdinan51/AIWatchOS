// Portable SHA-256 / HMAC-SHA256 / base64 / hex for the Hermes client.
//
// Device authentication needs exactly these primitives. Keeping them in the
// app (instead of a platform crypto library) makes the auth path identical
// on hardware and in host tests, and lets both be pinned to the published
// HermesGadget test vectors. Implemented from FIPS 180-4 / RFC 2104 /
// RFC 4648; verified by test_hermes_proto_vectors in the host test suite.
#include "hermes_proto.hpp"

#include <cstring>

namespace aiwatchos_hermes {
namespace {

constexpr uint32_t kShaK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha_block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) | (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(p[i * 4 + 2]) << 8) | static_cast<uint32_t>(p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + s1 + ch + kShaK[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void hex_encode(const uint8_t* data, size_t len, char* out) {
    static const char* kHex = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

}  // namespace

void HermesSha256::init() {
    h[0] = 0x6a09e667; h[1] = 0xbb67ae85; h[2] = 0x3c6ef372; h[3] = 0xa54ff53a;
    h[4] = 0x510e527f; h[5] = 0x9b05688c; h[6] = 0x1f83d9ab; h[7] = 0x5be0cd19;
    block_len = 0;
    total_len = 0;
}

void HermesSha256::update(const uint8_t* data, size_t len) {
    total_len += len;
    while (len > 0) {
        size_t take = 64 - block_len;
        if (take > len) take = len;
        std::memcpy(block + block_len, data, take);
        block_len += take;
        data += take;
        len -= take;
        if (block_len == 64) {
            sha_block(h, block);
            block_len = 0;
        }
    }
}

void HermesSha256::finish(uint8_t out[32]) {
    uint64_t bit_len = total_len * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (block_len != 56) update(&zero, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; ++i) lenbuf[i] = static_cast<uint8_t>(bit_len >> (56 - i * 8));
    // Feed the length directly (update() would count it in total_len, which no
    // longer matters, but keep the block mechanics identical).
    for (int i = 0; i < 8; ++i) {
        block[block_len++] = lenbuf[i];
        if (block_len == 64) {
            sha_block(h, block);
            block_len = 0;
        }
    }
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
}

void HermesSha256::finish_hex(char out[65]) {
    uint8_t digest[32];
    finish(digest);
    hex_encode(digest, 32, out);
}

void hermes_sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    HermesSha256 s;
    s.init();
    s.update(data, len);
    s.finish(out);
}

void hermes_sha256_hex(const uint8_t* data, size_t len, char out[65]) {
    HermesSha256 s;
    s.init();
    s.update(data, len);
    s.finish_hex(out);
}

namespace {

void hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len, uint8_t out[32]) {
    uint8_t key_block[64] = {};
    if (key_len > 64) {
        hermes_sha256(key, key_len, key_block);
    } else {
        std::memcpy(key_block, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = key_block[i] ^ 0x36;
        opad[i] = key_block[i] ^ 0x5C;
    }
    uint8_t inner[32];
    {
        HermesSha256 s;
        s.init();
        s.update(ipad, 64);
        s.update(msg, msg_len);
        s.finish(inner);
    }
    HermesSha256 s;
    s.init();
    s.update(opad, 64);
    s.update(inner, 32);
    s.finish(out);
}

}  // namespace

std::string hermes_base64_encode(const uint8_t* data, size_t len) {
    static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        int rem = static_cast<int>(len - i);
        if (rem > 1) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (rem > 2) v |= data[i + 2];
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(rem > 1 ? kB64[(v >> 6) & 63] : '=');
        out.push_back(rem > 2 ? kB64[v & 63] : '=');
    }
    return out;
}

bool hermes_base64_decode(const char* in, uint8_t* out, size_t out_cap, size_t* out_len) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    size_t n = 0;
    uint32_t acc = 0;
    int acc_bits = 0;
    for (const char* p = in; *p; ++p) {
        if (*p == '=') break;
        int v = val(*p);
        if (v < 0) {
            if (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') continue;
            return false;
        }
        acc = (acc << 6) | static_cast<uint32_t>(v);
        acc_bits += 6;
        if (acc_bits >= 8) {
            acc_bits -= 8;
            if (n >= out_cap) return false;
            out[n++] = static_cast<uint8_t>(acc >> acc_bits);
        }
    }
    if (out_len) *out_len = n;
    return true;
}

std::string hermes_device_id_for_key(const uint8_t key[32]) {
    uint8_t digest[32];
    hermes_sha256(key, 32, digest);
    char hex[65];
    hex_encode(digest, 32, hex);
    std::string id = "hg-";
    id.append(hex, 16);
    return id;
}

std::string hermes_auth_mac_b64(const uint8_t key[32], const char* device_id, const char* nonce_b64) {
    // "hermes-gadget/v1|" + device_id + "|" + nonce — built in a fixed buffer;
    // device_id is 19 chars and a 16-byte nonce encodes to 24, so this fits
    // with wide margin. Refuse rather than truncate on absurd inputs.
    char msg[160];
    int n = std::snprintf(msg, sizeof(msg), "hermes-gadget/v1|%s|%s", device_id, nonce_b64);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(msg)) return std::string();
    uint8_t mac[32];
    hmac_sha256(key, 32, reinterpret_cast<const uint8_t*>(msg), static_cast<size_t>(n), mac);
    return hermes_base64_encode(mac, 32);
}

std::string hermes_ota_mac_b64(const uint8_t key[32], const char* device_id, const char* nonce_b64,
                               const char* sha256_hex, uint32_t size) {
    char msg[256];
    int n = std::snprintf(msg, sizeof(msg), "hermes-gadget/v1|ota|%s|%s|%s|%u",
                          device_id, nonce_b64, sha256_hex, static_cast<unsigned>(size));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(msg)) return std::string();
    uint8_t mac[32];
    hmac_sha256(key, 32, reinterpret_cast<const uint8_t*>(msg), static_cast<size_t>(n), mac);
    return hermes_base64_encode(mac, 32);
}

size_t hermes_binary_header_pack(uint8_t* dst, uint8_t channel, uint8_t stream, uint16_t seq) {
    dst[0] = channel;
    dst[1] = stream;
    dst[2] = static_cast<uint8_t>(seq & 0xFF);
    dst[3] = static_cast<uint8_t>(seq >> 8);
    return kHermesBinaryHeader;
}

bool hermes_binary_parse(const uint8_t* data, size_t len, uint8_t* channel, uint8_t* stream,
                         uint16_t* seq, const uint8_t** payload, size_t* payload_len) {
    if (!data || len < kHermesBinaryHeader) return false;
    if (data[0] != kHermesChannelAudio && data[0] != kHermesChannelImage &&
        data[0] != kHermesChannelFirmware) return false;
    if (channel) *channel = data[0];
    if (stream) *stream = data[1];
    if (seq) *seq = static_cast<uint16_t>(data[2] | (data[3] << 8));
    if (payload) *payload = data + kHermesBinaryHeader;
    if (payload_len) *payload_len = len - kHermesBinaryHeader;
    return true;
}

}  // namespace aiwatchos_hermes
