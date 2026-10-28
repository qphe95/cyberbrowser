------------------------------ MODULE Protocol ------------------------------
(* The protocol between the coordinator, the tab processes and the servers   *)
(* (the network process and the storage service), SANDBOX_DESIGN.md         *)
(* section 8.3.  The C proofs cover each step of the coordinator; this model *)
(* checks how the steps of all the processes interleave: tabs that start,    *)
(* close and crash with requests in flight; grants and revocations that      *)
(* reach the servers later, in order, over their control channels; the UI   *)
(* reusing a tab id once its tab has ended.                                  *)
(*                                                                           *)
(* Safety: a server serves a request only on a channel it was granted, with  *)
(* the badge of the tab process at the other end; never after that tab's    *)
(* revocation; and holds one client per tab id.  Liveness: a closed tab's    *)
(* process ends, its slot frees, and its channels are revoked.               *)

EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    Ids,        \* the UI's tab ids (reused once a tab has ended)
    Sites,      \* site labels, "opaque" among them (no storage)
    MaxStarts   \* bound on tab processes started (each gets fresh channels)

Servers == {"net", "store"}
States == {"free", "starting", "running", "closing"}

VARIABLES
    slot,       \* the coordinator's view of each tab id: [state, site, chan]
    proc,       \* tab processes, by channel: [id, site, alive]
    ctl,        \* the coordinator's control channel to each server: a FIFO
    clients,    \* the channels each server serves: set of [chan, id, badge]
    inflight,   \* requests sent and not yet served: set of [chan, srv]
    wrong,      \* a server served a request with a badge not its tab's
    starts      \* tab processes started so far (= the next channel)

vars == <<slot, proc, ctl, clients, inflight, wrong, starts>>

Chans == 0 .. MaxStarts - 1
NoChan == MaxStarts         \* a free slot's channel: none

Grant(c, i, s) == [op |-> "grant", chan |-> c, id |-> i, badge |-> s]
Revoke(i) == [op |-> "revoke", id |-> i]

TypeOK ==
    /\ slot \in [Ids -> [state: States, site: Sites, chan: Chans \cup {NoChan}]]
    /\ starts \in 0 .. MaxStarts
    /\ DOMAIN proc = 0 .. starts - 1
    /\ \A c \in DOMAIN proc : proc[c] \in [id: Ids, site: Sites, alive: BOOLEAN]
    /\ \A v \in Servers : \A k \in 1 .. Len(ctl[v]) : ctl[v][k].op \in {"grant", "revoke"}

Init ==
    /\ slot = [i \in Ids |-> [state |-> "free", site |-> CHOOSE s \in Sites : TRUE, chan |-> NoChan]]
    /\ proc = <<>>              \* a function with an empty domain
    /\ ctl = [v \in Servers |-> <<>>]
    /\ clients = [v \in Servers |-> {}]
    /\ inflight = {}
    /\ wrong = FALSE
    /\ starts = 0

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
    /\ UNCHANGED <<clients, inflight, wrong>>

(* The process started (CE_TAB_STARTED). *)
Started(i) ==
    /\ slot[i].state = "starting"
    /\ slot' = [slot EXCEPT ![i].state = "running"]
    /\ UNCHANGED <<proc, ctl, clients, inflight, wrong, starts>>

(* The UI closes it (UC_CLOSE_TAB): the coordinator ends the process. *)
Close(i) ==
    /\ slot[i].state \in {"starting", "running"}
    /\ slot' = [slot EXCEPT ![i].state = "closing"]
    /\ UNCHANGED <<proc, ctl, clients, inflight, wrong, starts>>

(* A process ends: killed after Close, or a crash at any time. *)
Dies(c) ==
    /\ proc[c].alive
    /\ proc' = [proc EXCEPT ![c].alive = FALSE]
    /\ UNCHANGED <<slot, ctl, clients, inflight, wrong, starts>>

(* The coordinator sees it end (CE_TAB_EXITED): the slot frees and both  *)
(* servers are told to revoke the id (CA_TAB_GONE).                      *)
Exited(i) ==
    /\ slot[i].state # "free"
    /\ ~proc[slot[i].chan].alive
    /\ slot' = [slot EXCEPT ![i] = [@ EXCEPT !.state = "free", !.chan = NoChan]]
    /\ ctl' = [v \in Servers |-> Append(ctl[v], Revoke(i))]
    /\ UNCHANGED <<proc, clients, inflight, wrong, starts>>

(* A live tab sends a request on its channel to a server: the network     *)
(* process always, the storage service only if it was given a channel.    *)
Send(c, v) ==
    /\ proc[c].alive
    /\ v = "store" => proc[c].site # "opaque"
    /\ inflight' = inflight \cup {[chan |-> c, srv |-> v]}
    /\ UNCHANGED <<slot, proc, ctl, clients, wrong, starts>>

(* A server takes the next control message, in order. *)
Control(v) ==
    /\ ctl[v] # <<>>
    /\ LET m == Head(ctl[v]) IN
       clients' = [clients EXCEPT ![v] =
           IF m.op = "grant" THEN @ \cup {[chan |-> m.chan, id |-> m.id, badge |-> m.badge]}
           ELSE {x \in @ : x.id # m.id}]
    /\ ctl' = [ctl EXCEPT ![v] = Tail(@)]
    /\ UNCHANGED <<slot, proc, inflight, wrong, starts>>

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

(* A channel whose tab has ended and been revoked: its requests drop. *)
Drop(r) ==
    /\ ~proc[r.chan].alive
    /\ \A x \in clients[r.srv] : x.chan # r.chan
    /\ inflight' = inflight \ {r}
    /\ UNCHANGED <<slot, proc, ctl, clients, wrong, starts>>

Next ==
    \/ \E i \in Ids, s \in Sites : Open(i, s)
    \/ \E i \in Ids : Started(i) \/ Close(i) \/ Exited(i)
    \/ \E c \in DOMAIN proc : Dies(c) \/ \E v \in Servers : Send(c, v)
    \/ \E v \in Servers : Control(v)
    \/ \E r \in inflight : Serve(r) \/ Drop(r)

(* The coordinator kills a closed tab, notices every exit, and the       *)
(* servers keep reading their control channels.                          *)
Fairness ==
    /\ \A c \in Chans : WF_vars(c \in DOMAIN proc /\ slot[proc[c].id].chan = c
                                /\ slot[proc[c].id].state = "closing" /\ Dies(c))
    /\ \A i \in Ids : WF_vars(Exited(i))
    /\ \A v \in Servers : WF_vars(Control(v))

Spec == Init /\ [][Next]_vars /\ Fairness

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

(* Liveness *)

\* A tab the UI closes ends, and its id frees.
ClosedTabsEnd == \A i \in Ids : slot[i].state = "closing" ~> slot[i].state = "free"

\* Every channel of a dead tab is eventually revoked at every server.
DeadChannelsRevoked ==
    \A c \in Chans : \A v \in Servers :
        (c \in DOMAIN proc /\ ~proc[c].alive) ~> (\A x \in clients[v] : x.chan # c)
=============================================================================
