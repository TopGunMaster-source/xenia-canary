# Original Xbox games in Xenia through XeFu

This fork (branch [`xefu-backcompat`](https://github.com/TopGunMaster-source/xenia-canary/tree/xefu-backcompat)) aims to run original Xbox games in Xenia the way an Xbox 360 does: by running the 360's own backward-compatibility emulator, **XeFu**, inside Xenia. The test game is **Burnout 3: Takedown** (the Xbox Originals digital release, title `4541005B`, run by XeFu's title `FFFE07D2`).

The guiding rule: XeFu runs flawlessly on a real 360, so every failure is treated as a gap in Xenia, not a bug in XeFu.

No Microsoft files are in this repository. XeFu, the Compatibility folder and the game come from the owner's own console.

* **[[Progress]]**: what works now, step by step, and what's next.
* **[[Issues and Fixes]]**: every problem hit so far: symptom, cause, fix and commit.
* **[[How XeFu Works]]**: the parts of XeFu that Xenia has to provide.
* **[[Testing Setup]]**: how a test is run and read.
* **[[Mistakes and Lessons]]**: mistakes made along the way, why, and the rules that came out of them.

## Status in one paragraph (October 21, 2026)

XeFu boots in Xenia and runs a real x86 Xbox kernel on its x86-to-PowerPC translator. The emulated Xbox gets through its boot. It passes its EEPROM check and keeps time with timers and interrupts. It opens Burnout 3's `default.xbe`, verifies the RSA signature, passes the disc authentication check and starts loading the game's 17 code and data sections. The first four sections load and pass their SHA-1 check. The fifth (`XONLINE`) was only half copied because of an interrupt-timing bug in Xenia. The fix is built but not yet tested. Next: the remaining sections, then the game's own code, then graphics (XeFu's NV2A emulation on Xenia's GPU).
