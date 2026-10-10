#!/bin/sh
# Regenerates Classes/zstd/zstd.c (Stage S / S.2) from an UNPACKED zstd 1.5.7 release tree.
#   web/tools/vendor-zstd.sh /path/to/zstd-1.5.7 [workdir]
# The amalgamation is zstd's own build/single_file_libs/zstd-in.c minus the dictionary builder,
# the threading/pool/zstdmt units and the ZSTD_MULTITHREAD define. Pass a fresh EMPTY workdir.
set -e
SRC="$1"; WORK="${2:-$(mktemp -d)}"
[ -f "$SRC/lib/zstd.h" ] || { echo "usage: $0 <zstd-1.5.7 tree> [empty workdir]"; exit 1; }
grep -q 'ZSTD_VERSION_MINOR    5' "$SRC/lib/zstd.h" && grep -q 'ZSTD_VERSION_RELEASE  7' "$SRC/lib/zstd.h" || { echo "not 1.5.7 — the .emod spec pins 1.5.7"; exit 1; }
sed -n '/^#define DEBUGLEVEL/,$p' "$SRC/build/single_file_libs/zstd-in.c" \
  | grep -v 'dictBuilder/\|threading.c\|common/pool.c\|zstdmt_compress.c\|^#define ZSTD_MULTITHREAD' > "$WORK/zstd-in.c"
python3 -I "$SRC/build/single_file_libs/combine.py" -r "$SRC/lib" -x legacy/zstd_legacy.h -o "$WORK/zstd.c" "$WORK/zstd-in.c" >/dev/null
OUT="$(cd "$(dirname "$0")/../.." && pwd)/Classes/zstd"
cp "$WORK/zstd.c" "$SRC/lib/zstd.h" "$SRC/lib/zstd_errors.h" "$SRC/LICENSE" "$OUT/"
echo "wrote $OUT"
