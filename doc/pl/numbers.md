# Dokładne liczby i literały C

Do odczytu liczb dołącz [json_number.h](../../include/jb/json_number.h), a do
zapisu C — [literal.h](../../include/jb/literal.h). Funkcje przyjmują węzły
z poprawnie sparsowanego dokumentu; wejście i tokeny muszą pozostać dostępne
i niezmienione. Parser zachowuje oryginalne tokeny liczb, także wartości
spoza zakresu natywnych integerów i typów zmiennoprzecinkowych.

## Odczyt integerów

| Funkcja | Wynik |
| --- | --- |
| `jb_json_get_u64(node, out)` | Odczytuje dokładny integer jako `uint64_t`. Ujemna wartość niezerowa i wartość powyżej `UINT64_MAX` dają `HAL_EOVERFLOW`. |
| `jb_json_get_i64(node, out)` | Odczytuje dokładny integer jako `int64_t`, także `INT64_MIN`; przekroczenie zakresu daje `HAL_EOVERFLOW`. |
| `jb_json_number_is_integer(node, out)` | Sprawdza, czy wartość dziesiętna jest całkowita, nawet gdy nie mieści się w natywnym integerze. |

Wszystkie wskaźniki wynikowe są wymagane. `HAL_ETYPE` oznacza niewłaściwy typ
JSON lub próbę odczytu wartości ułamkowej jako integera. `HAL_EINVAL` oznacza
błędne argumenty, a `HAL_ESTATE` niepoprawny węzeł. Wyjścia zmieniają się
wyłącznie po sukcesie.

`100`, `100.0` i `1e2` dają integer 100. `1.5` zostaje odrzucone bez
zaokrąglania. `-0`, także z ogromnym wykładnikiem, daje zero. Nie ma niejawnej
konwersji booleanów ani stringów. Odczyty nie potrzebują dodatkowego scratch;
czas przeglądania ogranicza limit długości tokenu liczby w parserze.

```c
uint64_t identifier;
hal_status_t status = jb_json_get_u64(id_node, &identifier);
if (status != HAL_OK) {
    return status;
}
```

Kompletny [przykład liczbowy](../../examples/json_numbers.c) odczytuje pełny
zakres unsigned 64-bit bez wcześniejszej zamiany na `double`.

## Dokładne porównanie

`jb_json_number_compare_workspace_size(left, right, out_bytes)` oblicza scratch
dla wskazanych tokenów. Przed zapisaniem wyniku sprawdza przepełnienie rozmiaru.
`jb_json_number_compare(left, right, work, max_work, out_order)` zapisuje -1, 0
lub 1 zgodnie z dokładnym porządkiem wartości dziesiętnych.

```c
size_t needed;
hal_status_t status = jb_json_number_compare_workspace_size(left, right, &needed);
if (status != HAL_OK) {
    return status;
}
if (needed > work.size) {
    return HAL_EOVERFLOW;
}
int order;
status = jb_json_number_compare(left, right, work, 1000000, &order);
```

Porównanie rozróżnia `9007199254740992.0` i `9007199254740993.0`, a `1`, `1.0`
i `1e0` uznaje za równe. Ogromne wykładniki są porównywane bez rozwijania ich
wartości w olbrzymich buforach. Scratch może być niewyrównaną pamięcią bajtową.
Scratch i wynik nie mogą nakładać się na siebie ani na oba dokumenty.
Brak scratch lub wyczerpanie budżetu daje `HAL_EOVERFLOW`; zero nie dopuszcza
żadnej pracy. Liczony jest każdy krok na cyfrze lub limbie. `out_order`
zmienia się wyłącznie po sukcesie.

## Konwersja IEEE

`jb_json_float_workspace_size(node, bit_width, out_bytes)` przyjmuje szerokość
32 lub 64 i oblicza scratch dla tokenu. Cztery poniższe funkcje przyjmują
`(node, options, work, out)`; wskaźniki opcji i wyniku są wymagane:

| Funkcja | Wynik |
| --- | --- |
| `jb_json_get_f32_bits` | IEEE binary32 w `uint32_t`, zaokrąglone bezpośrednio z dokładnej wartości dziesiętnej. |
| `jb_json_get_f64_bits` | IEEE binary64 w `uint64_t`. |
| `jb_json_get_f32` | Natywny `float` z tym samym wynikiem binary32. |
| `jb_json_get_f64` | Natywny `double` z tym samym wynikiem binary64. |

