# Zależności i profil kompilacji

Przenośna biblioteka jest w C11. Publiczne nagłówki obsługują też C++17;
biblioteka nie linkuje runtime C++. CMake sprawdza hashe przyjętych zależności
i nie pobiera ich podczas configure.

## Wspólne pliki HAL

Do pierwszej integracji `jb/status.h` korzysta z jedynej definicji
`hal_status_t` w HAL. Testy i przykłady używają też `hal_array.h`, a test
literałów czyta fixture liczb z jawnie wskazanego checkoutu HAL. Runtime HAL,
SDK ani generatory produkcyjne nie są konfigurowane ani linkowane.

Hashe nagłówków HAL i źródeł Unity zapisuje
[dependencies.cmake](../../cmake/dependencies.cmake). Domyślny katalog HAL
to `../libraries/JaszczurHAL`; inny wybierz przez `JHBASE_HAL_ROOT`.

`jb::status` przekazuje `HAL_STATUS_FIXED_32`. W GCC/Clang ustala to reprezentację
enuma statusów na 32 bity bez zmiany globalnych flag rozmiaru enumów ani
atrybutów ABI ARM używanych przez libc. MSVC już używa enuma 32-bitowego.
Przy bezpośredniej kompilacji definiuj selektor spójnie przed pierwszym include
statusów. Bez selektora dotychczasowy domyślny układ HAL pozostaje bez zmian.

## Buforowy zapis Ryu

Niezmieniony podzbiór w [third_party/ryu](../../third_party/ryu/version.json)
pochodzi z [rewizji Ryu 4c0618b0](https://github.com/ulfjack/ryu/tree/4c0618b0e44f7ef027ebae05d2cc7812048f7c8f).
[Manifest SHA-256](../../third_party/ryu/SHA256SUMS) obejmuje każdy dostarczony
plik źródłowy, nagłówek i licencję. Ta dystrybucja wybiera
[Boost Software License 1.0](../../third_party/ryu/LICENSE-Boost); zachowana jest
również alternatywna [licencja Apache 2.0](../../third_party/ryu/LICENSE-Apache2)
z upstreamu. [Licencja projektu](../../LICENSE) obejmuje kod jhbase.

Writer wywołuje wyłącznie `f2s_buffered_n` i `d2s_buffered_n`, z
`RYU_OPTIMIZE_SIZE` oraz `RYU_ONLY_64_BIT_OPS` na każdym targecie. Te funkcje
zapisują do stałych buforów lokalnych. Konwersja wejścia dziesiętnego i literały
C o ustalonej precyzji korzystają ze wspólnej dokładnej arytmetyki jhbase.

Źródła upstream zawierają też helpery alokujące. Pozostają niezmienione
i poza publicznym API jhbase. Usuwanie nieużywanych sekcji eliminuje je ze
zlinkowanych programów. `jb::base` przekazuje tę zasadę linkowania; kompilacja
bezpośrednia musi używać sekcji funkcji/danych i ich GC lub odpowiedników
MSVC `/Gy /Gw` oraz `/OPT:REF`. Asercje upstream są wyłączone dla tej wewnętrznej
konwersji o ograniczonych danych; instrumentowane testy wykonują właściwe
funkcje buforowe.

Kontrola freestanding sprawdza odwołania własnych obiektów i kompletny
zlinkowany probe analityczny, w tym Ryu i libc, pod kątem symboli alokacji.
Hostowy test przerywa też wykonanie przy malloc/calloc/realloc/free podczas
wywołań biblioteki. Probe ma mapę pamięci tylko do analizy; nie jest firmware
do wgrania na urządzenie. Pomiary na płytkach korzystają z firmware
w `tests/device`, opisanego w [testach](testing.md#pomiary-na-urządzeniach).

## Porównanie i narzędzia jakości

coreJSON i yyjson to przypięte źródła badawcze opisane przy
[wyborze parsera](parser-choice.md); żaden nie należy do przenośnej biblioteki.
Unity jest zależnością testów hostowych. Valgrind, sanitizery, cppcheck i PMD CPD
są narzędziami kontroli jakości, bez udziału w runtime. Wymagania narzędzi
i osobne kontrole Windows opisują [testy](testing.md) oraz
[przygotowanie Windows](windows.md).

Unity, cppcheck, PMD oraz toolchainy RISC-V i Xtensa są przypięte w plikach
`*_version.conf` w [third_party](../../third_party), a
[ensure_tools.sh](../../scripts/ensure_tools.sh) pobiera je, weryfikuje
i buduje w `.build/tools`. Dla Unity, cppcheck, PMD i toolchainu RISC-V te
pliki są jedynymi pinami: JaszczurHAL czyta je ze swojego przypiętego
checkoutu jhbase zamiast trzymać kopie, więc obie bramki używają jednej
rewizji każdego narzędzia. Zawierają tylko rewizje i skróty; katalogi
instalacji wybiera każdy konsument osobno. Toolchain Xtensa służy profilowi
ESP32-S3. JaszczurHAL dostaje swój kompilator Xtensa razem z ESP-IDF
i sprawdza, czy ten pin zgadza się z manifestem narzędzi ESP-IDF, bo archiwum
z pomiarów na urządzeniu linkuje się z firmware ESP-IDF.
