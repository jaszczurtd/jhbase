# Pliki, procesy i konfiguracja JSON na hoście

API `jb/file.h` odczytuje pliki, wyznacza ścieżki UTF-8 i zapisuje całe pliki
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
budowany z tą opcją. Aplikacja w C potrzebuje także linkera i runtime C++.
Pakiet zawierający wyłącznie statusy nie dołącza kodu hosta.

## Odczyt i ścieżki

`jb_file_read(path, max_bytes, out)` czyta zwykły plik bez zmiany bajtów,
także NUL i CRLF. Wyzeruj `jb_file_data_t` przed pierwszym odczytem i zwolnij
go przez `jb_file_release` przed ponownym użyciem. Błąd zachowuje pusty wynik;
zajęty obiekt daje `HAL_ESTATE`. Limit rozmiaru jest sprawdzany przed alokacją
oraz podczas odczytu, również gdy plik rośnie. Odczyt podąża za istniejącymi
linkami; plik innego rodzaju daje `HAL_ETYPE`. Można wywołać `jb_file_release(NULL)`.

`jb_path_resolve(base, path, buffer, capacity, required)` wymaga jawnej,
bezwzględnej ścieżki bazowej. Łączy i porządkuje składniki ścieżki bez odczytu
linków i zmiany katalogu roboczego procesu. Ścieżki podlegają regułom systemu
hosta i używają UTF-8; na Windows są zamieniane na UTF-16 dla API systemowego.
Ścieżki względne wobec dysku Windows są odrzucane. `(NULL,0)` mierzy wynik
wraz z terminatorem NUL. Za mały bufor daje `HAL_EOVERFLOW`; funkcja podaje
pełny wymagany rozmiar i zachowuje wszystkie bajty bufora.

## Porównanie i zapis

`jb_file_write_atomic(path, data, length, check_only, changed)` najpierw
porównuje całą zawartość bajt po bajcie. Identyczny plik zachowuje mtime. Tryb sprawdzania ustawia
`changed`, gdy wyniku brakuje lub ma inną treść; nie tworzy katalogów ani
plików tymczasowych. Wywołujący decyduje, czy różnica oznacza błąd kontroli.

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

Funkcje, które mogą zakończyć się błędem, zwracają wspólny `hal_status_t`.
Błędny UTF-8 daje `HAL_EPROTO`, brak
pamięci `HAL_ENOMEM`, przekroczony limit `HAL_EOVERFLOW`, brak wejścia
`HAL_ENOENT`, a odmowa dostępu `HAL_EPERM`. Pozostałe błędy I/O dają `HAL_EIO`
lub `HAL_EBUSY`. Zajęty cel podmiany na Windows może też dać `HAL_EPERM`,
gdy system zgłasza odmowę dostępu. Każda granica API C przechwytuje wyjątki C++.

## Procesy potomne

`jb/process.h` udostępnia synchroniczne `jb_process_run(options, result)` przez
`jb::host`. Podaj bezwzględną ścieżkę programu i katalogu roboczego. Argumenty
to lista zakończona NULL, bez `argv[0]`; NULL oznacza brak argumentów.
API nie uruchamia powłoki ani nie szuka w PATH. Windows konwertuje UTF-8 na
UTF-16 i cytuje argumenty według reguł runtime Microsoft C. Program z własnym
parserem linii poleceń może interpretować je inaczej.

Wskaźnik środowiska równy NULL dziedziczy środowisko rodzica. Lista inna niż
NULL zastępuje je w całości; `{NULL}` daje puste środowisko. Wpisy mają postać
`NAME=VALUE`, a nazwy muszą być unikalne, bez rozróżniania wielkości liter na
Windows. Wywołanie nie zmienia cwd ani środowiska rodzica. Nie zmieniaj środowiska
rodzica równolegle z uruchamianiem procesu. Stdin potomka odczytuje EOF.

