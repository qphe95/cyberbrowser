#!/bin/bash
# Fetch the web-GPU process's prebuilt libraries into third_party/prebuilt/files/,
# checking every archive and file against third_party/prebuilt/manifest.
#
#   tools/fetch_prebuilt.sh            download what isn't cached
#   PREBUILT_CACHE=<dir>               archives already downloaded there are used
#   MSYS2_ROOT=<dir> (default C:/msys64)  where the "local" files come from
set -e
cd "$(dirname "$0")/.."
manifest=third_party/prebuilt/manifest
out=third_party/prebuilt/files
msys=${MSYS2_ROOT:-/c/msys64}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$out"
fail() { echo "fetch_prebuilt: $*" >&2; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

declare -A archive_file
while read -r kind a b c d; do
    case "$kind" in
    archive)
        name=$a want=$b url=$c
        base=$(basename "$url")
        f=""
        [ -n "$PREBUILT_CACHE" ] && f=$(find "$PREBUILT_CACHE" -name "$base" -print -quit)
        if [ -z "$f" ]; then
            f="$work/$base"
            echo "downloading $url"
            curl -sSfL -o "$f" "$url" || fail "could not download $url"
        fi
        [ "$(sha "$f")" = "$want" ] || fail "$base: SHA-256 is $(sha "$f"), not $want"
        mkdir -p "$work/$name"
        case "$base" in
        *.zip) unzip -oq "$f" -d "$work/$name" ;;
        *.tar.zst) zst=$(command -v zstd || echo "$msys/usr/bin/zstd"); "$zst" -dc "$f" | tar -xf - -C "$work/$name" ;;
        *) fail "unknown archive type: $base" ;;
        esac
        archive_file[$name]=1 ;;
    file)
        name=$a path=$b want=$c
        [ -n "${archive_file[$name]}" ] || fail "file $path: archive $name not fetched first"
        src="$work/$name/$path"
        [ -f "$src" ] || fail "$path is not in $name"
        [ "$(sha "$src")" = "$want" ] || fail "$path: SHA-256 is $(sha "$src"), not $want"
        cp "$src" "$out/" ;;
    local)
        path=$a want=$b
        src="$msys/$path"
        [ -f "$src" ] || fail "$src is missing (MSYS2 package $c)"
        [ "$(sha "$src")" = "$want" ] || fail "$src: SHA-256 is $(sha "$src"), not $want (MSYS2 package $c)"
        cp "$src" "$out/" ;;
    ''|'#'*) ;;
    *) fail "unknown manifest line: $kind" ;;
    esac
done < "$manifest"
echo "prebuilt libraries in $out:"
ls "$out"
