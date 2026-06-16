# BGMDWLDR — YouTube BGM Downloader

A native **macOS & Android** app for downloading background music from YouTube videos. Built from scratch with a custom browser emulation engine, Vulkan rendering, and native MP4 metadata injection — no Electron, no webviews, no Qt.

---

## What Makes It Different

Most YouTube downloaders are either command-line tools or bloated web apps wrapped in Electron. BGMDWLDR is a **fully native, self-contained binary** that:

- **Executes YouTube's actual player JavaScript** in an embedded QuickJS engine to extract media stream URLs
- **Renders its own UI in Vulkan** with a hand-rolled 8×8 bitmap font and a cyberpunk cyan/magenta color palette
- **Embeds album art and title metadata directly into M4A files** using native MP4 atom injection
- Runs on **macOS (desktop) and Android** from a single C/C++ codebase

---

## Screenshots

*Cyberpunk Vulkan UI with neon cyan borders, multiline URL input, and live progress bar.*

---

## Features

### Media Extraction
- Fetches YouTube page HTML and executes the page's obfuscated player JavaScript in a sandboxed QuickJS environment
- Extracts direct audio stream URLs and video metadata (title, thumbnail)
- Handles YouTube's `n` parameter deciphering and signature handling natively

### Native M4A Metadata Injection
- **Album art**: Embeds a default 512×512 JPEG cover into the M4A's `moov/udta/meta/ilst/covr` atom hierarchy
- **Title tag**: Writes the user-specified filename into the `©nam` metadata tag so your music player shows the correct song title
- Patches `stco`/`co64` chunk offsets to keep the file valid after atom insertion

### Custom Filename with `.m4a` Enforcement
- Edit the filename directly in the UI — the `.m4a` extension is enforced automatically
- Text selection, drag-to-select, and full clipboard support (copy/paste/cut) on both fields
- The filename field has **no focus concept** — cursor and selection persist even when you click into the URL box

### Vulkan UI
- Hand-rolled bitmap font renderer (8×8 glyphs, ASCII range) drawn as textured quads
- Dynamic layout that accounts for multi-line status text, wrapped download paths, and video titles
- Resizable window with density-independent scaling
- Retro cyberpunk aesthetic: cyan borders, magenta progress bars, block cursor

### Cross-Platform
| Platform | UI | HTTP | File Save | Metadata |
|----------|-----|------|-----------|----------|
| **macOS** | GLFW + Vulkan + MoltenVK | libcurl | `~/BGMDWLDR/` | Album art + title |
| **Android** | NativeActivity + Vulkan | Native Android HTTP | MediaStore | *(album art via MediaStore API)* |

---

## Quick Start (macOS)

### Prerequisites
```bash
brew install cmake glfw molten-vk curl
```

### Build
```bash
./build_macos_app.sh
```

This produces:
- The raw binary: `app/build_macos/bgmdwnldr-mac`
- An `.app` bundle: `app/build_macos/BGMDWLDR.app`

### Run
```bash
# Run the binary directly (no .app bundle needed)
./app/build_macos/bgmdwnldr-mac

# Or open the app bundle
open app/build_macos/BGMDWLDR.app
```

> **Note:** The first time you run the `.app` bundle, macOS Gatekeeper may block it. Right-click → **Open** to bypass. The raw `bgmdwnldr-mac` binary bypasses Gatekeeper entirely.

---

## Quick Start (Android)

### Prerequisites
- Android SDK + NDK (26.2.11394342)
- `$ANDROID_SDK_ROOT` set
- ADB in `$ANDROID_SDK_ROOT/platform-tools/`

### Build & Install
```bash
./rebuild.sh
```

Or step by step:
```bash
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### Launch
```bash
adb shell am start -n com.bgmdwldr.vulkan/.MainActivity
```

---

## Usage

1. **Enter a filename** (top field) — e.g. `My Song.m4a`. The `.m4a` extension is enforced automatically.
2. **Enter a YouTube URL** (bottom field) — e.g. `https://www.youtube.com/watch?v=dQw4w9WgXcQ`
3. **Press Enter** (or tap the action button on Android)
4. The app will:
   - Fetch the YouTube page
   - Execute the player's JavaScript to extract the audio stream URL
   - Download the audio
   - Save it with your custom filename to `~/BGMDWLDR/` (macOS) or MediaStore (Android)
   - Embed album art and set the title metadata tag

---

## Architecture

```
bgmdwnldr/
├── browser-emulator/          # Custom browser emulation library
│   ├── src/
│   │   ├── js_quickjs.cpp     # QuickJS JS engine integration
│   │   ├── html_media_extract.cpp
│   │   ├── browser_api_impl.cpp    # DOM stubs, XHR, console
│   │   ├── http_download.c
│   │   └── platform/          # macOS/Linux/Android abstractions
│   ├── include/               # Public headers
│   ├── tests/
│   └── third_party/
│       ├── quickjs/           # QuickJS JavaScript engine (MIT)
│       └── mbedtls/           # TLS/SSL library (Apache 2.0)
│
├── app/                       # Application code
│   ├── src/main/cpp/
│   │   ├── vulkan_ui.cpp      # Vulkan renderer, worker thread, input handling
│   │   ├── ui_layout.cpp      # Bitmap font, text layout, word wrapping
│   │   ├── mp4_metadata.cpp   # M4A album art & title metadata injection
│   │   ├── main_macos.cpp     # GLFW entry point (macOS desktop)
│   │   └── default_album_art.c    # Embedded 512×512 JPEG (~17KB)
│   └── src/main/assets/
│       ├── triangle.vert.spv  # Vulkan shaders
│       └── triangle.frag.spv
│
├── build_macos_app.sh         # macOS build script
└── rebuild.sh                 # Android build & install script
```

### Key Design Decisions

**Why QuickJS instead of a headless browser?**  
Headless Chrome or Safari would add 100+ MB of dependencies. QuickJS is a complete ES2020 engine in ~1 MB that can execute YouTube's player code directly with our stubbed DOM APIs.

**Why Vulkan instead of OpenGL or a UI toolkit?**  
The Android version uses Vulkan via NativeActivity (no Java UI). Using the same API on macOS (via MoltenVK) lets us share 100% of the rendering code between platforms.

**Why native MP4 atom injection instead of FFmpeg or taglib?**  
FFmpeg is a massive dependency. Our minimal MP4 parser finds the existing `moov/udta/meta/ilst` hierarchy, inserts `covr` (album art) and `©nam` (title) atoms, patches chunk offsets, and writes the file — all in ~400 lines of C with zero external dependencies.

---

## Tests

```bash
# MP4 metadata injection test
cd app/build_macos
./test-mp4-metadata

# Text render invariance test
./test-text-render

# Resize capture test
./test-resize-capture

# Browser emulator tests (native)
cd browser-emulator
./build.sh
./build/tests/browser-emulator-tests
```

---

## License

This project uses the following third-party libraries:
- **QuickJS** — MIT License
- **mbedtls** — Apache License 2.0
- **GLFW** — zlib/libpng License
- **stb_image** — Public Domain

---

## See Also

*No additional guides at this time.*
