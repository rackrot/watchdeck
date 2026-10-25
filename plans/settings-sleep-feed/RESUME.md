# Resume point (2026-10-24, paused for a host reboot)

## Main

- `main` holds waves 1–3 off-board: G1, G2, T1–T4, P1, P2, P4, N1–N5, L2–L4. 104+ commits ahead of `origin/main`, **unpushed** (ask before pushing; Forgejo only).
- Last green check on main: `fw:build` OK, 286 host unit + 28 FatFs tests, daemon lib 441 / device_sim 92 / push_api 24, app 113 tests, Biome, clippy, prek, site build.

## F1, format the card to FAT32 (merged 2026-10-24, `967dbd9`)

FAT32 only: IDF hardcodes `FF_FS_EXFAT 0` and cardfs is FAT32-only, so exFAT is a possible later project.

- Layout: one MBR partition, type 0x0C, from LBA 8192 to the card's end, so the data area is 4 MiB aligned.
- Clusters: 4 KiB under 8 GiB, 8 KiB under 16 GiB, 16 KiB under 32 GiB, otherwise 32 KiB. Cards under 512 MiB are refused.
- Write order: zero the GPTs, run `f_mkfs`, check the latched write error and the backup sectors, then write the MBR last.
- After a good format the board writes `wifi.txt` back from the network in use.
- Reviewed; all six minor findings fixed apart from the hook below. Main passes: `fw:build`, 286 unit tests, 35 FatFs tests (fsck clean on 128 GiB, 8 GiB and GPT images, cut at every write), lint.
- **Open: the hook tightening** (review finding 6). The permission system blocked the agent's edit to `.pre-commit-config.yaml`, so it needs the user's OK:
  - change `no-card-format`'s entry to `'\bf_mkfs\b'`, keeping the `cardmkfs.c` exclude;
  - add a pygrep `'\bcardmkfs_run\b'` hook excluding `^firmware/main/(sdcard\.c|cardmkfs\.[ch])$`;
  - add a pygrep `'\bsdcard_format\b'` hook excluding `^firmware/main/(cardformat\.c|sdcard\.[ch])$`;
  - both new hooks with `files: ^firmware/main/.*\.(c|h)$`.
- On the board (each flash needs the user's yes):
  1. Format the real card.
  2. Share it, then on the PC run `lsblk -o NAME,FSTYPE,SIZE,PARTTYPE` and `fsck.vfat -n /dev/sdX1`.
  3. Stop sharing, then check that the board remounts read-write and Wi-Fi comes back.
  4. Record the format time and the `stack bytes free` svc figure.

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
