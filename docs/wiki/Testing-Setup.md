# Testing Setup

## What you need (from your own console)

* XeFu's files from the hard drive's `Compatibility` folder: `config.bin`, `xbox.xex`, `xefu.xex`, `xefu7.xex`, `xefutitle7.xex`, `Dash\…`.
* The game: Burnout 3's Xbox Originals package, unpacked to its `default.xbe`.

## Steps

1. Build a test disc image: the original Xbox disc (XDVDFS) plus `default.xex` = `xefu7.xex` and `xefutitle7.xex` at the root. Xenia mounts it as `\Device\Cdrom0` and starts XeFu, which then opens the game's `default.xbe`.
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
