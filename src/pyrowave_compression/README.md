# Independent PyroWave compression version 1

Every frame is intra-only; no reference cache, previous-frame coefficients or ACK.
The native sequence header and coarse prefix stay raw. Detail groups contain up
to 64 KiB of complete native records with current sequence bits. Fast LZ4 uses a
4 KiB sampling gate and raw fallback. Total output never exceeds original framed
bytes. Each group starts on a shard boundary and has four little-endian u32 words:
magic 0xFFFFFFFE, compressed bytes, native bytes, CRC32C of native bytes. Compressed
bytes follow, padded to four bytes with zeros. Groups exclude coarse records,
sequence/padding headers and nested groups. CRC32C has SSE4.2 and scalar paths.

Loss skips affected groups; intact record-start shards allow resynchronization.
An intact coarse prefix permits partial rendering. A lost shard can remove up to
64 KiB of detail, but never creates an inter-frame dependency. Expansion validates
size and checksum; the framing parser validates every native record before GPU use.
Critical FEC is unchanged; optional detail FEC learns from native records before
compression. Negotiate PyroWaveCompressionVersion=1, pyrowaveCompression=1 and
feature 0x8 together; never reinterpret old Hybrid feature 0x4 or its wire format.

The full protocol is in docs/pyrowave-protocol.md. Keep shared C++ and LZ4 identical
in both repositories. CPU costs and wire savings require device/content measurement.
