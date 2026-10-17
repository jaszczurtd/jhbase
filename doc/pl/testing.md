# Testowanie biblioteki

`./runalltests.sh` uruchamia poniższe etapy na Linuksie. Błąd polecenia
zatrzymuje zestaw. `--stage NAME` uruchamia ten sam etap osobno. Buildy
i generowane corpusy trafiają do `.build/gate`; etap `clean` zastępuje
wyłącznie własną kopię źródeł.

| Etap | Kontrole |
| --- | --- |
| `host` | GCC Release/LTO, testy UTF-8/JSON/liczb/writera/literałów, C++17, niezależna kontrola najkrótszego zapisu, kompilowane literały i przykłady C/C++, odrzucanie błędnego ABI. |
| `sanitizers` | Clang ASan, UBSan i LeakSanitizer obejmujące bibliotekę i programy testowe. |
| `memcheck` | Valgrind na właściwych executable testów, probe literałów i przykładów. |
| `fuzz` | Osobne przebiegi libFuzzer z ustalonym seedem dla parsera/UTF-8, dokładnych liczb/literałów i operacji writera; każdy ma ograniczoną liczbę prób. |
| `freestanding` | Kompilacja/linkowanie probe analitycznego dla Cortex-M0+, M33, M4 i RP2350 RISC-V; rozmiary, raport stosu i odrzucanie symboli heap we własnych obiektach i kompletnym probe. |
| `comparison` | Przypięte porównanie coreJSON/yyjson z instrumentacją źródeł upstream i właściwego parsera. |
| `clean` | Pierwszy configure/build/test w kopii źródeł ze spacjami, bez stanu buildu; potem build przyrostowy. |
| `analysis` | Build cppcheck przypięty w `third_party/cppcheck_version.conf` sprawdza przenośne moduły C. |
| `quality` | Składnia Bash, clang-format, whitespace źródeł, PMD CPD dla własnego kodu produkcyjnego, testów i przykładów oraz hooki Git sprawdzane w tymczasowym repozytorium (`tests/check_hooks.sh`). |

Potrzebne narzędzia systemowe to CMake, Ninja, GCC/G++, Clang/Clang++ 18
z libFuzzer, Valgrind, arm-none-eabi GCC/binutils, Java, clang-format, ripgrep,
curl, unzip, tar i Git. Unity, cppcheck, PMD i toolchain RISC-V są przypięte
w `third_party/*_version.conf`; `./scripts/ensure_tools.sh` instaluje je
w `.build/tools`, sprawdzając rewizje i sumy SHA-256. Bramka tylko sprawdza
ich obecność; `--tools-root` albo `JHBASE_TOOLS_ROOT` wskazuje inne miejsce.
Testy czytają sprawdzone nagłówki statusów/tablic HAL i istniejące oczekiwania
liczbowe z `tests/fixtures/device_tree/cases.json`. Nie czytają firmware'u testów
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
zawartość do `.build`. Sama bramka niczego nie pobiera, a gdy brakuje
któregoś narzędzia w wymaganej wersji, kończy się poleceniem naprawy.
Zobacz [wybór parsera](parser-choice.md).
Etap `comparison` pozwala wskazać cache przez `JHBASE_COMPARISON_ROOT`.

`--commit [REF]` uruchamia runner wskazanego commita z czystego worktree na
odłączonym HEAD, a następnie usuwa to worktree. Domyślnie sprawdza HEAD
z jawnie wybranymi zależnościami HAL. Ich hashe muszą pasować do danej rewizji.
Lokalne niezacommitowane zmiany są pomijane, także zmiany samego runnera:

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
układu i pakowania. Próby z flagą krótkich enumów dotyczą tylko GCC/Clang: odrzucają krótki
enum statusów bez selektora i akceptują jawny selektor stałej szerokości.
Test przechwytujący alokacje przez linker GNU jest właściwy dla Linuksa.

Cross kompilacja i statyczne raporty `.su` nie potwierdzają czasu na urządzeniu,
maksymalnego stosu runtime ani rozmiaru zlinkowanego firmware. Pomiary sprzętowe
wymagają osobnego jawnego przebiegu. Wynik Linuksa nie potwierdza odbioru
MSVC/Win32. Prototyp nie dodaje jobów CI ani produkcyjnych workflowów generatorów.
