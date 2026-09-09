# MIDI Delay

[![build](https://github.com/berg-creator/midi-delay/actions/workflows/build.yml/badge.svg)](https://github.com/berg-creator/midi-delay/actions/workflows/build.yml)

**MIDI-Driven Pitch Delay** — аудио-эффект (VST3 / AU), чьи хвосты дилея транспонируются
под приходящие MIDI-ноты. Вокал повторяется не эхом, а мелодией: питч по ноте,
громкость по velocity, длина по длительности ноты.

Исходная задача — тихий фоновый подклад, который точечно дублирует мелодию припева.
С сентября 2026 это один из сценариев, а не единственный: движок выводится и на громкие
эффекты (пэд из слога, арпеджированное эхо, суб-октава). Почему — [docs/STRATEGY.md](docs/STRATEGY.md).
Целевая DAW: FL Studio.

## Статус

**M0 закрыта.** Скелет плагина собирается и проходит валидацию: VST3, AU и Standalone,
universal binary (arm64 + x86_64). CI собирает на macOS и Windows и гоняет pluginval
со strictness 10. Архитектура ядра зафиксирована двумя ADR.

**M1 начата.** Готов `DelayBuffer` — кольцевой буфер с дробным чтением
по Catmull-Rom (#7, #8), с офлайн-тестом. В плагин он ещё не подключён:
аудио пока проходит насквозь, редактор показывает счётчик пришедших MIDI-нот.

Следующий шаг — каркас `AudioProcessor` и параметры в APVTS (#9, #10).

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
| [docs/ANALYSIS.md](docs/ANALYSIS.md) | Технический разбор: архитектура, узкие места, стек |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Шесть вех от прототипа до MVP |
| [docs/ISSUES.md](docs/ISSUES.md) | 42 задачи с метками и критериями приёмки |
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
