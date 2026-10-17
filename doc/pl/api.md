# UTF-8 i JSON

jhbase sprawdza UTF-8, odczytuje wartości skalarne Unicode i udostępnia JSON
przez niemodyfikowalne węzły. Przenośna implementacja jest w C11 i korzysta
z pamięci wywołującego. Operacje tekstowe deklaruje
[utf8.h](../../include/jb/utf8.h), a JSON - [json.h](../../include/jb/json.h).
Oba nagłówki można również dołączać z C++17.

Dokładne konwersje i literały C opisuje [moduł liczb](numbers.md),
a zapis sekwencyjny - [writer](writer.md). JSON Pointer, JSON Schema i NFC
pozostają poza obecnym zakresem. Sposób kompilacji i ograniczenia platform
opisano w [dokumencie o testach](testing.md).

## Wspólne zasady

Długości i pojemności są podawane w bajtach, chyba że parametr jawnie liczy
tokeny lub elementy tablicy. Span nie oznacza stringa zakończonego NUL.
UTF-8 i zdekodowane stringi JSON mogą zawierać U+0000; korzystaj z ich
długości zamiast `strlen()`.

Wskaźniki wynikowe są wymagane, o ile opis nie mówi inaczej. Zwykłe wyjścia
pozostają bez zmian przy błędzie. Wyjątki to unieważnienie dokumentu przez
parser, diagnostyka i wymagane rozmiary podawane przez funkcje kopiowania
oraz enkoder. Wejście, tokeny, scratch i bufory wynikowe nie mogą się nakładać.

Niezależne instancje nie współdzielą modyfikowanego stanu. Można równolegle
czytać niemodyfikowalny dokument, jeśli jego wejście, tokeny i sam obiekt
dokumentu nadal istnieją i nie zmieniają się. Parsowanie, ponowne użycie
buforów i zmiany danych wymagają synchronizacji po stronie wywołującego.
Przykłady z buforami statycznymi mają jednego właściciela.

## UTF-8

Wartość skalarna Unicode (Unicode scalar value) to punkt kodowy od U+0000
do U+10FFFF, z wyłączeniem surrogatów U+D800-U+DFFF. Funkcje odrzucają kodowania nadmiarowe, błędne
continuation bytes i wartości spoza zakresu. Nie normalizują tekstu
i nie liczą klastrów grafemów.

### `jb_utf8_decode`

```c
hal_status_t jb_utf8_decode(const char *input, size_t length,
                           uint32_t *out_scalar, size_t *out_consumed);
```

Odczytuje pierwszą wartość skalarną z `input` i podaje liczbę zużytych bajtów,
od jednego do czterech. Nie sprawdza dalszych bajtów. Aby odczytać kolejną
wartość skalarną, przesuń wskaźnik o `out_consumed`.

`input` może być `NULL` tylko wtedy, gdy `length` wynosi zero. Oba wskaźniki wynikowe
są wymagane. `HAL_OK` udostępnia oba wyniki. `HAL_EPROTO` oznacza błędne lub
niepełne UTF-8, także puste wejście. `HAL_EINVAL` oznacza błędne wskaźniki.
Funkcja nie zachowuje stanu między wywołaniami; niepełną sekwencję trzeba
podać ponownie wraz z brakującymi bajtami.

### `jb_utf8_encode`

```c
hal_status_t jb_utf8_encode(uint32_t scalar, char *buffer, size_t capacity,
                           size_t *required);
```

Koduje jedną wartość skalarną bez terminatora. `required` otrzymuje długość kodowania
w bajtach i nie może być `NULL`. Dla `buffer == NULL` i `capacity == 0` funkcja
wykonuje sam pomiar i zwraca `HAL_OK`.

Rzeczywisty bufor musi pomieścić całe kodowanie. `HAL_EOVERFLOW` podaje
wymaganą długość, pozostawiając `buffer` bez zmian. `HAL_EINVAL` oznacza
surrogat, wartość ponad U+10FFFF, brak wskaźnika `required` albo brak bufora przy
niezerowej pojemności.

### `jb_utf8_validate`

```c
hal_status_t jb_utf8_validate(const char *input, size_t length,
                             size_t *error_offset);
```

Sprawdza dokładnie `length` bajtów, również za wewnętrznym NUL.
W odróżnieniu od decode obejmuje cały ciąg. Puste wejście, także (`NULL`, 0),
jest poprawne.

Wskaźnik `error_offset` jest opcjonalny. Przy `HAL_OK` otrzymuje `SIZE_MAX`,
a przy `HAL_EPROTO` `offset` bajtu rozpoczynającego pierwszą błędną
lub niepełną sekwencję. `HAL_EINVAL` oznacza brak `input` przy niezerowej
długości i pozostawia `error_offset` bez zmian.

**Przykład: walidacja tekstu i ponowne zakodowanie pierwszego znaku**

Dla bajtów E2 82 AC, po których występuje A, wynikiem jest trzybajtowy
znak euro. Błędny suffix przerywa walidację, nawet gdy pierwsza wartość skalarna jest
poprawna. Puste wejście przechodzi walidację, ale decode zwraca `HAL_EPROTO`.

