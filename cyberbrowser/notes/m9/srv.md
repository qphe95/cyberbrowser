# M9 notes: the storage service, the GPU process's uploads, the UI, the relay

Part of M9 (SANDBOX_DESIGN.md 8.7): step 3 of 8.7.3 for the storage service, the GPU process
and the UI, and these findings of 8.7.2: the half-made atlas, storage's denial of service, the
storage service never dropping a broken network-process client, a tab opening 60 tabs through
`TU_NAVIGATE`, and the relay's `step` dropping events past depth 4.

## What was built

| Target (`tools/prove.sh`) | File | New proved code | Goals | Time |
|---|---|---|---|---|
| `store` | `src/proc/store_core.c` | `store_decode` (request grammar, decision, path) | **not confirmed**: see below | 2731 s (cold, first run) |
| `gpu_input` (new) | `src/proc/gpu_input.c` | atlas and image uploads, frame header, viewport | 749 / 749 | 110 s (cold) |
| `ui` (new) | `src/proc/ui_core.c` | `from_tab`'s decoding, the tab-opening rule | 237 / 237 | 20 s (cold) |
| `loop` | `src/proc/loop_core.h` | one more postcondition of `loop_step` | 892 / 892 (874 at M7) | 260 s (cold) |

Times are wall clock on 12 cores in WSL, with other agents' proofs running at the same time,
so they overstate what a quiet machine takes. Nothing is admitted, no existing contract was
weakened (one was strengthened, see `store_join`). New targets are in `prove.sh`'s case list
and in `all`.

