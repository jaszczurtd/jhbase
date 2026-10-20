# Testowanie biblioteki

`./runalltests.sh` uruchamia poniższe etapy na Linuksie. Błąd polecenia
zatrzymuje zestaw. `--stage NAME` uruchamia ten sam etap osobno. Buildy
i generowane corpusy trafiają do `.build/gate`; etap `clean` zastępuje
wyłącznie własną kopię źródeł.

| Etap | Kontrole |
| --- | --- |
| `host` | GCC Release/LTO, testy UTF-8/JSON/liczb/writera/literałów, C++17, niezależna kontrola najkrótszego zapisu, kompilowane literały i przykłady C/C++, odrzucanie błędnego ABI, konsumenci C i C++ zainstalowanego pakietu. |
| `sanitizers` | Clang ASan, UBSan i LeakSanitizer obejmujące bibliotekę i programy testowe. |
| `memcheck` | Valgrind na właściwych executable testów, probe literałów i przykładów. |
| `fuzz` | Osobne przebiegi libFuzzer z ustalonym seedem dla parsera/UTF-8, dokładnych liczb/literałów i operacji writera; każdy ma ograniczoną liczbę prób. |
| `freestanding` | Kompilacja, archiwum i linkowanie probe analitycznego dla profili z `scripts/freestanding.sh`: Cortex-M0+ (RP2040), M33 (RP2350 ARM, softfp), M4 (STM32G474, hard-float), RP2350 RISC-V i ESP32-S3 (Xtensa), każdy z flagami maszynowymi firmware swojej platformy; rozmiary, raport stosu i odrzucanie symboli heap we własnych obiektach i kompletnym probe. |
| `comparison` | Przypięte porównanie coreJSON/yyjson z instrumentacją źródeł upstream i właściwego parsera. |
| `clean` | Pierwszy configure/build/test w kopii źródeł ze spacjami, bez stanu buildu, potem build przyrostowy; wszystko najstarszym obsługiwanym CMake (`third_party/cmake_minimum_version.conf`). Każde `cmake_minimum_required` musi deklarować tę wersję. |
| `analysis` | Build cppcheck przypięty w `third_party/cppcheck_version.conf` sprawdza przenośne moduły C. |
| `quality` | Składnia Bash, clang-format, whitespace źródeł, PMD CPD dla własnego kodu produkcyjnego, testów i przykładów oraz hooki Git sprawdzane w tymczasowym repozytorium (`tests/check_hooks.sh`). |

Potrzebne narzędzia systemowe to CMake, Ninja, GCC/G++, Clang/Clang++ 18
z libFuzzer, Valgrind, arm-none-eabi GCC/binutils, Java, clang-format, ripgrep,
curl, unzip, tar i Git. Unity, cppcheck, PMD, toolchainy RISC-V i Xtensa
oraz najstarszy obsługiwany CMake są przypięte w `third_party/*_version.conf`; `./scripts/ensure_tools.sh` instaluje je
w `.build/tools`, sprawdzając rewizje i sumy SHA-256. Bramka tylko sprawdza
ich obecność; `--tools-root` albo `JHBASE_TOOLS_ROOT` wskazuje inne miejsce.
Testy czytają istniejące oczekiwania liczbowe z HAL
`tests/fixtures/device_tree/cases.json`. Nie czytają firmware'u testów
sprzętowych. LeakSanitizer wymaga środowiska bez ograniczeń ptrace.
CTest zapisuje JUnit XML w każdym katalogu buildu hostowego. Zestaw
`vscode_workspace` (etykieta `tooling`) sprawdza dokumenty zadań VS Code
i uruchamia sterownik workspace na tymczasowej kopii drzewa; zobacz
[VS Code](vscode.md). `-DJHBASE_VSCODE_CHECK=OFF` pomija go w buildzie.

Testy zawierają niezależne wektory bajtów i składni, zdekodowane duplikaty
nazw, dokładne tokeny liczb, granice pojemności, niezmienność wyjść, głębokie
zagnieżdżenia i ponowne użycie scratch. Nie zależą od Pythona ani SDK.
Fuzzing sprawdza też zakresy tokenów, relacje rodziców/rodzeństwa i zdekodowane
UTF-8. Ograniczony przebieg jest smoke testem, nie dowodem pełnego pokrycia.

Przygotuj raz źródła porównania, a potem uruchom pełną bramkę Linuksa:

