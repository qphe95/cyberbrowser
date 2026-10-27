# Process-per-tab sandboxing

Status: proposal. Adds a coordinator process (new, formally verified C) that creates every
other process and every channel between them; sandboxed tab processes (the page engine of
today, `src/cyberbrowser_main.c` and everything it calls); and UI, GPU, network and storage
processes.

## Why

Today cyberbrowser is one process that loads one page, with the full rights of the user who
runs it: a bug in the JS engine, the HTML, CSS or image parsers, or the TLS stack lets a page
read and write any file the user can, open any network connection, and read every other site's
cookies and storage held in the same process. Browsers are attacked through exactly these
components, so the design assumes that **a tab process can be fully compromised** and makes
that compromise worth as little as possible:

- A tab process reads and writes only its own site's data, through a storage service that
  knows which site it is serving from the coordinator, not from the tab.
- It cannot see other tabs' memory, files, network traffic or windows.
- Every channel between processes is created by the coordinator, which is small, parses no
  web content, and is proved to connect processes only as the policy allows. Once a channel
  exists, calls on it go directly between the two processes, without the coordinator.

## Goals

1. Every tab runs in its own sandboxed process, locked to one site.
2. Each site's persistent data lives in `browser_data/sites/<site>/` next to the browser `.exe`,
   and a tab process can reach only its own site's folder.
3. Tabs can be dragged out into a new window and between windows without reloading.
4. Inter-process calls cost about as little as the OS allows: no third process on the path of
   a call, no system call while a channel is busy.
5. Machine-checked proofs, in C, of the parts that enforce isolation: the coordinator, the
   channel library, the storage service, and the GPU process's command decoder.
6. Pages are rasterized on the GPU, in a GPU process; the tab processes never touch the GPU.
   WebGL and WebGPU run in a separate GPU process per site that uses them (section 5.7).
7. The single-process mode stays, for the replay benchmark, tests and debugging.

## Non-goals

- Formally verifying the tab, UI, network or GPU processes as a whole. They are large and
  assumed compromisable (the tab) or kept away from web content's complex formats (the
  others); the sandbox and the verified pieces contain them.
- Timing and microarchitectural side channels between processes (cache timing, Spectre
  across processes). Process isolation is the standard mitigation for Spectre within a
  process; covert channels between processes are out of scope.
- Proving the WebGL and WebGPU command validators and shader translators. They are contained
  instead (section 5.7).
- macOS, Linux and Android sandboxes. The architecture carries over (seccomp-bpf and
  namespaces, the macOS Seatbelt), but this document specifies Windows only.
- Out-of-process iframes in the first phases (section 9).

## 1. What we start from

- **One page per process.** `main()` fetches the start URL, runs the page until it settles and
  writes a screenshot and the final HTML. There is no interactive window, no tab or window
  model, and no input events from a user. Painting is CPU rasterization of the display list
  (`render_display_list_to_jpg`); `src/vulkan_renderer.c` exists but the page path doesn't use it.
- **Globals everywhere.** The engine keeps per-page state in globals (`g_players`,
  `g_cookie_jar`, `g_visitor_data`, the DOM store). A process per tab suits this: each
  process has exactly one page, so the globals need no rework.
- **Nothing persists.** `localStorage` (`src/browser/storage_api.c`) and the cookie jar
  (`src/http_download.c`) live in memory and are lost at exit. Per-site storage on disk is
  new work, and it can be designed for the sandbox from the start.
- **Imports.** `cyberbrowser.exe` (26 MB) imports `KERNEL32`, `bcrypt`, `CRYPT32`, `msvcrt`,
  `libwinpthread-1`, `SHELL32`, `WINMM` and `WS2_32`. `SHELL32` and `WINMM` pull in `user32`, which
  connects the process to win32k at startup; the tab executable must drop them (section 6.3).

## 2. Processes

```
                     +------------------------------------+
                     |  coordinator (verified)            |
                     |  processes, labels, channel grants |
                     +------------------------------------+
                        creates every process and grants every channel;
                        never on the path of a call
                                       :
   +----------+  input, tab state  +-----------+  display lists, textures  +-----------+
   | UI       |<==================>| tab       |==========================>| GPU       |
   | windows, |                    | site A    |                           | raster +  |
   | tab strip|                    | (engine)  |==fetch==> +---------+     | compositor|
   +----------+                    +-----------+           | network |     +-----------+
        |  browser UI display lists      ||                +---------+          ^
        +--------------------------------||-----------------------------------+
                                         || files
                                   +-----------+
                                   | storage   |  browser_data/sites/
                                   | (verified)|
                                   +-----------+
```

| Process | Executable | Trust | Rights |
|---|---|---|---|
| Coordinator | `cyberbrowser.exe` | Trusted, verified | The user's rights. Starts every process; grants every channel. |
| UI | `cb_ui.exe` | Untrusted, handles no web content | Owns the windows and input. No GPU, files or network. |
| GPU | `cb_gpu.exe` | Untrusted, parses only the verified command format | The GPU, and drawing into the UI's windows. No files or network. |
| Web-GPU, one per site using WebGL or WebGPU | `cb_webgpu.exe` | Untrusted, runs page shaders | Its own GPU device. No files, network or windows. |
| Tab | `cb_tab.exe` | Untrusted, assumed compromised | Nothing ambient: no files, network, GPU, win32k or child processes. |
| Network | `cb_net.exe` | Untrusted (parses TLS and HTTP from the network) | Outbound network only. |
| Storage | `cb_store.exe` | Trusted, verified core | Read and write under `browser_data/sites/`, nothing else. |

Separate executables, not one `.exe` with roles, because what an executable imports decides what
gets mapped into it at startup: the tab must not load `user32` (win32k), only the network
process needs `WS2_32`, only the GPU process needs the graphics drivers, and the verified
processes should link as little as possible. Windows shares each executable's code pages
between all processes running it (section 10), so twenty tabs cost one copy of
`cb_tab.exe`'s code.

