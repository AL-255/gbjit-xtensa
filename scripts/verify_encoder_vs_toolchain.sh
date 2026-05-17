#!/usr/bin/env bash
# Assemble the same mnemonics our encoder emits with the IDF-bundled
# xtensa-esp32s3-elf-as, dump the bytes, and diff against the words listed
# in tests/test_encoder_bits.c. Anything that mismatches is a real encoder
# bug — the in-tree test passes because the simulator made the same choice
# as the encoder; this one is the ground-truth check.
#
# Requires IDF env to be sourced first.

set -euo pipefail

if ! command -v xtensa-esp32s3-elf-as >/dev/null; then
    echo "ERR: source ESP-IDF export.sh before running this script" >&2
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/probe.S" <<'EOF'
    .text
    .global _start
_start:
    add  a3, a4, a5
    sub  a3, a4, a5
    and  a3, a4, a5
    or   a3, a4, a5
    xor  a3, a4, a5
    mov  a3, a4
    addi a3, a4, -1
    addi a3, a4, 16
    movi a4, 0x42
    movi a4, -1
    l8ui   a3, a11, 16
    s8i    a3, a11, 16
    l32i   a3, a11, 16
    s32i   a3, a11, 16
    l16ui  a3, a11, 16
    s16i   a3, a11, 16
    jx     a4
    callx0 a4
    ret
    slli  a3, a4, 1
    slli  a3, a4, 16
    srli  a3, a4, 8
    srai  a3, a4, 1
    extui a3, a4, 0, 8
    extui a3, a4, 8, 8
EOF

xtensa-esp32s3-elf-as --target-align -o "$WORK/probe.o" "$WORK/probe.S"
xtensa-esp32s3-elf-objdump -d --insn-width=4 "$WORK/probe.o" \
  | awk '/^[[:space:]]*[0-9a-f]+:/{
        # parse: "  N:  AA BB CC          mnemonic ..."
        pc = $1; sub(":","",pc);
        line = $0;
        sub(/^[[:space:]]*[0-9a-f]+:[[:space:]]+/, "", line);
        # bytes: first 1-4 hex pairs separated by spaces
        n = split(line, parts, /[[:space:]]+/);
        bytes=""; nb=0;
        for (i=1; i<=n; i++) {
            if (parts[i] ~ /^[0-9a-f][0-9a-f]$/) { bytes = bytes parts[i]; nb++; }
            else break;
        }
        # mnemonic is the rest
        mnem="";
        for (j=i; j<=n; j++) mnem = mnem parts[j] " ";
        gsub(/[[:space:]]+$/, "", mnem);
        # Reverse byte order (little-endian word) for 3-byte instructions
        if (nb == 3) {
            word = substr(bytes,5,2) substr(bytes,3,2) substr(bytes,1,2);
            printf "%-30s 0x%s\n", mnem, word;
        } else if (nb == 2) {
            word = substr(bytes,3,2) substr(bytes,1,2);
            printf "%-30s 0x%s (narrow)\n", mnem, word;
        }
    }'
