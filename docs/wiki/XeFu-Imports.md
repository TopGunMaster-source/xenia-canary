# XeFu Imports

What XeFu (`xefu7.xex`) needs from the 360 kernel and xam, and how complete Xenia's versions are. Every gap here is a place a future bug can come from, so this list is checked before chasing a new problem. Audited October 24, 2026, from the import table Xenia logs at load (215 imports: 166 xboxkrnl, 47 xam, 2 xefutitle7).

## Were missing in Xenia (fixed in this branch)
An import Xenia doesn't define returns whatever happened to be in `r3`, usually the call's first argument, so XeFu silently took the wrong branch. Now defined:

| Import | Used by XeFu for | Xenia now |
|---|---|---|
| `XNetLogonGetServiceNetworkID` | network check at start-up (**called in every run so far**) | returns 0: not signed in |
| `XNetLogonGetExtendedStatus` | network status | 0, outputs cleared |
| `IoCreateFile` | opening a file | forwards to `NtCreateFile` |
| `XeKeysConsoleSignatureVerification` | checking console-signed data (saves) | accepts the signature |
| `StfsCreateDevice`, `StfsControlDevice` | content packages (saves, probably) | fail with `STATUS_NOT_IMPLEMENTED` and log |
| `VdSetColorProfileAdjustment`, `VdSetWSSOption` | display tweaks | no-op |

## Still missing
* `RtlUnwind`, `__C_specific_handler`: structured exception handling. Xenia doesn't dispatch guest exceptions to SEH handlers at all (`RtlRaiseException` just logs and returns, now with the exception code). XeFu raises `STATUS_NO_MEMORY` this way when its heap runs out (`820C0B14`, `820C167C`), so an out-of-memory would continue instead of unwinding. Not seen in any run yet.
* `XamExecuteChallenge`, `NetDll_Xnp*`: Xbox Live and system-link networking. Only needed for online/LAN play.

## Stubs worth watching
Stubs return a fixed answer. The ones XeFu depends on most: `KeSweepIcacheRange` (now drops stale translations, #9), `NtDeviceIoControlFile` (DVD queries; #23 added the media check), `NtFlushBuffersFile`, `IoDismountVolume*`, the `Vd*` video functions (graphics start-up: the next big area), `XAudio*Voice*` (audio), `XamInputGetCapabilities` (controllers).

## XeFu's own checks
XeFu calls its C runtime `abort()` (`820C75E0`) from about 30 places and `KeBugCheck`/`KeBugCheckEx` from 8. When one fires, Xenia now logs the guest call chain (`6f56aab`), which names the check.
