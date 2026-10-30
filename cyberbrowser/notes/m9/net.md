# M9 notes: the network process

Part of M9 ("proving the shims", SANDBOX_DESIGN.md section 8.7): steps 3 (decoding what a
tab sends the network process) and 4 (its policy into `net_policy.c`) of 8.7.3, and the
findings of 8.7.2 about the cross-site redirect, redirects keeping the old URL, PNA by an
unproved copy, and the network process's denial of service.

## The cross-site redirect was real

Read end to end at M8:

1. The tab loads its page: `page_load` → `fetch_start_page` sets `HTTP_MODE_NAVIGATE` and
   fetches through `HttpRemote` (`tab_main.c` `net_request`).
2. `net_main.c` `run_request`: for a navigation, `tls_set_space_limit(-1)`, then
   `http_request_to_memory`, which follows every `Location` by recursion
   (`http_download.c`, "Handle redirects"), to any site. `final_url` was used only for the
   address space and the frame list; `may_read` returned true for `HTTP_MODE_NAVIGATE`; the
   body went back with `ok`.
3. The tab ran that body with `g_cyber_start_url` and `location` set to the URL it asked
   for, its own site's storage (`g_site`, from that URL), its own cookie jar and same-origin
   fetches.

So an open redirect on site A ran site B's page as A, with A's `localStorage`,
`document.cookie` and credentialed same-origin requests. Not run (the browser was in use);
the sites test below checks the fix end to end.

## What was built

### P10: a tab is given only documents of its own site

`net_policy.h`:

```c
/*@ ensures \result == NET_DOC_OK || \result == NET_DOC_ELSEWHERE;
    ensures mode == NET_MODE_NAVIGATE ==>
        (\result == NET_DOC_OK <==> tab_len > 0 && same(tab_site, tab_len, doc_site, doc_len));
    ensures mode != NET_MODE_NAVIGATE ==> \result == NET_DOC_OK; */
int net_doc_verdict(int mode, const uint8_t *tab_site, uint32_t tab_len,
                    const uint8_t *doc_site, uint32_t doc_len);
```

In words: the page's own document (`NET_MODE_NAVIGATE`) is given to a tab only if its URL's
site (`site_of`, "" if it has none) is exactly the tab's badge, and an opaque tab (badge "",
`about:blank`, which fetches nothing) is given none. A frame's document may be of any site, as
section 9.1 requires while frames share their embedder's process; other modes aren't
documents (ORB and CORS cover them).

How `net_main.c` applies it, for a navigation:
- the request's own URL first: another site's → `NET_ELSEWHERE` with that URL, nothing fetched;
- every redirect hop, through a new per-thread redirect filter in `http_download.c`
  (`http_set_redirect_filter`): the first hop to another site isn't followed, and the reply
  is `NET_ELSEWHERE` with that hop's URL (as section 2.4 always said: "a redirect to another
  site isn't followed");
- and the URL it ended at, all the same, before a `NET_OK`.

The tab (`tab_main.c`): on `NET_ELSEWHERE` for its document, `page_load` fails, and instead of
"Failed to load" it sends `TU_NAVIGATE` (same tab) with that URL. The UI's existing path ends
this process and asks the coordinator for a tab of the URL's site (`coord_url_ok` still
decides). Nothing in the UI or the coordinator changed.

Not stated by P10, and still trusted: that the shim calls `net_doc_verdict` on every hop and
on the final URL, and computes the sites with `site_of` (proved to make well-formed labels).
The coordinator's badge being the tab's true label is P3.

### Redirects keep the old URL (fixed)

- `NetResponse` gained `url_len` and the URL (`NetResponse + url + headers + error + body`):
  where the request ended, for `NET_OK`; where to go, for `NET_ELSEWHERE`. `ok` is now
  `NET_FAILED` / `NET_OK` / `NET_ELSEWHERE` (proto.h).
- The tab sets it as the thread's `http_last_url` (`http_set_last_url`).
- `page_engine.c`: the document's URL becomes `http_last_url()` before any of the page is
  parsed (the observer's first call, or after the fetch): `g_cyber_start_url` (the base of
  relative URLs everywhere), `location`, `document.baseURI`, `document.domain`. The URL goes
  into JS as a string value; before, `page_prepare` pasted it into script text.
- `frame_api.c`: a frame's document likewise takes its final URL, so its realm's origin is
  where it ended, not where it started (it was the other way, so a frame redirected to
  another origin ran as the first).
