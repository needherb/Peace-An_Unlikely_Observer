# Peace Skyrim

An incremental SKSE plugin project making the player an inconsequential bystander
to Skyrim's combat. Actors should not choose the player as a combat target,
while NPC fights continue. Collateral area effects are acceptable; complete
player protection is not the goal.

## Current status

0.11.2 fixes a mount-hook crash introduced in 0.11.1 and retains protection for
the player's last-used mount after dismounting, including stolen horses returning
home. See [release notes](docs/RELEASE-0.11.2.md).
It retains optional
debug logging and the existing player retaliation and lockpicking/trespass policy.

Version 0.9.0 is an experimental, event-driven player combat-cleanup prototype
with actor IDs, player-related hit logging, repeated-cleanup diagnostics,
nearby NPC combat transitions, and main-thread combat-target snapshots.
It also logs classified player crime alarms, pending NPC crime reactions, and
vanilla theft-warning attempts to investigate retaliation exceptions.
It rejects unprovoked player-target admission for Steam 1.6.1170, permitting
vanilla retaliation after player hits and classified caught theft/pickpocket alarms.
Other targets except protected mounts retain vanilla admission behavior; cleanup respects permitted player fights.
See [native diagnostics](docs/NATIVE-DIAGNOSTICS.md) for log interpretation and tests.
The exception investigation adds shared-clock hit/admission timing, victim
snapshots, inventory transfers, and pass-through player bounty modification logs.
See [exception diagnostics](docs/EXCEPTION-DIAGNOSTICS.md) for the previous 0.8.0
observation build, and [0.9.0 policy](docs/POLICY-0.9.0.md) for current behavior.
See [the fallback investigation](docs/FALLBACK.md) for thresholds and test steps.
Combat/searching events involving the player or protected mounts schedule one main-thread cleanup.
It collects processed opponents targeting the player (or targeted by the player),
then stops unauthorized combat. New games and successful save
loads also schedule cleanup. Stale queued work is discarded during save loading.

Aggression, factions, detection, and schedules are not edited. Existing bounties
are not cleared. New player lockpicking and trespass reports are suppressed upstream;
caught taking/pickpocketing retain vanilla bounty responses. Combat may restart, and initial attacks,
projectiles, or damage may occur before cleanup. An opponent's other fights may
also end when its combat is stopped. No damage immunity or quest progression
guarantee is provided. Tested gameplay results are recorded in docs/TESTING.md.

## Toolchain

- Windows x64, Visual Studio C++ tools and Windows SDK with C++23 support.
- xmake 3.0.0 or newer (3.0.9 installed during setup).
- CommonLibSSE-NG 10.1.0: https://github.com/alandtse/CommonLibSSE-NG, `ng` branch,
  pinned at `39f9d07a6ffabea8fb559eee87ab7d27cd463e8a` on 2026-10-02.
- This diagnostic release accepts only runtime 1.6.1170; other runtimes are rejected.
  VR is excluded from this project's initial scope.
- Initial in-game test target: Steam Skyrim 1.6.1170 (user confirmed).
  Install SKSE and Address Library matching that runtime before smoke testing.

The upstream xmake recipe manages its supporting packages. Keep upstream's
dependency versions rather than independently upgrading ABI-sensitive packages.

## Build

```powershell
git submodule update --init --recursive
xmake f -m releasedbg -p windows -a x64 -y
xmake -y
```

The DLL and symbols are staged under `dist/Data/SKSE/Plugins`. Builds stage
inside this project, without deploying to an installed game or mod manager.
Install the contents of `dist/Data` as a separate mod when ready to smoke test.
The game needs matching SKSE and Address Library for its executable version.

Optional editor support:

```powershell
xmake project -k compile_commands
xmake project -k vsxmake
```

## Smoke test

Use a disposable test profile/save. Launch through SKSE, reach the main menu,
start or load a game, and check `PeaceSkyrim.log` in the game's SKSE log folder
(normally under Documents/My Games/Skyrim Special Edition/SKSE).
Enable Debug=true in Data/SKSE/Plugins/PeaceSkyrim.ini and restart for detailed diagnostics.
With debug enabled, expect the plugin-loaded and game-data-loaded messages and a new-game or
save-loaded message, `Combat cleanup and hit diagnostic listeners registered`, and
`Cleanup complete: opponents=..., playerWasInCombat=...`.
Follow [the gameplay checklist](docs/TESTING.md) to verify behavior separately.

## Vortex installation

With the game closed, activate the minimal test profile. Use Mods > Install From
File to select `dist/PeaceSkyrim-0.11.2.zip`, disable older versions, enable
0.11.2, and Deploy Mods. Launch through SKSE. There is no ESP in the Plugins tab.
Keep only one PeaceSkyrim version enabled. To roll back, disable 0.11.2, enable
0.6.0, deploy, and reload an untouched disposable test save.

## Development segments

See [the roadmap](docs/ROADMAP.md) for behavior decisions and validation cases.

## Dependency updates and licensing

Update CommonLib deliberately, record the new hash here, rebuild, and retest.
The submodule records the exact revision; following `ng` does not auto-update it.
Current upstream is GPL-3.0-or-later with its published modding/linking exceptions.
This project's build metadata uses GPL-3.0-or-later accordingly. Preserve upstream
notices and ship corresponding source when distributing linked binaries.
See `lib/CommonLibSSE-NG/COPYING.txt` and `EXCEPTIONS.md` for upstream terms.
The diagnostic detector marks candidates only; stronger suppression is not applied.

## Optional debug logging

The release ships `Data/SKSE/Plugins/PeaceSkyrim.ini` with `[Logging]` and
`Debug=false`. Missing configuration also defaults to reduced logging. Set
`Debug=true` and restart Skyrim to collect detailed troubleshooting records;
restore `false` afterward. Startup, lifecycle, warnings, and errors remain logged
with debug off. See [release notes](docs/RELEASE-0.10.0.md).
