# Rozpakowywanie archiwów

`jb/archive.h` rozpakowuje archiwa ZIP i TAR skompresowane gzipem do pustego
katalogu na Linuksie i Windows. Włącz `JHBASE_HOST=ON` i `JHBASE_ARCHIVE=ON`,
przygotuj przypięte checkouty libarchive i zlib opisane w
[zależnościach](dependencies.md#opcjonalne-rozpakowywanie-archiwów), a potem
linkuj `jb::archive`. Moduł jest napisany w C11 i alokuje pamięć ze sterty;
służy narzędziom hosta, nie firmware.

## Wywołanie

`jb_archive_extract(archive, destination, &options, &result)` czyta jeden
plik archiwum i zapisuje jego wpisy w katalogu `destination`:

- `archive` to ścieżka UTF-8 zwykłego pliku, względna wobec katalogu roboczego
  albo bezwzględna.
- `destination` to bezwzględna ścieżka UTF-8 istniejącego, pustego katalogu.
  Użyj prywatnego katalogu staging, np. z `jb_temp_dir_create`, i publikuj
  jego zawartość dopiero po `HAL_OK`. Nieudane wywołanie może zostawić
  niepełne drzewo; usuń wtedy cały katalog.
- `options.format` wybiera `JB_ARCHIVE_ZIP` albo `JB_ARCHIVE_TAR_GZIP`.
  Moduł niczego nie zgaduje. tar.gz podany jako ZIP, nieskompresowany TAR
  podany jako tar.gz i każdy inny format kończą się `HAL_EPROTO`. Wpisy ZIP
  pochodzą z katalogu centralnego. tar.gz moduł czyta do końca, a każdy człon
  gzip musi zgadzać się z CRC-32 i długością zapisanymi w swojej stopce.
- `max_entries`, `max_total_bytes` i `max_file_bytes` to twarde limity;
  0 nie dopuszcza niczego danego rodzaju. Rozmiary zadeklarowane w nagłówkach
  moduł sprawdza przed utworzeniem pliku, a dane liczy jeszcze raz podczas
  odczytu. Przypięte libarchive samo odrzuca też dane większe niż deklaruje
  nagłówek.
- `exclusions` to zakończona NULL lista dokładnych nazw wpisów z separatorem
  `/`; ukośnik wsteczny w nazwie wpisu porównuje się jak `/`. Każda nazwa musi
  pasować do jakiegoś wpisu, inaczej wywołanie po ostatnim wpisie zwraca
  `HAL_ENOENT` i podaje to wykluczenie w `result.entry`. Wykluczone wpisy
  omijają wszystkie kontrole nazw, co przydaje się przy plikach, których host
  nie potrafi zapisać. Zawartość wykluczonego katalogu nadal trafia na dysk.
- `canceled` jest odpytywany przed każdym wpisem i każdym blokiem danych
  oraz przed każdym blokiem tar.gz czytanym po końcu TAR-a; `true` daje
  `HAL_ECANCELED`.

```c
const char *const skip[] = {"[Content_Types].xml", NULL};
jb_archive_options_t options;
memset(&options, 0, sizeof(options));
options.format = JB_ARCHIVE_ZIP;
options.max_entries = 20000;
options.max_total_bytes = UINT64_C(1) << 30;
options.max_file_bytes = UINT64_C(256) << 20;
options.exclusions = skip;
jb_archive_result_t result;
const hal_status_t status = jb_archive_extract("package.nupkg", staging, &options, &result);
if (status != HAL_OK)
  fprintf(stderr, "extraction failed (%d): %s %s\n", (int)status, result.message, result.entry);
```

## Co trafia do katalogu docelowego

Nazwy wpisów muszą być w UTF-8. Komponenty rozdzielają zarówno `/`, jak i `\`,
więc archiwum daje to samo drzewo na Linuksie i Windows. Puste komponenty
i `.` moduł pomija. Komponent `..`, separator na początku albo prefiks dysku,
np. `C:`, dają `HAL_EPERM`, nawet gdy ścieżka nie wyszłaby poza katalog.
Nazwy i cele linków dłuższe niż 4096 bajtów dają `HAL_EOVERFLOW`.

Na Windows komponent z `:` daje `HAL_EPERM`, bo Win32 czyta go jako dysk albo
alternatywny strumień danych. Nazwy urządzeń (`CON`, `PRN`, `AUX`, `NUL`,
`COM0`-`COM9`, `LPT0`-`LPT9`, ich odmiany z cyfrą w indeksie górnym, `CONIN$`,
`CONOUT$`), także z rozszerzeniem, znaki `< > " | ? *`, znaki sterujące oraz
kropka lub spacja na końcu dają `HAL_EUNSUPPORTED`. Linux przyjmuje takie nazwy.

Moduł tworzy katalogi, zwykłe pliki, dowiązania symboliczne i twarde
dowiązania TAR. Pliki urządzeń, FIFO, gniazda i zaszyfrowane wpisy dają
`HAL_EUNSUPPORTED`. Każdy plik i katalog powstaje na wyłączność, więc drugi
wpis o tej samej nazwie daje `HAL_EEXIST` zamiast zastąpić pierwszy. Powtórzyć
może się tylko katalog. Ten sam status zgłaszają nazwy, które system plików
traktuje jak jedną, np. `Dir` i `dir` na NTFS albo krótka nazwa Windows.
`result.message` mówi wtedy o kolizji z istniejącym wpisem, a przy nazwie
powtórzonej w archiwum mówi o powtórzeniu.

Zapis nigdy nie przechodzi przez dowiązanie. Każdy komponent nadrzędny musi być
katalogiem utworzonym przez to samo wywołanie: plik w tym miejscu daje
`HAL_EEXIST`, a dowiązanie `HAL_EPERM`. Dowiązania symboliczne powstają po
wszystkich pozostałych wpisach. Cel dowiązania musi być względny i może
zaczynać się wyłącznie od komponentów `..`; ich liczba nie może przekroczyć
głębokości katalogu dowiązania, a po nazwie nie może wystąpić `..`
(`HAL_EPERM`). Przy tych zasadach każde dowiązanie i każdy łańcuch dowiązań
zostaje w katalogu docelowym. Separatory w celach zmieniają się na natywne.
Twarde dowiązanie musi wskazywać zwykły plik rozpakowany wcześniej przez to
samo wywołanie (inaczej `HAL_ENOENT`) i nie może nieść danych
(`HAL_EUNSUPPORTED`).

Na Linuksie plik dostaje tryb 0666 z bitami wykonywania wpisu, a katalog 0777,
w obu przypadkach ograniczone przez umask. Moduł nie odtwarza właścicieli,
czasów, ACL ani atrybutów rozszerzonych. Pliki nie są wymuszane na trwały
nośnik; o trwałości zdecyduj przed ich publikacją.

## Wynik i błędy

`result` jest zerowany przed jakąkolwiek kontrolą. `entries` liczy odczytane
nagłówki, łącznie z wykluczonymi wpisami i tym, na którym wystąpił błąd.
`bytes` liczy bajty zapisane do zwykłych plików, także te, które niepełny
zapis zdążył zapisać przed błędem. Po błędzie `entry` zawiera
nazwę przetwarzanego wpisu (obciętą na granicy znaku, pustą, gdy archiwum nie
dało czytelnej nazwy), a `message` krótki powód od jhbase albo libarchive.
`native_error` zawiera `errno`, `GetLastError()` albo numer błędu libarchive.
Po sukcesie oba teksty są puste.

| Status | Znaczenie |
| --- | --- |
| `HAL_EINVAL` | Brak argumentu, nieznany format, puste wykluczenie, względny katalog docelowy. |
| `HAL_ETYPE` | Archiwum nie jest zwykłym plikiem albo katalog docelowy nie jest katalogiem. |
| `HAL_EEXIST` | Katalog docelowy nie jest pusty albo nazwy się powtarzają lub kolidują. |
| `HAL_EPROTO` | Błędne lub uszkodzone dane (także CRC i stopki gzip), dane po ostatnim członie gzip, inny format lub kompresja, nazwa spoza UTF-8. |
| `HAL_EPERM` | Wpis lub dowiązanie wyszłyby poza katalog albo przeszłyby przez dowiązanie; dowiązanie na Windows bez potrzebnego uprawnienia. |
| `HAL_EUNSUPPORTED` | Rodzaj wpisu, szyfrowanie, dane w twardym dowiązaniu albo nazwa, której Windows nie zapisze. |
| `HAL_EOVERFLOW` | Limit albo granica 4096 bajtów tekstu. |
| `HAL_ENOENT` | Brak archiwum lub katalogu docelowego, celu twardego dowiązania albo dopasowania wykluczenia. |
| `HAL_ECANCELED`, `HAL_ENOMEM`, `HAL_EIO` | Anulowanie, brak pamięci albo błąd systemu podany w `native_error`. |

## Szczegóły platform

libarchive konwertuje nazwy według lokalizacji typu znaków. Dlatego na Linuksie
wątek wywołujący używa podczas wywołania `C.UTF-8`, także w czasie działania
callbacku `canceled`, i przed powrotem odzyskuje poprzednią lokalizację.
System bez `C.UTF-8` daje `HAL_EUNSUPPORTED`. Pozostałe wątki zachowują swoją
lokalizację.

Ścieżki Windows używają UTF-16 bez prefiksu rozszerzonej długości, więc
obowiązują zwykłe limity długości. Tworzenie dowiązań symbolicznych wymaga
trybu dewelopera albo uprawnienia do dowiązań symbolicznych; bez nich
archiwum z dowiązaniami kończy się `HAL_EPERM`. Dowiązanie powstaje jako
dowiązanie katalogu, gdy system plików docelowego katalogu rozwiązuje jego
cel do katalogu według własnych reguł wielkości liter i krótkich nazw;
pozostałe dowiązania, także wiszące, są dowiązaniami plików.

Moduł nie jest sandboksem. Zakłada prywatny katalog docelowy, którego żaden
inny proces nie zmienia w trakcie wywołania; dowiązania w ścieżce nadrzędnej
katalogu docelowego działają według reguł systemu. Niezależne wywołania mogą
działać równolegle.
