#!/bin/bash
# Installs the proof tools (SANDBOX_DESIGN.md section 8.5) without root, on
# Linux (here WSL Ubuntu), into ~/.local and ~/.opam:
#   GMP 6.3.0 (built from source; zarith needs it), opam, OCaml, Frama-C,
#   Why3, Alt-Ergo (from opam), Z3 and CVC5 (release binaries), and
#   tla2tools.jar (the TLA+ tools; needs Java).
# Then run tools/prove.sh.
set -e
prefix=$HOME/.local
mkdir -p "$prefix/bin" "$prefix/src"
cd "$prefix/src"
export PATH=$prefix/bin:$PATH
export C_INCLUDE_PATH=$prefix/include LIBRARY_PATH=$prefix/lib LD_LIBRARY_PATH=$prefix/lib

if [ ! -e "$prefix/include/gmp.h" ]; then
    curl -fsSLO https://gmplib.org/download/gmp/gmp-6.3.0.tar.xz
    echo "a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898  gmp-6.3.0.tar.xz" | sha256sum -c -
    tar xf gmp-6.3.0.tar.xz
    (cd gmp-6.3.0 && ./configure --prefix="$prefix" > /dev/null && make -j8 > /dev/null && make install > /dev/null)
fi

if [ ! -x "$prefix/bin/opam" ]; then
    curl -fsSL -o "$prefix/bin/opam" https://github.com/ocaml/opam/releases/download/2.6.1/opam-2.6.1-x86_64-linux
    chmod +x "$prefix/bin/opam"
fi
# Frama-C wants Graphviz's dot, only to draw graphs we don't ask for; a stub
# that reports a version satisfies the check without root.
if ! command -v dot > /dev/null; then
    printf '#!/bin/sh\necho "dot - graphviz version 0 (stub)" >&2\n' > "$prefix/bin/dot"
    chmod +x "$prefix/bin/dot"
fi
[ -d "$HOME/.opam" ] || opam init --bare --disable-sandboxing --no-setup -y
opam option --global depext=false
opam switch list -s | grep -qx proofs || opam switch create proofs ocaml-base-compiler.5.2.1 -y
eval "$(opam env --switch=proofs --set-switch)"
opam install -y frama-c alt-ergo why3

if [ ! -x "$prefix/bin/z3" ]; then
    curl -fsSLO https://github.com/Z3Prover/z3/releases/download/z3-4.13.4/z3-4.13.4-x64-glibc-2.35.zip
    unzip -oq z3-4.13.4-x64-glibc-2.35.zip
    cp z3-4.13.4-x64-glibc-2.35/bin/z3 "$prefix/bin/"
fi
if [ ! -x "$prefix/bin/cvc5" ]; then
    curl -fsSLO https://github.com/cvc5/cvc5/releases/download/cvc5-1.2.1/cvc5-Linux-x86_64-static.zip
    unzip -oq cvc5-Linux-x86_64-static.zip
    cp cvc5-Linux-x86_64-static/bin/cvc5 "$prefix/bin/"
fi
[ -e "$prefix/lib/tla2tools.jar" ] ||
    curl -fsSL -o "$prefix/lib/tla2tools.jar" https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar

why3 config detect
frama-c -version