```c
#include <jb/utf8.h>

/** Re-encode the first scalar after validating the entire input. */
hal_status_t example_first_scalar(const char *text, size_t length,
                                 char *output, size_t capacity,
                                 size_t *output_length, size_t *bad_offset) {
    hal_status_t status = jb_utf8_validate(text, length, bad_offset);
    if (status != HAL_OK) {
        return status;
    }

    uint32_t scalar;
    size_t consumed;
    status = jb_utf8_decode(text, length, &scalar, &consumed);
    if (status != HAL_OK) {
        return status;
    }

    size_t required;
    status = jb_utf8_encode(scalar, NULL, 0, &required);
    if (status != HAL_OK) {
        return status;
    }
    if (required > capacity) {
        return HAL_EOVERFLOW;
    }
    return jb_utf8_encode(scalar, output, capacity, output_length);
}
```

## Parsowanie

### Typy i pamięć

| Typ | Przeznaczenie |
| --- | --- |
| `jb_json_span_t` | Widok danych wywołującego: wskaźnik i długość, bez terminatora. |
| `jb_json_workspace_t` | Wskaźnik do modyfikowanego scratch i rozmiar w bajtach; `NULL` tylko przy zerowym rozmiarze. |
| `jb_json_token_t` | Rekord wartości, jej zakresu w źródle i powiązań w drzewie; zapisuje go parser. |
| `jb_json_doc_t` | Niemodyfikowalny widok wejścia i tokenów; `valid` ustawia tylko udany parse. |
| `jb_json_node_t` | Odwołanie do obiektu dokumentu i indeksu tokenu. |
| `jb_json_limits_t` | Skończone limity wejścia, kontenerów i pracy porównań. |
| `jb_json_requirements_t` | Liczba wartości i maksymalna głębokość otwartych kontenerów z measure. |
| `jb_json_error_t` | Przyczyna błędu, `offset`, powiązana lokalizacja i znane wymaganie pojemności. |

W JSON wartość prosta oznacza null, boolean, liczbę lub string.
Token reprezentuje wartość. Na przykład `{"a": 1}` wymaga dwóch tokenów:
obiektu oraz wartości jego pola. Token wartości zawiera też nazwę pola.
Tokeny występują w kolejności preorder, a root ma indeks zero. Brak rodzica,
pierwszego dziecka lub następnego rodzeństwa oznacza `JB_JSON_NODE_NONE`,
równe `SIZE_MAX`.

Pola `offset` i `length` określają zakres z delimiterami wartości,
np. cudzysłowami stringa i nawiasami kontenera. Zakres określony przez
`key_offset` i `key_length` nie obejmuje cudzysłowów nazwy.
`has_key` odróżnia pustą nazwę pola od wartości bez nazwy. Nie zmieniaj pól
tokenów.

Wejście i tokeny muszą istnieć i pozostać niezmienione podczas używania
dokumentu. Węzeł odwołuje się również do samego obiektu dokumentu:
nie można zwrócić węzła wskazującego lokalny dokument. Ponowne użycie tablicy
tokenów unieważnia wcześniejsze dokumenty, które z niej korzystają.
Biblioteka nie udostępnia destroy dla pamięci należącej do wywołującego.

Scratch parsera można wykorzystać ponownie od razu po parsowaniu.
Może to być statyczna tablica `unsigned char` o dowolnym wyrównaniu.
Rekordy są kopiowane przez `memcpy`, bez rzutowania scratch na wskaźnik
do struktury.

### Limity

Każde pole `jb_json_limits_t` określa skończony limit. Zero zawsze dopuszcza
zero danych lub pracy; nie oznacza braku ograniczeń.

| Pole | Co ogranicza |
| --- | --- |
| `max_input_bytes` | Całe wejście, także whitespace przed wartością i po niej. |
| `max_nodes` | Wszystkie tokeny wartości, łącznie z root. |
| `max_depth` | Jednocześnie otwarte tablice i obiekty; wartość prosta jako root ma głębokość zero. |
| `max_string_bytes` | Zdekodowane bajty UTF-8 jednego stringa lub nazwy pola. |
| `max_number_bytes` | Bajty jednego oryginalnego tokenu liczby. |
| `max_members` | Bezpośrednie dzieci każdej tablicy lub obiektu. |
| `max_key_compare_bytes` | Pracę porównań wspólną dla wszystkich obiektów w danym parse. |

Każde porównanie kluczy kosztuje jedną jednostkę oraz faktycznie porównane
zdekodowane bajty obu nazw. Porównanie pustych nazw również kosztuje jedną
jednostkę. Wykrywanie duplikatów przegląda wcześniejsze rodzeństwo i może
mieć koszt kwadratowy; ograniczają go limity dzieci i porównań. Odczyty,
np. find, nie mają osobnego budżetu pracy.

### `jb_json_parse_workspace_size`

