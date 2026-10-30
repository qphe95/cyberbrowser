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
| Coordinator | `cb_coord.exe` (opened through `cyberbrowser.exe`) | Trusted, verified | The user's rights. Starts every process; grants every channel. |
| UI | `cb_ui.exe` | Untrusted, handles no web content | Owns the windows and input. No GPU, files or network. |
| GPU | `cb_gpu.exe` | Untrusted, parses only the verified command format | The GPU, and drawing into the UI's windows (it shares the UI's AppContainer, section 5.7.1). No files or network. |
| Web-GPU, one per site using WebGL or WebGPU | `cb_webgpu.exe` | Untrusted, runs page shaders | Its own GPU device. No files, network or windows. |
| Tab | `cb_tab.exe` | Untrusted, assumed compromised | Nothing ambient: no files, network, GPU, win32k or child processes. |
| Network | `cb_net.exe` | Untrusted (parses TLS and HTTP from the network) | Outbound network only. |
| Loopback relay | `cb_loop.exe` | Trusted, verified core | TCP connections to `127.0.0.1` and `::1` only, relaying bytes for the network process (section 6.4). |
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
sites; Chrome accepts the same risk for its network service. It can't write files, and it
reaches the cookie jars on disk only through the storage service. It runs in LPAC and can't
read the user's files either: it reaches `localhost` through the loopback relay (section
6.4). A per-site network process is possible later, at a memory cost.

### 2.5 Storage service

Owns `browser_data/sites/`. A tab asks it to open, read, write, list, rename or delete a file
by a path relative to its site's folder; the site comes from the channel's badge, never from
the message. Its core (path grammar, containment, quotas) is verified (section 3.3, P4).
It runs in an LPAC sandbox that can reach `browser_data/sites/` and none of the user's other
files (section 6.4), so even a bug outside its verified core can't touch them.

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
from the Public Suffix List (compiled into the browser, about 190 KB; `a.github.io` and
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

### 3.4 The caches

Both caches live in the site's folder under `cache/`, so they are partitioned by top-level
site: a page can't learn from timing whether another site's tab fetched or ran something.
- **The budget.** Cache entries don't count toward the site's quota (that is for what the site
  stores itself). They have a budget of their own, 256 MB per site with entries of at most
  32 MB. The storage service evicts the least recently used entries to stay within it: a read
  marks an entry used.
- **The HTTP cache** (`src/proc/net_cache.c`), in `cache/http/<sha256(url)>`, is the network
  process's. It is filled with responses to the requests the site's tabs make. Only the
  network process may write there: the storage policy (`store_allowed`, proved) gives it
  `cookies` and `cache/http/...` and nothing else, and gives a tab neither, so a tab can't
  plant a response that would later be served as its site's.
  - **What is kept:** GET responses with status 200, not redirected, without `Set-Cookie`,
    `Vary` (but `Accept-Encoding`) or `no-store`, to requests without `Range`,
    `Authorization` or `no-cache`.
  - **Freshness:** `max-age`, `Expires`, or a tenth of the time since `Last-Modified` (at most
    a day). A stale entry with an `ETag` or `Last-Modified` is asked for again with
    `If-None-Match` / `If-Modified-Since`, and a 304 serves it.
  - A response from the cache goes through the same checks as one from the network (P10,
    CORS, ORB). `CYBER_HTTP_CACHE=0` turns it off.
- **The bytecode cache** (`src/code_cache.c`), in `cache/code/<sha256(source)>`, is the tab's.
  A classic script of 4 KB or more that a page ran is compiled again in full when the page is
  idle (a lazily compiled script can't be written) and written with `JS_WriteObject`; the
  next load of the same source runs that instead of parsing it. An entry records the engine
  build that wrote it and its source's whole SHA-256, both checked before its bytecode is
  read. Functions keep their source text as ranges in the script's
  (`JS_WRITE_OBJ_SHARE_SOURCE`), which the page supplies, so an entry is about 1.3 times its
  script. The harness takes a folder for it (`CYBER_CODE_CACHE`, off by default).
- **What a bad entry can do.** The bytecode reader is not hardened against crafted input. An
  entry is written only by a tab of its site, so a compromised tab could plant bytecode that
  its site's later tabs run: the compromise would outlive the tab, but stay in its site,
  whose tabs it already controls. The same holds for anything a site stores. The HTTP cache
  has no such path: its entries come only from the network process.

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
| `store.partition` | `net` → `store` | `net`; may name any site's `cookies` file and its HTTP cache (`cache/http/...`), nothing else | Once, at start |
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

### 5.4.1 As built in M6

- **Windows.** The UI has up to 8 windows, each with its own tab strip, address bar and
  content window. Tabs live in one table, and each window lists its own, in order. The UI
  tells the GPU process what each content window shows (`UG_WINDOW`: the window, the tab,
  the size; 0 × 0 when the window is gone). The GPU process keeps a swap chain per window
  (on the CPU, it presents each with GDI). `GU_SHOWN` names the window as well as the tab,
  so a tab dragged on isn't taken as shown in the window it left.
- **One layer per tab,** not tiles or separate scroll layers. The tab draws the viewport
  plus half a viewport above and below it, cut at the page's ends. Its frame header
  (`GpuFrame`) says where the layer lies in the page:
  - `origin_y`, the page's y at the layer's top row;
  - `view_h`, the viewport's height;
  - `scroll_y`, the page's y at the viewport's top;
  - `scroll_seq`, the last of the UI's scrolls the tab had seen;
  - `content_h`, the page's height.
- **Compositor scrolling.** The UI numbers each scroll of a tab. It sends the scroll to the
  tab (`UtScroll`) and to the GPU process (`UG_SCROLL`) at the same time. The GPU process
  shows the layer from the frame's `scroll_y`, moved by the scrolls numbered after the
  frame's `scroll_seq`, clamped at each step as the tab clamps. It presents again at once.
  Rows the layer doesn't cover stay white until the tab draws them. A page whose JS is
  busy still scrolls (section 11.1, M6).
- **The header is the tab's word.** The decoder skips the five numbers (its proof only
  moved the records 20 bytes on); `gpu_main.c` reads them from the message. It uses 64-bit
  arithmetic and clamps where the viewport may land, within `GPU_MAX_H` rows either side of
  the layer.
- **`position: fixed`** is drawn in the layer like everything else. During a compositor
  scroll it moves with the page until the tab's next frame puts it back.

### 5.5 When there is no GPU, or the GPU process crashes

The CPU rasterizer stays. Without a usable GPU, the GPU process rasterizes display lists on the
CPU with the same decoder, so the security story doesn't change. If the GPU process crashes,
the coordinator starts a new one, grants the `draw` channels again, and every client sends a
full display list and its resources again; tabs keep running.

### 5.5.1 As built in M5

What M5 built differs from sections 5.2 to 5.5 in these ways, all for simplicity at today's
size:
- **The wire format** (`src/ipc/gpu_wire.h`) has the display list's own seven record types
  (rect, border, glyph, image, mask begin and end, shadow), not clips, transforms or opacity
  layers, which the engine doesn't produce yet. Every number is an integer:
  - lengths in 1/256 px;
  - colors and gradient stops in 1/65535, so 8-bit CSS colors arrive exactly;
  - texture coordinates in 2^-24, so atlas positions arrive exactly.
- **What crosses** goes over the tab's draw channel (`bigmsg.h` messages), not a command
  section and a transfer section:
  - the frame (`TG_FRAME`);
  - rows of a font slot's glyph atlas (`TG_ATLAS`, only rows that changed);
  - decoded images (`TG_IMAGE`, again when their pixels change).

  The GPU process copies each message out of the channel before it is decoded. A tab's
  uploads count against 1 GB; past it, images are dropped.
- **One texture per tab,** at the viewport's size, not tiles: the tab sends the visible part
  of its frame, as it painted before. M6 makes it a layer taller than the viewport, for
  compositor scrolling (section 5.4.1).
- **The decoder** (`src/proc/gpu_decode.c`) is proved (P1, P5, P7; section 8.6). The
  rasterizers draw only what it passes.
- **Two rasterizers.** The Vulkan one is `src/proc/gpu_vk.c`, with shaders in
  `src/proc/shaders`, compiled by `glslc` at build time and embedded; it loads
  `vulkan-1.dll` at run time. It computes the same coverage as the CPU rasterizer
  (`raster_cpu.c`), so the two agree to within rounding (section 11.1, M5). Without
  Vulkan, or with `CYBER_GPU=cpu`, the GPU process draws with `raster_cpu.c` and presents
  with GDI.
- **A crash or exit of the GPU process** is an event for the coordinator's core
  (`CE_GPU_EXITED`). Its actions restart the GPU process and grant every live tab a new
  draw channel, badged with its label (`CA_REGRANT_GPU`, proved as part of P3). The new
  channel reaches the tab over a one-way channel from the coordinator (`CT_GPU`). The UI
  gets its own new channel (`CU_GPU`) and makes a new content window: the old one stays
  tied to the dead process's swap chain. After three starts within a minute the next GPU
  process draws on the CPU. `CYBER_GPU_CRASH_TEST=N` makes it exit after N frames, for
  tests.
- **Screenshots for tests** ask the GPU process for the content's pixels (`UG_SHOT`), which
  only the UI may do. A tab's channel has no message that returns pixels.

### 5.6 What stays in the tab

`<canvas>` 2D (it needs `getImageData`, which reads pixels back) is drawn on the CPU in the tab
and uploaded as an image every frame it changes. Video frames are decoded in the tab (on its
decode thread, `src/media/media_buffer.c`) and uploaded as images; zero-copy video decode on
the GPU is a later optimization that would need a hardware decoder in the GPU process.

### 5.7.1 The GPU process and the UI share an AppContainer

Spikes for M5 showed what Windows allows. An AppContainer process can create a Vulkan device
and a surface on another process's window, but:
- creating a swap chain on it fails (`VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`) unless both
  processes are in the same AppContainer;
- creating a child window of its own inside the other process's window fails too (access
  denied).

So the GPU process runs in the UI's AppContainer (`cyberbrowser.ui`), with a job of its own
(8 GB). It presents into the UI's content window, a child window that passes mouse input to
its parent (`HTTRANSPARENT`). The UI's AppContainer has no capabilities and no files, so the
GPU process gains little beyond the UI's windows. The cost: a compromised GPU process could
post input messages to the UI's windows (open or close tabs, type a URL). The UI still accepts
only `http:`, `https:` and `about:blank`, and has no files or network of its own. The
alternative, sending every frame's pixels to the UI to draw with GDI, keeps the processes
apart at the cost of a copy per frame. It was considered and set aside.

