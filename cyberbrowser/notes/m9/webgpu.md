# M9 notes: the web-GPU process's decoders

Step 3 of SANDBOX_DESIGN.md section 8.7.3, the web-GPU item: the WebGL record walker and
the WebGPU value-tree parser, with the object ids and the ranges checked before bytes go into
mapped memory; and the open question of section 8.7.2 about the canvas texture.

## What was built

Two new verified cores, each with its own `tools/prove.sh` target (both in `all`):

| Core | Target | Lines | What is proved | Goals | Time (cold, 12 cores) |
|---|---|---|---|---|---|
| `src/proc/webgl_decode.c` (+ `.h`) | `webgl` | 226 + 183 | P1, P7, the record grammar, the reply bound, the id tables | 1,350 | 135 s |
| `src/proc/webgpu_decode.c` (+ `.h`) | `webgpu` | 219 + 162 | P1, P7, the tree's limits and shape, the id lookup, the map ranges | 2,590 | 324 s |

Both use the `Typed+Cast` memory model, as the GPU decoder does, with the default 60 s a goal
(the slowest goal took 7.4 s). Times are from an empty session, in WSL, while other agents'
proofs ran too. A first version of the parser, with the node store and the links written
inline in its loop, timed out on the tree invariant; the store and the two links are now
small functions with their own contracts (`add`, `link_first`, `link_next`), and the tree
predicate is split into a node's own values and its links, with a lemma that links stay
valid as the tree grows.

### WebGL (`webgl_decode.h`, `.c`)

- **`webgl_next(recs, len, at, replied, &rec)`** walks a `TW_GL_RUN` batch one record at a
  time (`[u32 op][u32 n][n bytes]`). It returns 1 with the record checked, 0 at the end, -1
  if the rest is malformed. `webgl_run` (the shim) now calls it for every record, and calls
  the executor (`webgl_exec_plain`, `run_special`) only with what it accepted.
- **Proved for any bytes:** no undefined behavior (P1), bounded (P7), and when it returns 1:
  - the arguments lie inside the batch (`r->at == at + 8`, `r->n <= len - r->at`), the fixed
    head inside them (`r->head <= r->n`);
  - `r->op`, `r->n` are the record's words;
  - `rec_ok`: the op is known and the length matches its grammar (below), with the
    form's count;
  - the reply so far plus this record's is at most `WEBGL_REPLY_MAX` (128 MB).
  - It returns 0 exactly at the end of the batch.
- **The grammar** (`rec_ok`, `form_ok` in `webgl_decode.h`):
  - plain calls (ops 1..101): `n == webgl_plain_len[op]`, the table `tools/gen_webgl.js`
    now emits (`WEBGL_PLAIN_LENS` in `webgl_gen.h`), from the same table as the tab's
    encoder and `webgl_exec_plain`;
  - special ops (256..276) and queries (300..340): `webgl_forms[op - 256]`, a form, a head
    (bytes of fixed words), the offset of the word the form reads, a cap, and the reply's
    size (`reply + per * count`):

    | Form | Grammar | Ops |
    |---|---|---|
    | exact | `n == head` | create, delete, clearBuffer*v, pixelStorei, fenceSync, waitSync, and the fixed queries |
    | data | `n >= head`, then bytes | bufferSubData, texSubImage2D/3D, compressedTex(Sub)Image2D |
    | has | a flag word; without it, `n == head` | texImage2D, texImage3D |
    | bufdata | as has, and with data the i64 size is the data's length | bufferData |
    | str | `n >= head`, then at most 4 MB with no NUL | shaderSource, bindAttribLocation, getUniformLocation, getAttribLocation, getFragDataLocation, getUniformBlockIndex, requestExtension |
    | names | NUL-separated names, at most 4 MB, at most `cap` names (64, 256) | transformFeedbackVaryings, getUniformIndices |
    | words | `n - head` a multiple of 4, at most `cap` words (16, 16, 256) | drawBuffers, invalidateFramebuffer, getActiveUniforms |
    | counted | `n == head`, the count word at most `cap` | getParameter (16), getUniform (16), getVertexAttrib (4), getActiveUniformBlockParameter (256), getInternalformatParameter (64), readPixels and getBufferSubData (bytes, 256 MB) |
    | uniform | a kind 1..21; then a whole number of values of it | uniform*v, uniformMatrix*fv |
    | attrib | a kind 1..6; then exactly that kind's words | vertexAttrib*v |

