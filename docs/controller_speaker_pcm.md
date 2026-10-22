# Controller speaker PCM source contract

This implements the Windows composite DualSense USB/IP producer for
Nonary/Vibepollo#475. It is paired with moonlight-common-c PR #2 and
moonlight-qt PR #27. It is not proof of native Windows controller playback.
Bluetooth speaker rendering and other host audio producers are outside this
change; ordinary actuator PCM remains separate.

## Negotiation and compatibility

The existing controller-arrival capability field is 16 bits. The current
capabilities occupy bits 0 through 8, pending Steam extensions occupy bits 9
through 14, and actuator PCM occupies bit 15. There is no globally free bit.
Speaker capability 0x4000 is therefore explicitly PlayStation-type scoped:
`LI_CTYPE_PS` (2) plus 0x4000 means speaker rendering. For Steam type 4 the same
bit means Steam attributes, never speaker PCM. Both host admission and client
advertisement must check controller type. The client only advertises this for
an opened Windows USB DualSense endpoint. No contributor Steam PR is modified.

The independent session feature flag 0x08 additionally opts into speaker PCM;
0x01/0x02/0x04 retain FEC, session ID and actuator meanings.
The sender also rejects any control protocol other than encrypted protocol13. Legacy clients,
clients advertising only actuator support, and non-PlayStation controllers do
not receive speaker data. The prototype was source-only, so this does not claim
compatibility with a deployed producer. The library callback is registered by
an additive start API, not by growing the legacy listener table. The pending
Steam PR's callback-table changes still require ABI integration review.

## Packet and lifecycle

Encrypted control packet 0x5602 uses unreliable channel 0x08, independently of
actuator packet 0x5601. Its little-endian payload is 984 bytes: version1,
format1 (48 kHz stereo S16LE), controller16, generation32, sequence32,
sample-position64, frame-count16 (=240), flags16 (bit0 discontinuity), then
960 sample bytes. Sample positions count stereo frames, not bytes.

The USB protocol validates isochronous packet ranges and whole eight-byte
frames before extracting speaker channels1/2 and actuator channels3/4. Padding
between USB packet offsets is excluded. The speaker packetizer owns a separate
partial block and timeout. One unsent speaker block per controller is retained
in the bounded feedback queue; replacing it never removes other feedback.
Sequences and sample positions advance even when the queue rejects a block,
so downstream gaps cannot masquerade as contiguous audio.

Detach, retained-slot rebind, malformed input and stale partial blocks reset
the speaker generation. A weak epoch token is checked by the serializer to
reject already-popped blocks from an expired or reset producer. Already-sent
network packets cannot be recalled; the client also rejects old generations
and sequences and clears discontinuities. Slot callbacks use weak ownership
and the existing mutex/lifecycle rules. Actuator-only gaps do not reset an
active speaker queue.

## Validation boundaries

Portable tests exercise the actual USB protocol, actual USB gamepad backend
with only the OS transport replaced, and the production wire serializer.
Encryption uses the existing session encoder and unreliable control sender.
Native Windows SDK/build, live encrypted network delivery and physical
controller speaker/haptic coexistence require separate validation. Do not
mark the original issue resolved from these source-stage tests alone.