```c
hal_status_t jb_json_parse_workspace_size(const jb_json_limits_t *limits,
                                         size_t *out_bytes);
```

Wyznacza pojemność scratch w bajtach dla `limits->max_depth`.
Oba wskaźniki są wymagane. `HAL_OK` zapisuje `out_bytes`, `HAL_EINVAL` odrzuca
brak wskaźnika, a `HAL_EOVERFLOW` oznacza przepełnienie obliczeń rozmiaru.

Taką pojemność trzeba dostarczyć do measure lub parse, nawet gdy konkretne
wejście jest płytsze od ustawionego limitu. Przy `max_depth == 0` scratch
nie jest potrzebny, a akceptowane mogą być tylko wartości proste jako root.
Tablica tokenów wymaga osobnej pamięci.

### `jb_json_parse`

```c
hal_status_t jb_json_parse(jb_json_span_t input, const jb_json_limits_t *limits,
                          jb_json_token_t *tokens, size_t token_capacity,
                          jb_json_workspace_t work, jb_json_doc_t *out_doc,
                          jb_json_error_t *error);
```

Parsuje dokładnie jedną pełną wartość z opcjonalnym otaczającym whitespace
JSON. Akceptuje także scalar jako root. Odrzuca komentarze, trailing commas,
BOM, NaN, Infinity, błędne UTF-8, escape'y, nieparzyste surrogaty i dodatkowe
dane. Nazwy pól muszą być unikalne po zdekodowaniu escape'ów:
`"a"` i `"\u0061"` kolidują w jednym obiekcie. Nazwy nie są normalizowane do NFC.

`limits` i `out_doc` są wymagane, a `error` opcjonalne. `tokens` może być `NULL`
tylko przy zerowej pojemności, która nie pomieści poprawnego dokumentu.
`token_capacity` liczy rekordy, nie bajty: używaj `COUNTOF(tokens)` z nagłówka
tablic HAL. Wejście i scratch dopuszczają `NULL` tylko przy zerowej długości
lub zerowym rozmiarze.

`HAL_OK` udostępnia poprawny dokument. `HAL_EPROTO` oznacza odrzucone wejście,
`HAL_EOVERFLOW` brak pamięci w przekazanych buforach lub przekroczenie limitu,
a `HAL_EINVAL` błędne argumenty. Każdy błąd unieważnia `out_doc`; tokeny mogą
zawierać częściowy wynik. Nie wykonuj na nim nawigacji.

**Przykład: odczyt wartości boolean z pamięcią statyczną**

Dla `{"enabled": true}` i jego długości w bajtach funkcja zapisuje `true`
w `out_enabled`. Brak pola daje `HAL_ENOENT`, a `{"enabled": "true"}` daje
`HAL_ETYPE`. Helper zwraca wyłącznie skopiowany boolean, więc nie udostępnia
węzła zależnego od lokalnego dokumentu. Przed parsowaniem sprawdza
zapotrzebowanie implementacji na scratch.

```c
#include <jb/json.h>
#include <hal/core/hal_array.h>

/** Read a boolean field using fixed, single-owner parser storage. */
hal_status_t example_read_enabled(const char *data, size_t length,
                                 bool *out_enabled) {
    static jb_json_token_t tokens[16];
    static unsigned char scratch[1024];
    const jb_json_limits_t limits = {
        512, COUNTOF(tokens), 4, 128, 64, 8, 1024
    };
    if (data == NULL || out_enabled == NULL) {
        return HAL_EINVAL;
    }

    size_t scratch_bytes;
    hal_status_t status = jb_json_parse_workspace_size(&limits, &scratch_bytes);
    if (status != HAL_OK) {
        return status;
    }
    if (scratch_bytes > sizeof(scratch)) {
        return HAL_EOVERFLOW;
    }

    const jb_json_span_t input = {data, length};
    const jb_json_workspace_t work = {scratch, sizeof(scratch)};
    jb_json_doc_t doc;
    status = jb_json_parse(input, &limits, tokens, COUNTOF(tokens),
                           work, &doc, NULL);
    if (status != HAL_OK) {
        return status;
    }

    jb_json_node_t root;
    jb_json_node_t enabled = {NULL, 0}; /* Set only on success. */
    const jb_json_span_t key = {"enabled", 7};
    status = jb_json_root(&doc, &root);
    if (status == HAL_OK) {
        status = jb_json_find(root, key, &enabled);
    }
    if (status == HAL_OK) {
        status = jb_json_get_bool(enabled, out_enabled);
    }
    return status;
}
```

### `jb_json_measure`

```c
hal_status_t jb_json_measure(jb_json_span_t input, const jb_json_limits_t *limits,
                            jb_json_workspace_t work,
                            jb_json_requirements_t *out,
                            jb_json_error_t *error);
```

Sprawdza składnię, Unicode i limity strukturalne, a potem podaje liczbę
potrzebnych tokenów wartości oraz maksymalną głębokość. `limits` i `out`
są wymagane, `error` jest opcjonalne. Scratch musi spełniać te same wymagania
co przy parse.

