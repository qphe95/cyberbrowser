# M9 notes: the channels' framing (SANDBOX_DESIGN.md 8.7.3 step 1, 8.7.2's 256 MB item)

## What was built

- **`src/ipc/big_core.c/.h`**: the assembly of `bigmsg.h` messages, moved out of
  `big_recv`. Pure C, no library calls, one bounded loop. `big_core_part(state, limit,
  part type, part bytes, len, buf, cap, &need)` decides whether a part continues the message
  and copies its payload into the shim's buffer. It returns one of:
  - `BIG_REFUSED`;
  - `BIG_PART`, taken but not whole yet;
  - `BIG_DONE`, whole;
  - `BIG_GROW`: make the buffer `*need` bytes and call again with the same part. Nothing
    changed.

  `big_core_limit` maps a receiver's limit to the effective one (0, or anything over
  `BIG_MAX`, means `BIG_MAX`). `BIG_MORE` and `BIG_MAX` moved from `bigmsg.h` to
  `big_core.h` (`bigmsg.h` includes it, so callers are unchanged).
- **Proved** (WP + RTE, `Typed+Cast+ref`, in the `chan` target):
  - P1 for any part type, length and bytes, and any assembler state. A state with
    `have > total` or `total > limit` is refused, not trusted.
  - Every write is inside `buf[0 .. cap-1]` (assigns clause plus RTE). `*need` is at most
    the limit plus 1: the bytes received so far, this part's, and the NUL. So the buffer
    grows with the bytes received, not with the total claimed.
  - A taken part appends exactly its payload (after the 4-byte little-endian total, for a
    first part) at `have`, and leaves `buf[0 .. have-1]` unchanged. A first part starts at 0.
    So by induction over the parts, a message reported `BIG_DONE` is exactly the
    concatenation of its parts' payloads. Its length equals the announced total (`have ==
    total`), the total is at most the limit, and `buf[total] == 0`. The type is the first
    part's.
  - Refused: `BIG_MORE` with no message started, a start while busy, a first part shorter
    than 4, a total over the limit, a first part longer than its total, and a `BIG_MORE`
    carrying more than `total - have`. A refusal or `BIG_GROW` changes neither the state nor
    the buffer.
- **The shim** (`bigmsg.c` `big_recv`): copies `BigRx`'s fields into a `BigAsm` and calls the
  core. On `BIG_GROW` it reallocs, doubling from 4096 up to `limit + 1`, and calls again.
  Then it writes the state back. `big_send` is unchanged.
- **`BigRx.limit`**: the largest message accepted. The zero-initialised default means
  `BIG_MAX`, so every existing `BigRx` behaves as before.
- **`chan_ring_header_ok(magic, ring_bytes)`** in `chan_ring.c` is proved:
  `\result <==> magic == CHAN_MAGIC && ring_size_ok(ring_bytes)`. This is the same predicate
  that `chan_ring_put` and `chan_ring_take` require. `chan_attach` and `chan_create` call
  it, and `CHAN_MAGIC` moved to `chan_ring.h`. `chan_attach` reads each header field once,
  with a relaxed atomic load, so the compiler can't re-read it. It keeps the values private
  and never reads the shared header again (comment in `chan.c`).
- **Tests**: `test_chan_framing` in `src/proc/proc_tests.c` covers:
  - the header check;
  - the limit;
  - a two-part message with growth;
  - an empty message;
  - each refusal, including broken states;
  - `big_send`/`big_recv` over a real channel in one process (100,000 bytes from two pieces;
    an empty message; a message one byte over `rx.limit`, refused).

  190 checks, 0 failed.

## Goals (tools/prove.sh, `chan` target)

| File | Goals |
|---|---|
| `chan_ring.c` (with `chan_ring_header_ok`) | 226 / 226 |
| `big_core.c` | 390 / 390 |
| **`chan` target** | **616 / 616** |

Wall time of `prove.sh chan`: 54 s with a fresh session folder, 38 s with the session cached. Nothing is admitted, and no existing contract changed.

One note on running it: this worktree is checked out with CRLF line endings
(`core.autocrlf=true`), and bash in WSL won't run `tools/prove.sh` that way. I ran it with the
`\r` stripped (`bash -c "$(tr -d '\r' < tools/prove.sh)" tools/prove.sh chan`). Frama-C
reads the CRLF C files fine.

## Limits chosen (one line per BigRx, with a comment)

| Server, channel | Limit | Why |
|---|---|---|
| Storage, every client (`store_main.c` `grant`) | `sizeof(StoreRequest) + SITE_MAX + STORE_PATH_MAX + STORE_QUOTA_BYTES`, about 64 MB | `request()` accepts only `site_len < SITE_MAX` and `path_len <= STORE_PATH_MAX`. `store_quota_ok` refuses any `data_len > STORE_QUOTA_BYTES`. A larger message could only fail. The network process's channel gets the same limit: cookies files are under the same quota. |
| Network, a tab's channel (`net_main.c` `reader`) | `BIG_MAX / 2 + 4 MB`, 132 MB | The tab's `net_request` (`tab_main.c`) refuses bodies over `BIG_MAX / 2`. The rest of a `TN_REQUEST` is method, URL, at most 64 headers and an initiator of at most 512 bytes. 4 MB is twice Chromium's 2 MB URL limit, with room for headers. Cookie messages are a URL and a line. |
| GPU, a tab's draw channel (`gpu_main.c` `grant`) | `BIG_MAX`, explicit | `upload_image` accepts up to 16384 x 16384 RGBA, more than `BIG_MAX`. A frame can be about 64 MB (2^20 commands, 2^16 masks). What a tab keeps is bounded separately by `CLIENT_BYTES`. |
| Web-GPU, a tab's channel (`webgpu_main.c` `grant`) | `BIG_MAX`, explicit | WebGL `bufferData`/`texImage2D` and WebGPU `writeBuffer` have no smaller cap. |
| Loopback relay (`loop_main.c`) | none | It uses `chan_recv` only (`LOOP_DATA_MAX` per message), no `BigRx`. |

Receivers of messages from other servers (UI, network to storage, tab-side receivers) keep
the `BIG_MAX` default.

## Trusted, not proved

- `bigmsg.c` itself, which is reviewed:
  - copying `BigRx` to and from `BigAsm`;
  - the growth policy and `realloc`;
  - not touching `buf` between calls, which the induction argument needs (`realloc`
    preserves the contents).
- The cross-call property ("a whole message is the concatenation of its parts") is proved
  per step (append-only, earlier bytes unchanged, a start resets to 0). The induction over
  the parts is stated in the header's comment, not checked by a harness.
- `chan.c`'s single read of the header (the atomic loads) is reviewed; only the decision on
  the values read is proved.

## Behavior changes

- Storage clients are refused messages over about 64 MB, and a tab's network channel over
  132 MB. `big_recv` returns -1, which the servers treat as a broken channel, as for a
  message over `BIG_MAX` before. A well-behaved tab never sends one.
- A refused part no longer leaves `BigRx` half-updated. Before, a start refused for its
  length still set `busy`. Every caller drops the channel on -1, so this isn't visible.
- The buffer grows at most to `limit + 1`, not `BIG_MAX + 1`.
- The total is decoded as explicit little-endian rather than `memcpy`. On x86 this is the
  same; `big_send` still writes it with `memcpy`.
