# Exact existing-target lease prototype

This private prototype is disabled by default. It does not complete issue #501
and must not be enabled in a release without native platform qualification and
independent source review. No new UI selector is published.

A marked Remote Monitor profile can carry a literal `remote-monitor-target`
object with exactly three string fields: `platform` (`linux-drm` or
`windows-device`), `kind` (`physical` or `shared-virtual`), and `id` (the exact
platform identifier). This identity is separate from the configured profile
UUID and authenticated paired-client UUID. It is never environment-expanded.
Absent this object, the existing per-client builtin route is unchanged.

The Linux KScreen adapter resolves `linux-drm` to the literal canonical DRM
connector sysfs path and requires the exact output to be active and capture
registered. Shared virtual borrowing requires an existing `shared` reservation;
it cannot create a connector or borrow an arbitrary per-client private VDD.
Current width, height, refresh in millihertz, and HDR must match the effective
client request. A paired-client output override that names a different capture
output also conflicts; it cannot silently retarget the profile. A mismatch returns an explicit conflict without changing the
display. Existing app-then-client override precedence is retained. Runtime
configuration overrides use the existing transactional rollback on failed
admission; this is not a promise that no in-memory config field is touched.

The selected target and mode remain immutable until Disconnect Monitor.
Resume revalidates the same identity. Pending RTSP launches, admitted startup,
stream teardown and producer/capture contexts retain the lease. Reopening
capture revalidates the platform identity, mode and capture output, and fails
instead of selecting a replacement. Stale generations cannot release a newer
lease. Unpair and shutdown revoke role authority while existing capture holds
continue protecting the output through drain.

This prototype conservatively blocks all host display mutation while any exact
borrow exists, including mutation of unrelated outputs. This restriction avoids
an incomplete topology-wide dependency model. Normal-game and monitor creator
cleanup retain ownership metadata while blocked. Linux KScreen writes, broker
connector writes, restore/recovery/termination and display wake admission use
the same barrier. Unconfirmed helper completion blocks later borrowing, with no
automatic reconnect reset. Borrowers never invoke create, remove, mode setting or composition.

The V3 source prototype implements coordinated producers for both platforms.
Windows uses an exact helper process/creation epoch and connection token, drains
and parks its real mutation queue, and retains a host kernel mutex plus existing
native driver resources through pending/capture teardown. The final helper
RELEASE must be confirmed before ordinary native writes reopen. A helper restart
cannot bypass the retained mutex. Native Windows code remains uncompiled without
a genuine SDK and has not been installed or run against a display.

Machine-host Linux has a separate version-2 inhibit-only request. It never uses
the wake operation. Before readiness it verifies a unique context reason in
PowerDevil's active inhibition list, bracketed by exact service-owner checks;
a returned AddInhibition cookie alone is insufficient. Missing or older
producers/APIs fail admission. The helper transfers its authenticated native bus
socket through a private broker socketpair; the host retains that descriptor and
the exact shared kernel-lock open-file description. KScreen writes, connector
RPC contexts and autonomous wake callbacks require exclusive admission on the
same root-owned lock inode. No borrow creates or reconfigures an output.

Only the exact correlated host RELEASE authorizes the helper's one-byte release
record. Clean release waits for helper exit without racing a generic SIGTERM.
Unknown release, generation/connection loss or native uncertainty retains the
kernel/resource descriptors until process death and permanently fences the core
registry. Native runtime holds release before the last borrower count drops;
post-probe uncertainty prevents publication. The registry itself lives through
late process-global capture destruction. Persistent producer uncertainty markers
are not automatically cleared by reconnecting or restarting the host.

A duplicated socket proves transport ownership, not transfer of GDBus's reader,
pending calls or PowerDevil's daemon-side cookie state. The helper remains the
sole bus reader. Its authenticated health path reports owner/policy loss; native
EOF/error fails revalidation, while normal queued replies are left for that
reader. The host never reauthenticates, creates a second reader or silently
replaces a published inhibitor. Remote bus/PowerDevil failure, user revocation,
backpressure and the actual cookie lifetime after process death still require
native platform qualification. This prototype does not promise continued idle
protection after an external daemon loses its state, or atomic live-frame
behavior across external desktop changes. It remains disabled by default.

Portable proofs exercise the real registry/coordinator and monitor entrypoints.
The review packet additionally executes exact extracted pending/retirement and
Linux driver/helper/observer bodies against inert adapters. These are source and
lifecycle proofs, not device, compositor, HDR, capture-frame or installed-service
qualification. External desktop or kernel changes remain outside host ownership;
identity is rechecked before native capture open/reopen, but atomic native handle
binding and live capture behavior still require platform testing.
