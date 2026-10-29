------------------------------ MODULE Protocol ------------------------------
(* The protocol between the coordinator, the tab processes and the servers   *)
(* (the network process and the storage service), SANDBOX_DESIGN.md         *)
(* section 8.3.  The C proofs cover each step of the coordinator; this model *)
(* checks how the steps of all the processes interleave: tabs that start,    *)
(* close and crash with requests in flight; grants and revocations that      *)
(* reach the servers later, in order, over their control channels; the UI   *)
(* reusing a tab id once its tab has ended.                                  *)
(*                                                                           *)
(* With them, the web-GPU processes of section 5.7 (M8): a tab asks for its  *)
(* site's (UC_WEBGPU); the coordinator starts one for the site if it has     *)
(* none, and connects the tab to it once it runs, or tells the tab it gets   *)
(* none; one ends when its site's last tab does, and may crash at any time,  *)
(* breaking its channels (the tabs may ask again).                           *)
(*                                                                           *)
(* Safety: a server serves a request only on a channel it was granted, with  *)
(* the badge of the tab process at the other end; never after that tab's    *)
(* revocation; and holds one client per tab id.  A web-GPU process serves    *)
(* only tabs of its own site, and a site has one at a time.  Liveness: a     *)
(* closed tab's process ends, its slot frees, and its channels are revoked;  *)
(* a tab waiting for its site's web-GPU process to start is connected to it  *)
(* or told it gets none.                                                     *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    Ids,        \* the UI's tab ids (reused once a tab has ended)
    Sites,      \* site labels, "opaque" among them (no storage, no web-GPU)
    MaxStarts,  \* bound on tab processes started (each gets fresh channels)
    MaxWStarts, \* bound on web-GPU processes started (more: the table is full)
    WSites,     \* the sites whose tabs ask for one ({}: a model without them)
    Requests    \* do tabs send requests to the servers (Send)?  The liveness
                \* properties don't depend on them: ProtocolLive.cfg leaves
                \* them out, to have room for the web-GPU processes.

Servers == {"net", "store"}
States == {"free", "starting", "running", "closing"}
Live == {"starting", "running"}
Asks == {"none", "waiting", "granted", "failed"}

VARIABLES
    slot,       \* the coordinator's view of each tab id: [state, site, chan]
    proc,       \* tab processes, by channel: [id, site, alive]
    ctl,        \* the coordinator's control channel to each server: a FIFO
    clients,    \* the channels each server serves: set of [chan, id, badge]
    inflight,   \* requests sent and not yet served: set of [chan, srv]
    wrong,      \* a server served a request with a badge not its tab's
    starts,     \* tab processes started so far (= the next channel)
    want,       \* each tab id's want_webgpu: waits for its site's to start
    wproc,      \* web-GPU processes, by number: [site, state (the coordinator's view), alive]
    wstarts,    \* web-GPU processes started so far (= the next number)
    wclients,   \* the tab channels each web-GPU process serves
    wreq        \* each tab process's ask for a web-GPU process

vars == <<slot, proc, ctl, clients, inflight, wrong, starts, want, wproc, wstarts, wclients, wreq>>
wvars == <<want, wproc, wstarts, wclients, wreq>>

Chans == 0 .. MaxStarts - 1
NoChan == MaxStarts         \* a free slot's channel: none
WChans == 0 .. MaxWStarts - 1

Grant(c, i, s) == [op |-> "grant", chan |-> c, id |-> i, badge |-> s]
Revoke(i) == [op |-> "revoke", id |-> i]

TypeOK ==
    /\ slot \in [Ids -> [state: States, site: Sites, chan: Chans \cup {NoChan}]]
    /\ starts \in 0 .. MaxStarts
    /\ DOMAIN proc = 0 .. starts - 1
    /\ \A c \in DOMAIN proc : proc[c] \in [id: Ids, site: Sites, alive: BOOLEAN]
    /\ \A v \in Servers : \A k \in 1 .. Len(ctl[v]) : ctl[v][k].op \in {"grant", "revoke"}
    /\ want \in [Ids -> BOOLEAN]
    /\ wstarts \in 0 .. MaxWStarts
    /\ DOMAIN wproc = 0 .. wstarts - 1
    /\ \A w \in DOMAIN wproc : wproc[w] \in [site: Sites, state: States, alive: BOOLEAN]
    /\ wclients \in [WChans -> SUBSET Chans]
    /\ wreq \in [Chans -> Asks]

Init ==
    /\ slot = [i \in Ids |-> [state |-> "free", site |-> CHOOSE s \in Sites : TRUE, chan |-> NoChan]]
    /\ proc = <<>>              \* a function with an empty domain
    /\ ctl = [v \in Servers |-> <<>>]
    /\ clients = [v \in Servers |-> {}]
    /\ inflight = {}
    /\ wrong = FALSE
    /\ starts = 0
    /\ want = [i \in Ids |-> FALSE]
    /\ wproc = <<>>
    /\ wstarts = 0
    /\ wclients = [w \in WChans |-> {}]
    /\ wreq = [c \in Chans |-> "none"]

(* The UI opens tab i on a URL of site s (UC_OPEN_TAB): the coordinator   *)
(* starts its process with fresh channels and grants them to the servers, *)
(* badged with s: the storage service only for a site (grant_store).      *)
Open(i, s) ==
    /\ slot[i].state = "free"
    /\ starts < MaxStarts
    /\ LET c == starts IN
       /\ slot' = [slot EXCEPT ![i] = [state |-> "starting", site |-> s, chan |-> c]]
       /\ proc' = [k \in 0 .. c |-> IF k = c THEN [id |-> i, site |-> s, alive |-> TRUE] ELSE proc[k]]
       /\ ctl' = [ctl EXCEPT !["net"] = Append(@, Grant(c, i, s)),
                             !["store"] = IF s = "opaque" THEN @ ELSE Append(@, Grant(c, i, s))]
       /\ starts' = c + 1
    /\ want' = [want EXCEPT ![i] = FALSE]       \* start_slot
    /\ UNCHANGED <<clients, inflight, wrong, wproc, wstarts, wclients, wreq>>

(* The process started (CE_TAB_STARTED). *)
Started(i) ==
    /\ slot[i].state = "starting"
    /\ slot' = [slot EXCEPT ![i].state = "running"]
    /\ UNCHANGED <<proc, ctl, clients, inflight, wrong, starts>>
    /\ UNCHANGED wvars

(* The UI closes it (UC_CLOSE_TAB): the coordinator ends the process. *)
Close(i) ==
    /\ slot[i].state \in {"starting", "running"}
    /\ slot' = [slot EXCEPT ![i].state = "closing"]
    /\ UNCHANGED <<proc, ctl, clients, inflight, wrong, starts>>
    /\ UNCHANGED wvars

(* A process ends: killed after Close, or a crash at any time.  Its      *)
(* channels to web-GPU processes break.                                  *)
Dies(c) ==
    /\ proc[c].alive
    /\ proc' = [proc EXCEPT ![c].alive = FALSE]
    /\ wclients' = [w \in WChans |-> wclients[w] \ {c}]
    /\ UNCHANGED <<slot, ctl, clients, inflight, wrong, starts, want, wproc, wstarts, wreq>>

(* The coordinator sees it end (CE_TAB_EXITED): the slot frees and both  *)
(* servers are told to revoke the id (CA_TAB_GONE).  If it was its site's *)
(* last tab, the site's web-GPU process is ended (last_of_site:          *)
(* CA_KILL_WEBGPU).                                                      *)
Exited(i) ==
    /\ slot[i].state # "free"
    /\ ~proc[slot[i].chan].alive
    /\ slot' = [slot EXCEPT ![i] = [@ EXCEPT !.state = "free", !.chan = NoChan]]
    /\ ctl' = [v \in Servers |-> Append(ctl[v], Revoke(i))]
    /\ LET s == slot[i].site
           last == s # "opaque" /\ \A j \in Ids \ {i} : slot[j].state = "free" \/ slot[j].site # s
       IN wproc' = [w \in DOMAIN wproc |->
                       IF last /\ wproc[w].site = s /\ wproc[w].state \in Live
                       THEN [wproc[w] EXCEPT !.state = "closing"] ELSE wproc[w]]
    /\ UNCHANGED <<proc, clients, inflight, wrong, starts, want, wstarts, wclients, wreq>>

(* A live tab sends a request on its channel to a server: the network     *)
(* process always, the storage service only if it was given a channel.    *)
Send(c, v) ==
    /\ Requests
    /\ proc[c].alive
    /\ v = "store" => proc[c].site # "opaque"
    /\ inflight' = inflight \cup {[chan |-> c, srv |-> v]}
    /\ UNCHANGED <<slot, proc, ctl, clients, wrong, starts>>
    /\ UNCHANGED wvars

(* A server takes the next control message, in order. *)
Control(v) ==
    /\ ctl[v] # <<>>
    /\ LET m == Head(ctl[v]) IN
       clients' = [clients EXCEPT ![v] =
           IF m.op = "grant" THEN @ \cup {[chan |-> m.chan, id |-> m.id, badge |-> m.badge]}
           ELSE {x \in @ : x.id # m.id}]
    /\ ctl' = [ctl EXCEPT ![v] = Tail(@)]
    /\ UNCHANGED <<slot, proc, inflight, wrong, starts>>
    /\ UNCHANGED wvars

(* A server serves a request on a channel it holds, with that channel's  *)
(* badge (`wrong` notes a badge other than the sender's site).  A request *)
(* on a channel it doesn't hold (not yet granted, or revoked) waits or is *)
(* never served.                                                          *)
Serve(r) ==
    /\ \E x \in clients[r.srv] :
        /\ x.chan = r.chan
        /\ wrong' = (wrong \/ x.badge # proc[r.chan].site)
    /\ inflight' = inflight \ {r}
    /\ UNCHANGED <<slot, proc, ctl, clients, starts>>
    /\ UNCHANGED wvars

(* A channel whose tab has ended and been revoked: its requests drop. *)
Drop(r) ==
    /\ ~proc[r.chan].alive
    /\ \A x \in clients[r.srv] : x.chan # r.chan
    /\ inflight' = inflight \ {r}
    /\ UNCHANGED <<slot, proc, ctl, clients, wrong, starts>>
    /\ UNCHANGED wvars

---------------------------------------------------------------------------
(* Web-GPU processes (section 5.7) *)

\* A web-GPU process the coordinator would hand to tabs of site s: starting or running.
LiveW(s) == {w \in DOMAIN wproc : wproc[w].site = s /\ wproc[w].state \in Live}

\* Channels between the tabs on channels cs and web-GPU process w (the
\* shim's CA_GRANT_WEBGPU): none with a process that has died meanwhile.
Connect(w, cs) ==
    [wclients EXCEPT ![w] = IF wproc[w].alive THEN @ \cup {c \in cs : proc[c].alive} ELSE @]

(* A live, running tab asks (UC_WEBGPU, through the UI): first, or after  *)
(* its channel broke (the model doesn't ask again after "none", which only *)
(* adds cycles of the same states); and the coordinator                    *)
(* takes the ask at once (webgpu_request).  (The model leaves out asks in  *)
(* flight: one from a tab that has since ended does nothing, one from a   *)
(* tab not running is told it gets none.)  Opaque: none.  The site's       *)
(* web-GPU process runs: a channel to it now; starting: once it has       *)
(* (want_webgpu); none: start one, unless the table is full.              *)
Request(c) ==
    /\ proc[c].site \in WSites
    /\ proc[c].alive
    /\ slot[proc[c].id].chan = c /\ slot[proc[c].id].state = "running"
    /\ \/ wreq[c] = "none"
       \/ wreq[c] = "granted" /\ \A w \in WChans : c \notin wclients[w]
    /\ LET i == proc[c].id
           s == slot[i].site
           Failed == /\ wreq' = [wreq EXCEPT ![c] = "failed"]
                     /\ UNCHANGED <<want, wproc, wstarts, wclients>>
       IN
       IF s = "opaque" THEN Failed
       ELSE IF \E w \in LiveW(s) : wproc[w].state = "running" THEN
           \E w \in LiveW(s) :
               /\ wproc[w].state = "running"
               /\ wclients' = Connect(w, {c})
               /\ wreq' = [wreq EXCEPT ![c] = "granted"]
               /\ UNCHANGED <<want, wproc, wstarts>>
       ELSE IF LiveW(s) # {} THEN
           /\ want' = [want EXCEPT ![i] = TRUE]
           /\ wreq' = [wreq EXCEPT ![c] = "waiting"]
           /\ UNCHANGED <<wproc, wstarts, wclients>>
       ELSE IF wstarts < MaxWStarts THEN
           /\ wproc' = [w \in 0 .. wstarts |->
                           IF w = wstarts THEN [site |-> s, state |-> "starting", alive |-> TRUE]
                           ELSE wproc[w]]
           /\ wstarts' = wstarts + 1
           /\ want' = [want EXCEPT ![i] = TRUE]
           /\ wreq' = [wreq EXCEPT ![c] = "waiting"]
           /\ UNCHANGED wclients
       ELSE Failed
    /\ UNCHANGED <<slot, proc, ctl, clients, inflight, wrong, starts>>

\* The ids waiting for site s's web-GPU process.
Wanting(s) == {i \in Ids : want[i] /\ slot[i].site = s}

(* It started (CE_WEBGPU_STARTED, webgpu_started): it runs, and the       *)
(* waiting tabs of its site that run get channels to it.                  *)
WStarted(w) ==
    /\ w \in DOMAIN wproc
    /\ wproc[w].state = "starting"
    /\ LET W == Wanting(wproc[w].site)
           G == {i \in W : slot[i].state = "running"}
           gc == {slot[i].chan : i \in G}
       IN /\ wproc' = [wproc EXCEPT ![w].state = "running"]
          /\ want' = [i \in Ids |-> want[i] /\ i \notin W]
          /\ wclients' = Connect(w, gc)
          /\ wreq' = [c \in Chans |-> IF c \in gc THEN "granted" ELSE wreq[c]]
    /\ UNCHANGED <<slot, proc, ctl, clients, inflight, wrong, starts, wstarts>>

(* It ends: killed after its site's last tab, or a crash (or a failed     *)
(* start) at any time.  Its channels break.                               *)
WDies(w) ==
    /\ w \in DOMAIN wproc
    /\ wproc[w].alive
    /\ wproc' = [wproc EXCEPT ![w].alive = FALSE]
    /\ wclients' = [wclients EXCEPT ![w] = {}]
    /\ UNCHANGED <<slot, proc, ctl, clients, inflight, wrong, starts, want, wstarts, wreq>>

(* The coordinator sees it end (CE_WEBGPU_EXITED, webgpu_exited): its     *)
(* slot frees, and the tabs of its site still waiting hear they get none. *)
WExited(w) ==
    /\ w \in DOMAIN wproc
    /\ ~wproc[w].alive
    /\ wproc[w].state # "free"
    /\ LET W == Wanting(wproc[w].site)
           fc == {slot[i].chan : i \in {j \in W : slot[j].state # "free"}}
       IN /\ wproc' = [wproc EXCEPT ![w].state = "free"]
          /\ want' = [i \in Ids |-> want[i] /\ i \notin W]
          /\ wreq' = [c \in Chans |-> IF c \in fc THEN "failed" ELSE wreq[c]]
    /\ UNCHANGED <<slot, proc, ctl, clients, inflight, wrong, starts, wstarts, wclients>>

---------------------------------------------------------------------------

Next ==
    \/ \E i \in Ids, s \in Sites : Open(i, s)
    \/ \E i \in Ids : Started(i) \/ Close(i) \/ Exited(i)
    \/ \E c \in DOMAIN proc : Dies(c) \/ Request(c) \/ \E v \in Servers : Send(c, v)
    \/ \E v \in Servers : Control(v)
    \/ \E r \in inflight : Serve(r) \/ Drop(r)
    \/ \E w \in WChans : WStarted(w) \/ WDies(w) \/ WExited(w)

(* The coordinator kills a closed tab, notices every exit, and the       *)
(* servers keep reading their control channels.  It kills a closing     *)
(* web-GPU process and notices its start and exit.                       *)
Fairness ==
    /\ \A c \in Chans : WF_vars(c \in DOMAIN proc /\ slot[proc[c].id].chan = c
                                /\ slot[proc[c].id].state = "closing" /\ Dies(c))
    /\ \A i \in Ids : WF_vars(Exited(i))
    /\ \A v \in Servers : WF_vars(Control(v))
    /\ \A w \in WChans : WF_vars(WStarted(w)) /\ WF_vars(WExited(w))
    /\ \A w \in WChans : WF_vars(w \in DOMAIN wproc /\ wproc[w].state = "closing" /\ WDies(w))

Spec == Init /\ [][Next]_vars /\ Fairness

\* The tab ids are interchangeable: the safety check (Protocol.cfg) visits
\* one state of each class.  (Not for liveness, where it is unsound.)
IdSymmetry == Permutations(Ids)

---------------------------------------------------------------------------
(* Safety *)

\* Every request a server served carried the badge of the process at the
\* other end of its channel: a tab never acts as another site.
BadgeTrue == ~wrong

\* What a server holds is badged truly, and the storage service holds no
\* opaque tab.
ClientsTrue ==
    \A v \in Servers : \A x \in clients[v] :
        /\ x.chan \in DOMAIN proc
        /\ x.badge = proc[x.chan].site /\ x.id = proc[x.chan].id
        /\ v = "store" => x.badge # "opaque"

\* One client per tab id at each server, so a revocation by id ends exactly
\* one tab's channel.
OneClientPerId ==
    \A v \in Servers : \A x, y \in clients[v] : x.id = y.id => x = y

\* A server holds a channel only while its tab is the id's current tab, or
\* the revocation that will end it is on its way.
NoStaleClient ==
    \A v \in Servers : \A x \in clients[v] :
        \/ slot[x.id].chan = x.chan
        \/ \E k \in 1 .. Len(ctl[v]) : ctl[v][k] = Revoke(x.id)

\* The coordinator's view: a slot in use names a started process of its id.
SlotsTrue ==
    \A i \in Ids : slot[i].state # "free" =>
        slot[i].chan \in DOMAIN proc /\ proc[slot[i].chan].id = i /\ proc[slot[i].chan].site = slot[i].site

\* P3: the web-GPU process of site s serves only live tabs of site s, and
\* no web-GPU process is opaque's; one never started serves none.
WebgpuClientsTrue ==
    /\ \A w \in WChans \ DOMAIN wproc : wclients[w] = {}
    /\ \A w \in DOMAIN wproc :
        /\ wproc[w].site # "opaque"
        /\ \A c \in wclients[w] : c \in DOMAIN proc /\ proc[c].alive /\ proc[c].site = wproc[w].site

\* A site has at most one web-GPU process the coordinator hands out.
OneWebgpuPerSite ==
    \A v, w \in DOMAIN wproc :
        v # w /\ wproc[v].site = wproc[w].site => ~(wproc[v].state \in Live /\ wproc[w].state \in Live)

(* Liveness *)

\* A tab the UI closes ends, and its id frees.
ClosedTabsEnd == \A i \in Ids : slot[i].state = "closing" ~> slot[i].state = "free"

\* Every channel of a dead tab is eventually revoked at every server (at
\* all of them at once: once revoked, a channel is never granted again).
\* (One leads-to per channel, not per channel and server: TLC checks the
\* properties together far faster with fewer of them.)
DeadChannelsRevoked ==
    \A c \in Chans :
        (c \in DOMAIN proc /\ ~proc[c].alive) ~> (\A v \in Servers : \A x \in clients[v] : x.chan # c)

\* A tab that waits for its site's web-GPU process to start is connected to
\* it or told it gets none, unless it ends first.
WebgpuAnswered ==
    \A c \in Chans :
        (c \in DOMAIN proc /\ wreq[c] = "waiting") ~>
            (c \in DOMAIN proc /\ (wreq[c] \in {"granted", "failed"} \/ ~proc[c].alive))
=============================================================================
