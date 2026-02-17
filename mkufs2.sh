#!/bin/sh
# Create a PS4-compatible UFS2 image from a directory
# Usage: mkufs2.sh <input_dir> [output_file]

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 <input_dir> [output_file]"
    exit 1
fi

INPUT_DIR="$1"
OUTPUT="${2:-download0.dat}"

# Calculate required size (dir size + 20% overhead + 10MB slack)
BYTES=$(du -s -k "$INPUT_DIR" | awk '{print $1 * 1024}')
OVERHEAD=$((BYTES / 5))                 # 20%
SLACK=$((10 * 1024 * 1024))             # 10MB
TOTAL=$((BYTES + OVERHEAD + SLACK))

# Round up to nearest MB
MB=$(( (TOTAL + 1024*1024 - 1) / (1024*1024) ))

echo "Input size: $BYTES bytes"
echo "Image size: ${MB}MB"

truncate -s "${MB}M" "$OUTPUT"

MD=$(mdconfig -a -t vnode -f "$(realpath "$OUTPUT")")
newfs -O 2 -b 32768 -f 4096 /dev/${MD}

mkdir -p /mnt
mount /dev/${MD} /mnt

# Copy contents safely (includes hidden files)
tar -C "$INPUT_DIR" -cf - . | tar -C /mnt -xpf -

chmod -R 777 /mnt/*

umount /mnt
mdconfig -d -u ${MD}

echo "Created $OUTPUT"
