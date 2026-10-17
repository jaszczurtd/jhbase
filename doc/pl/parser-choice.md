# Wybór parsera

Obecna implementacja zachowuje tokenowy parser jhbase. Jego API C pozostaje
prototypem do czasu przyjęcia testów natywnego Windows i pomiarów targetów.

Wymagane zachowanie łączy jawne długości bajtowe, wewnętrzny NUL, surowe tokeny
liczb, odrzucanie zdekodowanych duplikatów kluczy, oryginalne zakresy w źródle,
diagnostykę offsetu, pamięć callera oraz skończone budżety głębokości, danych
i porównań. Parser i writer współdzielą UTF-8 i dekodowanie stringów; konwersja
liczb korzysta z oryginalnego tokenu dziesiętnego.

## Porównane wersje

| Kandydat | Rewizja | Zaobserwowana różnica względem wymagań |
| --- | --- | --- |
| [coreJSON v3.3.0](https://github.com/FreeRTOS/coreJSON/tree/e3b7663f6392d8c10e8db57506ec37e4801b145a) | `e3b7663f6392d8c10e8db57506ec37e4801b145a` | Odrzuca poprawny string z `\u0000`; przyjmuje powtórzone klucze zwykłe i równoważne po rozwinięciu escape'ów. |
| [yyjson 0.12.0](https://github.com/ibireme/yyjson/tree/8b4a38dc994a110abaec8a400615567bd996105f) | `8b4a38dc994a110abaec8a400615567bd996105f` | DOM z puli pamięci i surowymi liczbami zachowuje NUL, ale przyjmuje duplikaty kluczy; jego reprezentacja różni się od tokenów z zakresami źródłowymi. |

Obaj kandydaci mają licencję MIT. Są źródłami do porównania, pobieranymi do
`.build` i nie należą do `jb::base`. Kod upstream nie jest poprawiany.
Configure sprawdza hashe źródeł i nagłówków przed kompilacją porównania.

Porównanie używa natywnego `JSON_Validate` oraz `yyjson_read_opts` z
`YYJSON_READ_NUMBER_AS_RAW` i jawną pulą pamięci. Sprawdza też błędne UTF-8,
pary surrogate, dane po NUL, składnię liczb i limity głębokości. Odtworzenie:

```sh
./scripts/prepare_comparison.sh
./runalltests.sh --stage comparison
```

## Dlaczego pozostaje obecny rdzeń

coreJSON wymagałby zmiany obsługi NUL oraz dodania budowania tokenów,
kontroli zdekodowanych kluczy, diagnostyki i liczenia pracy. yyjson potrzebowałby
adaptera do wymaganych niemodyfikowalnych rekordów z zakresami w źródle,
liczenia limitów i kontroli duplikatów. Obecny rdzeń już realizuje te elementy
w tym samym modelu pamięci callera co writer i moduły liczbowe.

Benchmark porównuje różne operacje: pełne parsowanie tokenów jhbase z kontrolą
duplikatów, samą walidację coreJSON i parsowanie DOM yyjson z puli pamięci.
Czas pomaga w analizie, ale nie oznacza równych API ani gwarancji.
Surowe pomiary, granice rozmiaru puli i raporty stosu pozostają lokalnymi
artefaktami buildu. Rozmiary obiektów i szacunki stosu poszczególnych funkcji
nie są rozmiarem zlinkowanego firmware ani maksymalnym stosem runtime.
Pomiary na urządzeniu i natywne MSVC mają osobne wymagania odbioru.
