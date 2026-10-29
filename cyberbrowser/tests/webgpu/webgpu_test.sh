#!/bin/bash
# WebGL and WebGPU through the browser and its web-GPU process (SANDBOX_DESIGN.md
# M8), on the pages of server.js: triangles drawn and read back, a compute
# shader, canvases on the screen, a crashing web-GPU process taking down only
# its canvases, a blocked driver and denied features, the process ending
# with the site's last tab; then the record decoders under the fuzzer
# (cb_webgpu_fuzz).
#
#   tests/webgpu/webgpu_test.sh <build dir> [port=38524] [fuzz batches=2000]
build=$(cd "$1" && pwd)
P=${2:-38524}
batches=${3:-2000}
dir=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
fails=0
expect() {      # expect <name> <file> <pattern>: a line matching pattern
    if grep -qE -- "$3" "$2"; then echo "ok   $1"; else echo "FAIL $1 (no /$3/ in $(basename "$2"))"; fails=$((fails + 1)); fi
}
never() {       # never <name> <file> <pattern>
    if grep -qE -- "$3" "$2"; then echo "FAIL $1 ($(grep -m1 -E -- "$3" "$2"))"; fails=$((fails + 1)); else echo "ok   $1"; fi
}
run() {         # run <script> [env...]: the pages' lines, the web-GPU processes' logs, the coordinator's
    (cd "$build" && rm -rf browser_data logs && env "${@:2}" CYBER_UI_SCRIPT="$(cygpath -m "$1")" timeout 300 ./cb_coord.exe) \
        2> "$tmp/coord.txt"
    cat "$build"/logs/tab-*.log > "$tmp/tabs.txt"
    grep -h "\[gpu\]" "$tmp/tabs.txt" > "$tmp/pages.txt"
    cat "$build"/logs/webgpu-*.log > "$tmp/webgpu.txt" 2> /dev/null
}
# The color of a screenshot's pixel at x,y, as r,g,b.
pixel() {
    node -e "
const b=require('fs').readFileSync(process.argv[1]);const off=b.readUInt32LE(10),w=b.readInt32LE(18),h=b.readInt32LE(22);
const row=Math.ceil(w*3/4)*4;const i=off+(h-1-(+process.argv[3]))*row+(+process.argv[2])*3;console.log([b[i+2],b[i+1],b[i]].join(','));" \
        "$1" "$2" "$3"
}
# Where the page's content starts in a screenshot (below the tabs and the address bar).
content_top() {
    node -e "
const b=require('fs').readFileSync(process.argv[1]);const off=b.readUInt32LE(10),w=b.readInt32LE(18),h=b.readInt32LE(22);
const row=Math.ceil(w*3/4)*4;for(let y=0;y<h;y++){const i=off+(h-1-y)*row+40*3;if(b[i]==255&&b[i+1]==0&&b[i+2]==0){console.log(y);process.exit(0);}}console.log(-1);" "$1"
}

node "$dir/server.js" "$P" &
server=$!
sleep 1
W=$(cygpath -m "$tmp")

cat > "$tmp/s.txt" <<EOF
open http://127.0.0.1:$P/gl1
wait 6000
shot $W/gl1.bmp
open http://127.0.0.1:$P/gl2
wait 6000
open http://127.0.0.1:$P/gpu
wait 8000
shot $W/gpu.bmp
quit
EOF
run "$tmp/s.txt"
p="$tmp/pages.txt"
expect "WebGL 1: a context" "$p" "context \[object WebGLRenderingContext\]"
expect "WebGL 2: a context" "$p" "context \[object WebGL2RenderingContext\]"
expect "WebGL: what it says of itself" "$p" "version WebGL 2.0 / WebGL GLSL ES 3.00"
expect "WebGL: the program links" "$p" "linked true attributes 1"
expect "WebGL: a uniform reads back" "$p" "uniform 1,0.5,0,1"
expect "WebGL: the triangle drawn, read back" "$p" "inside 255,127,0,255 outside 0,0,255,255"
expect "WebGL: no error after it" "$p" "\] error 0$"
expect "WebGL: an out-of-range draw reads nothing (refused, or robust access)" "$p" "out of range draw error (0|1282)$"
expect "WebGL: an out-of-range write refused" "$p" "out of range write error 1281"
expect "WebGL: bindings kept" "$p" "bound buffer true program true"
expect "WebGL: extensions, no GPU timers" "$p" "extensions [0-9]+ oes_vao true timer false"
expect "WebGL: textures" "$p" "texture error 0 filter 9728"
expect "WebGL 2: vertex arrays" "$p" "vao true bound true"
expect "WebGL 2: buffers read back" "$p" "buffer read back 1,2,3,4"
expect "WebGL 2: syncs" "$p" "sync true"
expect "WebGL: another context's objects refused" "$p" "another context's buffer error 1282"
expect "WebGPU: navigator.gpu" "$p" "navigator.gpu true format bgra8unorm"
expect "WebGPU: an adapter, no timestamp queries" "$p" "features [0-9]+ timestamp false"
expect "WebGPU: a device with WebGPU's limits" "$p" "device true maxBindGroups 4"
expect "WebGPU: a compute shader" "$p" "compute 2,4,6,42"
expect "WebGPU: a triangle drawn, read back" "$p" "\[gpu\] inside 255,127,0,255 outside 0,0,255,255"
expect "WebGPU: validation errors in error scopes" "$p" "scope GPUValidationError"
expect "WebGPU: frames to a canvas" "$p" "canvas frames 30"
never "no script errors" "$p" "\] (error .* at|rejected)"
expect "one web-GPU process, for the site" "$tmp/webgpu.txt" "^\[webgpu\] for http\+127.0.0.1$"
for shot in gl1 gpu; do
    top=$(content_top "$tmp/$shot.bmp")
    if [ "$top" -ge 0 ]; then
        expect "the $shot canvas on the screen (orange inside, blue outside)" \
            <(echo "$(pixel "$tmp/$shot.bmp" 8 $((top + 56))) $(pixel "$tmp/$shot.bmp" 56 $((top + 8)))") \
            "^255,127,0 0,0,255$"
    else
        echo "FAIL the $shot canvas on the screen (no canvas found)"; fails=$((fails + 1))
    fi
