# MIDI Delay

**MIDI-Driven Pitch Delay** — аудио-эффект (VST3 / AU), чьи хвосты дилея транспонируются
под приходящие MIDI-ноты. Вокал повторяется не эхом, а мелодией: питч по ноте,
громкость по velocity, длина по длительности ноты.

Задача — тихий фоновый подклад, который точечно дублирует мелодию припева.
Целевая DAW: FL Studio.

## Статус

**M0 в работе.** Скелет плагина собирается и проходит валидацию: VST3, AU и Standalone,
universal binary (arm64 + x86_64). DSP пока нет — аудио проходит насквозь, редактор
показывает счётчик пришедших MIDI-нот.

Следующий шаг — сессия S02: два ADR, определяющих архитектуру ядра.

## Сборка

```bash
git clone --recursive <repo> && cd "MIDI Delay"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Плагины копируются в `~/Library/Audio/Plug-Ins/` автоматически.
Валидация AU: `auval -v aumf Mdly Mkev`.

**JUCE 9.0.2**, зафиксирован на теге в `libs/JUCE`.

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
| [docs/ISSUES.md](docs/ISSUES.md) | 40 задач с метками и критериями приёмки |
| [prompts/README.md](prompts/README.md) | Как вести работу короткими сессиями |
| [CLAUDE.md](CLAUDE.md) | Правила работы и технические инварианты |

## Залить задачи в GitHub

```bash
gh repo create midi-delay --private --source=. --remote=origin
scripts/create-issues.sh --dry-run   # посмотреть, что будет создано
scripts/create-issues.sh             # создать метки, вехи и 40 issues
```

## Начать следующую сессию

Открыть Claude Code в корне репозитория и написать:

> Прочитай prompts/NEXT.md и выполни его.
