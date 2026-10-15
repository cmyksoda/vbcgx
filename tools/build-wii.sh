#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/vbcgx"
build_image='ghcr.io/extremscorner/libogc2@sha256:237d779ab1ef1c0d8d828dd585afcd9ba990af5624f517f26ba2d949d237fc4b'
smb_revision='92e087c15e79e6adfdb3ff9a7ba81243c3c741d0'
mkdir -p "$cache_dir"
if [ ! -d "$cache_dir/libsmb2/.git" ]; then
    git clone https://github.com/dborth/libsmb2.git "$cache_dir/libsmb2"
fi
git -C "$cache_dir/libsmb2" checkout --detach "$smb_revision"
podman run --rm --volume "$project_dir:/src:Z" --volume "$cache_dir/libsmb2:/deps/libsmb2:Z" \
    --workdir /src "$build_image" sh -eu -c '
        make -C /deps/libsmb2 -f Makefile.platform wii_install -j2
        make -f Makefile.wii -j2
    '
