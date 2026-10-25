# Xenia Edge

[Xenia Edge](https://github.com/has207/xenia-edge) (by has207 / Herman S., BSD licensed) is a Xenia fork that added XeFu support between September 19 and October 8, 2026, independently of this project. This page compares the two, fix by fix, so nothing useful is left behind in either direction. Checked October 24, 2026 against Edge `4d0720457`.

Contact: [issue #319](https://github.com/has207/xenia-edge/issues/319) (our findings, and an offer to work together).

## How the two differ
* **Boot:** Edge runs games the way a modded 360 does, through `xbox.xex`, which picks the XeFu build whitelisted for the game; it also lists OG Xbox games in its game list and reads their XBE directly. We start `xefu7.xex` from a test disc image.
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