For the same reason the UI now stays sandboxed in scripted tests: an unconfined UI's window
can't take the GPU process's frames. The coordinator gives the UI's AppContainer the script's
folder, where its screenshots go too.

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

### 5.7.2 As built in M8

**The process.** `cb_webgpu.exe` (`src/proc/webgpu_main.c`), one per site, labeled with the site.
- **Startup.** The coordinator starts it the first time a tab of the site asks: the tab sends
  `TU_WANT_WEBGPU` to the UI, the UI sends `UC_WEBGPU` to the coordinator, and the coordinator
  answers with `CT_WEBGPU` (a channel) or `CT_WEBGPU_FAILED` on the tab's control channel.
- **Who it serves.** It accepts only channels badged with its own site. The coordinator core
  grants a tab only its own site's process (P3, proved).
- **Ending.** It ends with the site's last tab (`CA_KILL_WEBGPU`), or after 30 s with no tabs.
  Either way, what the libraries kept for those tabs goes with it.
- **Sandbox.** An AppContainer of its own per process (`cyberbrowser.webgpu.<pid>.<n>`) with
  no capabilities, win32k on, and a 4 GB job. The libraries are in `webgpu\` next to the
  executables, readable by every AppContainer.

**The libraries.**
- **wgpu-native** v29.0.1.1, from its GitHub release.
- **ANGLE** at commit 890b5d8f, from MSYS2's `mingw-w64-x86_64-angleproject` package.
  Neither Chrome nor Electron ships ANGLE as DLLs any more. This one is built with MinGW, so
  it brings the MinGW runtime DLLs, pinned too.
- **Pinned.** `third_party/prebuilt/manifest` pins every file by SHA-256, and
  `tools/fetch_prebuilt.sh` fetches and checks them. A stale ANGLE is a known-exploitable
  ANGLE: updating this pin is part of every security update.
- **Loading.** Both load at run time by full path, on the first context a tab asks for.

**WebGL** (`src/proc/webgl_backend.c`, `src/browser/webgl_api.js`).
- **The context.** Each context is an ANGLE context on D3D11 with:
  - `EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE`, so ANGLE does WebGL's checks;
  - robust access, and resources initialized before first use;
  - lost on a GPU reset;
  - binding a name `glGen*` never made is refused, and there are no client-side arrays.
- **Calls go as records:** `[u32 op][u32 n][args]`.
  - The 101 plain calls are generated from one table (`tools/gen_webgl.js` makes
    `webgl_gen.c`, `.h` and `.js`).
  - Uploads, uniforms and the like are special ops (256 and up).
  - Queries are ops 300 and up; each appends its answer to the reply.
- **What the tab never sees.** A tab's object ids map to GL names per context, so an id it
  never made, or another context's, names `WEBGL_NO_NAME` and ANGLE refuses it. Anything
  that reads memory goes through ANGLE's `robust*` entry points with the size the tab
  claimed.
- **When records go.** The tab batches them and sends them at the end of the task, or at
  once when something must be answered.
- **Context loss.** A lost context, or the process gone, fires `webglcontextlost` at the
  canvas.
- **Extensions.** These are ANGLE's requestable ones under their WebGL names.
  `EXT_disjoint_timer_query` is never offered.

**WebGPU** (`src/proc/webgpu_wgpu.c`, `src/browser/webgpu_api.js`).
- **The wire format.** Records carry descriptors as value trees, parsed within limits
  (`webgpu_wire.h`: depth 12, 16,384 nodes).
- **The adapter.** `requestAdapter` checks the block list, and gives the vendor, the
  architecture and the features. There is no `timestamp-query`. Devices get WebGPU's
  default limits.
- **Promises.** `mapAsync`, `onSubmittedWorkDone`, `popErrorScope` and `requestAdapter`
  are answered synchronously; their promises resolve when the reply comes.
- **wgpu-native aborts on page mistakes.** It aborts the process (a Rust panic) on much of
  what WebGPU defines as a validation error. The fuzzer found these cases:
  - an enum value it can't map, or a required one missing;
  - a missing module or view;
  - popping an error scope never pushed;
  - reading the mapping of an invalid buffer;
  - a second adapter handle released;
  - any submit that fails its checks: a command buffer encoded wrong, one submitted
    twice, one using a mapped or destroyed buffer.

  None of these reach it:
  - a call that would abort is skipped, and reported as a validation error in the
    page's next error scope;
  - buffers are created, and command buffers finished, inside error scopes of the
    process's own, so an invalid one is never kept;
  - a submit is checked first against what each command buffer uses (buffers and
    textures, mapped or destroyed: tracked per resource);
  - there is one adapter per process, each id with its own reference.
- **Leaks in wgpu-native.**
  - It keeps a device's memory for good when a command buffer is finished and never
    submitted. When a tab goes, its leftover valid command buffers are submitted (no
    one is left to see them) before the device is released.
  - `wgpuDeviceDestroy` leaked too. `device.destroy()` instead destroys the device's
    buffers and textures and stops using it.
  - Some cases remain, a submit refused while a buffer is mapped among them. They use up
    only that site's process, and end with it.
- **Ids.** Object ids come from one per-tab allocator shared with WebGL. They go back when
  WebGPU makes an object unusable, or when the page lets go of it
  (`FinalizationRegistry`).

**Canvases on the screen.** Each frame a canvas was drawn to:
1. the tab asks for its pixels (`TW_PRESENT`);
2. the web-GPU process reads them back (WebGL from its surface, WebGPU from the canvas
   texture);
3. the tab puts them in its image cache as `cyber-canvas:<node>`;
4. the display list draws them like an image, so the verified decoder (P5) resolves
   them in the tab's own namespace.

**Deviation.** Section 5.7 planned textures shared with the compositor. This is a copy
through the tab instead: simpler, and no new path into the compositor. The cost is a
readback and an upload per presented frame, which matters for large canvases at 60 fps.
Shared textures remain the plan for when that cost shows.

**Cost to pages that don't use them.** The two scripts (about 80 KB) are evaluated in a realm
only when it first touches WebGL or WebGPU: a global like `WebGLRenderingContext`,
`getContext("webgl"/"webgl2"/"webgpu")` or `navigator.gpu` (`webgpu_lazy.js`). Until then
these are stand-ins.

**The block list** (`src/proc/gpu_blocklist.c`) is vendor and driver-version rules.
- **Blocked:** a blocked device has neither WebGL nor WebGPU: `getContext` returns null and
  `requestAdapter` resolves to null. It covers software rasterizers, and old Intel, AMD and
  NVIDIA drivers.
- **Fewer features:** a rule may only deny features instead: compute shaders, storage
  textures, optional features.
- **Test hooks:** `CYBER_GPU_BLOCK` (a device-name substring) and `CYBER_GPU_DENY` (a
  feature mask).

## 6. The tab sandbox (Windows)

`src/proc/sandbox.c` (the coordinator's side) and `src/proc/spawn.c` (the process
attributes) set this up. Every tab process starts with:

### 6.1 A token with no rights

- **Less Privileged AppContainer (LPAC).** LPAC, unlike a plain AppContainer, doesn't get
  the default access that `ALL APPLICATION PACKAGES` has to much of the system. Only
  `ALL RESTRICTED APPLICATION PACKAGES` gets it, which covers the system DLLs and fonts.
  The token carries the `WIN://NOALLAPPPKG` attribute that marks it.
- **No capabilities.** In particular, no `internetClient`: Winsock won't even start.
- **An AppContainer profile of its own.** `CreateProcess` refuses an AppContainer SID with
  no registered profile (`ERROR_FILE_NOT_FOUND`; the question this section left open). So
  the coordinator registers a profile for each tab process,
  `cyberbrowser.tab.<coordinator pid>.<n>`, and deletes it when the process ends. Its SID
  is no other process's, ever. That matters beyond isolation: an AppContainer can write to
  its profile's folder (`%LOCALAPPDATA%\Packages\<name>\AC`) and registry storage. With a
  profile reused for the next tab, a page could leave data there for another site's page,
  which the escape tests showed. Registering takes about 7 ms and deleting 2.4 ms. A
  coordinator that crashed can't delete its tabs' profiles, so the next one deletes the
  profiles of coordinators that are gone; `cb_coord.exe --remove-profiles` deletes all of
  the browser's profiles.
- **Its objects out of reach.** The coordinator gives each child's process and initial
  thread objects a DACL of the user and SYSTEM only and a medium integrity label (set after
  creation, while the child is suspended: in the creation-time security descriptor, a label
  needs `SeRelabelPrivilege`). Every sandboxed process runs at low integrity, so none can
  open another, whatever the DACL says. The tab's token gets a default DACL (the user,
  SYSTEM and its own AppContainer SID) for the objects it creates itself, such as its later
  threads. That matters for the loopback relay, which isn't an AppContainer (section 6.4).
  Channel sections and events have no names, so a process reaches only those it was given
  handles to.

### 6.2 A job object

