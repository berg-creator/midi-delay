#!/usr/bin/env bash
# Собирает macOS-инсталлятор MIDI Delay (#33): VST3 и AU в Library/Audio/Plug-Ins.
# Обе ветки — с подписью и без — и что видит пользователь в каждой: docs/RELEASE.md.
#
#   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
#   scripts/package-mac.sh                 # -> build/pkg/MIDI-Delay-<версия>.pkg, без подписи
#
# С подписью. НЕ ПРОВЕРЕНО: сертификатов Developer ID на машине разработки нет.
#   DEVELOPER_ID_APP="Developer ID Application: <имя> (<TEAMID>)" \
#   DEVELOPER_ID_INSTALLER="Developer ID Installer: <имя> (<TEAMID>)" \
#   scripts/package-mac.sh
set -euo pipefail
cd "$(dirname "$0")/.."

version=$(sed -n 's/^project(MidiDelay VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
artefacts=build/MidiDelay_artefacts/Release
out=build/pkg
root="$out/root"

for bundle in "$artefacts/VST3/MIDI Delay.vst3" "$artefacts/AU/MIDI Delay.component"; do
    [[ -d "$bundle" ]] || { echo "нет $bundle — сначала Release-сборка, см. шапку" >&2; exit 1; }
done

rm -rf "$out"
mkdir -p "$root/VST3" "$root/Components"

# ditto, а не cp -R: бандл копируется вместе с подписью.
ditto "$artefacts/VST3/MIDI Delay.vst3"    "$root/VST3/MIDI Delay.vst3"
ditto "$artefacts/AU/MIDI Delay.component" "$root/Components/MIDI Delay.component"

# Бандлы подписываются до упаковки, а не после: pkg уносит то, что лежит в root.
# Hardened runtime нотариус требует от всего исполняемого, плагины не исключение.
if [[ -n "${DEVELOPER_ID_APP:-}" ]]; then
    for bundle in "$root/VST3/MIDI Delay.vst3" "$root/Components/MIDI Delay.component"; do
        codesign --force --timestamp --options runtime --sign "$DEVELOPER_ID_APP" "$bundle"
    done
fi

# Бандлы непереносимые. Иначе Installer ищет по диску уже стоящую копию с тем же
# идентификатором и обновляет её там, где нашёл, — хоть в build/ у разработчика.
# А идентификатор у VST3 и AU к тому же общий: com.Berg.MidiDelay.
pkgbuild --quiet --analyze --root "$root" "$out/components.plist"
for i in 0 1; do
    plutil -replace "$i.BundleIsRelocatable" -bool NO "$out/components.plist"
done

pkgbuild --quiet --root "$root" --component-plist "$out/components.plist" \
    --identifier com.Berg.MidiDelay.pkg --version "$version" \
    --install-location /Library/Audio/Plug-Ins "$out/MidiDelay-component.pkg"

# Обёртка ради выбора «для всех» или «только для меня». Второе ставит в ~/Library
# и не спрашивает пароль администратора — так установка и проверяется без sudo.
# hostArchitectures обязателен: без него Installer на Apple Silicon просит Rosetta,
# хотя внутри universal binary.
cat > "$out/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>MIDI Delay $version</title>
    <domains enable_localSystem="true" enable_currentUserHome="true" enable_anywhere="false"/>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <choices-outline>
        <line choice="com.Berg.MidiDelay.pkg"/>
    </choices-outline>
    <choice id="com.Berg.MidiDelay.pkg" title="MIDI Delay">
        <pkg-ref id="com.Berg.MidiDelay.pkg"/>
    </choice>
    <pkg-ref id="com.Berg.MidiDelay.pkg" version="$version">MidiDelay-component.pkg</pkg-ref>
</installer-gui-script>
EOF

final="$out/MIDI-Delay-$version.pkg"
productbuild --quiet --distribution "$out/distribution.xml" --package-path "$out" "$out/unsigned.pkg"

if [[ -n "${DEVELOPER_ID_INSTALLER:-}" ]]; then
    productsign --timestamp --sign "$DEVELOPER_ID_INSTALLER" "$out/unsigned.pkg" "$final"
else
    mv "$out/unsigned.pkg" "$final"
fi

echo "$final"
