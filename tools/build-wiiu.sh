#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/vbcgx"
build_image='ghcr.io/extremscorner/libogc2@sha256:237d779ab1ef1c0d8d828dd585afcd9ba990af5624f517f26ba2d949d237fc4b'
smb_revision='92e087c15e79e6adfdb3ff9a7ba81243c3c741d0'
mocha_revision='1a7d6034fcfd40468fcda5908ae32d019c7e7224'
dvm_revision='0b1430ab32fe18617db93c9b9d3045282c67c531'
mkdir -p "$cache_dir"
# (its own checkout: libsmb2 builds in place, and the Wii build's cache must stay a Wii build)
if [ ! -d "$cache_dir/libsmb2-wiiu/.git" ] && [ ! -f "$cache_dir/libsmb2-wiiu/.git" ]; then
    git clone https://github.com/dborth/libsmb2.git "$cache_dir/libsmb2-wiiu"
fi
if [ ! -d "$cache_dir/libmocha/.git" ]; then
    git clone https://github.com/dborth/libmocha.git "$cache_dir/libmocha"
fi
if [ ! -d "$cache_dir/libdvm/.git" ]; then
    git clone https://github.com/dborth/libdvm.git "$cache_dir/libdvm"
fi
git -C "$cache_dir/libsmb2-wiiu" checkout --detach "$smb_revision"
git -C "$cache_dir/libmocha" checkout --detach "$mocha_revision"
git -C "$cache_dir/libdvm" checkout --detach "$dvm_revision"
git -C "$cache_dir/libdvm" submodule update --init --recursive
podman run --rm --volume "$project_dir:/src:Z" --volume "$cache_dir/libsmb2-wiiu:/deps/libsmb2:Z" \
    --volume "$cache_dir/libmocha:/deps/libmocha:Z" --volume "$cache_dir/libdvm:/deps/libdvm:Z" \
    --workdir /src "$build_image" sh -eu -c '
        make -C /deps/libmocha install -j2
        make -C /deps/libdvm install -j2
        make -C /deps/libsmb2 -f Makefile.platform wiiu_install -j2
        make -f Makefile.wiiu -j2 "$@"
    ' sh "$@"