### 2.1 Coordinator: processes, labels, channels

The coordinator does three things, and nothing else:

1. **Starts processes** with their sandboxes (section 6), and ends them.
2. **Labels them.** Every process gets a label when it starts, which never changes: `site(s)` for
   a tab locked to site `s` (or `spare`, until it is locked, once), `webgpu(s)` for site `s`'s
   web-GPU process, `ui`, `gpu`, `net`, `store`.
3. **Grants channels.** A process asks for a channel of some interface to some peer; the
   coordinator checks the request against the policy table (section 4.3), creates the
   channel, gives each end its handles, and tells the serving end the **badge** of the client:
   its label and id, as the coordinator knows them. A server never takes the client's word
   for who it is.

It never relays calls, opens files, parses URLs beyond a strict host grammar, or holds tab and
window state. It is a single-threaded event loop over control messages, built as a pure state
machine, `step(state, process, message) -> (state', actions)`, and a thin shim that performs
the actions with Win32 calls (section 8).

### 2.2 UI process

Owns every top-level window, the tab strip, the address bar, keyboard and mouse input, and the
**tab table**: which tabs exist, which window each is in, its position in the strip, which is
active, and each tab's history (URLs and titles). It draws the browser's own interface by
sending display lists to the GPU process, like a tab does, and tells the GPU process which
tab's content goes in which window.

The UI handles no web content. Titles and URLs arrive as text, and favicons arrive as pixels
the tab already decoded. A compromised UI process could show a tab in the wrong window or route
keystrokes to the wrong tab, but nothing on the web can reach it except through those
three narrow inputs.

### 2.3 Tab processes

One per tab, running today's engine: HTML, CSS, layout, JS, image and font decoding, media,
and building the display list. Each is locked to one **site** for its whole life (section 3.1);
navigating to another site swaps in a new process (section 7.3). A tab process:

- sends its display lists, and the images and glyphs they draw, to the GPU process (section 5);
- receives input, resize, visibility and focus events from the UI;
- fetches through its channel to the network process;
- reads and writes its storage through its channel to the storage service.

### 2.4 Network process

Does DNS, TCP, TLS (mbedtls, `src/tls_client.c`) and HTTP (`src/http_download.c`) for all
tabs. It is separate from the tabs so that a tab process needs no network capability, and
so that a compromised tab cannot open arbitrary connections (to `localhost`, the intranet, or
other sites with credentials). Per request, it enforces:

- **Who is asking:** the badge of the tab's channel, from the coordinator.
- **Navigations match the badge:** a tab's top-level document request must be for a URL
  of the badge's site, and a redirect to another site isn't followed; it goes back to the
  tab as "navigate elsewhere", which goes to the UI and leads to a new process (section 7.3).
  Without this check, a compromised tab could get a process labeled with a victim's site
  and load the attacker's page into it.
- **Cookies:** the jar is partitioned by top-level site (as in Firefox's Total Cookie
  Protection and Safari): requests on a channel carry only cookies of the badge's
  partition. The network process holds the jars in memory and saves each partition
  through its own channel to the storage service, badged `net` and allowed to name the
  partition's site (section 4.3). Third-party cookies get their own partition under the
  top-level site (CHIPS-style).
- **Reading cross-site responses:** Opaque Response Blocking (ORB, Chrome's successor to
  CORB) drops cross-site `no-cors` responses that aren't images, media, scripts or styles,
  so a compromised tab can't load another site's HTML or JSON into its memory.
- **Private Network Access:** public sites can't reach `localhost` or private addresses.

The network process sees every site's traffic in plaintext, so a compromise of it leaks across
sites; Chrome accepts the same risk for its network service. It has no file access, so what it
can leak is what is in flight and in the in-memory jars. A per-site network process is
possible later, at a memory cost.

### 2.5 Storage service

Owns `browser_data/sites/`. A tab asks it to open, read, write, list, rename or delete a file
by a path relative to its site's folder; the site comes from the channel's badge, never from
the message. Its core (path grammar, containment, quotas) is verified (section 3.3, P4).
It runs with a token that can reach `browser_data/sites/` and nothing else on disk, so even a
bug outside its verified core can't touch the user's files.

It is one process for all sites, which means a compromise of it would expose every site's
data. It's small, it parses only paths, and its core is proved, which is why that's
acceptable. A process per site is possible later if it isn't.

### 2.6 GPU process

Owns the GPU device. It rasterizes the display lists that tabs and the UI send, keeps each
client's images and glyphs in GPU textures, and composites tab content and the browser's
interface into the UI's windows. It is described in section 5.

## 3. browser_data and per-site storage

### 3.1 Sites

A tab's site is the scheme and **registrable domain** of its top-level document's URL:
`https://www.youtube.com/watch?v=…` → `https://youtube.com`. The registrable domain is eTLD+1
from the Public Suffix List (shipped with the browser, about 230 KB; `a.github.io` and
`b.github.io` are different sites because `github.io` is a public suffix). Ports are
ignored, as in the web's definition of a site. Special cases:

| URL | Site |
|---|---|
| IP address, `localhost` | The host itself |
| `file:` | One `file` site with no storage; the UI asks the storage service for a read handle to each file the user opened |
| `data:`, `about:blank` from nowhere, sandboxed iframes | Opaque: storage in memory only, nothing on disk |

The function from a host to a site, `site_of(scheme, host)`, is a small verified library
(the host grammar of section 4.2 and a lookup in the Public Suffix List compiled to a
table), used by the coordinator to label processes and by the network process to check
navigations.

### 3.2 Layout

```
<exe folder>/
  cyberbrowser.exe  cb_ui.exe  cb_gpu.exe  cb_webgpu.exe  cb_tab.exe  cb_net.exe  cb_store.exe
  browser_data/                 DACL: the user, SYSTEM and the storage service; inheritance off
    ui/                         tabs and windows to restore, settings, permissions
    sites/
      https+youtube.com/
        cookies                 this top-level site's partition (written by the network process)
        local_storage
        indexeddb/
        cache/
        partitions/
          https+googlevideo.com/   a third party's storage under this top-level site
      https+bbc.co.uk/
      ...
```

