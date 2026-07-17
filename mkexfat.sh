#!/bin/sh
# For WSL2/Ubuntu/Debian: sudo apt-get install -y exfatprogs exfat-fuse fuse3 rsync
# Create a 64 KiB-cluster exFAT image for the LVD type-5/BFS sdimg profile.
# Usage: mkexfat.sh <input_dir> [output_file]

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 <input_dir> [output_file]"
    exit 1
fi

INPUT_DIR="$1"
OUTPUT="${2:-test.exfat}"

if [ ! -d "$INPUT_DIR" ]; then
    echo "Error: input directory not found: $INPUT_DIR"
    exit 1
fi

if [ ! -f "$INPUT_DIR/eboot.bin" ]; then
    echo "Error: eboot.bin not found in source directory: $INPUT_DIR"
    exit 1
fi

# More accurate sizing for exFAT:
# - file payload rounded to cluster size
# - FAT + allocation bitmap estimates from cluster count
# - directory/entry metadata estimate
# - fixed metadata and runtime headroom
CLUSTER_SIZE=65536
MKFS_CLUSTER_ARG="64K"
META_FIXED=$((32 * 1024 * 1024))   # boot region, upcase, root and misc
MIN_SLACK=$((64 * 1024 * 1024))    # minimum copy/runtime safety margin
SPARE_MIN=$((64 * 1024 * 1024))    # lower bound for dynamic headroom
SPARE_MAX=$((512 * 1024 * 1024))   # upper bound for dynamic headroom
ENTRY_META_BYTES=256

FILE_COUNT=$(find "$INPUT_DIR" -type f | wc -l | tr -d ' ')
DIR_COUNT=$(find "$INPUT_DIR" -type d | wc -l | tr -d ' ')
RAW_FILE_BYTES=$(find "$INPUT_DIR" -type f -printf '%s\n' | \
  awk '{s += $1} END {print s + 0}')

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
echo "exFAT LVD/BFS profile: -c $MKFS_CLUSTER_ARG (fixed 64 KiB cluster)"
echo "Image size: ${MB}MB"

truncate -s "${MB}M" "$OUTPUT"
mkfs.exfat -c "$MKFS_CLUSTER_ARG" "$OUTPUT"
mkdir -p /mnt/exfat
mount -t exfat-fuse -o loop "$OUTPUT" /mnt/exfat
rsync -r --info=progress2 "$INPUT_DIR"/ /mnt/exfat/

umount /mnt/exfat

echo "Created $OUTPUT"