```sh
./scripts/ensure_tools.sh
./scripts/prepare_comparison.sh
./runalltests.sh -j4
./runalltests.sh --list-stages
./runalltests.sh --stage host -j4
```

Oba skrypty przygotowania pobierają wyłącznie przypiętą, zweryfikowaną
zawartość. Narzędzia i źródła porównania trafiają do `.build`, a utf8proc
i dane zgodności Unicode do swoich ignorowanych katalogów źródeł i fixture.
Sama bramka niczego nie pobiera; brak lub zmiana zależności kończą się
poleceniem naprawy. Zobacz [zależności](dependencies.md)
i [wybór parsera](parser-choice.md).
Etap `comparison` pozwala wskazać cache przez `JHBASE_COMPARISON_ROOT`.

`--commit [REF]` uruchamia runner wskazanego commita z czystego worktree na
odłączonym HEAD, a następnie usuwa to worktree. Domyślnie sprawdza HEAD,
a fixture liczb czyta z jawnie wskazanego checkoutu HAL. Lokalne
niezacommitowane zmiany są pomijane, także zmiany samego runnera:

```sh
./runalltests.sh --commit HEAD --hal-root /path/to/matching/HAL -j4
```

Każdy fuzzer używa obecnie `-runs=100000 -seed=8259 -max_len=4096 -timeout=3`.
Corpora i artefakty awarii mają osobne katalogi dla każdego executable.
Sanitizery i kontrola wycieków pozostają włączone. CPD odrzuca bloki duplikacji
od 100 tokenów w obu własnych zakresach; kod porównawczy upstream jest poza
tymi zakresami. Kod testów urządzeń pozostaje poza domyślnymi kontrolami.

Probe literałów korzysta z niezależnych oczekiwanych bitów istniejącej fixture
HAL, a następnie kompiluje i uruchamia rzeczywiste wygenerowane literały jako
C11 i C++17. Odczyt liczb sprawdzają też ustalone wektory IEEE i bezpośrednie
odczyty natywne; najkrótszy zapis jest porównywany z C++17 `to_chars`/`from_chars`.
Kompletne przykłady są wyodrębniane z każdej wersji Markdown API i sprawdzane
jako C11 oraz C++17. Zmiana kodu uruchamia ponowne generowanie; obie wersje
językowe muszą mieć identyczne przykłady. Samodzielne przykłady liczb i writera
również działają w obu językach programowania.

Dla natywnego MSVC x64 i Win32 użyj [checklisty Windows](windows.md)
i `runalltests.ps1`. Runner uruchamia te same zestawy CTest, także sprawdzenie
układu i pakowania. Próby z flagą krótkich enumów dotyczą tylko GCC/Clang:
publiczny status musi zachować 32 bity z `-fshort-enums`, bez selektora.
Test przechwytujący alokacje przez linker GNU jest właściwy dla Linuksa.

