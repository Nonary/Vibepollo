# LZ4

The `lz4.c`, `lz4.h`, and `LICENSE` files are unmodified library sources from
[LZ4 v1.10.0](https://github.com/lz4/lz4/releases/tag/v1.10.0), under the BSD
2-clause license. They implement the optional Hybrid PyroWave block compression.

Source: `https://raw.githubusercontent.com/lz4/lz4/v1.10.0/lib/`.
Only the block codec is compiled; the CLI and frame-format library are not used.
Keep these files identical in the Moonlight and Vibeshine repositories.

Moonlight embeds the license in its executable and prints it with
`moonlight --lz4-license`, without opening a display or stream. Vibeshine
includes the license in its packaged files.
