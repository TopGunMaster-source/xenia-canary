# Issues and Fixes

Every problem hit so far, in the order it was found. Each entry gives what we saw, what Xenia was doing wrong, how it was fixed, and the commit(s) on [`xefu-backcompat`](https://github.com/TopGunMaster-source/xenia-canary/commits/xefu-backcompat).

Status key: ✅ fixed and confirmed in a test run · 🧪 fixed, waiting for a test run · 🔧 workaround, not a full fix

---

## Stage 1: getting XeFu itself to start

### 1. XeFu couldn't find its per-title table ✅
* **Symptom:** `xefu7.xex` loaded, but its imports from `xefutitle7.xex` stayed unresolved.
* **Cause:** XeFu imports the library by bare name. Xenia joined bare names to the *first* loaded module's path, which, while that first module was still loading, gave a path with no device.
* **Fix:** resolve bare-named libraries beside the importing module, as the 360's loader does. `5a51f1d`

### 2. No hard-drive Compatibility folder ✅
* **Symptom:** XeFu looks for `config.bin`, `xbox.xex` and the emulated Xbox's hard drive in `\Device\Harddisk0\Partition1\Compatibility`.
* **Fix:** mount `<storage root>\compatibility` there (cvar `mount_compatibility`); later a `compatibility_root` cvar to point at any folder. `8e08a4b`, `68869cc`

### 3. The user-mode kernel functions didn't exist ✅
* **Symptom:** XeFu calls `KeCreateUserMode`, `KeEnterUserMode`, `KeLeaveUserMode`, TB flushes and others that no 360 game uses.
* **Fix:** research stubs that log their arguments (`8e08a4b`, `dd33aca`, `f55703c`), then a real emulated **user mode** (`3e0782c`). XeFu's x86 game code runs in a separate address space built from XeFu's page tables (an "alias space" of extra views of guest memory). System calls go to XeFu's handler with a trap frame.

### 4. Crash on the first indirect call ✅
* **Cause:** `lea eax, [ebx - 0x80000000]` needs a −2³¹ displacement, which the x86 assembler library refuses.
* **Fix:** compute it with `mov`/`add`. `228f59a`

### 5. Writing past the indirection table ✅
* **Cause:** code at 0xB0000000+ (user mode) got indirection slots, but the table only covers 0x80000000–0x9FFFFFFF.
* **Fix:** don't write slots for code outside the table; resolve those calls the slow way. `82f86ff`, `3e0782c`

### 6. Page faults in user mode weren't delivered ✅
* **Symptom:** the first user-mode fault (the x86 stack, not mapped yet) stopped the emulator.
* **Fix:** deliver host access violations in the alias space to XeFu's handler as exception records. Run that handler ahead of Xenia's crash handler. Translate user code without context promotion, so registers are exact at any instruction. `a064d3c`, `ae93ade`

### 7. Overflow flags missing ✅
* **Cause:** XeFu translates x86 arithmetic with the PowerPC `o` forms (`addo`, `subfo`, `mullwo` …) to get the x86 overflow flag; Xenia didn't set XER[OV]/[SO].
* **Fix:** implement OV/SO for those instructions, including `mullwo`. `ae93ade`, `a96869e`

### 8. Endless loop after a system call ✅
* **Cause:** the trap frame's IAR pointed at the `sc` itself. On the 360, SRR0 is the *next* instruction.
* **Fix:** store the next instruction. `c5454a3`

### 9. Stale translations of rewritten code ✅
* **Cause:** XeFu writes new translated code and calls `KeSweepIcacheRange`; Xenia kept running its old translation.
* **Fix:** drop Xenia's translations of user code in the swept range. `3a09161`

### 10. Stackpoints overflowing from XeFu's block chaining ✅
* **Cause:** XeFu's translated blocks `bl` into its dispatcher and never return, so each block left a host frame behind.
* **Fix:** in user-mode code every branch-with-link is a jump. `6ab12db`

### 11. Memory pages XeFu maps weren't visible ✅
* **Cause:** XeFu maps the Xbox's RAM in 64 KB pages and its page tables in 4 KB pages through second-level tables; Xenia only mapped 16 MB segments.
* **Fix:** decode XeFu's page-table structure. Map 64 KB pages as views, and 4 KB pages as 64 KB views where they line up. Emulate the load or store on a fault where they don't. `f39b110`, `5f4b8bb`, `ac5d596`

---

## Stage 2: the emulated Xbox boots

### 12. The Xbox clock never ticked ✅
* **Symptom:** the Xbox kernel spun at x86 `80019470` waiting for time to pass.
* **Cause:** `KeSetTimerEx` was a stub, so XeFu's timer DPC (which drives the Xbox's timer interrupt) never ran.
* **Fix:** real kernel timers with DPCs on a host thread (`e7b4a58`). They expire on 1 ms clock ticks, not immediately (`3bf05a7`). Firing at once ran the Xbox clock about 1000× too fast.

