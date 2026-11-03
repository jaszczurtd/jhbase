# Stringi, wektory i uporządkowane mapy w C

`jb/string.h`, `jb/vector.h` i `jb/map.h` udostępniają kontenery z własną
pamięcią przez API C11. Linkuj `jb::base`. Można ich używać również z C++17,
lecz przechowują bajty, bez konstruktorów i destruktorów obiektów C++.

## Pamięć i własność

Zainicjalizuj właściciela przez `{0}` w C lub `{}` w C++, wywołaj jego `init`,
a po użyciu `destroy`.
Nie kopiuj aktywnego właściciela przez przypisanie. `clear` zachowuje opcje
i, w stringach oraz wektorach, pamięć do ponownego użycia. Mapa zwalnia
węzły. `destroy` zwalnia pamięć i zeruje obiekt.
Osobnych kontenerów można używać niezależnie. Dostęp do jednego wymaga
synchronizacji, jeśli którykolwiek wątek go zmienia.

Inicjalizacja kopiuje wymagany `jb_allocator_t`. Callbacki i kontekst muszą
żyć dłużej niż kontener. Alokator zapewnia wyrównanie dla `jb_max_align_t`;
brak pamięci daje `HAL_ENOMEM`. Operacje sprawdzają arytmetykę rozmiarów
i zwracają `HAL_EOVERFLOW` przy przepełnieniu. Nieudana zmiana zachowuje
dotychczasową zawartość. Callback nie może rzucać wyjątków ani ponownie
wywoływać operacji tego samego kontenera.

Kod przenośny nie wybiera sam `malloc`. Na Linuksie i Windows funkcja
`jb_heap_allocator()` udostępnia `malloc/free` przez `jb::filesystem`
lub korzystający z niego `jb::host`. Aplikacja może przekazać własny alokator
z limitem pamięci. API UTF-8 i parsera JSON nadal nie alokują pamięci.

Kontenery kopiują bajty elementów. Pamięć wskazywana przez wskaźniki
w elemencie pozostaje odpowiedzialnością wywołującego; usunięcie elementu
jej nie zwalnia. Na tym poziomie określ własność zasobów albo przechowuj
pożyczone odwołania, ważne przez cały czas użycia.

## Stringi

`jb_string_t` przechowuje długość i pojemność danych niezależnie od końcowego
NUL. Wewnętrzne bajty NUL są poprawne. Indeksy wskazują bajty; operacje
nie sprawdzają UTF-8 ani nie liczą znaków Unicode.

Zawartość zmieniają `assign`, `append`, `insert`, `erase`, `resize` i `reserve`.
Źródło może wskazywać dane tego samego stringa, także terminator, ale nie
nieużywaną pojemność. Wstawienie z takiego źródła może alokować mimo wolnej
pojemności. Udana zmiana może unieważnić pożyczone wskaźniki.
Jawne `reserve` zwiększa pojemność dokładnie do żądanej wartości;
automatyczny wzrost przy append/insert/resize może rezerwować zapas.

`jb_string_data` daje widok zakończony NUL do odczytu.
`jb_string_mutable_data` udostępnia zaalokowany bufor funkcjom zapisującym
tekst po wcześniejszym `resize`. Zachowaj długość i terminator.
Pusty string bez alokacji nie ma widoku do zapisu. `jb_string_compare`
porównuje bajty i długość, uwzględniając wewnętrzne NUL.

## Wektory

`jb_vector_t` przechowuje ciągłą tablicę elementów o stałym rozmiarze.
Przekaż `sizeof(T)` do `init`. Typ `T` musi pozwalać na przenoszenie bajtów
i wymagać wyrównania nie większego niż `jb_max_align_t`. Pojemność i indeksy
liczą elementy, nie bajty.

Operacje zmiany odpowiadają operacjom stringa. `resize` zeruje nowe bajty;
wywołujący nadaje im poprawną wartość, jeśli dany typ nie dopuszcza samych
zer. Mutowalne akcesory zachowują opis wektora, lecz pozwalają zmieniać
wskazywane elementy. Warianty `const_` dają widoki tylko do odczytu.

`jb_vector_detach` oddaje alokację wywołującemu i pozostawia pusty,
zainicjalizowany wektor. Pamięć zwalnia pierwotny alokator; dla NULL pomiń
callback zwalniania. `jb_vector_shrink_to_fit` zmniejsza pojemność do liczby
elementów albo zwalnia pusty bufor. Błąd alokacji zachowuje wektor.
Odczyt pliku początkowo rezerwuje dokładnie zgłoszony rozmiar. Jeśli nadejdzie
więcej bajtów, bufor rośnie geometrycznie do limitu `max_bytes`. Przed oddaniem
wyniku czytnik usuwa niewykorzystaną pojemność; wymaga to kopii, gdy bufor jest
większy od danych. Stabilny plik z poprawnie zgłoszonym rozmiarem nie wymaga
końcowej kopii.

## Uporządkowane mapy

`jb_map_t` kopiuje klucze i wartości o stałych rozmiarach oraz porządkuje
klucze przez comparator. Wyszukanie, wstawienie i usunięcie wykonują
O(log n) porównań. Wstawienie istniejącego klucza daje `HAL_EEXIST`;
`upsert` zastępuje wartość, zachowując pierwotny klucz. Dodawanie lub
usuwanie innych wpisów zachowuje adresy pożyczonych elementów. Usunięcie
wpisu unieważnia jego wskaźniki i iterator.

Klucz i wskazywane przez niego dane używane w porównaniu nie mogą się
zmieniać, dopóki należą do mapy. Klucz zawierający wskaźnik do tekstu
pożycza ten tekst; mapa nie kopiuje wskazywanych znaków. Iteracja przebiega
według comparatora, a nie kolejności wstawienia czy kolejności tablicy hash.

[Przykład kontenerów](../../examples/containers.c) zbiera nazwane próbki
w wektorze, sumuje je w mapie i buduje uporządkowany raport tekstowy.
Używa alokatora hosta i kompiluje się jako C11 lub C++17.
