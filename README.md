# harn-android

The app uses [libghostty-vt](https://github.com/ghostty-org/ghostty) for
VT parse, scrollback, and key encoding. A JNI layer feeds a Canvas
renderer. A local PTY runs `/system/bin/sh` as the app UID.

Full libghostty GPU embedding is not available on Android. This crate
uses libghostty-vt and paints the grid in the app.

## Requirements

- JDK 17 or later
- Android SDK with compile SDK 37
- Android NDK 29 (`29.0.14206865`)
- Network on the first native build (Zig and Ghostty source)

The Gradle task downloads Zig 0.16.0 when it is missing. You do not
need a system Zig install.

## Build

1. Set `ANDROID_HOME` or keep the SDK at `~/Android/Sdk`.
2. Install NDK 29, or set `ANDROID_NDK_HOME`.
3. From this directory, run `./gradlew :app:assembleDebug`.

The first build compiles libghostty-vt for `arm64-v8a` and `x86_64`.
Later builds reuse `native/vendor/` until you force a rebuild.

Force a native rebuild:

```bash
FORCE=1 ./scripts/build-libghostty-vt.sh
./gradlew :app:assembleDebug
```

The debug APK is:

`app/build/outputs/apk/debug/app-debug.apk`

## Layout

| Path | Role |
|------|------|
| `app/src/main/java/com/example/harn/` | Single activity |
| `app/src/main/java/com/example/harn/terminal/` | Terminal view, JNI, PTY |
| `app/src/main/cpp/` | JNI for libghostty-vt and the local PTY |
| `native/pin.env` | Ghostty commit and Zig version |
| `scripts/build-libghostty-vt.sh` | Cross-compile libghostty-vt |

Generated trees are gitignored: `native/tools`, `native/ghostty`,
`native/out-*`, and `native/vendor`.

## Runtime

The activity is edge-to-edge. The terminal grid uses window insets so
text stays below the status bar and above the nav bar and IME.

Tap the terminal to open the IME. Hardware keys go through the Ghostty
key encoder. Touch scroll moves the viewport.

## Security

The PTY is reachable only by the device user of this app. The shell
runs as the app UID. A failure returns local process errors. The code
does not print secrets.

## Tests

```bash
./gradlew :app:testDebugUnitTest
```

## License

This crate uses the MIT License. See `LICENSE`.

Third-party notices live in `NOTICE`. Ghostty MIT text lives in
`licenses/ghostty-MIT.txt`.