### 13. Interrupts were never taken ✅
* **Symptom:** the Xbox kernel sat in `KfLowerIrql` with an IRQ pending.
* **Causes:**
  * XeFu reports its trap instructions as illegal-instruction records; Xenia ignored traps. `6e04013`
  * XeFu raises an interrupt by setting CR bit 4 in the user thread's trap frame (found through KTHREAD+0x128), and Xenia had no trap frame there. `a59d20f`
* **Fix:** route traps to XeFu's handler; point KTHREAD+0x128 at the user trap frame and carry the bit into the live registers.

### 14. EEPROM check failed (front LED 0xA0) ✅
* **Symptom:** the Xbox kernel's phase 1 rejected the EEPROM and stopped.
* **Cause:** two crypto bugs. `XeCryptRc4Key` always cycled 16 key bytes (XeFu uses a 20-byte key), and `XeCryptHmacShaInit` padded the key wrongly.
* **Fix:** both corrected. `5b4c9a5`

### 15. Xbox kernel data at 0xFE000000 kept faulting ✅
* **Cause:** Xenia's JIT adds 4 KB to host addresses at or above 0xE0000000 (a 360 physical-heap quirk) when the host granularity is 64 KB, which the alias views can't follow, so those pages stayed unmapped.
* **Fix:** translate user-mode code without the offset and map those pages normally. `09b9cc6`

### 16. "Overflowed stackpoints" after a few hundred DPCs ✅
* **Cause:** Xenia ran `KeInsertQueueDpc` inline. XeFu's interrupt DPC queues itself again, which recursed without bound.
* **Fix:** DPCs queued from a DPC run after it, never nested (`e3e7ce6`); a safety net restores the stackpoint depth (`dee45f6`).

### 17. The Xbox clock stopped again (deadlock) ✅
* **Cause:** two of XeFu's DPCs ran at once (its interrupt thread and the clock timer) and deadlocked in its interrupt-controller emulation. On a real CPU, DPCs never run concurrently.
* **Fix:** while XeFu runs, all its DPCs run one at a time on the timer thread. `d175f20`, `baa31dd` (the first push failed to compile; fixed by declaring the flag earlier)

---

## Stage 3: loading Burnout 3

### 18. XeFu "exits" instead of starting the game ✅ (understood)
* **Symptom:** XeFu called `XamLoaderLaunchTitleEx("X:\xbox.xex")`. At first this looked like a game launch; it is actually XeFu leaving to the 360's backward-compatibility launcher after the Xbox gave up.
* **Fix:** implement `XamLoaderLaunchTitleEx` (relaunch Xenia like a real 360 does, cvar `relaunch_on_title_launch`), and log the x86 state when the Xbox kernel returns to the dashboard, so the real reason can be read. `124751b`, `89baabc`

