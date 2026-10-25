# Mistakes and Lessons

Mistakes made while working on this project (most by Claude, the AI assistant doing the code and analysis), recorded so they aren't repeated. Each entry: what happened, what it cost, why it happened, and the rule that came out of it. Written October 25, 2026, from memory and the project's own history, so some early details may be incomplete.

## The big ones

### 1. Every test used the wrong XeFu build
* **What:** all tests ran Burnout 3 on `xefu7.xex`. Microsoft assigned Burnout 3 to `xefu6.xex`. See [[Issues and Fixes]] #40.
* **Cost:** days of testing on a build that isn't the game's. Later stalls with `xefu7` may mean nothing for Burnout 3, and a public comment (below) was wrong because of it. The Xenia fixes themselves (#31-#39) remain valid.
* **Why:** the starting setup was never questioned once it was built. The wiki already said `xbox.xex` picks a build per game, but it wasn't checked against the game.
* **Rule:** check the foundations (which build, which files, from where) before building on them, and write them down per test ([[Testing Setup]] checklist).

### 2. Said `xefu6.xex` wasn't on the PC without searching everywhere
* **What:** three folders were searched, and the reply said xefu6 was missing and the console probably never had it. It was in `Desktop\Back Compact\From BC update`, a folder connected to the session the whole time, and the same folder `xefu7.xex` had come from.
* **Cost:** the owner was told something false about their own files and nearly went looking on the console for nothing.
* **Why:** "not found where I looked" was reported as "not there".
* **Rule:** when reporting something missing, list where it was searched and where it wasn't. Search every connected folder first.

### 3. Built the test folder from part of the backward-compatibility update
* **What:** the test's Compatibility folder took the 2005 `xbox.xex` and `xefu.xex` from the base folder and only `xefu7` from the update, leaving out the other builds and the `xbox.xexp`/`xefu.xexp` patches that update the 2005 launcher.
* **Cost:** the root of #1. The launcher in the tests was the outdated 2005 one.
* **Why:** grabbed the newest-looking build instead of reproducing the console's real set.
* **Rule:** copy a real configuration whole, then remove things on purpose, never the other way round.

### 4. Helped post a wrong claim publicly
* **What:** the reply drafted for Xenia Edge's issue #319 said Burnout 3 fails in Edge and gets further in our fork, from a test on the wrong XeFu build and without detailed logs. A later side-by-side test with `xefu6` and detailed logs showed the Edge failure was real for our files, caused by an unanswered media check, not by the build; but that wasn't known when it was posted.
* **Cost:** a follow-up was needed to explain what was actually happening.
* **Why:** a surprising result ("we're ahead") was treated as a win, not as something to explain, and it went public unchecked.
* **Rule:** anything posted in the owner's name is checked against its evidence first, and surprising results are investigated before they're claimed.

### 5. Didn't look for existing work before starting
* **What:** Xenia Edge already ran original Xbox games through XeFu (since September 2026). It was found only weeks into the project.
* **Cost:** some work duplicated Edge's.
* **Why:** went straight to building, without a search of other forks, issues and forums.
* **Rule:** at the start of any project, search for prior work.

## Debugging mistakes

### 6. Blamed the #35 stall on XeFu's `abort()`
* **What:** a breakpoint in the log was assumed to be XeFu's C runtime `abort()`, and logging was built around that. The real cause was Xenia landing on a zero word after a call in XeFu's one-shot handler.
* **Why:** a guess from where `twui 22` instructions were, stated as a finding.
* **Rule:** label guesses as guesses; log the exact address before deciding what fired.

### 7. Placed the return address without accounting for `Processor::Execute`'s stack adjustment
* **What:** the fix for #35 wrote the host return address 8 bytes below the handler's stack pointer, but `Execute` lowers r1 by 176 first, so the word went in the wrong place and XeFu returned to garbage.
* **Why:** didn't read the code path the value goes through.
* **Rule:** before relying on a value's location, read every function between where it's set and where it's used.

### 8. Stackpoint search ended the main thread
* **What:** the next fix searched Xenia's stack records for the frame to return from and picked the thread's first frame, ending the whole thread.
* **Why:** assumed the handler's own frame record still held the host return address, without checking.
* **Rule:** prefer mechanisms that record the exact target (a saved context) over searching for it.

### 9. First APC fix didn't help
* **What:** the first attempt to write the I/O status block at APC delivery (`ee059c7e9`) looked the APC up by the wrong address: the queue holds the APC's list entry, 8 bytes in. Fixed in `c3cea8cca`.
* **Rule:** check what a list pointer actually points at.

### 10. Return-to-sentinel fix first covered only part of the code
* **What:** `99e080ab6` handled returns to `0xBCBCBCBC` only in user-range code. The next run showed kernel-mode code doing it too (`c16878393`).
* **Rule:** fix the general case when the cause is general.

## Process mistakes

### 11. Pushed code without compiling it
* **What:** a script edit for the trap-address logging spliced a line into the middle of a statement. CI failed 25 minutes later.
* **Rule:** run the syntax check (`clang++ -fsyntax-only`) on every changed file before pushing.

### 12. A downloaded build silently didn't reach the PC
* **What:** build `aca9546` was "installed" but never arrived. Since then every installed exe is read back and its build tag checked.
* **Rule:** verify the result of every install, not just the command.

### 13. Edited Edge's config while Edge was running
* **What:** Edge rewrote its config on exit and undid the change, so one test ran with the wrong settings.
* **Rule:** close a program before editing its config, and confirm the change took effect in the log.

### 14. Opened a second Xenia by accident and left test runs open
* **What:** `open_application` started a new Xenia window instead of focusing the existing one. Earlier, old runs kept files locked (#27).
* **Rule:** close the previous run before starting the next, and check nothing is still running.

### 15. Wiki updates that didn't apply
* **What:** a few scripted wiki edits silently failed to match (commit IDs of a different length), leaving "Built, not yet tested" on fixes that were confirmed.
* **Rule:** after a scripted edit, check the result.

### 16. Left clutter
* **What:** failed downloads left empty `xenia_canary_windows_exe (1).zip` … `(4).zip` files in Downloads.
* **Rule:** after each download, clear what isn't needed, including failed attempts.

### 17. A settings file was sent to the PC as an older copy
* **What:** a changed config sent under a file name used before arrived as the old version, though the copy reported success. Caught by reading the file back.
* **Rule:** every file sent to the PC gets a new name, and every write is read back and checked.

### 18. Read handler-time registers as the x86 state
* **What:** a run that stopped early was reported as "the emulated Xbox's stack pointer points at code". The values came from the heartbeat, which reads `r2`/`r7` (XeFu's x86 EIP/ESP) and a trace of `r2` values; while XeFu's handler runs those registers hold kernel values, not x86 ones. A follow-up guess (a cache file of return sites) was also tested and was wrong.
* **Cost:** two wrong explanations given before being checked; one test run on the cache guess.
* **Rule:** before reading meaning into a diagnostic, check in the code where its numbers come from and in which state they're valid.

## Patterns behind them

1. Not questioning the setup once it worked well enough.
2. Reporting partial searches as complete answers.
3. Treating surprising results as wins.
4. Trusting memory and summaries of a long session over re-reading the real files.
5. Favouring speed and low usage over checks that take minutes.
