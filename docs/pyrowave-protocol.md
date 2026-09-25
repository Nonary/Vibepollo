# PyroWave over the GameStream/Sunshine protocol

This fork streams [PyroWave](https://github.com/Themaister/pyrowave), Hans-Kristian
Arntzen's intra-only GPU wavelet codec, over the normal Sunshine video stream.
Every frame is independently decodable, so a lost frame costs one frame and never
needs a keyframe request. PyroWave trades bandwidth for latency: encode and decode
take well under a millisecond, but a clean picture needs hundreds of Mbps, so it is
meant for wired LANs.

The same document lives in both repositories (`moonlight-qt/docs/pyrowave-protocol.md`
and `vibeshine/docs/pyrowave-protocol.md`). Change both together.

## Codec library and bitstream version

Both ends vendor upstream PyroWave at the same commit through
`pyrowave/vendor-pyrowave.ps1` (moonlight-qt) and `third-party/pyrowave`
(vibeshine); see `VENDOR.txt`. The PyroWave bitstream has no version field, so the
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
frames are always laid out for it (see "Partial frames"), and hosts ignore `0x2`.

The host rejects `bitStreamFormat=3` with `400 BAD REQUEST` when PyroWave is
unavailable, like HEVC/AV1.

Colour: the stock `x-nv-video[0].encoderCscMode` selects range and SDR matrix
exactly as for the other codecs. 10-bit streams on an HDR display are BT.2020 PQ.
The host keeps sending the usual HDR mode and metadata control messages.

## Frames

PyroWave frames ride the stock Sunshine video path unchanged: RTP, the
`NV_VIDEO_PACKET` header and FEC block layout (up to four blocks), optional
AES-GCM, and the 8-byte short frame header in front of the first payload. The frame
header `frameType` is always `2` (IDR). moonlight-common-c trims the last payload
to `lastPayloadLen` as it does for AV1. The host ignores IDR and
reference-invalidation requests for PyroWave sessions.

Every frame is independent, so a lost packet costs at most that frame, and with
record framing usually only the detail it carried (see "Partial frames"). The one
part a client cannot do without is PyroWave's coarsest wavelet level, which record
framing puts first (see "Layout"): the "critical" packets, a few percent of a
frame (3-6% in our tests). Only they get parity (`fec_percentage` does not apply
to PyroWave):

- FEC block 0 is exactly the critical packets, with Reed-Solomon parity at the
  host's `pyrowave_critical_fec_percentage` (default 20%, at least 2 parity
  packets, in `fecInfo` as for any block). The rest of the frame follows in up to
  three blocks without parity, each at most 255 packets like the blocks of the other
  codecs. With the setting at 0, for length-prefixed framing, or when block 0 would
  exceed a Reed-Solomon block (255 packets with parity), the whole frame goes in up
  to four blocks without parity.
- The last two bytes of the 8-byte short frame header (little-endian, unused by
  stock Sunshine) carry the number of critical packets, 0 when unknown. Clients
  that do not know it ignore these bytes.
- In record framing, `NV_VIDEO_PACKET.extraFlags` bit `0x80` marks each packet
  whose frame data starts with a record (the first packet always does). Clients
  that do not know it ignore the bit; stock Sunshine only uses `0x1` (LTR).

Parity on the rest would add bytes, send time and encode time to every frame for
what is only a blurred area; PyroWave is meant for wired LANs where loss is rare.
A block holds at most 1023 packets, so the host caps a frame at 3000 packets (about
4.1 MB with 1392-byte packets) when critical FEC is on and 4000 (about 5.5 MB) when
it is off. It paces PyroWave packets at its `pyrowave_send_rate_mbps` setting, by
default twice the stream bitrate and at least 800 Mbps; the other codecs keep FEC
and the stock 800 Mbps.

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
frame header, so its frame data ends 8 bytes early. After the sequence header our
host sends two groups:

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
first repairs what parity can (the critical packets). It does not drop a PyroWave
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
FEC: the critical packets' parity is well under 1% of the frame at the default. The
per-frame byte budget is `bitrate / capture rate / 8`, where the capture rate is
measured (a game at 60 fps in a 120 fps stream gets twice the bytes per frame),
bounded to at most twice the nominal per-frame budget. PyroWave's rate control never
exceeds that budget. Padding records are counted against the wire bitrate, not the
budget.

When no new capture arrives, the host re-encodes the last image at the minimum FPS
target (`minimum_fps_target`, by default a fifth of the stream rate and at least 10
fps), so a static screen recovers from a lost frame. Repeats do not count as captures,
and the default is low enough that a game rendering below the stream rate is never
padded out with repeats at its enlarged per-frame budget.

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
