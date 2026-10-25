# How XeFu Works

What we've learned about the 360's backward-compatibility emulator, and what Xenia has to provide for it. The detailed notes live in the repository at [`docs/xefu-backcompat.md`](https://github.com/TopGunMaster-source/xenia-canary/blob/xefu-backcompat/docs/xefu-backcompat.md).

## The big picture

* XeFu is a 360 program that emulates a whole original Xbox. It comes in several builds (`xefu.xex`, `xefu6.xex`, `xefu7.xex`, …, each with its `xefutitle` table); each approved game is assigned to one build, and `xbox.xex`, the 360's backward-compatibility launcher, starts the right one. Burnout 3's is `xefu6.xex`. What this wiki says about addresses (such as `82045848`) is from `xefu7.xex` and differs in other builds.
* It runs a **real x86 Xbox kernel** (the `xb1krnl` resource, loaded at x86 address 0x80010000) through an **x86-to-PowerPC translator**.
* The translated code runs in the 360's **user mode** with XeFu's own page tables. The 360 kernel calls behind this (`KeCreateUserMode`, `KeEnterUserMode`, TB flushes) are used by nothing else, so Xenia didn't have them.

## Things Xenia must do the way a 360 does

* **Page faults:** XeFu maps x86 memory on demand. A fault must reach XeFu's handler with an exception record and trap frame, then resume exactly where it happened.
* **Page sizes:** 16 MB, 64 KB and 4 KB pages, decoded from XeFu's page-table structure. Scattered 4 KB pages need 4 KB host views (Windows placeholders).
* **System calls:** `sc` from user mode goes to XeFu's handler. Port I/O is `sc 0xA`/`0xB`, the Xbox's fatal error is `sc 0x10`.
* **Hypercalls:** the x86 instruction `0F 3F aa bb` becomes a call into XeFu's runtime (`li r11, 0xbbaa; bl stub`). Examples: `0202` names the XBE to load, `0203` is the media type, `0201` checks a section digest, `060E` copies memory, `0702` returns to the dashboard.
* **Interrupts:** XeFu sets a pending byte and CR bit 4 in the user thread's trap frame. Translated code checks the bit and enters XeFu's dispatcher. The bit must only be live while an interrupt really is pending and enabled.
* **Timers and DPCs:** a 1 ms clock and DPCs that never nest or run at the same time.
* **Crypto:** the EEPROM uses HMAC-SHA1 and RC4 with an all-zero key; the XBE uses RSA-2048 and SHA-1. Xenia's implementations had to be exact.

## The emulated Xbox's boot, as observed

1. The Xbox kernel's phase 1 decrypts and checks the EEPROM.
2. It asks (hypercall `0202`) for `\Device\CdRom0\default.xbe`. XeFu's I/O thread opens `\??\d:\default.xbe` on the 360 side.
3. The loader reads the XBE header and verifies its RSA signature (XeFu does the RSA with the 360's `XeCrypt` functions). It checks the game region and the allowed media.
4. For each section: allocate pages, read the data, check its SHA-1. Reads come either from XeFu's I/O thread (direct reads into the Xbox's RAM) or from its read-ahead cache (copied by hypercall `060E`).
5. *(next)* Jump to the game's entry point.

If any step fails, the Xbox kernel returns to the dashboard (`sc 0x10` at x86 `80024627`). XeFu then exits to `X:\xbox.xex`, the 360's backward-compatibility launcher.

## XeFu builds

Public details, from [ConsoleMods](https://consolemods.org/wiki/Xbox_360:Xenon_Fusion_Emulator) and its [compatibility list](https://consolemods.org/wiki/Xbox_360:Original_Xbox_Games_Compatibility_List):

| File | Built | Title table |
|---|---|---|
| `xefu.xex` | 2005-09-14 | none |
| `xefu2.xex` | 2006-02-08 | none |
| `xefu3.xex` | 2006-03-31 | none |
| `xefu5.xex` | 2006-08-31 | `xefutitle5.xex` |
| `xefu1_1.xex` | 2006-10-19 | none |
| `xefu6.xex` | 2006-12-11 | `xefutitle6.xex` |
| `xefu7.xex`, `xefu7b.xex` | 2007-11-26 | `xefutitle7.xex`, `xefutitle7b.xex` |

* Officially supported games name the build to use; others use the latest. On the console they live in the hard drive's system partition (`xbox.xex`'s own header names `\Device\Harddisk0\SystemPartition\Compatibility\xbox.xex`); the main partition's `Compatibility\Xbox1` holds original Xbox saves and content.
* **Burnout 3: Takedown** is listed as supported on `xefu6` (and playable on the Xbox One build `xefu2021c`); untested on the others. Game rips missing data freeze on load.
* Our copy (checked October 25, 2026): `xbox.xex` and `xefu.xex` are the 2005 versions (`xefu.xex` matches ConsoleMods' SHA-1). `xefu7.xex` has the 2007 build date but a different SHA-1 from the one ConsoleMods lists, and `xefutitle7.xex` is dated 2018 and also differs. There is no `xefu6.xex`. See [[Issues and Fixes]] #40.

