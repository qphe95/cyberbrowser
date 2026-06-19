# BGMDWLDR — YouTube BGM Downloader

A native **browser emulation engine** for extracting media URLs from JavaScript-heavy sites like YouTube. Built from scratch with QuickJS, a custom DOM implementation, and native TLS/HTTP — no Electron, no webviews, no Qt.

---

## What Makes It Different

Most YouTube downloaders are either command-line tools or bloated web apps wrapped in Electron. BGMDWLDR is a **fully native, self-contained library** that:

- **Executes YouTube's actual player JavaScript** in an embedded QuickJS engine to extract media stream URLs
- **Embeds album art and title metadata directly into M4A files** using native MP4 atom injection
- Runs on **macOS, Linux, and Windows** from a single C/C++ codebase

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

### Cross-Platform
| Platform | HTTP | TLS |
|----------|------|-----|
| **macOS** | native sockets | mbedtls |
| **Linux** | native sockets | mbedtls |
| **Windows** | native sockets | mbedtls |

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
cyberbrowser/
├── cyberbrowser/              # Custom browser emulation library
│   ├── src/
│   │   ├── js_quickjs.cpp     # QuickJS JS engine integration
│   │   ├── html_media_extract.cpp
│   │   ├── browser/           # Split browser API implementations
│   │   ├── http_download.c
│   │   └── platform/          # macOS/Linux/Windows abstractions
│   ├── include/               # Public headers
│   ├── tests/
│   └── third_party/
│       ├── quickjs/           # QuickJS JavaScript engine (MIT)
│       └── mbedtls/           # TLS/SSL library (Apache 2.0)
│
├── build.sh                   # MSVC build wrapper
├── build_macos_app.sh         # macOS build script (legacy)
└── scripts/                   # Helper scripts
```

### Key Design Decisions

**Why QuickJS instead of a headless browser?**  
Headless Chrome or Safari would add 100+ MB of dependencies. QuickJS is a complete ES2020 engine in ~1 MB that can execute YouTube's player code directly with our stubbed DOM APIs.

**Why native MP4 atom injection instead of FFmpeg or taglib?**  
FFmpeg is a massive dependency. Our minimal MP4 parser finds the existing `moov/udta/meta/ilst` hierarchy, inserts `covr` (album art) and `©nam` (title) atoms, patches chunk offsets, and writes the file — all in ~400 lines of C with zero external dependencies.

---

## Tests

```bash
cd cyberbrowser
mkdir -p build && cd build
cmake .. -DBE_BUILD_TESTS=ON
cmake --build . --target browser-emulator-tests -j4
./tests/browser-emulator-tests
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
