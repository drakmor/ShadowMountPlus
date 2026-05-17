#!/bin/sh
# macOS exFAT image builder — requires macOS 10.6.5+ (exFAT and rsync built-in)
# Usage: mkexfat_macos.sh [-t tmp_dir|--tmp-dir tmp_dir|--temp-dir tmp_dir] <input_dir> [output_file]
# (c) @drakmor,@betmoar

set -e

# ── argument validation ───────────────────────────────────────────────────────

USE_TMP_BASE=0
TMP_BASE=""
INPUT_DIR=""
OUTPUT=""

usage() {
    echo "Usage: $0 [-t tmp_dir|--tmp-dir tmp_dir|--temp-dir tmp_dir] <input_dir> [output_file]"
}

while [ $# -gt 0 ]; do
    case "$1" in
        -t|--tmp-dir|--temp-dir)
            if [ -z "$2" ]; then
                echo "Error: missing value for $1"
                usage
                exit 1
            fi
            USE_TMP_BASE=1
            TMP_BASE="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            while [ $# -gt 0 ]; do
                if [ -z "$INPUT_DIR" ]; then
                    INPUT_DIR="$1"
                elif [ -z "$OUTPUT" ]; then
                    OUTPUT="$1"
                else
                    echo "Error: too many positional arguments"
                    usage
                    exit 1
                fi
                shift
            done
            break
            ;;
        -*)
            echo "Error: unknown option: $1"
            usage
            exit 1
            ;;
        *)
            if [ -z "$INPUT_DIR" ]; then
                INPUT_DIR="$1"
            elif [ -z "$OUTPUT" ]; then
                OUTPUT="$1"
            else
                echo "Error: too many positional arguments"
                usage
                exit 1
            fi
            shift
            ;;
    esac
done

if [ -z "$INPUT_DIR" ]; then
    usage
    exit 1
fi

OUTPUT="${OUTPUT:-test.exfat}"
# newfs_exfat interprets relative paths as device names under /dev/ — resolve to absolute
OUTPUT="$(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"

if [ ! -d "$INPUT_DIR" ]; then
    echo "Error: input directory not found: $INPUT_DIR"
    exit 1
fi

if [ ! -f "$INPUT_DIR/eboot.bin" ]; then
    echo "Error: eboot.bin not found in source directory: $INPUT_DIR"
    exit 1
fi

if [ "$USE_TMP_BASE" -eq 1 ]; then
    if [ ! -d "$TMP_BASE" ]; then
        echo "Error: temp directory not found: $TMP_BASE"
        exit 1
    fi
fi

# ── sizing constants ──────────────────────────────────────────────────────────

CLUSTER_SIZE=32768
LARGE_FILE_THRESHOLD=$((1024 * 1024))
META_FIXED=$((32 * 1024 * 1024))   # boot region, upcase, root and misc
MIN_SLACK=$((64 * 1024 * 1024))    # minimum copy/runtime safety margin
SPARE_MIN=$((64 * 1024 * 1024))    # lower bound for dynamic headroom
SPARE_MAX=$((512 * 1024 * 1024))   # upper bound for dynamic headroom
ENTRY_META_BYTES=256

# Single filesystem traversal: collect all file sizes into a temp file.
# FILE_COUNT and RAW_FILE_BYTES are derived from it in one awk pass.
# DIR_COUNT needs a separate traversal (different -type).
if [ "$USE_TMP_BASE" -eq 1 ]; then
    SIZES_FILE=$(mktemp "$TMP_BASE/mkexfat_sizes.XXXXXX")
else
    SIZES_FILE=$(mktemp)
fi
MOUNT_POINT=""
LOOP_DEVICE=""

cleanup() {
    if [ -n "$MOUNT_POINT" ] && mount | grep -q "on $MOUNT_POINT "; then
        umount "$MOUNT_POINT" 2>/dev/null || true
    fi
    if [ -n "$LOOP_DEVICE" ]; then
        hdiutil detach "$LOOP_DEVICE" 2>/dev/null || true
    fi
    if [ -n "$MOUNT_POINT" ]; then
        rmdir "$MOUNT_POINT" 2>/dev/null || true
    fi
    rm -f "$SIZES_FILE"
}
trap cleanup EXIT INT TERM

find "$INPUT_DIR" -type f -exec stat -f '%z' {} \; > "$SIZES_FILE"

