# M9 notes: the coordinator's handles, the `#escape` hook, redirects in the model

Part of milestone M9 (SANDBOX_DESIGN.md section 8.7): step 2 of 8.7.3 (the
coordinator's handles), the `#escape` finding of 8.7.2, and step 6 (the
protocol model).

## 1. `perform` split in two: `coord_plan.c`

New proved core `src/proc/coord_plan.c` / `coord_plan.h` (prove.sh target
`plan`, also in `all`).

- `coord_plan(state, tables, action, plan)` turns one `CoordAction` into a
  list of `CoordOp`s: `OP_CHAN` (a new channel, named by what it is for:
  `CH_CTL`, `CH_UI`, `CH_NET`, `CH_STORE`, `CH_DRAW`, `CH_WEBGPU`),
  `OP_SPAWN_TAB slot`, `OP_GRANT srv w ch` (server end to a server, badged
  with the action's site), `OP_GIVE_UI ch`, `OP_GIVE_TAB slot ch msg w`,
  `OP_COMMIT_TAB` / `OP_DISCARD_TAB`, `OP_REVOKE`, `OP_KILL_TAB`,
  `OP_CLOSE_CTL`, `OP_SEND_TAB`, `OP_SPAWN_WEBGPU`, `OP_KILL_WEBGPU`,
  `OP_SEND_UI`, `OP_RESTART_GPU`, `OP_EXIT`. Each op has an `undo` index: -1
  for the ops done in order, else an op done after them only if one at that
  index or later failed (the old rollbacks of `start_tab`, `regrant_gpu`,
  `grant_webgpu`, written as data).
- `coord_run(tables, action, plan, event)` performs a plan through
  `coord_do` (the shim's one entry to Windows), stops at the first failure
  if the plan says so, runs the undo ops, updates the tables (`apply`) and
  builds the result event (`CE_TAB_STARTED/_START_FAILED`,
  `CE_WEBGPU_STARTED/_EXITED`) for the plan's slot.
- `coord_next_tab_exit` / `coord_next_webgpu_exit` scan the tables, ask the
  shim (`coord_tab_signaled`, `coord_webgpu_signaled`) whether that entry's
  process ended, empty the entry and make the event naming that entry; the
  web-GPU one also plans the `CT_WEBGPU_FAILED` notices to the tabs it served.
- `CoordTables` mirrors the shim's tables: `tab_proc[i]` (g_tab_proc),
  `tab_ctl[i]` (g_tab_ctl), `tab_site[i]` (the label slot i's process was
  started with, recorded at `OP_COMMIT_TAB`), `tab_webgpu[i]` (was the shim's
  `g_tab_webgpu`, now only here), `webgpu_proc[w]`, `webgpu_site[w]`, `gpu`.
  Changed only by `coord_run`'s `apply` and the exit scans, as the wrappers
  change the HANDLE tables.

### What is proved (all goals, no `admit`)

`coord_plan` ensures, for every op (`op_ok` / `op_vals_ok` in coord_plan.h):
- a server's end of a channel goes to the server that channel is for
  (`srv_chan`: net/CH_NET, store/CH_STORE, gpu/CH_DRAW, webgpu/CH_WEBGPU),
  for the action's tab id, badged with the action's site, which is the label
  of the action's slot in the core's state (slot in use, same id); the
  storage service's only for a site;
- the other end goes only into the process of that slot: at its start
  (`OP_SPAWN_TAB`, slot STARTING with no process yet), or copied into the
  process the slot holds (`OP_GIVE_TAB`: `tab_is`, the label it was started
  with equals the badge); the UI only ever gets `CH_UI`;
- a web-GPU channel is granted at, and given only for, web-GPU process `w ==
  a->webgpu`, which runs (tables and core), and whose recorded and core
  labels equal the tab's (non-opaque) site: the same-site pairing of
  `CA_GRANT_WEBGPU`, carried down to the handles;
- every slot and index is in range; kill/send/give/spawn name a slot in use
  (`in_use`) with a process / open control channel; `OP_CLOSE_CTL` (after a
  tab's exit, when its slot is already free) names an open channel;
- the result event names the action's slot (`result_ok`).

`coord_run`: the event follows iff the plan has a result, and names the
plan's slot with its ok or fail kind. `apply`: a committed tab records the
action's label; a web-GPU start records its site iff it worked.
`coord_next_tab_exit` / `coord_next_webgpu_exit`: the event's slot is the
entry found ended, which held a process and is now empty; the web-GPU one's
notices go only to slots in use with an open control channel. P1 (RTE) and
P7 (variants) for all of it.

The plan doesn't assume the core's invariants: it re-checks, at run time,
what it needs (ranges, slot states, ids, labels, the tables). Each
operation is checked where it is written (`push` calls `op_valid`, proved
to imply `op_vals_ok`), and a plan missing an operation it should have is
emptied (`whole`). That shape is what made it provable: a first version
asked each caller to establish `op_vals_ok` before every push, and the
provers timed out carrying the state across a dozen writes to the plan
(12 of 2,889 goals). Now the frame is said once, as lemmas (`plan_same`,
`op_vals_kept`, `notices_kept`), as `coord_core.c` does for its actions. Every check
holds for every action `coord_step` makes (its `action_ok` plus the
tables' bookkeeping), so the behavior is unchanged; a forged action gets no
ops (and a start a failed result). proc_tests has cases for both.

Goals and times: see section 4.

### Still trusted: the Windows wrappers (coordinator.c)

Each has an ACSL-style contract in a comment (Frama-C doesn't read the
file; they are assumptions):
- `coord_do` (contract in coord_plan.h, used by WP: `assigns \nothing` of
  the proved state) — dispatches each op to the wrappers below;
- `dup_to` (DuplicateHandle into a named process), `close_in`
  (DUPLICATE_CLOSE_SOURCE);
- `grant` (dup_to into a server + CS_GRANT with the badge), `revoke`
  (CS_REVOKE);
- `spawn_tab` (spawn_child in sandbox_for(SB_TAB, slot), inheriting exactly
  the plan's channels), `give_ui`, `give_tab`, `commit_tab`, `discard_tab`
  (TerminateProcess/Wait/CloseHandle), `plan_end` (CloseHandle of the
  coordinator's own copies);
- `start_webgpu` (spawn in sandbox_for(SB_WEBGPU, w)), `exit_all`
  (TerminateProcess, ExitProcess), TerminateProcess for the kills;
- `coord_tab_signaled`, `coord_webgpu_signaled` (WaitForSingleObject,
  GetExitCodeProcess, UnregisterWaitEx, CloseHandle).
- `coord_run` and `coord_next_*_exit` call the shim through `coord_do`,
  `coord_tab_signaled` and `coord_webgpu_signaled`, whose contracts in
  coord_plan.h are the assumptions WP uses (they touch none of the proved
  state; `coord_do` may exit, for OP_EXIT).
- Also trusted: that the shim's HANDLE tables and `g_tables` change
  together (each wrapper writes its HANDLE entry, `coord_run` writes the
  mirror from the same op), and the startup in `main` (servers, relay, UI,
  GPU), which no tab reaches. `spawn.c` and `sandbox.c` are unchanged and
  still trusted.

### Behavior

Unchanged for every action the core makes, with these invisible differences:
- channel-creation failures in a tab start now close every channel made so
  far (the old code leaked the control channel when a later one failed);
- a grant to a GPU process that failed to restart (`g_gpu.proc == NULL`)
  fails before calling DuplicateHandle instead of in it (same outcome: the
  tab start fails).

## 2. The `#escape` trigger is gone

`coordinator.c` no longer looks at a URL's ending. The hook is now
`CYBER_TAB_EXE=<exe>` plus `CYBER_TAB_EXE_URL=<url>`, both read from the
coordinator's environment at start: the first tab opened on exactly that URL
runs `<exe>`; after that the hook is spent (cleared). With either variable
unset, every tab is `cb_tab.exe`.

Why this choice: the escape test needs ordinary tabs before and after the
hostile one in the same run (to show the servers it attacked still serve,
and that localStorage and cookies carry on), so "every tab" doesn't fit;
counting tab starts is fragile (navigations and reloads start processes).
An exact URL configured by whoever starts the coordinator, used once, keeps
the test as it was, and no tab can choose the executable: in a normal run
nothing is configured, and in a test run a later navigation to that URL, or
to any URL ending in `#escape`, gets `cb_tab.exe`.

`tests/sandbox/escape_test.sh` exports
`CYBER_TAB_EXE_URL="http://localhost:$port/#escape"` (and
`MSYS2_ENV_CONV_EXCL` for it, so Git Bash passes it unconverted), opens one
more tab on `http://127.0.0.1:$port/#escape` after the hostile one, and
checks that only one tab log is the hostile tab's (the design's "navigations
to `#escape`" attack). Run it as before, after merging (not run here; the
browser was in use):

    tests/sandbox/escape_test.sh <build dir> [port]

## 3. Redirected navigations in the protocol model

`proofs/tla/Protocol.tla` gains:
- `Load(c, f)`: the network process loads tab `c`'s navigation ending (after
  any redirects) at site `f`; it gives the page only if `f` is the badge it
  holds for the channel, else refuses it with its final URL
  (`navto[c] := f`);
- `Replace(c)`: the UI takes the tab's TU_NAVIGATE: closes the tab (if still
  its id's current one) and opens the final URL in a new tab with a free id,
  which the coordinator starts labeled with the final site (`StartTab`, now
  shared with `Open`); no free id or start left: only the close; a tab gone
  since: dropped. Weakly fair.
- invariant `DocTrue` (P10): no tab is given a document of another site
  (`wrongdoc` stays false), and a tab is replaced only for another site;
- property `RedirectsFollowed`: every refused navigation is acted on.

Constant `Redirects` turns the refusals on. A navigation needs a running tab
that is its id's current one, and a refused tab waits for the UI, which
closes it, so each tab process is refused at most once: no counter needed.
Same-site loads change no state, so they cost nothing.

- `ProtocolLive.cfg`: `Redirects = TRUE`, the only model with them; checks
  `DocTrue` and `RedirectsFollowed`.
- `Protocol.cfg`: `Redirects = FALSE` (loads still checked against the
  badge, only never refused). With them it had 24.2 million states, 3.7
  times as many, and took 12 minutes on the loaded machine.

A mutant whose network process ignores the badge (`IF TRUE` for
`IF x.badge = f`) violates `DocTrue` in ProtocolLive within a second.

`tools/prove.sh tla` now gives TLC `-metadir $PROVE_OUT/tla-states`: with its
state files under `proofs/tla` (a Windows folder seen from WSL), the
liveness check of the 2.0 million-node behavior graph ran for over 25
minutes without finishing; in WSL's own file system it takes 12 seconds.
It also stops TLC leaving `states/` folders in the tree.

## 4. Numbers

Measured in WSL on 12 cores while other agents ran Frama-C (load average
about 20), so the times are long.

| prove.sh target | goals | time |
|---|---|---|
| `plan` (new: coord_plan.c) | 4,362 / 4,362 | 319 s, cold session |
| `coord` (coord_core.c, unchanged) | not rerun: unchanged sources | |

| Model | before (HEAD) | after |
|---|---|---|
| `Protocol.cfg` (safety) | 6,502,489 distinct states, 162 s | 6,502,489 distinct states, 2.5–4 min |
| `ProtocolLive.cfg` (liveness) | 95,233 distinct states, 22 s | 252,227 distinct states, 76 s |

(Both "before" runs and the "after" ProtocolLive run used a state folder in
WSL's file system, as prove.sh now does.)
