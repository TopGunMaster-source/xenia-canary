# Xenia Edge

[Xenia Edge](https://github.com/has207/xenia-edge) (by has207 / Herman S., BSD licensed) is a Xenia fork that added XeFu support between September 19 and October 8, 2026, independently of this project. This page compares the two, fix by fix, so nothing useful is left behind in either direction. Checked October 24, 2026 against Edge `4d0720457`.

Contact: [issue #319](https://github.com/has207/xenia-edge/issues/319) (our findings, and an offer to work together).

## How the two differ
* **Boot:** Edge runs games the way a modded 360 does, through `xbox.xex`, which picks the XeFu build whitelisted for the game; it also lists OG Xbox games in its game list and reads their XBE directly. We start `xefu6.xex` (Burnout 3's assigned build; `xefu7.xex` until October 25, 2026, see [[Issues and Fixes]] #40) from a test disc image.
* **Kernel:** Edge has its own guest scheduler (threads as fibers), so its thread-switching fixes don't apply here.

## Ported from Edge (credited to its author)
| Edge commit | Fix | Here |
|---|---|---|
| `e1e4851f7` | `mtfsfi` wrote twice its immediate into the FPSCR, so rounding toward minus infinity became plus infinity (XeFu's x87 floor rounded up) | `482df4213` |
| `a5532a0d1` | `mcrfs` cleared control bits too, so XeFu lost its rounding mode | `6209f2821` |
| `90f47ed85` | NtReadFile/NtWriteFile without a byte offset on an asynchronous handle fail, as on NT | `4f05008ca` |
| `614779434` | Stack arguments past r10 were read from the wrong bytes (64-bit and narrow ones) | `636d56f7d` |
| `6f3029303` | Reads and writes at offset 0 and `FILE_USE_FILE_POINTER_POSITION` | `7840566a2` (adapted) |
| `4c60e0b2f` | Writes set the changed (C) bit of user-mode page table entries; XeFu polls it to find what the Xbox wrote (textures) | `4c402ea4a` (rewritten for our mapping code) |
| `cdf5adebf` | A branch to the host return address from a deeper frame returns to the host from the frame the host entered (XeFu's one-shot trap handler skips its own frame) | `e0124228f` (rewritten: uses our stackpoints' saved LR) |

The return-to-host fix is the cause of [[Issues and Fixes]] #35 (the breakpoints were not XeFu's `abort()` after all).

## Fixed in both, found independently
RSA bignum division (#22), RC4 key size (#14), `MmLockAndMapSegmentArray` (#21), the `\??\` prefix (#19), `XamLoaderLaunchTitleEx` (#18), 4 KB user-mode views (#25), instruction fetch faults at a user-mode branch target and returns to the host sentinel (#33), trap frame `ctr`/`cr`/`xer` at `0x1A0`/`0x1B0`/`0x1B8`.

## Ours, not in Edge (offered in issue #319)
* User APC delivery used the list-entry address as the APC address (#31): wrong pointer to kernel routines, wrong address freed.
* Imports XeFu calls that Xenia doesn't define ([[XeFu Imports]]).
* 4 KB page sync skipping partly mapped 64 KB blocks (#32).

## Not ported (yet), and why
| Edge commit | What | Why not |
|---|---|---|
| `97dbf7bc3` | Defer guest thread switches at DISPATCH_LEVEL | Edge's own scheduler; ours runs DPCs one at a time on a host thread (#17) |
| `13398ab9b` | longjmp landing in the right frame | Different longjmp handling here; revisit if XeFu's `r1`-as-x86-stack code misbehaves |
| `856ebcdb3` | User-mode context with `r1 == 0` gets part of the kernel stack | Not seen here yet; first suspect if user mode faults on its first call |
| `b9fa366e5`, `02d8afd7e`, `7646f6e68` | User page table edge cases (missing 4 KB entry, entries over kernel allocations, entries with no frame) | Different memory code; compare if a user-mode address shows the wrong memory |
| `0788c561e`, `723f54212` | Dropping translated code written through kernel addresses and run through user ones | Our sweep handling differs (#9); revisit for stale-code bugs |

## Side-by-side test with the same files (October 25, 2026)

Same game copy (Xbox Originals digital release, unpacked `default.xbe`; certificate allows only Xbox DVD and media board), same `xefu6.xex`/`xefutitle6.xex` from the console's backward-compatibility update, breakpoints off in both.

| | Our fork (`f0a2442`, test disc) | Edge `3558152` (opens `default.xbe`, `xefu_launcher = xefu6.xex`) |
|---|---|---|
| XeFu boots its Xbox kernel, reads `default.xbe`, RSA check | passes | passes |
| Disc authentication: IOCTL `0x24080` on `D:` | answered (fix #23, `b104a8c`) | unhandled (`NtDeviceIoControlFile(0x24080) - unhandled IOCTL!`), then `0x335142` unhandled |
| Result | game code keeps running (about 144,000 system calls in 100 s); black screen | XeFu gives up within a second (launch data `2, 2`) and returns to the dashboard |

So with these files, Edge stops at the disc authentication check our fork already answers (shown in Edge's own log at `log_level = 3`). Edge's author reports Burnout 3 in game on `xefu6` with his own setup, which must pass that check some other way. He later said he runs the game installed to the hard drive by booting `default.xbe` directly, and that he had seen the unhandled IOCTLs but they hadn't stopped any game he tried. Why his copy passes the check is still unconfirmed; a certificate that allows the hard drive is a guess, not a finding.

## Edge with the disc authentication check answered (October 25, 2026)

Test build `edge-mediacheck@05c1080b8`: Edge `3558152` plus one change, answering IOCTL `0x24080` with success (same as our `b104a8c`). Same game files, `xefu_launcher = xefu6.xex`; build tag and settings checked in the exe and the log.

* **Result:** Burnout 3 boots into the game and is playable: the globe, the region and track select, and races (the log shows `default.xbe`, then track `tracks\US\C3_V1`, crash sounds and EA Trax loading). It closed normally.
* **So:** with these files, the unanswered disc authentication check was the only thing stopping Edge. The fix was posted on Edge's issue #319.
* **Speed:** everything runs in slow motion, not just races: the globe, zooming into a continent, and loading a track are all slow. Edge's author reports about 25 fps with his setup.
  * First run had `log_level = 3` (2.17 GB of log); a second run at `log_level = 2` (166 KB of log) was still slow motion, so logging isn't the cause.
  * Edge's log counts frames presented: about 10,700 in roughly 3¾ minutes (about 47 a second, loading included). That counts XeFu's presented frames, which need not match how often the game itself updates, so it isn't the game's frame rate.
  * Not yet known: whether the game's own code is too slow under Xenia (CPU-bound) or its timing runs slow.


## Unpatched XeFu builds (October 27, 2026)

Edge's author explained on issue #319 that most people run patched XeFu builds, in which the disc authentication check, the region check, the XBE header signature and the section digests are skipped. Our files are Microsoft's stock builds, which run all four; that's why exact RSA, SHA-1 and RC4 mattered here (#14, #22). Edge has since added support for unpatched builds (`47d65f5939` disc authentication, `7180422251` the stock `xbox.xex`, `0427aa7649` key `0x15`) and reports Burnout 3 at 60 fps with memexport disabled (not tested here).

Ported from that set: key `0x15` and `XeKeysGetKey`'s length handling (`0427aa7649`) as `f994494777`. XeFu builds the original Xbox's hard drive key from key `0x15`; Xenia reported it read without writing it (`Key 0x0015 not implemented` in our logs).

## Latest Edge on the same files (October 28, 2026)

Edge `059cc5a5e` (build tag checked in the log), same game files and `xefu_launcher = xefu6.xex`, with `memexport_enable = false` as Edge's author recommends and `log_level = 2`. Result: the game runs a lot better than with `3558152`; the slow motion is gone. The frame rate hasn't been measured yet. Edge's log counts about 247,600 frames for the run, but it has no timestamps, so that count doesn't give a rate.
