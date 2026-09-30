# PyroWave hybrid transport version 1

This optional transport preserves ordinary PyroWave coefficients exactly. The
GPU decoder still clears before each frame and receives a reconstructed native
intra frame. It adds no future-frame lookahead. Both endpoints must negotiate
version 1, and the sender's coefficient packing must initialize unused sign
bits and alignment bytes so equal coefficients produce equal records.

The sender retains immutable snapshots and selects a reference only after a
receiver ACK names a retained snapshot. Unchanged native block records are
omitted. Removed records are explicit clears. Changed detail records are grouped
into at most 64 KiB and compressed with fast LZ4. A small sample chooses ordinary
data or XOR against matching reference records and skips expensive full passes
when a useful reduction is unlikely. Every group has a raw fallback. The coarsest wavelet records stay raw at
the beginning of the protected prefix. Three-bit native sequence values are
normalized for comparison and restored to the current value on reconstruction.

If update overhead would exceed an ordinary frame's size, the sender emits a
wrapped full native frame. Reserve 64 bytes from the maximum transport capacity
for the envelope; the ordinary image bitrate budget remains unchanged. The full refresh retires an active reference and
rejects earlier pending ACKs; subsequent full frames still accept delayed ACKs
for retained fresh snapshots, allowing RTT to exceed the frame period.

The receiver accepts only complete transport frames. Unrepaired packet loss
drops the frame while retaining healthy references. An unavailable reference,
invalid metadata, or a checksum failure drops the frame and requests a fresh
full frame through ACK ID zero. It never inherits
unverified data or acknowledges a damaged reconstruction. The caller must
validate the reconstructed native framing before sending the successful ACK.

Both caches default to at most 16 snapshots and a conservative 64 MiB charge.
Unchanged records share immutable storage. The active sender ACK and the base
named by an incoming delta are retained while those bounds allow; insufficient
history causes a full refresh rather than an ambiguous reference.

All integers are little endian. The wire starts with the original 8-byte native
sequence header, followed by this 64-byte envelope (offsets include that header):

| Offset | Field |
| --- | --- |
| 8 | u32 magic `0x31485950` (`PYH1`) |
| 12 | u16 version 1, u16 envelope size 64 |
| 16 | u32 flags: 0 delta, 1 full updates, 3 full native body |
| 20 | u32 update count (zero for a full native body) |
| 24 | u64 nonzero increasing frame ID |
| 32 | u64 base ID (zero for a full frame) |
| 40 | u32 reconstructed native frame bytes |
| 44 | u32 reconstruction CRC32C of the native header and block descriptors |
| 48 | u32 complete wire bytes |
| 52 | u32 wire CRC32C, with this field treated as zero |
| 56 | u32 end of complete critical prefix |
| 60 | u32 maximum block count for the stream geometry |
| 64 | u64 reserved, zero |

Update frames contain sorted, size-delimited records at offset 72. Each update
has six u32 fields: total record bytes, block ID, reconstructed record bytes,
stored payload bytes, codec, and normalized native record CRC32C. The payload
follows and is padded to four bytes with zeros. Codecs are 0 raw, 1 LZ4, 2 clear,
and 3 XOR plus LZ4. A clear has no payload or checksum. XOR requires a matching
base record of exactly the reconstructed size. Reconstructed records must have
the specified block ID, valid native size, and zero normalized sequence bits.

Codec 4 is an LZ4 group. Its reconstructed-byte field is the exact group size,
at most 64 KiB; its stored size is smaller, its checksum field is reserved zero,
and its block ID is the first expanded block ID. The decompressed group contains
complete ordinary update headers and payloads, using only codecs 0, 2, or 5.
Codec 5 is raw XOR against a matching reference record and is legal only inside
a group. Nested groups are forbidden. Every expanded ID must increase across
the complete frame, and coarsest records are forbidden inside groups. The
envelope update count counts outer containers and ungrouped updates.

A full native body contains canonical native block records directly at offset
72, without update headers. Its sequence header remains at offset zero. The
body and header together are the complete ordinary intra frame; both wire and
canonical checksums still apply.

The reconstruction checksum covers the exact 8-byte native header followed by
sorted 12-byte descriptors: block ID, native record bytes, and that record's
normalized CRC32C. Every new block CRC is verified against its actual bytes;
reused records retain their previously verified CRC. This avoids rescanning all
unchanged coefficient bytes while checking the complete reference state.

CRC32C uses the reflected Castagnoli polynomial `0x82F63B78`, initialized and
finalized with `0xFFFFFFFF`. x86 CPUs with SSE4.2 use a runtime-selected hardware
path; other CPUs use the identical portable slicing-by-eight implementation.
These checksums detect transport/reference errors; they do not
replace the stream's negotiated transport authentication.
