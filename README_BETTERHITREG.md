# Better Hitreg (LeviLauncher, Minecraft Bedrock 1.26.62.1)

Build: push to GitHub (Actions builds the .levipack) or run ./scripts/package.ps1 -Abi arm64-v8a

## Crash fix (menu)
The pinned SDK (0.2.2) has a smaller ModuleInfo than current launchers expect, which
crashed pl::modmenu::registerModule. scripts/patch_sdk.py (run by CMake) adds the missing
onKeybind member plus zeroed padding. If the patch cannot be applied the build FAILS with
the header text printed, instead of producing a mod that crashes.

## Crash guards (data folder)
menu.pending / attack_hook.pending / sound_hook.pending are written before risky work and
removed afterwards. If one is left over, that step crashed last time and is skipped on the
next start. Delete the file to retry.

## Log files (data folder)
- hit_calibration.txt: attack_calls rises when you hit something.
- sound_samples.txt: sounds played within 1.5 s after an attack (log only, max 80 lines).

## Not done yet
Instant hit sound and hit flash (functions not fully located).