### 19. `\??\d:\default.xbe` rejected (0xC0000033) ✅
* **Cause:** `\??\` (the DOS devices directory) contains `?`, which Xenia's path check rejects.
* **Fix:** accept `\??\` paths in `NtCreateFile` and friends; link `X:` to the Compatibility folder. `4e7b982`

### 20. Crash writing through a NULL pointer ✅
* **Symptom:** crash at `820C1D70` while creating `Y:\Xbox1\TDATA\FFFE0000\MUSIC\st.db`.
* **Cause:** XeFu's `WriteFile` wrapper stores the byte count even when the caller passed NULL. The 360 allows writes to page zero; Xenia protects it.
* **Fix:** unprotect page zero while XeFu runs. `499c90c`

### 21. The game file landed nowhere (0xC000007B, invalid image) ✅
* **Cause:** XeFu copies file data into the Xbox's scattered RAM pages through `MmLockAndMapSegmentArray`, which was a stub returning 0.
* **Fix:** implement it with a bounce buffer that is copied back on unmap. `499c90c`

### 22. Signature check failed (0xC0000190, trust failure) ✅
* **Cause:** Xenia's big-number remainder (used by `XeCryptBnQwNeRsaPubCrypt`) overflowed its quotient estimate, so RSA gave wrong answers for some inputs. Verified offline: Xenia's result was wrong, the correct math matched Burnout 3's real signature.
* **Fix:** rewrite the remainder (Knuth's algorithm D on 32-bit digits), checked against 3000 random cases. `05679f7`

### 23. "Media not allowed" (0xC0050002) ✅
* **Cause:** XeFu sends drive `D:` IOCTL `0x24080`, the CD-ROM driver's disc authentication request (we first called it a "media check"; Xenia Edge's author pointed out it is disc authentication). Xenia didn't answer, so XeFu fell back to `0x335142`, got media type 0, which the game's certificate doesn't allow. Patched XeFu builds skip this check; Microsoft's stock builds (ours) don't.
* **Fix:** answer the IOCTL with success. `b104a8c`

### 24. A memory-clearing loop that never ended ✅
* **Cause:** the game's memset uses `dcbz` on a 4 KB page Xenia couldn't map. The fault emulation only knew integer loads and stores, so the fault went back to XeFu, which saw the page mapped and retried forever.
* **Fix:** emulate `dcbz`, floating-point and vector loads and stores too. `592dd0b`

### 25. Most game memory went the slow way (performance) ✅
* **Cause:** Windows documents file views as 64 KB-granular, so scattered 4 KB pages couldn't be mapped, and every access faulted and was emulated.
* **Fixes:**
  * Use Windows *placeholders*, which on current Windows accept 4 KB views at 4 KB offsets. Xenia checks this at startup and falls back if not. Confirmed: `Alias address space is page granular`. `27f372d`
  * When XeFu has already mapped a page (it doesn't flush the TB for new mappings), map it and retry instead of emulating. `01563c6`

### 26. A game section only half loaded (0xC0000190 again) ✅
* **Symptom:** sections `.text`, `XMV`, `DSOUND`, `WMADEC` passed their SHA-1 check; `XONLINE` failed. Comparing RAM with the XBE showed exactly the first 64 KB copied and the rest zero.
* **Cause:** XeFu's copy routine (hypercall `060E`) stops early when CR bit 4 says an interrupt is pending, expecting to resume after the interrupt. If no interrupt is actually deliverable, XeFu's dispatcher continues past the copy. Xenia set that bit from another thread, so the bit could outlive the interrupt and the copy was silently cut short.
* **Fix:** set the bit only while an interrupt is pending *and* enabled, and take it back if that changed meanwhile. `40af7de`. Confirmed: `XONLINE` now passes.

### 31. The end of section `XNET` came from the wrong place (0xC0000190) ✅
* **Symptom:** `XNET` failed its SHA-1 check. Comparing RAM with the XBE showed everything right except the last 0x10E8 bytes, which held data from a different part of the file (from the next read-ahead buffer).
* **Cause:** XeFu's I/O thread reads ahead asynchronously. Its completion routine (an APC) reads the byte count from the read's `IO_STATUS_BLOCK`, which lives on a stack frame that has already returned. On the 360 that works, because the kernel writes the status block during completion, in the same thread, right before the APC runs. Xenia wrote it when the read was issued, so by the time the APC ran, the stack had been reused and XeFu recorded a garbage length for that buffer.
* **Fix:** for `NtReadFile`, `NtReadFileScatter` and `NtWriteFile` with a completion APC, write the status block again just before the APC is delivered (`ee059c7`).
* **First test (run 21) still failed.** A dump of XeFu's own memory (new, `xefu_host_ram_exit.bin`) showed the cache entry's length was `CE0008A0`, the descriptor's own address, so the APC still read a stale slot. The late write never happened: Xenia's APC delivery looked the APC up by the address of its *list entry* (8 bytes into the APC) instead of the APC itself. The same mistake meant kernel-routine APCs got the wrong pointer and finished APCs were freed at the wrong address. Fixed in `c3cea8c`. ✅ Confirmed in run 23: every section verifies, and the Xbox kernel starts the game.

### 32. The game stuck on its first function call: endless faults on its stack ✅
* **Symptom:** run 23 got into Burnout 3's own code (x86 `001D5E82`, `0025C110`, …), then stopped at x86 `002647B0`, the `push ebp` of a function, with about 400 000 page faults a second on the game's stack (`D0029CD0`). XeFu's page table maps that page read/write; Xenia's view of it was missing.
* **Cause:** two slips in how Xenia mirrors XeFu's 4 KB pages. If *any* of the 16 pages of a 64 KB block was unmapped (a stack's guard page, for example), Xenia gave up on the whole block. And when mapping the pages one by one, it stopped at the first page that was already right instead of going on to the rest. Each fault then went to XeFu, which saw the page mapped and retried, forever.
* **Fix:** skip unmapped pages and keep going, in both places (`66f3bb2`). Confirmed: the fault storm is gone and the game's start-up code runs on for about 15 seconds.

### 33. Xenia crashed jumping to `BCBCBCBC` ✅
* **Symptom:** with #32 fixed, the game's start-up ran on, then Xenia itself crashed (an access violation inside its PowerPC translator, found with the crash dump and the build's symbols): something branched to address `BCBCBCBC`, and Xenia tried to translate code there.
* **Cause:** `0xBCBCBCBC` is Xenia's own made-up return address for code it calls from the host (`Processor::Execute`). It happens to fall in the range Xenia reserves for XeFu's user-mode code (0xB0000000-0xBFFFFFFF), and code there is translated with every call turned into a jump (#10). So when XeFu's handler, called from the host, jumped into code in that range and that code returned, the return went to `BCBCBCBC` as if it were code, instead of back to Xenia.
* **Fix:** a return to Xenia's sentinel address now always goes back to the host (`99e080a` covered only user-range code; the log of the next run showed XeFu's kernel-mode handler making the return, so the check now covers all returns: `c168783`). Confirmed: no more crash, and the game reaches its own code within seconds of starting. Along the way: Xenia now raises an instruction fault, as a 360 does, when user code branches to a page that isn't mapped (`5ab2107`, `6fa126e`, `f207622`).

### 34. A breakpoint in XeFu stopped Xenia 🔧
* **Symptom:** with #33 fixed, XeFu's exception handler seemed to hang. It had actually hit a trap instruction meant as a debugger breakpoint, and Xenia (set to break on those by default) stopped the thread with an "Unhandled Exception: A breakpoint has been reached" dialog.
* **What a 360 does:** a retail console has no debugger attached, so breakpoints are skipped and the code carries on.
* **Fix:** test runs set `break_on_debugbreak = false`, which makes Xenia do the same. Confirmed: start-up continues well past that point. Xenia now also logs where such a trap is hit and shows guest debug messages at info level (`27b5b01`).

### 35. Start-up stops with XeFu retrying a fault ✅
* **Symptom:** after a long pass through the game's code section (the x86 EIP walks steadily from `0025xxxx` to the end of `.text` at `002CC200`, about 20,000 system calls), system calls stop and the same kind of page fault reaches XeFu about 33 times a second. The log shows about 2,000 "forced trap" breakpoints on the main thread.
* **Not `abort()` after all:** an earlier guess blamed XeFu's C runtime `abort()`. Logging the exact address of each breakpoint (`2cbc103`) showed every one is at `82045864`, a zero word right after a call in XeFu's *one-shot* user-mode exception handler (`82045848`). That isn't a real breakpoint; it's Xenia running into data.
* **Cause:** at some points XeFu swaps its user-mode exception handler for a one-shot one. That handler calls a helper that saves the registers into the trap frame, runs the real handling, and then returns through the word its *caller* left just below the stack pointer, skipping the handler's own frame. It never expects to come back to the handler. Xenia called the handler with nothing meaningful in that word. Its return also only ever went back one frame, so it landed in the handler, on the zero word after the call.
* **Fix (`e0124228f`):** Xenia now puts its host return address (`0xBCBCBCBC`) in that word before calling XeFu's handler, as the console's dispatcher leaves its own return address there. A branch to that address from any depth now returns to Xenia from the frame Xenia entered, using the stack records Xenia already keeps for each guest function. This replaces the one-frame return from #33. The same mechanism exists in Xenia Edge (credited on [[Xenia Edge]]); ours is written for this tree.
* **Test of `e698ff2`:** the breakpoints were gone and start-up got further (about 6,900 system calls, up from 6,600), then XeFu's main thread went quiet. Xenia lowers r1 by 176 bytes when it calls guest code, so the return address went in the wrong word.
* **Test of `3de766a`** (return address in the right word): the return was taken, but the search for the frame to return from picked the main thread's very first frame, so the whole thread ended. The handler's own frame record doesn't show the host return address by the time XeFu returns.
* **Fix (`55f747754`):** the handler call now records where it was made (like the restart point for user-mode faults). A return to the host address while a handler is running goes straight back there and restores the stack records, whatever happened to them in between. Confirmed in the test of `1e02228`: no breakpoints, and the main thread keeps running. Start-up then stops at #37.

### 37. XeFu's handler got the kernel's non-volatile registers instead of the x86 ones ✅
* **Symptom:** with #35 fixed (`1e02228`: the one-shot handler now returns properly, and the main thread survives), start-up ran until the game's Direct3D code first wrote to the Xbox GPU's interrupt registers (`0xFD600140`, `0xFD009140`). Right after that the x86 code jumped to address 0, and its stack pointer held `C4FF0690`, a value from the 360 side, not the Xbox.
* **Cause:** on the 360, the kernel saves only the volatile registers in the trap frame and calls the handler with r14-r31 (and f14-f31) still holding the user's values. XeFu keeps x86 registers in those. Its handlers that emulate an instruction (such as a GPU register write) copy r14-r31 into the frame, edit them there, and load them back. Xenia called the handler with *all* registers set to the kernel's, so XeFu copied the kernel's r14-r31 into the frame and handed them back to the x86 code.
* **Fix (`50c5944e0`):** XeFu's handler now runs with the user's r14-r31 and f14-f31 live, and user mode resumes with them as the handler left them. Confirmed in the test of `c695c83`: the game's own code ran on (see #38).

### 38. Xenia crashed when XeFu's code branched to address 0 ✅
* **Symptom:** with #37 fixed (`c695c83`), start-up went much further: the game's own x86 code ran (EIP `001B9090`, with a proper Xbox stack), about 7,500 system calls and 14,000 faults. Then Xenia stopped with an "unhandled exception" (an access violation in its own code, found with the crash dump and the build's symbols).
* **Cause:** XeFu's translated code branched to address `0`. On a 360, user code branching to an unmapped address takes an instruction fault, and XeFu handles it, probably by translating the missing block. Xenia only raised that fault for branches into its user-code range (`0xB0000000`-`0xBFFFFFFF`). For address 0 it tried to translate PowerPC code there, failed, and dereferenced the missing result.
* **Fix (`b4b733f8b`):** user-mode branches below `0x80000000` with no translated code now go through the same instruction-fault path. Confirmed in the test of `c071aff`: XeFu handled those faults and carried on (see #39).

### 39. `mtfsb0`/`mtfsb1` weren't implemented 🧪
* **Symptom:** with #38 fixed (`c071aff`), start-up went much further again: about 15,000 system calls and 21,000 faults, with the game's code reaching its USB (controller) driver. Then a breakpoint in Xenia's generated code stopped the main thread.
* **Cause:** XeFu's translated code uses `mtfsb0` (clear one FPSCR bit, at `BA03EAAC`), which Xenia never implemented, so it emitted a breakpoint there. `mtfsb1` was missing too.
* **Fix (`ff4fae92f`):** both instructions now set or clear the FPSCR bit, update the host rounding mode when it's a rounding bit, and copy FPSCR to CR1 for the record forms. Built, not yet tested. (A scan of XeFu's generated code found no other unimplemented instructions so far.)

### 36. Fixes brought over from Xenia Edge ✅
* Two FPU instruction bugs (`mtfsfi`, `mcrfs`) that broke XeFu's rounding mode, file I/O offsets, stack arguments, and the page table changed bit. Details and credit on [[Xenia Edge]]. In every test build since `960bcb1`; their effect wasn't tested on its own.

---

## Problems with the test setup itself

### 40. Every test so far used the wrong XeFu build ❗
* **What happened:** until October 25, 2026 all our tests started `xefu7.xex`. Burnout 3 is assigned to `xefu6.xex` ([ConsoleMods' compatibility list](https://consolemods.org/wiki/Xbox_360:Original_Xbox_Games_Compatibility_List); Xenia Edge's author also runs it in game with that build).
* **What it did and didn't affect:** in our fork, `xefu6` got much further (the game's code keeps running) than `xefu7` (stalled at about 15,000 system calls). In Edge, the build made no difference with our files: with `xefu7` and with `xefu6` alike, XeFu gave up right after opening `D:`. With `xefu6` and detailed logging, Edge's log shows that step is the disc authentication check (IOCTL `0x24080`), which Edge doesn't answer and our fork does (#23); the `xefu7` run wasn't logged in detail, but stopped at the same step. So the Edge failure reported in Edge's issue #319 was real for our setup, but its cause was the unanswered disc authentication check, not the XeFu build; the logs that show it are on the [[Xenia Edge]] page. An early correction on this page had blamed the build for it; the side-by-side test with detailed logs showed otherwise.
* **What it means for the fixes:** #31-#39 fixed things Xenia itself got wrong (crashes, wrong registers, missing instructions); those are real whatever build runs. But the later stalls seen with `xefu7` may come from running a build that doesn't support the game, so how far `xefu7` got says little about Burnout 3.
* **Why it was missed:** the build-per-game assignment was known (the [[Xenia Edge]] page already said `xbox.xex` picks the whitelisted build) but never checked against the game before testing. The setup now has a checklist ([[Testing Setup]]).
* **Found later:** `xefu6.xex` was already on the PC, in the backward-compatibility update copied from the console (`Back Compact\From BC update`); the test folder had been built from only part of that folder, and that folder wasn't searched when looking for it.
* **First run on `xefu6` (October 25, 2026):** test disc `Burnout 3 - XeFu6 test.iso` (made by `Make XeFu6 Test Disc.exe`: the old disc with `default.xex` = `xefu6.xex` and `xefutitle6.xex`, SHA-1 `7154a87e…` and `827e41c0…`, read back and checked), Xenia `f0a2442`. XeFu ran without stalling or crashing for the whole run: about 144,000 system calls and 187,000 faults in 100 seconds and still climbing, with the game's code mostly at x86 `0030AD65` (its WMA audio decoder section) and the USB driver writing the USB controller (`FED00010`). The screen stayed black: nothing reaches Xenia's display yet. Next: why no picture (XeFu's graphics path into Xenia).
* **Edge with the disc authentication check answered (October 25, 2026):** Edge `3558152` plus only the `0x24080` answer (`05c1080`), same files with `xefu6`: Burnout 3 boots and is playable, in slow motion throughout (also at `log_level = 2`). Details on [[Xenia Edge]].
* **Next:** booting through an up-to-date `xbox.xex` (the update's `xbox.xexp` patch applied), as a console does; and the black screen.

### 41. A long wait after the game is loaded (xefu6), first taken for a hang 🧪
* **Symptom:** with `728e6d03f` and the xefu6 test disc, three runs in a row (October 25, 2026) stop at the same point: XeFu loads the game's sections (memory faults writing `0x455000`-`0x48B000`), hashes them, calls `KeFlushCurrentEntireTb`, and then its handler on the main thread never returns. That thread keeps entering and leaving one critical section (about 20,000 times a second); no more Xbox system calls or faults (stuck at about 5,300 and 939). Timers, interrupts and other threads keep going. One frame (640x480) is swapped before this, then none; the screen is black.
* **Not the cause:** the saved instruction-info cache (`cache_host/modules/.../executable_addr_flags.bin`): reset to empty, the run stopped at the same point.
* **Open question:** an earlier run with `f0a2442` on the same disc got past this point into the game's code; what was different isn't known (its full log wasn't kept). Xenia Edge passes this point with the same `xefu6`, started from `default.xbe`.
* **Same disc in Edge (October 25, 2026):** Edge `05c1080` opened `Burnout 3 - XeFu6 test.iso` (`default.xex` = `xefu6` from the disc) and went on into the game (sound, data and the menu video opened). So with identical inputs the stall is in our fork.
* **Compared with Edge's detailed log:** XeFu's display thread makes the same calls in both up to the first frame. (That it then waits in Edge too was read from a log that leaves such calls out; see [[Mistakes and Lessons]] #19.) Both make the same two full TB flushes. After the second, Edge's main XeFu thread carries on (reopens `default.xbe`, many user TB flushes, then the game's own startup calls); ours enters the handler that never returns. Ours also logs "Invalid instruction" for words in XeFu's generated code (546 times; Edge never does): Edge's scanner stops at the end of the code XeFu committed, ours reads on. Not yet known whether that matters here.
* **Which handler call never returns (`14e38acb3`, October 25, 2026):** after the second full TB flush, 42 handler calls return (system calls `sc 0x15` and `0x16`, no page faults); the 43rd, `sc 0x10` with `r11 = 0x507` (hypercall `0507`, `r3 = 0x41B000`, `r4 = 0x920`, x86 EIP `80014F1E`), goes straight into the lock loop and never returns. In Edge's log, right after the same point the 360 GPU starts working again (the command processor makes memory coherent and loads textures); in ours the GPU does nothing after the first frame. So hypercall `0507` may be waiting on GPU work (a guess).
* **Not the GPU (`85b02e270`):** XeFu writes the ring buffer's write pointer 4 times in the whole run (last `0x1F`) and our command processor reads up to `0x1F`: everything submitted was consumed, and nothing more is submitted while `0507` waits.
* **Edge stalls the same way (full logging, October 25, 2026):** with `log_high_frequency_kernel_calls = true`, Edge's first boot of the test disc stopped at the same point: the main XeFu thread loops on the lock at `C4FF1E04` after reading `0x920` bytes of `default.xbe` (the size `0507` is given), and XeFu's worker thread (entry `8204D9D8`) is never woken. Its helper thread (entry `82042A58`) took the same locks almost the same number of times as ours (8,606 vs 8,622 and 1,199 vs 1,190). The game was then opened a second time; on that boot the helper set the worker's event and XeFu went on. So the stall is timing-dependent and happens in Edge too; Edge usually gets past it.
* **It ends by itself (`85b02e270`, about 2½-minute run, October 25, 2026):** left running, the handler for `0507` returned after about 50 seconds: it then opened `default.xbe`, read the `0x920` bytes itself, and returned. For about 35 seconds more the x86 position climbed steadily through `0x278000`-`0x2CA000`; then the game's code ran (faults climbing again), XeFu swapped 104 frames and wrote the ring buffer 836 times, and then swapped no more while the game's code kept running (x86 `0030AD65`). Earlier runs had been closed after one to two minutes, before the wait ended. What the wait is (it looks like XeFu waiting for its worker thread and then reading the file itself) and why Edge's waits are shorter isn't known yet.
* **Next:** what was on screen during those 104 frames, and why frames stop after them.

### 27. Old Xenia processes locked the exe 🔧
* Frozen test runs kept `xenia_canary.exe` locked. Workaround: every build runs from its own folder (`run2`, `run3`, …).

### 28. Auto-relaunch overwrote the log ✅
* The relaunched process wrote over the first process's log. Diagnosis runs set `relaunch_on_title_launch = false`.

### 29. Test runs filled the disk (~90 GB) ✅
* **Symptom:** downloads of new builds came out as 0-byte files.
* **Cause:** test processes that had finished (XeFu exited) stayed open, kept logging at debug level for hours, and reached up to 31 GB per run.
* **Fix:** the logs were cleared; the log file now stops at 2 GB (`log_file_max_mb`), and with relaunch off Xenia exits when the test ends. `6bf7a85`

### 30. "Silent crash" while checking the game's code (not a crash) ✅
* **Symptom:** with build `6bf7a85`, Xenia closed by itself about 4 seconds in, with no error message. The log stopped in the middle of the `.text` section check.
* **What it really was:** the new exit-when-done behaviour (#29). XeFu gave up and asked to launch `X:\xbox.xex`, so Xenia exited, but it exited before the debug log had caught up, so the log's last seconds (including why the Xbox gave up) were lost. Found with a new exit trace (`xenia_exit.txt`), which showed `quick_exit` called from `XamLoaderLaunchTitleEx`. Xenia also now writes a crash dump (`xenia_crash.dmp`) on a real crash (`a46c86f`, `a80e387`).
* **Fix:** before exiting, wait for the log to catch up and save the Xbox's RAM (`xefu_xbox_ram_exit.bin`) for offline checks.
