#!/bin/bash
# run_qemu.sh -- Launches PUNIX with a secondary PUNIX-FS image on /dev/hdb
#
# punix_fs.img is the PUNIX root filesystem extracted from disk.img at
# sector 256 (FS_SUPERBLOCK_SECTOR).  It is created by:
#   dd if=disk.img of=punix_fs.img bs=512 skip=256
# Run build.sh first to regenerate disk.img, then this script.

if [ ! -f punix_fs.img ]; then
    echo "punix_fs.img not found — extracting from disk.img ..."
    dd if=disk.img of=punix_fs.img bs=512 skip=256 status=none
    echo "Done."
fi

echo "Launching QEMU with both PUNIX block devices..."
qemu-system-i386 \
  -accel kvm \
  -drive file=disk.img,format=raw,media=disk,index=0 \
  -drive file=punix_fs.img,format=raw,media=disk,index=1 \
  -boot c \
  -display gtk \
  -serial mon:stdio
