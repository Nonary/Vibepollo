#include "pyrowavehybrid.h"

#define LZ4_STATIC_LINKING_ONLY
#include "../../third-party/lz4/lz4.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>

#if defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#include <nmmintrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <nmmintrin.h>
#endif

namespace PyroWaveHybrid {
namespace {

constexpr size_t kNativeHeader = 8;
constexpr size_t kEnvelopeBytes = 64;
constexpr size_t kWireHeader = kNativeHeader + kEnvelopeBytes;
constexpr size_t kUpdateHeader = 24;
constexpr uint32_t kSequenceMask = 0x70000000u;
constexpr uint32_t kPaddingMagic = 0xFFFFFFFFu;
constexpr uint32_t kKeyframe = 1;
constexpr uint32_t kNativeFrame = 2;
constexpr uint32_t kRaw = 0;
constexpr uint32_t kLz4 = 1;
constexpr uint32_t kClear = 2;
constexpr uint32_t kXorLz4 = 3;
constexpr uint32_t kGroupLz4 = 4;
constexpr uint32_t kXorRaw = 5; // Only inside a validated LZ4 group.
constexpr size_t kMaxBlockBytes = 0xFFFu * 4u;
constexpr size_t kMaxGroupBytes = 64u * 1024u;

uint32_t read32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint64_t read64(const uint8_t* p)
{
    return uint64_t(read32(p)) | (uint64_t(read32(p + 4)) << 32);
}

void write32(uint8_t* p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

void write64(uint8_t* p, uint64_t v)
{
    write32(p, uint32_t(v));
    write32(p + 4, uint32_t(v >> 32));
}

const std::array<std::array<uint32_t, 256>, 8>& crcTable()
{
    static const auto table = [] {
        std::array<std::array<uint32_t, 256>, 8> result{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) ? 0x82F63B78u : 0u);
            result[0][i] = value;
        }
        for (unsigned slice = 1; slice < result.size(); ++slice)
            for (uint32_t i = 0; i < 256; ++i) {
                const uint32_t value = result[slice - 1][i];
                result[slice][i] = result[0][value & 0xFFu] ^ (value >> 8);
            }
        return result;
    }();
    return table;
}

uint32_t crcUpdateScalar(uint32_t crc, const uint8_t* p, size_t size)
{
    const auto& table = crcTable();
    while (size >= 8) {
        const uint32_t value = crc ^ read32(p);
        crc = table[7][value & 0xFFu] ^ table[6][(value >> 8) & 0xFFu] ^
              table[5][(value >> 16) & 0xFFu] ^ table[4][value >> 24] ^
              table[3][p[4]] ^ table[2][p[5]] ^ table[1][p[6]] ^ table[0][p[7]];
        p += 8;
        size -= 8;
    }
    while (size--) crc = table[0][(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return crc;
}

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
bool hardwareCrcAvailable()
{
#if defined(_MSC_VER)
    int registers[4]{};
    __cpuid(registers, 1);
    return (registers[2] & (1 << 20)) != 0;
#else
    return __builtin_cpu_supports("sse4.2") != 0;
#endif
}

#if !defined(_MSC_VER)
__attribute__((target("sse4.2")))
#endif
uint32_t crcUpdateHardware(uint32_t crc, const uint8_t* p, size_t size)
{
#if defined(_M_X64) || defined(__x86_64__)
    while (size >= 8) {
        uint64_t word;
        std::memcpy(&word, p, sizeof(word));
        crc = uint32_t(_mm_crc32_u64(crc, word));
        p += 8;
        size -= 8;
    }
#endif
    while (size >= 4) {
        uint32_t word;
        std::memcpy(&word, p, sizeof(word));
        crc = _mm_crc32_u32(crc, word);
        p += 4;
        size -= 4;
    }
    while (size--) crc = _mm_crc32_u8(crc, *p++);
    return crc;
}
#endif

uint32_t crcUpdate(uint32_t crc, const uint8_t* p, size_t size)
{
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    static const bool hardware = hardwareCrcAvailable();
    if (hardware) return crcUpdateHardware(crc, p, size);
#endif
    return crcUpdateScalar(crc, p, size);
}

uint32_t checksum(const uint8_t* p, size_t size)
{
    return crcUpdate(0xFFFFFFFFu, p, size) ^ 0xFFFFFFFFu;
}

uint32_t wireChecksum(const uint8_t* p, size_t size)
{
    constexpr uint8_t zeros[4]{};
    uint32_t crc = crcUpdate(0xFFFFFFFFu, p, 52);
    crc = crcUpdate(crc, zeros, sizeof(zeros));
    return crcUpdate(crc, p + 56, size - 56) ^ 0xFFFFFFFFu;
}

bool sameGeometry(const StreamGeometry& a, const StreamGeometry& b)
{
    return a.width == b.width && a.height == b.height && a.chroma444 == b.chroma444;
}

uint32_t aligned(uint32_t extent)
{
    return std::max(128u, (extent + 31u) & ~31u);
}

uint32_t maxBlocks(const StreamGeometry& geometry)
{
    uint32_t blocks = 0;
    for (uint32_t level = 0; level < 5; ++level) {
        const uint32_t width = aligned(geometry.width) >> (level + 1);
        const uint32_t height = aligned(geometry.height) >> (level + 1);
        const uint32_t perBand = ((width + 31) / 32) * ((height + 31) / 32);
        blocks += perBand * (level == 4 ? 4u : 3u) *
                  (level == 0 && !geometry.chroma444 ? 1u : 3u);
    }
    return blocks;
}

uint32_t coarseBlocks(const StreamGeometry& geometry)
{
    return 12u * (((aligned(geometry.width) >> 5) + 31u) / 32u) *
                 (((aligned(geometry.height) >> 5) + 31u) / 32u);
}

bool parseHeader(const uint8_t* p, StreamGeometry& geometry, std::string& error)
{
    const uint32_t word0 = read32(p);
    const uint32_t word1 = read32(p + 4);
    if (!(word0 & 0x80000000u) || word0 == kPaddingMagic || (word1 & 0x03000000u)) {
        error = "invalid native sequence header";
        return false;
    }
    geometry = {(word0 & 0x3FFFu) + 1u, ((word0 >> 14) & 0x3FFFu) + 1u,
                bool(word1 & 0x04000000u)};
    if ((word1 & 0xFFFFFFu) > maxBlocks(geometry)) {
        error = "native block count exceeds geometry";
        return false;
    }
    return true;
}

struct Block {
    std::vector<uint8_t> bytes; // Native record, three-bit sequence normalized to zero.
    uint32_t crc = 0;
};

struct Entry {
    uint32_t id;
    std::shared_ptr<const Block> block;
};

struct Snapshot {
    uint64_t id = 0;
    StreamGeometry geometry{};
    uint32_t formatIdentity = 0;
    std::vector<Entry> blocks;
    size_t nativeBytes = kNativeHeader;
    size_t charge = 0;
};

// The charge deliberately counts shared blocks once per snapshot. This is a
// conservative memory bound and avoids per-frame hash tables for unique-pointer
// accounting. Structural sharing reduces actual allocation below this bound.
void chargeSnapshot(Snapshot& snapshot)
{
    snapshot.charge = snapshot.nativeBytes + sizeof(Snapshot) +
                      snapshot.blocks.capacity() * sizeof(Entry) +
                      snapshot.blocks.size() * (sizeof(Block) + 2 * sizeof(void*));
}

class Cache {
public:
    explicit Cache(Limits limits) : limits(limits) {}

    const Snapshot* find(uint64_t id) const
    {
        for (const auto& frame : frames) if (frame.id == id) return &frame;
        return nullptr;
    }

    bool canRetain(const Snapshot& snapshot) const
    {
        return limits.maxSnapshots != 0 && snapshot.charge <= limits.maxSnapshotBytes;
    }

    void insert(Snapshot&& snapshot, uint64_t pinned = 0)
    {
        while (!frames.empty() && (frames.size() >= limits.maxSnapshots ||
               snapshot.charge > limits.maxSnapshotBytes - bytes)) {
            auto victim = frames.begin();
            if (victim->id == pinned && frames.size() > 1) ++victim;
            bytes -= victim->charge;
            frames.erase(victim);
        }
        bytes += snapshot.charge;
        frames.push_back(std::move(snapshot));
    }

    void clear()
    {
        frames.clear();
        bytes = 0;
    }

    Limits limits;
    std::deque<Snapshot> frames;
    size_t bytes = 0;
};

struct InputRecord {
    uint32_t id;
    size_t offset;
    size_t size;
};

bool parseInput(const uint8_t* data, size_t size, StreamGeometry& geometry,
                std::vector<InputRecord>& records, std::string& error)
{
    records.clear();
    if (!data || size < kNativeHeader || size % 4 != 0 || !parseHeader(data, geometry, error)) {
        if (error.empty()) error = "native frame is empty or not word aligned";
        return false;
    }
    const uint32_t sequence = read32(data) & kSequenceMask;
    const uint32_t limit = maxBlocks(geometry);
    size_t offset = kNativeHeader;
    while (offset < size) {
        if (size - offset < kNativeHeader) {
            error = "truncated native record header";
            return false;
        }
        const uint32_t word0 = read32(data + offset);
        const uint32_t word1 = read32(data + offset + 4);
        if (word0 == kPaddingMagic) {
            const uint64_t bytes = 8u + uint64_t(word1) * 4u;
            if (bytes > size - offset) {
                error = "native padding exceeds frame";
                return false;
            }
            for (size_t pos = offset + 8; pos < offset + size_t(bytes); ++pos) {
                if (data[pos] != 0) {
                    error = "native padding is not zero";
                    return false;
                }
            }
            offset += size_t(bytes);
            continue;
        }
        const size_t bytes = size_t((word0 >> 16) & 0xFFFu) * 4u;
        if ((word0 & 0x80000000u) || (word0 & kSequenceMask) != sequence ||
            bytes < kNativeHeader || bytes > size - offset || (word1 >> 8) >= limit) {
            error = "invalid native coefficient record";
            return false;
        }
        records.push_back({word1 >> 8, offset, bytes});
        offset += bytes;
    }
    std::sort(records.begin(), records.end(), [](const InputRecord& a, const InputRecord& b) {
        return a.id < b.id;
    });
    for (size_t i = 1; i < records.size(); ++i) {
        if (records[i - 1].id == records[i].id) {
            error = "duplicate native coefficient block";
            return false;
        }
    }
    if (records.size() != (read32(data + 4) & 0xFFFFFFu)) {
        error = "native announced block count differs from records";
        return false;
    }
    return true;
}

bool sameRecord(const uint8_t* input, size_t size, const Block& block)
{
    return size == block.bytes.size() &&
           (read32(input) & ~kSequenceMask) == read32(block.bytes.data()) &&
           std::memcmp(input + 4, block.bytes.data() + 4, size - 4) == 0;
}

std::shared_ptr<const Block> makeBlock(const uint8_t* input, size_t size)
{
    auto block = std::make_shared<Block>();
    block->bytes.assign(input, input + size);
    write32(block->bytes.data(), read32(input) & ~kSequenceMask);
    block->crc = checksum(block->bytes.data(), block->bytes.size());
    return block;
}

uint32_t canonicalChecksum(const uint8_t* header, const Snapshot& frame)
{
    uint32_t crc = crcUpdate(0xFFFFFFFFu, header, kNativeHeader);
    uint8_t descriptor[12];
    for (const auto& entry : frame.blocks) {
        write32(descriptor, entry.id);
        write32(descriptor + 4, uint32_t(entry.block->bytes.size()));
        write32(descriptor + 8, entry.block->crc);
        crc = crcUpdate(crc, descriptor, sizeof(descriptor));
    }
    return crc ^ 0xFFFFFFFFu;
}

void clearOutput(EncodedFrame& frame)
{
    frame.bytes.clear();
    frame.recordStarts.clear();
    frame.criticalBytes = 0;
    frame.frameId = frame.baseFrameId = 0;
    frame.stats = {};
}

void clearOutput(DecodedFrame& frame)
{
    frame.records.clear();
    frame.frameId = frame.baseFrameId = 0;
    frame.stats = {};
}

} // namespace

struct Sender::Impl {
    explicit Impl(Limits limits) : cache(limits)
    {
        LZ4_initStream(&lz4, sizeof(lz4));
    }

    mutable std::mutex mutex;
    Cache cache;
    uint64_t ack = 0;
    uint64_t ackFloor = 0;
    uint64_t lastId = 0;
    LZ4_stream_t lz4{};
    std::vector<char> compressed;
    std::vector<uint8_t> residual;
    struct Prediction {
        size_t headerOffset;
        size_t payloadOffset;
        size_t payloadBytes;
        const Block* previous;
    };
    std::vector<uint8_t> group;
    std::vector<Prediction> predictions;
    std::vector<InputRecord> input;
};

struct Receiver::Impl {
    Impl(StreamGeometry geometry, Limits limits) : cache(limits), geometry(geometry) {}
    Cache cache;
    StreamGeometry geometry;
    uint64_t lastId = 0;
    std::vector<uint8_t> group;
};

bool isHybridFrame(const uint8_t* data, size_t size)
{
    return data && size >= 12 && read32(data + 8) == kWireMagic;
}

uint32_t checksumCrc32c(const uint8_t* data, size_t size, bool forceScalar)
{
    const uint32_t crc = forceScalar ? crcUpdateScalar(0xFFFFFFFFu, data, size) :
                                    crcUpdate(0xFFFFFFFFu, data, size);
    return crc ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> recordStartShards(const EncodedFrame& frame, size_t shardPayload,
                                     size_t firstShardExtraBytes)
{
    if (frame.bytes.empty() || shardPayload <= firstShardExtraBytes ||
        frame.bytes.size() > std::numeric_limits<size_t>::max() - firstShardExtraBytes)
        return {};
    const size_t total = frame.bytes.size() + firstShardExtraBytes;
    const size_t count = total / shardPayload + (total % shardPayload != 0);
    std::vector<uint8_t> starts(count, 0);
    starts.front() = 1;
    for (size_t offset : frame.recordStarts) {
        if (offset >= frame.bytes.size()) continue;
        const size_t wireOffset = offset + firstShardExtraBytes;
        if (wireOffset % shardPayload == 0) starts[wireOffset / shardPayload] = 1;
    }
    return starts;
}

Sender::Sender(Limits limits) : m_Impl(new Impl(limits)) {}
Sender::~Sender() = default;
Receiver::Receiver(StreamGeometry geometry, Limits limits) : m_Impl(new Impl(geometry, limits)) {}
Receiver::~Receiver() = default;

bool Sender::encode(const uint8_t* input, size_t size, uint64_t frameId,
                    size_t shardPayload, size_t maxFrameBytes,
                    EncodedFrame& output, std::string& error)
{
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    clearOutput(output);
    error.clear();
    const size_t wireLimit = std::min(maxFrameBytes, m_Impl->cache.limits.maxFrameBytes);
    if (frameId == 0 || frameId <= m_Impl->lastId || size > m_Impl->cache.limits.maxFrameBytes ||
        wireLimit < kWireHeader || wireLimit > UINT32_MAX ||
        shardPayload <= 8 || shardPayload % 4 != 0) {
        error = "invalid hybrid encode ID, size or shard payload";
        return false;
    }
    StreamGeometry geometry{};
    if (!parseInput(input, size, geometry, m_Impl->input, error)) return false;
    const Snapshot* base = m_Impl->cache.find(m_Impl->ack);
    const uint32_t formatIdentity = read32(input + 4) & 0xFF000000u;
    if (base && (!sameGeometry(base->geometry, geometry) || base->formatIdentity != formatIdentity)) base = nullptr;
    const uint32_t coarse = coarseBlocks(geometry);
    Snapshot snapshot;
    snapshot.id = frameId;
    snapshot.geometry = geometry;
    snapshot.formatIdentity = formatIdentity;
    snapshot.blocks.reserve(m_Impl->input.size());
    output.bytes.assign(input, input + kNativeHeader);
    output.bytes.resize(kWireHeader, 0);
    output.recordStarts.push_back(0);
    output.frameId = frameId;
    output.baseFrameId = base ? base->id : 0;
    output.stats.inputBytes = size;
    output.stats.keyframe = !base;
    output.criticalBytes = kWireHeader;
    uint32_t updates = 0;
    bool nativeFallback = false;
    m_Impl->group.clear();
    m_Impl->predictions.clear();

    auto compress = [&](const uint8_t* source, size_t bytes) -> size_t {
        m_Impl->compressed.resize(size_t(LZ4_compressBound(int(bytes))));
        const int result = LZ4_compress_fast_extState_fastReset(
            &m_Impl->lz4, reinterpret_cast<const char*>(source), m_Impl->compressed.data(),
            int(bytes), int(m_Impl->compressed.size()), 1);
        return result > 0 ? size_t(result) : bytes;
    };

    auto predict = [&](size_t bytes) {
        m_Impl->residual.assign(m_Impl->group.begin(), m_Impl->group.begin() + bytes);
        for (const auto& prediction : m_Impl->predictions) {
            if (!prediction.previous || prediction.payloadOffset >= bytes) continue;
            write32(m_Impl->residual.data() + prediction.headerOffset + 16, kXorRaw);
            const size_t count = std::min(prediction.payloadBytes, bytes - prediction.payloadOffset);
            for (size_t i = 0; i < count; ++i)
                m_Impl->residual[prediction.payloadOffset + i] ^= prediction.previous->bytes[i];
        }
    };

    auto flushGroup = [&]() {
        if (m_Impl->group.empty()) return;
        if (nativeFallback) {
            m_Impl->group.clear();
            m_Impl->predictions.clear();
            return;
        }
        size_t payloadBytes = 0;
        size_t cleared = 0;
        bool havePrediction = false;
        for (const auto& prediction : m_Impl->predictions) {
            payloadBytes += prediction.payloadBytes;
            cleared += prediction.payloadBytes == 0;
            havePrediction |= prediction.previous != nullptr;
        }
        // A small sample avoids full LZ4 passes on high-entropy frames. Account
        // for the update wrappers when estimating a useful byte reduction.
        const size_t sampleBytes = std::min(size_t(4096), m_Impl->group.size());
        const size_t rawSample = compress(m_Impl->group.data(), sampleBytes);
        size_t xorSample = sampleBytes;
        if (havePrediction) {
            predict(sampleBytes);
            xorSample = compress(m_Impl->residual.data(), sampleBytes);
        }
        const bool useXor = xorSample < rawSample;
        const size_t bestSample = useXor ? xorSample : rawSample;
        const size_t usefulBytes = payloadBytes + cleared * kUpdateHeader;
        const bool worthwhile = uint64_t(bestSample) * m_Impl->group.size() * 100u <
                                uint64_t(sampleBytes) * usefulBytes * 94u;
        size_t storedBytes = m_Impl->group.size();
        if (worthwhile) {
            if (useXor) {
                predict(m_Impl->group.size());
                storedBytes = compress(m_Impl->residual.data(), m_Impl->residual.size());
            } else {
                storedBytes = compress(m_Impl->group.data(), m_Impl->group.size());
            }
        }
        const size_t groupedBytes = kUpdateHeader + ((storedBytes + 3u) & ~size_t(3u));
        const bool grouped = worthwhile && groupedBytes < m_Impl->group.size();
        const size_t bytes = grouped ? groupedBytes : m_Impl->group.size();
        if (bytes > wireLimit - output.bytes.size()) {
            nativeFallback = true;
        } else {
            const size_t offset = output.bytes.size();
            if (grouped) {
                output.recordStarts.push_back(offset);
                output.bytes.resize(offset + bytes, 0);
                uint8_t* header = output.bytes.data() + offset;
                write32(header, uint32_t(bytes));
                write32(header + 4, read32(m_Impl->group.data() + 4));
                write32(header + 8, uint32_t(m_Impl->group.size()));
                write32(header + 12, uint32_t(storedBytes));
                write32(header + 16, kGroupLz4);
                write32(header + 20, 0);
                std::memcpy(header + kUpdateHeader, m_Impl->compressed.data(), storedBytes);
                ++updates;
                output.stats.compressedBlocks += uint32_t(m_Impl->predictions.size() - cleared);
            } else {
                output.bytes.insert(output.bytes.end(), m_Impl->group.begin(), m_Impl->group.end());
                for (const auto& prediction : m_Impl->predictions)
                    output.recordStarts.push_back(offset + prediction.headerOffset);
                updates += uint32_t(m_Impl->predictions.size());
                output.stats.rawBlocks += uint32_t(m_Impl->predictions.size() - cleared);
            }
            output.stats.clearedBlocks += uint32_t(cleared);
        }
        m_Impl->group.clear();
        m_Impl->predictions.clear();
    };

    auto appendUpdate = [&](uint32_t id, const Block* block, const Block* previous) -> bool {
        if (nativeFallback) return true;
        const uint32_t codec = block ? kRaw : kClear;
        const uint8_t* payload = block ? block->bytes.data() : nullptr;
        const size_t rawBytes = block ? block->bytes.size() : 0;
        const size_t recordBytes = kUpdateHeader + rawBytes;
        if (id >= coarse) {
            if (recordBytes > kMaxGroupBytes - m_Impl->group.size()) flushGroup();
            if (nativeFallback) return true;
            const size_t offset = m_Impl->group.size();
            m_Impl->group.resize(offset + recordBytes, 0);
            uint8_t* header = m_Impl->group.data() + offset;
            write32(header, uint32_t(recordBytes));
            write32(header + 4, id);
            write32(header + 8, uint32_t(rawBytes));
            write32(header + 12, uint32_t(rawBytes));
            write32(header + 16, codec);
            write32(header + 20, block ? block->crc : 0);
            if (rawBytes) std::memcpy(header + kUpdateHeader, payload, rawBytes);
            m_Impl->predictions.push_back({offset, offset + kUpdateHeader, rawBytes,
                previous && previous->bytes.size() == rawBytes ? previous : nullptr});
            return true;
        }
        if (recordBytes > wireLimit - output.bytes.size()) {
            nativeFallback = true;
            return true;
        }
        const size_t offset = output.bytes.size();
        output.recordStarts.push_back(offset);
        output.bytes.resize(offset + recordBytes, 0);
        uint8_t* header = output.bytes.data() + offset;
        write32(header, uint32_t(recordBytes));
        write32(header + 4, id);
        write32(header + 8, uint32_t(rawBytes));
        write32(header + 12, uint32_t(rawBytes));
        write32(header + 16, codec);
        write32(header + 20, block ? block->crc : 0);
        if (rawBytes) std::memcpy(header + kUpdateHeader, payload, rawBytes);
        ++updates;
        if (codec == kClear) ++output.stats.clearedBlocks;
        else if (codec == kRaw) ++output.stats.rawBlocks;
        if (id < coarse) output.criticalBytes = output.bytes.size();
        return true;
    };

    size_t baseIndex = 0;
    for (const auto& incoming : m_Impl->input) {
        while (base && baseIndex < base->blocks.size() && base->blocks[baseIndex].id < incoming.id) {
            if (!appendUpdate(base->blocks[baseIndex].id, nullptr, nullptr)) {
                clearOutput(output);
                return false;
            }
            ++baseIndex;
        }
        const Entry* previous = base && baseIndex < base->blocks.size() &&
                                base->blocks[baseIndex].id == incoming.id ? &base->blocks[baseIndex++] : nullptr;
        std::shared_ptr<const Block> block;
        if (previous && sameRecord(input + incoming.offset, incoming.size, *previous->block)) {
            block = previous->block;
            ++output.stats.unchangedBlocks;
        } else {
            block = makeBlock(input + incoming.offset, incoming.size);
            if (!appendUpdate(incoming.id, block.get(), previous ? previous->block.get() : nullptr)) {
                clearOutput(output);
                return false;
            }
        }
        snapshot.nativeBytes += block->bytes.size();
        snapshot.blocks.push_back({incoming.id, std::move(block)});
    }
    while (base && baseIndex < base->blocks.size()) {
        if (!appendUpdate(base->blocks[baseIndex++].id, nullptr, nullptr)) {
            clearOutput(output);
            return false;
        }
    }
    flushGroup();
    chargeSnapshot(snapshot);
    if (!m_Impl->cache.canRetain(snapshot)) {
        error = "hybrid snapshot exceeds reference cache limits";
        clearOutput(output);
        return false;
    }
    if (nativeFallback || output.bytes.size() >= snapshot.nativeBytes + kEnvelopeBytes) {
        if (snapshot.nativeBytes > wireLimit - kEnvelopeBytes) {
            error = "hybrid native refresh exceeds wire budget; reserve envelope bytes in encoder budget";
            clearOutput(output);
            return false;
        }
        output.bytes.resize(kWireHeader);
        output.recordStarts.resize(1);
        output.criticalBytes = kWireHeader;
        output.baseFrameId = 0;
        output.stats = {};
        output.stats.inputBytes = size;
        output.stats.keyframe = true;
        output.stats.rawBlocks = uint32_t(snapshot.blocks.size());
        for (const auto& entry : snapshot.blocks) {
            output.recordStarts.push_back(output.bytes.size());
            output.bytes.insert(output.bytes.end(), entry.block->bytes.begin(), entry.block->bytes.end());
            // Native fallback is a full ordinary intra payload, so restore the
            // current sequence directly in the wire records as well.
            const size_t start = output.recordStarts.back();
            write32(output.bytes.data() + start, read32(output.bytes.data() + start) |
                    (read32(input) & kSequenceMask));
            if (entry.id < coarse) output.criticalBytes = output.bytes.size();
        }
        updates = 0;
        nativeFallback = true;
        base = nullptr;
    }
    uint8_t* header = output.bytes.data();
    write32(header + 8, kWireMagic);
    write32(header + 12, kFeatureVersion | (uint32_t(kEnvelopeBytes) << 16));
    write32(header + 16, nativeFallback ? kKeyframe | kNativeFrame : (base ? 0 : kKeyframe));
    write32(header + 20, updates);
    write64(header + 24, frameId);
    write64(header + 32, output.baseFrameId);
    write32(header + 40, uint32_t(snapshot.nativeBytes));
    write32(header + 44, canonicalChecksum(input, snapshot));
    write32(header + 48, uint32_t(output.bytes.size()));
    write32(header + 56, uint32_t(output.criticalBytes));
    write32(header + 60, maxBlocks(geometry));
    write32(header + 52, wireChecksum(header, output.bytes.size()));
    output.stats.wireBytes = output.bytes.size();
    if (output.baseFrameId == 0 && m_Impl->ack != 0) {
        // Full refreshes let the receiver retire prior references. Stop using
        // the old ACK and ignore in-flight ACKs from before this transition.
        // Advance only when retiring an active base: advancing for every full
        // frame would starve references whenever RTT exceeds frame cadence.
        m_Impl->ack = 0;
        m_Impl->ackFloor = frameId;
    }
    m_Impl->cache.insert(std::move(snapshot), m_Impl->ack);
    if (!m_Impl->cache.find(m_Impl->ack)) m_Impl->ack = 0;
    m_Impl->lastId = frameId;
    return true;
}

bool Sender::acknowledge(uint64_t frameId)
{
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    if (frameId == 0) {
        m_Impl->cache.clear();
        m_Impl->ack = 0;
        m_Impl->ackFloor = m_Impl->lastId == UINT64_MAX ? UINT64_MAX : m_Impl->lastId + 1;
        // Preserve lastId: a refresh cannot make an old reliable ACK refer to a
        // newly reused ID. Caller-owned frame IDs continue increasing.
        return true;
    }
    if (frameId < m_Impl->ackFloor || frameId <= m_Impl->ack || !m_Impl->cache.find(frameId)) return false;
    m_Impl->ack = frameId;
    return true;
}

void Sender::reset()
{
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    m_Impl->cache.clear();
    m_Impl->ack = 0;
    m_Impl->ackFloor = 0;
    m_Impl->lastId = 0;
}

size_t Sender::retainedBytes() const
{
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    return m_Impl->cache.bytes;
}

size_t Sender::retainedFrames() const
{
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    return m_Impl->cache.frames.size();
}

bool Receiver::decode(const uint8_t* wire, size_t size, DecodedFrame& output, std::string& error)
{
    clearOutput(output);
    error.clear();
    const auto fail = [&](const char* message) {
        error = message;
        clearOutput(output);
        return false;
    };
    if (!wire || size < kWireHeader || size % 4 != 0 ||
        size > m_Impl->cache.limits.maxFrameBytes || size > UINT32_MAX || !isHybridFrame(wire, size))
        return fail("invalid hybrid frame size or magic");
    StreamGeometry geometry{};
    if (!parseHeader(wire, geometry, error)) return false;
    if (!sameGeometry(geometry, m_Impl->geometry)) return fail("hybrid geometry differs from stream");
    if (read32(wire + 12) != (kFeatureVersion | (uint32_t(kEnvelopeBytes) << 16)) ||
        (read32(wire + 16) != 0 && read32(wire + 16) != kKeyframe &&
         read32(wire + 16) != (kKeyframe | kNativeFrame)) || read64(wire + 64) != 0)
        return fail("unsupported hybrid version, flags or reserved fields");
    if (read32(wire + 48) != size || read32(wire + 52) != wireChecksum(wire, size))
        return fail("hybrid wire length or checksum mismatch");
    const uint64_t id = read64(wire + 24);
    const uint64_t baseId = read64(wire + 32);
    const uint32_t flags = read32(wire + 16);
    const uint32_t count = read32(wire + 20);
    const uint32_t nativeBytes = read32(wire + 40);
    const uint32_t criticalBytes = read32(wire + 56);
    const uint32_t totalBlocks = read32(wire + 4) & 0xFFFFFFu;
    if (!id || id <= m_Impl->lastId || baseId >= id || bool(flags & kKeyframe) != (baseId == 0) ||
        count > (size - kWireHeader) / kUpdateHeader ||
        nativeBytes < kNativeHeader || nativeBytes > m_Impl->cache.limits.maxFrameBytes ||
        nativeBytes % 4 != 0 || uint64_t(totalBlocks) * 8u + 8u > nativeBytes ||
        criticalBytes < kWireHeader || criticalBytes > size || criticalBytes % 4 != 0 ||
        read32(wire + 60) != maxBlocks(geometry))
        return fail("invalid hybrid frame metadata");
    const Snapshot* base = baseId ? m_Impl->cache.find(baseId) : nullptr;
    if (baseId && (!base || !sameGeometry(base->geometry, geometry) ||
                  base->formatIdentity != (read32(wire + 4) & 0xFF000000u)))
        return fail("hybrid reference frame is unavailable");
    Snapshot snapshot;
    snapshot.id = id;
    snapshot.geometry = geometry;
    snapshot.formatIdentity = read32(wire + 4) & 0xFF000000u;
    snapshot.blocks.reserve(totalBlocks);
    output.stats.inputBytes = nativeBytes;
    output.stats.wireBytes = size;
    output.stats.keyframe = !base;
    size_t offset = kWireHeader;
    size_t baseIndex = 0;
    uint32_t lastBlock = 0;
    const uint32_t limit = maxBlocks(geometry);
    const uint32_t coarse = coarseBlocks(geometry);
    size_t expectedCritical = kWireHeader;

    if (flags & kNativeFrame) {
        if (count || uint64_t(nativeBytes) + kEnvelopeBytes != size)
            return fail("invalid hybrid native refresh metadata");
        output.records.assign(wire, wire + kNativeHeader);
        output.records.insert(output.records.end(), wire + kWireHeader, wire + size);
        std::vector<InputRecord> input;
        StreamGeometry parsed{};
        if (!parseInput(output.records.data(), output.records.size(), parsed, input, error)) {
            clearOutput(output);
            return false;
        }
        for (const auto& record : input) {
            auto block = makeBlock(output.records.data() + record.offset, record.size);
            snapshot.nativeBytes += block->bytes.size();
            snapshot.blocks.push_back({record.id, std::move(block)});
            if (record.id < coarse) expectedCritical = kEnvelopeBytes + record.offset + record.size;
        }
        if (canonicalChecksum(wire, snapshot) != read32(wire + 44) ||
            criticalBytes != expectedCritical || snapshot.nativeBytes != nativeBytes)
            return fail("hybrid native refresh checksum or critical prefix mismatch");
        chargeSnapshot(snapshot);
        if (!m_Impl->cache.canRetain(snapshot)) return fail("hybrid snapshot exceeds reference cache limits");
        output.stats.rawBlocks = uint32_t(snapshot.blocks.size());
        output.frameId = id;
        output.baseFrameId = 0;
        m_Impl->cache.insert(std::move(snapshot));
        m_Impl->lastId = id;
        return true;
    }

    auto appendBlock = [&](const Entry& entry) -> bool {
        if (entry.block->bytes.size() > size_t(nativeBytes) - snapshot.nativeBytes)
            return false;
        snapshot.nativeBytes += entry.block->bytes.size();
        snapshot.blocks.push_back(entry);
        return snapshot.blocks.size() <= totalBlocks;
    };

    bool haveLastBlock = false;
    auto processUpdate = [&](const uint8_t* record, size_t available, bool inGroup) -> bool {
        if (available < kUpdateHeader) return fail("truncated hybrid update header");
        const uint32_t recordBytes = read32(record);
        const uint32_t blockId = read32(record + 4);
        const uint32_t rawBytes = read32(record + 8);
        const uint32_t storedBytes = read32(record + 12);
        const uint32_t codec = read32(record + 16);
        const uint32_t crc = read32(record + 20);
        if (blockId >= limit || (haveLastBlock && blockId <= lastBlock) ||
            (inGroup ? (codec != kRaw && codec != kClear && codec != kXorRaw) : codec > kXorLz4) ||
            (inGroup && blockId < coarse) ||
            uint64_t(kUpdateHeader) + ((uint64_t(storedBytes) + 3u) & ~uint64_t(3u)) != recordBytes ||
            recordBytes > available)
            return fail("invalid hybrid update size, ID or codec");
        if (codec == kClear) {
            if (rawBytes || storedBytes || crc) return fail("invalid hybrid clear record");
        } else if (rawBytes < kNativeHeader || rawBytes > kMaxBlockBytes || rawBytes % 4 != 0 ||
                   storedBytes == 0 || storedBytes > rawBytes ||
                   ((codec == kRaw || codec == kXorRaw) && storedBytes != rawBytes) ||
                   ((codec == kLz4 || codec == kXorLz4) && storedBytes >= rawBytes) ||
                   (blockId < coarse && codec != kRaw)) {
            return fail("invalid hybrid coefficient sizes or critical compression");
        }
        for (size_t padding = kUpdateHeader + storedBytes; padding < recordBytes; ++padding)
            if (record[padding]) return fail("nonzero hybrid update padding");
        while (base && baseIndex < base->blocks.size() && base->blocks[baseIndex].id < blockId) {
            if (!appendBlock(base->blocks[baseIndex++])) return fail("hybrid reconstruction exceeds announced size");
            ++output.stats.unchangedBlocks;
        }
        const Entry* previous = base && baseIndex < base->blocks.size() &&
                                base->blocks[baseIndex].id == blockId ? &base->blocks[baseIndex++] : nullptr;
        if (codec == kClear) {
            if (!previous) return fail("hybrid clear has no reference block");
            ++output.stats.clearedBlocks;
        } else {
            auto block = std::make_shared<Block>();
            block->bytes.resize(rawBytes);
            const char* payload = reinterpret_cast<const char*>(record + kUpdateHeader);
            if (codec == kRaw || codec == kXorRaw) std::memcpy(block->bytes.data(), payload, rawBytes);
            else {
                if (LZ4_decompress_safe(payload, reinterpret_cast<char*>(block->bytes.data()),
                                        int(storedBytes), int(rawBytes)) != int(rawBytes))
                    return fail("invalid hybrid LZ4 payload");
            }
            if (codec == kXorLz4 || codec == kXorRaw) {
                if (!previous || previous->block->bytes.size() != rawBytes)
                    return fail("hybrid residual has no matching reference block");
                for (size_t i = 0; i < rawBytes; ++i)
                    block->bytes[i] ^= previous->block->bytes[i];
            }
            const uint32_t word0 = read32(block->bytes.data());
            if (word0 & 0xF0000000u || ((word0 >> 16) & 0xFFFu) * 4u != rawBytes ||
                (read32(block->bytes.data() + 4) >> 8) != blockId)
                return fail("hybrid reconstructed native record is invalid");
            block->crc = checksum(block->bytes.data(), block->bytes.size());
            if (block->crc != crc) return fail("hybrid coefficient checksum mismatch");
            if (!appendBlock({blockId, std::move(block)})) return fail("hybrid reconstruction exceeds announced size");
            if (codec == kRaw && !inGroup) ++output.stats.rawBlocks;
            else ++output.stats.compressedBlocks;
        }
        lastBlock = blockId;
        haveLastBlock = true;
        return true;
    };

    for (uint32_t update = 0; update < count; ++update) {
        if (size - offset < kUpdateHeader) return fail("truncated hybrid update header");
        const uint8_t* record = wire + offset;
        const uint32_t recordBytes = read32(record);
        const uint32_t blockId = read32(record + 4);
        if (read32(record + 16) == kGroupLz4) {
            const uint32_t rawBytes = read32(record + 8);
            const uint32_t storedBytes = read32(record + 12);
            if (blockId < coarse || blockId >= limit || rawBytes < kUpdateHeader ||
                rawBytes > kMaxGroupBytes || rawBytes % 4 != 0 || !storedBytes || storedBytes >= rawBytes ||
                read32(record + 20) != 0 || recordBytes > size - offset ||
                uint64_t(kUpdateHeader) + ((uint64_t(storedBytes) + 3u) & ~uint64_t(3u)) != recordBytes)
                return fail("invalid hybrid compressed group metadata");
            for (size_t padding = kUpdateHeader + storedBytes; padding < recordBytes; ++padding)
                if (record[padding]) return fail("nonzero hybrid group padding");
            m_Impl->group.resize(rawBytes);
            if (LZ4_decompress_safe(reinterpret_cast<const char*>(record + kUpdateHeader),
                                    reinterpret_cast<char*>(m_Impl->group.data()),
                                    int(storedBytes), int(rawBytes)) != int(rawBytes))
                return fail("invalid hybrid compressed group payload");
            if (read32(m_Impl->group.data() + 4) != blockId)
                return fail("hybrid group first block differs from metadata");
            size_t inner = 0;
            while (inner < m_Impl->group.size()) {
                if (!processUpdate(m_Impl->group.data() + inner, m_Impl->group.size() - inner, true))
                    return false;
                inner += read32(m_Impl->group.data() + inner);
            }
        } else {
            if (!processUpdate(record, size - offset, false)) return false;
            if (blockId < coarse) expectedCritical = offset + recordBytes;
        }
        offset += recordBytes;
    }
    while (base && baseIndex < base->blocks.size()) {
        if (!appendBlock(base->blocks[baseIndex++])) return fail("hybrid reconstruction exceeds announced size");
        ++output.stats.unchangedBlocks;
    }
    if (offset != size || criticalBytes != expectedCritical || snapshot.nativeBytes != nativeBytes ||
        snapshot.blocks.size() != totalBlocks || canonicalChecksum(wire, snapshot) != read32(wire + 44))
        return fail("hybrid reconstruction size, critical prefix or checksum mismatch");
    chargeSnapshot(snapshot);
    if (!m_Impl->cache.canRetain(snapshot)) return fail("hybrid snapshot exceeds reference cache limits");
    output.records.assign(wire, wire + kNativeHeader);
    output.records.reserve(snapshot.nativeBytes);
    const uint32_t sequence = read32(wire) & kSequenceMask;
    for (const auto& entry : snapshot.blocks) {
        const size_t start = output.records.size();
        output.records.insert(output.records.end(), entry.block->bytes.begin(), entry.block->bytes.end());
        write32(output.records.data() + start, read32(output.records.data() + start) | sequence);
    }
    output.frameId = id;
    output.baseFrameId = baseId;
    m_Impl->cache.insert(std::move(snapshot), baseId);
    m_Impl->lastId = id;
    return true;
}

void Receiver::reset()
{
    m_Impl->cache.clear();
    m_Impl->lastId = 0;
}

uint64_t Receiver::acknowledgedFrameId() const { return m_Impl->lastId; }
size_t Receiver::retainedBytes() const { return m_Impl->cache.bytes; }
size_t Receiver::retainedFrames() const { return m_Impl->cache.frames.size(); }

} // namespace PyroWaveHybrid
