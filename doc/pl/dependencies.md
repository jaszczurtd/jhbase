# Zależności i profil kompilacji

Przenośna biblioteka używa C11. Publiczne nagłówki obsługują też C++17;
biblioteka nie linkuje runtime C++. CMake sprawdza hashe przyjętych zależności
i nie pobiera ich podczas configure.

## Kody statusów i HAL

`jb/status.h` zawiera jedyną definicję `hal_status_t` i jego funkcji
pomocniczych, a `jb/array.h` jedyne `COUNTOF`. JaszczurHAL zależy od jhbase:
`hal/core/hal_status.h` i `hal/core/hal_array.h` dołączają te nagłówki.

`hal_status_t` ma wszędzie 32 bity. W GCC i Clang nagłówek nadaje enumowi
`mode(SI)`, więc `-fshort-enums`, domyślne w ARM EABI, go nie zmniejsza,
a pozostałe enumy zachowują swój rozmiar. MSVC zawsze używa enuma 32-bitowego.
Nie trzeba niczego definiować.

Biblioteka nie konfiguruje ani nie linkuje HAL. Tylko test literałów czyta
zestaw oczekiwanych wyników liczbowych z checkoutu HAL, domyślnie
`../JaszczurHAL`; inny wskażesz przez
`JHBASE_HAL_ROOT`. Hashe źródeł Unity zapisuje
[dependencies.cmake](../../cmake/dependencies.cmake).

## Zainstalowany pakiet

`cmake --install` zapisuje nagłówki, bibliotekę statyczną i pakiet CMake:
`find_package(jhbase)` udostępnia `jb::base` i `jb::status`. Z `JHBASE_HOST=ON`
eksportuje też target C `jb::filesystem` i `jb::host`; zobacz
[operacje systemu plików](filesystem.md). `JHBASE_ARCHIVE=ON` dokłada
`jb::archive`, a `JHBASE_DOWNLOAD=ON` dokłada `jb::download`; oba opisano niżej.
Pakiet zapisuje
rewizję biblioteki w `jhbase_REVISION` i układ publicznych nagłówków
w `jhbase_ABI_PROFILE`, na przykład `natural-ptr32-status32-bool8`. Rewizja
pochodzi z `JHBASE_REVISION`, a bez niej z git HEAD tego checkoutu, z dopiskiem
`-dirty` przy lokalnych zmianach; w drzewie bez gita ma wartość `unknown`.
Plik wersji przyjmuje tylko tę samą wersję minor i tę samą szerokość wskaźnika.

Projekt nadrzędny, który dołącza jhbase przez `add_subdirectory`, buduje
bibliotekę tylko wtedy, gdy linkuje `jb::base`, i nie rejestruje testów jhbase.
Jego instalacja zapisuje same nagłówki statusów (`abi.h`, `array.h`,
`status.h`) i pakiet `jb::status`; `JHBASE_INSTALL_LIBRARY=ON` dokłada
bibliotekę i pozostałe nagłówki.

## Opcjonalne Unicode

