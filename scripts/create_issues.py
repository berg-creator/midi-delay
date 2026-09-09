#!/usr/bin/env python3
"""Парсит docs/ISSUES.md и создаёт метки/вехи/issues через gh."""
import re, subprocess, sys, pathlib

SRC = pathlib.Path("docs/ISSUES.md")
MILESTONES = {
    "M0": "M0 — Фундамент",
    "M1": "M1 — Аудио-скелет",
    "M2": "M2 — Ядро фичи",
    "M3": "M3 — Музыкальность",
    "M4": "M4 — Интерфейс",
    "M5": "M5 — MVP-релиз",
    "Post-MVP": "Post-MVP",
}
LABEL_COLORS = {
    "architecture": "5319e7", "dsp": "1d76db", "midi": "0e8a16", "ui": "d93f0b",
    "build": "555555", "ci": "555555", "test": "fbca04", "docs": "c5def5",
    "perf": "b60205", "daw-compat": "e99695", "research": "bfd4f2",
    "release": "0052cc", "post-mvp": "d4c5f9", "sound": "006b75",
    "legal": "8b4513",
}

def parse(text):
    # Разделяем по заголовкам вида "## [12] Название"
    chunks = re.split(r"^## \[(\d+)\] (.+)$", text, flags=re.M)[1:]
    for num, title, body in zip(chunks[0::3], chunks[1::3], chunks[2::3]):
        labels = re.search(r"^\*\*Labels:\*\* *(.+)$", body, re.M)
        milestone = re.search(r"^\*\*Milestone:\*\* *(.+)$", body, re.M)
        # тело issue — всё после строки Milestone
        start = milestone.end() if milestone else 0
        md = body[start:].strip().rstrip("-").strip()
        yield {
            "num": int(num),
            "title": title.strip(),
            "labels": [l.strip() for l in labels.group(1).split(",")] if labels else [],
            "milestone": milestone.group(1).split()[0].strip() if milestone else "",
            "body": md,
        }

def gh(*args, check=True):
    return subprocess.run(["gh", *args], check=check, capture_output=True, text=True)

def main(argv):
    dry = "--dry-run" in argv
    only = None
    if "--only" in argv:
        only = {int(x) for x in argv[argv.index("--only") + 1].split(",")}

    issues = [i for i in parse(SRC.read_text(encoding="utf-8"))
              if only is None or i["num"] in only]
    if not issues:
        sys.exit("Ничего не разобрано — проверь формат docs/ISSUES.md")

    labels = sorted({l for i in issues for l in i["labels"]})
    miles = sorted({i["milestone"] for i in issues})
    unknown = [l for l in labels if l not in LABEL_COLORS]
    if unknown:
        sys.exit(f"Неизвестные метки (добавь цвет в LABEL_COLORS): {unknown}")

    print(f"Задач: {len(issues)}  метки: {', '.join(labels)}  вехи: {', '.join(miles)}\n")
    for i in issues:
        print(f"  #{i['num']:>2} [{i['milestone']:<8}] {i['title']}  ({', '.join(i['labels'])})")
    if dry:
        print("\n--dry-run: ничего не создано.")
        return

    for name in labels:
        gh("label", "create", name, "--color", LABEL_COLORS[name], "--force")
    for key in miles:
        title = MILESTONES.get(key, key)
        gh("api", "repos/:owner/:repo/milestones", "-f", f"title={title}", check=False)

    # id вех для привязки
    for i in issues:
        args = ["issue", "create", "--title", i["title"], "--body", i["body"]]
        for l in i["labels"]:
            args += ["--label", l]
        args += ["--milestone", MILESTONES.get(i["milestone"], i["milestone"])]
        r = gh(*args, check=False)
        print(("OK   " if r.returncode == 0 else "FAIL ") + i["title"] + " " + r.stdout.strip())
        if r.returncode:
            print("     " + r.stderr.strip())

def _selfcheck():
    """Проверка парсера на живом файле: 40 задач, у каждой есть метки, веха и критерии."""
    items = list(parse(SRC.read_text(encoding="utf-8")))
    assert len(items) == 50, len(items)
    for i in items:
        assert i["labels"], i["num"]
        assert i["milestone"] in MILESTONES, (i["num"], i["milestone"])
        assert "Критерии приёмки" in i["body"], i["num"]
        assert i["body"].count("- [") >= 3, (i["num"], i["body"].count("- ["))
    print(f"selfcheck ok: {len(items)} задач разобрано")

if __name__ == "__main__":
    if "--selfcheck" in sys.argv:
        _selfcheck()
    else:
        main(sys.argv[1:])
