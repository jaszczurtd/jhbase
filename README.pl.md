# jhbase

*Dostępna jest także [wersja angielska](README.md).*

jhbase udostępnia przenośny dekoder/enkoder UTF-8, parser JSON, dokładny odczyt
liczb, literały C i sekwencyjny writer JSON w C11. Wywołujący dostarcza pamięć
na tokeny i scratch. Parser zachowuje oryginalny zapis liczb.

To samodzielny prototyp. Korzysta z istniejących nagłówków statusów i tablic
HAL; nie konfiguruje ani nie linkuje HAL. Ich hashe są w
[cmake/dependencies.cmake](cmake/dependencies.cmake). Narzędzia testów
i bramki (Unity, cppcheck, PMD, toolchain RISC-V) mają własne piny
w `third_party/*_version.conf`; `./scripts/ensure_tools.sh` instaluje je
w `.build/tools`. Buforowy zapis float korzysta z przypiętego podzbioru Ryu;
zobacz [zależności i licencje](doc/pl/dependencies.md).

Do kompilacji potrzebne są CMake co najmniej 3.22 i kompilator C11/C++17,
po wcześniejszym `./scripts/ensure_tools.sh unity`:

```sh
cmake -S . -B .build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .build/host
ctest --test-dir .build/host --output-on-failure
```

Domyślny katalog HAL to `../libraries/JaszczurHAL`.
`-DJHBASE_HAL_ROOT=/path/to/JaszczurHAL` oraz, w razie potrzeby,
`-DJHBASE_TOOLS_ROOT=/path/to/tools` pozwalają wskazać inne lokalizacje.
Configure niczego nie pobiera ani nie zapisuje w tych katalogach.

Na Linuksie uruchom raz `./scripts/ensure_tools.sh` i `./scripts/prepare_comparison.sh`,
a następnie pełny lokalny zestaw kontroli przez `./runalltests.sh`.
`--list-stages` pokazuje etapy, a `--stage host` wybiera jeden z nich.
[Opis testów](doc/pl/testing.md) wymienia narzędzia i ograniczenia platform.
Do pracy w VS Code otwórz katalog główny repozytorium: [opis VS Code](doc/pl/vscode.md)
wymienia zadania, profile budowania i odświeżanie IntelliSense, sterowane
jednym skryptem CMake na każdym hoście.

Commity podlegają regułom JaszczurHAL: `./scripts/ensure_tools.sh` ustawia
`core.hooksPath` na śledzony katalog `.githooks` (albo wykonaj raz
`git config core.hooksPath .githooks`). `commit-msg` wymaga tematu w formacie
Conventional Commits (`type(scope): subject`, najwyżej 100 znaków);
`pre-commit` normalizuje stagowany tekst, formatuje C/C++ przez clang-format
i sprawdza drzewo stagowane (whitespace własnych źródeł, manifest Ryu,
składnia skryptów).

[Opis API](doc/pl/api.md) wyjaśnia obsługę pamięci, limitów i błędów.
JSON Pointer, JSON Schema i NFC są poza obecnym zakresem.
Publiczne ABI prototypu może zmienić się przed pierwszą integracją.

Licencja: [JaszczurHAL Attribution License (MIT-based)](LICENSE).
