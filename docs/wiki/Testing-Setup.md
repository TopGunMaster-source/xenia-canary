# Testing Setup

## What you need (from your own console)

* XeFu's files from the hard drive's `Compatibility` folder: `config.bin`, `xbox.xex`, all the `xefu*.xex` builds and their `xefutitle*.xex` tables, `Dash\…`.
* **The right XeFu build for the game.** XeFu is not one program: the 360 has several builds (`xefu.xex`, `xefu6.xex`, `xefu7.xex`, …), and Microsoft assigned each approved game to one of them. On a console, `xbox.xex` looks the game up and starts its build; a build that isn't the game's hands back to `xbox.xex` (launch data `2, 2`). **Burnout 3 uses `xefu6.xex`** (per Xenia Edge's author, who runs it in game with that build; not yet checked against `xbox.xex`'s own list). Our copy of the Compatibility folder has no `xefu6.xex` yet.
* The game: Burnout 3's Xbox Originals package, unpacked to its `default.xbe`.

## Before every test (checklist)

1. The XeFu build is the one the game is assigned to (see above), and the run says which build it used.
2. The build of Xenia is the one meant: the log's first line (`Build: xefu-backcompat@…`) matches the commit.
3. Nothing from an earlier run is still open or locking files.
4. The setup is compared with how a real console does it before a failure is blamed on an emulator, ours or anyone else's.

## Steps

1. Build a test disc image: the original Xbox disc (XDVDFS) plus `default.xex` = the game's XeFu build and its `xefutitle` table at the root. **Until October 25, 2026 every test used `xefu7.xex`, which is not Burnout 3's build** (see [[Issues and Fixes]] #40). Xenia mounts it as `\Device\Cdrom0` and starts XeFu, which then opens the game's `default.xbe`.
2. Put the Compatibility files in a folder and set `compatibility_root` to it in `xenia-canary.config.toml`.
3. For diagnosis runs, also set:
   * `log_level = 3`
   * `relaunch_on_title_launch = false` (keeps the log of the run that failed; Xenia exits when the test ends)
   * `log_file_max_mb` (default 2048) to keep logs from filling the disk
4. Run Xenia with the disc image. A small `.bat` in each build's folder does that.

* Builds are now tested from one reused folder, `test`, with `log_level = 2`, `log_file_max_mb = 350` (staging limit) and `break_on_debugbreak = false` (retail behaviour), so test runs don't pile up on the disk. A crash leaves `xenia_crash.dmp`; the build's `xenia_canary_windows_pdb` artifact symbolizes it.

## Reading a run

* `xenia.log`: look for `BACKCOMPAT` lines, especially `the Xbox kernel is returning to the dashboard`. Its x86 stack shows the Xbox's NTSTATUS for the failure (for example `C0000190` for a bad signature or digest).
* `xefu_heartbeat.txt`, every 3 s: the x86 EIP, system calls, faults, timer DPCs and interrupts, and each guest thread's last kernel call. A stuck EIP means a hang.
* `xefu_syscalls.txt`: the last 16384 user-mode system calls, written at the first stall.
* `xefu_xbox_ram.bin`: the emulated Xbox's 64 MB of RAM at the first stall. Used, for example, to compare loaded sections against the XBE byte for byte.