Oba potoki wyjściowe są opróżniane naprzemiennie z ograniczonymi buforami.
Opcjonalny callback otrzymuje porcje binarnych danych i identyfikator strumienia
w wątku wywołującym. Bajty zachowują kolejność w obrębie strumienia; API nie
ustala kolejności między stdout i stderr. NULL odrzuca dane, nadal opróżniając
potoki. Status inny niż `HAL_OK` kończy wywołanie z tym statusem. Callback
korzysta z pożyczonego bufora, musi szybko wracać i nie może rzucać wyjątków.

`timeout_ms` mierzy czas monotoniczny od chwili przed uruchomieniem; zero oznacza
oczekiwanie bez limitu. Opcjonalny callback anulowania jest sprawdzany przed
uruchomieniem i podczas oczekiwania. True daje `HAL_ECANCELED`. Inny wątek może
żądać anulowania przez atomowy stan należący do wywołującego. Uruchamianie,
sprzątanie systemowe i wykonanie callbacków nie mają gwarancji hard real-time.
Niezależne wywołania mogą działać równolegle; konteksty i synchronizacja
callbacków należą do wywołującego.

Wynik jest inicjalizowany także przy błędzie. `HAL_OK` oznacza zebranie procesu
i opróżnienie wyjścia, niezależnie od kodu zakończenia. Sprawdź `exit_code`
oraz `term_signal` na Linuksie. Windows zapisuje w `exit_code` także wymuszone
zakończenie przez OS; przy zatrzymanym wywołaniu nie jest to wynik aplikacji. `started` może być true przy błędzie exec na
Linuksie; pozostaje on błędem API z `native_error`, zamiast sukcesem z kodem
127. Reporter po stronie forka kończy się kodem 127 przed sygnałami cleanup,
zachowując błąd systemowy i zebrane pola zakończenia. Niepoprawne wejście
i błędy callbacków nie mają kodu błędu systemowego.
Timeout lub anulowanie rozpoznane przed zaobserwowaniem wyjścia pozostaje
statusem API, nawet gdy dziecko zakończy się naturalnie podczas sprzątania.
`HAL_ETIMEOUT` z `exited=true` i linuksowym `term_signal=0` jest więc poprawnym
wynikiem: pola zakończenia opisują zebrane dziecko, a nie dotrzymanie budżetu.
Błąd sprzątania grupy również zachowuje wynik zebranego procesu głównego.

Każdy potomek ma własną grupę procesów na Linuksie lub Windows Job Object.
Timeout, anulowanie, błąd callbacka lub I/O wymuszają zakończenie i zebranie
procesu głównego. Pozostali potomkowie są kończeni również po normalnym wyjściu
procesu głównego, więc odziedziczone potoki nie blokują końca wywołania.
Po zaobserwowaniu zakończenia API utrwala liczbę bajtów w kolejce i opróżnia
ją przed oceną timeoutu lub anulowania. Późniejsze zapisy innych procesów są
pomijane. Drenowanie może skończyć się po deadline; błąd callbacka wyjścia nadal
zatrzymuje wywołanie natychmiast. Dane pozostałe po zatrzymaniu mogą zostać odrzucone. API nie służy zatem do
uruchamiania trwałych usług w tle.

Potomek Linuksa, który celowo zmienia grupę lub sesję, opuszcza nadzór; API
nie zapewnia sandboxingu. Windows przypisuje wstrzymany proces do joba przed
rozpoczęciem wykonania i nie pozwala na breakaway. Pozostałe odziedziczone
uchwyty i deskryptory są wykluczane. Dalszych potomków zbiera ich rodzic
systemowy. Na Linuksie nie ignoruj SIGCHLD, nie ustawiaj `SA_NOCLDWAIT` ani
nie zbieraj tego procesu innym mechanizmem.

## Strumieniowy zapis atomowy

`jb_file_writer_begin(path, max_bytes, out)` tworzy tymczasowego sąsiada pliku
wskazanego bezwzględną ścieżką i tworzy brakujące katalogi nadrzędne. Wyzeruj
wskaźnik właściciela. `jb_file_writer_write` dopisuje dokładne bajty przy
ograniczonym zużyciu pamięci; limit obejmuje ich łączną liczbę, a zero pozwala
tylko na pusty plik. Błąd dopisania blokuje późniejszą publikację niepełnego wyniku.