Pomiar nie sprawdza duplikatów nazw ani nie zużywa budżetu ich porównań.
Służy do doboru pamięci; dokument nadal musi przejść parse. `HAL_EINVAL`,
`HAL_EPROTO` i `HAL_EOVERFLOW` mają znaczenie takie jak przy parse.
Wynikowe wymagania zmieniają się wyłącznie przy sukcesie.

**Przykład: pomiar i alokacja tokenów na hoście**

Helper alokuje tylko tokeny; scratch pozostaje statyczny i ma jednego
właściciela. Przed `malloc` sprawdza mnożenie rozmiarów, po użyciu zwalnia
tokeny i udostępnia liczbę dopiero po udanym parse. Dokument z duplikatami
może przejść measure, a następnie nie przejść parse. W firmware zwykle
wystarcza stała tablica tokenów jak w poprzednim przykładzie.

```c
#include <jb/json.h>
#include <stdlib.h>

/** Host-only allocation: publish the count only after a complete parse. */
hal_status_t example_measured_parse(const char *data, size_t length,
                                   size_t *out_nodes) {
    static unsigned char scratch[1024];
    const jb_json_limits_t limits = {4096, 256, 8, 1024, 128, 64, 8192};
    if (out_nodes == NULL) {
        return HAL_EINVAL;
    }

    size_t scratch_bytes;
    hal_status_t status = jb_json_parse_workspace_size(&limits, &scratch_bytes);
    if (status != HAL_OK) {
        return status;
    }
    if (scratch_bytes > sizeof(scratch)) {
        return HAL_EOVERFLOW;
    }

    const jb_json_span_t input = {data, length};
    const jb_json_workspace_t work = {scratch, sizeof(scratch)};
    jb_json_requirements_t needed;
    status = jb_json_measure(input, &limits, work, &needed, NULL);
    if (status != HAL_OK) {
        return status;
    }
    if (needed.node_count > SIZE_MAX / sizeof(jb_json_token_t)) {
        return HAL_EOVERFLOW;
    }
    jb_json_token_t *tokens =
        (jb_json_token_t *)malloc(needed.node_count * sizeof(jb_json_token_t));
    if (tokens == NULL) {
        return HAL_ENOMEM;
    }

    jb_json_doc_t doc;
    status = jb_json_parse(input, &limits, tokens, needed.node_count,
                           work, &doc, NULL);
    if (status == HAL_OK) {
        *out_nodes = doc.token_count;
    }
    free(tokens);
    return status;
}
```

## Pamięć i odczyt

Poniższe funkcje odczytują poprawnie sparsowany dokument. O ile opis nie mówi
inaczej, niepoprawny dokument lub indeks węzła daje `HAL_ESTATE`, brak
wskaźnika wynikowego `HAL_EINVAL`, a błąd pozostawia wyjścia bez zmian.
Węzeł należy do swojego dokumentu; indeksu tokenu nie można przenosić
między dokumentami.

### `jb_json_root`

```c
hal_status_t jb_json_root(const jb_json_doc_t *doc, jb_json_node_t *out);
```

Zwraca root, także dla wartości prostej. Oba wskaźniki są wymagane.
Niepoprawny lub pusty dokument daje `HAL_ESTATE`. Sam obiekt `doc` musi
pozostać żywy przez cały czas używania uzyskanego węzła.

### `jb_json_type`

```c
hal_status_t jb_json_type(jb_json_node_t node, jb_json_type_t *out);
```

Podaje jeden z typów `JB_JSON_NULL`, `JB_JSON_BOOL`, `JB_JSON_NUMBER`,
`JB_JSON_STRING`, `JB_JSON_ARRAY` lub `JB_JSON_OBJECT`. Wskaźnik `out` jest wymagany.
Sprawdzenie typu nie konwertuje wartości ani nie zmienia dokumentu.

### `jb_json_count`

```c
hal_status_t jb_json_count(jb_json_node_t node, size_t *out);
```

Podaje liczbę bezpośrednich pól obiektu albo elementów tablicy.
Nie liczy dalszych potomków. Wskaźnik `out` jest wymagany. Wartość prosta daje `HAL_ETYPE`,
a pusty kontener poprawny wynik zero.

### `jb_json_first`

```c
hal_status_t jb_json_first(jb_json_node_t container, jb_json_node_t *out);
```

Zwraca pierwsze pole lub element w kolejności wejścia. Wskaźnik `out` jest wymagany.
Pusty kontener daje `HAL_ENOENT`, a wartość prosta `HAL_ETYPE`.

### `jb_json_next`

```c
hal_status_t jb_json_next(jb_json_node_t node, jb_json_node_t *out);
```

Zwraca następne rodzeństwo z pominięciem potomków bieżącego węzła.
Wskaźnik `out` jest wymagany. Koniec rodzeństwa, także dla root bez rodzeństwa, daje
`HAL_ENOENT`. Iteracja first/next po kontenerze ma łączny koszt liniowy.

### `jb_json_find`

