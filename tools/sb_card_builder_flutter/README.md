# SB Card Builder (Flutter)

Prepares an SD card for the ESP32-P4 SB Player from an xLights show folder.
Runs as a Windows desktop app or in the browser (Chrome or Edge), and writes
the same files as `tools/sb_card_builder.py`. See the "SB Card Builder"
section of the [main README](../../README.md) for what each tab does.

## Run and build

```
flutter run -d windows        # desktop
flutter run -d chrome         # web

flutter build windows         # -> build\windows\x64\runner\Release\
flutter build web --release   # -> build\web\
```

## Hosting the web build

The browser version opens folders through the File System Access API, so it
only works in Chrome or Edge on a computer, served over https (or `localhost`).

1. Build with the path it will be served from (omit `--base-href` for the site root):
   `flutter build web --release --base-href /sbcard/`
2. Copy everything in `build\web\` to that path on the server.
3. Make sure `.wasm` is served as `application/wasm`.
4. Serve `index.html`, `flutter_bootstrap.js` and `flutter_service_worker.js`
   with `Cache-Control: no-cache`, so visitors pick up new uploads.

To try it locally: `cd build\web` then `python -m http.server 8000` and open
`http://localhost:8000` (for a build made without `--base-href`).

## Tests

```
flutter analyze
flutter test
```

Tests that compare against real shows and the Python tool are skipped unless
`SB_SHOWS` / `SB_REFERENCE`, `SB_PIXEL16` or `SB_SYNTH` / `SB_SYNTH_REF` are set.