- **Ids:** `webgl_name_of(names, kind, id)` returns 0 for id 0, the name the context's own
  table holds for it, or `WEBGL_NO_NAME`; it never indexes out of the tables (`kind <
  WEBGL_KINDS`, `id < WEBGL_MAX_IDS`, a table not made yet is none). `webgl_location_of`
  likewise for uniform locations, `webgl_id_ok` for the range. `webgl_name`,
  `webgl_location`, `create`, `destroy`, `is_object`, `sync_of` in `webgl_backend.c` use
  them. "Only names this context created" holds because the tables are per context
  (`WebglCtx.names`) and only `create` writes a non-zero entry, with what `glGen*` /
  `glCreate*` returned: reviewed, not proved (the writes are in the shim, next to the GL
  calls).
- **Memory model hypotheses** (WP states them for `webgl_next`): the batch and the record
  out-parameter don't overlap the constant tables (`webgl_forms`, `webgl_plain_len`,
  `webgl_uniform_per`, `webgl_attrib_per`). They hold: the batch is a heap buffer (`BigRx`),
  the tables are in read-only data.

### WebGPU (`webgpu_decode.h`, `.c`)

- **`wire_parse(in, len, &pos, &tree)`**, the value-tree parser, is now iterative, with an
  explicit stack of the open arrays and objects (`WIRE_STACK` = 13 frames). The wire format
  is unchanged (`webgpu_wire.h`); the nodes keep offsets into the record instead of
  pointers, and a number's f64 bits (converted in the shim). Proved for any bytes:
  - no undefined behavior, bounded (variant `2 * (WIRE_MAX_NODES - n) + sp`);
  - `tree_ok`: at most 16,384 nodes; every node's tag is one of the nine; every string's,
    byte string's and key's bytes lie inside the input; keys at most 256 bytes, strings
    8 MB; every child and sibling index is -1 or a later node of the tree (so the shim's
    walks, `get`, `item`, the feature list, stay inside it and end);
  - the result is -1 or the index of the first node it added; `pos` stays within the input;
  - a second value (`G_WRITE_TEXTURE`'s bytes) appends to the same tree, keeping `tree_ok`.
  - Depth at most 12 holds by construction (a value is refused when 13 containers are
    open, as M8's `depth > WIRE_MAX_DEPTH`) and is tested, but not stated as an ACSL
    property: the tree has no parent links to state it over.
  - proc_tests checks it against M8's recursive parser (kept in the test as a reference)
    on 4,000 random trees and every prefix of many of them: same result, same nodes.
- **`wire_next`** is `wgpu_run`'s record framing: proved inside the batch, 0 exactly at its
  end.
- **`wgpu_obj(objs, id, kind)`**, the id lookup (`obj` in the shim): returns `&objs[id]`
  exactly when `live(objs, id, kind)` (in range, that kind, a handle, not lost, its device
  not destroyed), else NULL. `objs` is the client's own table (`WgpuClient.objs`), so it
  returns only this client's live objects. `wgpu_id_free` is `put_obj`'s check for a new
  id. The `Obj` struct and the `O_*` kinds moved to `webgpu_decode.h` (`WgpuObj`).
- **Map ranges** (item 3), in integers:
  - `wire_whole(bits, &v)`: an f64's bits as a whole number below 2^53, truncated toward
    zero; refused if negative (but -0), infinite, NaN, or 2^53 or more. Integer arithmetic
    on the bits only.
  - `wgpu_map_ok(off, size, buf_size)` <=> `off % 8 == 0 && size % 4 == 0 && size <= 256 MB
    && off + size <= buf_size` (mathematical integers: no overflow).
  - `wgpu_write_ok(off, len, map_off, map_size)` <=> `len != 0 && off % 8 == 0 && len % 4 == 0
    && map_off <= off && off + len <= map_off + map_size`.
  - `buffer_map`, `buffer_unmap` (the `memcpy` into the mapping) and `G_WRITE_BUFFER`'s
    offset use them. **Found:** M8 converted the page's doubles with `(uint64_t)off` after
    `off < 0 || off > 9e15` tests, which are false for NaN: a NaN offset or size in
    `G_BUFFER_MAP` reached `(uint64_t)NaN`, undefined behavior (in practice a garbage
    offset that wgpu then refused).

## What is and isn't proved

Proved (all goals, no `admit`, no assumptions beyond the memory-model hypotheses above):
- the WebGL walker's framing, the per-op grammar of every plain call (by the generated
  table), every special op and query (by `webgl_forms`), the string and blob lengths, the
  counts and caps, and the reply accounting;
- the id → GL-name and location lookups never index out of range, and return only an entry
  of the context's own table, 0, or `WEBGL_NO_NAME`;
- the WebGPU parser's P1 and limits, and the shape above; the record framing; the id lookup;
  the map ranges.

Not proved (tested, fuzzed, or reviewed):
- **The executor's offsets match the table's heads.** `run_special` reads its fixed words at
  offsets below each op's head; each case keeps its `NEED(head)` check as a cheap
  restatement, so a mismatch fails the record rather than reading past it. The plain calls'
  executor is generated from the same table as the lengths.
- That every query appends at most what `webgl_forms` counts for it: reviewed case by case;
  ANGLE's strings are now cut at 64 KB and info logs at 1 MB so that every reply has a
  bound. `put` still refuses past 128 MB.
- What the shim does with the tree (which keys and values it reads), the WebGL executor,
  and everything behind them (ANGLE, wgpu, the driver), as section 8.4 says.
- The WebGPU ops' own argument layouts in `run_one` (how many u32s before the tree): small
  checks in the shim, unchanged.

## The canvas texture (section 8.7.2)

**Question:** can a texture released in `canvas_texture` still be used through a page-held
object?

**Path:** `G_CURRENT_TEXTURE` gives the page's id the canvas's `WGPUTexture` handle
(`objs[a].h = cv->texture`) and takes another wgpu reference for it; `canvas_texture`
(on `G_CONFIGURE`, `wgpu_resize`) releases the canvas's own reference and makes a new
texture. The page's id is released by `free_slot` (`G_RELEASE`, or the client going).

**Answer:** in M8 it was safe only because `wgpuTextureAddRef` happened to exist: the
reference for the page's id was taken through a `GetProcAddress` looked up at the call, and
**if it was missing, the page's id kept the handle with no reference of its own**, so a
configure or resize released the texture under it (use after free in wgpu on the page's
next use of the id), and its later release was a double release. With the pinned
wgpu-native the function exists, so this was latent. Fixed: `wgpuTextureAddRef` is now
loaded with the other entry points (`WGPU_FUNCS`), so the library doesn't load without it,
and the page's id always holds its own reference. A view made from it holds the texture
inside wgpu (reference-counted there).

Found on the way, and fixed: each `getCurrentTexture()` gave its id a `Res` of its own, so
destroying the canvas texture through one id wasn't seen through another (or by
`wgpu_present`), and a submit using the other id, or the next present's copy, reached
wgpu-native with a destroyed texture, which aborts the process (a site's own web-GPU
process: availability, not isolation). Now the canvas holds one `Res` for its texture,
shared by every id given it; `wgpu_present` skips a destroyed one, and the next
`G_CURRENT_TEXTURE` replaces it with a new texture.

