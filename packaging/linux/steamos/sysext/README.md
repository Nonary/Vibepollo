# Optional Gamescope system extension scaffold

This directory is release scaffolding, not a ready-to-activate system extension.
The normal SteamOS user install does not use it and does not modify the read-only
operating system.

The implemented [HDR capture patch](../gamescope/README.md) can be built and
validated independently. A privileged release must build a signed system-extension
image from the exact Valve Gamescope package used by one SteamOS build. The release process
must render both templates, include the rendered `extension-release` file at
`/usr/lib/extension-release.d/extension-release.vibepollo-gamescope` inside the
image, and publish the manifest and image under the same signature.

Activation tooling must fail closed unless all of these match the running host:

- OS ID, version/build ID, and architecture;
- installed Gamescope package version and `/usr/bin/gamescope` SHA-256;
- Vibepollo patch revision and extension-image SHA-256.

It must also verify the release signature before making the image visible to
`systemd-sysext`. An OS or Gamescope upgrade must deactivate the extension and
leave Vibepollo on its stock SDR capture path. Never advertise the independent
HDR path merely because an image file is present.

The image should be additive under `/opt/vibepollo-gamescope` where possible.
If replacing `/usr/bin/gamescope` is unavoidable, the release must document the
SteamOS-specific activation mechanism and retain Valve's required file
capabilities. Do not copy a patched executable into a `nosuid` home directory:
that drops the `CAP_SYS_NICE` behavior of the packaged compositor.
