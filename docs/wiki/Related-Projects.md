# Related Projects

Other work on the 360's backward compatibility (XeFu), and what it means for this project. Collected October 24, 2026.

## Microsoft's XeO3 ("Fission") on PC
* In July 2026 Microsoft re-released four original Xbox games on PC (Fuzion Frenzy, BLiNX, …). Dataminers found they run **XeFu inside XeO3**, Microsoft's own Xbox 360 emulator (the one in Xbox One and Series back compat), recompiled for x86 PCs.
* XeO3 emulates the 360's hardware and hypervisor, but **runs the real 360 kernel and xam**, translated ahead of time to x86-64 DLLs. XeFu (`xefu.xex`) runs as an ordinary 360 title, and the original Xbox kernel and each game's x86 code also come precompiled (`xefu_…` DLLs).
* **What it means here:** XeFu needs nothing beyond a real 360 kernel. Every problem we hit is a gap in Xenia's kernel emulation (user mode, exceptions, I/O completion, …), not in XeFu. The real kernel's code for `KeEnterUserMode`, user-mode exception delivery and trap handling is the reference to compare against.
* Sources: [Tom's Hardware](https://www.tomshardware.com/video-games/xbox/microsoft-shipped-a-native-xbox-360-emulator-with-its-new-backwards-compatible-releases-on-pc-modders-quickly-got-360-games-running-with-minor-tweaks), [PC Gamer](https://pcgamer.com/gaming-industry/game-development/the-new-backward-compatible-xbox-games-on-pc-run-on-an-og-xbox-emulator-inside-an-xbox-360-emulator-and-people-have-already-used-it-to-run-other-xbox-360-games), [Wccftech](https://wccftech.com/microsofts-pc-xbox-backward-compatibility-hides-360-emulator/).

## xeo3-recomp-kit
* [fxceless/xeo3-recomp-kit](https://github.com/fxceless/xeo3-recomp-kit) (MIT) runs 360 games on XeO3 by recompiling their code into the DLL format XeO3 loads (building on [WitherOrNot's XenonRecomp-XEO3](https://github.com/WitherOrNot/XenonRecomp-XEO3) and hedge-dev's XenonRecomp). It needs Microsoft back-compat packages (Fuzion Frenzy for the emulator, an Xbox One back-compat package for the 360 system files) plus XWine1/SlimEra's `kernelx.dll`.
* Original Xbox games: only the ones Microsoft shipped (with their precompiled `xefu_…` DLLs) are known to run. A game Microsoft never shipped this way, like Burnout 3, would need an x86 translator that doesn't exist yet. So it isn't a shortcut for Burnout 3, but it shows XeFu running on the real kernel.

## Xbox Dev Wiki: Fusion
* [xboxdevwiki.net/Fusion](https://xboxdevwiki.net/Fusion): early notes on `xefu.xex`, its modified Xbox kernel (`xb1krnl`) and the `0F 3F` guest-to-host calls. Matches what we found (see [[How XeFu Works]]).

## xemu, Cxbx-Reloaded
* Original Xbox emulators. They run Burnout 3 directly, without XeFu; useful for comparing the game's own behaviour (what it does at start-up), not XeFu's.

## Known issues outside Xenia (identified, not patched)
Problems that come from Burnout 3 or XeFu themselves, so they aren't mistaken for Xenia bugs later.

**XeFu (on a real 360).** Microsoft's notes for Xbox Originals list, for all titles: company logos at start-up can stutter, and extra content on the disc (trailers, demos) can misbehave and need a restart. No issue was listed for Burnout 3 itself. ([VidaExtra](https://www.vidaextra.com/fps/xbox-originals-con-algunos-problemas))

**Burnout 3 itself (seen in xemu, an original Xbox emulator, so these are about the game's own demands):** stutter when driving into or out of overhead structures (tied to audio DSP), music much quieter than effects, black bars at the top of the screen, texture misalignment / lines on the road, laggy embedded videos and menus, and a freeze when launched from the original Xbox dashboard. Some of these are xemu's GPU/audio gaps, but they mark the parts of the game that stress an emulator: the audio DSP, signed texture filtering (`NV_PGRAPH_TEXFILTER_SIGNED`) and video playback. ([xemu compatibility page](https://xemu.app/titles/4541005b/))

**What it means here:** once graphics and audio run, check those spots first (overhead structures, music volume, road textures, the intro videos), and compare against a real 360 rather than assuming a Xenia bug.
