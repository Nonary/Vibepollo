#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Independent intra-frame LZ4 groups. No previous frame, reference cache or ACK.
namespace PyroWaveCompression {
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMagic = 0xFFFFFFFEu;
constexpr size_t kHeaderBytes = 16;
constexpr size_t kMaxGroupBytes = 64 * 1024;
constexpr size_t kMaxFrameBytes = 16 * 1024 * 1024;

struct EncodedFrame {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> recordStartShards;
    size_t criticalBytes = 0;
    size_t compressedGroups = 0;
};

class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    // Native record framing including optional padding. Critical bytes are
    // copied unchanged; output never exceeds the original framed byte count.
    bool encode(const uint8_t* records, size_t size, size_t criticalBytes,
                size_t shardPayload, EncodedFrame& output, std::string& error);
private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

// Validate one complete, bounded group and append its native records. The caller
// validates native geometry, sequence and detail-only block IDs before GPU use.
bool expandGroup(const uint8_t* group, size_t size, std::vector<uint8_t>& records,
                 std::string& error);
uint32_t checksumCrc32c(const uint8_t* data, size_t size, bool forceScalar = false);
}
