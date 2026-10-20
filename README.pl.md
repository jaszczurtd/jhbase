# jhbase

*Dostępna jest także [wersja angielska](README.md).*

jhbase udostępnia przenośny dekoder/enkoder UTF-8, parser JSON, dokładny odczyt
liczb, literały C i sekwencyjny writer JSON w C11. Wywołujący dostarcza pamięć
na tokeny i scratch. Parser zachowuje oryginalny zapis liczb.

jhbase definiuje kody statusów (`jb/status.h`) i `COUNTOF` (`jb/array.h`),
z których JaszczurHAL korzysta przez własne nagłówki. Biblioteka nie
konfiguruje ani nie linkuje HAL. Narzędzia testów
i bramki (Unity, cppcheck, PMD, toolchainy RISC-V i Xtensa, najstarszy
obsługiwany CMake) mają własne piny
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

Testy czytają fixture liczb z checkoutu HAL w `../JaszczurHAL`.
`-DJHBASE_HAL_ROOT=/path/to/JaszczurHAL` oraz, w razie potrzeby,
`-DJHBASE_TOOLS_ROOT=/path/to/tools` pozwalają wskazać inne lokalizacje.
Configure niczego nie pobiera ani nie zapisuje w tych katalogach.
`cmake --install` instaluje bibliotekę, nagłówki i pakiet CMake
(`find_package(jhbase)`); zobacz [zainstalowany pakiet](doc/pl/dependencies.md#zainstalowany-pakiet).

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
Opcjonalne NFC i właściwości znaków włącza `JHBASE_UNICODE=ON` i target
`jb::unicode`; opisuje je [sekcja Unicode](doc/pl/api.md#unicode).
Przypięty checkout przygotowuje `cmake -P scripts/ensure_utf8proc.cmake`.
JSON Pointer odczytuje i zapisuje ścieżki przez [json_pointer.h](include/jb/json_pointer.h).
Operacje plikowe hosta włącza `JHBASE_HOST=ON` i target `jb::host`; zobacz [pliki hosta](doc/pl/host.md).
JSON Schema pozostaje poza obecnym zakresem.
Publiczne ABI prototypu może zmienić się przed pierwszą integracją.

Licencja: [JaszczurHAL Attribution License (MIT-based)](LICENSE).
