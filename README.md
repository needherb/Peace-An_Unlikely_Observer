# Peace Skyrim

An incremental SKSE plugin project making the player an inconsequential bystander
to Skyrim's combat. Actors should not choose the player as a combat target,
while NPC fights continue. Collateral area effects are acceptable; complete
player protection is not the goal.

## Current status

0.11.2 fixes a mount-hook crash introduced in 0.11.1 and retains protection for
the player's last-used mount after dismounting, including stolen horses returning
home.

Aggression, factions, detection, and schedules are not edited. Existing bounties
are not cleared. New player lockpicking and trespass reports are suppressed upstream;
caught taking/pickpocketing retain vanilla bounty responses. Combat may restart, and initial attacks,
projectiles, or damage may occur before cleanup. An opponent's other fights may
also end when its combat is stopped. No damage immunity or quest progression
guarantee is provided.

## Toolchain

- Windows x64, Visual Studio C++ tools and Windows SDK with C++23 support.
- xmake 3.0.0 or newer (3.0.9 installed during setup).
- CommonLibSSE-NG 10.1.0: https://github.com/alandtse/CommonLibSSE-NG, `ng` branch,
  pinned at `39f9d07a6ffabea8fb559eee87ab7d27cd463e8a` on 2026-10-02.
- This diagnostic release accepts only runtime 1.6.1170; other runtimes are rejected.
  VR is excluded from this project's initial scope.
- Initial in-game test target: Steam Skyrim 1.6.1170.
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

## Optional debug logging

The release ships `Data/SKSE/Plugins/PeaceSkyrim.ini` with `[Logging]` and
`Debug=false`. Missing configuration also defaults to reduced logging. Set
`Debug=true` and restart Skyrim to collect detailed troubleshooting records;
restore `false` afterward. Startup, lifecycle, warnings, and errors remain logged
with debug off.
