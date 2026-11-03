# System plików przez API C

`jb/filesystem.h` udostępnia operacje katalogowe, metadane i ścieżki UTF-8
na Linuksie oraz Windows. Włącz `JHBASE_HOST=ON` i linkuj `jb::filesystem`.
Ten target ma implementację C11 i nie wymaga runtime C++. `jb::host` również
go linkuje dla istniejących konsumentów operacji plików i procesów.

Ścieżki muszą być niepuste, zakończone NUL i zapisane w UTF-8. Implementacja
używa POSIX na Linuksie i funkcji Win32 z UTF-16 na Windows. Błędne kodowanie
daje `HAL_EPROTO`, także gdy nazwa listowanego wpisu nie jest poprawnym UTF-8.
Usuwanie rekursywne korzysta z natywnych nazw dzieci i potrafi usunąć takie
wpisy. Ścieżki względne korzystają z katalogu roboczego procesu;
operacje nigdy go nie zmieniają.

## Ścieżki

`jb_fs_path_normalize`, `jb_fs_path_join`, `jb_fs_path_parent` i
`jb_fs_path_filename` przetwarzają natywną składnię ścieżki bez otwierania
plików. Dyski Windows i UNC podlegają regułom Windows. `jb_path_resolve`
z `jb/file.h` korzysta z tej samej implementacji, lecz wymaga bezwzględnej
bazy i odrzuca wejście względne wobec dysku Windows.
Na POSIX zachowuje bazę złożoną wyłącznie z ukośników, np. `//`.
Wynik wyodrębnienia katalogu nadrzędnego używa natywnych separatorów,
czyli backslash na Windows.

`jb_fs_cwd` odczytuje katalog roboczy. `jb_fs_realpath` rozwiązuje istniejącą
ścieżkę przez dowiązania; wszystkie składniki muszą istnieć. Windows
zachowuje rozszerzony prefiks zwrócony przez system. Operacje leksykalne
nie dodają go same; dopuszczalna długość ścieżki zależy od systemu
i manifestu programu.

Dla ścieżek wynikowych `(NULL, 0)` mierzy wymagany rozmiar razem z NUL.
Za mały bufor daje `HAL_EOVERFLOW`, pełny wymagany rozmiar i zachowuje
zawartość bufora. Inne błędy zachowują oba wyjścia. Ścieżka może zmienić
się między pomiarem rozmiaru a kolejnym wywołaniem.
Jeśli `jb_fs_realpath` zgłasza systemowy limit długości ścieżki, ustawia
`required` na zero. Nie udało się rozwiązać ścieżki, więc większy bufor nie pomoże.
Bufor wynikowy może wykorzystać pamięć wejścia. Zmienna na wymagany rozmiar
nie może nakładać się na tekst wejścia ani bufor wynikowy; takie nakładanie
daje `HAL_EINVAL`.

## Wpisy i katalogi

`jb_fs_status` zwraca rodzaj wpisu i rozmiar zwykłego pliku; jawnie wybierz,
czy odczyt podąża za końcowym dowiązaniem. `jb_fs_list` przekazuje bezpośrednie
wpisy poza `.` i `..` w nieustalonej kolejności natywnej. Callback pożycza
nazwę i metadane tylko na czas wywołania; jego błąd przerywa listowanie.
Metadane nie podążają za końcowym dowiązaniem.
Przy `follow_symlinks=false` odczyt statusu pomija także końcowe separatory,
żeby zbadać samo dowiązanie.

`jb_fs_mkdir` może tworzyć brakujące katalogi nadrzędne. `jb_fs_remove`
może schodzić w katalogi, lecz usuwa dowiązania i punkty reparse Windows
bez wejścia do ich celów, także przy separatorach na końcu wejścia.
Odrzuca korzenie i końcowe `.` lub `..`. Separator na końcu ścieżki zwykłego
pliku daje `HAL_ETYPE`. Błąd może pozostawić częściowo utworzone lub usunięte drzewo.

Usuwanie otwiera katalogi POSIX przez `openat(O_NOFOLLOW)` i używa `unlinkat`
względem utrzymywanych deskryptorów. Windows blokuje podmianę otwartych wpisów
i otwarcia do zapisu, schodzi po dosłownych ścieżkach rozszerzonych i usuwa
przez uchwyty. Przed usunięciem kasuje atrybut read-only, a po niepowodzeniu
próbuje go przywrócić. Odrębne nazwy, np. `x` i `x. `, pozostają odrębne przy
schodzeniu. Pamięć i liczba otwartych uchwytów rosną z głębokością katalogów.
API nie izoluje operacji od innych użytkowników filesystemu: wywołujący nadal
koordynuje zmiany cwd i ścieżki nadrzędnej, a równoległe zmiany drzewa mogą
spowodować częściowe niepowodzenie.

Na Windows usuwanie obsługuje ścieżki dysków, UNC oraz ścieżki z prefiksem
urządzenia wskazujące dysk, UNC lub `Volume{GUID}`. Odrzuca korzenie woluminów
i udziałów przed uzyskaniem do nich dostępu. Dla innych przestrzeni nazw
urządzeń usuwanie zwraca `HAL_EUNSUPPORTED`; funkcje leksykalne nadal
przetwarzają ich natywną składnię.
Usuwanie odrzuca też końcową nazwę złożoną wyłącznie z kropek i spacji,
którą Win32 może odczytać jako rodzica lub korzeń. Dokładny prefiks `\\?\`
zachowuje dosłowne nazwy i pozwala usuwać takie wpisy; dokładne `.` i `..`
nadal podlegają odrzuceniu.
Usuwanie nie obsługuje alternatywnych strumieni danych: dwukropek poza
prefiksem dysku daje `HAL_EUNSUPPORTED`, również z prefiksem dosłownym.
Nazwa strumienia katalogu może wskazywać sam katalog albo korzeń woluminu.

`jb_fs_rename` może zastąpić zwykły plik, lecz odrzuca istniejący katalog
lub dowiązanie w miejscu docelowym. Na Windows zmienia także samą wielkość
liter nazwy; odrębne hardlinki do tego samego pliku pozostają bez zmian.
Przenoszenie między systemami plików
nie uruchamia zastępczego kopiowania i usuwania. `jb_fs_equivalent`
porównuje tożsamość istniejących wpisów po przejściu przez dowiązania.

Odczyt z limitem, atomową publikację, zapis strumieniowy, blokady i prywatne
katalogi tymczasowe zapewnia [dotychczasowe API plików](host.md).
Operacje zachowują interfejs `jb/file.h` i linkowanie przez `jb::host`.