read -r FILE_COUNT RAW_FILE_BYTES <<EOF
$(awk '{c++; s += $1} END {print c+0, s+0}' "$SIZES_FILE")
EOF

DIR_COUNT=$(find "$INPUT_DIR" -type d | wc -l | tr -d ' ')

AVG_FILE_BYTES=0
if [ "$FILE_COUNT" -gt 0 ]; then
    AVG_FILE_BYTES=$((RAW_FILE_BYTES / FILE_COUNT))
fi

# exFAT profile selection:
# - large-file sets: 64K cluster
# - small/mixed-file sets: 32K cluster
if [ "$AVG_FILE_BYTES" -ge "$LARGE_FILE_THRESHOLD" ]; then
    CLUSTER_SIZE=65536
fi

# DATA_BYTES re-reads the already-collected sizes from the temp file — no second traversal
DATA_BYTES=$(awk -v cls="$CLUSTER_SIZE" \
  '{s += int(($1 + cls - 1) / cls) * cls} END {print s + 0}' "$SIZES_FILE")
rm -f "$SIZES_FILE"

DATA_CLUSTERS=$(( (DATA_BYTES + CLUSTER_SIZE - 1) / CLUSTER_SIZE ))
FAT_BYTES=$((DATA_CLUSTERS * 4))
BITMAP_BYTES=$(( (DATA_CLUSTERS + 7) / 8 ))
ENTRY_BYTES=$(( (FILE_COUNT + DIR_COUNT) * ENTRY_META_BYTES ))

BASE_TOTAL=$((DATA_BYTES + FAT_BYTES + BITMAP_BYTES + ENTRY_BYTES + META_FIXED))
SPARE_BYTES=$((BASE_TOTAL / 200))   # ~0.5%
if [ "$SPARE_BYTES" -lt "$SPARE_MIN" ]; then SPARE_BYTES=$SPARE_MIN; fi
if [ "$SPARE_BYTES" -gt "$SPARE_MAX" ]; then SPARE_BYTES=$SPARE_MAX; fi
TOTAL=$((BASE_TOTAL + SPARE_BYTES))
MIN_TOTAL=$((RAW_FILE_BYTES + MIN_SLACK))
if [ "$TOTAL" -lt "$MIN_TOTAL" ]; then TOTAL=$MIN_TOTAL; fi

# Round up to nearest MB
MB=$(( (TOTAL + 1024*1024 - 1) / (1024*1024) ))

echo "Input size (raw files): $RAW_FILE_BYTES bytes"
echo "Input size (exFAT alloc): $DATA_BYTES bytes"
echo "Files: $FILE_COUNT, Dirs: $DIR_COUNT"
echo "exFAT profile: -b ${CLUSTER_SIZE} (avg file=$AVG_FILE_BYTES bytes)"
echo "Image size: ${MB}MB"
if [ "$USE_TMP_BASE" -eq 1 ]; then
    echo "Temp directory: $TMP_BASE"
fi

# ── create, attach, format, mount ────────────────────────────────────────────

if [ "$USE_TMP_BASE" -eq 1 ]; then
    MOUNT_POINT=$(mktemp -d "$TMP_BASE/mkexfat.XXXXXX")
else
    MOUNT_POINT=$(mktemp -d)
fi

# mkfile -n creates a sparse file without writing zeros (equivalent to Linux truncate)
mkfile -n "${MB}m" "$OUTPUT"

# Attach as a block device first — newfs_exfat requires real sector geometry via ioctl
# and cannot format a raw file directly
ATTACH_OUTPUT=$(hdiutil attach \
    -imagekey diskimage-class=CRawDiskImage \
    -nomount "$OUTPUT")
LOOP_DEVICE=$(printf '%s\n' "$ATTACH_OUTPUT" | awk 'NR==1 {print $1; exit}')
if [ -z "$LOOP_DEVICE" ]; then
    echo "Error: failed to determine attached device: $OUTPUT"
    exit 1
fi

# newfs_exfat -b takes bytes-per-cluster directly
newfs_exfat -b "$CLUSTER_SIZE" "$LOOP_DEVICE"

mount -t exfat "$LOOP_DEVICE" "$MOUNT_POINT"

rsync -r --progress "$INPUT_DIR"/ "$MOUNT_POINT"/

umount "$MOUNT_POINT"
hdiutil detach "$LOOP_DEVICE"
LOOP_DEVICE=""   # prevent double-detach in trap

echo "Created $OUTPUT"
