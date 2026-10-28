#!/bin/bash
# The proofs of SANDBOX_DESIGN.md section 8 (M4): Frama-C/WP on each verified
# core, with runtime-error goals (P1) added by the RTE plugin, and TLC on the
# TLA+ model of the protocol.  Runs on Linux (here WSL), after
# tools/proof_setup.sh.
#
#   tools/prove.sh [component ...]    chan coord site store loop gpu tla (default: all)
#
# Fails if any goal is unproved.
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

all="chan coord site store loop gpu tla"
for c in ${@:-$all}; do
    case $c in
    # Typed+Cast+ref separates pointer parameters used as references (the
    # cores' state, event and actions); the decoder has none, and its
    # read-only resource table is simplest without it.
    chan)  wp chan  Typed+Cast+ref src/ipc/chan_ring.c ;;
    coord) wp coord Typed+Cast+ref src/proc/coord_core.c src/proc/site.c ;;
    site)  wp site  Typed+Cast+ref src/proc/site.c ;;
    store) wp store Typed+Cast+ref src/proc/store_core.c src/proc/site.c ;;
    loop)  wp loop  Typed+Cast+ref src/proc/loop_core.c ;;
    gpu)   timeout=${PROVE_TIMEOUT:-120} wp gpu Typed+Cast src/proc/gpu_decode.c ;;   # its postconditions need longer
    tla)
        # Safety on the larger model, liveness on the smaller one.  No deadlock
        # check: with the starts bounded, every behavior ends in a quiet state.
        for cfg in Protocol ProtocolLive; do
            echo "== tla ($cfg.cfg)"
            (cd proofs/tla && java -XX:+UseParallelGC -cp "$HOME/.local/lib/tla2tools.jar" tlc2.TLC \
                -workers auto -deadlock -config $cfg.cfg Protocol.tla > "$out/$cfg.log" 2>&1) ||
                { tail -40 "$out/$cfg.log"; exit 1; }
            grep -E "Model checking completed|states generated" "$out/$cfg.log"
        done ;;
    *) echo "unknown component $c"; exit 2 ;;
    esac
done
echo "all proved"
