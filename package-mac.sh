#!/bin/bash
# Package OpenRiseOfNations as a macOS app (and a .dmg disk image):
#   build/OpenRiseOfNations.app            (universal: Apple Silicon + Intel; ad-hoc signed unless --sign is given)
#   build/OpenRiseOfNations-<version>.dmg  (the app plus a shortcut to /Applications; skipped with --app-only)
# Usage: bash package-mac.sh [--app-only] [--sign "Developer ID Application: Name (TEAMID)"] [--notarize]
#   --app-only   only the .app (no disk image). `bash build-mac.sh --app` builds for this Mac's architecture and then does this.
#   --sign ID    codesign with this identity (hardened runtime + secure timestamp) instead of the ad-hoc signature "-".
#                CODESIGN_ID=... in the environment does the same.
#   --notarize   after signing, zip the app, run `xcrun notarytool submit --wait` with the keychain profile named by
#                NOTARY_PROFILE (default "ron-notary"; create it once with
#                `xcrun notarytool store-credentials ron-notary --apple-id <you> --team-id <TEAMID>`), then `xcrun stapler staple`
#                the app and the .dmg. Needs --sign with a Developer ID identity and the user's own Apple credentials; this
#                script never runs it unless asked.
# Needs only Apple's Command Line Tools. SKIP_BUILD=1 reuses an existing build/ronmap (and skips the universal build).
# The version is `git describe` (falling back to the VERSION file). On first launch the app looks for the game folder in
# preferences.txt (key game_dir, ~/Library/Application Support/OpenRiseOfNations) and asks with an open panel when it is not there.
set -euo pipefail
cd "$(dirname "$0")"

APP_ONLY=0
NOTARIZE=0
SIGN_ID="${CODESIGN_ID:--}"
while (($#)); do
	case "$1" in
		--app-only) APP_ONLY=1 ;;
		--notarize) NOTARIZE=1 ;;
		--sign) SIGN_ID="${2:?--sign needs an identity}"; shift ;;
		*) echo "unknown option: $1 (see the header of package-mac.sh)"; exit 2 ;;
	esac
	shift
done
if [[ $NOTARIZE == 1 && "$SIGN_ID" == "-" ]]; then
	echo "--notarize needs a Developer ID signature: add --sign \"Developer ID Application: Name (TEAMID)\"."
	exit 2
fi

NAME=OpenRiseOfNations
VERSION=$(git describe --tags --always 2>/dev/null | sed "s/^v//") || true
VERSION=${VERSION:-$(cat VERSION 2>/dev/null || echo 0.0.0)}
# CFBundleVersion must be dotted numbers: the VERSION file; the git describe string is the short version.
BUILD_VERSION=$(cat VERSION 2>/dev/null || echo 0.0.0)
APP=build/$NAME.app
DMG=build/$NAME-$VERSION.dmg

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
	UNIVERSAL=1 bash build-mac.sh
fi
[[ -x build/ronmap ]] || { echo "build/ronmap is missing: run bash build-mac.sh first."; exit 1; }

echo "Assembling $APP ..."
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp build/ronmap "$APP/Contents/MacOS/$NAME"

# Icon: tools/AppIcon.png (1024 px, macOS icon grid), else the front end's menu-logo.png, scaled into an .iconset.
ICON_SRC=tools/AppIcon.png
[[ -f "$ICON_SRC" ]] || ICON_SRC=apps/map-mac/resources/menu-logo.png
rm -rf build/AppIcon.iconset
mkdir -p build/AppIcon.iconset
for s in 16 32 128 256 512; do
	sips -z $s $s "$ICON_SRC" --out "build/AppIcon.iconset/icon_${s}x${s}.png" >/dev/null
	sips -z $((s * 2)) $((s * 2)) "$ICON_SRC" --out "build/AppIcon.iconset/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns build/AppIcon.iconset -o "$APP/Contents/Resources/AppIcon.icns"

cp LICENSE README.md "$APP/Contents/Resources/"
# The front end's own files (the main-menu logo); everything else is read from the player's install.
cp apps/map-mac/resources/* "$APP/Contents/Resources/"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>$NAME</string>
	<key>CFBundleDisplayName</key><string>$NAME</string>
	<key>CFBundleIdentifier</key><string>org.openriseofnations.app</string>
	<key>CFBundleExecutable</key><string>$NAME</string>
	<key>CFBundleIconFile</key><string>AppIcon</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>$VERSION</string>
	<key>CFBundleVersion</key><string>$BUILD_VERSION</string>
	<key>LSMinimumSystemVersion</key><string>12.0</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.strategy-games</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSPrincipalClass</key><string>NSApplication</string>
	<key>NSHumanReadableCopyright</key><string>GPL-3.0. Not affiliated with Microsoft or Big Huge Games.</string>
</dict>
</plist>
EOF

echo "Signing with ${SIGN_ID/#-/ad-hoc} ..."
if [[ "$SIGN_ID" == "-" ]]; then
	codesign --force --deep --options runtime --timestamp=none --sign - "$APP"
else
	codesign --force --deep --options runtime --timestamp --sign "$SIGN_ID" "$APP"
fi
codesign --verify --verbose=1 "$APP"

if [[ $NOTARIZE == 1 ]]; then
	PROFILE="${NOTARY_PROFILE:-ron-notary}"
	echo "Notarizing $APP with the keychain profile '$PROFILE' (Apple credentials; this uploads the app to Apple) ..."
	ZIP=build/$NAME-notarize.zip
	ditto -c -k --keepParent "$APP" "$ZIP"
	xcrun notarytool submit "$ZIP" --keychain-profile "$PROFILE" --wait
	xcrun stapler staple "$APP"
	rm -f "$ZIP"
fi

if [[ $APP_ONLY == 1 ]]; then
	echo
	echo "Done: $APP   ($(lipo -archs "$APP/Contents/MacOS/$NAME"), $VERSION)"
	exit 0
fi

echo "Creating $DMG ..."
STAGE=build/dmg-stage
rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
cp README.md "$STAGE/README.md"
hdiutil create -volname "$NAME $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null 2>&1
rm -rf "$STAGE"
if [[ $NOTARIZE == 1 ]]; then
	xcrun notarytool submit "$DMG" --keychain-profile "${NOTARY_PROFILE:-ron-notary}" --wait
	xcrun stapler staple "$DMG"
fi

echo
echo "Done:"
echo "  $APP   ($(lipo -archs "$APP/Contents/MacOS/$NAME"), $VERSION)"
echo "  $DMG   ($(du -h "$DMG" | cut -f1))"
