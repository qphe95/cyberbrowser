#!/usr/bin/env bash
# Regenerate cyberbrowser/shaders/embedded_shaders.{h,c} from the GLSL sources.
#
# Needs glslangValidator. One-time local install (no admin rights):
#   mkdir -p tools && cd tools
#   curl -sL -o glslang.zip \
#     https://github.com/KhronosGroup/glslang/releases/download/main-tot/glslang-master-windows-Release.zip
#   unzip -q glslang.zip
# Then run this script from the repo root.
set -e

GLSLANG="${GLSLANG:-tools/bin/glslangValidator.exe}"

"$GLSLANG" -V cyberbrowser/shaders/flamegraph.vert -o cyberbrowser/shaders/flamegraph_vert.spv
"$GLSLANG" -V cyberbrowser/shaders/flamegraph.frag -o cyberbrowser/shaders/flamegraph_frag.spv
python scripts/embed_shaders.py cyberbrowser/shaders \
    cyberbrowser/shaders/flamegraph_vert.spv \
    cyberbrowser/shaders/flamegraph_frag.spv
