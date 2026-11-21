#!/bin/bash
# K.0 fuzz: build the four libFuzzer targets (.emod reader, .eden/.gz/.zip converter, Json.cpp, net framer + SNAPZ)
# into <out> (default native/build-fuzz). Run from the repo root.
# Linux: clang's own -fsanitize=fuzzer. macOS: Apple clang has no fuzzer runtime, so set FUZZER_LIB to Homebrew LLVM's
# libclang_rt.fuzzer_osx.a (and use Apple clang for the objects).
set -e
B=${1:-native/build-fuzz}; S=port/tools/fuzz; mkdir -p "$B"
CC=${CC:-clang}; CXX=${CXX:-clang++}
if [ -n "$FUZZER_LIB" ]; then L="-fsanitize=address,undefined $FUZZER_LIB -lz"; else L="-fsanitize=address,undefined,fuzzer -lz"; fi
F="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-sanitize=vptr -fsanitize-coverage=inline-8bit-counters,pc-table,trace-cmp"
I="-IClasses -Iport/src/shim/vendor -Iport/src/net -Iport/src/shim/vendor/zstd"
[ -f "$B/zstd.o" ] || $CC -O2 -g -fsanitize=address -c port/src/shim/vendor/zstd/zstd.c -o "$B/zstd.o"
for s in Classes/EdenWorldStore.cpp Classes/EdenWorldSource.cpp Classes/Json.cpp port/src/net/Snapz.cpp; do
  o=$B/$(basename $s .cpp).o; [ "$o" -nt "$s" ] || $CXX -std=c++17 $F $I -c $s -o "$o"; done
$CXX -std=c++17 $F $I $S/fz_json.cpp "$B/Json.o" $L -o "$B/fz_json"
$CXX -std=c++17 $F $I $S/fz_net.cpp "$B/Snapz.o" $L -o "$B/fz_net"
$CXX -std=c++17 $F $I $S/fz_emod.cpp "$B/EdenWorldStore.o" "$B/zstd.o" $L -o "$B/fz_emod"
$CXX -std=c++17 $F $I $S/fz_eden.cpp "$B/EdenWorldSource.o" "$B/EdenWorldStore.o" "$B/zstd.o" $L -o "$B/fz_eden"