`JHBASE_UNICODE=ON` buduje `jb::unicode`, który linkuje bibliotekę podstawową
i niezmieniony checkout utf8proc, ignorowany przez Git.
[Pin](../../third_party/utf8proc_version.conf) wskazuje utf8proc 2.12.0
z Unicode 18.0.0. Przygotuj go jawnie przez
`cmake -P scripts/ensure_utf8proc.cmake`; `-DJB_VERIFY_ONLY=ON` sprawdza go
bez pobierania i zastępowania plików. Aby użyć checkoutu zarządzanego przez
projekt nadrzędny, ustaw `JHBASE_UTF8PROC_DIR` na jego bezwzględną ścieżkę.
Pusta wartość wybiera `third_party/utf8proc`; błędna ścieżka zewnętrzna kończy
się błędem, bez przełączania na ten katalog. Obie ścieżki obowiązuje ten sam
pin. Skrypt przygotowania zarządza tylko domyślnym katalogiem; configure
i build nie zmieniają zewnętrznych źródeł.
Configure i każdy build modułu Unicode sprawdzają rewizję, origin,
czystość oraz bajty źródeł względem obiektów Git, także przy zmianach
ukrytych flagami indeksu. Instalacja pobiera licencję z wybranego checkoutu.
[Licencja upstream](https://github.com/JuliaStrings/utf8proc/blob/79cdd5ab40c79afa559f689ffc13b76812dee1ac/LICENSE.md) to MIT dla kodu,
Unicode-DFS-2015 dla tabel.

Testy wymagają też niezależnego zestawu normalizacji i jego licencji
Unicode-3.0. Ich [pin](../../third_party/unicode_test_version.conf) zapisuje
adresy pobrania i SHA-256; wersję Unicode pobiera z pinu utf8proc.
Adres licencji wskazuje pełny commit w oficjalnym repozytorium ICU;
zachowujemy cały plik upstream z notą Unicode i dodatkowymi notami ICU.
Walidacja pinu odrzuca adresy bez pełnego commita.
`cmake -P scripts/ensure_unicode_test.cmake` przygotowuje je w ignorowanym
katalogu `tests/fixtures/unicode`. `-DJB_VERIFY_ONLY=ON` sprawdza je offline.
Configure weryfikuje hashe plików; hook dla indeksu sprawdza tylko pin
i nie potrzebuje pobranych danych. Paczka testowa Windows zawiera
zweryfikowane pliki.

Adapter używa operacji buforowych z jawnymi długościami. Alokujące funkcje
upstream pozostają niezmienione i są usuwane przez GC sekcji. Linkowana
normalizacja nadal wymaga tabel Unicode we flashu; brak heap nie usuwa tego
kosztu. Domyślny build pomija moduł. Pełna instalacja z włączonym Unicode
eksportuje `jb::unicode` i dostarcza jego licencję. Zasady pamięci scratch
i limitów pracy opisuje [API](api.md#unicode).

## Opcjonalne rozpakowywanie archiwów

`JHBASE_ARCHIVE=ON` buduje [`jb::archive`](archive.md) i wymaga
`JHBASE_HOST=ON`. Moduł linkuje niezmienione checkouty libarchive 3.8.11
i zlib 1.4, ignorowane przez Git. Ich piny,
[libarchive](../../third_party/libarchive_version.conf) i
[zlib](../../third_party/zlib_version.conf), zapisują repozytorium i pełny
commit. Przygotuj je jawnie przez `./scripts/ensure_tools.sh libarchive zlib`,
które uruchamia `scripts/ensure_libarchive.cmake` i `scripts/ensure_zlib.cmake`;
`-DJB_VERIFY_ONLY=ON` przekazane tym skryptom sprawdza checkouty bez pobierania.
Tak jak przy utf8proc, configure i każdy build modułu sprawdzają rewizję,
origin, czysty stan i bajty źródeł obu checkoutów.

Oba projekty budują się własnymi plikami CMake, dołączanymi z funkcji, która
wyłącza wszystko, czego moduł nie używa: programy libarchive, testy, instalację,
biblioteki kryptograficzne i XML, iconv, ACL i atrybuty rozszerzone oraz kodeki
xz, zstd, bzip2, lz4 i lzo. Moduł rejestruje tylko czytniki TAR i ZIP oraz
filtr gzip, więc żaden zewnętrzny program dekompresujący nie startuje. Te opcje
zostają w zasięgu funkcji. Cache projektu nadrzędnego dostaje opcje, które
zlib i libarchive deklarują same, oraz `ZLIB_INCLUDE_DIR` wskazujące
przypięte zlib; inna, wcześniej ustawiona wartość zatrzymuje configure.
Generator jednokonfiguracyjny wymaga `CMAKE_BUILD_TYPE`, bo libarchive wpisałoby
inaczej Release do cache całego buildu.

Pełna instalacja eksportuje `jb::archive` razem z `jb::libarchive`
i `jb::zlib`. Ich pliki nazywają się `jb_libarchive` i `jb_zlib`, więc we
wspólnym prefiksie nie zastąpią bibliotek systemowych, a pakiet zawiera obie
licencje w `share/jhbase/licenses`. libarchive używa licencji BSD w wersji
dwuklauzulowej; część plików ma trzyklauzulową notę UC Regents, status
domeny publicznej albo wybór CC0, OpenSSL i Apache 2.0, co wymienia jego
[COPYING](https://github.com/libarchive/libarchive/blob/03036ffa409a2aa1c5bbad35be2202641a040fda/COPYING). zlib używa [licencji zlib](https://github.com/madler/zlib/blob/99623ef7097352da7993ba614161fd6a311e046e/LICENSE).

## Opcjonalne pobieranie przez HTTPS

`JHBASE_DOWNLOAD=ON` buduje [`jb::download`](download.md) i wymaga
`JHBASE_HOST=ON`. Moduł linkuje niezmieniony checkout curl 8.23.0 ignorowany
przez Git; jego [pin](../../third_party/curl_version.conf) zapisuje repozytorium
i pełny commit taga wydania. Przygotuj go przez `./scripts/ensure_tools.sh curl`,
które uruchamia `scripts/ensure_curl.cmake`; `-DJB_VERIFY_ONLY=ON` tylko
sprawdza, bez pobierania. Configure i każdy build modułu sprawdzają rewizję,
origin, czysty stan i bajty źródeł, tak jak dla pozostałych pinów. Każdy
przygotowany checkout wyłącza konwersje z `.gitattributes` upstreamu, więc
pliki, które curl oznacza do zapisu z CRLF, zachowują bajty przypiętego commita.

curl buduje się przez własne pliki CMake jako biblioteka statyczna, dodana z
funkcji, która wybiera backend TLS, zamiast go wykrywać: Schannel na Windows,
systemowy OpenSSL na Linuksie. Przed configure zainstaluj pakiet deweloperski
OpenSSL (`libssl-dev` w Debianie i Ubuntu) oraz `ca-certificates`. Funkcja
włącza wątkowe rozwiązywanie nazw i IPv6. Wyłącza program curl, testy,
przykłady, dokumentację, wszystkie protokoły poza HTTP i HTTPS, kompresję
(zlib, brotli, zstd), HTTP/2 i HTTP/3, IDN, listy sufiksów publicznych, SSH,
LDAP, cookies, pamięć podręczną HSTS i alt-svc, netrc oraz uwierzytelnianie
Digest, Kerberos, NTLM i Negotiate; Basic zostaje dla danych logowania proxy.
Configure przerywa pracę, gdy curl zgłasza inne protokoły, nie ma
asynchronicznego DNS, TLS lub bezpiecznej wielowątkowo inicjalizacji albo
włącza którąś z wyłączonych funkcji. Na Linuksie przerywa też, gdy curl nie
znajdzie systemowego pakietu ani katalogu CA, a w przeciwnym razie wypisuje
znaleziony magazyn.

Pełna instalacja eksportuje `jb::download` razem z `jb::libcurl`, którego plik
nazywa się `jb_libcurl`, i dołącza licencję curl w `share/jhbase/licenses/curl`.
Na Linuksie konfiguracja pakietu ponownie wyszukuje OpenSSL i Threads, bo
statyczne libcurl je linkuje. curl używa
[licencji curl](https://github.com/curl/curl/blob/3163ec065643ba20709f190111f42c3e1fe95273/COPYING)
w stylu MIT. Nowsze libcurl trafia do programu dopiero po ponownym linkowaniu.

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

Źródła upstream zawierają też funkcje alokujące pamięć. Pozostają niezmienione
i poza publicznym API jhbase. Usuwanie nieużywanych sekcji eliminuje je ze
zlinkowanych programów. `jb::base` przekazuje tę zasadę linkowania; kompilacja
bezpośrednia musi używać sekcji funkcji/danych i ich GC lub odpowiedników
MSVC `/Gy /Gw` oraz `/OPT:REF`. Asercje upstream są wyłączone dla tej wewnętrznej
konwersji o ograniczonych danych; instrumentowane testy wykonują właściwe
funkcje buforowe.

Kontrola freestanding szuka symboli alokacji w odwołaniach własnych obiektów
i kompletnym zlinkowanym programie kontrolnym, w tym Ryu i libc.
Hostowy test przerywa wykonanie przy malloc/calloc/realloc/free podczas operacji
JSON, liczb, writera, literałów i opcjonalnego NFC z `tests/library_exercise.h`.
Nie wykonuje operacji kontenerów będących właścicielami pamięci: te korzystają
z jawnie przekazanego alokatora i mają osobne kontrole własności oraz błędów alokacji.
Program kontrolny ma mapę pamięci tylko do analizy; nie jest firmware
do wgrania na urządzenie. Pomiary na płytkach korzystają z firmware
w `tests/device`, opisanego w [testach](testing.md#pomiary-na-urządzeniach).

## Porównanie i narzędzia jakości

coreJSON i yyjson to przypięte źródła badawcze opisane przy
[wyborze parsera](parser-choice.md); żaden nie należy do przenośnej biblioteki.
Unity jest zależnością testów hostowych. Valgrind, sanitizery, cppcheck i PMD CPD
są narzędziami kontroli jakości, bez udziału w runtime. Wymagania narzędzi
i osobne kontrole Windows opisują [testy](testing.md) oraz
[przygotowanie Windows](windows.md).

Unity, cppcheck, PMD, toolchainy RISC-V i Xtensa oraz najstarszy obsługiwany
CMake mają piny w plikach
`*_version.conf` w [third_party](../../third_party), a
[ensure_tools.sh](../../scripts/ensure_tools.sh) pobiera je, weryfikuje
i buduje w `.build/tools`. Dla Unity, cppcheck, PMD, toolchainu RISC-V
i najstarszego obsługiwanego CMake te
pliki są jedynymi pinami: JaszczurHAL czyta je ze swojego przypiętego
checkoutu jhbase zamiast trzymać kopie, więc obie bramki używają jednej
rewizji każdego narzędzia. Zawierają tylko rewizje i skróty; katalogi
instalacji wybiera każdy projekt osobno. Toolchain Xtensa służy profilowi
ESP32-S3. JaszczurHAL dostaje swój kompilator Xtensa razem z ESP-IDF
i sprawdza, czy ten pin zgadza się z manifestem narzędzi ESP-IDF, bo archiwum
z pomiarów na urządzeniu linkuje się z firmware ESP-IDF.

[Pin ccache](../../third_party/ccache_version.conf) dostarcza cache kompilatora
dla macierzy buildów projektów korzystających z jhbase. Zapisuje oficjalne
archiwa statyczne dla Linuksa x86_64 i ARM64 oraz archiwum Windows x64,
wraz z sumami SHA-256. Projekty pobierają je i sprawdzają we własnych katalogach
narzędzi. Bramka jhbase nie wymaga ccache. Upstream tego wydania nie dostarcza
binarki dla Linuksa ARM32.