`jb_file_writer_commit` opróżnia plik, porównuje oba pliki porcjami i zachowuje
mtime celu przy identycznej zawartości. W przeciwnym razie korzysta z tych samych
helperów publikacji i trwałości co `jb_file_write_atomic`, także do zachowania
uprawnień POSIX. Bity rwx istniejącego celu są odczytywane i stosowane przy
`begin`; późniejsza zmiana uprawnień celu nie jest kopiowana przy commit.
`jb_file_writer_abort` zamyka i usuwa plik tymczasowy bez publikacji. Obie operacje zwalniają i zerują poprawnego właściciela także przy
błędzie OS; niepoprawne argumenty commit pozostawiają właściciela. Błąd sprzątania
może pozostawić plik tymczasowy na dysku. Może go pozostawić również śmierć procesu.

Commit ustawia `changed` według reguł API jednorazowego, także po publikacji,
po której zawiedzie fsync katalogu POSIX. Oba API odrzucają końcowe dowiązania,
reparse points i cele inne niż zwykły plik, podążają za linkami rodziców oraz nie
blokują innych piszących. Uchwyty mają jednego właściciela i wymagają serializacji
wywołań. API nie ograniczają wrogiej aktywności w systemie plików.

## Blokady plików

`jb_file_lock_acquire(path, timeout_ms, canceled, context, out)` otwiera trwały
zwykły plik bez obcinania zawartości i zakłada wyłączną blokadę OS. Ścieżka UTF-8
musi być bezwzględna, a katalog nadrzędny musi istnieć. Końcowe dowiązania i
reparse points są odrzucane. Wyzeruj właściciela; zajęty wynik daje `HAL_ESTATE`.
Linux używa `flock`, Windows blokuje pierwszy bajt przez `LockFileEx`.
Niezależne próby konkurują także wewnątrz jednego procesu. Nowy plik blokady
POSIX żąda trybu 0600 ograniczonego przez umask; istniejący zachowuje swój tryb.

Timeout zero oznacza jedną próbę i `HAL_EBUSY` przy zajęciu; `UINT32_MAX`
czeka bez końca. Inne wartości mierzą czas monotoniczny i dają `HAL_ETIMEOUT`.
Pierwsza próba blokady poprzedza sprawdzenie upływu czasu; anulowanie nadal
ma pierwszeństwo przed próbą. Kolejne próby uwzględniają termin.
Opcjonalny callback anulowania działa w wątku czekającym przed otwarciem oraz
podczas oczekiwania; true daje `HAL_ECANCELED`. Musi szybko wracać i nie może
rzucać wyjątków. Kontekst może być NULL; żądanie z innego wątku wymaga atomowego stanu.

`jb_file_lock_release` zwalnia i zeruje właściciela także przy błędzie zamknięcia.
NULL wskaźnika lub właściciela jest dozwolone. Zakończenie procesu również zwalnia
blokadę OS. Uchwyty nie są dziedziczone przez exec/CreateProcess; surowy fork POSIX
może je zachować. Nie usuwaj ani nie zastępuj pliku blokady, również po zwolnieniu:
czekający mogą nadal korzystać z jego starej tożsamości. Blokady wymagają współpracy
wywołujących i zachowują semantykę używanego systemu plików.

## Własne katalogi tymczasowe

`jb_temp_dir_create(parent, out)` tworzy unikalny katalog pod istniejącym rodzicem
wskazanym bezwzględną ścieżką UTF-8. Wyzeruj właściciela. Tryb POSIX wynosi 0700;
Windows używa chronionej DACL przyznającej dziedziczony pełny dostęp wyłącznie
bieżącemu użytkownikowi. Uprzywilejowany dostęp systemowy pozostaje możliwy.

