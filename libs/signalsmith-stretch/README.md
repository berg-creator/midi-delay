# Signalsmith Stretch — вендоренная копия

HQ-движок питчинга (#38). Здесь лежит **копия** заголовков, а не submodule.

## Почему копия, а не submodule

`signalsmith-stretch` сам по себе не самодостаточен: он включает
`signalsmith-linear/stft.h` из отдельного репозитория, который подтягивается
через `FetchContent` в его CMakeLists. Submodule потребовал бы либо второго
submodule, либо сети на этапе конфигурации сборки. Заголовков всего девять
и 220 КБ — копия дешевле любого из этих вариантов.

## Что откуда

| Файл | Источник | Версия |
|---|---|---|
| `signalsmith-stretch.h`, `LICENSE.txt` | https://github.com/Signalsmith-Audio/signalsmith-stretch | `57b93f4e9206a089a45387eaa39bdc9f310d3308`, 2026-01-24 |
| `signalsmith-linear/` | https://github.com/Signalsmith-Audio/linear | тег `0.3.1`, `5668673560146a9cfe38c25315071e3fd68c8317` |

Раскладка каталогов сохранена: `signalsmith-stretch.h` ищет
`signalsmith-linear/stft.h` относительно include-пути, `stft.h` — `./fft.h`,
`fft.h` — `./platform/*.h`. Include-путь — сам этот каталог, он объявлен
INTERFACE-целью `signalsmith-stretch` в корневом `CMakeLists.txt`.

Выброшено из обеих поставок: `cmd/`, `web/`, `tests/`, `include/`
(форвардящие заглушки), их CMake-файлы. Только заголовки и лицензии.

## Лицензия

Обе библиотеки под MIT. Требование одно — сохранить текст лицензии и копирайт
в поставке:

- `LICENSE.txt` — MIT, © 2022 Geraint Luff / Signalsmith Audio Ltd.
- `signalsmith-linear/LICENSE.txt` — MIT, © 2025 Signalsmith Audio

Оба текста обязаны попасть в раздел About/Credits готового плагина (#40).
Проверка лицензий по первоисточникам — [ADR 0002](../../docs/adr/0002-pitch-engine.md).

## Как обновить

Клонировать оба репозитория, скопировать те же файлы, обновить таблицу выше
и прогнать `Source/DSP/test_pitch_shifter.cpp` — он меряет латентность
и расстройку и покраснеет, если поведение движка изменилось.
