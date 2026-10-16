# Podstawowy odbiór prototypu

`./runalltests.sh` uruchamia poniższe etapy na Linuksie. Błąd polecenia
zatrzymuje zestaw. `--stage NAME` uruchamia ten sam etap osobno. Buildy
i generowane corpusy trafiają do `.build/gate`; etap `clean` zastępuje
wyłącznie własną kopię źródeł.

| Etap | Kontrole |
| --- | --- |
| `host` | GCC Release/LTO, testy Unity UTF-8/JSON, użycie API z C++17 i odrzucanie błędnego ABI. |
| `sanitizers` | Clang ASan, UBSan i LeakSanitizer obejmujące bibliotekę i programy testowe. |
| `memcheck` | Valgrind na właściwych executable testów UTF-8, JSON i C++. |
| `fuzz` | Ograniczony przebieg libFuzzer z ustalonym seedem: parser, pomiar, odczyt i UTF-8. |
| `freestanding` | Kompilacja kodu przenośnego dla Cortex-M0+, M33 i M4; raport stosu i odrzucanie symboli heap. |
| `clean` | Pierwszy configure/build/test w kopii źródeł ze spacjami, bez stanu buildu; potem build przyrostowy. |
| `analysis` | Przypięty build cppcheck z HAL sprawdza przenośne moduły C. |
| `quality` | Składnia Bash, clang-format i kontrola whitespace Gita. |

Potrzebne są CMake, Ninja, GCC/G++, Clang/Clang++ 18 z libFuzzer,
Valgrind, arm-none-eabi GCC/binutils, przypięty build cppcheck z HAL,
clang-format, ripgrep, tar i Git.
Żadne narzędzie nie instaluje się automatycznie. Testy czytają tylko przypięte
nagłówki statusów/tablic HAL i pliki Unity; nie czytają firmware'u testów
sprzętowych. LeakSanitizer wymaga środowiska bez ograniczeń ptrace.
CTest zapisuje JUnit XML w każdym katalogu buildu hostowego.

Testy zawierają niezależne wektory bajtów i składni, zdekodowane duplikaty
nazw, dokładne tokeny liczb, granice pojemności, niezmienność wyjść, głębokie
zagnieżdżenia i ponowne użycie scratch. Nie zależą od Pythona ani SDK.
Fuzzing sprawdza też zakresy tokenów, relacje rodziców/rodzeństwa i zdekodowane
UTF-8. Ograniczony przebieg jest smoke testem, nie dowodem pełnego pokrycia.

Natywne MSVC wymaga wskazania windowsowych kopii tych samych zależności:

```powershell
cmake -S . -B .build/msvc -G "Visual Studio 17 2022" -A x64 -DJHBASE_HAL_ROOT=C:/src/JaszczurHAL
cmake --build .build/msvc --config Release
ctest --test-dir .build/msvc -C Release --output-on-failure
```

Polecenia uruchamiają te same przypadki CTest, także kontrolę naturalnego
układu i pakowania. Próba z flagą krótkich enumów dotyczy GCC/Clang.
Kompilacja ARM nie potwierdza działania na urządzeniu, a przebieg Linuksa
nie potwierdza MSVC ani Win32. Pełny etap 1a nadal wymaga natywnego MSVC,
szerszego porównania parserów, konwersji liczb, writera i pomiarów targetów.
Prototyp nie zmienia jobów CI ani produkcyjnych workflowów HAL.