```c
hal_status_t jb_json_find(jb_json_node_t object, jb_json_span_t key,
                         jb_json_node_t *out);
```

Wyszukuje dokładną zdekodowaną nazwę pola obiektu z rozróżnieniem wielkości
liter. `key` zawiera zwykłe bajty UTF-8, bez składni escape JSON i bez
wyrażenia ścieżki. (`NULL`, 0) oznacza pustą nazwę. Wewnętrzny NUL ma
znaczenie. Zapisane `"\u0061"` znajdziesz na przykład jednobajtowym kluczem `"a"`.

Wskaźnik `out` jest wymagany. Brak nazwy daje `HAL_ENOENT`, wartość inna niż obiekt
daje `HAL_ETYPE`, błędny wskaźnik/długość `HAL_EINVAL`, a błędne UTF-8 klucza `HAL_EPROTO`.
Funkcja przegląda pola i porównuje nazwy bez NFC ani case folding.
Wywołujący powinien ograniczać liczbę powtarzanych wyszukiwań w dużych
obiektach.

**Przykład: podsumowanie tablicy boolean**

Dla `{"flags": [true, false, true]}` helper podaje łącznie 3 elementy,
w tym 2 `true`. Dla pustej tablicy oba wyniki wynoszą zero. Element innego
typu przerywa działanie bez udostępnienia wyników. `HAL_ENOENT` z first/next
oznacza normalny koniec iteracji, a `HAL_ENOENT` z find brak wymaganego
pola `"flags"`.

```c
#include <jb/json.h>

/** Count all items and true values in the "flags" array. */
hal_status_t example_flags_summary(const jb_json_doc_t *doc,
                                  size_t *out_total, size_t *out_true) {
    if (out_total == NULL || out_true == NULL) {
        return HAL_EINVAL;
    }
    jb_json_node_t root;
    jb_json_node_t flags = {NULL, 0}, item = {NULL, 0};
    const jb_json_span_t key = {"flags", 5};
    hal_status_t status = jb_json_root(doc, &root);
    if (status == HAL_OK) {
        status = jb_json_find(root, key, &flags);
    }
    if (status != HAL_OK) {
        return status;
    }
    jb_json_type_t type;
    status = jb_json_type(flags, &type);
    if (status != HAL_OK) {
        return status;
    }
    if (type != JB_JSON_ARRAY) {
        return HAL_EINVAL;
    }

    size_t total, true_count = 0;
    status = jb_json_count(flags, &total);
    if (status != HAL_OK) {
        return status;
    }
    status = jb_json_first(flags, &item);
    while (status == HAL_OK) {
        bool value;
        status = jb_json_get_bool(item, &value);
        if (status != HAL_OK) {
            return status;
        }
        if (value) {
            ++true_count;
        }
        status = jb_json_next(item, &item);
    }
    if (status != HAL_ENOENT) {
        return status;
    }
    *out_total = total;
    *out_true = true_count;
    return HAL_OK;
}
```

### `jb_json_at`

```c
hal_status_t jb_json_at(jb_json_node_t array, size_t position,
                       jb_json_node_t *out);
```

Zwraca element tablicy o indeksie liczonym od zera. Wskaźnik `out` jest wymagany.
Indeks poza zakresem daje `HAL_ENOENT`, a wartość inna niż tablica `HAL_ETYPE`.
Funkcja przegląda wcześniejsze rodzeństwo, więc koszt jest liniowy względem
`position`. Do pełnej iteracji używaj first/next zamiast powtarzanych at.

**Przykład: wybór jednego boolean**

Dla `{"flags": [true, false]}` pozycja 1 daje `false`, a pozycja 2 `HAL_ENOENT`.

```c
#include <jb/json.h>

/** Read one zero-based element of the "flags" array. */
hal_status_t example_flag_at(const jb_json_doc_t *doc, size_t position,
                            bool *out_value) {
    jb_json_node_t root;
    jb_json_node_t flags = {NULL, 0}, item = {NULL, 0};
    const jb_json_span_t key = {"flags", 5};
    hal_status_t status = jb_json_root(doc, &root);
    if (status == HAL_OK) {
        status = jb_json_find(root, key, &flags);
    }
    if (status == HAL_OK) {
        status = jb_json_at(flags, position, &item);
    }
    if (status == HAL_OK) {
        status = jb_json_get_bool(item, out_value);
    }
    return status;
}
```

### `jb_json_raw`

```c
hal_status_t jb_json_raw(jb_json_node_t node, jb_json_span_t *out);
```

Udostępnia oryginalne bajty JSON dowolnej wartości, bez otaczającego
whitespace. Wskaźnik `out` jest wymagany. Span stringa obejmuje cudzysłowy i escape'y,
a span kontenera całą jego zawartość i delimitery. Funkcja nie alokuje
pamięci, nie dekoduje i nie dopisuje terminatora. Wynik jest ważny tak
długo jak wejście.

### `jb_json_number_token`

```c
hal_status_t jb_json_number_token(jb_json_node_t node, jb_json_span_t *out);
```

