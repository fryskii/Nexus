#!/bin/bash
# Builds and runs the core fuzzer with AddressSanitizer + UndefinedBehaviorSanitizer.
# usage: tests/fuzz/run.sh <work-dir> [seconds]   (needs clang and libclang-rt-dev)
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${1:?work dir}; SECS=${2:-300}
mkdir -p "$WORK/corpus"
clang++ -std=c++20 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -I"$ROOT/src" \
  -DNEXUS_VERSION_STR='"0.0"' "$ROOT/tests/fuzz/fuzz_core.cpp" "$ROOT"/src/core/{Format,Settings,Smbios,DiagnosticsRules,Filter,Regex}.cpp \
  -o "$WORK/fuzz_core"
cd "$WORK"
exec ./fuzz_core -max_total_time="$SECS" -max_len=4096 -timeout=5 -rss_limit_mb=2048 -print_final_stats=1 corpus
