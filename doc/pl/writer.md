# Sekwencyjny zapis JSON

Writer zapisuje jedną wartość JSON bez zbędnych białych znaków przez
synchroniczny callback. Dołącz [json_writer.h](../../include/jb/json_writer.h).
Stan, stos kontenerów i skopiowane klucze znajdują się w pamięci wywołującego. Heap ani mutowalny DOM nie są potrzebne.
Kompletny [przykład writera](../../examples/json_writer.c) jest budowany
i uruchamiany jako C11 oraz C++17.

## Inicjalizacja i zakończenie

`jb_json_writer_workspace_size(limits, out_bytes)` oblicza wymagany scratch
w bajtach, uwzględniając jednocześnie otwarte kontenery i klucze. Sprawdza
przepełnienie rozmiaru; oba wskaźniki są wymagane. Scratch może być celowo
niewyrównaną pamięcią bajtową.

`jb_json_writer_init(writer, limits, work, sink, user)` rozpoczyna nowy dokument.
Wskaźniki writer, limits i sink są wymagane; user trafia do sinka bez zmian.
Błędne argumenty dają `HAL_EINVAL`, a za mały scratch daje `HAL_EOVERFLOW`.
Nieudany init zostawia nieaktywny uchwyt z zapamiętanym błędem. Ponowny init
pozwala też zacząć od nowa po błędzie.

| Limit | Znaczenie |
| --- | --- |
| `max_output_bytes` | Wszystkie bajty wyniku, bez terminatora. |
| `max_depth` | Jednocześnie otwarte tablice i obiekty. |
| `max_keys` | Wszystkie klucze w aktualnie otwartych obiektach. |
| `max_key_bytes` | Skopiowane zdekodowane bajty UTF-8 tych kluczy. |
| `max_key_compare_bytes` | Budżet porównań całego dokumentu: jeden na porównanie, potem oba faktycznie porównane bajty. |

Zero zawsze dopuszcza zero. Zamknięcie kontenera zwalnia jego rekordy i bajty
kluczy, zachowując dane rodziców. Zamknięcie nie odnawia budżetu porównań.

```c
static unsigned char scratch[512];
static char output[128];
const jb_json_writer_limits_t limits = {sizeof(output), 1, 3, 32, 128};
const jb_json_workspace_t work = {scratch, sizeof(scratch)};
jb_json_buffer_t buffer = {output, sizeof(output), 0};
jb_json_writer_t writer;
size_t needed;
hal_status_t status = jb_json_writer_workspace_size(&limits, &needed);
if (status != HAL_OK) {
    return status;
}
if (needed > work.size) {
    return HAL_EOVERFLOW;
}
status = jb_json_writer_init(&writer, &limits, work, jb_json_buffer_sink, &buffer);
```

`jb_json_writer_finish(writer, out_bytes)` wymaga dokładnie jednego ukończonego
korzenia i braku otwartych kontenerów. Oba wskaźniki są wymagane. Nieukończony
dokument daje `HAL_ESTATE`. Sukces zwraca liczbę przyjętych bajtów; ponowny
finish zwraca ten sam wynik bez wywołania sinka. Zapis po finish daje
`HAL_ESTATE`.

## Kontenery i wartości proste

| Funkcja | Działanie |
| --- | --- |
| `jb_json_writer_object_begin(writer)` | Otwiera obiekt w miejscu wartości. |
| `jb_json_writer_object_end(writer)` | Zamyka najgłębszy obiekt; klucz bez wartości oznacza błąd. |
| `jb_json_writer_array_begin(writer)` | Otwiera tablicę w miejscu wartości. |
| `jb_json_writer_array_end(writer)` | Zamyka najgłębszą tablicę; obiekt w tym miejscu oznacza błąd. |
| `jb_json_writer_key(writer, key)` | Kopiuje zdekodowaną nazwę UTF-8, sprawdza jej unikalność w bieżącym obiekcie i oczekuje wartości. |
| `jb_json_writer_string(writer, value)` | Zapisuje bajty UTF-8 z escape'ami dla znaków sterujących, cudzysłowów i backslashy. |
| `jb_json_writer_bool(writer, value)` | Zapisuje `true` albo `false`. |
| `jb_json_writer_null(writer)` | Zapisuje `null`. |
| `jb_json_writer_u64(writer, value)` | Zapisuje dokładną liczbę całkowitą bez znaku. |
| `jb_json_writer_i64(writer, value)` | Zapisuje dokładną liczbę całkowitą ze znakiem, także `INT64_MIN`. |
| `jb_json_writer_number(writer, token)` | Sprawdza kompletny token liczby JSON i zachowuje jego bajty. |
| `jb_json_writer_f32(writer, value)` | Zapisuje skończone natywne binary32 najkrótszą postacią dziesiętną odtwarzającą tę samą wartość. |
| `jb_json_writer_f64(writer, value)` | Zapisuje skończone natywne binary64 według tej samej zasady. |
| `jb_json_writer_node(writer, node)` | Iteracyjnie zapisuje sparsowane poddrzewo, stosując limity writera. |

