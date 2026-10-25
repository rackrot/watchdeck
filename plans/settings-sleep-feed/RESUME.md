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
- The hook tightening is applied (`87cfcaf`) and proven on a planted file.
- **On the board (2026-10-24, main at `87cfcaf` flashed):**
  - The user formatted the real 128 GB card from Settings > Storage: FAT32, 32 KiB clusters, 29 s.
  - After a reboot: `FAT32 volume at card lba 8192`, mounted read-write, Wi-Fi joined from `wifi.txt` (source: sd).
  - The serial log of the format itself was lost (the capture script buffered; it's fixed now). Its step timings may still be on Settings > Logs.
  - Not done yet: the PC-side `lsblk` and `fsck.vfat -n` check, blocked by the bug below.

## Blocker: Share over USB doesn't enumerate (first run of Share on hardware)

- The board logs `card shared over USB` and `USB: sharing the card (no computer)`. TinyUSB installed on port 1 at boot (HS config, `CONFIG_TINYUSB_MODE_DMA=y`, esp_tinyusb ^2.0.0).
- The PC kernel (J16 on usb 3-2) sees a **high-speed** device, then:
  - 22:21: `Device not responding to setup address`, `device not accepting address N, error -71`
  - 22:22: `device descriptor read/64, error -110`
- So the wiring and PHY are fine, and the device stack doesn't answer control transfers.
- Suspects, untested:
  - DMA mode with buffers the OTG DMA can't reach (PSRAM or cache alignment); try `CONFIG_TINYUSB_MODE_SLAVE`.
  - The TinyUSB task starved or not running.
  - Something about installing with no storage mapped.
  - A full-speed-only test.
- Next session: the user brings a second data cable, so both the CH340 (log) and J16 (MSC) plug into the PC.
  1. Capture the log with `scratchpad/bootlog.py` (now streams and reopens), or rewrite it. Raise `CONFIG_TINYUSB_DEBUG_LEVEL`.
  2. Read `journalctl -k`.
  3. Then the PC check above, and Stop sharing.

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
