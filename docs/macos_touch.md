# Direct touch on macOS 27

On macOS 27 and later, Vibepollo can pass a client's fingers to the Mac as a real touchscreen on
the streamed display. Taps, drags, pinches, and the icons in the Dock respond to them as direct
touches, the way a touchscreen Mac would. Without it, touch is emulated with the mouse: a tap
clicks, one finger scrolls (hold, then move, to drag), and two fingers scroll.

Direct touch needs two things that a download of Vibepollo doesn't give you:

- **A build you sign yourself, with a capability from Apple.** You need a paid Apple Developer
  account to turn on the HID Virtual Device (development) capability and sign the touch helper
  with it, as described below. Release builds of Vibepollo can't turn direct touch on for you,
  because Apple only grants that capability for development signing, which runs only on the
  Macs listed in your profile.
- **A Moonlight client that sends native touch.** The client has to send each finger as a touch
  event. A client that turns touches into mouse movement on its own side never sends any, so
  the Mac sees a mouse.

Without both, touch is emulated, and everything else works as it did.

## What it needs

- macOS 27 or later on the host. On earlier versions touch is always emulated.
- `native_pen_touch` enabled in the configuration (it is by default).
- The touch helper, `Vibepollo.app/Contents/Helpers/VibepolloTouch.app`, signed with the
  **HID Virtual Device (development)** capability by the same team that signed Vibepollo.
- A Moonlight client that supports native touch and has it turned on, on a phone or tablet.
  Clients name the setting differently; it is the one that passes touches to the host instead
  of moving a pointer with them.

The pen keeps working as a tablet (pressure, tilt, and buttons) either way; only fingers go to
the touchscreen.

## How it works

Making a virtual HID device needs the `com.apple.developer.hid.virtual.device` entitlement. So
that Vibepollo itself doesn't need it, a small helper owns the device:

- Vibepollo starts the helper when a client connects, so the touchscreen is ready before the
  first touch (macOS ignores a new device's first moments), after checking that the helper's
  signature is valid, that its identifier is `io.github.Nonary.vibepollo.touch-helper`, that it
  is signed by Vibepollo's own team, and that it has the entitlement.
- They talk over a socket pair made for that one helper; nothing is left on disk for another
  process to connect to. The helper checks that the process on the other end is its parent and
  is Vibepollo, signed by the helper's team, before it does anything. It exits when Vibepollo
  does, and it has no options of its own.
- Each streamed display gets its own touchscreen, so a Remote Monitor stream and each client
  touch their own display. A touchscreen is removed when the last stream on its display ends.
- If the helper is missing, unsigned, or stops, touch goes back to the emulation. Vibepollo
  logs why once and tries the helper again after 30 seconds.

## Get the development capability

You need an Apple Developer account. The free tier can't make the profile.

1. In [Certificates, Identifiers & Profiles](https://developer.apple.com/account/resources/identifiers/list),
   register an explicit App ID for the helper under your own reverse-DNS name, for example
   `com.example.vibepollo-touch`. App IDs are unique across all teams, so use your own.
2. Open the App ID, go to **Additional Capabilities**, turn on **HID Virtual Device
   (development)**, and save. The variant without "(development)" needs approval from Apple and
   isn't needed here.
3. Make a **macOS App Development** provisioning profile for that App ID, your Apple
   Development certificate, and the Mac that runs Vibepollo. Xcode can make it for you: sign in
   under **Xcode > Settings > Accounts**, then build any macOS app target with that bundle id and
   the entitlement, using automatic signing. The profile lands in
   `~/Library/Developer/Xcode/UserData/Provisioning Profiles/`.

A development profile runs only on the Macs listed in it and expires after a year; make it again
when it does.

## Sign the helper

Build Vibepollo for macOS, then run:

```bash
scripts/macos_sign_touch_helper.sh \
  --app path/to/Vibepollo.app \
  --profile path/to/profile.provisionprofile \
  --helper-identity "Apple Development: Your Name (XXXXXXXXXX)" \
  --app-identity "Developer ID Application: Your Name (TEAMID)"
```

The script checks that the profile has the capability, embeds it in the helper, signs the helper
with only the entitlements it needs, and signs the app again so its seal covers the new helper.
Use identities from one team for both; the script stops if they differ.

Signing the app again keeps its entitlements and requirements, so privacy permissions granted to
Vibepollo by the same identity carry over. A notarized download can't be changed this way
without losing its notarization, so do this on a build you made yourself.

Run the script again after each build: the build signs the helper without the entitlement.

## What touch can't do yet

- **Bring up a hidden Dock or menu bar.** macOS 27.2 has no touch gesture for either: its
  `DockSystemGesture` and `MenuBarSystemGesture` types exist, but nothing registers them, on any
  display. They appear only when the pointer rests on their edge, and touches don't move the
  pointer. Use the keyboard instead: Control-F3 moves focus to the Dock and Control-F8 to the
  status menus, which shows each one, including in a full-screen app. A client can offer these
  as buttons. Vibepollo reports its platform in serverinfo (`HostPlatform`) so a client knows
  which shortcuts apply.
- **Make the interface larger for fingers.** A Mac with a built-in touchscreen enlarges window
  buttons and menus under a finger. That doesn't happen here: on macOS 27.2.1, AppKit records
  that the last interaction was a direct touch, but windows and controls keep their size (a
  window's close button stays 14 points). On a phone, a lower stream resolution makes everything larger: at the phone's own
  point size (for example 1864 by 860 on a 932 by 430 point screen) the Mac draws at the scale
  iOS does.
- **Report finger size or pressure.** Each finger is a point. macOS hit-tests a touch at a
  single point either way.

## Without it

With the helper missing or unsigned, on macOS 26 and earlier, or with `native_pen_touch`
disabled, touch works as before: it's emulated with mouse clicks and scrolling. Vibepollo logs one
line saying why direct touch isn't used, for example:

```
Touchscreen: not using the touch helper: .../VibepolloTouch.app: it isn't signed with the HID virtual device entitlement (see docs/macos_touch.md); touch uses the emulation
```

## Troubleshooting

- The helper's own messages are in Console under the subsystem
  `io.github.Nonary.vibepollo.touch-helper`.
- `log stream --predicate 'subsystem == "com.apple.BackBoard" AND eventMessage CONTAINS "digitizer"'`
  shows "digitizer attached" with the display's UUID when a touchscreen is made, and "digitizer
  detached" when it's removed.