done

# A web-GPU process that ends after 10 frames: the canvas's context is lost,
# the page lives on, and the next page gets a new process.
cat > "$tmp/c.txt" <<EOF
open http://127.0.0.1:$P/crash
wait 6000
open http://127.0.0.1:$P/gl1
wait 6000
quit
EOF
run "$tmp/c.txt" CYBER_WEBGPU_CRASH_TEST=10
p="$tmp/pages.txt"
expect "crash: the coordinator saw the web-GPU process end" "$tmp/coord.txt" "web-GPU process [0-9]+ ended"
expect "crash: the tab heard" "$tmp/tabs.txt" "the web-GPU process ended"
expect "crash: the context is lost" "$p" "lost after [0-9]+ frames"
expect "crash: webglcontextlost" "$p" "\] context lost$"
expect "crash: the page lives on" "$p" "page still runs"
expect "crash: the next page draws with a new one" "$p" "inside 255,127,0,255 outside 0,0,255,255"
never "crash: nothing else ended" <(grep -v "web-GPU process" "$tmp/coord.txt") "ended"

# A blocked driver: neither API is there.
cat > "$tmp/b.txt" <<EOF
open http://127.0.0.1:$P/avail
wait 5000
quit
EOF
run "$tmp/b.txt"
expect "available on this driver: WebGL" "$tmp/pages.txt" "webgl context"
expect "available on this driver: WebGPU" "$tmp/pages.txt" "adapter some"
run "$tmp/b.txt" CYBER_GPU_BLOCK=" "
expect "blocked: no WebGL context" "$tmp/pages.txt" "webgl null"
expect "blocked: no WebGPU adapter" "$tmp/pages.txt" "adapter null"
expect "blocked: the web-GPU process says why" "$tmp/webgpu.txt" "blocked for tests"

# Features the block list takes away (here compute): the pipeline is never
# made, so the command buffer that dispatches it is invalid and none of it
# runs (WebGPU's rule; wgpu-native would abort on the submit).
cat > "$tmp/d.txt" <<EOF
open http://127.0.0.1:$P/gpu
wait 8000
quit
EOF
run "$tmp/d.txt" CYBER_GPU_DENY=1
expect "denied compute: nothing of the invalid command buffer runs" "$tmp/pages.txt" "compute 0,0,0,0"
expect "denied compute: drawing still works" "$tmp/pages.txt" "\[gpu\] inside 255,127,0,255"

# When the site's last tab goes, so does its web-GPU process (and what
# wgpu-native kept); the next tab of the site gets a new one.
cat > "$tmp/i.txt" <<EOF
open http://127.0.0.1:$P/gl1
wait 5000
open http://127.0.0.1:$P/none
wait 5000
open http://127.0.0.1:$P/gl1
wait 5000
quit
EOF
run "$tmp/i.txt"
expect "the site's last tab gone: a new web-GPU process for the next" <(grep -c "^\[webgpu\] for http+127.0.0.1" "$tmp/webgpu.txt") "^2$"
expect "the next tab draws with it" <(grep -c "inside 255,127,0,255 outside 0,0,255,255" "$tmp/pages.txt") "^2$"

kill $server 2> /dev/null

# The decoders under random records, against the real libraries.
(cd "$build" && timeout 1800 ./cb_webgpu_fuzz.exe gl "$batches" 1 > "$tmp/fuzz_gl.txt" 2>&1)
expect "fuzz: WebGL records ($batches batches)" "$tmp/fuzz_gl.txt" "^gl: .* 0 failed$"
(cd "$build" && timeout 1800 ./cb_webgpu_fuzz.exe gpu "$batches" 1 > "$tmp/fuzz_gpu.txt" 2>&1)
expect "fuzz: WebGPU records ($batches batches)" "$tmp/fuzz_gpu.txt" "^gpu: .* 0 failed$"
(cd "$build" && timeout 1800 ./cb_webgpu_fuzz.exe gpuseq "$batches" 1 > "$tmp/fuzz_seq.txt" 2>&1)
expect "fuzz: WebGPU call sequences, used right and wrong" "$tmp/fuzz_seq.txt" "^gpuseq: .* 0 failed$"

echo "$fails failed; logs in $tmp"
exit $((fails > 0))
