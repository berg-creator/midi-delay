// Загружает исполняемый файл бандла плагина в процесс — так же, как это делает хост.
// Замер для #33: пропустит ли Gatekeeper плагин, скачанный из интернета. Карантин
// ставится руками так же, как его ставит браузер (docs/RELEASE.md, раздел 4):
//   cc -framework CoreFoundation scripts/load-bundle.c -o /tmp/load-bundle
//   ditto "build/MidiDelay_artefacts/Release/VST3/MIDI Delay.vst3" /tmp/gk/"MIDI Delay.vst3"
//   xattr -r -w com.apple.quarantine "0081;$(printf %x $(date +%s));Safari;" /tmp/gk/"MIDI Delay.vst3"
//   /tmp/load-bundle /tmp/gk/"MIDI Delay.vst3"     # load: OK или load: FAILED с причиной
// Карантин ставить только на копию: установленный плагин с ним не загрузится и в хосте.
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf (stderr, "usage: load-bundle <path to .vst3 or .component>\n");
        return 2;
    }

    CFURLRef url = CFURLCreateFromFileSystemRepresentation (NULL, (const UInt8*) argv[1], (CFIndex) strlen (argv[1]), true);
    CFBundleRef bundle = CFBundleCreate (NULL, url);

    if (bundle == NULL)
    {
        printf ("not a bundle: %s\n", argv[1]);
        return 2;
    }

    CFErrorRef error = NULL;
    Boolean ok = CFBundleLoadExecutableAndReturnError (bundle, &error);
    printf ("load: %s\n", ok ? "OK" : "FAILED");
    fflush (stdout);

    if (error != NULL)
        CFShow (error);

    return ok ? 0 : 1;
}
