# Релиз: сборка, упаковка, подпись

Как из исходников получается то, что скачивает пользователь. Задача [#33](ISSUES.md).

Документ честный: на 11.09.2026 членства в Apple Developer Program у проекта нет,
сертификатов на машине разработки нет (`security find-identity -v` — 0), и всё, что
касается подписи, **не проверено**. Ветка без подписи проверена замером целиком,
кроме установки для всех пользователей.

| Шаг | Состояние |
|---|---|
| Release-сборка, universal binary | проверено |
| pkg без подписи, установка «только для меня» | проверено, раздел 3 |
| pkg без подписи, установка «для всех» в `/Library` | не проверено: нужен пароль администратора |
| Что видит пользователь без подписи | замерено, раздел 4 |
| Подпись Developer ID, нотаризация, stapler | не проверено: нет членства, раздел 5 |
| Windows | не проверено: машины нет, раздел 6 |

## 1. Сборка

```bash
rm -rf build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
lipo -archs "build/MidiDelay_artefacts/Release/VST3/MIDI Delay.vst3/Contents/MacOS/MIDI Delay"
```

`rm -rf build` перед релизом обязателен: CMake запекает абсолютные пути, и после
переименования домашнего каталога сборка ищет файлы по старым. `lipo` обязан
ответить `x86_64 arm64`.

## 2. Проверки перед упаковкой

```bash
cmake --build build --target ProcessorTest && ./build/ProcessorTest_artefacts/Release/ProcessorTest
```

Включает регрессионные рендеры DSP ([#34](ISSUES.md)): звук сверяется с эталонами
в `tests/regression/`. Тесты буфера и питчера — одной командой clang из шапки
`Source/DSP/test_delay_buffer.cpp` и `test_pitch_shifter.cpp`. pluginval — только
с `--skip-gui-tests`, команда и известное расхождение [#54](ISSUES.md) — в
[README.md](../README.md).

## 3. Упаковка macOS

```bash
scripts/package-mac.sh        # -> build/pkg/MIDI-Delay-<версия>.pkg
```

Скрипт кладёт VST3 в `Library/Audio/Plug-Ins/VST3`, AU — в `Library/Audio/Plug-Ins/Components`,
и в установщике даёт выбор «для всех пользователей» или «только для меня». Версия
берётся из `project(... VERSION ...)` в `CMakeLists.txt`.

Три места, где упаковка ломается молча, и что с каждым сделано:

- **Переносимые бандлы.** По умолчанию `pkgbuild` помечает бандлы переносимыми:
  Installer ищет по диску уже стоящую копию с тем же идентификатором и обновляет
  её там, где нашёл, — хоть в `build/` у разработчика. У VST3 и AU к тому же общий
  идентификатор `com.Berg.MidiDelay`. Скрипт ставит `BundleIsRelocatable = NO`;
  в `PackageInfo` это видно как `relocatable="false"` и пустой `<relocate/>`.
- **Rosetta на Apple Silicon.** Без `hostArchitectures="arm64,x86_64"` в дистрибутиве
  Installer просит поставить Rosetta, хотя внутри universal binary.
- **Файлы `._*` в payload.** `pkgutil --payload-files` показывает одиннадцать штук.
  Это не мусор, а атрибут `com.apple.provenance`, который macOS 26 вешает на всё
  и который не снимается ни `xattr -cr`, ни `COPYFILE_DISABLE=1`, ни `ditto --noextattr`.
  При установке они сливаются обратно в атрибуты — файлов `._*` на диске после
  установки ноль (замерено).

Проверка установки без пароля администратора — ровно та, что проведена 11.09.2026:

```bash
installer -pkg build/pkg/MIDI-Delay-0.1.0.pkg -target CurrentUserHomeDirectory
codesign --verify --strict -v ~/Library/Audio/Plug-Ins/VST3/"MIDI Delay.vst3"
find ~/Library/Audio/Plug-Ins -name '._*' | wc -l          # 0
pkgutil --pkgs --volume ~ | grep Berg                       # com.Berg.MidiDelay.pkg
```

Результат: установка прошла, оба бандла легли по своим каталогам, подпись цела,
бинарник побайтово совпадает со сборкой, оба формата загружаются.

**«Только для меня» прячет VST3 от FL Studio.** Этот вариант кладёт VST3
в `~/Library/Audio/Plug-Ins/VST3`, а FL этот каталог не сканирует — находка сессии 01
в `prompts/PROGRESS.md`. VST3 в FL не появится, пока путь не добавлен в Manage plugins.
AU из `~/Library` FL видит сразу, и для FL на macOS AU и так рекомендован
([FL_STUDIO.md](FL_STUDIO.md), шаг 0). Вариант оставлен ради тех, у кого нет прав
администратора, и ради проверки установки без sudo.

## 4. Что видит пользователь, если не подписывать

Замерено на macOS 26.6.2. Сборка подписана ad-hoc: так по умолчанию подписывает
линкер, а arm64-код без подписи не запускается вовсе. Инструмент замера —
[`scripts/load-bundle.c`](../scripts/load-bundle.c): грузит бандл в процесс так же,
как хост, на копии с карантином, поставленным так же, как его ставит браузер.

| Как доставлен | Что происходит | Чем замерено |
|---|---|---|
| Бандл из скачанного zip | **Плагин не загружается.** `dlopen`: `code signature ... not valid for use in process: library load disallowed by system policy`. В журнале `syspolicyd`: `Code did not match any currently allowed policy` | `load-bundle` + `log show`. VST3 и AU одинаково |
| pkg без подписи, двойной клик | **Установщик не открывается.** `spctl -a -vvv -t install`: `rejected`, `source=no usable signature` — и с карантином, и без | `spctl` |
| Плагин, поставленный из pkg | **Загружается.** Карантин pkg на разложенные файлы не переходит | pkg с карантином, `installer` из командной строки: на бандлах атрибута нет, `load: OK` |
| Собранный у себя | Загружается | `load-bundle` без карантина |

`spctl -a -vvv` отвергает ad-hoc-бандл **и без карантина**, так что сам по себе он
ничего не различает — различает только загрузка. `syspolicy_check distribution`
называет причину прямо: `Notary Ticket Missing`, `Severity: Fatal`.

**Чего этим не замерено.** Консольному процессу система окна не показала: отказ
молчаливый, в журнале `CoreServicesUIAgent` ни строки. Покажет ли окно хост
с интерфейсом (FL Studio) и какое — не проверено. Графический Installer.app тоже
не проверялся: переход карантина проверен только через `installer`. Окно, которое
получит пользователь на неподписанном pkg, не снималось; на этой версии macOS для
неподписанного кода заведены строки «“%@” Not Opened» и «Apple could not verify
“%@” is free of malware that may harm your Mac or compromise your privacy.», обход —
System Settings → Privacy & Security → «Open Anyway». Какую из строк система выберет
для pkg — не замерено.

**Выводы.**

1. **zip без подписи не выпускать.** Плагин не загрузится, а обход — `xattr -dr
   com.apple.quarantine` в Терминале — для бесплатного плагина это барьер,
   за которым пользователь уходит.
2. **pkg без подписи — терпимый запасной путь.** Один обход в System Settings, после
   установки всё работает. Но это ровно то первое впечатление, от которого
   предостерегает [STRATEGY.md](STRATEGY.md) §6: «подпись становится обязательной».
3. **Чистый путь один — подпись и нотаризация, $99 в год.** Окупается не этим
   плагином, а следующим (STRATEGY §7): тот же сертификат подписывает все.

## 5. Ветка с подписью — не проверено

Порядок: подпись бандлов → pkg → подпись pkg → нотаризация → staple.
**Подпись — не шаг поверх готового pkg:** бандлы подписываются до `pkgbuild`, иначе
в pkg уедут ad-hoc-копии. Скрипт это учитывает — подписывает между копированием
и упаковкой, если заданы переменные.

`notarytool` и `stapler` в Command Line Tools есть (`xcrun --find notarytool` →
`/Library/Developer/CommandLineTools/usr/bin/notarytool`), полный Xcode не нужен —
но в деле они не запускались.

**Один раз:**

1. Членство в Apple Developer Program.
2. Два сертификата на developer.apple.com → Certificates: **Developer ID Application**
   (бандлы) и **Developer ID Installer** (pkg). Запрос сертификата — через Keychain
   Access → Certificate Assistant, Xcode не нужен.
3. `security find-identity -v` — обе строки на месте.
4. Пароль приложения на appleid.apple.com, затем
   `xcrun notarytool store-credentials berg-notary --apple-id <почта> --team-id <TEAMID>`
   (пароль спросит сам и положит в связку).

**Каждый релиз:**

```bash
DEVELOPER_ID_APP="Developer ID Application: <имя> (<TEAMID>)" \
DEVELOPER_ID_INSTALLER="Developer ID Installer: <имя> (<TEAMID>)" \
scripts/package-mac.sh

xcrun notarytool submit build/pkg/MIDI-Delay-<версия>.pkg --keychain-profile berg-notary --wait
xcrun stapler staple build/pkg/MIDI-Delay-<версия>.pkg
```

Нотариус отвечает минуты, иногда десятки минут — в короткую сессию это закладывать
заранее. При отказе причина: `xcrun notarytool log <id> --keychain-profile berg-notary`.

**Проверка.** Релиз готов, когда зелёные все четыре:

```bash
pkgutil --check-signature build/pkg/MIDI-Delay-<версия>.pkg   # Developer ID Installer
spctl -a -vvv -t install build/pkg/MIDI-Delay-<версия>.pkg     # accepted
xcrun stapler validate build/pkg/MIDI-Delay-<версия>.pkg

# Главная: подписанный бандл под карантином грузится. Сегодня с ad-hoc — FAILED.
ditto "build/pkg/root/VST3/MIDI Delay.vst3" /tmp/gk/"MIDI Delay.vst3"
xattr -r -w com.apple.quarantine "0081;$(printf %x $(date +%s));Safari;" /tmp/gk/"MIDI Delay.vst3"
/tmp/load-bundle /tmp/gk/"MIDI Delay.vst3"                     # load: OK
```

Ожидаемые ответы в комментариях — не замер. Первая сессия с сертификатом записывает
сюда фактические формулировки и снимает пометку «не проверено».

## 6. Windows — не проверено

CI собирает VST3 и выкладывает артефактом `MIDI-Delay-Windows`. Установка —
скопировать `MIDI Delay.vst3` в `C:\Program Files\Common Files\VST3`. Подпись
Authenticode, SmartScreen и то, как Windows относится к плагину из скачанного архива,
не проверялись: Windows-машины нет. CI не запускалась с сессии 05 ([#31](ISSUES.md)).

## 7. До публичного релиза ещё

- [#42](ISSUES.md) — тексты лицензий Signalsmith и условия JUCE. В pkg их пока нет.
- [#49](ISSUES.md) — имя разработчика. Идентификаторы — `com.Berg.MidiDelay`, код
  производителя `Berg`, пакет `com.Berg.MidiDelay.pkg` — после первого релиза
  не меняются: по ним хосты находят плагин в сохранённых проектах.
- Версия в `CMakeLists.txt` — одна на бандлы и pkg. Сейчас `0.1.0`.
- Standalone собирается, но в pkg не входит нарочно: продукт — плагин.
