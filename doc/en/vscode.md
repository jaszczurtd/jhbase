# Work in VS Code

Open the repository root as the workspace folder. The tasks mirror the
JaszczurHAL projects: the same labels and shortcuts, the same place for local
state and generated IntelliSense data. Instead of the Python entry point of the
firmware projects, one CMake script drives everything, so the tasks behave the
same on Linux, macOS and Windows: `scripts/vscode_workspace.cmake`.

## Requirements

- CMake 3.22 or later on `PATH`; the setting `jhbase.cmake` in
  `.vscode/settings.json` names another executable when needed.
- Ninja, or on Windows a Developer PowerShell with NMake and `cl.exe`. The
  Visual Studio generator writes no compile database, so the driver prefers
  Ninja and falls back to NMake Makefiles.
- The pinned Unity sources: `./scripts/ensure_tools.sh unity` on Linux, or
  `JHBASE_TOOLS_ROOT` pointing at an installed tools directory.
- The HAL headers at `../JaszczurHAL`, or `JHBASE_HAL_ROOT`.
- The `ms-vscode.cpptools` extension; `ms-vscode.cmake-tools` is optional and
  configures below `.build/vscode/cmake-tools`.

## Tasks

| Shortcut | Task | Effect |
| --- | --- | --- |
| `Ctrl+Shift+1` | `Project: Build` | Configure and build the active profile in Release, then select its compile database. |
| | `Project: Build (Debug)` | The same with `CMAKE_BUILD_TYPE=Debug` in a separate tree. |
| | `Project: Test` | Build, then run the CTest suites of the active profile. |
| `Ctrl+Shift+6` | `Project: Refresh IntelliSense` | Build, then regenerate `.vscode/c_cpp_properties.json`. |
| `Ctrl+Shift+7` | `Project: Clean` | Remove the build trees of the active profile and the generated IntelliSense file; other profiles and tools stay. |
| `Ctrl+Shift+9` | `Project: Config Dump` | Print the active profile, generator and paths. |
| `Ctrl+Shift+Alt+1` | `Project: Select board (GUI)` | Pick the active profile from a list. |
| `Ctrl+Shift+Alt+2` | `Project: Select board` | Type the id of the active profile. |
| | `Library: Full gate` | `./runalltests.sh` on Linux, `runalltests.ps1` on Windows. |
| | `Install git hooks` | Runs when the folder opens and sets `core.hooksPath` to `.githooks`. |

Shortcuts come from the user keybindings file of VS Code;
`.vscode/keybindings.reference.json` lists the entries, as in JaszczurHAL.
Upload, serial monitor and debug-probe tasks do not exist here: jhbase is a
host library.

## Profiles

| Profile | Compiler | Build tree |
| --- | --- | --- |
| `host` (default) | the default host compiler: GCC, Clang or MSVC | `.build/vscode/host/<config>/` |
| `host-clang` | `clang-18`, otherwise `clang` | `.build/vscode/host-clang/<config>/` |
| `host-sanitizers` | Clang with `JHBASE_SANITIZERS=ON` | `.build/vscode/host-sanitizers/<config>/` |

The selection is stored in the ignored `.vscode/jhbase.local.json`. Every
profile configures with `BUILD_TESTING=ON` and
`CMAKE_EXPORT_COMPILE_COMMANDS=ON` below `.build/vscode`, inside the only
build root the project accepts.

## IntelliSense

`Project: Build` and `Project: Refresh IntelliSense` write the ignored
`.vscode/c_cpp_properties.json`: the compile database of the active profile
and configuration, the compiler recorded there, C11 and C++17. Before the first
build, `settings.json` already points cpptools at the `host` Release database.
Generated sources, such as the API examples extracted from `doc/*/api.md`,
enter the database during a build, which is why the refresh builds first.

## Terminal

The tasks run this script; a terminal can call it the same way:

```sh
cmake -DJB_ACTION=select -DJB_PROFILE=host-clang -P scripts/vscode_workspace.cmake
cmake -DJB_ACTION=build -P scripts/vscode_workspace.cmake
cmake -DJB_ACTION=test -DJB_CONFIG=Debug -P scripts/vscode_workspace.cmake
cmake -DJB_ACTION=config-dump -P scripts/vscode_workspace.cmake
```

`JB_TOOLS_ROOT`, `JB_UNITY_ROOT`, `JB_HAL_ROOT`, `JB_GENERATOR`,
`JB_CMAKE_ARGS` and `JB_CTEST_ARGS` are described at the top of the script.

## Checks

The CTest suite `vscode_workspace` (label `tooling`; part of the host and
clean stages and of the MSVC runner) checks the task documents against the
driver and runs every action the tasks use on a scratch copy of the tree:
select, build, test, refresh-intellisense, config-dump, clean and
install-hooks. `-DJHBASE_VSCODE_CHECK=OFF` leaves it out of a build.
