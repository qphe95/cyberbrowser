#!/bin/bash
# The proofs of SANDBOX_DESIGN.md section 8 (M4): Frama-C/WP on each verified
# core, with runtime-error goals (P1) added by the RTE plugin, and TLC on the
# TLA+ model of the protocol.  Runs on Linux (here WSL), after
# tools/proof_setup.sh.
#
#   tools/prove.sh [component ...]    chan coord plan site store loop gpu gpu_input ui net webgl webgpu tla
#                                     (default: all)
#   tools/prove.sh eva                Eva on every process's shim (M9), or one:
#                                     eva-chan eva-loop eva-store eva-coord
#                                     eva-gpu eva-webgpu eva-ui eva-net
#
# Fails if any goal is unproved, or (eva) any alarm is unexplained.
set -e
cd "$(dirname "$0")/.."
eval "$(opam env --switch=proofs --set-switch 2> /dev/null)"
export PATH=$HOME/.local/bin:$PATH
out=${PROVE_OUT:-/tmp/cb_prove}
mkdir -p "$out"
jobs=${PROVE_JOBS:-$(nproc)}
timeout=${PROVE_TIMEOUT:-60}

wp() {   # wp <name> <memory model> <files...>
    local name=$1 model=$2; shift 2
    echo "== $name"
    frama-c -cpp-extra-args="-Isrc/ipc -Isrc/proc -DFRAMA_C" -machdep x86_64 \
        -wp -wp-rte -wp-model "$model" -wp-check-memory-model -wp-split \
        -warn-signed-overflow -no-warn-unsigned-overflow \
        -wp-prover alt-ergo,z3,cvc5 -wp-par "$jobs" -wp-timeout "$timeout" \
        -wp-session "$out/$name.session" -wp-cache update \
        "$@" -then -report-unclassified-warning ERROR -report \
        > "$out/$name.log" 2>&1 || { tail -40 "$out/$name.log"; return 1; }
    grep -E "^\[wp\] Proved goals|Timeout|Unknown|Failed" "$out/$name.log" | head -20
    ! grep -qE "^\[wp\] .*(Timeout|Unknown|Failed|Unsuccess)" "$out/$name.log" &&
        grep -qE "^\[wp\] Proved goals: +([0-9]+) / \1$" "$out/$name.log"
}

# Eva on each process's whole shim (SANDBOX_DESIGN.md 8.7.3, step 5): P1 for
# the code around the cores.  The Windows calls are described by the trusted
# contracts of proofs/eva/include (a few with bodies, win_models.c), on the
# MinGW machdep (LLP64, 16-bit wchar_t); each process's entry point is driven
# by proofs/eva/drv_<name>.c.  The proved cores run as they are (so Eva also
# checks the shim's calls against their preconditions), the rings as their
# proved contracts (chan_ring_model.c), and the libraries outside M9 as
# models (lib_*.c).  Allocation may fail (no -eva-no-alloc-returns-null).
#
# Every alarm must be listed in proofs/eva/<name>.explained or
# proofs/eva/common.explained, under a comment saying why it can't happen or
# why it is acceptable.  An alarm is a run-time check, or a callee's
# precondition at a call, whose status is Unknown or Invalid; it is keyed by
# file, function, kind and property (no line numbers).  See M9_NOTES_eva.md.
#   EVA_PRECISION (default 2)
eva_one() {   # eva_one <name> <files...>: writes $out/eva-<name>.summary
    local name=$1; shift
    local log="$out/eva-$name.log" csv="$out/eva-$name.csv" got="$out/eva-$name.alarms"
    local sum="$out/eva-$name.summary" t0=$SECONDS
    echo "== eva-$name" > "$sum"
    rm -f "$csv"
    frama-c -cpp-extra-args="-Iproofs/eva/include -Isrc/ipc -Isrc/proc -Isrc -Isrc/quickjs -Iproofs/eva -DFRAMA_C" \
        -machdep proofs/eva/machdep_mingw_x86_64.yaml \
        -eva -eva-precision "${EVA_PRECISION:-2}" -eva-split-return auto -eva-unroll-recursive-calls 12 \
        -eva-plevel 300 -eva-auto-loop-unroll "${EVA_UNROLL:-64}" ${EVA_FLAGS:-} \
        -eva-warn-undefined-pointer-comparison pointer -eva-no-show-progress \
        "$@" proofs/eva/win_models.c \
        -then -report-csv "$csv" > "$log" 2>&1 || { tail -40 "$log" >> "$sum"; return 1; }
    local bad="degenerated|Neither code nor specification|without assigns clause|Frama-C aborted"
    if grep -qE "$bad" "$log"; then
        grep -E "$bad" "$log" | head >> "$sum"
        return 1
    fi
    # Inside the proved cores, WP's proof of P1 covers every input that meets
    # their preconditions; Eva checks those at the shim's calls (kept).
    awk -F'\t' 'NR > 1 && ($6 == "Unknown" || $6 ~ /^Invalid/) &&
                $2 !~ /^(chan_ring|big_core|coord_core|coord_plan|loop_core|store_core|site|gpu_decode|gpu_input|ui_core|net_policy|net_decode)\.c$/ &&
                $5 !~ /^(assigns clause|from clause|postcondition|precondition|loop invariant|loop assigns|loop variant|termination clause|exit clause|disjoint behaviors|complete behaviors|assertion|check)$/ {
                    print $2 ":" $4 ": " $5 ": " $7 }' "$csv" | sort -u > "$got"
    local explained=$(mktemp) own=$(mktemp)
    cat proofs/eva/common.explained "proofs/eva/$name.explained" 2> /dev/null | grep -v '^#' | grep -v '^ *$' |
        sort -u > "$explained"
    grep -v '^#' "proofs/eva/$name.explained" 2> /dev/null | grep -v '^ *$' | sort -u > "$own" || true
    local new stale
    new=$(comm -23 "$got" "$explained")
    stale=$(comm -13 "$got" "$own")
    rm -f "$explained" "$own"
    echo "   $(wc -l < "$got") alarms in $((SECONDS - t0)) s; $(grep -E 'statements reached' "$log" | head -1 |
         sed 's/^ *//')" >> "$sum"
    [ -z "$stale" ] || { echo "   explained, no longer reported (remove):"; echo "$stale" | sed 's/^/     /'; } >> "$sum"
    [ -z "$new" ] || { echo "   NOT EXPLAINED:"; echo "$new" | sed 's/^/     /'; } >> "$sum"
    [ -z "$new" ]
}
eva_ipc="src/ipc/chan.c src/ipc/bigmsg.c src/ipc/big_core.c proofs/eva/chan_ring_model.c"
eva_files() {   # the files of one process's analysis
    case $1 in
    chan)   echo proofs/eva/drv_chan.c $eva_ipc ;;
    loop)   echo proofs/eva/drv_loop.c $eva_ipc src/proc/spawn.c proofs/eva/loop_core_model.c ;;
    store)  echo proofs/eva/drv_store.c $eva_ipc src/proc/spawn.c src/proc/store_core.c src/proc/site.c ;;
    coord)  echo proofs/eva/drv_coord.c $eva_ipc src/proc/spawn.c src/proc/sandbox.c src/proc/coord_core.c \
                 src/proc/coord_plan.c src/proc/site.c ;;
    gpu)    echo proofs/eva/drv_gpu.c $eva_ipc src/proc/spawn.c src/proc/gpu_decode.c src/proc/gpu_input.c \
                 proofs/eva/lib_gpu.c ;;
    webgpu) echo proofs/eva/drv_webgpu.c $eva_ipc src/proc/spawn.c src/proc/site.c src/proc/gpu_blocklist.c \
                 proofs/eva/lib_webgpu.c ;;
    ui)     echo proofs/eva/drv_ui.c $eva_ipc src/proc/spawn.c src/proc/ui_core.c ;;
    net)    echo proofs/eva/drv_net.c proofs/eva/drv_net_loop.c $eva_ipc src/proc/spawn.c src/proc/net_policy.c \
                 src/proc/net_decode.c src/proc/site.c proofs/eva/lib_net.c ;;
    esac
}
eva() {   # eva <name...>: in parallel (each Eva is one thread), PROVE_JOBS at a time
    local failed=0 running=0 n
    for n in "$@"; do
        eva_one "$n" $(eva_files "$n") &
        running=$((running + 1))
        if [ "$running" -ge "$jobs" ]; then wait -n || failed=1; running=$((running - 1)); fi
    done
    while [ "$running" -gt 0 ]; do wait -n || failed=1; running=$((running - 1)); done
    for n in "$@"; do cat "$out/eva-$n.summary"; done
    return $failed
}
eva_all="chan loop store coord gpu webgpu ui net"

