# M9 notes: Eva on the shims (SANDBOX_DESIGN.md 8.7.3, step 5)

Step 5 of the M9 plan: Frama-C's Eva on each process's whole shim, with
stubs for the Windows calls; every alarm fixed or explained.  This is the
setup, run on the M8 shims (commit c1d22fc).  Nothing under `src/` was
changed; the alarms below are for the shims' authors to fix or explain.

## Running it

    wsl -e bash -lc 'cd /mnt/c/<worktree>/cyberbrowser && PROVE_OUT=$HOME/cb_prove_eva bash tools/prove.sh eva'
    ... tools/prove.sh eva-chan     # one process: eva-chan eva-loop eva-store eva-coord
                                    #   eva-gpu eva-webgpu eva-ui eva-net

`eva` runs the eight analyses in parallel (`PROVE_JOBS` at a time, default
`nproc`; each Eva is one thread and takes 1-4 GB, so on a 16 GB machine use
`PROVE_JOBS=4`).  `EVA_PRECISION` (default 2) sets `-eva-precision`.  Per
process it writes `$PROVE_OUT/eva-<name>.log` (Eva's output),
`.csv` (every property and its status, `-report-csv`), `.alarms` (the
alarms, one key per line) and `.summary`.  It fails if Eva stops (a
degenerated analysis, a call to a function with neither code nor
specification, a recursion it can't unroll) or if an alarm is not listed in
`proofs/eva/<name>.explained` or `proofs/eva/common.explained`.  It also
lists explained entries that are no longer reported, to remove.

`eva` is not in `all`: see the run times below.

### What counts as an alarm, and the explained lists

An alarm is a row of the report whose status is Unknown or Invalid and that
is either a run-time check Eva added (memory access, initialization,
dangling pointer, overflow, index, Boolean value, pointer comparison,
function pointer, float...) or a callee's precondition at a call site
("precondition of X").  The key is `file:function: kind: property`, without
the line number, so that a key survives edits elsewhere in the file and one
key covers each place in the function with that property.

Alarms inside the proved cores (`chan_ring.c`, `coord_core.c`,
`loop_core.c`, `store_core.c`, `site.c`, `gpu_decode.c`, `net_policy.c`) are
not counted: WP proves P1 for them for every input meeting their
preconditions, and Eva checks those preconditions at the shims' calls (those
alarms are counted).

`*.explained`: comment lines (`#`) give the reason for the keys after them.
Each group says IMPRECISION (it can't happen; the comment is the argument),
LATENT (can't happen with today's callers; worth a fix) or MODEL (a limit
of the drivers or models, said there).  Alarms judged real bugs are not
listed, so `tools/prove.sh eva` fails until they are fixed.

## How it is set up

| File | What |
|---|---|
| `proofs/eva/machdep_mingw_x86_64.yaml` | The target: MinGW-w64 x86_64, GCC, LLP64 (`long` and `DWORD` 32 bits), 16-bit `wchar_t`, `_WIN32`/`_WIN64`/`__MINGW64__` defined.  Generated from Frama-C 33's `machdep_gcc_x86_64.yaml`.  The WP runs keep `-machdep x86_64` (the cores use only fixed-size types). |
| `proofs/eva/include/` | First on the include path, so `<windows.h>`, `<winsock2.h>`, `<ws2tcpip.h>`, `<windowsx.h>`, `<aclapi.h>`, `<userenv.h>`, `<sddl.h>`, `<bcrypt.h>`, `<basetsd.h>`, `<direct.h>`, `<io.h>`, `<intrin.h>` are these.  `windows.h` (kernel, files, processes, jobs, security, registry, text, CRT extras), `fc_winuser.h` (user32, gdi32), `fc_winsock.h` (Winsock), `fc_atomic.h` (GCC's `__atomic_*_n`, which Frama-C 33 lacks, as plain accesses). |
| `proofs/eva/win_models.c` | Bodies for the few functions a contract alone can't give Eva (below). |
| `proofs/eva/chan_ring_model.c`, `loop_core_model.c` | Proved cores run as their contracts (below). |
| `proofs/eva/lib_net.c`, `lib_gpu.c`, `lib_webgpu.c` | Models of the libraries outside M9. |
| `proofs/eva/drv_*.c`, `drv.h` | One driver per process. |
| `proofs/eva/*.explained` | The explained alarms. |
| `tools/prove.sh` | Targets `eva`, `eva-<process>`. |

Options: `-eva-precision 2 -eva-split-return auto
-eva-unroll-recursive-calls 12 -eva-plevel 300 -eva-auto-loop-unroll 64
-eva-warn-undefined-pointer-comparison pointer` (`EVA_UNROLL`, `EVA_FLAGS`
override).  `-eva-plevel 300`: the UI writes through `Tab *t` / `Win *w`
that may be any of 60 tabs; at the default (40) Eva approximates those
writes over the whole array and the UI run degenerates into ~870 alarms.
Unrolling 64 keeps the 60- to 63-entry wait-handle loops exact.
`-eva-precision 3` was tried on loop: over 10 minutes against 83 s, so 2.  Allocation failure is considered (no `-eva-no-alloc-returns-null`):
every `malloc`, `calloc` and `realloc` may return NULL, and so may the
models' allocations.  `-eva-unroll-recursive-calls 12` follows the shims'
two bounded recursions (`loop_main.c` `step` through `perform` and
`event_slot`, depth 5; `store_main.c` `folder_bytes`, depth 10); a recursion
deeper than that stops the run.

### The drivers

Each `drv_<name>.c` includes the process's main file with `main` renamed
(`#define main loop_main` / `#include "loop_main.c"`), so no source changes,
and calls it with a command line from `drv.h`: one to eleven words, each the
word a well-formed line has there (`--chan`, `--root`, `--site`...) or any
string, of any length up to a cap (8 KB).  Eva keeps each argument count
apart (`//@ split argc`).  Everything else a process reads comes through the
models: channel bytes (unknown: `MapViewOfFile` below), environment
variables (any string or none: `getenv`), file contents, window messages.

Eva follows one thread.  `CreateThread` and `RegisterWaitForSingleObject`
run nothing.  The threads' functions are driven explicitly:
- `cb_net` (`drv_net.c`): (0) `main`; (1) what main sets up, any number of
  grants (as the coordinator's channel delivers them, any bytes), then the
  reader thread of one of the clients they made, until it ends, then a
  worker, which takes the requests that reader queued; (2) the relay client
  (`drv_net_loop.c`, which includes `loop_client.c`): its reader thread, or
  the TLS code's calls through `TlsLoopback` with any host, port and sizes.
  Not covered: interleavings (a revoke while a reader or worker uses the
  client, which the reference count is for; a worker running before its
  reader ends), so data races are outside what this shows.
- `cb_coord`'s `on_exit` callback only calls `SetEvent`.
- `cb_ui`'s window procedures: `DispatchMessageW` calls one of those
  registered (`RegisterClassW`, `SetWindowLongPtrW`) with the message
  `PeekMessageW` gave, which is any message.  Messages Windows sends during
  other calls (`CreateWindowExW`, `SendMessageW`, `DestroyWindow`) are not
  followed: they are recursive, which Eva can't follow, and their contents
  are among those `DispatchMessageW` already delivers.

### The Windows contracts: what they assume (trusted, section 8.4)

Every contract in `proofs/eva/include` is an assumption, part of the trusted
base; they follow Microsoft's documentation.  In summary:
- **Buffers.**  Each call requires what it reads or writes to be valid for
  the size passed, in the API's unit (bytes or characters), and strings
  NUL-terminated where it reads strings.  It writes at most there.  Counts
  it returns are bounded by the room given (`ReadFile`'s `*read <= n`,
  `recv`/`send` `<= len`, `GetWindowTextW` `< nMaxCount` and terminated,
  `MultiByteToWideChar` `<= cchWideChar` and terminated when the input was).
  Where the API returns the size it needed instead (`GetEnvironmentVariableW`,
  `GetFinalPathNameByHandleW`, `GetModuleFileNameW`), the buffer is a
  terminated string only when the result is less than the room.
- **Results.**  Any call may fail: handles may be NULL (or
  `INVALID_HANDLE_VALUE` for the ones that use it), Booleans false, counts
  0.  Waits return one of their documented values
  (`WaitForMultipleObjects`: an index below `nCount`, an abandoned index,
  `WAIT_TIMEOUT` or `WAIT_FAILED`), and require 1 to 64 initialized handles.
- **Effects.**  A call that changes the system (processes, files, events,
  the registry, windows) assigns `__fc_win_state`, which the results depend
  on: nothing a shim learns from Windows is known to Eva.
- **Preconditions the shims' uses pin down**: `DuplicateHandle` with no
  target handle only with no target process; `CreateProcess*W` with a
  writable command line under 32,767 characters and, with
  `EXTENDED_STARTUPINFO_PRESENT`, a `STARTUPINFOEXW`;
  `UpdateProcThreadAttribute` reading `cbSize` initialized bytes;
  `SetInformationJobObject` with the class's exact size; `SetDIBitsToDevice`
  reading `cLines` rows of the bitmap's padded stride; ACL calls within the
  ACL's own `AclSize`; `inet_pton` writing 4 or 16 bytes.
- **Simplifications** (sound for the shims as written): `ReadFile` is taken
  to set all `n` bytes (Windows sets `*read` of them; Eva can't express the
  difference, so a read of bytes past `*read` isn't flagged);
  locks and condition variables need only valid, initialized memory
  (threads aren't modeled); `Interlocked*` are plain updates.

Bodies (`win_models.c`), each doing what its contract says:
- **`MapViewOfFile`**: a fresh block of exactly the size asked, or NULL;
  `UnmapViewOfFile` frees it (so a use after unmapping is an alarm).  Its
  bytes are the peer's: unknown.  Eva can only make a range of known size
  unknown, so a view of 4096 bytes or more has its first 4096 unknown (every
  header the shims read: `ChanShared` is 448 bytes) and the rest reads as
  0; past the header are the rings, which only `chan_ring_put` and
  `chan_ring_take` touch, and those run as their contracts (any message).
- **`_snwprintf`, `fwprintf`** (msvcrt): a small formatter that reads each
  `%ls`/`%hs`/`%s` argument as a string (so an unterminated one is an
  alarm), consumes numeric arguments by their length modifier, writes at
  most `count` characters, and is Microsoft's on truncation: no NUL and -1
  (or `count` when the text is exactly `count` long).
- **Allocated results**: `ConvertSidToStringSidW`,
  `ConvertStringSecurityDescriptorToSecurityDescriptorW`,
  `GetNamedSecurityInfoW` (descriptor; the DACL points into it),
  `SetEntriesInAclW`, `CreateAppContainerProfile`,
  `DeriveAppContainerSidFromAppContainerName`, `CreateDIBSection` (pixels:
  `|height|` rows of the 4-byte-padded stride): `malloc`'d, so `LocalFree`
  and `FreeSid` (`free`) check they get one of them.
- **`GetProcAddress`**: NULL, or for `"DeriveCapabilitySidsFromName"` a
  model of kernelbase's function (arrays of 0 to 2 SIDs, each to
  `LocalFree`).
- **Window procedures**: `RegisterClassW` and `SetWindowLongPtrW`
  (`GWLP_WNDPROC`, returning an edit control's procedure) remember them;
  `DispatchMessageW` calls one.
- **`getenv`**: NULL, or any string (Frama-C's own model returns strings
  Eva can't follow).

### The proved cores

`coord_core.c`, `store_core.c`, `site.c`, `gpu_decode.c` and `net_policy.c`
run as they are: Eva computes what they return, and checks the shims' calls
against their preconditions.  Two run as their contracts instead
(`*_model.c`), because the shim depends on what their postconditions
promise and Eva loses it through their bodies:
- `chan_ring_put`/`chan_ring_take` (chan_ring.h): any outcome the contract
  allows; a message taken has a length of at most `CHAN_MSG_MAX`.  Their
  contracts have no `\from`, which Eva needs anyway.
- `loop_init`/`loop_step`/`loop_want_read` (loop_core.h): any state keeping
  `loop_ok`, any actions `act_ok` allows (a slot for each socket action, a
  loopback family and port, the lengths).
That is sound because WP proves the real functions meet those contracts
(P1, P2, P7, P8).

### Libraries outside M9 (trusted models)

- `lib_net.c`: `platform_init`, the cookie jar (`http_cookie_jar_*`: the
  model requires a valid jar where the real code dereferences it without a
  check), `http_cookie_get/set_for_document`, `http_request_to_memory`
  (requires valid strings and buffers; fills the `HttpBuffer` with malloc'd
  NUL-terminated blocks; on the way it may call the shim's address
  classifier with any address and the relay's functions through
  `TlsLoopback`), `http_last_url`, `http_free_buffer`, `tls_*`,
  `wurl_parse`/`wurl_origin`/`wurl_free`.
- `lib_gpu.c`: the CPU rasterizer (`raster_display_list_with`: requires the
  list's arrays and `rgb` of `w*h*3` bytes; calls the shim's `atlas` and
  `image` callbacks for each glyph, image and mask the list names and reads
  the `w*h*channels` bytes each returns) and the Vulkan backend (`gpu_vk_backend`
  returns NULL or a table whose functions check their buffers against the
  texture's and target's sizes: `texture_update` rows within the texture,
  `readback` writing `w*h*4`).
- `lib_webgpu.c`: `webgl_*` and `wgpu_*`: record bytes read, the output
  buffer grown with `realloc` and its length within its capacity, the
  frame's `w*h*4` bytes written, contexts allocated and freed.  The record
  walkers themselves are step 3's work.

## Results on the M8 shims (incomplete: WSL stalled)

The runs below were cut short.  Eight analyses running together with
three exploratory runs of mine (`-eva-alloc-builtin fresh` and
`-eva-domains octagon` on chan, which never converge and grew to 2-4 GB
each) filled the WSL VM's 15 GB, and WSL stopped answering.  The fresh
runs, not Eva on the shims, were the cause.  Don't use `fresh` here, and run
`eva` with `PROVE_JOBS=3` or fewer on a 16 GB machine.

| Process | Status | Flags | Time | Alarms |
|---|---|---|---|---|
| chan (chan.c, bigmsg.c) | **final**, passes | current | 6 s | 17, all explained (`common.explained`, `chan.explained`) |
| loop | **final** | current | 83 s | 21; explained in `loop.explained` (not rerun after writing it) |
| gpu | early flags | no plevel, unroll 32, old models | 568 s | 387 |
| webgpu | early flags | same | 307 s | 166 |
| net | early flags | same | 5,488 s (thrashing) | 291 |
| ui | early flags | same; degenerated (plevel) | 5,054 s (thrashing) | 870 |
| store | running with the current flags when WSL stalled | | | none |
| coord | running with early flags when WSL stalled | | over 80 min | none |

The early runs had: no `getenv` model (Frama-C's, whose strings Eva can't
follow: dozens of `valid_read_string` alarms on `getenv` results), a
`MapViewOfFile` whose bytes Eva saw as uninitialized when the view was
allocated in a loop (every `r->head`/`r->tail` initialization alarm in
chan.c), 4-byte-aligned handle structs (every `pointer_alignment` alarm on
HWND/HDC/...), no `loop_core` model, plevel 40 and unroll 32.  All of that
is fixed in the committed setup, so their counts are an upper bound.
Times under 15 minutes were not reached for gpu, ui, net or coord even
before the stall, so `eva` stays out of `all`.

### chan.c and bigmsg.c (every process)

All IMPRECISION or LATENT; see `common.explained` for each argument:
- ring preconditions (`ring_size_ok`, `\valid(ring ...)`) at
  `chan_send`/`chan_recv`: Eva can't relate the view's size to ring_bytes.
- `chan.c` `chan_send`: `int spins` can overflow after about 2^31 waits (about 25
  days at 1 ms) with timeout INFINITE.  No caller passes more than 30 s.
  **LATENT: make it unsigned.**
- `big_send` memcpy (used + n <= CHAN_MSG_MAX), `big_recv` memcpy and
  `buf[total]` (cap >= have + n + 1): non-relational.
- `big_recv`/`big_free` `\dangling(&rx->buf)`: Eva's weak heap summary.

### loop_main.c

16 alarms beyond chan's, all IMPRECISION (`loop.explained`): the action's
kind and slot kept apart (perform/sock_* index `g_socks` by a slot that
`act_ok` bounds for socket actions), and the wait arrays' written prefix
merged across n.  No real bug found.

### Real bugs found (early runs, by reading the alarms)

All are allocation failures (Eva considers every allocation may fail):
- `net_main.c` `site_jar`: `Site *s = calloc(...)` is used unchecked
  (`snprintf(s->label, ...)`, `s->jar = ...`): NULL dereference on OOM.
- `net_main.c` `site_jar`/`save_jars`/`grant`: `http_cookie_jar_new()`
  (calloc) may return NULL, then `http_cookie_jar_version(jar)`,
  `http_cookie_jar_load`, `http_cookie_jar_save` dereference it
  (http_download.c reads `jar->...` unchecked).
- `net_main.c` `reader`: `Job *j = malloc(...)`, then `j->client = c` unchecked.
  (Section 8.7.2 already lists "its malloc unchecked".)
- `net_main.c` `frame_loaded`/`may_read`: `url_origin` returns
  `strdup("null")`, which may be NULL; then `strcmp(c->frames[i], o)` /
  `strcmp(o, initiator)` on NULL.
- `store_main.c` `usage_of`: `Usage *u = calloc(...)` unchecked
  (by reading; store's run didn't finish).

By reading, to confirm on the next run:
- `store_main.c` `make_root` and `coordinator.c` `make_root`:
  `_snwprintf(g_root, MAX_PATH, L"%lsbrowser_data\\sites", base)` (and
  `L"%ls\\sites"`) truncates when the executable's folder is longer than
  about 240 characters.  Microsoft's `_snwprintf` then writes no NUL and
  returns -1, and `g_root` is used as a string (`wcslen`, `to_path`'s
  `(size_t)_snwprintf(...)` would be `SIZE_MAX`).  The `_snwprintf` model
  is written to catch exactly this.
- `gpu_main.c` `ui_message` (UG_SHOT): `out` is sized with `c->width`, but
  `readback` returns the target's own width.  They differ if a backend's
  `render` resized the target and then failed (c->width is updated only on
  success).  Whether `gpu_vk.c` can do that needs checking.  Eva flags the
  `readback` precondition with the Vulkan model, which allows it.

### gpu, webgpu, net, ui (early runs): what the alarms are

Grouped from the early lists.  Each group's explanation is the same kind as
chan's.  None is in an `.explained` file yet, so those targets fail until
they are rerun and classified:
- **dangling_pointer** (most of them): Eva's weak heap summaries.  Clients,
  Sites, Jobs, LConns and rx buffers are allocated in the main loop and freed
  on revoke, and every free makes every pointer to any of them "maybe
  dangling".  Each free in these shims is followed by clearing the pointer
  (`g_clients[i] = NULL`, `*pp = c->next`, ...).  IMPRECISION, but each
  needs reading once per site.
- **message fields** (`initialization` of `q.*`, `a.*`, `t.*` after
  `memcpy(&q, m, sizeof q)`; memcpy src validity): the message buffer is
  `rx.buf`, realloc'd to a size Eva can't relate to `rx.total`, and filled by
  weak updates.  IMPRECISION, inherited from big_recv.  Step 1 (framing
  into a proved core with a contract) is what removes these.
- **gpu `to_display_list`, `frame`** (cmds/masks/dl arrays realloc'd to
  cmd_cap, indexed below n <= cmd_cap): non-relational, IMPRECISION.
- **gpu_decode preconditions at `frame`** (`\valid(cmds + (0 .. cmd_cap - 1))`):
  same.  The decoder's internal alarms are not counted (proved by WP).
- **webgpu `present`/`gl_run`**: through `*slot` (a WebglCtx from the
  model), same heap imprecision.  `g_frames + 1` signed overflow (`present`):
  LATENT, after 2^31 presented frames with `CYBER_WEBGPU_CRASH_TEST` set
  (test-only counter).
- **net `run_request`/`header_value`/`may_read`**: string and length checks
  over the request bytes (each field NUL-checked at `len - 1`): the
  imprecise message buffer again.
- **ui**: degenerated at plevel 40 (see Options).  Needs a rerun.

## Next steps

1. Restart WSL, then run `PROVE_JOBS=3 tools/prove.sh eva` (no exploratory
   runs alongside).
2. Classify the store, coord, gpu, webgpu, ui and net lists into
   `<name>.explained`.  Fix the real bugs above in the rewritten shims.
3. Likely high-value modelling next: an executable contract for big_recv
   (as for chan_ring) once step 1 gives it one.  That removes most of the
   message-field alarms in every server.
