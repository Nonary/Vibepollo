#!/bin/bash
# Signs Vibepollo's touch helper with your own Apple Development identity and a development
# provisioning profile that includes "HID Virtual Device (development)", then signs the app that
# contains it again so its seal covers the new helper. See docs/macos_touch.md.
#
# Usage:
#   scripts/macos_sign_touch_helper.sh --app <Vibepollo.app> --profile <profile.provisionprofile> \
#       --helper-identity "<Apple Development identity>" --app-identity "<identity the app is signed with>"
#
# The helper and the app must be signed by the same team: the host only starts a helper from its
# own team, and the helper only answers a host from its own team.
set -euo pipefail

usage() {
  sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
  exit 64
}

app="" profile="" helper_identity="" app_identity=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --app) app="${2:-}"; shift 2 ;;
    --profile) profile="${2:-}"; shift 2 ;;
    --helper-identity) helper_identity="${2:-}"; shift 2 ;;
    --app-identity) app_identity="${2:-}"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "Unknown argument: $1" >&2; usage ;;
  esac
done
[[ -n "$app" && -n "$profile" && -n "$helper_identity" && -n "$app_identity" ]] || usage

app="${app%/}"
helpers=("$app"/Contents/Helpers/*Touch.app)
helper="${helpers[0]}"
if [[ ! -d "$helper" ]]; then
  echo "No touch helper in $app/Contents/Helpers. Build Vibepollo for macOS first." >&2
  exit 1
fi
[[ -f "$profile" ]] || { echo "No profile at $profile" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Read what the profile allows.
security cms -D -i "$profile" > "$work/profile.plist"
read_profile() { /usr/libexec/PlistBuddy -c "Print :$1" "$work/profile.plist" 2>/dev/null; }

team="$(read_profile 'TeamIdentifier:0')"
app_id="$(read_profile 'Entitlements:com.apple.application-identifier' || read_profile 'Entitlements:application-identifier')"
hid="$(read_profile 'Entitlements:com.apple.developer.hid.virtual.device' || true)"
expires="$(read_profile 'ExpirationDate')"
if [[ "$hid" != "true" ]]; then
  echo "The profile doesn't include com.apple.developer.hid.virtual.device." >&2
  echo "Turn on \"HID Virtual Device (development)\" for its App ID and make the profile again." >&2
  exit 1
fi
if [[ -z "$team" || -z "$app_id" || "$app_id" == *"*"* ]]; then
  echo "The profile needs an explicit App ID (no wildcard) and a team." >&2
  exit 1
fi
echo "Profile: App ID $app_id, expires $expires"

# The entitlements the helper is signed with: only what the profile grants and the helper needs.
cat > "$work/helper.entitlements" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>com.apple.application-identifier</key>
  <string>$app_id</string>
  <key>com.apple.developer.team-identifier</key>
  <string>$team</string>
  <key>com.apple.developer.hid.virtual.device</key>
  <true/>
</dict>
</plist>
EOF

# The host checks the helper's code signing identifier, which is fixed by the build (the
# helper's CFBundleIdentifier) and doesn't depend on the App ID in your profile.
identifier="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$helper/Contents/Info.plist")"

cp "$profile" "$helper/Contents/embedded.provisionprofile"
/usr/bin/codesign --force --sign "$helper_identity" --identifier "$identifier" \
  --options runtime --entitlements "$work/helper.entitlements" "$helper"

# Sign the app again, keeping its own entitlements, requirements, and flags, so its seal covers the
# re-signed helper.
/usr/bin/codesign --force --sign "$app_identity" \
  --preserve-metadata=entitlements,requirements,flags,runtime "$app"

/usr/bin/codesign --verify --deep --strict "$app"

helper_team="$(/usr/bin/codesign -dv "$helper" 2>&1 | sed -n 's/^TeamIdentifier=//p')"
app_team="$(/usr/bin/codesign -dv "$app" 2>&1 | sed -n 's/^TeamIdentifier=//p')"
if [[ "$helper_team" != "$app_team" ]]; then
  echo "The helper (team $helper_team) and the app (team $app_team) are signed by different teams." >&2
  echo "Vibepollo won't start a helper from another team; sign both with identities from one team." >&2
  exit 1
fi
echo "Signed $helper as $identifier for team $helper_team."
