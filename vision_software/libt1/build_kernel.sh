#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <src.S> <out.h> <kernel_name>" >&2
    exit 1
fi

src=$1
dst=$2
name=$3

case "$name" in
    [A-Za-z_][A-Za-z0-9_]*) ;;
    *)
        echo "Invalid C identifier for kernel name: $name" >&2
        exit 1
        ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

riscv64-linux-gnu-as -march=rv32imafc_zve32x_zvl256b "$src" -o "$tmp/kernel.o"
riscv64-linux-gnu-objcopy -O binary -j .text "$tmp/kernel.o" "$tmp/kernel.bin"

mkdir -p "$(dirname "$dst")"

{
    echo "/* Auto-generated from $src by build_kernel.sh. */"
    echo "#pragma once"
    echo
    echo "#include <stdint.h>"
    echo
    printf "static const uint32_t %s[] = {\n" "$name"
    od -An -v -tx4 -w16 "$tmp/kernel.bin" |
        awk 'NF { printf "    "; for (i = 1; i <= NF; i++) printf "0x%s, ", $i; printf "\n" }'
    echo "};"
    printf "static const uint32_t %s_count = sizeof(%s) / sizeof(%s[0]);\n" \
        "$name" "$name" "$name"
} > "$dst"

echo "Wrote $dst"
