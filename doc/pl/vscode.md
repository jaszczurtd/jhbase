# Praca w VS Code

Otwórz katalog główny repozytorium jako folder roboczy. Zadania odpowiadają
projektom JaszczurHAL: te same etykiety i skróty, to samo miejsce na stan
lokalny i generowane dane IntelliSense. Zamiast pythonowego punktu wejścia
projektów firmware wszystkim steruje jeden skrypt CMake, więc zadania działają
tak samo na Linuksie, macOS i Windows: `scripts/vscode_workspace.cmake`.

## Wymagania

- CMake 3.22 lub nowszy na `PATH`; ustawienie `jhbase.cmake`
  w `.vscode/settings.json` wskazuje w razie potrzeby inny plik wykonywalny.
- Ninja albo na Windows Developer PowerShell z NMake i `cl.exe`. Generator
  Visual Studio nie zapisuje bazy kompilacji, dlatego sterownik woli Ninja,
  a w drugiej kolejności NMake Makefiles.
- Przypięte źródła Unity: `./scripts/ensure_tools.sh unity` na Linuksie albo
  `JHBASE_TOOLS_ROOT` wskazujący zainstalowany katalog narzędzi.
- Fixture liczb HAL do testów w `../JaszczurHAL` albo `JHBASE_HAL_ROOT`.
- Rozszerzenie `ms-vscode.cpptools`; `ms-vscode.cmake-tools` jest opcjonalne
  i konfiguruje w `.build/vscode/cmake-tools`.

## Zadania

| Skrót | Zadanie | Działanie |
| --- | --- | --- |
| `Ctrl+Shift+1` | `Project: Build` | Konfiguruje i buduje aktywny profil w trybie Release, potem wybiera jego bazę kompilacji. |
| | `Project: Build (Debug)` | To samo z `CMAKE_BUILD_TYPE=Debug` w osobnym drzewie. |
| | `Project: Test` | Buduje, potem uruchamia zestawy CTest aktywnego profilu. |
| `Ctrl+Shift+6` | `Project: Refresh IntelliSense` | Buduje, potem generuje na nowo `.vscode/c_cpp_properties.json`. |
| `Ctrl+Shift+7` | `Project: Clean` | Usuwa drzewa budowania aktywnego profilu i wygenerowany plik IntelliSense; inne profile i narzędzia zostają. |
| `Ctrl+Shift+9` | `Project: Config Dump` | Wypisuje aktywny profil, generator i ścieżki. |
| `Ctrl+Shift+Alt+1` | `Project: Select board (GUI)` | Wybiera aktywny profil z listy. |
| `Ctrl+Shift+Alt+2` | `Project: Select board` | Przyjmuje wpisany identyfikator aktywnego profilu. |
| | `Library: Full gate` | `./runalltests.sh` na Linuksie, `runalltests.ps1` na Windows. |
| | `Install git hooks` | Uruchamia się przy otwarciu folderu i ustawia `core.hooksPath` na `.githooks`. |

Skróty pochodzą z pliku skrótów użytkownika VS Code;
`.vscode/keybindings.reference.json` wymienia wpisy, tak jak w JaszczurHAL.
Zadań wgrywania, monitora szeregowego i sondy debugowania tu nie ma: jhbase
jest biblioteką hostową.

## Profile

| Profil | Kompilator | Drzewo budowania |
| --- | --- | --- |
| `host` (domyślny) | domyślny kompilator hosta: GCC, Clang albo MSVC | `.build/vscode/host/<config>/` |
| `host-clang` | `clang-18`, w przeciwnym razie `clang` | `.build/vscode/host-clang/<config>/` |
| `host-sanitizers` | Clang z `JHBASE_SANITIZERS=ON` | `.build/vscode/host-sanitizers/<config>/` |

Wybór jest zapisany w ignorowanym `.vscode/jhbase.local.json`. Każdy profil
konfiguruje z `BUILD_TESTING=ON` i `CMAKE_EXPORT_COMPILE_COMMANDS=ON`
w `.build/vscode`, wewnątrz jedynego katalogu budowania, jaki projekt
dopuszcza.

## IntelliSense

`Project: Build` i `Project: Refresh IntelliSense` zapisują ignorowany
`.vscode/c_cpp_properties.json`: bazę kompilacji aktywnego profilu
i konfiguracji, zapisany tam kompilator, C11 i C++17. Przed pierwszym buildem
`settings.json` wskazuje cpptools bazę profilu `host` w trybie Release.
Generowane źródła, na przykład przykłady API wydobyte z `doc/*/api.md`,
trafiają do bazy podczas buildu, dlatego odświeżenie najpierw buduje.

## Terminal

Zadania uruchamiają ten skrypt; terminal może wywołać go tak samo:

```sh
cmake -DJB_ACTION=select -DJB_PROFILE=host-clang -P scripts/vscode_workspace.cmake
cmake -DJB_ACTION=build -P scripts/vscode_workspace.cmake
cmake -DJB_ACTION=test -DJB_CONFIG=Debug -P scripts/vscode_workspace.cmake
cmake -DJB_ACTION=config-dump -P scripts/vscode_workspace.cmake
```

`JB_TOOLS_ROOT`, `JB_UNITY_ROOT`, `JB_HAL_ROOT`, `JB_GENERATOR`,
`JB_CMAKE_ARGS` i `JB_CTEST_ARGS` są opisane na początku skryptu.

## Kontrole

Zestaw CTest `vscode_workspace` (etykieta `tooling`; część etapów host i clean
oraz runnera MSVC) sprawdza dokumenty zadań względem sterownika i wykonuje na
tymczasowej kopii drzewa każdą akcję, której używają zadania: select, build,
test, refresh-intellisense, config-dump, clean i install-hooks.
`-DJHBASE_VSCODE_CHECK=OFF` pomija go w buildzie.
