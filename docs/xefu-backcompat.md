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
