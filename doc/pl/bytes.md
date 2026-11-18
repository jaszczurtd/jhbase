# Kolejność bajtów i SHA-256

[endian.h](../../include/jb/endian.h) odczytuje i zapisuje liczby 16-, 32-
i 64-bitowe w buforach bajtów, w kolejności little-endian albo big-endian.
[sha256.h](../../include/jb/sha256.h) liczy skróty SHA-256 (FIPS 180-4)
z danych podawanych w częściach. Oba moduły są przenośnym kodem C11, działają
także w C++17 i nie alokują pamięci.

## Kolejność bajtów

Funkcje odczytu (`jb_load_le16`, `jb_load_le32`, `jb_load_le64` i ich
odpowiedniki `be`) oraz zapisu (`jb_store_le16` i kolejne) sięgają do bufora
po jednym bajcie. Bufor może zaczynać się pod dowolnym adresem, a wynik nie
zależy od kolejności bajtów na hoście. Zapis zmienia dokładnie dwa, cztery
albo osiem bajtów i nie rusza sąsiednich. Funkcje nie sprawdzają wskaźników:
wywołujący zapewnia tyle bajtów, ile wymaga szerokość liczby.

`jb_bswap16`, `jb_bswap32` i `jb_bswap64` odwracają kolejność bajtów wartości.
`jb_u16_msb` i `jb_u16_lsb` zwracają starszy i młodszy bajt liczby 16-bitowej,
a `jb_u16_from_bytes(msb, lsb)` składa je z powrotem.

```c
uint8_t frame[6];
jb_store_be16(frame, 0x1234u);         /* 12 34 */
jb_store_le32(frame + 2, 0xA1B2C3D4u); /* D4 C3 B2 A1 */
uint32_t value = jb_load_le32(frame + 2); /* 0xA1B2C3D4 */
```

Wszystkie funkcje są zdefiniowane w nagłówku jako `static inline`, więc nie
wymagają biblioteki i nie mają stanu.

## SHA-256

`jb_sha256_init()` rozpoczyna liczenie skrótu, `jb_sha256_update()` dodaje
kolejną część danych, a `jb_sha256_final()` zapisuje 32-bajtowy wynik. Podział
danych na części dowolnej wielkości daje ten sam skrót co jedno wywołanie
z całością. Po `jb_sha256_final()` kontekst jest zakończony; przed następnym
skrótem trzeba go ponownie rozpocząć.

```c
jb_sha256_context_t sha;
uint8_t digest[JB_SHA256_DIGEST_BYTES];
jb_sha256_init(&sha);
jb_sha256_update(&sha, header, header_length);
jb_sha256_update(&sha, payload, payload_length);
jb_sha256_final(&sha, digest);
```

Funkcje nie sprawdzają argumentów. `data` może być NULL tylko przy zerowej
długości, a wywołanie z długością zero nie zmienia kontekstu. Funkcje
zgłaszające błędy, np. `hal_sha256_update_ex()` w JaszczurHAL, najpierw same
sprawdzają argumenty i korzystają z pola `initialized`. Ma ono wartość true od
`jb_sha256_init()` do `jb_sha256_final()`.

`jb_sha256_context_t` ma stały układ: 112 bajtów przy wskaźnikach 32-bitowych
i 120 bajtów przy 64-bitowych, z wyrównaniem jak `uint64_t`. Nagłówek się nie
kompiluje, gdy `#pragma pack` albo flaga kompilatora zmienia przesunięcia pól,
rozmiar lub wyrównanie. Firmware może trzymać kontekst w zmiennej statycznej.
Pozostałe pola traktuj jako prywatne. Osobne konteksty są od siebie niezależne;
dostęp do jednego kontekstu z kilku wątków wymaga synchronizacji po stronie
wywołującego.

## Firmware

Oba moduły należą do `jb::base`. Biblioteka firmware, która nie linkuje
`jb::base`, może skompilować rdzeń SHA-256 do własnego archiwum:
[sources.cmake](../../cmake/sources.cmake) ustawia w `JHBASE_SHA256_SOURCES`
jego pliki źródłowe. W ten sposób JaszczurHAL wbudowuje rdzeń w każdą
bibliotekę HAL. Nagłówki statusów w zainstalowanym pakiecie obejmują
`endian.h` i `sha256.h`, więc pakiet dostarczający je razem z własnym
archiwum pozostaje kompletny
(zobacz [zainstalowany pakiet](dependencies.md#zainstalowany-pakiet)).

JaszczurHAL zachowuje własne nazwy jako aliasy tych definicji:
`hal/core/jh_endian.h` przypisuje funkcje `jh_*` oraz `MSB()`/`LSB()` do
funkcji `jb_*`, a `hal_sha256_context_t` jest typem `jb_sha256_context_t`.