- `http_download.c`: a `Location` is resolved against the hop's URL (`wurl_resolve`), so a
  relative one works; more than 20 hops fail ("Too many redirects": the recursion had no
  bound, so a redirect loop could overflow the network process's stack); `http_last_url`
  is set for every request, including ones `url_map` serves (in the network process a
  worker could otherwise report a URL left over from another tab's request).

### PNA through the proved function

`tls_client.c` `space_allowed(space)` is now
`t_space_limit < 0 || net_space_allows(t_space_limit, space)`. The arguments: the thread's
limit is the client's space, `space` the target's. Same results as before for every value
that occurs: the limit is -1 or 0..2 (`site_space`, `net_space_v4/v6`, `tls_last_space() >= 0`);
an unknown target (-1) is refused under a limit either way. (For a limit above 2, which
nothing sets, the old code allowed 0..limit and the proved one allows nothing.)
`net_policy.c` moved into the engine library (it is linked by every program with
`tls_client.c`).

### Policy into net_policy.c

New pure functions, with contracts (all in `net_policy.h`):
- `net_same`: two byte strings are equal.
- `NetFrames`, `net_frames_has`, `net_frames_add`: the frame-origin list, a fixed table (64
  origins of at most 512 bytes, as before; a longer origin can't be named as an initiator,
  which is at most 511 bytes, so it isn't kept). Adding only appends; `has` is exactly
  membership.
- `net_initiator_ok`: exactly "the initiator's site is the tab's, or the initiator is a
  frame origin it loaded".
- `net_may_read` on a `NetRead` (everything the shim extracted): documents pass; a response
  that isn't cross (from the initiator's origin when the initiator counts, else from the
  tab's site) passes; a cors response passes exactly when `cors_ok` (below); a no-cors one
  when `orb_verdict` allows; an unknown mode never.

`cors_ok`: an `Access-Control-Allow-Origin`; if the request carried the jar's cookies, it isn't
`*` and `Access-Control-Allow-Credentials` is `true` (any case); and it is `*`, or names the
initiator (when it counts), or (when none counts) is an origin of the tab's site.

The shim keeps: parsing URLs into sites and origins, finding header values, taking the
frame list's lock, and the bookkeeping.

### Decoding (net_decode.c)

`net_decode_request`, `net_decode_cookie_get`, `net_decode_cookie_set`: proved free of
undefined behavior for any bytes, and to accept only:
- `TN_REQUEST`: the eight fields of `NetRequest` (read byte by byte, little-endian, stated as
  `le32` in the contract), then method, URL, headers, initiator and body filling the message
  exactly (32 + the lengths == the message's length); method and URL non-empty strings with
  their NUL and no NUL inside; exactly `nheaders` (≤ 64) NUL-terminated headers filling their
  part; initiator absent or a string with its NUL, ≤ 512 bytes; mode ≤ `NET_MODE_FRAME`. Every
  offset it returns is inside the message and every string ends with its NUL there.
- `TN_COOKIE_GET`: the id, then a URL of at least one byte without NUL, and at most one NUL
  after it.
- `TN_COOKIE_SET`: a URL of at least one byte, NUL, a cookie line without NUL, at most one
  NUL after it.

The shim uses the strings in place (`m + offset`) and copies the cookie messages' strings
before use (it no longer relies on `big_recv`'s NUL after the message).

### Denial of service

- **Queue bounded.** A tab may have 256 requests (and 256 MB of them) waiting for a worker,
  all tabs 2,048; beyond that a request is answered "too many requests". A legitimate tab
  waits for each reply on the thread that asked (`net_call`), so it has as many requests
  outstanding as it has fetching threads (the job system's 64 plus a few), far below 256.
- **`malloc` checked:** jobs, replies and their copies; a failed reply allocation stops
  serving that tab rather than losing a reply silently.
- **Workers never wait on a tab.** Replies go into the tab's own queue and a sender thread
  per tab sends them (30 s per part, as before). A tab that doesn't read its replies holds
  only its own thread. When it has 256 replies (or 512 MB) unsent, or a part isn't taken in
  30 s, the network process stops serving it (logs "... it is served no more"; its reader and
  sender end, its queued jobs are skipped; the coordinator's revocation frees it later).
  Large legitimate replies are unaffected: the tab's receiver thread reads the channel
  continuously into memory, and the limits count only replies not yet started.
- **One tab can't take every worker.** 32 workers (was 16), and a tab runs at most 16 at once
  (as many as it could before), so a tab whose requests hang (a slow server it chose) leaves
  16 for the others.

Why this and not a short send timeout: a short timeout would cut off a legitimate tab whose
receiver is briefly descheduled during a large reply (video segments, big pages), and
without a separate sender a slow tab still holds a worker for the timeout; a per-tab sender
removes the cross-tab effect whatever the timeout.

## Proofs

`tools/prove.sh net` now runs WP on `net_policy.c` and `net_decode.c` together.

| Target | Files | Goals (M8) | Goals (now) | Time |
|---|---|---|---|---|
| `net` | `net_policy.c`, `net_decode.c` | 1,873 | 4,541, all proved | 424 s (session cleared, prover cache warm, machine shared with other proof runs) |
| `site` | `site.c` (unchanged) | 2,709 | 2,709, all proved | 1,441 s (session cleared, same shared machine) |

The first run left 5 goals timed out; both causes were fixed in the code, not by weakening a
contract:
- `rd32`: `m[o + 1]` is `m[(o + 1) mod 2^32]` in C, so the bound check needed `o <= 28` (its
  callers pass NetRequest's offsets) and the bytes' ranges as an assertion;
- `net_frames_add`'s "\result ⇔ in_frames" needed its witness (`frame_is(f, k, o, n)`
  for the new entry) asserted.

No `admit`, no new assumptions; P9's contract is unchanged.

## Tests

Unit tests (`proc_tests.exe`, `test_net_core`): P10's cases, `net_same`, the frame list
(duplicates, too long, full), initiators, `net_may_read` (documents, same-site, ORB, a named
frame origin, an initiator it may not name, CORS with and without credentials, `*`, a site's
origin, mismatches, a bad mode), and the decoders on good messages and on each way of
breaking the grammar. `proc_tests.exe`: 216 checks, 0 failed. `cyberbrowser-tests.exe`: ALL
TESTS PASSED (449).

Integration tests, not run here (the browser was in use). Build first, then, in Git Bash
from the `cyberbrowser` folder (node on the PATH; ports 38517 and 38518 free):

```
tests/sandbox/escape_test.sh build-m9      # prints pass/FAIL per check, exits 1 on a failure
tests/sites/sites_test.sh build-m9         # prints ok/FAIL per check, exits 1 on a failure
```

New in the escape test (`cb_escape.exe`, a hostile tab of `http+localhost`):
- `documents()`: a navigation to `http://localhost:P/redirect?to=http://127.0.0.1:P/page?...`
  must be answered `NET_ELSEWHERE` with that URL and no body; asking for the other site's page
  outright likewise; a relative redirect within its site (`/dir/sub/redirect?to=../rel`) is
  followed and the reply says `http://localhost:P/dir/rel`.
- `unread_replies()`: 600 requests for 1 MB (`/big`), replies never read (last on the network
  channel). The script checks `net.log` for "served no more", that the other site's page was
  never fetched for the hostile tab's navigations, and (as before) that the next pages of both
  sites load and keep their storage and cookies.

New in the sites test (real tabs), a third run:
- `http://127.0.0.1:A/dir1/start` redirects to `../dir2/landing`: the page reports
  `location` and `document.baseURI` as `/dir2/landing`, and its `fetch('rel')` gets
  `/dir2/rel` ("in-dir2"), not `/dir1/rel`.
- `http://127.0.0.1:A/go-other` redirects to `http://localhost:A/landed-other`: the page must
  report `location` and `origin` of localhost and no `localStorage` of 127.0.0.1 (the
  `/dir2/landing` page set one); `net.log` has "redirects to ... another site's: elsewhere",
  and a tab was started for `http://localhost:A/landed-other`.

## Behavior changes

- A navigation to, or a redirect of the page's document to, another site isn't given to the
  tab: the tab is replaced by one of that site, at the redirect's target (so the target is
  fetched by the new tab: a POST's body isn't carried over, as it isn't by `TU_NAVIGATE`).
- Pages and frames get their URL after redirects (location, base URL, origin of a frame).
- Relative `Location` headers work; more than 20 redirects fail.
- A credentialed (the jar's cookies were attached) cross-origin cors read now needs
  `Access-Control-Allow-Credentials: true` and not `*`. The tab's own fetch/XHR code already
  required that for credentialed requests (`cors_validate_response_headers`) and marks the
  others with an empty `Cookie:` header, which stops the jar's cookies, so legitimate pages
  see no difference.
- `TN_REQUEST`s are checked against the exact grammar: the headers must be exactly
  `nheaders` strings (before, extra ones were ignored and missing ones tolerated); method,
  URL and initiator may not contain a NUL; an empty method or URL is refused. A malformed
  request shorter than the fixed part is now answered (with the id in its first 4 bytes, if
  any). `TN_COOKIE_SET` with an empty URL is ignored.
- `NetResponse` has a new field and part (the URL) and `ok` values; tabs and the escape tab
  were updated.
- The network process has 32 workers, at most 16 for one tab, bounded queues, a sender
  thread per tab, and stops serving a tab that leaves its replies unread.

## Still trusted or uncertain

- The shim: extracting sites, origins and header values (`site_url_host`/`site_of` are
  proved; `wurl_origin` and `header_value` are not), calling the policy on every hop and
  reply, and the queues and threads (reviewed, not proved; Eva on the shim is step 5).
- Whether "credentialed" is right in every case: it is "no `Cookie:` header in the request
  and not `OPTIONS`", mirroring `http_download.c`, which is what decides whether the jar's
  cookies go.
- The ORB part of `net_may_read`'s contract is only P1/P7 (as before): its result is
  `orb_verdict`'s, which has no functional contract.
- `document.baseURI` in the sites test: assumed assignable as `page_prepare` always did; if
  the engine computes it, the expectation still holds since its inputs changed too.
- The protocol model (`Protocol.tla`, 8.7.3 step 6) doesn't yet have redirected navigations.
