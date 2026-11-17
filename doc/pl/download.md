# Pobieranie przez HTTPS

`jb/download.h` pobiera jeden zasób HTTPS do pliku na Linuksie i Windows.
Włącz `JHBASE_HOST=ON` i `JHBASE_DOWNLOAD=ON`, przygotuj przypięty checkout curl
opisany w [zależnościach](dependencies.md#opcjonalne-pobieranie-przez-https),
a potem linkuj `jb::download`. Moduł jest napisany w C11 nad API easy libcurl;
w `jb/download.h` nie pojawia się żaden typ ani nagłówek curl. Służy narzędziom
hosta, nie firmware.

## Inicjalizacja

Wywołaj `jb_download_global_init()` raz, zanim wystartują wątki pobierające, i
`jb_download_global_cleanup()` raz po ich zakończeniu. Każda udana
inicjalizacja wymaga jednego cleanup; cleanup czeka też na wątki robocze
porzuconych prób opisanych niżej. Inicjalizacja sprawdza linkowaną bibliotekę
i zwraca `HAL_EUNSUPPORTED`, gdy brakuje jej HTTPS, backendu TLS,
asynchronicznego rozwiązywania nazw albo bezpiecznej wielowątkowo
inicjalizacji. Między tymi wywołaniami niezależne `jb_download_file` mogą
pracować w różnych wątkach.

`jb_download_info(&info)` opisuje linkowane libcurl, także bez inicjalizacji:
tekst wersji, `version_number` w postaci `0xMMmmpp`, backend TLS (`Schannel`
albo `OpenSSL/<wersja>`) oraz flagi `https`, `async_dns` i `threadsafe`.
Biblioteka zbudowana z taga gita upstreamu podaje tekst wersji z sufiksem
`-DEV`, np. `8.23.0-DEV`; `version_number` wskazuje wydanie.

## Pobranie pliku

`jb_download_file(url, destination, &options, &result)` pobiera `url`
i zapisuje treść odpowiedzi w `destination`:

- `url` musi mieć schemat `https`. Inne schematy, także nieobecne w buildzie
  libcurl, dają `HAL_EPERM`; błędny URL daje `HAL_EINVAL`. Obie kontrole
  odbywają się przed jakimkolwiek połączeniem i dostępem do plików.
- `destination` to bezwzględna ścieżka UTF-8. Brakujące katalogi nadrzędne
  powstają same. Treść trafia do pliku tymczasowego obok celu i zastępuje cel
  atomowo dopiero po pełnym transferze z końcowym statusem 2xx. Błąd,
  anulowanie albo kolejna próba usuwają plik tymczasowy, a cel zachowuje
  poprzednią zawartość. Identyczne bajty zostawiają istniejący plik bez zmian.
- `options.connect_timeout_ms`, `options.timeout_ms` i `options.max_attempts`
  są wymagane. `max_bytes` ogranicza treść jednej próby; 0 dopuszcza tylko pustą
  odpowiedź. `max_redirects` ogranicza przekierowania jednej próby; 0 odrzuca
  każde przekierowanie.
- `result` jest czyszczony na początku i opisuje ostatnią próbę: zapisane
  bajty, liczbę rozpoczętych prób, końcowy status HTTP, odpowiedź proxy na
  `CONNECT` w `proxy_status`, kod wyniku libcurl w `transfer_error` i krótki
  komunikat UTF-8 w `message`.

```c
jb_download_options_t options;
memset(&options, 0, sizeof(options));
options.max_bytes = UINT64_C(512) << 20;
options.connect_timeout_ms = 15000;
options.timeout_ms = 600000;
options.retry_delay_ms = 2000;
options.max_attempts = 4;
options.max_redirects = 5;
jb_download_result_t result;
const hal_status_t status = jb_download_file(
    "https://example.org/tool-1.2.tar.gz", "/home/me/.cache/tools/tool-1.2.tar.gz", &options,
    &result);
if (status != HAL_OK)
  fprintf(stderr, "download failed (%d, HTTP %u, curl %d): %s\n", (int)status,
          (unsigned)result.http_status, (int)result.transfer_error, result.message);
```

Moduł nie sprawdza treści. Przed użyciem pobranego archiwum sprawdź jego
rozmiar i SHA-256.

## TLS i zaufanie

Weryfikacja certyfikatu i nazwy hosta jest zawsze włączona, dla pierwszego
URL i każdego przekierowania, a przekierowanie może prowadzić tylko do URL
`https`. Przekierowanie do zwykłego HTTP kończy wywołanie z `HAL_EPERM`,
zanim moduł połączy się z serwerem HTTP.

`options.ca_file = NULL` używa magazynu systemowego, także dla proxy `https`. Na Windows Schannel
korzysta z magazynu certyfikatów użytkownika i komputera. Na Linuksie libcurl
używa pakietu i katalogu CA dystrybucji, które configure znalazł podczas budowania
biblioteki, np. `/etc/ssl/certs/ca-certificates.crt`; zainstaluj
`ca-certificates` przed configure. W przeciwnym razie `ca_file` wskazuje plik
PEM, który zastępuje magazyn systemowy, także linuksowy katalog CA, zarówno
dla serwera, jak i dla proxy `https`.

Schannel sprawdza dodatkowo unieważnienie każdego certyfikatu w łańcuchu
i pobiera wskazane w nich listy CRL, więc łańcuch bez osiągalnych danych
o unieważnieniu zostaje odrzucony. OpenSSL na Linuksie nie sprawdza unieważnień.

Odrzucony certyfikat, nazwa hosta albo status unieważnienia dają `HAL_EAUTH`.
Nieczytelny `ca_file` daje `HAL_EINVAL`. Inne błędy handshake TLS dają
`HAL_EPROTO`. Żaden z nich nie uruchamia kolejnej próby.

## Próby, timeouty i anulowanie

`timeout_ms` to budżet całego wywołania. Obejmuje rozwiązywanie nazw,
połączenia, transfery i oczekiwanie między próbami, a każda próba dostaje
pozostały czas. `connect_timeout_ms` ogranicza jedno połączenie razem z DNS
i handshake TLS. Rozwiązywanie nazw pracuje w wątku pomocniczym libcurl, więc
limity obejmują także wolny serwer DNS: gdy próba skończy się w trakcie
wyszukiwania nazwy, wywołanie wraca od razu, a wątek pomocniczy sam czeka
na systemowy resolver i potem się kończy.

Kolejna próba następuje tylko po błędzie przejściowym: nieudanym rozwiązaniu
nazwy lub połączeniu, limicie połączenia, który minął, gdy budżet miał jeszcze
czas, przerwanym transferze albo statusie HTTP 408, 429, 500, 502, 503 lub 504
od serwera albo od proxy, które odmawia tunelu `CONNECT`.
Najpierw wywołanie czeka `retry_delay_ms`, a dłużej, gdy serwer przyśle
`Retry-After`. Gdy oczekiwanie sięgnęłoby końca budżetu, wywołanie od razu
zwraca ostatni błąd. Po wyczerpaniu prób też zwraca ten błąd: `HAL_EAGAIN`
dla powyższych statusów HTTP, a dla pozostałych przypadków `HAL_ETIMEOUT`,
`HAL_EIO` albo `HAL_ENOENT`.

Ostateczne błędy HTTP kończą wywołanie od razu: 404 i 410 dają `HAL_ENOENT`;
401, 403 i 407 dają `HAL_EAUTH`, także gdy proxy odpowie nimi na `CONNECT`;
każdy inny status spoza 2xx, także 304 bez przekierowania, daje `HAL_EPROTO`.
Treść większa niż `max_bytes` daje `HAL_EOVERFLOW`, a zapowiedziany rozmiar
powyżej limitu kończy próbę przed odczytem treści, także przy `max_bytes`
równym 0. Za dużo przekierowań również daje `HAL_EOVERFLOW`. Nieudany zapis
celu zwraca status API plików, a `result.bytes` obejmuje wtedy bajty, które
nieudany zapis zdążył zapisać przed błędem.

libcurl wykonuje każdą próbę w wątku roboczym, a wątek wywołujący zachowuje
oba callbacki i limity. Tam `options.canceled` jest odpytywane przed każdą
próbą, co najmniej co 50 ms podczas próby i przerwy między próbami oraz raz
jeszcze po odebraniu całej treści, przed jej publikacją; `true` daje
`HAL_ECANCELED`. `options.progress` dostaje numer próby, bajty zapisane w tej
próbie i zapowiedziany rozmiar (0, gdy nieznany) za każdym razem, gdy któraś
z tych wartości się zmieni.

Wątek wywołujący kończy też próbę po upływie terminu oraz, dopóki TLS nie
jest zestawione, po upływie limitu połączenia, niezależnie od tego, co robi
libcurl. Zatrzymany wątek roboczy zwykle kończy się w ciągu 100 ms, a
wywołanie usuwa jego plik tymczasowy. Gdy dłużej trzyma go blokujące
wywołanie systemowe, np. Schannel pobierający listę CRL z serwera, który nie
odpowiada, albo zawieszony zapis na dysk, wywołanie i tak wraca i porzuca
próbę: wątek roboczy usuwa plik tymczasowy, gdy wywołanie systemowe wróci,
a `jb_download_global_cleanup` na niego czeka.

## Proxy

`options.proxy = NULL` łączy się bezpośrednio i pomija zmienne środowiskowe
`https_proxy`, `ALL_PROXY` i podobne. URL proxy, np.
`http://proxy.example:3128`, kieruje przez to proxy każde połączenie; HTTPS
idzie tunelem `CONNECT`, a `no_proxy` nie ma zastosowania. Dane logowania mogą
być częścią URL proxy, np. `http://user:secret@proxy:3128`, i używają
uwierzytelniania Basic; proxy, które przyjmuje tylko inną metodę, odpowiada
407, co daje `HAL_EAUTH`. URL proxy może używać schematu `http`, `https`,
`socks4`, `socks4a`, `socks5` albo `socks5h`; bez schematu oznacza proxy HTTP,
niezależnie od nazwy hosta. Inny schemat lub błędny URL proxy daje
`HAL_EINVAL` przed jakimkolwiek połączeniem.

## Sygnały i wątki

Na Linuksie i innych systemach POSIX każdy wątek roboczy blokuje `SIGPIPE`
i zdejmuje jeden taki sygnał zgłoszony w nim; maska sygnałów wątku
wywołującego i dyspozycja sygnałów procesu pozostają bez zmian. libcurl i tak
wysyła dane z `MSG_NOSIGNAL`, gdy system to umożliwia, a jego wątki
rozwiązywania nazw dziedziczą zablokowany sygnał po wątku roboczym.

Moduł nie przechowuje stanu między wywołaniami: nie ma cookies ani pamięci
podręcznej HSTS czy alt-svc i nigdy nie czyta `.netrc`. Każda próba otwiera
nowe połączenie.
