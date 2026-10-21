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

## First run of the game (observed)

* The dispatcher at `B9FF7000` misses its cache for the game's entry (Xbox
  `00030878`) and asks xefu (through a stub at `B9FFB9A8`, `r0 = 4`) for a
  translation; a worker thread translates the block, writes it at
  `B8000050` (kernel view `C8000050`) and calls `KeSweepIcacheRange`.
* The translated block keeps the x86 registers in PowerPC ones (`r7` = ESP,
  `r2` = EIP, ...), stores little-endian with `stwbrx`, calls helpers in the
  `B9FF....` runtime for memory and I/O, and uses `addco.` and friends for
  the x86 flags on plain 32-bit values: user mode looks like it runs with
  32-bit flag semantics (cvar `user_mode_flags_64bit` switches to 64-bit).
* Its first instruction pushes to the Xbox stack (`803FFFFC`), which isn't
  mapped. The fault reaches xefu's handler, which maps the Xbox's 64 MB of
  RAM (user `80000000`-`83FFFFFF`) in 64 KB pages: it fills attribute words
  in per-page arrays, queues pairs of addresses (user `80000000 + n * 64K`,
  `F0000000 + n * 64K`) in a list at page table `+0x884` (count at
  `+0x880`), and calls `KeFlushUserModeCurrentTb`. Xenia's 16 MB segment view
  doesn't see these 64 KB mappings yet, so the fault repeats.

## The page table structure (decoded)

Diffing the structure at `KeCreateUserMode`'s `r4` (`T`) around TB flushes:

* `T + 0x400 + 4 * n`: 16 MB pages for user segment `n` (above).
* `T + 0x800 + 2 * r`: for each 128 MB region `r` (`r << 27`), the physical
  address `>> 13` of a table of 2048 words mapping it in 64 KB pages
  (`physical page | attributes`). Region `0x10` (user `80000000`, the Xbox's
  RAM) points at `T + 0x2000`, whose entries map physical `10000000` up in
  order (`10000196`, `10010196`, ...); regions `0x11` and `0x1F` have tables
  at `T + 0xA000` and `T + 0x6000`.
* `T + 0x840`: one byte per region (`04`, `06`, `03`); meaning unknown.
* `T + 0x880`: count of words in the list at `T + 0x884` that xefu fills
  before `KeFlushUserModeCurrentTb`: the pages to flush, in pairs (user
  `803F0000` and `F03F0000` for the first fault).
* Attribute low bits are the PowerPC PP field: 2 read/write, 1 or 3 read
  only, 0 no access.

The Xbox RAM's 64 KB pages are mapped before user mode is entered; xefu's
handler finds them valid on the first fault and only flushes the TB.

## The emulated Xbox boot (observed)

xefu boots a real x86 Xbox kernel (the `xb1krnl` resource of xefu7.xex,
loaded at 0x80010000) on its x86-to-PowerPC translator.

* **Interrupts.** xefu's PIC emulation sets the pending byte at state+0x254
  and, when IF is set, ORs 0x08000000 (CR bit 4) into the CR slot of the
  user thread's trap frame (`[KTHREAD+0x128]+0x1B0`). The dispatcher entry
  tests that bit (`bt 4`) and takes the interrupt. Xenia has to set
  `KTHREAD+0x128` to the user trap frame and copy that bit into the live CR.
* **Timers.** xefu's clock DPC re-arms itself for 1 ms; timers must fire on
  1 ms ticks, not immediately.
* **Port I/O.** x86 `in`/`out` look up a per-port handler table
  (`[state-0x3FFC]`, index table at `[state-0x3FF8]`); ports with no PowerPC
  handler go to `sc 0xA` (in) / `sc 0xB` (out), port in r11, size in r12,
  value in r3. Handled devices: PCI config (CF8/CFC), ACPI (8000-80FF),
  SMBus (C000-C00F: SMC at 0x20, EEPROM at 0xA8), PIT (40-4F), RTC (70-72).
* **Hypercalls.** `0F 3F aa bb` becomes `li r11, 0xbbaa; bl <group stub>`.
  Group 06 (`B9FF9D7C`) handles 0630/0632 as `sc 0x15`/`sc 0x16`, 0660-0669
  record the title's sections in the state block. Group 04 and other codes
  are queued (64-byte records, ring at `[state+0x2A0]`) or handed to xefu's
  worker thread synchronously (state+0x260.., done flag at state+0x29C).
* **EEPROM.** xefu builds an EEPROM image and encrypts it the Xbox way:
  `HMAC-SHA1(key, data)` for the hash, `RC4(HMAC-SHA1(key, hash))` over the
  0x1C data bytes; the key is all zeros, as is the kernel's
  `XboxEEPROMKey`. Phase 1 decrypts and checks it; on a mismatch it sets the
  front LED to 0xA0 and terminates its thread, which leaves the kernel in
  the idle loop. Xenia's `XeCryptRc4Key` used to ignore the key length (it
  always cycled 16 bytes), which broke this.
* **DPCs.** xefu's DPCs re-queue themselves and must not nest or run
  concurrently (its interrupt DPC and clock DPC deadlock otherwise). While
  xefu runs, `KeInsertQueueDpc` queues them for the kernel timer thread,
  which runs them one at a time.
* **Loading the game.** The Xbox kernel asks for
  `\Device\CdRom0\default.xbe` (hypercall 0202); xefu's I/O thread opens
  `\??\d:\default.xbe`. Xenia used to reject `\??\` paths (C0000033), which
  made the Xbox kernel's loader fail and return to the dashboard (`sc 0x10`
  at EIP 80024627, hypercall 0702), and xefu then exits to `X:\xbox.xex`.
* **Copying file data into Xbox RAM.** xefu reads into its own buffer, then
  maps the Xbox RAM pages (a segment array of page addresses) with
  `MmLockAndMapSegmentArray`, copies, and unmaps with
  `MmUnlockAndUnmapSegmentArray`. Xenia emulates the mapping with a bounce
  buffer copied back on unmap.
* **Page zero.** xefu's WriteFile wrapper (820C1D70) stores the byte count
  even when the caller passes NULL (creating
  `Y:\Xbox1\TDATA\FFFE0000\MUSIC\st.db`), so page zero must be writable;
  `KeCreateUserMode` unprotects it.
* **Exit.** xefu leaves through `XamLoaderSetLaunchData` +
  `XamLoaderLaunchTitleEx("X:\xbox.xex")` (the backward-compatibility
  launcher). With `relaunch_on_title_launch` Xenia restarts itself with the
  same disc, which is what the 360 does; turn it off when reading logs.