The folder name is `<scheme>+<registrable domain>`, lowercased, IDNs in punycode, so only
`[a-z0-9.+-]` appear. Anything else, or a name over 100 characters, becomes `h+<sha256 hex>`.
Since every name contains a `+`, no Windows device name (`CON`, `NUL`, `COM1`, `AUX`) and no
`.` or `..` can occur.

If the `.exe` is installed somewhere the user can't write (`Program Files`), `browser_data/`
falls back to `%LOCALAPPDATA%\cyberbrowser\browser_data`; the portable layout (next to the
`.exe`) is the default.

### 3.3 How the storage service keeps a tab in its folder

A tab process has **no file-system rights at all**. It sends `OPEN(rel_path, mode)` over its
storage channel, and the storage service:

1. checks `rel_path` against a strict grammar: 1–8 components of `[a-z0-9_.-]{1,64}`, none of
   them `.` or `..`, none starting with `.`, no `\`, `/` used only as the separator, no `:`;
2. joins it to `browser_data/sites/<folder(badge site)>/`;
3. opens it with `CreateFileW(..., FILE_FLAG_OPEN_REPARSE_POINT)`, refuses reparse points
   and hard-linked files (`nNumberOfLinks > 1`), and checks with `GetFinalPathNameByHandleW`
   that the opened file really is inside that folder;
4. for reading, duplicates a read-only handle to that file into the tab process; for writing,
   keeps the handle itself and returns a file id.

The tab gets at most a read handle to one file, never a directory handle or a write handle,
so there's no relative path to escape through and no way around the quota. It reads directly
through its handle; it writes with `WRITE(file id, offset, length)` calls whose data is in the
channel's arena, so a write costs one call and no copy through the ring. Creating, listing,
renaming and deleting go through the service the same way. Steps 1, 2, the containment check
in 3 and the quota are the verified core; the Win32 calls are its shim.

This works on every file system, including FAT32 and exFAT USB drives with no ACLs, which
matters for a browser that lives next to its `.exe`.

Quotas: the service counts bytes per site and refuses writes past the site's quota (the
`navigator.storage.estimate()` figure), so one site can't fill the disk. Since every write
goes through the service, it sees every byte that could grow a file.

## 4. IPC

### 4.1 Control plane and data plane

The cost of a call between processes is mostly context switches and system calls. A design
where every call goes through the coordinator pays twice: client → coordinator → server is two
hops each way, four switches per round trip, and the coordinator becomes a bottleneck every
process waits on.

Microkernels that care about IPC cost don't do that. seL4's fast path and the IPC of HarmonyOS's
HongMeng kernel (OSDI 2024) check a capability once and then transfer a call directly from
the caller to the callee, with the kernel switching straight from one to the other. We can't
change how Windows schedules threads, but the same split works in user mode:

- **Control plane, the coordinator:** decides who may talk to whom, creates the channel,
  hands out its handles, and tells the server the client's badge. This happens once per
  channel (when a tab starts, opens a popup, or connects to a service), not once per call.
- **Data plane, the channel:** two processes share a memory section and two events. Calls
  and replies go into rings in the shared memory, and the events are signaled only when the
  other side is asleep. A busy channel moves messages with no system call at all.

So the coordinator is verified and in charge of every connection, yet it's never on the
path of a fetch, a frame or a storage read.

### 4.2 Channels

A channel is one shared section with:

- two single-producer, single-consumer rings of fixed-size slots (256 bytes, cache-line aligned),
  one per direction, each with a head and a tail index in their own cache lines;
- an arena for larger payloads, referenced from slots by offset and length (response bodies,
  display lists, textures go in separate sections granted the same way, section 5.2);
- a `waiting` flag per side and one auto-reset event per side.

Sending: write the slot, publish the tail (release), and only if the receiver's `waiting`
flag is set, `SetEvent`. Receiving: read slots until the ring is empty, spin briefly, then set
`waiting`, re-check the ring (no lost wakeup), and `WaitForSingleObject`. Calls are
request/reply pairs matched by a request id; a process can have many calls in flight on one
channel.

The peer is untrusted, and it can rewrite the shared memory at any moment, including while
we read it. The **channel library** (`libchan`), used by every process, handles that:

- every index and length is read **once** into a local, then checked against the ring and
  arena sizes; nothing is read twice from shared memory (no double fetch);
- a message is copied out of the ring into private memory before it is decoded;
- a corrupt index or length closes the channel and reports the peer to the coordinator,
  which ends it.

`libchan` is verified (P2, section 8.3): whatever the peer writes, it never reads or writes
outside the section, and the bytes it returns are a copy that can't change afterwards.

Expected costs, to be measured in M1: a blocking round trip on an idle channel is two event
signals and two context switches, a few microseconds on Windows; on a busy channel a message
costs tens to hundreds of nanoseconds, with no system call. Bulk data (a frame, a response
body) is not copied through the ring: the sender writes it into its section and sends an
offset and length.

### 4.3 Interfaces and the policy

Every channel has an interface, and the coordinator grants it only for these pairs:

| Interface | Client → server | Badge the server gets | Granted when |
|---|---|---|---|
| `fetch` | `site(s)` → `net` | `site(s)`, tab id | The tab is locked to `s` |
| `store` | `site(s)` → `store` | `site(s)` | The tab is locked to `s` |
| `store.partition` | `net` → `store` | `net`; may name any site's `cookies` file, nothing else | Once, at start |
| `draw` | `site(s)` or `ui` → `gpu` | tab id or `ui` | The tab starts; the UI starts |
| `input` | `ui` → `site(s)` | `ui` | The tab starts |
| `tab` | `site(s)` → `ui` | tab id, `site(s)` | The tab starts |
| `opener` | `site(s)` ↔ `site(t)` | Each side's label | One tab opens the other (`window.open`, a link with a target): `postMessage` and the `opener` relationship only, as opaque bytes |
| `compose` | `ui` → `gpu` | `ui` | Once, at start |
| `gl` | `site(s)` → `webgpu(s)` | tab id, `site(s)` | The tab first uses WebGL or WebGPU; the coordinator starts `webgpu(s)` if it isn't running |
| `canvas` | `webgpu(s)` → `gpu` | `webgpu(s)` and the tab id it renders for; one shared texture per canvas | With `gl`, for each canvas the tab creates |
| `files.user` | `ui` → `store` | `ui`; read handles to files the user picked, write handles in the downloads folder | Once, at start |

Nothing else. In particular a tab never gets a channel to another tab except through
`opener`, never to `store` under another site's badge, and never to anything with the
coordinator's rights.

## 5. GPU process and GPU rasterization

### 5.1 Why not the GPU in the tab

A process that uses the GPU needs the graphics kernel calls that the tab sandbox blocks
(win32k-routed `D3DKMT*`), and loads the vendor's user-mode driver, a large and complex DLL,
with the kernel-mode driver behind it. GPU driver bugs are a common way to escape a browser
sandbox, which is why Chrome and Firefox keep the GPU in one separate process. A GPU device
per tab would also cost tens of milliseconds to create and tens of MB of driver memory per
process. So the tabs build display lists and the GPU process draws them.

### 5.2 What a tab sends

The rule: **the GPU process never parses a complex format.** No HTML, CSS, images, fonts or
SVG. Everything that needs parsing is done in the tab, inside its sandbox, and what crosses is:

- **Display lists**, in a fixed-layout binary form of today's `DisplayList` (`src/display_list.h`):
  a sequence of records (fill rect, rounded rect, border, image, glyph run, clip push and pop,
  transform, opacity layer, mask), each with a type, a fixed size per type, numbers, and ids of
  resources. It goes in a per-tab command section, written by the tab, read by the GPU process.
- **Images** as decoded pixels: the tab decodes PNG, JPEG, WebP and video frames
  (`src/image_cache.c`, `src/media/`) and writes the pixels into a transfer section, then sends
  `UPLOAD(image id, w, h, format, offset)`. The GPU process copies them into a texture.
- **Glyphs** as rasterized masks: the tab shapes text and rasterizes glyphs (`src/text_shaper.c`)
  as it does today, and uploads each new glyph to the GPU process's atlas for that tab. Web
  fonts are parsed only in the tab.

Glyph runs and images in a display list refer to resources by id, in the client's own
namespace: id 12 from tab 3 and id 12 from tab 4 are different textures, and the decoder
looks them up only in the sending client's table.

### 5.3 The decoder and the rasterizer

The GPU process copies each display list out of the command section (`libchan`'s rule: read
once, then decode private memory), and decodes it with a verified decoder (P5, section 8.3):
every record is in bounds, every number within range (no NaN, sizes and coordinates clamped),
every resource id resolves in the sender's table or the record is dropped, clip and layer
nesting balanced and bounded. What comes out is a typed array of draw operations that the
rasterizer, built on `src/vulkan_renderer.c`, turns into draw calls into the tab's tiles.

Tiles are per tab: each tab's content is rendered into its own textures (256×256 tiles, plus
layers for what scrolls, animates or has opacity), and only compositing combines textures from
different clients into a window. No client can read a texture back: there is no readback to
tabs, so `canvas.getImageData()` and similar stay CPU-rendered in the tab (section 5.6).

### 5.4 Compositing and the windows

The UI tells the GPU process what each window shows: its own interface (from its own display
lists) and which tab's content goes in the content area. The GPU process creates a swap chain
for each of the UI's windows (window handles are valid across processes on one desktop),
composites, and presents. Input never goes to the GPU process; it goes from the UI to the
tab.

Dragging a tab to another window (section 7.4) is the UI saying "window 2 now shows tab 7".
The tab's tiles are already textures in the GPU process; nothing is re-rendered.

Scrolling can then be done by the compositor: the tab sends its scroll layers with their
content larger than the viewport, the UI sends scroll input to both the tab and the GPU
process, and the GPU process moves the layer at once while the tab catches up. The page
scrolls smoothly even while its JS is busy.

### 5.5 When there is no GPU, or the GPU process crashes

The CPU rasterizer stays. Without a usable GPU, the GPU process rasterizes display lists on the
CPU with the same decoder, so the security story doesn't change. If the GPU process crashes,
the coordinator starts a new one, grants the `draw` channels again, and every client sends a
full display list and its resources again; tabs keep running.

### 5.6 What stays in the tab

`<canvas>` 2D (it needs `getImageData`, which reads pixels back) is drawn on the CPU in the tab
and uploaded as an image every frame it changes. Video frames are decoded in the tab (on its
decode thread, `src/media/media_buffer.c`) and uploaded as images; zero-copy video decode on
the GPU is a later optimization that would need a hardware decoder in the GPU process.

### 5.7 WebGL and WebGPU

WebGL and WebGPU hand the page the GPU itself: buffers, textures, shaders and draw calls,
with readback. Running them safely needs a command validator (are the buffers big enough,
the formats compatible, the state legal?) and a shader translator and validator (GLSL ES and
WGSL in, the driver's shader language out, with bounds checks inserted). In Chrome these are
the command buffer decoder, ANGLE, Dawn and Tint: hundreds of thousands of lines, and a
steady source of sandbox escapes. That's far too big to prove now, so the design doesn't
rely on it being correct. Three measures limit what a bug in it can do:

**1. Its own process, per site, apart from the compositor.** WebGL and WebGPU run in a
**web-GPU process**, `cb_webgpu.exe`, labeled `webgpu(s)`, one per site that uses them, started
on the first `getContext("webgl"/"webgl2")` or `requestAdapter()`. Each has its own GPU device.
The compositor's GPU process (sections 5.1–5.6) never runs page-supplied shaders or commands.
So a page that breaks the validator ends up in a process holding only its own site's GPU
work: no other site's textures, no browser interface, no display lists from other tabs.
Memory of one GPU device isn't visible to another process's device without a driver bug
(WDDM gives each process its own GPU virtual address space). Most pages never use WebGL or
WebGPU, so the extra device and process are paid only where needed.

**2. Memory-safe, widely tested implementations, unmodified.** We don't write the validator:

| API | Implementation | Language | Notes |
|---|---|---|---|
| WebGPU | `wgpu-native` (wgpu and its shader translator Naga, used by Firefox) | Rust | Exposes the standard `webgpu.h` C API; Rust rules out most memory-safety bugs in the validator itself |
| WebGL 1 and 2 | ANGLE (used by Chrome, Firefox and Safari) | C++ | EGL and GLES C API; its shader translator validates GLSL ES and adds bounds checks; on Windows it runs on D3D11 or Vulkan |

**Prebuilt binaries, not built here.** Both libraries expose C APIs (EGL and GLES for ANGLE,
`webgpu.h` for wgpu), so DLLs built with MSVC link and run fine from our MinGW build, and
neither needs its own tool chain (GN and depot_tools for ANGLE, Rust for wgpu) in ours:

| Library | Files | Source |
|---|---|---|
| `wgpu-native` | `wgpu_native.dll`, `webgpu.h`, `wgpu.h` | The project's GitHub releases, Windows x86-64 build |
| ANGLE | `libEGL.dll`, `libGLESv2.dll`, the GLES and EGL headers | ANGLE has no official binary releases; the DLLs are taken from an official Chromium build of a stable release (Chrome ships them), with the headers from ANGLE at the same revision |

- Each file is pinned in the repository by version and SHA-256 (`third_party/prebuilt/manifest`),
  and a script fetches and checks them; a mismatch fails the build. The binaries aren't
  committed.
- Updates follow upstream security releases: ANGLE with Chromium's stable channel, wgpu-native
  with its releases. A stale ANGLE is a known-exploitable ANGLE.
- They are loaded only by `cb_webgpu.exe`, from the browser's own folder by full path, never
  through the DLL search path.
- `d3dcompiler_47.dll`, which ANGLE's D3D11 back end uses, comes with Windows 10 and 11.

The tab implements the JS API objects and sends calls over its `gl` channel as fixed-layout
records; the web-GPU process decodes them (the same discipline as `libchan`: read once,
check sizes and ids, then call) and calls the library. Calls are batched in the ring and
flushed at synchronization points (`getError`, `readPixels`, `mapAsync`, end of frame), so a
WebGL draw call costs a ring write, not a round trip.

**3. Defense in depth around it.**

- Its sandbox: an AppContainer with no capabilities, no files, no network, no child
  processes; win32k is on (the driver needs it), as in the compositor's GPU process.
- Robust buffer access (Vulkan `robustBufferAccess2`, D3D11's bounds behavior) on every
  device, so an out-of-bounds shader access the translator missed reads zero instead of
  another allocation.
- Every resource zero-initialized before first use (both specs require it), so no
  uninitialized GPU memory reaches the page.
- Timer queries off, or coarsened to 100 µs, against GPU timing attacks.
- Continuous fuzzing of the decoder and the library entry points, with the WebGL and WebGPU
  conformance test suites (CTS) as the seed corpus.

**What reaches the compositor.** A web-GPU process renders each canvas into a texture it
shares with the compositor's GPU process (a shared D3D11 texture or a Vulkan external-memory
image, created by the compositor and granted to the web-GPU process for one canvas). The tab's
display list refers to that canvas by id, and the verified decoder (P5) resolves it only in the
tab's own namespace, so one tab can't put another tab's canvas on screen. The compositor only
samples these textures; it runs nothing from the web-GPU process.

**The driver is the real exposure.** A correct validator only guarantees that the commands
and shaders reaching the driver are *valid*. Driver bugs are triggered by valid input: the
driver compiles every shader with its own large compiler, and manages memory for every
buffer and texture. Its user-mode half runs inside the web-GPU process, contained by the
sandbox, but its kernel-mode half is reachable through the graphics kernel calls, and GPU
kernel drivers are among the most exploited ways out of a browser sandbox.

**Decision: WebGL and WebGPU run on the hardware GPU for every site**, through ANGLE (on D3D11)
and wgpu (on Vulkan or D3D12), as in Chrome and Firefox. A software implementation (SwiftShader,
a CPU Vulkan) would keep page shaders away from the driver entirely, but at a large cost in
speed for 3D content; we accept the driver exposure instead, and limit it:

- **A block list**, as in Chrome: on GPUs, drivers and driver versions with known exploitable
  bugs, and drivers older than a minimum date, WebGL and WebGPU are turned off (there is no
  software fallback).
- **Fewer features** on drivers with a poor record: no compute shaders, no storage textures or
  uncommon extensions, as a per-driver list.
- Everything above: one process per site, the validators, robust buffer access, zeroed
  resources, coarse timers, fuzzing.

**The compositor's GPU process is a different case.** It also talks to the hardware driver,
but with commands and shaders the browser wrote: a fixed set of shaders, compiled once, and
draw calls built by our rasterizer. The page controls only data (pixels, coordinates, colors,
already checked by the verified decoder), not code. That's a much smaller driver surface than
running the page's own shaders, and the same trade-off every browser makes for composition.

**What remains.** Any site can reach the driver through the validators, and a driver bug can
lead out of the sandbox. That is the same exposure Chrome and Firefox have. What the design
adds is that the process it starts from holds one site's GPU work and nothing else, and that
the tab sandbox stays fully locked down (no win32k, no driver), so getting there takes a bug in
the tab first. If this proves not to be enough, software mode (SwiftShader by default,
hardware as a per-site permission) can be added without changing the rest of the design.

**Later verification, piece by piece.** The parts small enough to prove come first: the
decoder of `gl` records (sizes, ids, enum ranges), and the resource tables that map a tab's ids
to library objects. These are similar to the GPU decoder and `libchan`. The validator's rules
and the shader translators stay tested, fuzzed and sandboxed rather than proved.

## 6. The tab sandbox (Windows)

Every tab process starts with:

### 6.1 A token with no rights

- **Less Privileged AppContainer (LPAC)**, a fresh SID per process. LPAC, unlike a plain
  AppContainer, doesn't get the default read access that `ALL APPLICATION PACKAGES` has to
  much of the system. Only `ALL RESTRICTED APPLICATION PACKAGES` gets it, which covers the
  system DLLs and fonts.
- **No capabilities.** In particular, no `internetClient`, so no sockets.
- A unique SID per process, so two tab processes can't open each other's processes,
  threads, sections or events even if both run as the same user. Each process, thread and
  channel section is also created with an explicit security descriptor that grants access to
  the coordinator only; the ends receive duplicated handles. Whether `CreateProcess` with
  `SECURITY_CAPABILITIES` accepts a derived SID with no registered AppContainer profile
  (one per tab would mean a registry entry per tab) is the first thing to check in M3.
  Chromium's sandbox creates "lowbox" tokens for this.

### 6.2 A job object

`JOB_OBJECT_LIMIT_ACTIVE_PROCESS = 1` (no child processes), a memory limit, kill on
coordinator exit (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), and UI restrictions (no clipboard,
no global atoms, no `SetWindowsHookEx`, no changing display settings).

### 6.3 Process mitigation policies (set at creation)

| Policy | Effect |
|---|---|
| Win32k system calls disabled | Removes the largest kernel attack surface. The tab executable must not import `user32`, `gdi32`, `SHELL32` or `WINMM`, and doesn't need them: it has no windows and no GPU. |
| No child processes | Duplicates the job limit. |
| Image load: no remote images, no low-label images, prefer `System32` | DLL planting. |
| Dynamic code prohibited (ACG) | No new executable memory, so shellcode can't be written and run. Compatible today because T1 and T2 are off; when the JIT returns, it runs in a separate JIT process (as in Edge) or this policy is dropped for that tab (section 12). |
| Strict handle checks, extension points disabled, CFG where the toolchain supports it | Hardening. |

### 6.4 The other processes' sandboxes

| Process | Token | Notable rights |
|---|---|---|
| GPU | AppContainer, no capabilities | win32k and the graphics driver (it can't do without them); job with no child processes; ACG off (drivers generate code) |
| Web-GPU | LPAC, no capabilities, a fresh SID per process | win32k and the driver, as for GPU; memory limit; no windows (it renders only into shared textures) |
| Network | LPAC with `internetClient` | Sockets; win32k disabled |
| UI | AppContainer | win32k for windows and input; no network or files except through `store` |
| Storage | Restricted token, normal integrity | Only `browser_data/sites/`; win32k disabled |

## 7. Tabs, windows and navigation

### 7.1 Painting

A tab lays out, builds its display list, uploads new images and glyphs, and writes the display
list into its command section, then sends `FRAME(n)` on its `draw` channel. The GPU process
rasterizes the frame into the tab's tiles and composites at the next vertical blank. The tab
can build frame n+1 while the GPU process draws frame n.

### 7.2 Opening a tab

The coordinator keeps one **spare** tab process started, sandboxed and initialized (runtime
created, built-ins and the UA stylesheet loaded), labeled `spare`. When the UI opens a tab
for some host, it asks the coordinator to lock a process to that host's site. The coordinator
computes the site with `site_of`, relabels the spare (once), grants its `fetch`, `store`,
`draw`, `input` and `tab` channels, and starts another spare. Windows has no `fork`, so the
spare process does the job Chrome's zygote does on Linux.

### 7.3 Navigating

Navigating within the same site stays in the same process. A navigation to another site (a
link, a form, `location = …`, a cross-site redirect) goes from the tab to the UI as
`NAVIGATE(url, method, body)`. The UI asks the coordinator for a process locked to the
destination's site, hands it the request, and when its first frame is drawn makes it the tab's
process in the tab table and tells the GPU process to show it; the old process is ended. The
network process checks that the new process's document request matches its badge (section 2.4),
so the label always matches the content in the process. Back and forward use the history in
the UI's tab table.

### 7.4 Dragging a tab to another window

The tab process isn't involved, since tabs don't own windows, and neither is the coordinator:

1. The user drags tab 7 out of window 1. The UI creates window 2, moves tab 7 to it in its
   tab table, and makes the next tab in window 1 active.
2. It tells the GPU process that window 2 shows tab 7's content (textures it already has).
3. It sends tab 7 a `RESIZE` if the new window's viewport differs, and focus events, and
   from now on routes window 2's input to tab 7.

Dropping onto another window is the same with an existing window. The page keeps running
throughout: no reload, its JS state, media and network requests continue.

## 8. Verification

### 8.1 What is verified

| Component | Size (target) | What is proved |
|---|---|---|
| Coordinator core | 2,000–3,000 lines | P1, P3, P6, P7 |
| `libchan`, the channel library | ~1,000 lines | P1, P2 |
| `site_of` and the host grammar | ~500 lines plus the PSL table | P1, and that it computes eTLD+1 as specified |
| Storage service core | ~1,500 lines | P1, P4 |
| GPU command decoder | ~1,500 lines | P1, P5 |

Each verified component has the same shape as the coordinator:

```
  Win32 shim (unverified, small)                verified core (Frama-C/WP)
  ------------------------------                --------------------------
  wait on channels and events         --msg-->  step(state, from, msg) -> actions
  perform actions: CreateProcess,     <-acts--  pure: no I/O, no globals except state,
  DuplicateHandle, CreateFileW, ...             bounded loops, no recursion,
                                                no dynamic allocation (fixed tables)