`jb_temp_dir_path` pożycza bezwzględną ścieżkę UTF-8 do usunięcia; NULL daje NULL.
Przed `jb_temp_dir_remove` zatrzymaj użytkowników katalogu. Funkcja usuwa zawartość
i katalog rekurencyjnie, bez podążania za dowiązaniami dzieci. Sukces zwalnia i
zeruje właściciela; błąd pozostawia go do ponownej próby i może oznaczać częściowe
sprzątanie. NULL wskaźnika lub właściciela jest dozwolone. Nie zmieniaj nazwy ani
nie zastępuj katalogu głównego; nie zmieniaj wpisów równolegle ze sprzątaniem.
API nie chroni przed wrogimi zmianami systemu plików, a śmierć procesu nie usuwa katalogu.

## Drzewa konfiguracji JSON

[jb/json_tree.h](../../include/jb/json_tree.h) udostępnia własne, modyfikowalne
drzewo hostowe nad tym samym niemodyfikowalnym parserem JSON.
`jb_json_tree_create` kopiuje sparsowane poddrzewo, więc jego wejściowe bajty
i tokeny można potem zwolnić. `jb_json_tree_copy` kopiuje poddrzewo hostowe
z jawnymi limitami. Wyzeruj wskaźnik wynikowego właściciela i zwolnij go przez
`jb_json_tree_destroy`; zajęty wynik daje `HAL_ESTATE`. Reprezentacja korzysta
z płaskiej areny i iteracyjnego przechodzenia także przy głębokim zagnieżdżeniu.

Klucze i stringi zachowują zdekodowane bajty UTF-8, także NUL, bez NFC.
Liczby zachowują oryginalne tokeny: również integers poza uint64, zapis
wykładnika i ujemne zero. Import używa istniejącego ścisłego parsera. To API
nie zmienia zasad komentarzy w manifestach, powtórzonych kluczy ani wartości
nieskończonych.

Korzeń pożycza `jb_json_tree_root`. Funkcje `type`, `count`, `find`, `at`,
`key`, `text` i `get_bool` czytają wartości bez konwersji typu. `at` przechodzi
obiekt w kolejności pól albo wybiera element tablicy; `key` czyta nazwę pola.
`text` pożycza zdekodowany string albo oryginalny token liczby. Widoki i spany
wymagają żywego, niezmienionego właściciela. Udana modyfikacja unieważnia
wszystkie jego widoki i spany, również te do niezmienionych pól.

`jb_json_tree_merge(tree, overlay)` scala obiekty rekurencyjnie. Tablice
i pozostałe wartości zastępują całą wartość bazową. Istniejące klucze zachowują
pozycje, a nowe są dopisywane w kolejności overlay. `set` zastępuje całe pole
obiektu głównego, `erase` usuwa pole, a `append` dopisuje element głównej
tablicy. Każda operacja kopiuje wartość wejściową, także należącą do zmienianego
drzewa. Modyfikacja zatwierdza tylko kompletną nową arenę: błąd zachowuje
poprzednią zawartość i jej widoki. Aby osobno zmienić zagnieżdżony obiekt,
skopiuj jego widok, zmodyfikuj tego właściciela i ustaw go ponownie w rodzicu.

`jb_json_tree_limits_t` ogranicza liczbę wartości wyniku, głębokość kontenerów,
łączną liczbę bajtów zdekodowanych kluczy/stringów i liczb oraz pracę porównań
kluczy. Skalar ma głębokość zero; zero nigdy nie oznacza braku limitu.
Przekroczenie limitu daje `HAL_EOVERFLOW`, brak pamięci `HAL_ENOMEM`, brak pola
`HAL_ENOENT`, a niewłaściwy rodzaj wartości `HAL_ETYPE`. Zwykłe odczyty zachowują
wyniki przy błędzie. Niezależne drzewa mogą działać równolegle; modyfikacja
wspólnego właściciela wymaga serializacji przez wywołującego.