Udostępnia oryginalny zapis liczby. Wskaźnik `out` jest wymagany. Inny typ,
także boolean lub string z liczbą, daje `HAL_ETYPE`.

Funkcja nie konwertuje do integer ani typu zmiennoprzecinkowego.
18446744073709551615, 9007199254740993.0 i 1e400 pozostają dokładnymi
bajtami wejścia, jeśli mieszczą się w jego limitach. Funkcja nie rozstrzyga,
czy token mieści się w konkretnym typie liczbowym C.

**Przykład: zachowanie zapisu identyfikatora liczbowego**

Dla `{"id": 1.8446744073709551615e19}` helper kopiuje dokładnie taki zapis.
Wynik ma jawną długość i nie ma terminatora. Span jest używany tylko wtedy,
gdy `doc` i jego wejście nadal istnieją.

```c
#include <jb/json.h>
#include <string.h>

/** Copy the original numeric spelling of "id", without a terminator. */
hal_status_t example_number_text(const jb_json_doc_t *doc, char *output,
                                size_t capacity, size_t *out_length) {
    if (output == NULL || out_length == NULL) {
        return HAL_EINVAL;
    }
    jb_json_node_t root;
    jb_json_node_t id = {NULL, 0};
    const jb_json_span_t key = {"id", 2};
    hal_status_t status = jb_json_root(doc, &root);
    if (status == HAL_OK) {
        status = jb_json_find(root, key, &id);
    }
    if (status != HAL_OK) {
        return status;
    }
    jb_json_span_t number;
    status = jb_json_number_token(id, &number);
    if (status != HAL_OK) {
        return status;
    }
    if (number.length > capacity) {
        return HAL_EOVERFLOW;
    }
    memcpy(output, number.data, number.length);
    *out_length = number.length;
    return HAL_OK;
}
```

### `jb_json_get_bool`

```c
hal_status_t jb_json_get_bool(jb_json_node_t node, bool *out);
```

Odczytuje boolean JSON do wymaganego `out`. Akceptuje wyłącznie `true` i `false`.
Liczby, stringi i `null` dają `HAL_ETYPE`; nie ma niejawnej konwersji
na wartość logiczną. Funkcję pokazują przykłady parsowania i tablic.

### `jb_json_string_copy`

```c
hal_status_t jb_json_string_copy(jb_json_node_t node, char *buffer,
                                size_t capacity, size_t *required);
```

Dekoduje string do bajtów UTF-8, bez cudzysłowów i terminatora.
Rozwija escape'y i pary surrogate; `"\u0000"` daje wewnętrzny NUL.
Wskaźnik `required` jest wymagany; podana długość nie obejmuje terminatora.

(`NULL`, 0) wykonuje pomiar i daje `HAL_OK`. Za mały rzeczywisty bufor daje
`HAL_EOVERFLOW` i pozostaje bez zmian, a `required` podaje pełny rozmiar.
Wartość inna niż string daje `HAL_ETYPE`. Brak `required` lub `NULL` `buffer`
przy niezerowej pojemności daje `HAL_EINVAL`.

Nie wypisuj wyniku przez `%s`; zdekodowane bajty mogą zawierać NUL i nie mają
terminatora. String C tworzy opisana niżej `jb_json_string_cstr`, gdy wartość
na to pozwala. Pusty string wymaga zero bajtów danych; funkcja nie czyści
automatycznie bufora.

### `jb_json_key_copy`

```c
hal_status_t jb_json_key_copy(jb_json_node_t member, char *buffer,
                             size_t capacity, size_t *required);
```

Dekoduje nazwę pola według tych samych zasad pojemności, pomiaru i NUL
co string_copy. Wskaźnik `required` jest wymagany. Sama wartość pola może mieć dowolny
typ JSON; musi jedynie posiadać nazwę w obiekcie. Root i element tablicy
nie mają nazwy i dają `HAL_EINVAL`. Pusta nazwa jest poprawna i wymaga
zero bajtów.

**Przykład: porównanie oryginalnego JSON i zdekodowanego tekstu**

Dla `{"label": "Hi\n"}` raw obejmuje cudzysłowy i dwubajtowy escape,
a string_copy daje H, i oraz LF. Przykład działa na hoście i wypisuje
dane z jawną liczbą bajtów, więc `"\u0000"` nie skróci wyniku.
Lokalne bufory ograniczają nazwę do 32 bajtów, a wartość do 64;
większy wynik daje `HAL_EOVERFLOW` przed wypisaniem czegokolwiek.

