# How XeFu Works

What we've learned about the 360's backward-compatibility emulator, and what Xenia has to provide for it. The detailed notes live in the repository at [`docs/xefu-backcompat.md`](https://github.com/TopGunMaster-source/xenia-canary/blob/xefu-backcompat/docs/xefu-backcompat.md).

## The big picture

* XeFu is a 360 program (`xefu7.xex`, plus `xefutitle7.xex`, a per-title table) that emulates a whole original Xbox.
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
