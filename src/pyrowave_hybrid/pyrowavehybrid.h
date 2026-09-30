#pragma once

// Optional, lossless transport for ordinary PyroWave intra coefficient records.
// The GPU codec remains intra-only: successful decode() reconstructs one complete
// ordinary frame, with the current three-bit sequence restored in every record.
// Sender references are immutable and used only after explicit receiver ACK.
// A transport frame with any unrepaired packet loss must never enter decode().

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace PyroWaveHybrid {

constexpr uint32_t kFeatureVersion = 1;
constexpr uint32_t kWireMagic = 0x31485950u; // PYH1, little endian

struct StreamGeometry {
    uint32_t width;
    uint32_t height;
    bool chroma444;
};

struct Limits {
    size_t maxFrameBytes = 16u * 1024u * 1024u;
    size_t maxSnapshots = 16;
    size_t maxSnapshotBytes = 64u * 1024u * 1024u;
};

struct Stats {
    size_t inputBytes = 0;
    size_t wireBytes = 0;
    uint32_t unchangedBlocks = 0;
    uint32_t compressedBlocks = 0;
    uint32_t rawBlocks = 0;
    uint32_t clearedBlocks = 0;
    bool keyframe = false;
};

struct EncodedFrame {
    std::vector<uint8_t> bytes;
    // Complete prefix containing the envelope and all changed coarsest blocks.
    size_t criticalBytes = 0;
    // Offsets of complete hybrid update records (plus offset zero).
    std::vector<size_t> recordStarts;
    uint64_t frameId = 0;
    uint64_t baseFrameId = 0;
    Stats stats;
};

struct DecodedFrame {
    // Padding-free native records sorted by block ID; pixels/coefficients are
    // identical to the input intra frame, irrespective of input record order.
    std::vector<uint8_t> records;
    uint64_t frameId = 0;
    uint64_t baseFrameId = 0;
    Stats stats;
};

// Detection only; decode() performs complete version, checksum and size checks.
bool isHybridFrame(const uint8_t* data, size_t size);

// Version-1 integrity primitive (Castagnoli). Optional scalar selection is
// useful for independent wire validators and architecture differential checks.
uint32_t checksumCrc32c(const uint8_t* data, size_t size, bool forceScalar = false);

// Map record boundaries to RTP shard starts. shardPayload includes the normal
// eight-byte frame header which occupies the beginning of shard zero only.
std::vector<uint8_t> recordStartShards(const EncodedFrame& frame, size_t shardPayload,
                                     size_t firstShardExtraBytes = 8);

class Sender {
public:
    explicit Sender(Limits limits = {});
    ~Sender();
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    // Input is normal record framing, optionally with its padding records.
    // IDs must be nonzero and increase throughout an active sender generation.
    // Failure leaves reference state unchanged and empties output.
    bool encode(const uint8_t* records, size_t size, uint64_t frameId,
                size_t shardPayload, size_t maxFrameBytes,
                EncodedFrame& output, std::string& error);
    // Zero requests a full refresh; unknown/evicted/delayed older ACKs are ignored.
    // encode(), acknowledge() and reset() may be called from different threads.
    bool acknowledge(uint64_t frameId);
    void reset();
    size_t retainedBytes() const;
    size_t retainedFrames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

class Receiver {
public:
    explicit Receiver(StreamGeometry geometry, Limits limits = {});
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    // Failure never changes reference snapshots or acknowledgedFrameId(). ACK
    // only after this output passes the caller's normal native-frame validation.
    bool decode(const uint8_t* wire, size_t size, DecodedFrame& output, std::string& error);
    void reset();
    uint64_t acknowledgedFrameId() const;
    size_t retainedBytes() const;
    size_t retainedFrames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace PyroWaveHybrid
