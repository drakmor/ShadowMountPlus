#!/bin/sh
# For WSL2/Ubuntu/Debian: sudo apt-get install -y exfatprogs exfat-fuse fuse3 rsync
# Create an exFAT image from a directory
# Usage: mkexfat.sh [-t tmp_dir|--tmp-dir tmp_dir] <input_dir> [output_file]

set -e

TMP_BASE="/tmp"
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

if [ ! -d "$INPUT_DIR" ]; then
    echo "Error: input directory not found: $INPUT_DIR"
    exit 1
fi

if [ ! -f "$INPUT_DIR/eboot.bin" ]; then
    echo "Error: eboot.bin not found in source directory: $INPUT_DIR"
    exit 1
fi

if [ ! -d "$TMP_BASE" ]; then
    echo "Error: temp directory not found: $TMP_BASE"
    exit 1
fi
if [ ! -w "$TMP_BASE" ] || [ ! -x "$TMP_BASE" ]; then
    echo "Error: temp directory is not writable/executable: $TMP_BASE"
    exit 1
fi

# More accurate sizing for exFAT:
# - file payload rounded to cluster size
# - FAT + allocation bitmap estimates from cluster count
# - directory/entry metadata estimate
# - fixed metadata and runtime headroom
CLUSTER_SIZE=32768
MKFS_CLUSTER_ARG="32K"
LARGE_FILE_THRESHOLD=$((1024 * 1024))
META_FIXED=$((32 * 1024 * 1024))   # boot region, upcase, root and misc
MIN_SLACK=$((64 * 1024 * 1024))    # minimum copy/runtime safety margin
SPARE_MIN=$((64 * 1024 * 1024))    # lower bound for dynamic headroom
SPARE_MAX=$((512 * 1024 * 1024))   # upper bound for dynamic headroom
ENTRY_META_BYTES=256

FILE_COUNT=$(find "$INPUT_DIR" -type f | wc -l | tr -d ' ')
DIR_COUNT=$(find "$INPUT_DIR" -type d | wc -l | tr -d ' ')
RAW_FILE_BYTES=$(find "$INPUT_DIR" -type f -printf '%s\n' | \
  awk '{s += $1} END {print s + 0}')

AVG_FILE_BYTES=0
if [ "$FILE_COUNT" -gt 0 ]; then
    AVG_FILE_BYTES=$((RAW_FILE_BYTES / FILE_COUNT))
fi

# exFAT profile selection (same idea as UFS2 profile):
# - large-file sets: 64K cluster
# - small/mixed-file sets: 32K cluster
if [ "$AVG_FILE_BYTES" -ge "$LARGE_FILE_THRESHOLD" ]; then
    CLUSTER_SIZE=65536
    MKFS_CLUSTER_ARG="64K"
fi

DATA_BYTES=$(find "$INPUT_DIR" -type f -printf '%s\n' | \
  awk -v cls="$CLUSTER_SIZE" '{s += int(($1 + cls - 1) / cls) * cls} END {print s + 0}')
DATA_CLUSTERS=$(( (DATA_BYTES + CLUSTER_SIZE - 1) / CLUSTER_SIZE ))
FAT_BYTES=$((DATA_CLUSTERS * 4))
BITMAP_BYTES=$(( (DATA_CLUSTERS + 7) / 8 ))
ENTRY_BYTES=$(( (FILE_COUNT + DIR_COUNT) * ENTRY_META_BYTES ))

BASE_TOTAL=$((DATA_BYTES + FAT_BYTES + BITMAP_BYTES + ENTRY_BYTES + META_FIXED))
SPARE_BYTES=$((BASE_TOTAL / 200))   # ~0.5%
if [ "$SPARE_BYTES" -lt "$SPARE_MIN" ]; then
    SPARE_BYTES=$SPARE_MIN
fi
if [ "$SPARE_BYTES" -gt "$SPARE_MAX" ]; then
    SPARE_BYTES=$SPARE_MAX
fi
TOTAL=$((BASE_TOTAL + SPARE_BYTES))
MIN_TOTAL=$((RAW_FILE_BYTES + MIN_SLACK))
if [ "$TOTAL" -lt "$MIN_TOTAL" ]; then
    TOTAL=$MIN_TOTAL
fi

# Round up to nearest MB
MB=$(( (TOTAL + 1024*1024 - 1) / (1024*1024) ))

echo "Input size (raw files): $RAW_FILE_BYTES bytes"
echo "Input size (exFAT alloc): $DATA_BYTES bytes"
echo "Files: $FILE_COUNT, Dirs: $DIR_COUNT"
echo "exFAT profile: -c $MKFS_CLUSTER_ARG (avg file=$AVG_FILE_BYTES bytes)"
echo "Image size: ${MB}MB"
echo "Temp directory: $TMP_BASE"

truncate -s "${MB}M" "$OUTPUT"
mkfs.exfat -c "$MKFS_CLUSTER_ARG" "$OUTPUT"
MOUNT_DIR=$(mktemp -d "$TMP_BASE/mkexfat.XXXXXX")

cleanup() {
    if mountpoint -q "$MOUNT_DIR" 2>/dev/null; then
        umount "$MOUNT_DIR" || true
    fi
    rmdir "$MOUNT_DIR" 2>/dev/null || true
}

trap cleanup EXIT INT TERM

# exfat-fuse can open a plain file without a kernel loop device.  mount -o loop
# uses libmount/losetup and can fail on some removable exFAT targets (~large images).
if ! mount -t exfat-fuse "$OUTPUT" "$MOUNT_DIR"; then
    mount -t exfat-fuse -o loop "$OUTPUT" "$MOUNT_DIR"
fi
rsync -r --info=progress2 "$INPUT_DIR"/ "$MOUNT_DIR"/

echo "Created $OUTPUT"
