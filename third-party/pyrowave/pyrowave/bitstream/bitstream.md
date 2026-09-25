# Bitstream definition

### Disclaimer

This specification is considered a draft and may change at any time.

## Introduction

PyroWave is a byte oriented image codec which is organized into a series of *packets*.
It is intended to be transmitted over transmission mediums such as IP networks.
There is no defined mapping to bit-oriented mediums such as serial interfaces.
Such adaptations must define a convention separately.

The codec is intended to be implemented efficiently as (Vulkan) compute shaders running on mainstream consumer GPUs,
designed for extremely high throughput with very low latency for both encoding and decoding
while maintaining reasonable compression ratios.

### Previous work

The design of this codec is a reimagining of [my master thesis from 2014](https://ntnuopen.ntnu.no/ntnu-xmlui/handle/11250/2400689),
with laser focus on the local game streaming use case.

### Conventions

#### Endianness

A byte is assumed to be 8 bits.
When multi-byte values are encoded, little-endian layout is assumed.

#### Code snippet interpretation

The decoding process is explained with C99 and GLSL.
Right-shift on signed integers is assumed to be an arithmetic right shift.
All structures presented are assumed to be tightly packed, i.e., no padding between fields.

### Intra video codec

PyroWave is fundamentally a still-image codec which is designed to be used for video as a series of still images.
This is often called intra-only video. This is in contrast to most video codecs which rely on inter-prediction as well
to significantly reduce bit-rate.

## Wavelet transform

PyroWave uses the Discrete Wavelet Transform (DWT) with 5 levels of decomposition.

```
*************************
* LL4 * HL4 *           *
*************    HL3    *
* LH4 * HH4 *           *
*************************
*           *           *
*    LH3    *    HH3    *
*           *           *
*************************
                          .
                            .
                              .
                                .
                                  .
                                     HH0
```

Note that some implementations would start counting at 1 instead of 0.
It was more convenient to start counting at 0 for purposes of programming.
The full-resolution image would be considered LL-1 in this diagram.

The basic concept is that a single image component (Y, Cb or Cr) is transformed into 4 sub-bands, then subsampled.
In the forward transform, the image is filtered horizontally with two filter kernels, a low-pass and a high-pass.
The even samples become the low-pass band, and odd samples become the high-pass band,
effectively deinterleaving the values after critically sub-sampling the bands.
This process repeats vertically for the two sub-bands.
This forms 4 subbands. LL is low-pass filtered both horizontally and vertically,
HL is high-pass filtered horizontally and low-pass filtered vertically.
Once the first LL0 band is computed, that band is further decomposed into {LL,HL,LH,HH}1 sub-bands, and so it goes
until the final LL4 sub-band is complete. No other LL sub-band is transmitted, since they are reconstructed from
the other bands.

The inverse transform performs the operations in reverse. The 4 subbands are interleaved back to full resolution,
then synthesis filters are applied. This transform is fully reversible assuming infinite precision.

PyroWave uses the irreversible [CDF 9/7 filter](https://en.wikipedia.org/wiki/Cohen%E2%80%93Daubechies%E2%80%93Feauveau_wavelet).
This is the same as used in JPEG2000.
This filter can be implemented using a lifting scheme.
See section F.4.8.2 in ITU-T Rec T.800 (06/2019) for reference on how to implement the lifting scheme.

Signal extension to define the filtering kernel on image edges works like JPEG2000 as well.
The input pixels are mirrored on the edges. The mirror applies to both forward and inverse transforms equally.
This can be efficiently implemented with `VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT` and `textureGather()` on GPUs.
See source code for the trick on how to do it. Note that GPU mirroring is not quite the same as JPEG2000 mirroring.

The decoding process is not bit-exact.
This is generally the case for the CDF 9/7 since it is defined in floating-point.
The inverse wavelet transform must be performed with at least FP16 precision.
The range of intermediate floating point values can exceed +/- 1.0.
Intermediate values above +/- 4.0 may be saturated to that range for practical reasons.
Inf and NaN cannot occur.

### Image dimension alignment and padding

The internal image dimensions are padded and aligned to make the transform easier to deal with.

```c
int DecompositionLevels = 5;
int Alignment = 1 << DecompositionLevels;
int MinimumImageSize = 4 << DecompositionLevels;
int NumComponents = 3;
int NumFrequencyBandsPerLevel = 4;
```

```c
int align(int value, int align)
{
  return (value + align - 1) & ~(align - 1);
}

int max(int a, int b)
{
  return (a > b) ? a : b;
}
```

`Width` and `Height` can take any value from 1 up to `2^14` (16K).
These values are defined by `BitstreamSequenceHeader` (defined later).

```c
AlignedWidth = align(Width, Alignment);
AlignedHeight = align(Height, Alignment);
AlignedWidth = max(AlignedWidth, MinimumImageSize);
AlignedHeight = max(AlignedHeight, MinimumImageSize);
```

The dimensions of each sub-band is defined as:

```
for (int level = 0; level < DecompositionLevels; level++)
{
  SubbandWidth[level] = AlignedWidth >> (level + 1);
  SubbandHeight[level] = AlignedHeight >> (level + 1);
}
```

During encoding, if `Width < AlignedWidth` or `Height < AlignedHeight`,
the image is extended using clamp-to-edge semantics on right and bottom edges.
When encoding, it is possible to fold clamp-to-edge semantics
with the wrapping behavior required by DWT into one sampling operation by cleverly adjusting the coordinates.

When decoding edge pixels, outputs which lie past the image's `Width` and `Height` are discarded after decoding.

When 4:2:0 chroma sub-sampling is used, the highest resolution sub-band does not exist,
and 4 levels of decomposition is used instead for chroma.
Decoding of Cb and Cr components stop once LL0 is decoded.

## Decoding process

Decoding happens in four stages:

- Decoding wavelet coefficients
- Scaling wavelet coefficients into floating-point (de-quantization)
- Inverse DWT per component (Y, Cb, Cr)
- DC shift fixup + clamping to [0, 1] range.

The first two and two latter can trivially be fused together for purposes of implementation.

### Decoding wavelet coefficients

Each sub-band is organized into blocks of 32x32 coefficients.
A 32x32 block of coefficients is signalled in isolation with no prediction or context.
If a 32x32 block is missing (either deliberately or by packet loss),
all coefficients are assumed to be 0.0.

A 32x32 block starts with an 8 byte header:

```c
struct BitstreamHeader
{
  uint16_t ballot;
  uint16_t payload_words : 12;
  uint16_t sequence : 3;
  uint16_t extended : 1;
  uint32_t quant_code : 8;
  uint32_t block_index : 24;
};
```

- `ballot` signals 1 bit per 8x8 block inside the 32x32 block.
  If a bit is 0, the group of 8x8 coefficients is all 0, and are skipped.
  The assumed layout of the 8x8 blocks is row-major with respect to bit-position.
  If `ballot` is 0, the 32x32 block should not be transmitted at all.
  A decoder can safely ignore such a block if it is observed.
- `payload_words` is the number of u32 words contained within the 32x32 block, including this header.
  The effective byte size is `payload_words * sizeof(uint32_t)`.
  This alignment can lead to padding bytes at the end. The content of alignment bytes is ignored.
  When multiple 32x32 blocks are packed into a network packet, this field is sufficient to extract the individual blocks.
- `sequence` is a wrapping counter used to detect frame progression. It increases by one (modulo 8) every frame transmitted.
  This can be used to detect when a new frame begins, and if there have been frame drops. Signalling presentation timing
  is left to external mechanisms.
- `extended` is 1 if this packet contains "special" information which is not related to decoding wavelet coefficients.
- `quant_code` encodes how to scale wavelet coefficients into floating-point (de-quantization).
- `block_index` is a linear block index. Every possible 32x32 block is assigned a block index (defined later).
  Out of range block indices must be recognized and skipped by a decoder,
  but a decoder is allowed to discard any previously received data if observed.

#### Start of frame header

If `extended` is 1, the definition of the header is reinterpreted to:

```c
enum
{
  BITSTREAM_EXTENDED_CODE_START_OF_FRAME = 0,
};

enum
{
  CHROMA_RESOLUTION_420 = 0,
  CHROMA_RESOLUTION_444 = 1
};

enum
{
  CHROMA_SITING_CENTER = 0,
  CHROMA_SITING_LEFT = 1
};

enum
{
  YCBCR_RANGE_FULL = 0,
  YCBCR_RANGE_LIMITED = 1
};

enum
{
  COLOR_PRIMARIES_BT709 = 0,
  COLOR_PRIMARIES_BT2020 = 1
};

enum
{
  YCBCR_TRANSFORM_BT709 = 0,
  YCBCR_TRANSFORM_BT2020 = 1
};

enum
{
  TRANSFER_FUNCTION_BT709 = 0,
  TRANSFER_FUNCTION_PQ = 1
};

struct BitstreamSequenceHeader
{
  uint32_t width_minus_1 : 14;
  uint32_t height_minus_1 : 14;
  uint32_t sequence : 3;
  uint32_t extended : 1;
  uint32_t total_blocks : 24;
  uint32_t code : 2;
  uint32_t chroma_resolution : 1;
  uint32_t color_primaries : 1;
  uint32_t transfer_function : 1;
  uint32_t ycbcr_transform : 1;
  uint32_t ycbcr_range : 1;
  uint32_t chroma_siting : 1;
};
```

The only defined extended header is currently this one. The kind of header is signalled by
`code` for which only `BITSTREAM_EXTENDED_CODE_START_OF_FRAME` is defined.
Other values for `code` is reserved for future use which can extend this definition in any required way.

A `BITSTREAM_EXTENDED_CODE_START_OF_FRAME` should be transmitted for every frame of video.
This packet may be sent in any order relative to other packets for any given frame.
A decoder may discard received packet until it has observed
`BITSTREAM_EXTENDED_CODE_START_OF_FRAME` at least once.

In a video sequence, `width_minus_1`, `height_minus_1` and `chroma_resolution` must remain invariant.
What a "video sequence" is, is not defined here, but left to relevant higher-level protocols.
These fields may also be provided through external means allowing a decoder to be instantiated before any
packet data is received by decoder, but this mechanism is also not defined here.

`total_blocks` specifies up-front how many non-zero 32x32 blocks are encoded for the given frame `sequence`.
When the decoder observes that a given `sequence` has received enough packets to decode this many
blocks, the decoding process can begin immediately.

If the received number of unique packets is less than `total_blocks`, this indicates packet loss or similar.
Any missing block is decoded as all zero values. If a missing block belongs
to any high-pass band, this leads to intermittent blurring which may be barely noticeable.
A loss in the LL4 band is more severe, and selectively applying Forward Error Correction to those packets in particular
may be considered. Other error masking techniques may be employed as desired, which is not defined here.
The decoder may reject duplicate `block_index` for the same `sequence` as well.
The encoder may send duplicate `block_index` values for the same `sequence` for purposes of crude error correction.

Decoding an incomplete frame may be forced by external means. Typical reasons to force a decode can be:

- A timeout was reached while waiting for all packets to come through
- The next sequence count was observed, meaning we likely won't see any more packets from the previous sequence.

Image dimensions are signalled here:

```c
Width = width_minus_1 + 1;
Height = height_minus_1 + 1;
```

`chroma_resolution` signals if 420 sub-sampling is used or not. If 420 is used, level = 0 for non-luma components
are skipped, and are not assigned a `block_index`.
If 420 subsampling is used, `Width` and `Height` must be even.

The last 5 fields are purely "video usability" information. It has no semantic impact on the decoding process,
but are used to signal how to interpret the output Y, Cb and Cr values.
The definitions of full/limited, bt709/bt2020, etc, are left to the respective specifications.
bt2020 YCbCr transform is the NCL variant.

There is no distinction for 8-bit and 10-bit.
The decoding process is defined in floating-point, and it is not specified how the final decoded values are quantized into a UNORM image.

#### Decoding 8x8 blocks

After the 8 byte header follows `N` values, packed into two arrays to make memory access more practical:

```c
BitstreamHeader Header;
uint16_t CodeWords[N];
uint8_t QScale[N];
uint8_t Payload[PayloadSize];
uint8_t SignPayload[SignSize];
```

where `N` is `popcount(ballot)` (the number of bits set to 1 in `ballot`).
For any given 8x8 block, the index into the array is given by how many preceding `ballot` bits are set to 1,
i.e., the 8x8 blocks are tightly packed.

An 8x8 block may be fully out of range of the particular sub-band.
In this case, the decoded values are discarded after dequantization.
An encoder should not encode an out of range 8x8 block.
If an 8x8 block is partially out of range, only the out of range coefficients are discarded after dequantization.

```c
if (ballot & (1u << RowMajor8x8Index))
  Compacted8x8Index = popcount(ballot & ((1u << RowMajor8x8Index) - 1u));
else
  Compacted8x8Index = undefined;
```

`PayloadSize` depends on the contents of `CodeWords` and `QScale`.
`SignSize` depends on the coefficient values.

The wavelet coefficients are organized as bit-planes, without any entropy coding.
This ensures extremely fast and parallel encoding and decoding at the cost of bitrate.
Each 8x8 block is organized into 8 4x2 subblocks. The ordering of these subblocks is given as:

```
subblock order:
------> +x
|  0  4
|  1  5
|  2  6
|  3  7
+y
```

Within a 4x2 subblock, the ordering is given as:

```
pixel order:
---------> +x
| 0 2 4 6
| 1 3 5 7
+y
```

Decoding a linear index between 0 and 63 into a 8x8 coordinate can be done with this GLSL snippet as an example:

```glsl
ivec2 unswizzle8x8(uint index)
{
  uint y = bitfieldExtract(index, 0, 1);
  uint x = bitfieldExtract(index, 1, 2);
  y |= bitfieldExtract(index, 3, 2) << 1;
  x |= bitfieldExtract(index, 5, 1) << 2;
  return ivec2(x, y);
}
```

Each subblock encodes 8 magnitude values with a variable number of bits.
The number of bits is encoded `CodeWords[i]` and `QScale[i]`.

```c
int SubblockPosition4x2(int IndexWithin8x8Block)
{
  return IndexWithin8x8Block >> 3;
}

BitPlanes = (CodeWords[Compacted8x8Index] >> (2 * SubblockPosition4x2(IndexWithin8x8Block))) & 0x3;
BitPlanes += QScale[Compacted8x8Index] & 0xf;
```

The bitplanes are organized starting with the most significant, down to least significant.
The ordering of the bits corresponds to the pixel order for a 4x2 subblock.
The bitplanes are loaded from the `Payload[]` array. The offset to use for each 4x2 subblock is implicit.
All payload data is tightly packed organized as:

```c
// Pseudo-code

int offset = 0;
foreach_bit(BlockIndex8x8 in ballot)
{
  for (int SubblockIndex = 0; SubblockIndex < 8; SubblockIndex++)
  {
    // Figure out BitPlanes based on CodeWords and QScale.
	
    // Decode 8 values in one go
    int8 values = int8(0);
    for (int plane = 0; plane < BitPlanes; plane++)
    {
      values <<= 1;
      // bit 0 -> element 0
      // bit 1 -> element 1
      // ... etc
      values |= ConvertBitsToVector(Payload[offset++]);
    }
  }
}
```

After decoding magnitude values, all non-zero coefficients also decode a sign bit.
The sign bits are tightly packed after all magnitude bit-planes.
The bit position of the sign bit is the number of non-zero coefficients that come before it in the 32x32 block.
The ordering of coefficients is the same as for magnitude planes: 8x8 block, then subblock, then pixel within subblock.
In every byte of `SignPayload[]`, signs are packed such that smaller to larger index go from LSBs to MSBs.
If a sign bit is 1, the resulting coefficient is negative.

#### Dequantization

After integer coefficients are decoded, they are converted to floating-point by scaling it with a factor.
The factor depends on `quant_code` from the 32x32 block header as well as `QScale[]` per 8x8 block.
The effective factor is computed as:

```c
float Block32x32Scale(uint8_t quant_code)
{
  const int MaxScaleExp = 4;
  // Custom FP formulation for numbers in (0, 16) range.
  int e = MaxScaleExp - (quant_code >> 3);
  int m = quant_code & 0x7;
  float inv_quant = (1.0f / (8.0f * 1024.0f * 1024.0f)) * (float)((8 + m) * (1 << (20 + e)));
  return inv_quant;
}

float Block8x8Scale(uint8_t code)
{
  return (float)code / 8.0 + 0.25;
}

float scale = Block32x32Scale(quant_code) * Block8x8Scale((QScale[i] >> 4) & 0xf);

// Output from coefficient decoding.
float DecodedCoefficientFloat = DecodedCoefficient;

// Apply deadzone.
if (DecodedCoefficientFloat > 0.0)
  DecodedCoefficientFloat += 0.5;
else if (DecodedCoefficientFloat < 0.0)
  DecodedCoefficientFloat -= 0.5;

float DequantizedCoefficient = scale * DecodedCoefficientFloat;
```

A deadzone quantizer is used here, meaning that quantization biases towards zero.

#### Block index ordering

Components are ordered from 0 to 2:

- Component 0: Y
- Component 1: Cb
- Component 2: Cr

Bands are ordered from 0 to 3:

- Band 0: LL (low-pass)
- Band 1: HL (horizontal high-pass, vertical low-pass)
- Band 2: LH (horizontal low-pass, vertical high-pass)
- Band 3: HH (high-pass)

```c
int block_index = 0;

for (int level = DecompositionLevels - 1; level >= 0; level--)
{
  for (int component = 0; component < NumComponents; component++)
  {
    if (level == 0 && component != 0 && IsYCbCr420)
      continue;

    for (int band = (level == DecompositionLevels - 1 ? 0 : 1); band < 4; band++)
    {
      uint32_t level_width = AlignedWidth >> (level + 1);
      uint32_t level_height = AlignedHeight >> (level + 1);

      int blocks_x_32x32 = (level_width + 31) / 32;
      int blocks_y_32x32 = (level_height + 31) / 32;

      for (int y = 0; y < blocks_y_32x32; y++)
        for (int x = 0; x < blocks_x_32x32; x++)
          AssignBlockIndex(level, component, band, block_index++);
    }
  }
}
```

#### Inverse DC shift

After completing the decoding process, wavelet values are shifted and clamped into `[0, 1]` range.
The bit-depth of the decoded image is not specified and may depend on the use case of the image.
E.g. a PQ encoded image may desire a higher bit-depth.

```c
float clamp(float v, float lo, float hi)
{
  if (v < lo)
    return lo;
  else if (v > hi)
    return hi;
  else
    return v;
}

LumaShifted = clamp(DecodedLuma + 0.5, 0.0, 1.0);
CbShifted = clamp(DecodedCb + 0.5, 0.0, 1.0);
CrShifted = clamp(DecodedCr + 0.5, 0.0, 1.0);

WriteToImage(LumaShifted);
WriteToImage(CbShifted);
WriteToImage(CrShifted);
```