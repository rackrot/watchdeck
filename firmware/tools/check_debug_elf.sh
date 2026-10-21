#!/usr/bin/env bash
# Structural check of the stamped debug ELF (see stamp_build_id.py), run by CI after the build.
# Needs riscv32-esp-elf-readelf and esptool on PATH, which IDF's export.sh provides.
# Usage: check_debug_elf.sh [BUILD_DIR]   (default: build)
set -euo pipefail
export LC_ALL=C

build="${1:-build}"
elf="$build/watchdeck.elf"
debug="$build/watchdeck.debug.elf"
bin="$build/watchdeck.bin"
image_addr="0x30100000"

fail() {
    echo "check_debug_elf: FAIL: $*" >&2
    exit 1
}

for f in "$elf" "$debug" "$bin"; do
    [ -f "$f" ] || fail "$f does not exist"
done

elf_sha="$(sha256sum "$elf")"
elf_sha="${elf_sha%% *}"

# 2. The build id is the first 20 bytes (40 hex digits) of the ELF's SHA-256.
notes="$(riscv32-esp-elf-readelf -n "$debug")"
build_id="$(awk '/Build ID:/ { print $3; exit }' <<<"$notes")"
[ -n "$build_id" ] || fail "$debug has no GNU build-id note"
[ "$build_id" = "${elf_sha:0:40}" ] ||
    fail "$debug build id $build_id is not the first 40 hex digits of sha256($elf) ${elf_sha:0:40}"

# 3. Every LOAD segment in the debug copy has a file size of 0, and the first and lowest is at image_addr.
# readelf -lW LOAD rows: LOAD Offset VirtAddr PhysAddr file-size mem-size Flg Align
loads="$(riscv32-esp-elf-readelf -lW "$debug" | awk '$1 == "LOAD" { print $3, $5 }')"
[ -n "$loads" ] || fail "$debug has no LOAD segment"
while read -r vaddr filesz; do
    [[ "$vaddr" =~ ^0x[0-9a-fA-F]+$ && "$filesz" =~ ^0x[0-9a-fA-F]+$ ]] ||
        fail "cannot parse LOAD row: vaddr '$vaddr' file size '$filesz'"
    [ "$((filesz))" -eq 0 ] || fail "$debug has a LOAD segment at $vaddr with file size $filesz, so it still holds loadable bytes"
done <<<"$loads"
first="$(awk 'NR == 1 { print $1 }' <<<"$loads")"
lowest="$(awk '{ print $1 }' <<<"$loads" | while read -r v; do echo "$((v))"; done | sort -n | head -n 1)"
[ "$((first))" -eq "$((image_addr))" ] || fail "the first LOAD segment is at $first, not $image_addr"
[ "$lowest" -eq "$((image_addr))" ] || fail "the lowest LOAD segment is at $(printf '0x%x' "$lowest"), not $image_addr"

# 4. The linked ELF carries no build-id note, or elf2image's stored hash would not cover the stamp.
elf_notes="$(riscv32-esp-elf-readelf -n "$elf")"
if grep -q 'Build ID:' <<<"$elf_notes"; then
    fail "$elf has a GNU build-id note"
fi

# 5. The ELF SHA-256 stored in the app image matches. CONFIG_APP_RETRIEVE_LEN_ELF_SHA=64 stores all
# 64 hex digits and esptool prints them, so a shorter value is an error, not a prefix match. esptool 5 spells the command image-info, esptool 4 image_info.
if info="$(esptool image-info "$bin" 2>/dev/null)"; then :; else info="$(esptool image_info "$bin")"; fi
stored="$(awk -F': *' '/ELF file SHA256/ { print $2; exit }' <<<"$info")"
[ -n "$stored" ] || fail "esptool image-info shows no ELF file SHA256 for $bin"
[[ "$stored" =~ ^[0-9a-fA-F]{64}$ ]] || fail "$bin stores '$stored', not a full 64-digit ELF SHA256"
[ "${stored,,}" = "$elf_sha" ] || fail "$bin stores ELF SHA256 $stored but sha256($elf) is $elf_sha"

echo "check_debug_elf: OK: build id ${build_id}, ${stored:0:16}... matches the image, no loadable bytes"
