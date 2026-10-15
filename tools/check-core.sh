#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d -t vbcgx-check.XXXXXX)
trap 'rm -rf -- "$build_dir"' EXIT
cmake -S "$project_dir" -B "$build_dir" -DVBC_HOST_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --parallel 2
python3 "$project_dir/tools/make-test-rom.py" "$build_dir/diagnostic.vb"
"$build_dir/core-smoke" "$build_dir/diagnostic.vb"
"$build_dir/input-check"
"$build_dir/video-check"
