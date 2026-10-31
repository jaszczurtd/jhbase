# jhbase

*Dostępna jest także [wersja angielska](README.md).*

jhbase udostępnia w C11 przenośny dekoder i enkoder UTF-8, parser JSON,
dokładny odczyt liczb, literały C i sekwencyjny writer JSON. Parser nie zmienia
wejścia i zachowuje oryginalny zapis liczb. Wywołujący przekazuje pamięć
na tokeny i scratch.

jhbase definiuje kody statusów (`jb/status.h`) i `COUNTOF` (`jb/array.h`),
z których korzysta JaszczurHAL. Biblioteka nie konfiguruje ani nie linkuje HAL.

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
Opisuje też [kontrole commitów](doc/pl/testing.md#kontrole-commitów).
Do pracy w VS Code otwórz katalog główny repozytorium i skorzystaj
z [instrukcji VS Code](doc/pl/vscode.md).

[Opis API](doc/pl/api.md) wyjaśnia obsługę pamięci, limitów i błędów.
Opcjonalną normalizację Unicode i właściwości znaków włącza `JHBASE_UNICODE=ON` i target
`jb::unicode`; opisuje je [sekcja Unicode](doc/pl/api.md#unicode).
JSON Pointer wyszukuje wartości i zapisuje ścieżki węzłów przez
[json_pointer.h](include/jb/json_pointer.h).
Pliki, procesy i własne drzewa konfiguracji JSON na hoście włącza
`JHBASE_HOST=ON` i target `jb::host`; zobacz [operacje hosta](doc/pl/host.md).
JSON Schema pozostaje poza obecnym zakresem.
Publiczne ABI nadal może się zmieniać; integracje powinny używać
pinu sprawdzonego commita.
[Opis zależności](doc/pl/dependencies.md) wyjaśnia ich przygotowanie, piny narzędzi
oraz wykorzystanie Ryu, utf8proc i ich licencje.

Licencja: [JaszczurHAL Attribution License (MIT-based)](LICENSE).
