# HearBridge Browser for Android v1.0.0

by X-F1REBALL-X. A small WebView wrapper around the player page that the
HearBridge Browser PS5 payload serves at `http://<console-ip>:8096/`.

- Start screen: scans the phone's /24 for `GET :8096/status` with
  `"app":"HearBridge Browser"`, tries the last-used console first (and opens it
  on a cold start when it answers), lists every console found, Retry, manual IP.
- Player: JavaScript + DOM storage, media plays without a gesture, the screen
  stays on while the page's `playing` flag is set, exports (`<a download href="blob:">`)
  are streamed through a JS bridge into Downloads (MediaStore on Android 10+,
  storage permission on 7-9). Links off the console (Ko-fi, X) open in the
  system browser.
- Cleartext http is allowed (LAN IPs cannot be whitelisted by domain).
- minSdk 24, targetSdk 35, Java only, no libraries.

Not yet: playback with the screen off / in the background (planned later).

## Build

```
./gradlew assembleRelease lintRelease
# -> app/build/outputs/apk/release/app-release.apk
```

Needs JDK 17+ and an Android SDK (`local.properties`: `sdk.dir=...`).
Release signing reads `keystore/keystore.properties` (storeFile, storePassword,
keyAlias, keyPassword). The `keystore/` folder is git-ignored: never commit it.
Without it the release APK is built unsigned.
