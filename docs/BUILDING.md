# Building Second Sight Switch

> **Status:** this guide documents the current developer workspace. It is not yet a clean-checkout build guide. This repository does not contain the game executable, generated guest code, runtime dependencies, or Vulkan SDK components.

## Build flow

The current build has two stages:

1. **Generate the guest code** from the supported Xbox executable with the selected local `xboxrecomp` tool checkout and project patches.
2. **Compile the Switch NRO** with devkitPro/devkitA64, the Switch runtime, and the Vulkan/NVK dependencies.

The build does not copy the NRO to a console. It stages the output under the local `switch_sd` directory.

## Required local inputs

| Input | Current workspace location | Notes |
|---|---|---|
| Xbox NTSC executable | `porting/switch/game_files/game_ntsc.xbe` | Must match SHA-256 `226ebc4834fbd2f8f7c7e657fb4b1c8b8a7f53105d9189e6615bca1f4983c83e` |
| Recompilation toolkit | `external_versions/mksm-switch/xboxrecomp` | Used by the current generation script |
| Python environment | `external_versions/xboxrecomp/.venv/Scripts/python.exe` | Used for disassembly and recompilation |
| Switch runtime | `external_versions/secondsight-vulkan-runtime` | The Switch-enabled runtime used by the current Vulkan build |
| NVK SDK files | `external_versions/nvk_sdk` | Passed to CMake as `NVK_SDK` |
| glslang CMake package | `external_versions/glslang-switch/lib/cmake/glslang` | Passed to CMake as `glslang_DIR` |
| Switch toolchain | `C:\devkitPro` | The current scripts use this installed path and devkitPro MSYS2 |

All game inputs must come from the user's own copy. Do not add them to Git.

## 1. Generate the guest code

From the workspace root, use Windows PowerShell:

```powershell
.\porting\switch\regen.ps1
```

The script verifies the XBE hash before analyzing it. It then runs disassembly, function identification, ABI analysis, project-specific analysis repairs, and code generation. It applies the recorded patches and writes the generated sources under `porting/switch/generated_boot/`.

`-LiftOnly` skips the first analysis stages and reuses the saved local analysis. It is only suitable when that analysis already exists and only the lift/overrides need regeneration:

```powershell
.\porting\switch\regen.ps1 -LiftOnly
```

Generated game implementation code is a local build input. It is excluded from this repository and must not be committed or redistributed.

## 2. Build the NRO

Open the devkitPro MSYS2 shell, change to the workspace root, and run:

```bash
cd "/c/path/to/Reverse Engineer"
bash porting/switch/tools/build_r13k.sh
```

Replace `/c/path/to/Reverse Engineer` with the MSYS path to the local workspace. The current script configures a Release build with Vulkan enabled, CPU boost and profiling disabled, and the reviewed r13k options. It uses six parallel build jobs.

Expected outputs:

- NRO: `porting/switch/switch_sd/switch/secondsight/second_sight_vulkan_r13k_language.nro`
- ELF snapshot: `porting/switch/snapshots/r13k-language/r13k_language.elf`
- CMake build tree: `porting/switch/build-vulkan-perfdiag/`

The script prints the staged NRO's SHA-256 when it completes. A successful compiler/linker run does not confirm that the game works correctly on hardware.

## Game resources and language

Resources are separate from the compilation inputs. The running NRO expects user-supplied game files under:

```text
sdmc:/switch/secondsight/game/e/
sdmc:/switch/secondsight/game/r/
```

The Xbox executable is identical in the English and Russian resource trees used by this project; the language setting selects which resource root is mounted. Place `en` or `ru` in:

```text
sdmc:/switch/secondsight/language.txt
```

Then fully close and relaunch the NRO. Saves are shared. The r13k language selector has not yet been confirmed on the console.

## Clean-checkout limitations

This guide describes a local working setup, not a reproducible GitHub release. Before a fresh clone can build, the project still needs to:

- document and pin the exact revisions of the recompiler, Switch runtime, SDK pieces, and other dependencies;
- make dependency setup independent of the developer's `external_versions/` folder and local absolute paths;
- provide a clean, documented route to supply and validate the user's own XBE and resources;
- verify that all required project-authored patches are present and licensed for the repository;
- perform a clean build from the resulting checkout and document the supported toolchain versions.

Until then, the Quick Build section in the top-level README is only for the existing developer workspace.

