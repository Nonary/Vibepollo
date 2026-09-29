# PyroWave over the GameStream/Sunshine protocol

This fork streams [PyroWave](https://github.com/Themaister/pyrowave), Hans-Kristian
Arntzen's intra-only GPU wavelet codec, over the normal Sunshine video stream.
Every frame is independently decodable, so a lost frame costs one frame and never
needs a keyframe request. PyroWave trades bandwidth for latency: encode and decode
take well under a millisecond, but a clean picture needs hundreds of Mbps, so it is
meant for wired LANs.

The same document lives in both repositories (`moonlight-qt/docs/pyrowave-protocol.md`
and `Vibepollo/docs/pyrowave-protocol.md`). Change both together.

## Codec library and bitstream version

Both ends vendor upstream PyroWave at the same commit through
`pyrowave/vendor-pyrowave.ps1` (moonlight-qt) and `third-party/pyrowave`
(Vibepollo); see `VENDOR.txt`. The PyroWave bitstream has no version field, so the
host advertises the vendored commit (below) and the client warns on a mismatch.

| Name | Value |
|---|---|
| `PYROWAVE_BITSTREAM_ID` | `186f0393` (first 8 hex digits of the vendored pyrowave commit) |

Local patches (buffer pool, 4:4:4 payload sizing, zero-length block guard) do not
change the bitstream.

## Negotiation

The capability and format constants match the Aurora (moonlight-qt fork) and
Solarflare (Sunshine fork) PyroWave implementation, so those clients and hosts can
negotiate with ours. Other PyroWave forks share the capability bits but use
different frame framing; see "Compatibility".

### `/serverinfo`

The host ORs these bits into `ServerCodecModeSupport` when PyroWave encoding works
on the capture adapter:

For a paired HTTPS `/serverinfo` request, a capable host also returns
`PyroWaveHostLinkMbps` (zero if its outbound route is not a known physical wired
link) and `PyroWaveBandwidthProbeBytes=33554432`. The link number is the host's
local transmit speed, not measured end-to-end throughput. Linux and Windows
resolve the route to the requesting client; Linux ignores virtual, wireless,
half-duplex and inactive interfaces.

The paired client can GET `/pyrowave-bandwidth-probe` over its pinned HTTPS
connection. It receives exactly 32 MiB of fixed binary payload
to time. The client discards a warm-up and uses the slowest of three measurements,
then reserves 20% for protocol overhead and contention. The result is a bulk
host-to-client throughput estimate. It does not prove that live UDP bursts will
avoid packet loss, so calibration remains a recommendation rather than a stream
quality guarantee.

| Bit | Value | Meaning |
|---|---|---|
| `SCM_PYROWAVE` | `0x00800000` | 8-bit 4:2:0 |
| `SCM_PYROWAVE_444` | `0x01000000` | 8-bit 4:4:4 |
| `SCM_PYROWAVE_HDR10` | `0x02000000` | 10-bit 4:2:0 (HDR10 when the display is HDR) |
| `SCM_PYROWAVE_HDR10_444` | `0x04000000` | 10-bit 4:4:4 |

`SCM_MASK_10BIT` gains the two HDR10 bits and `SCM_MASK_YUV444` the two 4:4:4 bits.

Client-local format constants (moonlight-common-c `Limelight.h`, never on the wire):

| Constant | Value |
|---|---|
| `VIDEO_FORMAT_PYROWAVE` | `0x010000` |
| `VIDEO_FORMAT_PYROWAVE_444` | `0x020000` |
| `VIDEO_FORMAT_PYROWAVE_HDR10` | `0x040000` |
| `VIDEO_FORMAT_PYROWAVE_HDR10_444` | `0x080000` |
| `VIDEO_FORMAT_MASK_PYROWAVE` | `0x0F0000` |
| `VIDEO_FORMAT_MASK_10BIT` | `0xCAA00` (adds `0x40000 \| 0x80000`) |
| `VIDEO_FORMAT_MASK_YUV444` | `0xACC04` (adds `0x20000 \| 0x80000`) |

Extending the two masks makes the existing SDP and `/launch` code emit
`chromaSamplingType=1`, `dynamicRangeMode=1` and `hdrMode=1` for the matching
PyroWave profiles without further changes.

### RTSP DESCRIBE (host to client)

```
a=rtpmap:99 PYROWAVE/90000
a=x-ss-pyrowave.bitstream:186f0393
```

The `rtpmap` line is a capability marker only; no RTP payload type 99 is sent.
Aurora clients require it. Our client selects PyroWave only when the user chose the
PyroWave codec and `ServerCodecModeSupport` has `SCM_PYROWAVE`; it does not require
the marker, because the dimizago/azafrob-protocol hosts omit it and their framing is
detected per frame (see below). It picks the best mutual profile: HDR10 4:4:4,
HDR10, 4:4:4, then 8-bit 4:2:0. PyroWave is never chosen automatically, since it
needs a wired link with hundreds of Mbps to spare.

### RTSP ANNOUNCE (client to host)

| Attribute | Value |
|---|---|
| `x-nv-vqos[0].bitStreamFormat` | `3` (PyroWave; 0/1/2 are H.264/HEVC/AV1) |
| `x-ss-video[0].chromaSamplingType` | `1` for 4:4:4, else `0` (stock attribute) |
| `x-nv-video[0].dynamicRangeMode` | `1` for 10-bit, else `0` (stock attribute) |
| `x-ss-video[0].pyrowaveAdaptiveFec` | `0` (Aurora attribute; its presence selects record framing) |
| `x-ss-video[0].pyrowaveAdaptiveBitrate` | `0` (Aurora attribute) |
| `x-ss-video[0].pyrowaveFeatures` | bitmask, below |

`pyrowaveFeatures` bits:

| Bit | Name | Meaning |
|---|---|---|
| `0x1` | `PYROWAVE_FEATURE_RECORD_FRAMING` | Client parses record framing with padding records. |

`0x2` was once reserved for partial-frame decoding. No bit is needed: record-framed
frames use the partial-recovery layout when the negotiated packet size supports
record alignment (see "Record framing"), and hosts ignore `0x2`.

The host rejects `bitStreamFormat=3` with `400 BAD REQUEST` when PyroWave is
unavailable, like HEVC/AV1.

Colour: the stock `x-nv-video[0].encoderCscMode` selects range and SDR matrix
exactly as for the other codecs. 10-bit streams on an HDR display use BT.2020 PQ
unless 10-bit SDR is preferred or SDR is forced; those streams use the SDR matrix.
The host keeps sending the usual HDR mode and metadata control messages.

## Frames

PyroWave frames ride the stock Sunshine video path unchanged: RTP, the
`NV_VIDEO_PACKET` header and FEC block layout (up to four blocks), optional
AES-GCM, and the 8-byte short frame header in front of the first payload. The frame
header `frameType` is always `2` (IDR). moonlight-common-c trims the last payload
to `lastPayloadLen` as it does for AV1. The host ignores IDR and
reference-invalidation requests for PyroWave sessions.

Every frame is independent, so a lost packet costs at most that frame, and with
record framing usually only the detail it carried (see "Partial frames"). The
coarsest wavelet level is required for decoding. With aligned record framing and
`pyrowave_critical_fec_percentage > 0`, its leading shards receive parity at that
percentage, with at least two parity shards. Protection is skipped if the critical
data and parity cannot fit in one Reed-Solomon block of 255 shards. Length-prefixed
frames and record frames without shard alignment have no critical prefix and
receive no parity. `fec_percentage` does not apply to PyroWave. Finer detail
normally has no parity, but the host can protect it alongside the critical data
when sustained low frame rates make loss-induced flicker more visible.

For record framing with `pyrowave_critical_fec_percentage > 0`, the encoded image
budget is capped at the encoder bitrate divided by the negotiated frame rate.
At session setup, the host derives the encoder bitrate from the client's requested
bandwidth budget after allowing for audio, packet overhead and control traffic.
A frame-rate drop leaves bandwidth available for protection instead of making
each encoded image larger. For example, an encoder bitrate of 800 Mbps at 120 FPS
allows about 0.83 MB of encoded data per frame; that ceiling remains about 0.83 MB
at 60 or 30 FPS. Faster-than-negotiated submissions still receive a smaller
elapsed-time budget.

The adaptive policy requires at least 250 ms of frames below negotiated FPS with at
least 75% unchanged encoded record bytes. It compares records by block ID,
ignoring sequence numbers, padding and packing order. This is a conservative
proxy for picture activity, not a pixel motion measurement: changes in encoder
quantization count as changes too. A 1% timing tolerance ignores small cadence
noise. The requested detail parity percentage is
`min(50, 100 * (negotiated FPS / observed FPS - 1))`. Thus a 120 FPS session
requests about 9% at 110 FPS, 20% at 100 FPS, 33% at 90 FPS and 50% at 80 FPS or
below. This cap is independent of the critical block's configured parity rate;
the critical rate remains unchanged. Motion or a return to negotiated cadence disables extra
protection immediately; cadence smoothing and the entry delay avoid reacting to
isolated hitches.

Extra parity uses only unused bytes from the current bitrate allowance, after
accounting for data, baseline parity, network headers and encryption. The
allowance uses the smaller of the current and smoothed frame intervals, capped
at 50 ms, without accumulating idle credit. A frame that already fills its
allowance gets no extra parity. This limits overhead against the configured
bitrate; it does not estimate available downstream network capacity.
Unused bandwidth beyond useful parity is left idle; the sender does not try to
fill the configured bitrate after a cadence drop.

The 50% cap corresponds to an ideal one-third missing-packet tolerance within a
fully protected block, not a guarantee at one-third average network loss. The
planner may select less parity or protect only part of the detail to satisfy
the wire budget and the four-block limit.

The planner prefers protecting all finer detail, reducing its parity percentage
if necessary. Frames too large for that receive partial coverage: first by
extending the critical block, then adding protected detail blocks where space
allows. Every protected block stays within the Reed-Solomon limit of 255 data
plus parity packets, and the frame still fits four blocks. The announced
critical packet count continues to describe only the required coarse data.
No new client protocol is needed. Setting `pyrowave_critical_fec_percentage=0`
also disables adaptive detail FEC; length-prefixed clients keep their existing
behavior. The debug statistic `PyroWave: detail packets protected` reports actual
coverage after budget and block limits, rather than just the requested rate.

The frame is split into at most four blocks. The host caps a frame at 3000
packets when critical FEC is on and 4000 when it is off. The codec budget and
record padding honor this limit and the negotiated packet size. PyroWave
refreshes the routed link speed every two seconds for pacing; if it is unavailable,
pacing follows packet demand and stream bitrate. `pyrowave_send_rate_mbps`
is ignored. Other codecs retain their existing pacing and FEC settings.

When sending falls behind, a newly encoded PyroWave frame replaces that
session's pending frame in the send queue. The frame already being sent
completes, and other sessions keep their queue positions.

### Record framing (host default for PyroWave-aware clients)

Used when the client sent `x-ss-video[0].pyrowaveAdaptiveFec` or
`pyrowaveFeatures & 0x1`. The frame payload is a concatenation of 32-bit
little-endian records:

1. A PyroWave `BitstreamSequenceHeader` (8 bytes, `extended = 1`, `code = 0`),
   first in the frame, with the negotiated width, height and chroma resolution and
   `total_blocks` equal to the number of block records in the frame.
2. PyroWave block records (`BitstreamHeader` + payload, `payload_words` words
   including the header, `sequence` equal to the sequence header's), in any order.
   PyroWave places blocks by `block_index`.
3. Padding records, anywhere between other records:
   `0xFFFFFFFF`, a word count `N`, then `N` zero words (`8 + 4N` bytes). Decoded as a
   sequence header this would be an impossible 16384-pixel width with code 3, so it
   cannot be confused with real data.

Layout: the RTP layer splits the frame into payloads of `packetSize - 16` bytes
(1376 for the usual 1392-byte packets); the first payload also carries the 8-byte
frame header, so its frame data ends 8 bytes early. Alignment requires this payload
size to be a multiple of four and at least 24 bytes. Otherwise (for example, with
1390-byte packets), the host copies the encoder's records without reordering or
padding and reports no critical prefix. The layout below applies when alignment
is available. After the sequence header our host sends two groups:

1. PyroWave's coarsest wavelet level: block indices below
   `12 * ceil(W / 32) * ceil(H / 32)`, where `W` and `H` are the frame's width and
   height rounded up to 32 pixels (at least 128), divided by 32. PyroWave indexes
   these first. They end in the last critical packet.
2. All other records.

Within each group, oversized records (too large to share a payload with a padding
record: more than a payload less 8 bytes) come first, in encoder order, and span
payloads; a minimal padding record goes before one that would otherwise end 4 bytes
before a payload boundary. The group's other records follow, packed first-fit: when
the next record does not fit the rest of a payload, a later record of the group
that does fills it, and padding fills it only when none fits (well under 1% of the
frame in practice, against 16-21% if records kept strict encoder order). None of
them crosses a payload boundary, and no 4-byte remainder (too small for a padding
record) is left, so every payload after the second group's oversized records starts
with a record.

Receivers must not rely on this layout to parse a complete frame: they parse
records sequentially and accept straddling records and padding anywhere. Only
partial-frame recovery depends on it.

Receivers must reject a block record whose `payload_words` is smaller than 2 (the
header itself), a record that runs past the end of the frame, a sequence header
whose size or chroma resolution differs from the negotiated stream, a second
sequence header, a block record before the sequence header or with another
`sequence`, and `block_index` values outside the frame. A rejected frame is
dropped; the next frame is independent.

This is the Aurora/Solarflare framing, minus their conditional-replenishment
extensions (sequence code 1 "keep previous" frames and header-only zero blocks),
which upstream PyroWave does not decode and our host never sends.

### Length-prefixed framing (compatibility)

Used for clients that request PyroWave without either attribute above (the
azafrob/andygrundman/dimizago Moonlight PyroWave clients, including Moonlight
PyroWave for Xbox):

```
[u32 LE packet_count] { [u32 LE size] [size bytes: PyroWave packet] } * packet_count
```

Each PyroWave packet is the output of `pyrowave_encoder_packetize` with a 1024-byte
boundary; packet 0 starts with the sequence header.

Our client detects the framing per frame: a record-framed frame starts with a
sequence header, whose first word has bit 31 (`extended`) set, while a packet count
never does.

## Decoding

The decoder is cleared before every frame (`pyrowave_decoder_clear`), all records
are pushed, and the frame is decoded when `pyrowave_decoder_decode_is_ready`
reports it complete. Clearing per frame keeps PyroWave's 3-bit sequence counter from
discarding frames after four or more consecutive network drops.

### Partial frames

Our client decodes a record-framed frame that lost packets. moonlight-common-c
first repairs what parity can (critical packets and any protected detail). It does not drop a PyroWave
frame whose FEC block cannot complete: once the next block or frame starts
arriving, each missing data packet is replaced by zeros and delivered as a
`BUFFER_TYPE_LOST` buffer. The frame is still dropped when its first packet
(sequence header) or a whole FEC block is missing, which parity on the critical
block makes rare. Packets flagged `0x80` arrive as `BUFFER_TYPE_RECORD_START`
buffers, and the critical packet count as `DECODE_UNIT.pyrowaveCriticalPackets`.

The parser skips every record that lost a byte. A record whose header arrived but
whose payload did not has a known end, so parsing continues after it. When a
header itself was lost, parsing resumes at the next received payload flagged as
starting with a record. From a host that does not flag payloads, it resumes at the
next received payload only once a finer record of ordinary size (one that fits a
payload with 8 bytes to spare) was seen inside a single payload, which the layout
above makes a record boundary; before that the rest of the frame is given up.

The coarsest level is intact when none of the announced critical packets was lost
(without an announcement: when no loss came before the first finer record). The
frame is then decoded if more than 90% of its records arrived
(`pyrowave_decoder_decode_is_ready_with_sideband` with no pristine-band
requirement); missing finer blocks decode as zero coefficients, which blurs their
area for that frame. Losing the packets right after the critical ones blurs the
most, since they hold the next-coarsest level. PyroWave's own pristine-band check
is not used because it cannot tell a lost block from an all-zero block that was
never sent.

Length-prefixed frames with any loss lose the frame, and frames from hosts that do
not follow the layout lose everything after the first lost record header.

Output planes are three single-channel UNORM images (R8 for 8-bit streams, R16 for
10-bit): full-resolution Y, and Cb/Cr at half resolution in each direction for
4:2:0. Sample values are normalized code values `code / (2^N - 1)` with `N` = 8 or
10, in the negotiated range and matrix. 4:2:0 chroma is sited at the center of each
2x2 luma quad.

## Rate control

The client's configured bitrate (`x-ml-video.configuredBitrateKbps`) is used exactly
as for the other codecs; the host subtracts audio and control overhead, but not
FEC. The initial per-frame byte budget uses the negotiated frame rate. With record
framing and FEC enabled, subsequent image budgets use the smaller of that interval
and the elapsed time between encoding attempts, including repeated images.
Slower submissions therefore keep the same encoded image ceiling and make room
for adaptive detail FEC. Disabling FEC or using length-prefixed compatibility
framing retains elapsed-time image budgets. Dynamic bitrate changes update the
ceiling using the new bitrate and the original negotiated FPS. The negotiated
transport capacity also bounds the budget. Budgets are rounded
down to codec words; a budget too small for the codec headers skips that attempt.
Framing and network headers still add overhead to the codec bitrate.

When no new capture arrives, the host re-encodes the last image. By default the
wait is one negotiated frame interval, so a static screen can recover promptly
from a lost frame. An explicit `minimum_fps_target` can reduce the repeat cadence
and is capped at the negotiated frame rate. Capture mutex waits are also bounded
by the negotiated frame interval. Packetizer storage follows the codec's reported
bitstream buffer size plus its sequence header.

Guidance: about 1.6 bits per pixel is visually clean for 4:2:0 SDR (Themaister's
reference point, 200 Mbps at 1080p60). 4:4:4 costs about 1.6x, and 10-bit about
1.15x.

## Compatibility

| Peer | Result |
|---|---|
| Aurora client, our host | Negotiates PyroWave, record framing. Aurora's decoder is an older WiVRn-derived PyroWave; frames decode only if its bitstream matches `186f0393`. |
| Our client, Solarflare host | Negotiates PyroWave. Solarflare's full frames decode; its "keep previous" frames (code 1) are rejected and dropped. A frame that lost packets keeps only the records before its first lost record header unless Solarflare lays frames out as ours does. |
| Xbox / azafrob-protocol client, our host | Negotiates PyroWave, length-prefixed framing. Bitstream compatibility depends on their PyroWave commit. |
| Our client, dimizago Vibepollo host | Negotiates PyroWave from the SCM bits; length-prefixed framing is detected per frame. |
| Stock Moonlight | Never sees PyroWave; negotiates H.264/HEVC/AV1 as before. |