Cross kompilacja i statyczne raporty `.su` nie potwierdzają czasu na urządzeniu,
maksymalnego stosu runtime ani rozmiaru zlinkowanego firmware; to dają
[pomiary na urządzeniach](#pomiary-na-urządzeniach). Wynik Linuksa nie
potwierdza odbioru MSVC/Win32. Prototyp nie dodaje jobów CI ani produkcyjnych
workflowów generatorów.

Bramka hosta sprawdza build z Unicode wyłączonym i włączonym. Sanitizery,
Valgrind, fuzzer JSON/UTF-8 i czysty build obejmują adapter NFC oraz właściwe
źródło utf8proc. Zestaw UTF-8 sprawdza niezależny przypięty
`NormalizationTest.txt` i regresję pomiaru z buforem NULL. Freestanding
linkuje każdy profil z NFC i bez niego, w obu szukając symboli alokacji.
Windows również sprawdza oba ustawienia modułu dla każdej architektury.

## Pomiary na urządzeniach

`scripts/measure_device.sh` mierzy bibliotekę na płytce. To osobny, jawnie
uruchamiany przebieg: bramka nigdy nie buduje, nie wgrywa ani nie czyta kodu
urządzeń, a domyślne kontrole pomijają `tests/device`.

Skrypt buduje archiwum biblioteki w profilu freestanding danej platformy,
linkuje je z firmware JaszczurHAL z `tests/device` i wgrywa na płytkę.
Firmware parsuje i zapisuje dokumenty z `tests/device/inputs`, czyta zawarte
w nich liczby i wypisuje raport. Skrypt dopisuje do niego, ile bajtów zajmuje
biblioteka w zlinkowanym obrazie, na podstawie mapy linkera.

| Platforma | Montaże | Profil | Wgrywanie i port raportu |
| --- | --- | --- | --- |
| `rp2040` | `pico`, `picow` | `cortex-m0plus` | picotool z `.build/tools` HAL; port USB płytki |
| `rp2350-arm` | `pico2w` | `cortex-m33` | picotool z `.build/tools` HAL; port USB płytki |
| `rp2350-riscv` | `pico2-riscv` | `rp2350-riscv` | picotool z `.build/tools` HAL; port USB płytki |
| `stm32g474` | `nucleo`, `nucleo-canhat` | `cortex-m4` | OpenOCD przez ST-LINK; port szeregowy ST-LINK |
| `esp32s3` | `esp32s3-zero` | `esp32s3` | ESP-IDF przygotowany przez HAL; port USB Serial/JTAG |

```sh
./scripts/measure_device.sh --target rp2040 --assembly picow --serial <serial>
./scripts/measure_device.sh --target rp2350-arm --assembly pico2w --serial <serial>
./scripts/measure_device.sh --target rp2350-riscv --assembly pico2-riscv --serial <serial>
./scripts/measure_device.sh --target stm32g474 --assembly nucleo-canhat
./scripts/measure_device.sh --target esp32s3 --assembly esp32s3-zero
```

Skrypt potrzebuje checkoutu JaszczurHAL z zainstalowanymi narzędziami
(`--hal-root`, domyślnie `../JaszczurHAL`). Płytki RP wymagają też
`--serial`, czyli numeru wypisanego przez `picotool info -a` (`flash id` na
RP2040, `chipid` na RP2350): picotool wgrywa firmware tylko na tę płytkę,
a skrypt po tym numerze znajduje port raportu.
Płytka może czekać w trybie BOOTSEL albo działać z firmware JaszczurHAL, który
przechodzi w BOOTSEL, gdy skrypt zamknie jego port USB przy 1200 b/s.
`--port` zastępuje wykryty port szeregowy, a `--no-flash` tylko buduje
i wypisuje rozmiar. Buildy i `report.txt` zostają
w `tests/device/.build/<montaż>`. Dla ESP32-S3 skrypt odrzuca build ESP-IDF,
który nie używa toolchainu Xtensa przypiętego
w `third_party/xtensa_toolchain_version.conf`.

Archiwum i firmware muszą mieć to samo ABI. W buildach Pico SDK i STM32 skrypt
porównuje flagi maszynowe (opcje `-m`) pliku `app.c` z profilem i przerywa
pracę, gdy się różnią. Firmware RP2350 RISC-V kompiluje ten sam przypięty GCC
z `.build/tools`, który kompiluje archiwum.

Montaż `pico2-riscv` opisuje Pico 2 z diodą na GPIO25. Działa też na Pico 2 W,
ale dioda wtedy nie świeci, bo na tej płytce jest podłączona do CYW43, którego
HAL nie obsługuje w buildach RISC-V. O wyniku informuje wtedy tylko raport na
porcie szeregowym.

Każda linia `JBM case=` opisuje jeden przypadek:

- `ns` to czas jednego wywołania: najszybsza z pięciu serii trwających co
  najmniej 40 ms, przy włączonych przerwaniach.
- `stack` liczy bajty zapisane poniżej ramki pomiarowej, przy zamaskowanych
  przerwaniach. Wartości poniżej 16 B są nierozróżnialne (`baseline_stack` to
  puste wywołanie).
- `scratch_used` to najwyższy bajt scratch zapisany przez przypadek,
  a `scratch_required` to wartość z funkcji `*_workspace_size`.

`JBM size` podaje kod i stałe biblioteki (`flash`) oraz jej dane statyczne
(`ram`) w zlinkowanym obrazie. `JBM end` podsumowuje osiem niezależnych
sprawdzeń: poprawne zaokrąglenie wektorów binary32 i binary64, dokładne
porównanie liczb dziesiętnych, pełne zakresy uint64 i int64, zgodność measure
z parse, walidację UTF-8 oraz odczyt zapisanych liczb do tych samych bitów.
`result=PASS` wymaga wszystkich. Dioda miga podczas pomiaru, a potem raz na
sekundę przy PASS albo pięć razy na sekundę przy FAIL; dioda adresowalna
świeci na niebiesko, a potem na zielono albo czerwono.