Zaokrąglanie wybiera najbliższą wartość, a przy remisie parzystą mantysę,
niezależnie od locale i bieżącego trybu FPU. Odczyty natywne kopiują
reprezentację po sprawdzeniu formatu IEEE podczas kompilacji. Obsługiwane są
wartości subnormalne. Przepełnienie do infinity zawsze daje `HAL_EOVERFLOW`;
wyjścia pozostają bez zmian.

`jb_json_float_options_t` określa dwie niezależne zasady obsługi zera i skończony
budżet pracy:

| Pole | Znaczenie |
| --- | --- |
| `reject_nonzero_to_zero` | Odrzuca niezerową wartość dziesiętną zaokrągloną do zera. |
| `normalize_negative_zero` | Zwraca dodatnie zero dla obu znaków zera. |
| `max_work` | Maksymalna liczba kroków na cyfrach i limbach; zero nie dopuszcza żadnego. |

```c
const jb_json_float_options_t options = {true, true, 1000000};
uint32_t bits;
hal_status_t status = jb_json_get_f32_bits(value_node, &options, work, &bits);
```

Dla `0.22` bity binary32 wynoszą `3e6147ae`. Token minimalnie powyżej wartości
połówkowej, `1.0000000596046447753906250000000000000000001`, daje `3f800001`.
Nie ma pośredniego zaokrąglania binary64. Jeśli dozwolone jest zaokrąglenie
niezerowej wartości do zera, wynik zachowuje znak, chyba że włączono również
normalizację.

## Literały C

`jb_c_literal_workspace_size(node, type, out_bytes)` oblicza scratch; literały
integer, boolean i string potrzebują zero bajtów. Literały zmiennoprzecinkowe
korzystają ze wspólnego scratch dokładnej konwersji i formatowania.
`jb_c_literal(node, type, work, max_work, buffer, capacity, required)` zapisuje
literał bez terminatora.

| Selektor | Zapis C |
| --- | --- |
| `JB_C_BOOL` | `1` lub `0`; wymaga booleana JSON. |
| `JB_C_U8`, `JB_C_U16`, `JB_C_U32` | Sprawdzony zakres unsigned i suffix `U`. |
| `JB_C_U64` | Pełny zakres unsigned i suffix `ULL`. |
| `JB_C_I8`, `JB_C_I16`, `JB_C_I32` | Sprawdzony zakres signed; wartości ujemne w nawiasach. |
| `JB_C_I64` | Sprawdzony zakres signed i suffix `LL`. |
| `JB_C_F32`, `JB_C_F64` | Jedno dokładne zaokrąglenie IEEE, potem 9/17 cyfr znaczących w notacji naukowej. |
| `JB_C_STRING` | Bajty UTF-8 w cudzysłowach; bajty spoza ASCII i sterujące mają trzycyfrowe escape'y ósemkowe. |

Minima signed mają postać odejmowania, np. `(-2147483647 - 1)` oraz
`(-9223372036854775807LL - 1LL)`. Literały zmiennoprzecinkowe odrzucają
przepełnienie i zaokrąglenie niezerowej wartości do zera, normalizują ujemne
zero i obejmują wartości ujemne nawiasami. Dla binary32 `0.22` wynikiem jest
`2.19999999e-01f`.

Zapis stringa zachowuje wewnętrzny NUL. Cudzysłowy, backslashe i znaki zapytania
korzystają z escape'ów; trzy cyfry ósemkowe zapobiegają włączeniu kolejnej
cyfry do escape'u. `required` nie obejmuje terminatora.

```c
char literal[48];
size_t required;
hal_status_t status = jb_c_literal(gain_node, JB_C_F32, work, 1000000,
                                  literal, sizeof(literal), &required);
```

Wskaźnik `required` jest wymagany. Za mały bufor, także `(NULL, 0)`, otrzymuje
pełną wymaganą długość i daje `HAL_EOVERFLOW` bez zapisu. Pozostałe błędy
zachowują oba wyjścia. Niewłaściwy typ lub ułamek daje `HAL_ETYPE`; zakres,
wyczerpanie pracy lub scratch — `HAL_EOVERFLOW`. Kompletny przykład liczbowy
sprawdza zarówno bity, jak i wygenerowany literał.