## Fuzzing

`cb_webgpu_fuzz` (real ANGLE and wgpu, in process), all passing:

| Mode | Batches, seed | Result |
|---|---|---|
| gl | 3000, 3; 2000, 11; 600, 1 and 7 | 0 failed, 0 contexts lost |
| gpu | 500, 1; 1000, 2 | 0 failed |
| gpuseq | 400, 1; 600, 2 | 0 failed |

`gpu` with seed 1 rejects exactly as many batches as M8's build did (404 of 500), so the new
parser and framing accept the same records. The `gl` generator now also writes records as
their grammar allows (`gl_grammatical`, a third of them, from `webgl_forms`), since random
special records now nearly all fail the exact grammar and would no longer reach the
executor.

proc_tests: `test_webgl_decode` and `test_wgpu_decode`, about 319,000 checks (mostly the
parser compared with M8's on random trees and their prefixes, and random batches through
the walker).

## Behavior changes

- **WebGL batches are stricter.** A record whose length its grammar doesn't allow is
  malformed (the batch's `TW_GL_RUN` reply is "not ok", the rest dropped, as for any
  malformed record before):
  - fixed-layout special ops and queries need exactly their head (M8 accepted and ignored
    extra bytes); `getError`, `getExtensions`, `getGraphicsResetStatus` exactly 0;
  - `texImage2D`/`3D` and `bufferData` without data must have no tail;
  - `vertexAttrib*v` exactly the kind's words (M8: at least);
  - `transformFeedbackVaryings` more than 64 names, `getUniformIndices` more than 256, are
    refused (M8 silently used the first 64 / 256, and `getUniformIndices` then answered
    fewer than asked);
  - names lists are capped at 4 MB, like strings;
  - trailing bytes after the last record (fewer than 8) make the batch malformed (M8 ignored
    them). The same for WebGPU batches (`wire_next`).
  - The tab's encoder (`webgl_api.js`) sends exactly these lengths, so pages see no
    difference.
