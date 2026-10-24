# Resume point (2026-10-24, paused for a host reboot)

## Main

- `main` holds waves 1–3 off-board: G1, G2, T1–T4, P1, P2, P4, N1–N5, L2–L4. 104+ commits ahead of `origin/main`, **unpushed** (ask before pushing; Forgejo only).
- Last green check on main: `fw:build` OK, 286 host unit + 28 FatFs tests, daemon lib 441 / device_sim 92 / push_api 24, app 113 tests, Biome, clippy, prek, site build.

## In flight: F1, format the card to FAT32 from Settings > Storage

User request (2026-10-24): format to FAT32/exFAT from settings, handle large cards (theirs is 128 GB), no backup needed.
Decided: **FAT32 only** (IDF hardcodes `FF_FS_EXFAT 0`, and cardfs is FAT32-only; exFAT is a possible later project).

- Branch `worktree-agent-ac765236d76e73704`, worktree `.claude/worktrees/agent-ac765236d76e73704`, WIP commit `0522a92`. Not merged.
- Done:
  - `cardmkfs.c/.h`: pure layout/MBR/`f_mkfs`. Order: zero LBA 0–33 and the last 33 sectors, `f_mkfs` `FM_FAT32|FM_SFD` from LBA 2048, check the backup boot sector/FSINFO, write the MBR last.
  - `sdcard_format` in sdcard.c, `cardformat.c/.h` (the svc job), `wifi_save_current()`.
  - Share refused while formatting; the Format button in `tab_storage.c` (an `arm_t` two-tap).
  - Hooks split: `no-card-format` (`f_mkfs`, excludes `cardmkfs.c`) and `no-tinyusb-app-mount`.
  - CLAUDE.md updated. `fw:build` passed.
- Failing: `mise run fw:test:fatfs`, 4 of the 5 new `test_format.c` cases.
  - Opening `build/format-*.img` fails, probably because IDF's linux VFS refuses paths outside its prefixes. Try a raw `syscall(SYS_openat, ...)`.
  - `fopen("/fmt/old.txt")` on the second drive fails. Try re-pointing drive 0 at the window instead.
- To do:
  1. Fix those tests and check `n_fatent` (128 GiB → 4193122, 8 GiB → 2091778, 16 GiB GPT → 2094466). Capture the `fsck.vfat -n -v` and `file -s` output.
  2. `site/src/wifi.md` lines 30 and 38 still say the board never formats.
  3. `fw:build`, `fw:test:unit`, `mise run lint`, and `prek run --files` on the new files. Check that the hook fires on a planted `f_mkfs(` elsewhere.
  4. Review. Points to check:
     - The data area is only 1 MiB aligned on the card (the partition starts at LBA 2048 and align is relative to the volume). Start at 8192 for 4 MiB.
     - `wifi_save_current` saves while still retrying.
     - The format time is unmeasured (estimated 30–40 s for 128 GB).
  5. Squash into Conventional Commits and merge to main.
- On the board, when the user is ready (every flash needs their yes):
  1. Format the real card.
  2. Share it, then on the PC run `lsblk -o NAME,FSTYPE,SIZE,PARTTYPE` and `fsck.vfat -n /dev/sdX1`.
  3. Stop sharing, then check the board remounts read-write and Wi-Fi comes back.

## Waiting on the user

- **Batch A** at the panel: the G2 card self-test with power cuts, Share/Stop sharing, T1–T4 acceptance, the P0 backlight probe (branch `worktree-agent-a2d05a0f97e403ffb` @ `6d212dd`), P1 touch, N3 feed page. The card backup gate is **waived** (user, 2026-10-24).
- After A: P3 (sleep image download, `sleep.cfg` writes). Then T4 reads `sleep.cfg`, and the backlight becomes a slider if P0 passes.
- Batch B: P2/P3 on the board; N2–N5 end to end (CLI, LAN curl with `lan = true`, HA); the L4 boot logo; memory runs. Window checks for L3/P4/N4 in `mise run app:dev`. H1 docs (the CLAUDE.md push API facts).

## Standing rules

- Never `erase_flash`; don't OTA the C6.
- Every flash needs the user's yes.
- No secrets, tokens or `identity.key` in output or commits.
- Forgejo (`fj`), never `gh`; no attribution lines.
- Don't hand-edit generated font/logo files.
