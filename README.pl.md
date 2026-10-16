# jhbase

*Dostępna jest także [wersja angielska](README.md).*

jhbase udostępnia przenośny dekoder/enkoder UTF-8 i parser JSON w C11.
Wywołujący dostarcza pamięć na tokeny i scratch. Parser zachowuje oryginalny
zapis liczb, bez zamiany na wartości zmiennoprzecinkowe.

To samodzielny prototyp. Korzysta z istniejących nagłówków statusów i tablic
HAL oraz przypiętej kopii Unity do testów. Nie konfiguruje ani nie linkuje HAL.
Ścieżki CMake i hashe zależności są w
[cmake/dependencies.cmake](cmake/dependencies.cmake).

Do kompilacji potrzebne są CMake co najmniej 3.22 i kompilator C11/C++17:

```sh
cmake -S . -B .build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .build/host
ctest --test-dir .build/host --output-on-failure
```

Domyślny katalog zależności to `../libraries/JaszczurHAL`.
`-DJHBASE_HAL_ROOT=/path/to/JaszczurHAL` oraz, w razie potrzeby,
`-DJHBASE_UNITY_ROOT=/path/to/Unity` pozwalają wskazać inne lokalizacje.
Build niczego nie pobiera ani nie zapisuje w tych katalogach.

Na Linuksie pełny podstawowy zestaw kontroli uruchamia `./runalltests.sh`.
`--list-stages` pokazuje etapy, a `--stage host` wybiera jeden z nich.
[Opis testów](doc/pl/testing.md) wymienia narzędzia i ograniczenia platform.

[Opis API](doc/pl/api.md) wyjaśnia obsługę pamięci, limitów i błędów.
Konwersje liczb, writer JSON, JSON Schema i NFC są poza obecnym zakresem.
Publiczne ABI prototypu może zmienić się przed pierwszą integracją.

Licencja: [JaszczurHAL Attribution License (MIT-based)](LICENSE).
