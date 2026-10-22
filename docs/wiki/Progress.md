# Progress

Milestones in order. The numbers in brackets refer to [[Issues and Fixes]].

| # | Milestone | Status |
| --- | --- | --- |
| 1 | XeFu (`xefu7.xex`) loads with its per-title table | ✅ [1, 2] |
| 2 | XeFu enters user mode and runs translated x86 code | ✅ [3–11] |
| 3 | The emulated Xbox's clock and interrupts work | ✅ [12, 13, 16, 17] |
| 4 | The Xbox kernel passes its EEPROM check | ✅ [14, 15] |
| 5 | The Xbox kernel asks for `\Device\CdRom0\default.xbe` and XeFu opens it | ✅ [19, 20] |
| 6 | The XBE header reaches the Xbox's memory | ✅ [21] |
| 7 | The XBE's RSA signature verifies | ✅ [22] |
| 8 | The media check passes | ✅ [23] |
| 9 | Sections load: `.text`, `XMV`, `DSOUND`, `WMADEC` verified | ✅ [24, 25] |
| 10 | Section `XONLINE` verified | ✅ [26] |
| 10b | Section `XNET` and the remaining sections | 🧪 fix built [31] |
| 11 | The Xbox kernel jumps to Burnout 3's entry point | ⏳ |
| 12 | Graphics: XeFu's NV2A emulation on Xenia's GPU | ⏳ |
| 13 | Audio, controller input, saves | ⏳ |
| 14 | Speed: playable frame rate (also for Steam Deck) | ⏳ |

## What's next

1. Test the status-block fix (#31) and check that `XNET` and the remaining sections pass.
2. Follow the Xbox kernel into the game's entry point; fix whatever Xenia gaps show up there.
3. Graphics is the next big unknown: XeFu turns the Xbox's NV2A commands into Xenos GPU work, which Xenia has to render.

## Rough timeline (estimate, October 21, 2026)

* The game's own code running: days.
* A first picture on screen: days to a couple of weeks, depending on graphics.
* Playable: weeks. Speed and graphics are the biggest unknowns.

## Builds

Each push to `xefu-backcompat` builds Windows and Linux binaries in GitHub Actions; the Windows exe is the `xenia_canary_windows_exe` artifact.