- **Replies are accounted up front:** a query whose reply could take the batch past 128 MB is
  refused before it runs (M8 ran it and failed when the buffer couldn't grow). ANGLE's
  strings (`GL_EXTENSIONS`, the renderer's and vendor's) are cut at 64 KB; real ones are a
  few KB.
- `getActiveUniformBlockName`: ANGLE's returned length is checked before it indexes the
  name buffer.
- **WebGPU numbers:** offsets and sizes are converted by `wire_whole`; a NaN is refused
  instead of being undefined behavior; the limit is 2^53 instead of 9e15.
- **Canvas textures:** wgpu-native without `wgpuTextureAddRef` no longer loads; a destroyed
  canvas texture is replaced on the next `getCurrentTexture()` and not presented.

## Files

- New: `src/proc/webgl_decode.h`, `.c`; `src/proc/webgpu_decode.h`, `.c`.
- Changed: `src/proc/webgl_backend.c`, `.h` (the walker, the lookups, string caps);
  `src/proc/webgpu_wgpu.c` (the parser, lookups, ranges, framing, canvas texture);
  `tools/gen_webgl.js` and its output `src/proc/webgl_gen.h` (the length table);
  `src/proc/proc_tests.c`; `tests/webgpu/webgpu_fuzz.c`; `tools/prove.sh`; `CMakeLists.txt`.
- `webgpu_main.c` is unchanged.

## Running the proofs

`tools/prove.sh` is checked out with CRLF line endings on Windows (`core.autocrlf`), which
bash in WSL refuses. It `cd`s relative to its own folder, so the copy with LF endings has to
sit next to it: `tr -d '\015' < tools/prove.sh > tools/prove_lf.sh` (not committed), then
`PROVE_OUT=... bash tools/prove_lf.sh webgl webgpu`.