```c
#include <jb/json.h>
#include <stdio.h>

/** Host-only display of the key, original JSON and decoded label bytes. */
hal_status_t example_print_label(const jb_json_doc_t *doc) {
    jb_json_node_t root;
    jb_json_node_t label = {NULL, 0};
    const jb_json_span_t key = {"label", 5};
    hal_status_t status = jb_json_root(doc, &root);
    if (status == HAL_OK) {
        status = jb_json_find(root, key, &label);
    }
    if (status != HAL_OK) {
        return status;
    }

    jb_json_span_t raw;
    char name[32], value[64];
    size_t name_bytes = 0, value_bytes = 0;
    status = jb_json_raw(label, &raw);
    if (status == HAL_OK) {
        status = jb_json_key_copy(label, name, sizeof(name), &name_bytes);
    }
    if (status == HAL_OK) {
        status = jb_json_string_copy(label, NULL, 0, &value_bytes);
    }
    if (status == HAL_OK) {
        status = jb_json_string_copy(label, value, sizeof(value), &value_bytes);
    }
    if (status != HAL_OK) {
        return status;
    }

    if (fwrite(name, 1, name_bytes, stdout) != name_bytes ||
        fputs(": ", stdout) == EOF ||
        fwrite(raw.data, 1, raw.length, stdout) != raw.length ||
        fputs(" -> ", stdout) == EOF ||
        fwrite(value, 1, value_bytes, stdout) != value_bytes) {
        return HAL_EIO;
    }
    return HAL_OK;
}
```

### `jb_json_string_equal`

```c
hal_status_t jb_json_string_equal(jb_json_node_t node, jb_json_span_t value,
                                 bool *out);
```

Porównuje string z bajtami UTF-8 po rozwinięciu escape'ów i par surrogate,
bez kopiowania, NFC ani case folding. `value` nie jest składnią JSON:
zapisane `"a"` równa się jednobajtowej wartości `a`. Może zawierać NUL,
a (`NULL`, 0) oznacza pusty string. Wskaźnik `out` jest wymagany i otrzymuje
`true` tylko dla identycznych zdekodowanych bajtów.

Wartość inna niż string daje `HAL_ETYPE`. Błędne UTF-8 w dowolnym miejscu
`value` daje `HAL_EPROTO`, także po wcześniejszej różnicy. `NULL` w
`value.data` przy niezerowej długości lub brak `out` daje `HAL_EINVAL`.
Przy błędzie `out` pozostaje bez zmian.

### `jb_json_string_cstr`

```c
hal_status_t jb_json_string_cstr(jb_json_node_t node, char *buffer,
                                size_t capacity, size_t *required);
```

Dekoduje string i dopisuje terminator NUL. Wskaźnik `required` jest wymagany,
a podana długość obejmuje terminator, więc pusty string wymaga jednego bajtu.
(`NULL`, 0) wykonuje pomiar i daje `HAL_OK`. Za mały rzeczywisty bufor daje
`HAL_EOVERFLOW`, pozostaje bez zmian, a `required` podaje pełny rozmiar.

Wartość zawierająca U+0000 nie może być stringiem C. Daje `HAL_ETYPE`
i pozostawia oba wyjścia bez zmian zamiast cichego skrócenia; takie dane
czytaj przez `jb_json_string_copy`. Wartość inna niż string również daje
`HAL_ETYPE`. Brak `required` lub `NULL` `buffer` przy niezerowej pojemności
daje `HAL_EINVAL`.

## Wyniki i ABI

### Status operacji

| Status | Znaczenie |
| --- | --- |
| `HAL_OK` | Operacja zakończona; można korzystać z jej wyników. |
| `HAL_EINVAL` | Błędne argumenty. |
| `HAL_ETYPE` | Odczyt niewłaściwego typu JSON lub wartości bez żądanej dokładnej reprezentacji. |
| `HAL_EPROTO` | Błędny JSON, UTF-8/escape'y, powtórzone nazwy lub dodatkowe dane. |
| `HAL_EOVERFLOW` | Brak pojemności, przekroczony limit danych/pracy lub przepełnienie rozmiaru. |
| `HAL_ESTATE` | Niepoprawny dokument albo węzeł. |
| `HAL_ENOENT` | Brak pola/elementu lub koniec iteracji. |

Wszystkie wyniki używają istniejącego `hal_status_t` z
[status.h](../../include/jb/status.h). `HAL_ETYPE` pochodzi z tego wspólnego
nagłówka; prototyp nie definiuje własnych kodów wyników.
Przykład alokacji na hoście może dodatkowo zwrócić `HAL_ENOMEM`, a przykłady
wypisywania `HAL_EIO`. To błędy alokacji lub wyjścia w przykładach;
przenośny parser nie wykonuje tych operacji.

### Diagnostyka parsera

`error` może być `NULL`, gdy wystarczy sam status. W przeciwnym razie parse
i measure wypełniają poniższe pola:

| Pole | Znaczenie |
| --- | --- |
| `code` | Jedna z przyczyn poniżej; `JB_JSON_ERROR_NONE` przy sukcesie lub błędzie argumentów. |
| `offset` | Offset bajtowy wejścia; EOF to `input.length`. Duplikat wskazuje późniejszy otwierający cudzysłów. |
| `related_offset` | Otwierający cudzysłów wcześniejszej nazwy przy duplikacie; poza tym `SIZE_MAX`. |
| `required_capacity` | Znana liczba potrzebnych tokenów lub bajtów scratch; zero, jeśli nieznana. |