Miejsce wartości to korzeń, element tablicy albo wartość po kluczu obiektu.
Writer sam dodaje przecinki i dwukropki. Drugi korzeń, wartość lub klucz
w niewłaściwym miejscu oraz zamknięcie kontenera niewłaściwego typu dają `HAL_ESTATE`.
Wyczerpanie limitu daje `HAL_EOVERFLOW`.

Klucze i stringi korzystają ze spanów, bez terminatora. `(NULL, 0)` oznacza
pusty ciąg; NUL wewnątrz spana pozostaje danymi. Błędne UTF-8 daje `HAL_EPROTO`.
Klucze są porównywane według zdekodowanych bajtów, bez NFC i case folding;
duplikat daje `HAL_EEXIST`. Bufor źródłowy klucza można wykorzystać ponownie
od razu po powrocie funkcji.

```c
const jb_json_span_t key = {"message", 7};
const jb_json_span_t value = {"a\0b", 3};
(void)jb_json_writer_object_begin(&writer);
(void)jb_json_writer_key(&writer, key);
(void)jb_json_writer_string(&writer, value);
(void)jb_json_writer_object_end(&writer);
size_t length;
status = jb_json_writer_finish(&writer, &length);
```

Wynikiem jest `{"message":"a\u0000b"}`. Udany finish jest obowiązkowy także po
poprawnym zamknięciu ostatniego kontenera. Można sprawdzać sam finish, ponieważ każda operacja
zapamiętuje pierwszy błąd. Można też sprawdzać wynik każdej operacji.

Token liczby nie zawiera otaczających białych znaków ani wewnętrznego NUL.
Wartości takie jak `1e400` są poprawnymi tokenami, mimo że konwersja do
natywnego typu zmiennoprzecinkowego przekroczyłaby zakres. Błędny token daje
`HAL_EPROTO`. Zapis zmiennoprzecinkowy odrzuca NaN/Infinity przez `HAL_EINVAL`
i zachowuje znak zera jako `-0`. Zapis odtwarzający tę samą wartość różni się
od [formatu literałów C](numbers.md) o ustalonej precyzji.
Buforowy backend Ryu korzysta ze stałej pamięci lokalnej i tablic zoptymalizowanych
pod kątem rozmiaru; nie wymaga dodatkowego scratch writera.
Zobacz [zależności](dependencies.md).

`writer_node` ponownie dekoduje i zapisuje klucze oraz stringi, zachowując
surowe tokeny liczb. Nie zachowuje białych znaków ani oryginalnego zapisu
escape'ów. Pomija nazwę pola samego wskazanego węzła; w razie potrzeby
wywołujący podaje zewnętrzny klucz obiektu. Limit głębokości obejmuje
istniejący kontekst writera i dołączane poddrzewo. Dokument i jego bufory
muszą pozostać dostępne i niezmienione przez całe wywołanie.

## Sinki i błędy

`jb_json_sink_fn(user, data, length)` musi synchronicznie przyjąć cały fragment
albo zwrócić zdefiniowany ujemny status HAL. Nie może zachować wskaźnika `data`, ponownie
wywołać tego samego writera ani modyfikować jego scratch. `HAL_NONE` i nieznane
kody zmieniają się w `HAL_EINTERNAL`. Zdefiniowany błąd, także `HAL_EAGAIN`,
jest przekazywany bez zmian.

| Sink | Działanie |
| --- | --- |
| `jb_json_buffer_sink` | Przyjmuje `jb_json_buffer_t` jako user. Fragment mieści się w całości albo nic z niego nie zostaje skopiowane; `used` liczy przyjęte bajty. |
| `jb_json_count_sink` | Przyjmuje `size_t *` jako user. Liczy bajty z kontrolą przepełnienia, bez zapisywania wyniku. |

Pierwszy błąd zostaje zapamiętany: późniejsze operacje zwracają go bez wywołania
sinka. Wyjście może już zawierać początek dokumentu z wcześniejszych przyjętych fragmentów.
Nie ma ponawiania zapisu ani cofania transmisji. Jeśli aplikacja wymaga
sprawdzenia JSON przed wysłaniem, najpierw zbuduj wynik w buforze i sprawdź finish.

Przebieg liczący sprawdza te same operacje i nadal potrzebuje scratch. Drugi
przebieg musi użyć tych samych wartości, limitów i kolejności oraz sprawdzić
własny wynik; udane liczenie nie gwarantuje udanej transmisji.

Scratch, wyjście, bieżące wejście i zapisywany dokument nie mogą się nakładać.
Nie kopiuj aktywnego uchwytu: kopia dzieliłaby mutowalny scratch. Niezależne
writery mogą działać ze swoimi buforami i odpowiednio synchronizowanym sinkiem.
