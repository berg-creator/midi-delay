# MIDI Delay

**MIDI-Driven Pitch Delay** — аудио-эффект (VST3 / AU), чьи хвосты дилея транспонируются
под приходящие MIDI-ноты. Вокал повторяется не эхом, а мелодией: питч по ноте,
громкость по velocity, длина по длительности ноты.

Задача — тихий фоновый подклад, который точечно дублирует мелодию припева.
Целевая DAW: FL Studio.

## Статус

Планирование завершено. Кода ещё нет — следующий шаг сессия S01.

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