| Przyczyna | Znaczenie |
| --- | --- |
| `JB_JSON_ERROR_NONE` | Brak diagnostyki parsera. |
| `JB_JSON_ERROR_SYNTAX` | Błędna składnia JSON, literału lub liczby. |
| `JB_JSON_ERROR_UTF8` | Błędne UTF-8 w stringu lub nazwie. |
| `JB_JSON_ERROR_ESCAPE` | Błędny escape lub sekwencja surrogate. |
| `JB_JSON_ERROR_DUPLICATE_KEY` | Dwie zdekodowane nazwy kolidują w jednym obiekcie. |
| `JB_JSON_ERROR_TRAILING_DATA` | Po pełnym root występują dane inne niż whitespace. |
| `JB_JSON_ERROR_LIMIT` | Przekroczony limit danych lub pracy. |
| `JB_JSON_ERROR_TOKEN_CAPACITY` | Tablica tokenów nie pomieści następnej wartości. |
| `JB_JSON_ERROR_WORKSPACE` | Za mały scratch albo przepełnienie obliczeń jego rozmiaru. |

`required_capacity` nie zawsze opisuje cały dokument. Błąd pojemności
tokenów może podać tylko liczbę potrzebną do zapisania następnego rekordu.
Do doboru całej tablicy służy measure, a do akceptacji dokumentu parse.

### `jb_json_locate`

```c
hal_status_t jb_json_locate(jb_json_span_t input, size_t offset,
                           size_t *out_line, size_t *out_column);
```

Przelicza `offset` bajtowy, także EOF, na linię i kolumnę bajtową liczone
od jedynki. Tylko LF rozpoczyna nową linię; CR jest liczony jako bajt.
Kolumna nie jest pozycją na ekranie ani liczbą znaków Unicode.
Funkcja przegląda prefix, nie wymagając poprawnego JSON ani UTF-8.

Oba wyjścia są wymagane. `HAL_EINVAL` oznacza `offset` za końcem wejścia,
brak `input` przy niezerowej długości albo brak wskaźnika wynikowego. Przepełnienie
licznika daje `HAL_EOVERFLOW`. Przy błędzie oba wyjścia pozostają bez zmian.

**Przykład: lokalizacja błędu JSON**

Po trailing comma w poniższym wejściu występuje zamykający nawias obiektu
w linii 3, kolumnie bajtowej 1. Helper wypisuje tę lokalizację i zwraca
oryginalny `HAL_EPROTO`. Przy duplikatach można osobno przekazać
`related_offset` do locate.

```c
#include <jb/json.h>
#include <hal/core/hal_array.h>
#include <stdio.h>

/** Show a byte location for a trailing-comma error. */
hal_status_t example_json_error(void) {
    static const char text[] = "{\n  \"x\": 1,\n}";
    static jb_json_token_t tokens[8];
    static unsigned char scratch[1024];
    const jb_json_limits_t limits = {
        sizeof(text) - 1, COUNTOF(tokens), 2, 16, 16, 4, 64
    };
    size_t needed;
    hal_status_t status = jb_json_parse_workspace_size(&limits, &needed);
    if (status != HAL_OK) {
        return status;
    }
    if (needed > sizeof(scratch)) {
        return HAL_EOVERFLOW;
    }

    const jb_json_span_t input = {text, sizeof(text) - 1};
    const jb_json_workspace_t work = {scratch, sizeof(scratch)};
    jb_json_doc_t doc;
    jb_json_error_t error;
    const hal_status_t parsed = jb_json_parse(
        input, &limits, tokens, COUNTOF(tokens), work, &doc, &error);
    if (parsed != HAL_OK && error.code != JB_JSON_ERROR_NONE) {
        size_t line, column;
        status = jb_json_locate(input, error.offset, &line, &column);
        if (status != HAL_OK) {
            return status;
        }
        if (printf("JSON error %u at %zu:%zu\n",
                   (unsigned)error.code, line, column) < 0) {
            return HAL_EIO;
        }
    }
    return parsed;
}
```

### Układ struktur i profil buildu

Prototyp używa naturalnego układu struktur, 32/64-bitowych wskaźników
i `size_t`, jednobajtowego `bool` oraz 32-bitowego enuma statusów.
CMake przekazuje `HAL_STATUS_FIXED_32`, ustalając 32-bitowy enum statusów
w GCC/Clang bez zmiany innych enumów i globalnych atrybutów ABI toolchaina.
Przy bezpośredniej kompilacji definiuj go spójnie przed pierwszym include
statusów HAL. MSVC używa natywnego enuma 32-bitowego.

Asercje nagłówków odrzucają niezgodne pakowanie, także rozpoczęte przed
pierwszym include. Token zajmuje 40 bajtów w profilu 32-bitowym i 80
w 64-bitowym. Te struktury nie są formatem pliku ani transmisji.

Moduły hostowe mogą korzystać z C++17 i heap za API C; każdy wyjątek musi
zostać przetłumaczony na status HAL przed przekroczeniem granicy C.
Obecne przenośne moduły UTF-8 i JSON nie alokują pamięci.
