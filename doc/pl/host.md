# Pliki hosta

API `jb/file.h` odczytuje pliki, rozwiązuje ścieżki UTF-8 i zapisuje całe pliki
wynikowe na Linuksie oraz Windows. Włącz `JHBASE_HOST=ON` i linkuj `jb::host`.
Implementacja wymaga kompilatora i runtime C++17; funkcje publiczne można
wywoływać z C11. Alokacje hosta i nagłówki systemowe pozostają poza `jb::base`.
Opcja jest domyślnie wyłączona i wymaga targetu Linux lub Windows.

```sh
cmake -S . -B .build/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DJHBASE_HOST=ON
cmake --build .build/host
ctest --test-dir .build/host --output-on-failure
```

Instalację wykonuje `cmake --install`. Pakiet eksportuje `jb::host`, gdy był
budowany z tą opcją. Konsument w C potrzebuje także linkera i runtime C++.
Pakiet zawierający wyłącznie statusy nie dołącza kodu hosta.

## Odczyt i ścieżki

`jb_file_read(path, max_bytes, out)` czyta zwykły plik bez zmiany bajtów,
także NUL i CRLF. Wyzeruj `jb_file_data_t` przed pierwszym odczytem i zwolnij
go przez `jb_file_release` przed ponownym użyciem. Błąd zachowuje pusty wynik;
zajęty obiekt daje `HAL_ESTATE`. Limit rozmiaru jest sprawdzany przed alokacją
oraz podczas odczytu, również gdy plik rośnie. Odczyt podąża za istniejącymi
linkami; plik innego rodzaju daje `HAL_ETYPE`. Można wywołać `jb_file_release(NULL)`.

`jb_path_resolve(base, path, buffer, capacity, required)` wymaga jawnej,
absolutnej ścieżki bazowej. Łączy i porządkuje składniki ścieżki bez odczytu
linków i zmiany cwd. Ścieżki podlegają regułom systemu hosta i używają UTF-8;
Windows otrzymuje je w postaci wide. Ścieżki względne wobec dysku Windows
są odrzucane. `(NULL,0)` mierzy wynik WRAZ z terminatorem NUL. Za mały bufor
daje `HAL_EOVERFLOW`, pełny wymagany rozmiar i zachowuje wszystkie bajty bufora.

## Porównanie i zapis

`jb_file_write_atomic(path, data, length, check_only, changed)` najpierw
porównuje całe bajty. Identyczny plik zachowuje mtime. Tryb sprawdzania ustawia
`changed`, gdy wyniku brakuje lub ma inną treść; nie tworzy katalogów ani
plików tymczasowych. Wywołujący decyduje, czy różnica oznacza nieudany check.

Zapis tworzy brakujące katalogi nadrzędne i osobny plik tymczasowy obok celu.
Przed podmianą zapisuje wszystkie bajty, wykonuje flush i zamyka plik.
POSIX wykonuje też flush katalogu nadrzędnego; Windows używa `FlushFileBuffers`
i `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`. Błąd usuwa plik tymczasowy.
Nowy plik POSIX ma prywatne uprawnienia; podmiana zwykłego pliku zachowuje
jego bity rwx. Końcowy symlink, reparse point lub cel innego rodzaju jest
odrzucany. Katalogi nadrzędne mogą prowadzić przez istniejące linki, więc API
nie ogranicza niezaufanych ścieżek do jednego katalogu.

Wywołania nie blokują zewnętrznych edytorów. Przy równoległych zapisach
obowiązuje ostatnia udana podmiana. Błędy zachowują `changed`, poza błędem
flush katalogu POSIX już po podmianie: `changed=true` oznacza wtedy, że nowe
bajty są widoczne, lecz nie potwierdzono trwałości zapisu. Błąd nie usuwa nowo
utworzonych katalogów nadrzędnych. Nadal obowiązują ograniczenia systemu plików
i współdzielenia plików.

Funkcje zwracają wspólny `hal_status_t`. Błędny UTF-8 daje `HAL_EPROTO`, brak
pamięci `HAL_ENOMEM`, przekroczony limit `HAL_EOVERFLOW`, brak wejścia
`HAL_ENOENT`, a odmowa dostępu `HAL_EPERM`. Pozostałe błędy I/O dają `HAL_EIO`
lub `HAL_EBUSY`. Zajęty cel podmiany na Windows może też dać `HAL_EPERM`,
gdy system zgłasza odmowę dostępu. Każda granica API C przechwytuje wyjątki C++.
