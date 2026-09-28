#!/bin/sh
# Disassemble the dumped image: tools/dis.sh <start_hex> <end_hex>   (e.g. 82151640 82151680)
here=$(dirname "$0")
"$here/../../recomp/rexglue-sdk/tools/binutils/powerpc-none-elf-objdump.exe" -D -b binary -m powerpc:common64 -EB \
  --adjust-vma=0x82000000 --start-address=0x$1 --stop-address=0x$2 "$here/../runs/image.bin" | tail -n +8