`jb_json_tree_format` zapisuje poddrzewo przez `jb_json_sink_fn`. `indent`
przyjmuje 0 dla zapisu zwartego albo 2/4 spacje dla plików repozytorium.
`sort_keys` porządkuje zdekodowane klucze według punktów kodowych Unicode,
a `ensure_ascii` zapisuje małe cyfry szesnastkowe w sekwencjach Unicode,
w tym pary surogatów dla znaków spoza BMP. Zapis z wcięciami używa odstępów
i krótkich sekwencji sterujących jak Python, zachowując oryginalne tokeny
liczb. Puste kontenery pozostają `{}` i `[]`. Wynik nie ma końcowego LF ani
NUL; wywołujący dodaje je, jeśli wymaga tego format pliku.

Formatter używa podstawowego writera do zapisu stringów. Limit bajtów obejmuje
odstępy i sekwencje specjalne. Sink przyjmuje całą porcję, nie zachowuje
buforów, nie modyfikuje ani nie wywołuje ponownie formatowanego drzewa
i nie może rzucać wyjątków. `written` liczy przyjęte bajty także wtedy,
gdy późniejszy zapis zawiedzie. Wcześniejsze porcje mogą już być wysłane;
strumieniowy writer atomowy pozwala nie publikować częściowego pliku po
błędzie formatowania. Błędy sinka są przekazywane, a niezdefiniowane statusy
dają `HAL_EINTERNAL`.

## Szczegóły platform

Oba API hosta używają `jb_cancel_fn` z `jb/cancel.h`. Uruchomienie na Linuksie
dziedziczy maskę sygnałów wątku wywołującego i ignorowane sygnały; obsługiwane
handlery są resetowane według reguł exec. Biblioteka nie instaluje handlera SIGINT
rodzica. CLI wymagające uporządkowanego sprzątania przy Ctrl+C musi przekazać
żądanie anulowania i zaczekać na zakończenie run przed wyjściem.

Linux ustawia `PR_SET_PDEATHSIG(SIGKILL)` przed exec i sprawdza PID rodzica
po ustawieniu, uwzględniając śmierć procesu rodzica podczas przygotowania.
Chronione jest tylko bezpośrednie dziecko przy śmierci wątku uruchamiającego.
Ustawienie znika po forku dziecka, uprzywilejowanym exec lub odpowiedniej zmianie
poświadczeń; uruchomiony program może je też zmienić. Nie daje to sprzątania
całego joba Windows po nagłej śmierci wywołującego. Szczegóły opisują
[reguły sygnału śmierci rodzica Linux](https://man7.org/linux/man-pages/man2/PR_SET_PDEATHSIG.2const.html).

Windows na czas `CreateProcessW` wyłącza dialogi błędów loadera przez
`SetThreadErrorMode`, po czym przywraca poprzedni tryb wątku przy sukcesie lub
błędzie. Niepoprawne executable zwraca zatem błąd API zamiast czekać na dialog
u wywołującego bez interakcji użytkownika.

Windows konwertuje ścieżki na UTF-16, ale nie dodaje automatycznie `\\?\`.
Obowiązują limity OS oraz manifest i polityka długich ścieżek wywołującego
programu; obsługa UTF-8 sama nie gwarantuje długich ścieżek. Jawne ścieżki
rozszerzone muszą spełniać reguły konkretnego API Win32. Zobacz
[limity ścieżek Windows](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation).
Nazwy tymczasowych plików i katalogów Windows zawierają 128 losowych bitów
z systemowego CNG; o własności nadal decyduje wyłączne utworzenie.

Błędy OS są mapowane wspólnie: nadmierne argumenty lub środowisko dają
`HAL_EOVERFLOW`, składnik ścieżki inny niż katalog `HAL_ENOENT`, niepoprawny format
executable `HAL_ETYPE`, przejściowy brak zasobów `HAL_EAGAIN`, a wyczerpanie
deskryptorów `HAL_ENOMEM`. Zajęty executable lub naruszenie współdzielenia
na Windows daje `HAL_EBUSY`. Wynik procesu zachowuje oryginalny kod błędu OS.