all="chan coord plan site store loop gpu gpu_input ui net webgl webgpu tla"
for c in ${@:-$all}; do
    case $c in
    eva)   eva $eva_all ;;
    eva-*) eva "${c#eva-}" ;;
    # Typed+Cast+ref separates pointer parameters used as references (the
    # cores' state, event and actions); the decoder has none, and its
    # read-only resource table is simplest without it.
    chan)  wp chan  Typed+Cast+ref src/ipc/chan_ring.c src/ipc/big_core.c ;;
    coord) wp coord Typed+Cast+ref src/proc/coord_core.c src/proc/site.c ;;
    plan)  wp plan  Typed+Cast+ref src/proc/coord_plan.c ;;
    site)  wp site  Typed+Cast+ref src/proc/site.c ;;
    store) wp store Typed+Cast+ref src/proc/store_core.c src/proc/site.c ;;
    loop)  wp loop  Typed+Cast+ref src/proc/loop_core.c ;;
    gpu)   timeout=${PROVE_TIMEOUT:-240} wp gpu Typed+Cast src/proc/gpu_decode.c ;;   # its postconditions need longer
    gpu_input) wp gpu_input Typed+Cast+ref src/proc/gpu_input.c ;;   # M9: uploads and the frame header
    ui)    wp ui    Typed+Cast+ref src/proc/ui_core.c ;;            # M9: the UI's reading of a tab's messages
    net)   wp net   Typed+Cast src/proc/net_policy.c src/proc/net_decode.c ;;
    webgl) wp webgl Typed+Cast src/proc/webgl_decode.c ;;
    webgpu) wp webgpu Typed+Cast src/proc/webgpu_decode.c ;;
    tla)
        # Safety on the larger model, liveness on the smaller one.  No deadlock
        # check: with the starts bounded, every behavior ends in a quiet state.
        # TLC's state files go in $out, not proofs/tla: on a Windows folder
        # under WSL, the liveness check of the state graph is many times slower.
        meta=$(cd "$out" && pwd)/tla-states
        rm -rf "$meta"
        for cfg in Protocol ProtocolLive; do
            echo "== tla ($cfg.cfg)"
            (cd proofs/tla && java -XX:+UseParallelGC -cp "$HOME/.local/lib/tla2tools.jar" tlc2.TLC \
                -workers auto -deadlock -metadir "$meta/$cfg" -config $cfg.cfg Protocol.tla > "$out/$cfg.log" 2>&1) ||
                { tail -40 "$out/$cfg.log"; exit 1; }
            grep -E "Model checking completed|states generated" "$out/$cfg.log"
        done ;;
    *) echo "unknown component $c"; exit 2 ;;
    esac
done
echo "all proved"