**The `store` target is not confirmed.** Its first run (6,323 goals, store_core.c plus
site.c) proved all but 8: `store_join`'s two new asserts (that the joined string has no NUL
before its end) and 6 cases of `store_decode`'s P4 postcondition. I then added stepping asserts
and invariants (`rel[k] != 0` after `store_rel_path_ok`, `out[k] != 0` in `store_join`'s second
loop, positivity of the label and path characters, then `wpath` itself as a final assert), and
stated "the path/site is the message's bytes" as `(char)m[...]` (exactly what is stored) rather
than `m[...]`, which needed cast reasoning. The re-run hung with WSL (unresponsive for hours
under the other agents' load) and never reported. `ui`, `gpu_input` and `loop` are fully proved
on the final code. The store target must be re-run: `tools/prove.sh store`.

All are pure C, no library calls, bounded loops, proved with WP + RTE like the other cores:
no undefined behavior for any bytes (P1), bounded (P7), and the contracts below.

### Storage: `store_decode` (store_core.h)

Grammar of `TS_READ`, `TS_WRITE`, `TS_DELETE`:

```
id, site_len, path_len, data_len    four little-endian uint32
site                                site_len bytes, < SITE_MAX, no NUL (only the network process names one)
path                                path_len bytes, <= STORE_PATH_MAX
data                                data_len bytes
```
and nothing after: `16 + site_len + path_len + data_len == len`, computed without overflow.

Results: `STORE_REQ_SHORT` (under 16 bytes: no answer, as before), `STORE_REQ_BAD` (lengths
don't add up: refused with the request's id, as before), `STORE_REQ_REFUSED`, `STORE_REQ_OK`.

Proved for `STORE_REQ_OK`, tying the path the shim opens to P4:
- `label_ok(target)`, `rel_ok(rel)`, and `rel` is the message's path bytes;
- a tab: `same_label(badge, target)` and the path is not `cookies`; the network process: the
  path is `cookies` and the target is the message's site bytes;
- `wpath`: the UTF-16 path is exactly `root`, `\`, the target label, `\`, then `rel` with `/` as
  `\`, NUL-terminated, inside the buffer. That is store_join's `<target>/<rel>` with `\` for
  `/` (the code converts `joined`, store_join's output; the proof shows the result equals the
  form above).

`to_path` is gone: the shim passes `store_decode` its root and buffer. The old `to_path`
silently cut a path short when root + site + path didn't fit in 520 UTF-16 units, which could
name a different file (a prefix of the approved one, a trailing dot, a device name such as
`aux` from `auxzzz`). Now such a path is refused.

`store_join`'s contract gained `out[i] != 0` before the terminator (a strengthening), so a
caller can find the joined string's end.

### GPU: `gpu_input.c`

Grammars:
```
TG_ATLAS  slot, width, height, y0, rows (uint32 LE), then rows * width bytes
          slot < 255, 1 <= width, height <= 4096, y0 + rows <= height, nothing after
TG_IMAGE  id, width, height, then width * height * 4 bytes
          id < 16384, 1 <= width, height <= 16384, nothing after
TG_FRAME  the ten uint32 of GpuFrame; ncmds <= GPU_MAX_CMDS, nmasks <= GPU_MAX_MASKS
          (the rest is gpu_decode's, P5)
```

- `gpu_atlas_parse`, `gpu_image_parse`: accept exactly the grammar (`<==>` contracts), fields
  passed on equal the message's.
- `gpu_upload_plan`: WRITE into the current pixels (same size), NEW (the slot's or id's mark is
  cleared *before* the old pixels are freed, the bytes given back, and the new size fits under
  `GPU_CLIENT_BYTES`), or DROP (nothing changed). The atlas limit still counts the old atlas
  (`c->bytes + new`), the image limit doesn't (`c->bytes - old + new`), as M5's shim did.
- `gpu_atlas_put`, `gpu_image_put`: copy the band or image into the pixels and set the mark,
  only if the pixels exist and are that resource's size; proved: the mark is set iff so, and
  the bytes copied are the message's. The shim's assumption (its ACSL precondition) is that a
  non-NULL `px` has `w * h` (or `w * h * 4`) bytes: the allocation, which is trusted.
- `gpu_frame_header`: the ten numbers as they came; false if short or if the counts the shim
  allocates for are too large.
- `gpu_view_y` in `[0, max(content_h - view_h, 0)]`, `gpu_layer_row` (the `src_y` presented
  from) in `[-GPU_MAX_H, GPU_MAX_H]`, `gpu_scrolls_seen` (the scrolls a frame has seen, with
  the UI's numbering wrapping).

### UI: `ui_core.c`

Grammar of what a tab sends the UI:
```
TU_FRAME        TuFrame (six uint32), 1 <= width <= SURFACE_MAX_W, 1 <= height <= SURFACE_MAX_H
TU_TITLE        0 .. PROTO_TITLE_MAX bytes (shown up to a NUL, as before)
TU_STATE        one uint32; TAB_LOADED and TAB_LOAD_FAILED are themselves, anything else TAB_LOADING
TU_WANT_WEBGPU  empty
TU_NAVIGATE     uint32 new_tab (nonzero = 1), then a URL of 1 .. PROTO_URL_MAX bytes, no NUL
```
`ui_tab_message` returns the type if the message is of the grammar, 0 otherwise; proved with
the fields equal to the message's, the text NUL-terminated after its length, and the URL free
of NUL. `ui_gesture` / `ui_may_open_tab`: the tab-opening rule below.

### Relay: `loop_step`'s new postcondition

`e->kind != LE_NET_MESSAGE ==>` every action is `LA_SOCK_CLOSE` or `LA_NET_SEND`.

## The findings

**Atlas half-made (8.7.2).** `upload_atlas` used to free the old atlas and, if `calloc`
failed, return with the slot still marked: P5's "every glyph names an uploaded atlas" then named
one with no pixels (the CPU rasterizer would read a NULL `px`). Now `gpu_upload_plan` clears the
mark before the old pixels go, and only `gpu_atlas_put` sets it, and only when pixels of that
atlas's size are in place. A failed allocation leaves the slot unmarked (its glyphs are dropped
by the decoder until a new upload succeeds) and logs it. Images had the right order already;
they now go through the same proved functions. (A GPU texture that fails to be made, `tex ==
NULL`, is drawn by the Vulkan backend with its stand-in, as before: the mark means the CPU copy
is there.)

**Storage denial of service (8.7.2).** The service is one thread and `reply` blocked up to 10 s
on each part of a reply to a tab that didn't read it (so longer for a large one), stalling every
other site. Chosen fix: a per-client reply queue of one, sent without waiting.
- `reply` hands the reply to the client's `Reply`; `send_reply` sends parts with big_send's
  framing (first part: type and total size; the rest `BIG_MORE`) while the ring has room, and
  resumes later. While a reply is on its way, that client's next request stays in its channel:
  backpressure, so the service holds at most one reply per client (at most a 64 MB file, the
  quota, which `do_read` had allocated anyway).
- A full ring costs one `chan_send(..., 0)`, which spins at most a timer tick (chan.c's
  shortest wait; chan.c belongs to another part of M9, so no non-blocking send was added), then
  waits 1, 2, 4 .. 250 ms before the next try; the main loop's wait times out for it. So a tab
  that never reads costs the others at most about a tick every 250 ms.
- A client none of whose reply has gone for `REPLY_STALL_MS` (10 s, the old timeout) is dropped:
  its channel closed, its memory freed. The old code left such a tab's channel mid-message
  anyway (its next reply would break the tab's reassembly), so dropping changes nothing for it.
- Why not a shorter timeout and drop: every new tab of an attacking page (a navigation starts a
  new process with a new channel) would still stall everyone for that timeout once. Why not a
  thread per client: closing a channel while its sender thread is inside `chan_send` needs
  synchronization the single-threaded service doesn't have.
- Normal use is unchanged: tabs and the network process wait for each reply synchronously
  (`store_call`), so their rings drain at once and the first try sends everything.

**Storage never dropping a broken network-process client (8.7.2).** A client that broke its
channel was "revoked" by id, and `revoke` matched `GRANT_TAB` only, so the network process's
client (id 0) stayed: polled every loop, printing "broke its channel" each time. Now a broken
(or stalled) client is dropped by its slot (`drop`), whatever its kind; the coordinator's
`CS_REVOKE` still names tabs only.

**A tab opening 60 tabs (8.7.2).** The protocol carries no gesture with `TU_NAVIGATE`, but the
UI knows when it sent a tab a click (`UT_CLICK`): a tab's only way to navigate today is a click
on a link (`tab_main.c` `click`). So a tab may open a new tab only in answer to a click the UI
sent its current process, once per click, within `UI_GESTURE_MS` (5 s) of it
(`ui_may_open_tab`, proved: true only for an unused click no older than that, and it uses it).
A refused one is logged and dropped, like a blocked popup. Same-tab navigations (`new_tab ==
0`) are not limited: they replace the tab's own process and can't add tabs (and the redirect
fix of another M9 part sends the tab's own navigations this way). A rate per second was the
alternative; it would still let a page open a tab every second with no user present.

**The relay's depth (8.7.2).** `step` recursed into the events its actions caused and silently
dropped any past depth 4, so the core could believe a connection open whose socket the shim had
dropped an event for. It is now a loop: events caused while an action is performed are queued
(`event_slot`) and stepped right after that action, before the step's next one (the old
order). The new postcondition of `loop_step` proves that such follow-up events (`LE_CONNECTED`,
`LE_SOCK_SENT`, `LE_SOCK_CLOSED`) produce only `LA_SOCK_CLOSE` and `LA_NET_SEND`, which lead to
no event in `perform`; only `LA_CONNECT` (a connect failing at once) and `LA_SOCK_SEND` (a
write finishing or failing) do, at most one each. So nothing goes deeper than one level, every
event is stepped, and a queue of `LOOP_MAX_ACTIONS` is never full; if it ever were (the shim's
side of the argument is reviewed, not proved), the relay aborts rather than drop an event.

## Behavior changes

- Storage: a path that doesn't fit in 520 UTF-16 units with the root is refused (it was cut
  short). A site named by the network process containing a NUL is refused (it was cut at the
  NUL). Replies are sent as the client reads them; a client that leaves a reply unread for 10 s
  is dropped; a broken network-process client is dropped. A refused request is logged as before.
- GPU: a failed atlas allocation leaves the slot unmarked (was: marked, no pixels) and logs; a
  failed image allocation now logs too. **A bug found:** `frame` dropped the scrolls a frame had
  seen by comparing with `f.scroll_seq`, but `gpu_decode` never sets that field of `f`, so it
  read an uninitialized value; it now uses the header's `scroll_seq` (what was intended, since
  `c->at` was copied from the header). A frame shorter than its header still logs "malformed
  frame"; one with too many records is still dropped silently.
- UI: `TU_NAVIGATE` with a NUL in its URL is ignored (was: the URL cut at the NUL);
  `TU_WANT_WEBGPU` must be empty (every sender's is); a new tab only after a click (above).
- Relay: no event is dropped; at most one level of follow-up events, as the proof shows.

## Still trusted or uncertain

- The shims' allocation: that `px` has the bytes `GpuPixels` says (the precondition of the
  `*_put` functions), and the Windows calls around them (`CreateFileW`, `contained`, etc.).
- `send_reply`'s framing is a copy of big_send's, reviewed, not proved; so is the claim that
  `perform` raises at most one event per action and none for `LA_SOCK_CLOSE`/`LA_NET_SEND`.
- The storage DoS fix bounds the stall a non-reading client causes by chan.c's shortest wait
  (one timer tick, about 15.6 ms by default) per try; a `chan_try_send` in chan.c would make it
  zero. Memory: up to one reply per client (62 x 64 MB in the worst case, for at most 10 s),
  less than what the channels' 256 MB reassembly can already make a server hold (another part
  of M9 limits that).
- The tab-opening rule trusts the UI's own record of its clicks; a click the page receives but
  turns into no navigation leaves one opening usable for 5 s by that page.
- Not run here (per instructions): the browser, the escape tests and the integration tests.
  `proc_tests.exe` passes (228 checks, 0 failed; 4 new test functions). The escape test's `fuzz_ui` sends `TU_NAVIGATE` with
  `new_tab` and no click; those are now refused by the UI (they were refused by the
  coordinator's URL check before).