Each child has a job of its own, nested in the coordinator's job, which ends them all
when the coordinator does (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`). A tab's job sets:
- one active process (no child processes);
- a 4 GB memory limit;
- UI restrictions: no desktops, display settings, system parameters, logging off, global
  atoms, clipboard, or USER handles from outside the job.

The child process policy (`PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY`) would repeat the
first limit. It isn't used, because with it our processes fail their DLL initialization
(`STATUS_DLL_INIT_FAILED`). Children start detached (`DETACHED_PROCESS`), with their
output going to an inherited log file. A console would come with a console host process,
which the escape tests found a tab could open.

### 6.3 Process mitigation policies (set at creation)

| Policy | Effect |
|---|---|
| Win32k system calls disabled | Removes the largest kernel attack surface. The tab executable imports only `kernel32`, `advapi32`, `msvcrt`, `bcrypt`, `crypt32` and `ws2_32`, none of which loads `user32`. `SHELL32` is loaded on demand by the one function that uses it. Everything else is linked statically, MinGW's `libwinpthread` included: a sandboxed process can't load DLLs from outside System32 and its own folder. A process with win32k disabled can't initialize `user32` at all, so it can't make windows, use the clipboard or reach the GPU. |
| Image load: no remote images, no low-label images, prefer `System32` | DLL planting. |
| Dynamic code prohibited (ACG) | No new executable memory, so shellcode can't be written and run. Compatible today because T1 and T2 are off; when the JIT returns, it runs in a separate JIT process (as in Edge) or this policy is dropped for that tab (section 12). |
| Strict handle checks, extension points disabled, heap terminate, bottom-up and high-entropy ASLR, no non-system fonts | Hardening. |
| CFG | Not yet: MinGW's GCC can't build with it. |

### 6.4 The other processes' sandboxes

| Process | Token | Notable rights |
|---|---|---|
| GPU | The UI's AppContainer (`cyberbrowser.ui`), no capabilities (section 5.7.1) | win32k and the graphics driver (it can't do without them); its own job: one process, 8 GB, UI restrictions except handles (it presents into the UI's window); ACG off (drivers generate code) |
| Web-GPU | LPAC, no capabilities, a fresh SID per process | win32k and the driver, as for GPU; memory limit; no windows (it renders only into shared textures) |
| Network | LPAC (profile `cyberbrowser.net`) with `internetClient`, `privateNetworkClientServer`, `registryRead` and `lpacCryptoServices` | Sockets, except to loopback; none of the user's files, only the replay folder (reading) or the recording folder (writing) when those are used; win32k disabled; a 2 GB job |
| Loopback relay | Restricted (every access), low integrity | TCP to `127.0.0.1` and `::1`; reads the system's files, not the user's; writes nowhere; win32k disabled; a 256 MB job |
| UI | AppContainer (profile `cyberbrowser.ui`) | win32k for windows and input; no network or files except through `store` |
| Storage | LPAC (profile `cyberbrowser.store`), no capabilities | Full control of `browser_data/sites/` alone, granted by the coordinator on that folder; win32k disabled; a 512 MB job |

**The loopback relay (M4).** An AppContainer can't connect to loopback addresses at all unless
an administrator exempts it, so `localhost` pages and local development servers go through
`cb_loop.exe`, a relay with a verified core (`src/proc/loop_core.c`, P8):
- **The protocol.** For a URL whose host is `localhost`, `*.localhost`, `127.0.0.1` or
  `::1`, the network process asks the relay over a channel to connect to a port of
  `127.0.0.1` or `::1` (`NL_CONNECT`). The relay connects there and nowhere else, and passes
  bytes both ways without reading them (`NL_DATA`, `LN_DATA`). The network process still
  does all the TLS and HTTP. The relay's buffers stay fixed: it writes one `NL_DATA` at a
  time to a socket, and reads from a socket only while the bytes it passed on that the
  network process hasn't credited back (`NL_CREDIT`) stay under 256 KB. The TLS client (`tls_client.c`) routes loopback
  hosts through `net_loopback.h`, implemented in the network process by
  `src/proc/loop_client.c`.
- **The relay's token** is a restricted token at low integrity, restricted for every access,
  not only writes. Each access needs one of its restricting SIDs (restricted code, Everyone,
  Users, Authenticated Users) as well as the user's. The system's files grant those reading,
  so DLLs load and Winsock starts. The user's profile, the other children and the tabs'
  objects don't, so it can neither read nor write them. A spike confirmed this: under that
  token a file in `Documents` is refused and loopback connects, while under the
  write-restricted token of M3 the same file opened.
- **Replays:** the coordinator grants the network process's AppContainer read access to the
  folder `CYBER_REPLAY` names, and full access to the one `CYBER_RECORD` names, as it grants
  the storage service `browser_data/sites`. Replays don't touch the relay.
- **LPAC needs two named capabilities** besides `internetClient` (and
  `privateNetworkClientServer`, for the local network): `registryRead` (without it Winsock
  won't start: `WSAStartup` fails with 10107, reading its catalog) and `lpacCryptoServices`
  (the certificate store). Their SIDs come from `DeriveCapabilitySidsFromName`.

The relay doesn't decide which pages may reach `localhost`: that's Private Network Access, a
policy of the network process (M7).

In the scripted tests (`CYBER_UI_SCRIPT`) the UI runs unconfined, since it reads the script
and writes screenshots wherever the test says. `CYBER_NO_SANDBOX=1` starts every child
unconfined, for debugging.

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

As built in M6, a press on a tab becomes a drag once the mouse moves 8 px. Where it's
released decides the outcome:
- on its own strip, the tab moves along the strip;
- on another window's strip, the tab joins that window where it was dropped;
- anywhere else, the tab gets a window of its own at the drop point. A tab alone in its
  window moves the window instead.

A window left with no tabs closes, and closing the last window ends the browser. There are
no focus events yet. Test scripts drag with the same mouse handlers (`dragout`,
`dragto W`), or move tabs directly (`detach`, `attach W`).

## 8. Verification

### 8.1 What is verified

| Component | Size (target) | Size (M7) | What is proved | Goals (M7) |
|---|---|---|---|---|
| Coordinator core (`coord_core.c`) | 2,000–3,000 lines | 430 lines | P1, P3, P6 (local respect), P7 | 4,898 |
| `libchan`'s rings (`chan_ring.c`) | ~1,000 lines | 100 lines | P1, P2 | 214 |
| `site_of` and the host grammar (`site.c`) | ~500 lines plus the PSL table | 290 lines | P1, P7, and that every label it makes is well formed | 2,709 |
| Storage service core (`store_core.c`) | ~1,500 lines | 120 lines | P1, P4, P7 | 5,153 |
| Loopback relay core (`loop_core.c`) | ~300 lines | 320 lines | P1, P7, P8 | 874 |
| GPU display-list decoder (`gpu_decode.c`) | ~1,500 lines | 300 lines | P1, P5, P7 | 1,265 |
| Network policy core (`net_policy.c`) | — | 210 lines | P1, P7, P9 | 1,873 |

The cores came out much smaller than the targets because what they decide is still small:
three kinds of tab channel, no spare process, no navigation swaps. Section 8.6 says what M4
proved exactly, and where it differs from the properties below.

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
  only `cookies` files and HTTP cache entries (`cache/http/...`, section 3.4), which a tab never
  gets. The quota is never exceeded.
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
- **P8. The relay reaches only loopback.** Every connect action the relay core emits names
  `127.0.0.1` or `::1`, and every byte it forwards on a connection comes from the channel
  message for that connection or from that connection's socket, in order and unchanged.
- **P9. No page reaches a more private network.** The network process's policy core
  (`net_policy.c`) allows a connection only to an address space no more private than the
  requesting tab's: public pages reach no private or loopback address, private pages no
  loopback one. Its address classes and its ORB verdict are proved free of undefined
  behavior and bounded (P1, P7) for any bytes. Applying them is the shim's work.

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
check the code. M4's model (`proofs/tla/Protocol.tla`) covers what exists so far: tabs that
start, close and crash with requests in flight, grants and revocations reaching the servers
later over their control channels, and tab ids reused across sites (section 8.6).

### 8.4 What is trusted (not proved)

The Windows kernel and its AppContainer, job and mitigation enforcement; the shims and their
ACSL contracts (the coordinator's, the storage service's, the relay's, and `chan.c`, which
reads each shared counter once and hands the values to `chan_ring.c`); the hypotheses of WP's
memory model (section 8.6); the network process and its side of the relay
(`loop_client.c`); the UI process; the GPU process outside its decoder,
and the graphics driver; the web-GPU processes (ANGLE, wgpu, Naga), trusted only to the
extent that a compromise of one exposes its own site's GPU work and the driver; the C compiler (GCC/MinGW; CompCert, a verified compiler, if its
targets cover Windows x64 when we get there); the hardware; and the Frama-C tool chain and
provers. Leaks through timing are not covered (see "Non-goals").

### 8.5 Tooling

Frama-C runs on Linux and macOS; here it runs in WSL (Ubuntu 24.04).
`tools/proof_setup.sh` installs everything without root, into `~/.local` and `~/.opam`:
- GMP 6.3.0, built from source (`zarith` needs it, and WSL had no headers);
- opam 2.6.1, OCaml 5.2.1, Frama-C 33.0, Why3 1.8.2 and Alt-Ergo 2.6.4, from opam;
- Z3 4.13.4 and CVC5 1.2.1, as release binaries;
- `tla2tools.jar` 1.7.4, run with the Java that WSL already had;
- a stub `dot`: Frama-C's package wants Graphviz, which only draws graphs we don't use.

`tools/prove.sh [chan coord site store loop tla]` runs WP on each core (with the RTE plugin
for P1, the `Typed+Cast+ref` memory model, and `-wp-check-memory-model`) and TLC on the
model. It fails if any goal is unproved. Since M5 it splits goals into their cases
(`-wp-split`), which counts more goals but proves each faster, and gives the decoder 240 s a
goal (60 s for the rest; 120 s until M6). The seven cores take about 45 minutes from a cold cache on 12 cores,
and the two model checks about 9.5 minutes. A goal that timed out is cached as such: clear
the session (`/tmp/cb_prove/<core>.session`) before retrying with a longer limit. The repository has no CI yet, so the script is
run by hand before changes to a core are committed.

### 8.6 What M4 proved, and how it differs from the plan

- **P1, P7 everywhere.** Every core is free of undefined behavior for any input (RTE goals:
  memory accesses, signed overflow, shifts, division, casts), and every loop has a variant.
  Unsigned wraparound isn't flagged, because it is defined in C and the channel counters wrap
  on purpose; out-of-bounds accesses are flagged whatever the arithmetic before them.
- **P2.** `chan_ring_put` and `chan_ring_take` stay inside the ring and the private message
  whatever the counters and the ring's bytes hold, and a message they return is at most
  `CHAN_MSG_MAX` bytes. Reading each counter once is `chan.c`'s part, which is reviewed, not
  proved: it reads `tail` once with an acquire load and passes the value in.
- **P3, in the core's terms.**
  - **Labels:** every label a slot holds is well formed, or `""` for an opaque origin.
  - **Badges:** a tab starts with channels badged with its slot's label, the network
    process's always and the storage service's only for a site.
  - **Starts and ends:** a start takes a free slot. A slot's label and tab id never change
    while it is in use. Revocations name the slot's own tab, and the UI's ids name one tab
    at a time.
  - **URLs:** a URL is started only if it is `http:`, `https:` or `about:blank` and
    printable.

  To make that provable, the decision about which servers a tab gets channels to moved from
  the shim into the core (`grant_net`, `grant_store`). That the shim hands each process only
  its own handles is reviewed, not proved.
- **P4.** Every path `store_rel_path_ok` accepts satisfies the grammar of section 3.3, as an
  ACSL predicate:
  - components of `[a-z0-9_.-]`, none empty;
  - none starting or ending with `.` (so no `.`, `..` or trailing dot);
  - none a device name.

  `store_join` makes `<label>/<path>` and nothing else. `store_allowed` lets a tab only at
  its own label and never at `cookies`, and the network process only at `cookies`. The quota
  check implies `used - old + new <= quota`. Labels hold only lowercase letters, so two
  sites never share a folder even on a case-insensitive file system. Opening the file
  (refusing reparse points and hard links) is the shim's, and tested by the escape tests.
- **`site_of`: well-formed labels, not "eTLD+1 as specified".** The proofs show every label
  it makes is `<scheme>+<suffix of the host>` with exactly one `+` and only `[a-z0-9.-]`,
  which is what the folders and badges rely on. Which suffix the Public Suffix List picks
  isn't proved: the binary search would need the table's order and contents as axioms. It is
  checked instead against an independent implementation on 41,590 hosts (M2), and its table
  lookups bound every read by the table's size, so their safety doesn't depend on the
  generated data.
- **P6 as local respect, without the two-run harness.** The coordinator gets nothing from a
  tab but its exit (the process ending, with its exit code). The proof shows that such an
  event changes no other slot and causes only that tab's revocation and a message to the
  UI. The only data it carries, the exit code, goes to the UI alone. What the coordinator
  sends a tab, its start action, comes from the UI's request and that slot alone. Given
  that, the self-composition harness adds nothing yet. It will be needed when tabs ask the
  coordinator for things: `opener` channels and the spare process.
  - **Two flows remain, outside P6's scope:**
    - A site's tabs can take all 60 slots, so another site's tab fails to open (an
      availability channel).
    - When a slot frees up, the next tab may reuse it; that slot number goes nowhere a tab
      can read.
  - **Found and fixed by the analysis:** each tab's AppContainer profile was named with a
    count of the tabs started before it. A tab can read its own profile name, so the count
    told a page how many tabs of any site came before it. The name is now 64 random bits
    (`sandbox.c`).
- **P8.** Every connect action of the relay core is to `LOOP_V4` or `LOOP_V6`, which the shim
  maps to `127.0.0.1` and `::1` and nothing else, on a port from 1 to 65535. Bytes written to
  a socket are an `NL_DATA`'s, unchanged, for the open connection with that message's id.
  Bytes sent to the network process as `LN_DATA` are the ones read from that connection's
  socket, unchanged, under its id.
- **Memory model.** WP's `Typed+Cast+ref` model treats pointer parameters used only as
  references as separate objects. It states the hypotheses that requires, and
  `-wp-check-memory-model` proves them at every internal call. At the shims' calls they are
  assumptions, and they hold there:
  - the inputs don't overlap the constant PSL tables or string literals;
  - an event doesn't overlap the state or the action list.

  The plain `Typed` model left 7 goals unproved, because every write to an action made the
  provers re-derive, for each slot, that the slot table was untouched.
- **The cores were reshaped to be provable,** without changing what they do:
  - `chan.c` was split into a Win32 shim and `chan_ring.c`;
  - masks became `%` and `/`, which the provers handle; `chan_bench` shows no change;
  - slot updates go through two small functions with contracts (`set_state`, `start_slot`);
  - each step changes the state before it writes its actions.
- **The TLA+ model** checks the invariants and liveness properties in
  `proofs/tla/Protocol.tla`, with 2 tab ids, 2 sites and an opaque origin. TLC found no
  violation of the invariants in 13.0 million distinct states with up to 3 tab processes
  (`Protocol.cfg`), nor of the liveness properties and invariants together in 141,505 states
  with up to 2 (`ProtocolLive.cfg`, smaller because checking liveness keeps the whole state
  graph). The deadlock check is off: with the starts bounded, every behavior ends quiet.

  The invariants:
  - a server serves a request only with the badge of the process at the other end of its
    channel;
  - what a server holds is badged truly, and the storage service holds no opaque tab;
  - a server has one client per tab id;
  - a server holds a channel only while its tab is the id's current one, or its revocation
    is on its way.

  The liveness properties: a closed tab ends and its id frees, and every dead tab's channel
  is eventually revoked at every server.

### 8.7 Plan for M9: proving the shims

**Why.** A proof covers the functions it is run on, under their preconditions. The cores
decide correctly, but the shims around them are ordinary C:
- they turn a tab's bytes into the events the cores see;
- they turn the cores' actions into Windows calls: which handle goes into which process.

A compromised tab reaches the shims first. A misread id, or a handle copied into the wrong
process, is an escape the proofs can't see. M9 moves that line: the shims keep only thin
wrappers around Windows calls, with ACSL contracts. Everything that reads a tab's bytes, or
chooses a handle or a process, moves into proved code.

#### 8.7.1 What a tab reaches

From a review of the code at M8. No privileged process reads a handle value or a file path
from a tab. Every handle is made and granted by the coordinator's shim.

| Process | Unproved code facing tabs | Proved today | Behind it, out of M9's scope |
|---|---|---|---|
| All servers | `chan.c` (`chan_attach` reads the shared header's size and magic); `bigmsg.c` `big_recv`, reassembling up to 256 MB | `chan_ring.c` (P2) | |
| Network | `run_request`'s parsing of `NetRequest`; cookie messages; CORS (`may_read`), `initiator_ok`, the frame list; PNA in `tls_client.c` (`space_allowed`) | `site_of`, `net_space_v4`/`v6`, `orb_verdict` | the HTTP, TLS and URL code (about 5,400 lines) |
| Storage | `request`'s parsing; `to_path`; the containment check on handles | `store_allowed`, `store_join`, `store_quota_ok` | |
| GPU | `upload_atlas`, `upload_image`; `frame`'s header and the view it presents | `gpu_decode` (P5) | the rasterizers (about 1,450 lines) |
| Web-GPU | `client_message`; the WebGL record walker (`webgl_run`, `run_special`, `webgl_gen.c`); the WebGPU value-tree parser and object ids (`webgpu_wgpu.c`) | nothing | ANGLE, wgpu, Naga |
| UI | `from_tab` (about 60 lines: frames, titles, state, navigations) | `coord_url_ok`, through the coordinator | the window code |
| Coordinator | Reads no tab bytes. `perform` copies handles (`dup_to`): the maps from slot to process, from web-GPU index to process, from channel to server, and from an exit to its slot | the decisions (P3, P6) | `spawn.c`, `sandbox.c` |

#### 8.7.2 Findings to fix first

The review and the M8 conformance runs found these. Each gets a fix and, where a tab can
cause it, an attack in the escape test.
- **A redirect to another site runs as the first site.** For a navigation, the network
  process follows redirects anywhere and returns the last page. The tab runs that page with
  its own site's origin and storage. So an open redirect on site A would run site B's script
  as A. This was found by reading the code; it needs confirming with a test first. The fix:
  a navigation whose final site isn't the tab's is refused with its final URL, and the tab
  asks the UI to navigate there. The coordinator then starts a tab of the right site. A new
  property, P10, says that a document a tab is given is from the tab's own site.
- **Redirects keep the old URL.** Even within the site, a page keeps the URL from before the
  redirect, so its relative URLs resolve against the wrong folder. A relative `Location` isn't
  resolved at all. The network reply should carry the final URL, and the tab should use it.
- **PNA is enforced by an unproved copy.** `tls_client.c` `space_allowed` compares address
  spaces itself. The proved `net_space_allows` is called only by the tests.
- **A tab can pick the test hook's tab executable.** A navigation URL ending in `#escape`
  starts `CYBER_TAB_EXE` instead of `cb_tab.exe` when that variable is set (`coordinator.c`).
  The URL trigger should go: the escape test can set the variable alone.
- **An atlas left half-made.** If replacing a font atlas runs out of memory, the slot stays
  marked uploaded with no pixels (`gpu_main.c` `upload_atlas`). Then P5's guarantee, that
  every glyph names an uploaded atlas, no longer means a usable one.
- **Denial of service across sites.** None of these crosses a site's data, but each lets one
  tab slow the others:
  - a tab that doesn't read its replies holds a network worker for 30 s (of 16), and the
    single-threaded storage service for 10 s;
  - the network process's job queue is unbounded, and its `malloc` unchecked;
  - each channel can make a server hold 256 MB;
  - a tab can open 60 tabs through `TU_NAVIGATE` and use up every slot.
- **Smaller:**
  - the relay's `step` drops events past a depth of 4, so the core and its sockets can
    disagree;
  - the storage service never drops a broken network-process client (`revoke` matches tabs
    only);
  - whether a WebGPU canvas texture can be used after `canvas_texture` released it hasn't
    been traced.

#### 8.7.3 The work, in order of what a tab reaches first

1. **The channels' framing.** `big_recv` and `chan_attach`'s checks move into a proved core:
   - no undefined behavior for any bytes (P1);
   - a message is exactly the fragments sent, at most `BIG_MAX`;
   - a per-channel limit on what a server holds.
2. **The coordinator's handles.** `perform` splits in two:
   - a proved `coord_plan` turns an action into a list of operations, each "copy handle *h* of
     table *T*, entry *i*, into the process of table *U*, entry *j*";
   - a wrapper makes each Windows call, with an ACSL contract for `DuplicateHandle`,
     `CloseHandle` and the spawn.

   With the handle tables as ghost state, the proof shows:
   - a tab's process receives only channels to servers badged with its slot's label, and a
     web-GPU channel only from its own site's process;
   - a process's exit is paired with the slot that started it.

   This proves at the code level what section 8.6 lists as reviewed. The `#escape` trigger
   goes.
3. **Each server's decoding of a tab's messages,** as functions without Windows calls, each
   proved free of undefined behavior and to accept only messages of a stated grammar, with
   every field it passes on in range:
   - network: `NetRequest` and the cookie messages;
   - storage: `StoreRequest` and `to_path`;
   - GPU: atlases, images and the frame header (fixing the atlas finding);
   - UI: `from_tab`;
   - web-GPU: the WebGL record walker and the WebGPU value-tree parser (section 5.7's plan).
4. **The network process's policy into `net_policy.c`:**
   - PNA through `net_space_allows`;
   - CORS (`may_read`), `initiator_ok` and the frame list;
   - the redirect check for navigations (P10).
5. **The rest of each shim, with Eva.** Frama-C's Eva runs on each process's whole shim, with
   stubs for the Windows calls. Every alarm is fixed or explained in this document. That
   shows the shim has no undefined behavior (P1). It doesn't show that the shim does the
   right thing; steps 2 to 4 cover that for what a tab controls.
6. **The protocol model.** `Protocol.tla` gains redirected navigations: a tab's document
   comes from its own site, or the tab is replaced by one of the final site's.

**Escape tests to add:**
- a cross-site redirect of the tab's own navigation;
- replies left unread on the network and storage channels;
- a 256 MB message;
- navigations to `#escape`;
- an out-of-memory atlas, by fault injection in a test build.

**After M9,** section 8.4's trusted list is:
- the Windows wrappers, named one by one;
- the UI's window code;
- the HTTP, TLS and URL code behind the decoded requests;
- the rasterizers;
- ANGLE, wgpu, Naga and the driver;
- what it lists today below the processes (the kernel, the compiler, the tools).

**Cost.** `tools/prove.sh` already takes about an hour. Each new core gets its own target, so
a change proves only what it touches. Steps 1, 2 and 4 are like M4's work. Step 3's web-GPU
decoders are the largest. If they would hold up the rest, they can follow as their own
milestone.

### 8.8 As built in M9

What a tab sends each privileged process is now read by proved code, and the coordinator's
choice of which handle goes into which process is proved. The details are in `notes/m9/`
(one file per part).

**New proved cores** (`tools/prove.sh` targets):

| Target | Core | What it shows | Goals |
|---|---|---|---|
| `chan` | `big_core.c` | a message is assembled only from its own parts, within the channel's limit, never past its buffer | 616 (with `chan_ring.c`) |
| `plan` | `coord_plan.c` | a tab's process gets only its own slot's channels; a server's end goes only to that server, badged with the slot's label; a web-GPU channel only from its own site's process; an exit is paired with the process that ended | 4,362 |
| `net` | `net_policy.c`, `net_decode.c` | P10 (below); CORS, initiators and the frame list; `TN_REQUEST` and the cookie messages decoded to a grammar | 4,541 |
| `store` | `store_core.c` | `store_decode`: the request read to its grammar, and the path opened is the one P4 allowed | see below |
| `gpu_input` | `gpu_input.c` | atlases, images and the frame header read to their grammars; a slot or image is marked uploaded only when its pixels are in place | 749 |
| `ui` | `ui_core.c` | the UI's reading of every tab message; when a tab may open a tab | 237 |
| `webgl` | `webgl_decode.c` | WebGL records: known ops, each at its layout's length; ids name only the context's own objects | 1,350 |
| `webgpu` | `webgpu_decode.c` | the value-tree parser (now iterative) and record framing stay inside the input; ids name only the client's live objects; map ranges without overflow | 2,590 |

**P10.** A document a tab is given (`HTTP_MODE_NAVIGATE`) is from the tab's own site. For any
other mode the core places no limit (frames may be any site's, as in section 9.1). The
network process refuses another site's document with its final URL (`NET_ELSEWHERE`), at the
request, at each redirect hop and at the end, and the tab asks the UI to navigate there. The
coordinator then starts a tab of that site. `Protocol.tla` models it: invariant `DocTrue`, and
`RedirectsFollowed` (a refused navigation is followed).

**Findings fixed** (section 8.7.2):
- **The cross-site redirect was real.** An open redirect on site A ran site B's page as A.
- Pages take their URL after redirects, and a relative `Location` resolves (there was no hop
  limit either: now 20).
- PNA goes through the proved `net_space_allows`.
- The `#escape` trigger is gone: the test hook comes from the coordinator's own environment
  only (`CYBER_TAB_EXE`, `CYBER_TAB_EXE_URL`), once.
- A half-made atlas is no longer marked uploaded.
- One tab can't stall the others:
  - the network process has bounded queues and a sender thread per tab;
  - storage has a one-reply queue per client, and drops a client whose reply hasn't moved
    for 10 s;
  - a tab opens a new tab only once per click it was sent.
- The storage service drops a broken network-process client; the relay no longer drops events.
- Each channel has a limit on what it holds (storage 64 MB, network 132 MB, GPU and web-GPU
  256 MB). The tab doesn't send storage a write over its quota.

**Found along the way, fixed:**
- the GPU process read an unset scroll value when presenting a frame;
- a WebGPU canvas texture destroyed through one id was still used through another;
- a NaN map offset converted to an integer;
- an unchecked name length from ANGLE;
- `location.origin` without its port;
- a screenshot buffer sized from the tab but filled at the render target's size;
- unchecked allocations (cookie jars, storage usage);
- long install paths leaving the data folder's path without a NUL.

**Eva** (`tools/prove.sh eva`, or `eva-<process>`). It runs on each process's shim with
stubbed Windows calls:
- `proofs/eva/include` declares about 150 Windows functions with contracts (trusted), on
  a MinGW machine description;
- `proofs/eva/drv_*.c` drive each process;
- the proved cores run as they are; the libraries outside M9 (HTTP/TLS, rasterizers, ANGLE,
  wgpu) are models.

A run fails on any alarm not listed, with its reason, in `proofs/eva/<process>.explained`.

**Not finished:**
- **The proofs.** Every WP target was proved by the agent that wrote it except `store`, whose
  final run was cut off with 8 of 6,323 goals open. Re-proving all of them together on this
  code was under way at this commit: `chan` 616/616 is confirmed.
- **Eva.** Only the channels (17 alarms, all explained) and the relay (21, explained) have
  final runs. Storage, the coordinator, GPU, web-GPU, UI and network need runs on the merged
  code and their alarms classified, so `eva` is not in `all`. The web-GPU process's
  WebGL/WebGPU executors (`webgl_backend.c`, `webgpu_wgpu.c`) are modelled, not analysed.
- **The model check.** `Protocol.cfg` checks safety with redirects off (6,502,489 states);
  `ProtocolLive.cfg` checks them, with liveness (252,227 states).

**Trusted after M9** (section 8.4 otherwise unchanged):
- the Windows calls behind the coordinator's operations, named with their contracts in
  `coordinator.c`;
- `bigmsg.c`'s copying and `realloc` around `big_core`;
- the shims' extraction of origins and header values for the network policy;
- storage's `send_reply` framing;
- the Windows stub contracts Eva relies on.

## 9. Iframes from other sites

In the first phases a cross-site iframe runs in its embedder's process, as in browsers before
site isolation. Its storage and cookies go to `partitions/<its site>/` under the embedder's
folder, so the embedder's process sees only the iframe's data partitioned under itself. It
never sees the iframe site's own (top-level) data. That keeps the containment property
about folders exact, but a compromised embedder can read what the iframe has in memory.
Out-of-process iframes (a tab made of several processes, composited together by the GPU
process, with input routed by hit testing in the UI) come after M7.

### 9.1 As built in M7

- **Frames of other origins load,** each in a realm of its own in the page's process
  (`frame_api.c`). Across origins a window is a stand-in made in the realm that looks at
  it. The stand-in offers only:
  - `postMessage`;
  - `location`, to navigate (reading it throws `SecurityError`);
  - `parent`, `top`, `frames`, `length` and `closed`.

  `contentDocument` and `frameElement` are `null`. Messages are queued and delivered on
  the event loop as JSON clones, with the sender's origin and its stand-in as `source`; a
  message whose `targetOrigin` doesn't match is dropped. `about:blank` frames take their
  parent's origin and fetch nothing.
- **Iframes load when they are connected,** parsed or inserted by script, as well as when
  script sets `src`. Before M7 only the last loaded them.
- **localStorage is kept per origin.** A frame's origin from another site keeps its store at
  `partitions/<its site>/local_storage/<origin>` under the page's site folder. A `+` becomes
  `_`, since path components can't contain one.
- **Cookies** were already partitioned: the network process keeps one jar per top-level site,
  and a frame's requests and `document.cookie` use it. Third-party cookies live in that
  site's `cookies` file rather than under `partitions/`.
- **Every request says what it is for** (`NetRequest`'s `mode` and `initiator`, from
  `http_request_context_set`): the page's document, a frame's document, cors, or no-cors.
  It also says which origin asks.
- **ORB and CORS** (section 2.4) are checked in the network process on the final URL, after
  redirects:
  - a no-cors response from another origin gets through only if `orb_verdict` allows it;
  - a cors response only with an `Access-Control-Allow-Origin` naming the initiator, or
    `*`.
- **PNA:** a tab's address space is its document connection's (until then its site's host's:
  `localhost` or an IP). Every other connect, redirects included, resolves the host in the
  network process and goes only to an address the tab may reach (P9). So does a pooled
  connection opened for another tab. `CYBER_PNA_PUBLIC_PORTS` makes loopback ports public,
  for tests.
- **What a compromised tab can still do:**
  - the mode and initiator are its word, so it can claim "frame document" and read another
    site's page, as frames require while they share their embedder's process;
  - an initiator counts only if it is the tab's own site or an origin it loaded a frame
    from, so claiming one gains nothing more;
  - what it reads comes without that site's own cookies, which stay in that site's jar;
  - PNA doesn't rest on its word: its documents are exempt, but its document connection
    sets its space.

  Out-of-process iframes close the frame gap.

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
| M4 | Proofs: coordinator, `libchan`, `site_of`, storage core (P1–P4, P6, P7); TLA+ model of the protocol. The loopback relay with its verified core (P1, P7, P8), and the network process moved to LPAC (section 6.4) | All goals proved (`tools/prove.sh`; CI when there is one); `localhost` pages and replays work with the network process in LPAC; the escape tests pass |
| M5 | GPU process: display-list format, verified decoder (P5), Vulkan rasterizer, compositing into the UI's windows | Pages render the same as the CPU path (screenshot diff on the replays); CPU fallback works (done) |
| M6 | Multiple windows; dragging tabs out and between windows; compositor scrolling | Drag works while a video plays, without a reload (done) |
| M7 | Cross-site iframe partitioning, ORB, Private Network Access | Frames from other sites load with their storage and cookies apart; the network process enforces ORB, CORS and PNA; the escape tests pass (done) |
| M8 | Web-GPU process: WebGPU on `wgpu-native`, WebGL 1 and 2 on ANGLE (pinned prebuilt DLLs), on the GPU; shared canvas textures; the driver block list and feature limits; fuzzing | The WebGPU and WebGL conformance suites pass at a level to be set then; a crashing web-GPU process takes down only its canvases; on a blocked driver the APIs are unavailable |
| M9 | Proving the shims (sections 8.7, 8.8): every privileged process's decoding of what a tab sends, and the coordinator's handing out of handles, in proved code; the rest of each shim free of undefined behavior (Eva); the findings of section 8.7.2 fixed first | All goals proved, Eva with no open alarm on the shims it covers; the escape tests pass with the new attacks; replays unchanged |
| Later | Out-of-process iframes; JIT process; GPU video decode | |

The single-process mode (`--single-process`, and what the replay harness uses) stays through
every milestone, so the YouTube replay benchmark keeps measuring the engine alone.

### 11.1 Progress

**M0 (done):** the page engine is a library (`src/page_engine.h`); `src/cyberbrowser_main.c` is
the single-process harness on top of it; the CPU rasterizer is `src/raster_cpu.c`.

**M1 (done):** `cb_coord.exe [url ...]` opens a browser window.

- **Executables.** `cb_coord.exe` (the coordinator), `cb_ui.exe` (the window), `cb_tab.exe` (a
  tab). `cyberbrowser.exe` was the single-process harness until the multi-process browser
  became the default (after M9): it is now what a user opens, a launcher with no console that
  starts `cb_coord.exe` beside it. The harness is `cb_harness.exe`, which
  `tests/jit/replay.sh` and the tests take as their executable.
- **libchan** (`src/ipc/chan.c`): the rings and events of section 4.2. Every message is
  copied out of shared memory before it is used, and indices and lengths are read once and
  checked. `chan_bench.exe` measures it between two processes:

  | Measurement | Cost |
  |---|---|
  | Blocking round trip (each side sleeps on its event) | 4.7 µs |
  | Spinning round trip (no system calls) | 0.13 µs |
  | Stream of 32-byte messages | 6 million/s |
  | Stream of 16 KB messages | 1.2 GB/s |

- **The coordinator.** Its decisions are a pure state machine (`src/proc/coord_core.c`): no
  Win32, no library calls, no allocation, bounded loops, ready for M4. A shim
  (`src/proc/coordinator.c`) performs its actions. Children inherit only the handles they're
  given (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`) and run in a job that ends with the coordinator.
  It accepts `http:`, `https:` and `about:blank` URLs only.
- **Tabs.** A tab loads its page with the engine and then runs `page_turn` without blocking,
  so input is answered while timers are pending. It rasterizes the visible part of each new
  frame (the display list shifted by the scroll offset) into one of its surface's two
  buffers. A click is hit-tested on the tab's own layout and dispatched as a DOM `click`; if
  nothing cancels it and it is on an `<a href>`, the UI navigates. Every navigation starts a
  new tab process: the engine holds one page per process.
- **The UI.** Win32 and GDI: a tab strip, an address bar, the content area, keyboard
  shortcuts, the wheel. It shows a crashed tab as such, and the other tabs carry on.
  `CYBER_UI_SCRIPT=<file>` drives it for tests (open, navigate, click, scroll, resize, save a
  screenshot of the window).
- **Memory of a tab process:** 20 MB private at start, 42 MB once the engine is initialized
  (JS runtime, browser APIs, fonts indexed), 47 MB with `about:blank` loaded (25 MB working
  set). With a page: 250–500 MB private for Wikipedia articles, 280 MB for github.com. The
  executable's code is shared between all of them (section 10).
- **Fixed on the way:** a live page fetch sent with gzip was never handed to the streaming
  parser (only brotli and plain bodies were decoded as they arrived), so a live load of
  Wikipedia parsed an empty page. Whatever couldn't be observed while it arrived is now
  observed once the response is complete. Replays were unaffected, since their bodies are
  decoded already.

**M2 (done):** the network process (`cb_net.exe`) and the storage service (`cb_store.exe`).

- **The coordinator labels each tab with its site** (`src/proc/site.c`: scheme and
  registrable domain; `about:blank` is opaque). When it starts a tab it grants channels to
  the network process and, unless the site is opaque, the storage service, badged with that
  label. It revokes them when the tab ends. The network process and the storage service are
  connected at start, and on quit the network process saves its cookie jars before
  everything ends.
- **Tabs have no network.** `http_download.c` takes a remote transport (`http_set_remote`):
  in a tab every request and `document.cookie` access goes to the network process, and the
  network code refuses to run. Large messages travel as parts (`src/ipc/bigmsg.c`); a
  receiver thread files responses by request id, so the engine's fetch threads can wait on
  them concurrently. Sampled during live loads of Wikipedia and GitHub: tabs held no TCP
  connections, the network process held them all.
- **Cookies live in the network process**, one jar per site (`http_cookie_jar_use` chooses
  a thread's jar). A request carries only its tab's site's cookies, added by the network
  process; HttpOnly cookies never reach a tab. Jars are saved every 2 seconds and at quit
  to `browser_data/sites/<site>/cookies` through the storage service, and loaded when a
  site's first tab starts.
- **The storage service** (`src/proc/store_main.c`) owns `browser_data/sites/`. What a client
  may name is decided by `src/proc/store_core.c`: the path grammar of section 3.3, a tab
  only in its own site's folder, the network process only a site's `cookies` file, a
  64 MB quota per site. Every file is opened refusing reparse points and hard links, and
  checked with `GetFinalPathNameByHandleW` to be in that folder. Writes go to a temporary
  file and replace the old one. One deviation from section 3.3: a read returns the file's
  content rather than a read-only handle, since the files so far are small.
- **`localStorage` persists**, per origin, in `local_storage/<scheme>_<host>_<port>` under
  the site's folder. It is loaded on first use, and the tab saves changes at most every
  500 ms.
- **Tested** with a local test server, over two browser sessions: the `localStorage`
  counter carried over; the network process sent the saved cookies, HttpOnly included, on
  the first request of the second session; `document.cookie` never showed the HttpOnly
  one. A tab of another site (`127.0.0.1` beside `localhost`) saw none of the first site's
  cookies or storage. Replays work through the network process (`CYBER_REPLAY` is served
  there): Wikipedia 42,607 DOM nodes, against 42,608 in the harness; the YouTube watch page
  7,366 against 7,485. The tab runs in real time rather than on the deterministic clock,
  and doesn't stop when the page settles.
- `proc_tests.exe` checks the pure parts: sites, the storage rules, the coordinator's core.
- **Fixed on the way**, both also in the single-process browser:
  - The engine's JS `fetch` wrapper put `document.cookie` into a `Cookie` header, so
    credentialed fetches never sent HttpOnly cookies. The native layer adds cookies from the
    jar, by credentials mode.
  - The image cache read any image source that wasn't a network or `data:` URL from the
    local disk. Tabs now turn that off (`image_cache_allow_local_files`).
- **The Public Suffix List**, all of it (10,397 rules, with wildcards and exceptions), is
  `third_party/psl/public_suffix_list.dat`. `tools/gen_psl.js` compiles it into sorted
  tables in `src/proc/psl_data.h` (about 190 KB in each binary that uses `site.c`), which
  `site_of` binary-searches. Checked against a separate implementation of the list's
  algorithm on 41,590 hosts built from every rule: no differences. To update it,
  download the list again and rerun the generator.

**M3 (done):** every child of the coordinator runs sandboxed (section 6).

- **What each process gets:**

  | Process | Token | win32k | Dynamic code | Job |
  |---|---|---|---|---|
  | Tab | LPAC, a profile per process, low integrity | off | prohibited | 4 GB |
  | Storage | LPAC, low integrity | off | allowed | 512 MB |
  | Network | restricted, write-restricted, low integrity (LPAC since M4) | off | allowed | 2 GB |
  | UI | AppContainer, low integrity | on | allowed | no limit |

  A tab reads its fonts straight from `C:\Windows\Fonts`, which LPAC may read and list. It
  can't read the user's own fonts in `%LOCALAPPDATA%`.
- **Changes that made the sandbox possible:**
  - Children link statically, `libwinpthread` included.
  - `SHELL32` is loaded on demand rather than imported (it would load `user32`).
  - The tab no longer starts Winsock, which LPAC refuses.
  - The coordinator creates `browser_data\sites` and grants the storage service's
    AppContainer full control of it, then passes the path with `--root`.
- **Escape tests:** `tests/sandbox/escape_test.sh <build dir>` runs `cb_escape.exe`
  (`src/proc/escape_main.c`) in a tab's sandbox, among normal tabs of two sites, and passes:
  75 attempts, none got through. It tries:
  - **Files:** to read and create files outside its folder (the user's profile, temp,
    `browser_data`, the executables' folder, `C:\Users\Public`, `C:\ProgramData`, the SAM
    hive), to list the user's folders, and to write to the registry.
  - **Sockets and processes:** to open sockets, to start processes (inside and outside its
    job), to open every other process with every dangerous right, to take over another
    process's threads, and to debug.
  - **Privileges and memory:** to enable privileges, to make executable memory, and to
    commit more than its job allows.
  - **Storage:** other sites' files and its own cookie jar, through 30 kinds of path: `..`,
    absolute and UNC paths, 8.3 names, alternate data streams, device names, trailing dots,
    a junction and a hard link the test plants in its folder. Also a write over its quota.
  - **Network:** HttpOnly cookies through `document.cookie`, another site's cookies through
    `document.cookie` and through requests, and `file:` URLs.
  - **Windows and the GPU:** to load `user32`, make a window, open the clipboard, and create
    a D3D11 device.
  - **Malformed messages:**
    - 3,000 well-framed requests with lying lengths to each server, which must still answer
      afterwards;
    - broken framing, and messages that claim 256 MB and never arrive;
    - scribbling on the rings' indices in shared memory;
    - impossible frames, a 16 KB title and `file:`/`javascript:` navigations to the UI.

  Afterwards the script checks that:
  - the network process and the storage service lived;
  - the next tabs' `localStorage` counter and cookies carried on;
  - no file was planted in the other site's folder;
  - no tab was started for a non-http URL;
  - a cookie the hostile tab set for the other site never reached that site's requests;
  - the UI lived;
  - no tab profile outlived the browser.
- **Found and fixed by the escape tests:**
  - A tab could read and write its own site's `cookies` file through the storage service,
    so it could read its HttpOnly cookies. Tabs may no longer name it.
  - The path grammar allowed a trailing dot, which Windows drops (`cookies.` names
    `cookies`), and DOS device names (`nul`, `com1.txt`). Both are refused now.
  - Tab profiles were first one per slot, reused by the next tab of that slot, so a page
    could leave files in the profile's folder for another site's page. Each process now
    gets its own profile.
  - Children started with `CREATE_NO_WINDOW` still got a console host process, which the
    tab could open. They start detached now.
  - `big_recv` allocated a message's full announced size (up to 256 MB) on its first
    part. A tab could announce a huge message and stop, and push the storage service past
    its 512 MB job limit, and the browser ends when a service does. The buffer now grows
    with what arrives.
- **Costs:**
  - Starting a tab takes about 10 ms longer, for registering and deleting its profile
    (ten tabs: 10.70 s against 10.60 s unconfined).
  - A blank tab's memory is unchanged: 48.5 MB private either way.
- **Tests:**
  - The replays load the same through the sandboxed processes: Wikipedia 42,606 DOM nodes,
    the YouTube watch page 7,471.
  - Replays need fresh `browser_data`. Wikipedia keeps its modules in `localStorage`, so
    after a live visit it asks for module versions the recording doesn't have.
  - `proc_tests` (87 checks), the unit tests, the JIT tests and `chan_bench` pass.
- **Not covered yet:**
  - The UI is unconfined in scripted tests.
  - Display lists with other tabs' resource ids wait for the GPU process (M5).
  - CFG needs another toolchain.
  - The network process can still read what the user can (section 6.4). Done in M4: it runs
    in LPAC now.

**M4 (done):** the verified cores are proved (section 8.6), the protocol is model-checked,
and the network process runs in LPAC, reaching loopback through the relay (section 6.4).

- **Proofs:** `tools/prove.sh` proves 2,748 goals in the five cores (Frama-C 33.0 with
  Alt-Ergo, Z3 and CVC5) in about 8 minutes, and TLC checks the protocol model in about 9.5: no violation in 13.0
  million states (safety, up to 3 tab processes) or 141,505 (liveness, up to 2).
- **The relay:**
  - `cb_loop.exe` (`loop_main.c` around `loop_core.c`), with a fully restricted,
    low-integrity token;
  - the network process's side is `loop_client.c`, which the TLS client uses for loopback
    hosts.
- **What each process gets now:**

  | Process | Token | Can read the user's files |
  |---|---|---|
  | Tab | LPAC, a profile per process | no |
  | Storage | LPAC | `browser_data/sites` only |
  | Network | LPAC, network capabilities | only a replay or recording folder it is given |
  | Loopback relay | restricted (every access), low integrity | no (system files only) |
  | UI | AppContainer | no |

- **Found and fixed:**
  - Tab profile names counted the tabs started before them, which a tab can read (section
    8.6). They are random now.
  - The relay's token was planned as write-restricted, which still reads the user's files. A
    spike showed a token restricted for every access still runs Winsock and connects to
    loopback, so the relay reads only system files.
- **Tests:**
  - The escape tests pass: 75 attempts, none got through, and the services survived. Their
    pages on `localhost` and `127.0.0.1` now load through the relay.
  - A live HTTPS page (Wikipedia) loads through the LPAC network process, certificates
    checked.
  - The replays load the same through the sandboxed processes: Wikipedia 42,606 to 42,608
    DOM nodes (the same spread with the sandbox off), the YouTube watch page 7,471.
  - `proc_tests` (114 checks, the relay core's among them) and `chan_bench` pass. The rings'
    new arithmetic costs nothing measurable: 4.1 µs round trip, 1.2 GB/s for 16 KB messages.
- **Not covered yet:**
  - Which public suffix `site_of` picks is tested, not proved (section 8.6).
  - P6's two-run harness waits until tabs send the coordinator requests.
  - The shims, `chan.c`'s reads of the shared counters, and the network process stay
    trusted (section 8.4).
  - There is no CI to run the proofs on every change.

**M5 (done):** tabs no longer rasterize. They send display lists to the GPU process
(`cb_gpu.exe`), which decodes them with a proved decoder and draws them with Vulkan, or with
the CPU rasterizer where there is no usable GPU (section 5.5.1).

- **Rendering matches the CPU path.** Each replay was screenshotted through the GPU process
  twice, on the CPU rasterizer and on Vulkan (an RTX 4070 SUPER). No pixel differs by more
  than 10%:

  | Replay | Pixels differing (of 1,152,000) | By more than 10% |
  |---|---|---|
  | YouTube watch page | 88 | 0 |
  | YouTube home | 1,322 | 0 |
  | YouTube search | 12 | 0 |
  | BBC News | 0 | 0 |
  | Wikipedia | 18 | 0 |

  The first Vulkan run differed in one image row and column. Vulkan allows division 2.5 ulp
  off, so `floor()` of a texel coordinate landing on an integer picked the neighbouring
  texel. One `fma` correction step in the shaders (`div_`) fixed it.
- **The CPU fallback works** (`CYBER_GPU=cpu`, or no Vulkan), and so do restarts. With the
  GPU process made to exit after 4 frames, the page came back after the restart identical to
  before. Made to exit on every frame, the browser survived 48 restarts and switched to the
  CPU rasterizer at the fourth start within a minute.
- **The proofs:** the decoder, 1,263 goals (P1, P5, P7). The coordinator core, re-proved
  with the GPU's grants and restarts, 4,898 goals. All six cores and the model checks take about an hour from a cold cache.
- **The escape tests pass:** 11 of 11 checks, none of the attempts got through. The hostile
  tab now also attacks the GPU process:
  - 107 frames naming image ids and font slots it never uploaded (dropped: another tab's
    namespace is out of reach);
  - lying counts, extreme coordinates, 2,000 unbalanced mask brackets, masks of no kind;
  - uploads past the end of an atlas, and 1.5 GB of images (past 1 GB, dropped);
  - broken framing, and scribbling on the draw channel's shared memory.

  The GPU process kept drawing for it, and sent it nothing but "drawn".
- **Memory,** with a blank tab and the Wikipedia replay loaded:
  - a blank tab is 45.8 MB private, down from 48.5: it no longer rasterizes;
  - the GPU process is 298 MB private. That is the driver, and two copies of each tab's
    resources: textures, and CPU copies for the fallback rasterizer. Dropping the CPU copies
    when Vulkan works would save most of the second.
- **Tests:**
  - `proc_tests`: 122 checks, the decoder's among them.
  - The unit tests and the JIT tests pass.
  - The replays load as before: Wikipedia 42,608 DOM nodes, the YouTube watch page 7,471.
- **Not covered yet:**
  - The UI's own interface is still GDI, in the UI process; only page content goes through
    the GPU process.
  - Tiles, layers and compositor scrolling wait for M6.
  - No WebGL or WebGPU (M8).
  - The Vulkan rasterizer waits for each frame (one command buffer). Overlapping a frame's
    drawing with the next one's recording is a later optimization.

**M6 (done):** several windows, tabs dragged out of and between them, and compositor
scrolling (sections 5.4.1 and 7.4). A tab dragged while its video plays keeps playing, in
the same process, without a reload.

- **The test** (`tests/ui/window_test.sh <build> [replay root]`) drives the UI's test
  script and checks its screenshots:
  - **Scrolling while the page's JS is busy** for 4 s: 200 ms after the scroll, the window
    shows the old content moved by exactly 200 px. The tab's own frame comes when its JS
    is done.
  - **A page whose frames keep coming,** dragged with the mouse handlers into a window of
    its own and back. The frames keep changing in every window, and no tab process starts
    again.
  - **With the replay root, the YouTube watch page** while its video plays, dragged out and
    back. In each 1.5 s between screenshots, 69 to 75% of the video's pixels change, and
    the tab's process stays the same.

  It passes on Vulkan and on the CPU rasterizer. It also passes with the GPU process
  restarting during the drags (`CYBER_GPU_CRASH_TEST=12`): every window gets a new
  content window and shows its tab again.
- **Found and fixed on the way:**
  - **No video on the YouTube watch page,** at least since M5. A custom element with no
    display rule (`ytw-player-with-controls`) holding block children is laid out as a
    block (block-in-inline). The player's `height: 100%` resolved against that block's
    auto height, 0, so the video was clipped away. Percentages inside such a box now
    resolve against the block around it, as browsers do (`layout_pct_base_height`). Of
    the 17 recorded sites, the change reaches Apple's `<picture>` and an SVG on Google,
    and both render byte for byte as before. Only the YouTube player changes.
  - **A tab's localStorage writes stalled for 10 s,** freezing the page, since M5.
    `spawn_child` passed at most 14 inherited handles. With M5's draw channel a tab has
    15, so the storage channel's second event never reached the tab. The storage service
    then woke only when the network process wrote to it. The cap is now 24, and a spawn
    asked for more fails.
- **The replays** still render the same on Vulkan as on the CPU rasterizer. No pixel differs
  by more than 10%. The YouTube watch page now differs in 13,831 pixels, all slight, because
  its video shows.
- **The proofs:** the decoder reads the header's first five numbers as before and skips the
  new five (`gpu_main.c` reads those), 1,265 goals. Every other core is unchanged.
- **The escape tests pass,** 11 of 11. The hostile tab now also sends frames placing its
  layer anywhere at all (2^32 - 1, 0, 2^31), with a frame the UI believes so that they're
  presented. The GPU process clamps them.
- **Tests:** `proc_tests` 123 checks; the unit tests (449) and the JIT tests pass.
- **Not covered yet:**
  - One layer per tab, half a viewport beyond each edge. A fling past that shows white
    until the tab draws. `position: fixed` moves with a compositor scroll until the next
    frame.
  - The video repaints twice a second (the engine's pace for media, unchanged).
  - The UI's tab strip is still GDI, and there are no focus events.

**M7 (done):** frames from other sites, ORB, CORS and Private Network Access (section 9.1).

- **The test** (`tests/sites/sites_test.sh <build> [port A] [port B]`; 127.0.0.1 and
  localhost are two sites) checks 19 things:
  - **Frames.** A frame from another site:
    - loads, with its own localStorage, stored under the page's site;
    - can't reach its parent's document or element, nor the page the frame's;
    - talks to the page through `postMessage` both ways, with the right origins;
    - doesn't get a message meant for another origin.

    The frame's site, opened as a page of its own, sees none of the frame's storage or
    cookies. An `about:blank` frame made by script is the page's own origin.
  - **ORB and CORS.** ORB blocks JSON and HTML read across sites and lets scripts
    through. A cors read without `Access-Control-Allow-Origin` fails; with it, it works.
  - **PNA.** On a port made public, a page can't fetch from a loopback address, nor frame a
    page there, but fetches from a public one.
- **The escape tests pass,** 11 of 11 checks and 78 attempts. The hostile tab now also tries
  to read another site's page as a no-cors subresource, in that site's name, and with
  cors. All three are blocked. It can read the page by claiming a frame document, which the
  test notes, as section 9.1 says.
- **The proofs:** the network policy core, 1,873 goals (P1, P7, P9). WP took the enum's
  constants as unsigned, so `client < NET_SPACE_PUBLIC` was never true for it: the
  comparisons are now `int`.
- **The replays:** the YouTube watch page still plays and drags (`window_test.sh`).
  Its frames now load: an `about:blank` one, and a sign-in one that isn't in the
  recording. Loading them costs nothing measurable. The A/B was run in one build, with a
  temporary switch turning frame loading off (`tests/jit/replay.sh`, 5 runs each, median
  CPU cycles):

  | Replay | Frames off | Frames on |
  |---|---|---|
  | YouTube watch page | 10,921 M | 10,582 M |
  | YouTube home | 4,932 M | 4,886 M |
  | YouTube search | 13,439 M | 12,672 M |
- **Tests:** `proc_tests` 147 checks (24 for the policy core); the unit tests, the JIT tests
  and the window tests pass.
- **Not covered yet:**
  - Frames aren't painted (same-origin ones weren't either).
  - Messages are JSON clones, not structured clones, and `MessagePort`s can't be
    transferred across realms.
  - A frame can't navigate the page (`top.location = ...`): the stand-in ignores it.
  - A frame's `about:blank` child keeps localStorage in memory only.

**M8 (done; the conformance level is WebGL 1's first run, below, and the WebGL 2 suite and
the WebGPU CTS are still to run):** the web-GPU process: WebGL 1 and 2 on ANGLE,
WebGPU on wgpu-native, on the GPU (section 5.7.2).

- **The test** (`tests/webgpu/webgpu_test.sh <build> [port] [fuzz batches]`) checks 46
  things:
  - **WebGL 1 and 2.**
    - A triangle is drawn and read back.
    - Uniforms, bindings, textures, vertex arrays, buffers read back, and syncs work.
    - Errors come for out-of-range writes.
    - Another context's objects are refused.
    - The extensions offered include no GPU timers.
  - **WebGPU.**
    - An adapter with WebGPU's limits, and no timestamp queries.
    - A compute shader runs, and a triangle is drawn into a texture and read back.
    - Validation errors reach error scopes.
    - 30 frames go to a canvas.
  - **On the screen.** Both canvases appear, orange inside and blue outside.
  - **Crash containment.** A web-GPU process that ends after 10 frames
    (`CYBER_WEBGPU_CRASH_TEST`):
    - the tab hears at once (the coordinator tells tabs whose process ended);
    - the context is lost and `webglcontextlost` fires;
    - the page lives on;
    - the next page draws with a new process;
    - nothing else ends.
  - **The block list.**
    - A blocked driver has neither API (`getContext` returns null, the adapter is null).
    - A denied feature (compute) is refused as a validation error. The command buffer
      using it is invalid, so none of it runs.
  - **Process lifetime.** The site's last tab gone ends its process; the next tab of the site
    gets a new one.
  - **Fuzzing,** against the real libraries (`cb_webgpu_fuzz`):
    - `gl`: random WebGL records, shaped by each op's length;
    - `gpu`: random WebGPU records and value trees;
    - `gpuseq`: well-formed calls on real objects, used right and wrong.

    After each batch, a clear must read back. Long runs: `gl` 7,000 batches; `gpu`
    11,000; `gpuseq` 13,000 over four seeds (each run stops after about 3,000, when
    wgpu-native's leaks have used up device memory).
- **The fuzzer found eight ways a page could abort the web-GPU process** through
  wgpu-native. Each is now refused as a validation error (section 5.7.2):
  - an unknown or missing enum;
  - a missing module or view;
  - an error scope popped that was never pushed;
  - the mapping of an invalid buffer;
  - a second adapter handle;
  - a submit using a mapped or destroyed buffer;
  - a command buffer encoded wrong;
  - a submit to a destroyed device.

  It also found two leaks of device memory, which it then worked around. The cases left
  are bounded by the process's life.
- **The escape tests pass,** 11 of 11 checks and 79 attempts. The hostile tab now also:
  - asks for impossible contexts;
  - presents canvases it never made;
  - sends 2,000 batches of lying WebGL records, a value tree 3,000 deep, and 1,500
    batches of lying WebGPU records;
  - then breaks the channel.

  The web-GPU process draws afterwards, and never ends.
- **The proofs:** the coordinator core with its web-GPU table, 1,091 properties (P3,
  `webgpu_ok`). Its proofs had started to time out: each write of an action now goes through a
  small helper that promises what it leaves unchanged. The TLA+ model gains web-GPU processes:
  - each serves only live tabs of its own site, and never opaque's (`WebgpuClientsTrue`);
  - one per site (`OneWebgpuPerSite`);
  - a tab waiting for one is answered or dies (`WebgpuAnswered`).

  These are checked on the smaller live model (2 tab starts, 1 web-GPU start). The larger
  safety model runs with web-GPU off, since together they pass 40 million states.
- **The replays:** an M7 build against this one (`tests/jit/replay.sh`, 5 runs each, CPU
  cycles; the machine was busy with the proofs during runs 2 to 5, so rounds are compared
  in pairs):
  - **YouTube home and search:** no change. Within ±4% per round, and 0.1% more JS calls.
  - **YouTube watch page:** 5 to 8% more cycles and 5.7% more JS calls. Its script reads
    `WebGLRenderingContext`, which now exists, and it then takes a longer path, as it
    would in Chrome. Installing the APIs costs about 7 ms. Pages that never touch them pay
    nothing beyond the stand-ins.
- **The conformance suites** (`tests/conformance/conformance_test.sh`, run against shallow
  clones of the Khronos WebGL suite and the WebGPU CTS). The browser can't yet run their own
  harnesses: a script can't navigate (`location.href = ...` only updates the object), and a
  page keeps its URL from before a redirect. So the UI script opens each page by its URL, and
  a small server takes the counts each page posts.
  - **WebGL 1 (1.0.4), the level M8 sets:** 568 of 971 pages pass every check, and 38,186 of
    55,452 checks pass (69%). 107 pages never answered (76 timed out, 31 crashed).
  - **The fixes the suite found:**
    - `<script>.text` and `.src`;
    - XHR's `overrideMimeType`, and relative URLs in XHR (they weren't resolved);
    - `bufferData` with null or a non-buffer argument;
    - binding one buffer for indices and for vertices (refused, as WebGL requires);
    - `FLIP_Y` on typed arrays with unpadded last rows, and the 16-bit packed types;
    - `texImage2D` from a WebGL canvas.

    The XHR fix was made after that run. Of the failed checks, about 2,400 were on the
    OpenGL ES ports, which load their shaders by relative XHR; a sample of them now passes
    every check.
  - **What fails most:**
    - textures from a 2D canvas: the 2D context doesn't rasterize, so they come out
      transparent black;
    - context loss and restore;
    - video sources;
    - some extensions (float and half-float textures, instancing's errors).
  - **Not run yet:** the WebGL 2 suite (2,865 pages, with dEQP) and the WebGPU CTS (693 spec
    files). The runner covers both.
- **Tests:** `proc_tests` 164 checks (17 for the web-GPU flows of the coordinator core). The
  unit tests, the JIT tests, the sites tests and the window tests pass. In the window
  tests, the one exception is dragging a tab back into its first window: it doesn't move.
  That code is unchanged, and the drop depends on where windows are on the desktop, but it
  is not checked against M7 here.
- **Not covered yet:**
  - **The WebGL 2 suite and the WebGPU CTS haven't been run,** and WebGL 1 passes 69% of its
    checks (above). The level is set at that run; raising it is correctness work for later.
  - Canvases reach the screen as a copy through the tab, not as textures shared with the
    compositor. That costs a readback and an upload per presented frame.
  - WebGPU's promises resolve when the web-GPU process answers. That process waits on the
    GPU, so a long `mapAsync` holds the tab's JS thread.
  - `copyExternalImageToTexture` and `texImage2D` take `<img>`, `<video>`, `ImageData` and
    `ImageBitmap`-like objects. `texImage2D` also takes a WebGL canvas, and a 2D canvas
    (which comes out blank). `copyExternalImageToTexture` takes no canvas yet.
  - Missing for now: query sets and render bundles; external textures;
    `getCompilationInfo` messages (shader errors come as validation errors).
  - wgpu-native still leaks a device's memory in a few cases a page can bring about, a submit
    refused while a buffer is mapped among them. They end with the site's process.
  - The record decoders are fuzzed, not proved (section 5.7 plans that next).

**M9 (done but for the proofs' and Eva's final runs):** the shims around the cores (section
8.8).
- **Proofs:** eight targets new or extended (section 8.8's table), about 21,000 goals. `chan`
  was re-proved on the merged code; the others as each part was built (`store` unconfirmed).
- **Tests:** on the merged build, with the machine quiet:
  - `proc_tests`: 319,610 checks;
  - the escape tests: 14 of 14 checks, 81 attempts, 0 escaped. The hostile tab also tries
    cross-site redirects of its own navigation and floods the network process with replies it
    never reads;
  - the sites tests, including where pages land after redirects;
  - the web-GPU tests (1,000 fuzz batches);
  - the window tests, including dragging a tab back into its first window, which failed in
    M8;
  - the unit tests.
- **Conformance:** with the M8 fixes, WebGL 1 passes 41,955 of 53,952 checks (77.8%), and 725
  of 971 pages pass every check. On a sample of 67 pages, the stricter record grammar changed
  no result.
- **The replays** weren't re-measured for M9.
- **A note on WSL:** Eva runs with high precision can take several GB each; three at once
  filled WSL's memory and hung it, and the hung VM slowed the web-GPU process's adapter
  requests from 1 s to 30 s. `PROVE_JOBS` stays at 3 or less for `eva`.

**Not in M1** (later milestones, or not yet planned):
- Typing into the page (forms, text fields).
- `scroll` events and `window.scrollY` for the page.
- `position: fixed` that stays put while scrolling.
- Script-initiated navigation (`location = ...`).
- Back and forward.

**Escape tests** (done in M3 except where the GPU process is needed). A hostile tab tries, from inside the
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
- **Undocumented corners of the Windows sandbox.** Each is checked with a small spike before
  the milestone that needs it. M3 settled two: AppContainers need registered profiles (one
  per tab process, section 6.1), and win32k lockdown works with our runtime libraries once
  they are linked statically. M5 settled the third: a swap chain on another process's window
  needs both processes in one AppContainer (section 5.7.1).
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
