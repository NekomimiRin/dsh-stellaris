# dsh-stellaris — a DSH plugin + local bridge for Stellaris

[中文说明](README.md) · English

Brings **Stellaris 4.5.x** under **DSH** (DeepSeek Harness) control: a plugin that registers 187 tools,
and a C++ DLL that executes them inside the game process through the engine's own command interface.

| Directory | What it is |
|---|---|
| `dsh-stellaris/` | The plugin: **187 `stellaris_*` tools** — reads and commands over a named pipe |
| `stellaris-bridge-patch/` | The bridge: a C++ DLL injected into the game, **299 IPC methods** (direct memory reads + the engine's own command producers), plus the full reverse-engineering notes |
| `stellaris-save/` | Small Python tools that read save files (cross-checks, injection, reports) |

## Downloads (if you do not want to build)

The [Releases](https://github.com/NekomimiRin/dsh-stellaris/releases) page carries three archives:

| Archive | What it is for |
|---|---|
| `stellaris-bridge-4.5.4-win64.zip` | **4.5.4 only**: a prebuilt `stellaris_bridge.dll` + the injector scripts + install notes |
| `dsh-stellaris-plugin-0.1.0.zip` | the plugin itself — drop it into the DSH plugins directory |
| `dsh-stellaris-0.1.0.zip` | a snapshot of the whole repository (bridge source and docs included), for machines without git |

On any other game version do not use the prebuilt DLL — offsets change; build it yourself as described in `docs/build.md`.

## Read this first

- **[dsh-stellaris/docs/tools.md](dsh-stellaris/docs/tools.md)** — what every tool does, its parameters and status
- **[dsh-stellaris/docs/build.md](dsh-stellaris/docs/build.md)** — build, inject, install, self-check
- **[dsh-stellaris/README.md](dsh-stellaris/README.md)** — concepts and terminology, the full tool table
- **[stellaris-bridge-patch/README.md](stellaris-bridge-patch/README.md)** — engine internals: where the fields live, how the commands are built, what did not work

## Requirements

- Stellaris 4.5.x on Windows (the bridge was built against 4.5.4)
- [Yidhar/stellaris-mcp](https://github.com/Yidhar/stellaris-mcp) (MIT) for the DLL skeleton,
  and [stl](https://github.com/Yidhar/stellaris-Launcher) to launch the game with plugins loaded
- DSH — the plugin registers its tools through `apply(ctx)`
- Node ≥ 22; Python 3.10+ (only for the save tools)

## How it works

```
DSH  ──tools──▶  plugin (index.js)  ──named pipe \\.\pipe\stellaris_mcp_bridge──▶  bridge DLL  ──▶  game
```

The bridge never re-implements game logic. Wherever the engine exposes a producer (a command builder,
a factory function), the bridge calls it — the same path the UI takes — instead of writing raw memory.
Read-only probes cover what has no producer.

## Status

All 187 tools have been exercised in a real game. The tool table has no `⚠` left.

## Notes

- **Unofficial.** Not affiliated with Paradox Interactive. No game files, assets or saves are included.
- The bridge reads memory and calls the engine's command interface **inside the local game process only**:
  no network access, no modification of game files.
- Game version upgrades change offsets. Re-generate the SDK headers and re-check the bridge notes.

## Acknowledgements

- **[V2t-Nostalgia/OGAS](https://github.com/V2t-Nostalgia/OGAS)** (formerly Imperial Auto Governor, GPL-3.0):
  the idea of *calling the game's own producer functions instead of hand-assembling commands* comes from it.
  Only its public documentation and reverse-engineering notes were read; **no code was copied** —
  the implementations differ (a C++ DLL + local IPC here, a Python agent there).
- **[Yidhar/stellaris-mcp](https://github.com/Yidhar/stellaris-mcp)** (MIT): the DLL skeleton.
- **[Yidhar/stellaris-Launcher](https://github.com/Yidhar/stellaris-Launcher)**: the `stl` plugin loader.

## Licence

MIT — see [LICENSE](LICENSE). Third-party notices in [NOTICE](NOTICE).
