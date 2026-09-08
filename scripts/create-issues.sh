#!/usr/bin/env bash
# Создаёт метки, вехи и issues из docs/ISSUES.md через gh.
# Использование:
#   scripts/create-issues.sh --dry-run     # показать, что будет создано
#   scripts/create-issues.sh               # создать всё
#   scripts/create-issues.sh --only 1,2,5  # создать только указанные номера
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 scripts/create_issues.py "$@"