```

The shims translate actions to Win32 calls and results back to messages, are small enough to
review line by line (under 1,000 lines each), and are described to the prover by ACSL
contracts, which are assumptions.

### 8.2 Control messages

Every control message (to the coordinator, the storage service, and the GPU decoder's command
records) has a fixed header (`type`, `length`, `request id`) and a body whose layout is fixed
per type: integers, ids, and at most a few length-prefixed byte strings (a host, a relative
path), each with a type-specific maximum. The decoders are in the verified cores, so their
memory safety and their acceptance of only well-formed messages are proved.

### 8.3 Properties

Labels, badges and the policy are those of sections 2.1 and 4.3. Proved with Frama-C's WP
plugin, with Alt-Ergo, Z3 and CVC5 as provers:

- **P1. No undefined behavior** in any verified component, for any input: no out-of-bounds
  access, overflow, uninitialized read or invalid pointer (WP plus the RTE plugin).
- **P2. Channels are safe against a hostile peer.** Whatever the peer writes to the shared
  section, and whenever, `libchan` accesses only the section's bounds, reads each shared
  index and length once, and returns a private copy. Shared memory is modeled as returning an
  arbitrary value on every read.
- **P3. The channel graph follows the policy.** After every step of the coordinator: every
  channel's interface and end labels are a row of the policy table, every badge given to a
  server is the client's true label, a process's label never changes after it is set (except
  `spare` → `site(s)`, once), and every handle the coordinator has given a process is one of
  its own channels, sections or events.
- **P4. File containment.** Every file the storage core opens for a client badged `site(s)`
  is `browser_data/sites/<folder(s)>/` followed by a path accepted by the grammar of section
  3.3, and `folder` is injective, so two sites never share a folder. A client badged `net` gets
  only `cookies` files. The quota is never exceeded.
- **P5. The GPU decoder stays in its client's namespace.** Every resource id in a decoded
  operation refers to a resource uploaded by the client that sent the display list, every
  coordinate and size is finite and within bounds, and nesting is balanced.
- **P6. Noninterference in the coordinator.** For any two runs that differ only in what
  processes labeled `site(s)` sent the coordinator, everything it sends to a process labeled
  `site(t)` (`t ≠ s`) is the same, except where the policy allows a flow (granting an
  `opener` channel when `s` opens `t`). Proved by the standard unwinding conditions (output
  consistency, step consistency, local respect): per-step lemmas that together imply
  noninterference for every sequence of messages. WP proves properties of one run, so the
  two-run lemmas are proved on a self-composition harness: a function that runs `step` on two
  states equal in what `t` can observe and asserts the results are equal too.
- **P7. Bounded steps.** Every step of every verified core terminates within a bounded number
  of operations (WP loop variants), so a hostile client can't hang them.

**How these add up to isolation between sites.** Data from site `s` sits in `s`'s tab process,
`s`'s folder, `s`'s channels, the network process, the GPU process's textures for `s`'s tabs,
`s`'s web-GPU process (which `t` has no channel to), and the UI's text and favicons. A tab of site `t` can reach only its own channels (P3), whose
servers act on its true badge (P3); it can open only `t`'s files (P4); it can draw only with
its own resources (P5); no channel connects it to `s`'s tab except `opener` (P3); and the
coordinator doesn't let anything `s` says change what `t` sees (P6). The network process (not
verified) and the UI (not verified, but it handles no web content) complete the argument and
are trusted for it.

The protocol between processes (navigation swaps, a tab closing while its calls are in flight,
a process restarting while channels to it are granted) is modeled in TLA+ and model-checked
with TLC for small numbers of tabs, windows and channels. That checks the design; the C proofs
check the code.

### 8.4 What is trusted (not proved)

The Windows kernel and its AppContainer, job and mitigation enforcement; the shims and their
ACSL contracts; the network process; the UI process; the GPU process outside its decoder,
and the graphics driver; the web-GPU processes (ANGLE, wgpu, Naga), trusted only to the
extent that a compromise of one exposes its own site's GPU work and the driver; the C compiler (GCC/MinGW; CompCert, a verified compiler, if its
targets cover Windows x64 when we get there); the hardware; and the Frama-C tool chain and
provers. Leaks through timing are not covered (see "Non-goals").

### 8.5 Tooling

Frama-C runs on Linux and macOS; on this machine it would run in WSL or a container. Getting
it needs downloads (opam and Frama-C with its provers, a few hundred MB; the TLA+ tools,
about 30 MB), to be approved before M4 starts. The proofs run in CI, and an unproved goal
fails the build.

## 9. Iframes from other sites

In the first phases a cross-site iframe runs in its embedder's process, as in browsers before
site isolation. Its storage and cookies go to `partitions/<its site>/` under the embedder's
folder, so the embedder's process sees only the iframe's data partitioned under itself. It
never sees the iframe site's own (top-level) data. That keeps the containment property
about folders exact, but a compromised embedder can read what the iframe has in memory.
Out-of-process iframes (a tab made of several processes, composited together by the GPU
process, with input routed by hit testing in the UI) come after M7.

## 10. Memory

**Code is already shared.** Windows maps an executable's image section once and shares its
pages between every process that runs it. Read-only code and data pages are the same
physical memory, and written pages (the data section) are copied on write. ASLR picks one
base per image per boot, so relocations don't break the sharing. Twenty tabs of
`cb_tab.exe` share one copy of its code, and nothing needs building for it.

What isn't shared is each process's heap: the JS runtime and its built-ins, the UA style
sheet, the CSS atom table (24 MB reserved up front, section 5.8 of JS_PERF_DESIGN.md), the
4 MB JS stack and the worker threads' stacks. With GPU rasterization, a tab also no longer
needs a full-window pixel buffer; its tiles live in GPU memory. To measure in M1: the private
memory of an idle tab process (about:blank). Things that would reduce it, in order:

1. Reserve the CSS atom table and the GC regions lazily instead of up front, and cap compile
   workers per tab process (today they assume one page owns the machine).
2. A read-only startup snapshot (built-in atoms, shapes, the parsed UA style sheet) in a
   file mapping shared by all tab processes, copy-on-write.
3. Discard background tabs' tiles and caches under memory pressure.

## 11. Milestones

| | Milestone | Done when |
|---|---|---|
| M0 | Split `main()`: the page engine as a library, the current harness on top | `cyberbrowser.exe` single-process behaves exactly as today; replays unchanged |
| M1 | Processes without sandboxing: coordinator, `libchan`, UI with one window, tabs drawing on the CPU into a shared buffer the UI shows | Browse with several tabs in one window; channel costs and idle tab memory measured |
| M2 | Network and storage processes; tabs fetch and store over their channels; `localStorage` and cookies persisted per site | No socket or file code in `cb_tab.exe`; replays work through the network process |
| M3 | Sandboxes: LPAC, job objects, mitigations, no win32k in tabs | The escape tests (below) pass |
| M4 | Proofs: coordinator, `libchan`, `site_of`, storage core (P1–P4, P6, P7); TLA+ model of the protocol | All goals proved in CI |
| M5 | GPU process: display-list format, verified decoder (P5), Vulkan rasterizer, compositing into the UI's windows | Pages render the same as the CPU path (screenshot diff on the replays); CPU fallback works |
| M6 | Multiple windows; dragging tabs out and between windows; compositor scrolling | Drag works while a video plays, without a reload |
| M7 | Cross-site iframe partitioning, ORB, Private Network Access | |
| M8 | Web-GPU process: WebGPU on `wgpu-native`, WebGL 1 and 2 on ANGLE (pinned prebuilt DLLs), on the GPU; shared canvas textures; the driver block list and feature limits; fuzzing | The WebGPU and WebGL conformance suites pass at a level to be set then; a crashing web-GPU process takes down only its canvases; on a blocked driver the APIs are unavailable |
| Later | Out-of-process iframes; JIT process; GPU video decode | |

The single-process mode (`--single-process`, and what the replay harness uses) stays through
every milestone, so the YouTube replay benchmark keeps measuring the engine alone.

**Escape tests.** A test build of `cb_tab.exe` with a hostile mode tries, from inside the
sandbox: to open files outside its folder (other sites' folders, the user's profile,
`browser_data/ui`), with `..`, reparse points, hard links, short (8.3) names, and ADS
(`file:stream`); to open sockets; to start a process; to open other tab processes, their
sections and events; to create windows or a GPU device; to scribble on its channels' shared
memory while the server reads (random indices and lengths, changing them between reads); to
send malformed, oversized and out-of-order messages, display lists with other tabs' resource
ids, and navigations whose URL doesn't match its site. Every attempt must fail, and the
coordinator, storage, network and GPU processes must stay up.

## 12. Risks

- **Per-tab memory.** Process-per-tab costs the most memory of any model (Chrome now
  groups same-site tabs into one process under memory pressure). Measuring idle tab memory
  in M1 decides whether that fallback is needed.
- **Startup latency.** Process creation plus runtime initialization on navigation to a new
  site. The spare process hides it for one navigation at a time.
- **JIT and ACG.** T1 and T2 are off now. Turning them on again in a tab conflicts with
  "dynamic code prohibited"; the options are an out-of-process JIT (Edge's design, complex)
  or dropping ACG for tabs, which makes shellcode easier after a compromise. To decide then.
- **GPU rendering differences.** Anti-aliasing, gradients and text on the GPU won't match the
  CPU rasterizer pixel for pixel. The screenshot comparisons need a tolerance, and the
  accuracy work against Chrome should use one path consistently.
- **WebGL and WebGPU stay the weakest point.** Their validators, shader translators and the
  driver behind them are unproved, and any page reaches the GPU driver through them (section
  5.7). Keep ANGLE, wgpu and the block list current: their fixes are security fixes.
- **Two prebuilt dependencies.** ANGLE and wgpu-native come as pinned prebuilt DLLs (about
  12–25 MB together), so the build needs no C++ or Rust tool chain, but we depend on their
  publishers and must keep the pins current. They are the only code in the browser we
  don't write, and downloading them needs approval first.
- **ANGLE's binaries come from Chromium.** With no official ANGLE releases, a Chromium build
  is the source, and a Chromium version's DLLs must match the headers we compile against. If
  that becomes awkward, building ANGLE once in CI with its own tools is the fallback.
- **Verification effort.** Proving noninterference of real C is research-grade work. Keeping
  each verified core small, pure and table-driven is what makes it tractable; it must stay
  that way as features are added, which means the shims and the unverified processes take the
  complexity.
- **Undocumented corners of the Windows sandbox** (lowbox tokens without profiles, win32k
  lockdown with our runtime libraries, swap chains on another process's windows). Each is
  checked with a small spike before the milestone that needs it.
- **An interactive UI is new.** Windows, a tab strip, an address bar, and turning mouse and
  keyboard input into DOM events, scrolling and focus are needed for M1 and don't exist yet.

## References

- Chromium: Sandbox (Windows), Site Isolation, Out-of-Process iframes, Viz and the GPU
  process, Opaque Response Blocking, Private Network Access.
- Microsoft: AppContainer isolation, Less Privileged AppContainers, `SetProcessMitigationPolicy`,
  Job Objects.
- Firefox: Total Cookie Protection; CHIPS (partitioned cookies).
- ANGLE; wgpu, Naga and `webgpu.h`; SwiftShader (the software option not taken); Chrome's GPU block list; Dawn and Tint (Chrome's WebGPU); the WebGL and WebGPU
  conformance test suites; Vulkan `VK_EXT_robustness2`.
- seL4: the IPC fast path, and its noninterference proof as a precedent at larger scale.
- Chen et al., "HongMeng: A General-purpose OS Kernel" (OSDI 2024), HarmonyOS's microkernel.
- Frama-C WP manual and ACSL reference; J. Rushby, "Noninterference, Transitivity, and Channel
  Control Security Policies" (unwinding); T. Terauchi and A. Aiken, "Secure Information Flow as a
  Safety Problem" (self-composition).
- Lamport, TLA+ and the TLC model checker.
