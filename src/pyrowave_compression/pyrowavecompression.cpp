#include "pyrowavecompression.h"
#define LZ4_STATIC_LINKING_ONLY
#include "../../third-party/lz4/lz4.h"
#include <algorithm>
#include <array>
#include <cstring>
#if defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#include <nmmintrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <nmmintrin.h>
#endif
namespace PyroWaveCompression {
namespace {
uint32_t read32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void write32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
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

constexpr uint32_t kPadding = 0xFFFFFFFFu;
struct Record { size_t offset, size; };
}

struct Encoder::Impl {
    Impl() { LZ4_initStream(&stream, sizeof(stream)); }
    LZ4_stream_t stream{};
    std::vector<uint8_t> raw;
    std::vector<char> compressed;
    std::vector<Record> detail;
};
Encoder::Encoder() : m_Impl(new Impl) {}
Encoder::~Encoder() = default;
uint32_t checksumCrc32c(const uint8_t* data, size_t size, bool forceScalar) {
    return (forceScalar ? crcUpdateScalar(0xFFFFFFFFu, data, size) :
                         crcUpdate(0xFFFFFFFFu, data, size)) ^ 0xFFFFFFFFu;
}

bool Encoder::encode(const uint8_t* input, size_t size, size_t criticalBytes,
                     size_t shardPayload, EncodedFrame& out, std::string& error) {
    out.bytes.clear(); out.recordStartShards.clear(); out.compressedGroups = 0;
    out.criticalBytes = criticalBytes;
    error.clear();
    auto fail = [&](const char* message) { error = message; out.bytes.clear(); return false; };
    if (!input || size < 8 || size > kMaxFrameBytes || size % 4 ||
        !(read32(input) & 0x80000000u) || read32(input) >= kMagic ||
        criticalBytes < 8 || criticalBytes > size || criticalBytes % 4 ||
        shardPayload < 32 || shardPayload % 4)
        return fail("invalid independent compression input or shard size");
    auto& impl = *m_Impl;
    impl.detail.clear();
    size_t pos = 8, blocks = 0;
    const uint32_t sequence = read32(input) & 0x70000000u;
    std::vector<size_t> originalStarts{0};
    bool criticalBoundary = criticalBytes == 8;
    while (pos < size) {
        if (size - pos < 8) return fail("truncated native record");
        originalStarts.push_back(pos);
        const uint32_t word = read32(input + pos);
        const bool padding = word == kPadding;
        const uint64_t bytes = padding ? 8ull + uint64_t(read32(input + pos + 4)) * 4 :
                                       uint64_t((word >> 16) & 0xFFFu) * 4;
        if (bytes < 8 || bytes > size - pos ||
            (!padding && ((word & 0x80000000u) || (word & 0x70000000u) != sequence)))
            return fail("invalid native record size or sequence");
        if (pos < criticalBytes && bytes > criticalBytes - pos)
            return fail("critical prefix splits a record");
        if (!padding) {
            ++blocks;
            if (pos >= criticalBytes) impl.detail.push_back({pos, size_t(bytes)});
        }
        pos += size_t(bytes);
        criticalBoundary |= pos == criticalBytes;
    }
    if (!criticalBoundary || blocks != (read32(input + 4) & 0xFFFFFFu))
        return fail("native record count or critical boundary mismatch");
    out.bytes.assign(input, input + criticalBytes);
    std::vector<size_t> starts;
    for (size_t offset : originalStarts) if (offset < criticalBytes) starts.push_back(offset);
    auto mark = [&]() { starts.push_back(out.bytes.size()); };
    auto padToBoundary = [&]() {
        const size_t remaining = (shardPayload - (out.bytes.size() + 8) % shardPayload) % shardPayload;
        if (!remaining) return;
        const size_t padding = remaining >= 8 ? remaining : remaining + shardPayload;
        mark();
        const size_t offset = out.bytes.size();
        out.bytes.resize(offset + padding, 0);
        write32(out.bytes.data() + offset, kPadding);
        write32(out.bytes.data() + offset + 4, uint32_t((padding - 8) / 4));
    };
    auto appendRaw = [&](const Record& record) {
        const size_t remaining = shardPayload - (out.bytes.size() + 8) % shardPayload;
        // Reserve eight bytes for a padding header, as ordinary record framing does.
        if (record.size + 8 > remaining) padToBoundary();
        mark();
        out.bytes.insert(out.bytes.end(), input + record.offset, input + record.offset + record.size);
    };
    size_t first = 0;
    while (first < impl.detail.size()) {
        impl.raw.clear();
        size_t end = first;
        while (end < impl.detail.size() && impl.detail[end].size <= kMaxGroupBytes - impl.raw.size()) {
            const auto& record = impl.detail[end++];
            impl.raw.insert(impl.raw.end(), input + record.offset, input + record.offset + record.size);
        }
        impl.compressed.resize(size_t(LZ4_compressBound(int(impl.raw.size()))));
        auto compress = [&](size_t bytes) {
            return LZ4_compress_fast_extState_fastReset(&impl.stream,
                reinterpret_cast<const char*>(impl.raw.data()), impl.compressed.data(),
                int(bytes), int(impl.compressed.size()), 1);
        };
        const size_t sample = std::min(size_t(4096), impl.raw.size());
        const int sampled = compress(sample);
        const bool useful = sampled > 0 && uint64_t(sampled) * 100 < uint64_t(sample) * 94;
        const int stored = useful ? (sample == impl.raw.size() ? sampled : compress(impl.raw.size())) : 0;
        const size_t framed = stored > 0 ? kHeaderBytes + ((size_t(stored) + 3) & ~size_t(3)) : impl.raw.size();
        if (stored > 0 && framed + shardPayload < impl.raw.size()) {
            // Every group begins on a shard boundary, so losing its header still
            // permits recovery at the next received record-start shard.
            padToBoundary(); mark();
            const size_t offset = out.bytes.size();
            out.bytes.resize(offset + framed, 0);
            auto* header = out.bytes.data() + offset;
            write32(header, kMagic); write32(header + 4, uint32_t(stored));
            write32(header + 8, uint32_t(impl.raw.size()));
            write32(header + 12, checksum(impl.raw.data(), impl.raw.size()));
            std::memcpy(header + kHeaderBytes, impl.compressed.data(), size_t(stored));
            ++out.compressedGroups;
        } else {
            for (size_t i = first; i < end; ++i) appendRaw(impl.detail[i]);
        }
        first = end;
    }
    // Padding or high-entropy groups can erase the gain. Send the original
    // intra frame verbatim instead of expanding the image/transport budget.
    if (!out.compressedGroups || out.bytes.size() >= size) {
        out.bytes.assign(input, input + size);
        out.compressedGroups = 0;
        starts = std::move(originalStarts);
    }
    const size_t count = (out.bytes.size() + 8 + shardPayload - 1) / shardPayload;
    out.recordStartShards.assign(count, 0); out.recordStartShards[0] = 1;
    for (size_t offset : starts)
        if (offset && (offset + 8) % shardPayload == 0)
            out.recordStartShards[(offset + 8) / shardPayload] = 1;
    return true;
}

bool expandGroup(const uint8_t* group, size_t size, std::vector<uint8_t>& records,
                 std::string& error) {
    auto fail = [&](const char* message) { error = message; return false; };
    if (!group || size < kHeaderBytes || read32(group) != kMagic)
        return fail("invalid compressed detail header");
    const size_t stored = read32(group + 4), raw = read32(group + 8);
    if (!raw || raw > kMaxGroupBytes || raw % 4 || !stored || stored >= raw ||
        stored > size - kHeaderBytes || kHeaderBytes + ((stored + 3) & ~size_t(3)) != size)
        return fail("invalid compressed detail sizes");
    for (size_t i = kHeaderBytes + stored; i < size; ++i)
        if (group[i]) return fail("nonzero compressed detail padding");
    const size_t offset = records.size();
    if (offset > kMaxFrameBytes - raw) return fail("expanded frame exceeds size limit");
    records.resize(offset + raw);
    if (LZ4_decompress_safe(reinterpret_cast<const char*>(group + kHeaderBytes),
                            reinterpret_cast<char*>(records.data() + offset), int(stored), int(raw)) != int(raw) ||
        checksum(records.data() + offset, raw) != read32(group + 12)) {
        records.resize(offset);
        return fail("compressed detail payload or checksum mismatch");
    }
    return true;
}
} // namespace PyroWaveCompression
