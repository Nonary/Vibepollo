# Vibeshine HDR virtual DRM driver

This directory contains Vibeshine's out-of-tree virtual KMS driver. It is
derived from the Linux 7.2 VKMS driver and retains the upstream SPDX license
identifiers on every source file. The imported upstream revision is Linux tag `v7.2`
(`237a1c39e8df`).

Vibeshine's changes give each configfs-created virtual connector a stable HDR10
monitor contract:

- a CTA-861 EDID advertising BT.2020, PQ, HLG, and static HDR metadata;
- atomic `HDR_OUTPUT_METADATA`, `Colorspace`, and 8-16 `max bpc` properties;
- 10-bit RGB plane formats in addition to upstream VKMS formats; and
- an independent `/sys/kernel/config/vibeshine-drm` configfs namespace, so the
  driver can coexist with a distribution's normal `vkms` module.

The EDID is generated deterministically by `generate_hdr_edid.py`. Update the
generator, not `vibeshine_hdr_edid.h`, and regenerate the header with:

```bash
./generate_hdr_edid.py --header vibeshine_hdr_edid.h
```

The current source targets the DRM APIs in Linux 7.2. The installer builds it
for the running kernel (or registers it with DKMS when available). If it cannot
be built or loaded, Vibeshine's display-pool helper can use upstream VKMS as an
SDR fallback.

With Secure Boot enforcement, the locally built module must be signed by a key
trusted by the machine before the kernel will load it.
