# Original Xbox games through the 360's emulator (xefu)

Research notes for running original Xbox games in Xenia by running the Xbox
360's own backward-compatibility emulator, xefu, the way the console does.
The test title is Burnout 3: Takedown (Xbox title ID `4541005B`), owned as an
"Xbox Originals" Games on Demand package (content type `00005000`, stored
under the emulator's title ID `FFFE07D2`).

None of Microsoft's files are in this repository. They come from the user's
own console: the hard drive's Compatibility folder, and the emulator's title
update (`FFFE07D2\000B0000\tu20075c00_00000000`), which holds `xefu1_1` to
`xefu7b` and the per-title tables `xefutitle5` to `xefutitle7b`.

## How to run the test

1. Build a disc image from the game's package with xefu added at the root:
   the original Xbox disc (XDVDFS, descriptor at `0x10000`) plus
   `default.xex` = `xefu7.xex` and `xefutitle7.xex`. Xenia mounts an XISO as
   `\Device\Cdrom0` and starts `default.xex`; xefu then opens
   `\Device\CdRom0\default.xbe`, which is the game.
2. Put the Compatibility folder's files in `<storage root>\compatibility`
   (`config.bin`, `xbox.xex`, `xefu.xex`, `dash\...`). Xenia mounts it as
   `\Device\Harddisk0\Partition1\Compatibility` and
   `...\SystemPartition\Compatibility` (cvar `mount_compatibility`).
3. Open the image; read `xenia.log` for the `BACKCOMPAT` lines.

## What xefu does, as observed

1. Loads `xefutitle7.xex` (by bare name, beside itself) and calls its
   ordinals 1 and 2: the per-title lookup. `xefutitle7` contains
   `4541005B`.
2. Opens the disc, network and sign-in checks (stubbed).
3. `KeCreateUserMode(r3=0, r4=C5000000, r5=C5000000, r6=C4FF0FA0,
   r7=C4FF0690, r8=3)`. Just before the call (around `0x82041C00`) xefu
   writes page table entries (protection bits ORed in) into a structure:
   it is describing the Xbox game's address space for the hardware MMU.
   The block at `r4` was all zero at the call.
4. Starts about ten worker threads, registers an audio client.
5. Creates `Compatibility\Xbox1\TDATA\FFFE0000\MUSIC` and rewrites
   `Compatibility\config.bin`: the emulated Xbox hard drive.
6. `XeKeysGetKey(0x15)` (not implemented), `KeSetTimerEx`.
7. `KeEnterUserMode(r3=context, r4=0x82045FD8, r5=0, r6=0)` at
   `0x82046078`, returning a status that xefu treats as the user-mode exit
   reason. With a stub returning 0, xefu takes the game as ended and
   launches `X:\xbox.xex` through `XamLoaderLaunchTitleEx` (its exit path,
   `0x8203FB18`).

## The user-mode starting context

Filled by `0x82042C8C` into a zeroed `0xA40`-byte block. Assuming the 360's
CONTEXT layout (flags, MSR, IAR, LR at `0x0..0xC`, CTR at `0x10`, GPRs as
qwords from `0x18`, CR at `0x118`):

| Offset | Field (assumed) | Value seen / source |
| --- | --- | --- |
| `0x08` | IAR | `B9FF7000` (state `+1328`) |
| `0x28` | r2 | `80030878` (state `+1320`) |
| `0x40` | r5 | `0x500` |
| `0x50` | r7 | `80400000` (state `+1316`) |
| `0x100` | r29 | `0xFF00` |
| `0x108` | r30 | state `+1324` OR `0x40201 << 32` |
| `0x110` | r31 | from state `+192`/`+136` |
| `0x118` | CR | option bits from per-title flags |

The handler at `0x82045FD8` tail-calls a larger routine with xefu's global
state and two string labels: the dispatcher for traps out of user mode.

## The user address space

The page table at `KeCreateUserMode`'s `r4` has 32-bit entries at index
`0x100 + n` for user segment `n` (16 MB at `n << 24`): the top byte is the
physical 16 MB frame, the low 12 bits attributes (`0x193`, `0x196`,
`0x197`). Seen for Burnout 3:

| User segment | Physical | Entry |
| --- | --- | --- |
| `B6000000` | `06000000` | `06000197` |
| `B7000000` | `07000000` | `07000196` |
| `B8000000`-`BB000000` | `08000000`-`0B000000` | `...193` |
| `BD000000`, `BE000000` | `0D000000`, `0E000000` | `...196` |

Segment `80` (the Xbox game's own addresses, see below) is not mapped at
entry. The kernel can read any of it through the `0xC0000000` physical view
(user `B9FF7000` = kernel `C9FF7000`).

## The translated-code runtime (user mode)

At the entry (`B9FF7000`) is xefu's dispatcher, real PowerPC:

* `r2` is the current Xbox (x86) instruction address; the Xbox address space
  sits at user `0x80000000 + x`, so the starting `r2 = 80030878` is the
  game's entry at Xbox `00030878`.
* `r30` (`B711F000`) is a lookup cache from Xbox addresses to translated
  blocks (pairs; empty entries `FFFFFFFD`). A hit branches to the block.
* `r31` (`B7000000`) is the runtime's state block.
* A miss loads a service number into `r0` (`0x104`, `0x204`, `0x304`, ...)
  and branches to a stub that does `sc`: a system call back to xefu, e.g.
  to translate the next block. Plain `li r0, N; sc` sequences also appear.

## The trap handler

`KeEnterUserMode`'s `r4` (`0x82045FD8`) forwards to `0x821073B8` with xefu's
state, `r4` = exception record, `r5` = context, `r6` = `0x82045E08` (general
handler), `r7` = `0x82045828`. It dispatches on `record->ExceptionCode`:

* `C0000005` (access violation) with the faulting address
  (`record + 24`) in `FD000000`-`FEFFFFFF`: the original Xbox's NV2A
  registers, emulated by `r7`'s routine. Games reach the GPU by faulting.
* `80000004` (single step): handled specially.
* Anything else: the general handler `0x82045E08(record, context, &out)`.

So `KeEnterUserMode` must: run the context in the user address space; on a
system call or fault, build an exception record and the user CONTEXT, call
the handler in kernel mode, and resume the user context as the handler left
it (presumably until the handler calls `KeLeaveUserMode`, which makes
`KeEnterUserMode` return).

## What Xenia has to provide (the hard part)

* A second address space: the user-mode page tables, separate from the
  kernel's, with addresses that overlap Xenia's own heaps.
* `KeEnterUserMode`: run the guest at the context's IAR in that address
  space until it traps, then return to xefu through its handler with the
  trap's details; `KeLeaveUserMode` and the TB flushes alongside.
* Traps: system calls and page faults from translated code (the Xbox's
  hardware is likely reached through faults) delivered to xefu.
* `KeSweepIcacheRange`: xefu writes new PowerPC code as the game runs;
  Xenia must drop its translations of that range.

## Status in this fork

* User address space: a 4 GB host range per process (`Memory::CreateAliasSpace`)
  with 16 MB views of physical memory, rebuilt from xefu's page table on
  `KeEnterUserMode` and the TB flushes. User code runs with it as
  `virtual_membase`; code in `B0000000`-`C0000000` is read from it when
  translated.
* System calls: `sc` in user code calls xefu's handler with
  `(record = 0, trap frame)` and resumes at the frame's IAR.
* Access violations (Windows x64): a host fault in the alias space from user
  code is mapped back to the guest instruction (the source map); user code is
  translated without context promotion, so the guest context is exact there.
  The thread is restarted from a host context captured in `KeEnterUserMode`,
  which hands xefu an `EXCEPTION_RECORD` (`C0000005`, address at `+0x0C`,
  read/write and data address at `+0x14`/`+0x18`) with the trap frame, then
  resumes at the frame's IAR.
* Not yet: illegal-instruction and trap records, misalignment,
  `KeSweepIcacheRange` invalidation, `KeSetTimerEx` with a DPC.
