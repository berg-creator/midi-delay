# MIDI Delay

[![build](https://github.com/berg-creator/midi-delay/actions/workflows/build.yml/badge.svg)](https://github.com/berg-creator/midi-delay/actions/workflows/build.yml)

**MIDI-Driven Pitch Delay** — аудио-эффект (VST3 / AU), чьи хвосты дилея транспонируются
под приходящие MIDI-ноты. Вокал повторяется не эхом, а мелодией: питч по ноте,
громкость по velocity, длина по длительности ноты.

Исходная задача — тихий фоновый подклад, который точечно дублирует мелодию припева.
С сентября 2026 это один из сценариев, а не единственный: движок выводится и на громкие
эффекты (пэд из слога, арпеджированное эхо, суб-октава). Зачем этот инструмент вообще
и во что он может вырасти — [docs/VISION.md](docs/VISION.md), продаётся ли это —
[docs/STRATEGY.md](docs/STRATEGY.md). Целевая DAW: FL Studio.

## Статус

**M0, M1 и M2 закрыты.** Киллер-фича работает: вокал плюс MIDI-мелодия на входе дают
хвост, который поёт эту мелодию. Проверено не только тестами, но и живьём в FL Studio —
пользователь отрендерил материал из хоста, и по нему замерено:

| Что | Результат |
|---|---|
| Тайминг подпевки относительно вокала | 0,00 мс |
| Интонация на живом вокале | 8–10 центов медианы |
| Прозрачность на единичном ratio | −25,5 dB |
| Репортируемая латентность в режиме Free | 0 сэмплов |

Работают: sample-accurate MIDI, восемь голосов с кражей, два движка питчинга
(varispeed и Signalsmith Stretch), огибающие из velocity и длительности, компенсация
латентности, MIDI Offset, стерео по голосам, два режима времени — Free (дилей)
и Follow (подпевка звучит одновременно с вокалом, [ADR 0006](docs/adr/0006-follow-mode.md)).

**M3 идёт** — то, ради чего плагин станет пригоден для микса: диффузия, фильтры хвоста,
ping-pong, tempo sync, анти-клик. После разворота позиционирования это уже
не «музыкальность», а обязательная часть ([docs/STRATEGY.md](docs/STRATEGY.md) §9).

## Сборка

```bash
git clone --recursive https://github.com/berg-creator/midi-delay.git && cd midi-delay
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Плагины копируются в `~/Library/Audio/Plug-Ins/` автоматически.
Валидация AU: `auval -v aumf Mdly Mkev`.

**JUCE 9.0.2**, зафиксирован на теге в `libs/JUCE` (submodule).

**Signalsmith Stretch** — HQ-движок питчинга, MIT, вендорен копией заголовков
в `libs/signalsmith-stretch` вместе со своей зависимостью `signalsmith-linear`.
Почему копия, а не submodule, и как обновлять —
[libs/signalsmith-stretch/README.md](libs/signalsmith-stretch/README.md).

### Офлайн-тесты DSP

Заголовки в `Source/DSP/` не зависят от JUCE, поэтому тесты собираются одним clang,
без CMake и без линковки с фреймворком. Каждый тест — `assert` и `int main`.

```bash
c++ -std=c++20 -O2 Source/DSP/DelayBuffer.cpp Source/DSP/test_delay_buffer.cpp -o /tmp/tdb && /tmp/tdb

c++ -std=c++20 -O2 -Ilibs/signalsmith-stretch Source/DSP/DelayBuffer.cpp \
    Source/DSP/PitchShifter.cpp Source/DSP/test_pitch_shifter.cpp -o /tmp/tps && /tmp/tps
```

Тест питчера заодно печатает таблицу расстройки обоих движков в центах — это тот
замер, из которого выросли [ADR 0004](docs/adr/0004-varispeed-window.md)
и [ADR 0005](docs/adr/0005-hq-pitch-engine.md).

### Про Xcode

Полного Xcode для разработки **не требуется** — хватает Command Line Tools.
Проверено на практике: все три формата собираются, universal binary собирается,
`auval` проходит. Открытым остаётся вопрос подписи Developer ID и нотаризации
для распространения — выясняется в задаче #33.

## Навигация

| Файл | Что внутри |
|---|---|
| [docs/VISION.md](docs/VISION.md) | Зачем инструмент нужен, во что может вырасти и чего делать не будет |
| [docs/STRATEGY.md](docs/STRATEGY.md) | Продаётся ли это: позиционирование, бесплатный релиз, аудитория |
| [docs/ANALYSIS.md](docs/ANALYSIS.md) | Технический разбор: архитектура, узкие места, стек |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Шесть вех от прототипа до MVP |
| [docs/ISSUES.md](docs/ISSUES.md) | 50 задач с метками и критериями приёмки |
| [docs/FL_STUDIO.md](docs/FL_STUDIO.md) | Как подать MIDI в плагин в FL Studio и как его откалибровать |
| [docs/adr/](docs/adr/) | Архитектурные решения: ядро на голосах, движок питчинга |
| [prompts/README.md](prompts/README.md) | Как вести работу короткими сессиями |
| [CLAUDE.md](CLAUDE.md) | Правила работы и технические инварианты |

## Залить задачи в GitHub

Репозиторий уже создан. Осталось завести в нём issues:

```bash
scripts/create-issues.sh --dry-run   # посмотреть, что будет создано
scripts/create-issues.sh             # создать метки, вехи и issues
```

## Начать следующую сессию

Открыть Claude Code в корне репозитория и написать:

> Прочитай prompts/NEXT.md и выполни его.
