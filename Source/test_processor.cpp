// Офлайн-тест процессора (#9-#15): ноль аллокаций в аудиопотоке, точность смещения
// дилея, смена sample rate, раскладки шин, живучесть состояния, sample-accurate MIDI,
// пул голосов и маппинг ноты в pitch ratio. Без фреймворков.
// Собирается и запускается так (в CI намеренно не собирается, см. CMakeLists.txt):
//   cmake --build build --target ProcessorTest && ./build/ProcessorTest_artefacts/Release/ProcessorTest
// Офлайн-рендер для слуховой проверки (#19):
//   ./build/ProcessorTest_artefacts/Release/ProcessorTest --render out-hq.wav hq
//   ./build/ProcessorTest_artefacts/Release/ProcessorTest --render out-fast.wav fast

#include "PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <chrono>
#include <thread>
#include <algorithm>
#include <numeric>
#include <vector>

// Не assert: сборка Release определяет NDEBUG, и assert превратился бы в пустоту —
// тест «проходил» бы, ничего не проверяя. Проверка обязана быть безусловной.
#define CHECK(cond) \
    do { if (! (cond)) { std::printf ("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
                         std::abort(); } } while (false)

// Счётчик аллокаций включается только вокруг processBlock: JUCE снаружи выделяет
// память постоянно, и это нормально. Проверка должна быть измерением, а не обещанием.
//
// Фильтр по потоку обязателен. JUCE держит живой message thread, тот изредка выделяет
// свои 16 байт, и без фильтра они попадали в счётчик просто по совпадению во времени:
// проверка краснела примерно на трети прогонов и указывала каждый раз на разную строку.
// Ловили не плагин, а соседний поток. Пишется id один раз в начале main, дальше только
// читается, поэтому гонки нет.
static std::atomic<bool> counting { false };
static std::atomic<long> allocations { 0 };
static std::thread::id countedThread;

void* operator new (std::size_t n)
{
    if (counting.load (std::memory_order_relaxed) && std::this_thread::get_id() == countedThread)
        allocations.fetch_add (1, std::memory_order_relaxed);

    return std::malloc (n);
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

namespace
{
    /** Playhead с фиксированным темпом. В офлайн-тесте хоста нет, а Sync (#20) берёт
        BPM именно у него. Отдаётся только темп: больше плагин ничего не читает,
        и придумывать позицию в такте значило бы обещать то, чего никто не спрашивал. */
    struct FixedTempoPlayHead final : juce::AudioPlayHead
    {
        explicit FixedTempoPlayHead (double bpmIn) : bpm (bpmIn) {}

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setBpm (bpm);
            return info;
        }

        double bpm;
    };

    /** Playhead, отдающий позицию без темпа. Так ведёт себя хост, который про BPM
        не знает, — случай отдельный от «playhead вообще нет», и фолбэк обязан
        отработать в обоих. */
    struct NoTempoPlayHead final : juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override { return PositionInfo {}; }
    };

    /** Возвращает параметры к значениям по умолчанию. С сессии 15 плагин открывается
        на фабричном пресете Chord Pad (#48) — это верно для пользователя, но тесты
        меряют движок, а не продуктовое решение, и база у них обязана быть одна и та же
        независимо от того, какой пресет сегодня стоит нулевым. Сам пресет по умолчанию
        проверяется отдельно, в разделе пресетов. */
    void engineDefaults (MidiDelayProcessor& proc)
    {
        for (auto* p : proc.getParameters())
            p->setValueNotifyingHost (p->getDefaultValue());
    }

    /** Значение параметра в тех же единицах, что в окне. Через сырое значение APVTS,
        а не через нормализованное: сверять пресет с таблицей в ISSUES надо в тех же
        числах, какими она написана. */
    float getParam (const MidiDelayProcessor& proc, const char* id)
    {
        auto* v = proc.apvts.getRawParameterValue (id);
        CHECK (v != nullptr);
        return v->load();
    }

    void setParam (MidiDelayProcessor& proc, const char* id, float value)
    {
        auto* p = proc.apvts.getParameter (id);
        CHECK (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        CHECK (std::abs (proc.apvts.getRawParameterValue (id)->load() - value) < 0.01f);
    }

    /** Выключает окраску петли: фильтры на краях диапазона, диффузия и дрейф в нуле,
        приседание выключено, Sync выключен. Почти
        все замеры ниже меряют тайминг и уровни, и окраска в них только мешает; те
        тесты, что меряют её саму, ставят значения руками. Заодно замеры перестают
        зависеть от того, какие значения выбраны значениями по умолчанию.

        Sync здесь же и по той же причине: с сессии 14 он включён по умолчанию, а без
        playhead это 120 BPM и четверть, то есть 500 мс поверх любого выставленного
        руками delay time. Замер тайминга обязан слушаться ручки, а не сетки; тесты
        самой сетки включают Sync сами. */
    void plainLoop (MidiDelayProcessor& proc)
    {
        // Характер Clean (#56) закреплён явно, а не унаследован от умолчания: эталоны
        // регрессии (#34) обязаны остаться бит-в-бит и тогда, когда умолчание сменят.
        setParam (proc, "character", 0.0f);
        // Stereo Ping-Pong (#55) — по той же причине. Хор поёт ноту парой расстроенных голосов,
        // а замерам тайминга и уровня нужен один голос: у ping-pong нота в тишину — один голос
        // в центре, бит-в-бит как нулевой слот прежнего разлёта, на котором замеры сделаны.
        setParam (proc, "stereo", 2.0f);
        setParam (proc, "sync", 0.0f);
        setParam (proc, "diffusion", 0.0f);
        setParam (proc, "modulation", 0.0f);
        setParam (proc, "ducking", 0.0f);
        setParam (proc, "filterLo", 20.0f);
        setParam (proc, "filterHi", 20000.0f);
    }

    /** Прогоняет блок с включённым счётчиком аллокаций. Копия MidiBuffer делается
        до включения счётчика: считаем только то, что делает сам processBlock. */
    void runBlock (MidiDelayProcessor& proc, juce::AudioBuffer<float>& buffer,
                   const juce::MidiBuffer& midiIn = {})
    {
        juce::MidiBuffer midi (midiIn);

        counting = true;
        proc.processBlock (buffer, midi);
        counting = false;
    }

    juce::MidiBuffer noteOnAt (int samplePosition, int note = 60, float velocity = 1.0f)
    {
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, note, velocity), samplePosition);
        return midi;
    }

    /** Амплитуда синусоиды частоты f: ДПФ в одной точке, без БПФ и без окна. */
    double amplitudeAt (const juce::AudioBuffer<float>& x, int from, int count, double f, double sr)
    {
        double re = 0.0, im = 0.0;

        for (int i = 0; i < count; ++i)
        {
            const double a = -juce::MathConstants<double>::twoPi * f * i / sr;
            re += x.getSample (0, from + i) * std::cos (a);
            im += x.getSample (0, from + i) * std::sin (a);
        }

        return 2.0 * std::sqrt (re * re + im * im) / count;
    }

    /** Частота самой сильной составляющей в окрестности: перебор сетки в 1 Гц. */
    double dominantFrequency (const juce::AudioBuffer<float>& x, int from, int count,
                              double centre, double span, double sr)
    {
        double best = centre, bestAmp = -1.0;

        for (double f = centre - span; f <= centre + span; f += 1.0)
        {
            const double a = amplitudeAt (x, from, count, f, sr);

            if (a > bestAmp) { bestAmp = a; best = f; }
        }

        return best;
    }

    void fillDC (juce::AudioBuffer<float>& buffer, float value = 1.0f)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                buffer.setSample (ch, i, value);
    }
}

/** Офлайн-рендер для проверки на слух. Не тест — тесты не умеют сказать «звучит
    убедительно». Это #19. Материалом может быть синтетическая пила 220 Гц (по умолчанию)
    или WAV с диска: живой голос показывает то, о чём пила молчит, — атаку слога
    и согласные, то есть ровно слабое место фазового вокодера.
    Рендер воспроизводим: зерно случайной фазы у HQ-движка фиксировано.
    Mix 100 %: слушаем сам движок, а не то, как он прячется за сухим сигналом.
    По умолчанию диффузия, фильтры петли и обратная связь выключены — рендер судит
    питчер, а не обвязку. Пятый аргумент colour оставляет все значения по умолчанию,
    то есть даёт плагин таким, каким его услышит пользователь: это A/B к тому же файлу,
    одной командой.
    Слово shift в хвосте команды переводит форманты в Shift (#24) — это A/B к Hold
    на том же файле и одной командой, ровно как colour к окраске петли. Там же можно
    выставить любой параметр по имени: feedback=75 modulation=60. Это дешевле, чем
    заводить флаг под каждую ручку, которую захочется послушать в следующий раз.
    Слово pluck меняет мелодию на ритмичный паттерн восьмыми — сценарий, ради которого
    плагин и задуман: голос идёт своим флоу, хвост повторяет ноты плака в сетке трека.
    Запуск: ProcessorTest --render out.wav [fast|hq|follow] [input.wav] [colour] [shift] [pluck] [id=value ...] */
static int renderDemo (const juce::String& path, const juce::String& mode,
                       const juce::String& inputPath, bool colour, bool formantShift,
                       bool pluck, const juce::StringPairArray& overrides)
{
    const bool hq     = mode != "fast";
    const bool follow = mode == "follow";

    constexpr int blockSize = 512;
    constexpr double maxSeconds = 30.0;   // хватает на слух, а файл остаётся лёгким

    // Материал. Файл читается как есть, без ресемплинга: плагин просто готовится
    // на частоту файла. Моно раскладывается на оба канала — вход у нас обычно моно-вокал.
    juce::AudioBuffer<float> input;
    double sr = 48000.0;

    if (inputPath.isNotEmpty())
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

       #if JUCE_MAC || JUCE_IOS
        // Не роскошь: материал для прослушивания приходит из диктофона и мессенджеров,
        // и там сплошь AAC — иногда прямо под расширением .wav. registerBasicFormats
        // знает только WAV и AIFF и на таком файле молча вернёт nullptr.
        formats.registerFormat (new juce::CoreAudioFormat(), false);
       #endif

        juce::File source (inputPath);
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));

        if (reader == nullptr)
        {
            std::printf ("cannot read %s\n", source.getFullPathName().toRawUTF8());
            return 1;
        }

        sr = reader->sampleRate;

        // Читается весь файл, а не первые полминуты: нужный кусок в нём может быть
        // где угодно, см. ниже. Потолок в пять минут — чтобы случайно указанный
        // час подкаста не съел память.
        const int frames = static_cast<int> (juce::jmin (reader->lengthInSamples,
                                                         static_cast<juce::int64> (sr * 300.0)));
        juce::AudioBuffer<float> raw (2, frames);
        reader->read (&raw, 0, frames, 0, true, true);

        if (reader->numChannels < 2)
            raw.copyFrom (1, 0, raw, 0, 0, frames);

        // Ручное начало, если названо: автопоиск ищет, где голоса больше всего, а это
        // не то же самое, где он лучше всего записан. У живой записи начало бывает
        // хуже середины, и слышит это только человек.
        // Откуда брать кусок. Обрезать тишину в начале мало: дикторская запись — это
        // сессия с дублями, и после первой фразы идёт пауза в семь секунд. Берётся
        // окно, в котором голос звучит дольше всего: тридцать секунд, наполовину
        // состоящие из комнаты, про согласные ничего не расскажут.
        const int start = [&raw, frames, sr, &overrides]
        {
            if (overrides.containsKey ("startS"))
                return juce::jlimit (0, juce::jmax (0, frames - 1),
                                     static_cast<int> (overrides["startS"].getDoubleValue() * sr));

            const int frame = juce::jmax (1, static_cast<int> (sr * 0.25));
            const int count = frames / frame;
            const int window = juce::jmin (count, static_cast<int> (maxSeconds / 0.25));

            if (window <= 0 || count <= window)
                return 0;

            std::vector<float> level (static_cast<size_t> (count));

            for (int i = 0; i < count; ++i)
                level[static_cast<size_t> (i)] = raw.getRMSLevel (0, i * frame, frame);

            // Порог — четверть от громкого уровня самой записи. Абсолютное число
            // не годится: у одной шумовая полка -60 dB, у другой -35, и одно и то же
            // число одну запись обрежет по живому, а в другой примет комнату за речь.
            std::vector<float> sorted (level);
            std::sort (sorted.begin(), sorted.end());
            const float threshold = sorted[static_cast<size_t> (count * 9 / 10)] * 0.25f;

            // Считается не только «сколько кадров звучат», но и насколько громко.
            // Вырезка из старого трека — это куски разного качества и уровня, и окно,
            // набранное тихими кадрами, формально плотное, а на слух гнилое.
            // Вес — сумма уровней звучащих кадров: тихое окно проигрывает громкому
            // при той же плотности, и это ровно то, что просил пользователь.
            int best = 0;
            double bestScore = -1.0;

            for (int i = 0; i + window <= count; ++i)
            {
                double score = 0.0;

                for (int k = i; k < i + window; ++k)
                    if (level[static_cast<size_t> (k)] > threshold)
                        score += level[static_cast<size_t> (k)];

                if (score > bestScore) { bestScore = score; best = i; }
            }

            return best * frame;
        }();

        const int kept = juce::jmin (frames - start, static_cast<int> (sr * maxSeconds));
        input.setSize (2, kept);

        for (int ch = 0; ch < 2; ++ch)
            input.copyFrom (ch, 0, raw, ch, start, kept);

        if (start > 0)
            std::printf ("  кусок взят с %.1f с — там голоса больше всего\n", start / sr);
    }
    else
    {
        // Пила 220 Гц: у неё много гармоник, и транспонирование на ней слышно сразу.
        const int frames = static_cast<int> (sr * 8.0);
        input.setSize (2, frames);
        double phase = 0.0;

        for (int i = 0; i < frames; ++i)
        {
            phase += 220.0 / sr;
            if (phase >= 1.0) phase -= 1.0;

            const auto value = static_cast<float> (0.25 * (2.0 * phase - 1.0));
            input.setSample (0, i, value);
            input.setSample (1, i, value);
        }
    }

    // Длина куска ключом seconds=: критерий #48 просит пятнадцатисекундный ролик,
    // а автопоиск берёт тридцать. Обрезается после выбора куска, а не вместо него.
    const int total = overrides.containsKey ("seconds")
        ? juce::jmin (input.getNumSamples(),
                      static_cast<int> (sr * overrides["seconds"].getDoubleValue()))
        : input.getNumSamples();

    MidiDelayProcessor proc;
    engineDefaults (proc);
    // Sync гасится: команды рендера задают время в миллисекундах, посчитанных под темп
    // материала руками (noteMs, delayTime). Включить его обратно можно ключом sync=1,
    // и тогда темп задаётся ключом bpm= — иначе сетка встанет на фолбэк 120.
    setParam (proc, "sync", 0.0f);
    setParam (proc, "delayTime", 400.0f);
    setParam (proc, "mix", 100.0f);
    setParam (proc, "outputGain", 0.0f);
    setParam (proc, "bypass", 0.0f);
    setParam (proc, "attack", 10.0f);
    setParam (proc, "release", 300.0f);
    setParam (proc, "rootKey", 0.0f);       // C
    setParam (proc, "pitchRange", 12.0f);
    setParam (proc, "quality", hq ? 1.0f : 0.0f);
    setParam (proc, "timeMode", follow ? 1.0f : 0.0f);
    // Только когда слово shift названо явно: иначе рендер навязывал бы своё значение
    // вместо умолчания, и с colour показывал бы не тот плагин, который откроет
    // пользователь. Ровно тот же капкан, что был с числами окраски до сессии 12.
    if (formantShift)
        setParam (proc, "formants", 0.0f);
    // Без colour окраска петли и обратная связь выключаются руками — слышно голый
    // питчер. С colour не выставляется ничего: остаются значения по умолчанию,
    // то есть ровно то, что услышит пользователь, открыв плагин. Числа тут
    // намеренно не дублируются — иначе рендер начнёт врать при первой же их правке.
    if (! colour)
    {
        setParam (proc, "feedback", 0.0f);
        setParam (proc, "diffusion", 0.0f);
        setParam (proc, "modulation", 0.0f);
        setParam (proc, "ducking", 0.0f);
        setParam (proc, "filterLo", 20.0f);
        setParam (proc, "filterHi", 20000.0f);
    }

    // Фабричный пресет ключом preset=N (#48). Идёт после базы рендера и до ручных
    // значений: пресет обязан перебивать умолчания рендера — иначе слушали бы не его, —
    // а названное руками обязано перебивать пресет.
    if (overrides.containsKey ("preset"))
    {
        const int index = overrides["preset"].getIntValue();

        if (index < 0 || index >= proc.getNumPrograms())
        {
            std::printf ("нет такого пресета: %d\n", index);
            return 1;
        }

        proc.setCurrentProgram (index);
        std::printf ("  пресет %d: %s\n", index, proc.getProgramName (index).toRawUTF8());
    }

    // Ручные значения идут последними: они обязаны перебивать и умолчания, и то,
    // что выключил режим без colour.
    for (const auto& id : overrides.getAllKeys())
    {
        if (id == "noteMs" || id == "startS" || id == "notes" || id == "bpm"
            || id == "preset" || id == "drySeconds" || id == "seconds")
            continue;   // настройки рендера, а не параметры плагина

        if (proc.apvts.getParameter (id) == nullptr)
        {
            std::printf ("нет такого параметра: %s\n", id.toRawUTF8());
            return 1;
        }

        setParam (proc, id.toRawUTF8(), overrides[id].getFloatValue());
        std::printf ("  %s = %s\n", id.toRawUTF8(), overrides[id].toRawUTF8());
    }

    // Темп подсовывается плагину так же, как это делает хост: своим playhead.
    // Живёт до конца рендера — отсюда и не локальная переменная в if.
    FixedTempoPlayHead playHead (overrides.containsKey ("bpm")
                                     ? overrides["bpm"].getDoubleValue() : 120.0);
    if (overrides.containsKey ("bpm"))
        proc.setPlayHead (&playHead);

    proc.setPlayConfigDetails (2, 2, sr, blockSize);
    proc.prepareToPlay (sr, blockSize);

    // Мелодия крутится по кругу до конца материала: иначе на длинном файле хвост
    // молчал бы там, где как раз и интересно слушать согласные. Номера — MIDI 60,
    // 64, 67, 72, в терминах FL Studio C5, E5, G5, C6: унисон, терция, квинта, октава
    // от Root Key. Унисон в списке не случайно — на нём слышно прозрачность движка.
    // Два разных материала, и разница между ними — это разница между «проверить движок»
    // и «проверить идею». Обычный: четыре длинные ноты по секунде, на них слышно
    // прозрачность и расстройку. Ритмичный (слово pluck в команде): восьмые при 120 BPM,
    // паттерн как у синтезаторного плака — то, ради чего плагин задуман. Хвост при
    // delay time 250 мс попадает ровно на следующую восьмую, то есть в сетку трека.
    static constexpr int melody[] { 60, 64, 67, 72 };
    static constexpr int pluckPattern[] { 60, 64, 67, 64, 69, 67, 64, 60 };

    // Свой паттерн ключом notes=71,74,78,74. Зашитый годится только для до-мажора:
    // у материала своя тональность, и ноты не из неё дают не гармонию, а фальшь.
    std::vector<int> custom;

    for (const auto& part : juce::StringArray::fromTokens (overrides["notes"], ",", ""))
        if (part.trim().isNotEmpty())
            custom.push_back (part.getIntValue());

    const int* notes = ! custom.empty() ? custom.data() : (pluck ? pluckPattern : melody);
    const int numNotes = ! custom.empty() ? static_cast<int> (custom.size()) : (pluck ? 8 : 4);
    const double noteSeconds = overrides.containsKey ("noteMs")
        ? overrides["noteMs"].getDoubleValue() * 0.001 : (pluck ? 0.25 : 1.0);
    const int noteLength = static_cast<int> (sr * noteSeconds);
    const int gap = static_cast<int> (sr * (pluck ? 0.03 : 0.05));
    const int firstNote = static_cast<int> (sr * 0.5);

    // Демо «сухой -> эффект» одним файлом (#48): первые drySeconds плагин обойдён,
    // дальше включается. Слушать два файла подряд и держать в голове первый — не то же
    // самое, что услышать переход. Заодно это кроссфейд обхода (ADR 0003) на живом
    // голосе, а не на синусе.
    const double drySeconds = overrides.containsKey ("drySeconds")
                                  ? overrides["drySeconds"].getDoubleValue() : 0.0;
    if (drySeconds > 0.0)
        setParam (proc, "bypass", 1.0f);

    juce::AudioBuffer<float> out (2, total);
    juce::AudioBuffer<float> block (2, blockSize);

    for (int start = 0; start < total; start += blockSize)
    {
        const int n = juce::jmin (blockSize, total - start);

        if (drySeconds > 0.0 && start >= static_cast<int> (drySeconds * sr)
            && start < static_cast<int> (drySeconds * sr) + blockSize)
            setParam (proc, "bypass", 0.0f);

        for (int ch = 0; ch < 2; ++ch)
            block.copyFrom (ch, 0, input, ch, start, n);

        juce::MidiBuffer midi;

        for (int k = 0; firstNote + k * noteLength < total; ++k)
        {
            const int on  = firstNote + k * noteLength;
            const int off = on + noteLength - gap;
            const int note = notes[k % numNotes];

            if (on  >= start && on  < start + n) midi.addEvent (juce::MidiMessage::noteOn  (1, note, 0.9f), on  - start);
            if (off >= start && off < start + n) midi.addEvent (juce::MidiMessage::noteOff (1, note), off - start);
        }

        juce::AudioBuffer<float> view (block.getArrayOfWritePointers(), 2, n);
        proc.processBlock (view, midi);

        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, start, block, ch, 0, n);
    }

    // Плак в миксе. Без него на слух нечего сравнивать: вопрос «попадает ли хвост
    // в гармонию» без гармонии не имеет смысла. Синтез самый простой, какой звучит
    // щипком, — пила с быстрым спадом через однополюсник. Играет он ровно те же ноты
    // и в те же моменты, что уходят в плагин: это и есть проверка «хвост повторяет
    // ноты плака», а не «хвост звучит как что-то».
    if (pluck)
    {
        const auto decay = static_cast<float> (std::exp (-1.0 / (sr * 0.18)));
        float lp = 0.0f;

        for (int k = 0; firstNote + k * noteLength < total; ++k)
        {
            const int on = firstNote + k * noteLength;
            const double frequency = 440.0 * std::pow (2.0, (notes[k % numNotes] - 69) / 12.0);
            const int length = juce::jmin (noteLength, total - on);

            double phase = 0.0;
            // Уровень плака: он тут аккомпанемент, а не солист. На слух 0,22 забивало
            // и голос, и хвост — то есть ровно то, что проверялось, слышно не было.
            float env = 0.03f;
            lp = 0.0f;

            // Ноты плака расходятся по панораме через одну, равномощно. Голос при этом
            // остаётся моно по центру: стерео нужно фону, а не солисту.
            const float angle = 0.25f * juce::MathConstants<float>::pi
                              * (((k % 2) == 0 ? -0.6f : 0.6f) + 1.0f);
            const float pan[2] { std::cos (angle) * 1.41421356f, std::sin (angle) * 1.41421356f };

            for (int i = 0; i < length; ++i)
            {
                phase += frequency / sr;
                if (phase >= 1.0) phase -= 1.0;

                lp += 0.25f * (static_cast<float> (2.0 * phase - 1.0) - lp);
                env *= decay;

                for (int ch = 0; ch < 2; ++ch)
                    out.addSample (ch, on + i, lp * env * pan[ch]);
            }
        }

        // Плак прибавлен поверх готового микса, значит пик мог уйти за единицу.
        // Нормировать весь файл честнее, чем клиповать: клип слышен как хруст
        // и его потом ищут в плагине, которого он не касается.
        const float peak = juce::jmax (out.getMagnitude (0, 0, total), out.getMagnitude (1, 0, total));

        if (peak > 0.99f)
        {
            out.applyGain (0.99f / peak);
            std::printf ("  пик %.2f, файл приглушён на %.1f dB\n", peak,
                         juce::Decibels::gainToDecibels (0.99f / peak));
        }
    }

    juce::File file (juce::File::getCurrentWorkingDirectory().getChildFile (path));
    file.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (new juce::FileOutputStream (file), sr, 2, 24, {}, 0));

    if (writer == nullptr)
    {
        std::printf ("cannot write %s\n", file.getFullPathName().toRawUTF8());
        return 1;
    }

    writer->writeFromAudioSampleBuffer (out, 0, total);
    writer.reset();

    std::printf ("rendered %s (%.1f s, %s%s%s%s, formants %s)\n", file.getFullPathName().toRawUTF8(),
                 total / sr, hq ? "hq" : "fast", follow ? ", follow" : "",
                 colour ? ", colour" : "", pluck ? ", pluck" : "",
                 proc.apvts.getRawParameterValue ("formants")->load() > 0.5f ? "hold" : "shift");

    // Уровень файла — в таблицу рендеров (#56). Характер меняет громкость хвоста: насыщение
    // сжимает, полоса Lo-Fi срезает. Разницу в 3 dB ухо читает как «лучше» или «хуже»
    // раньше, чем как тембр, и без числа рядом вердикт о тембре был бы вердиктом о громкости.
    const auto rms = std::sqrt (0.5 * (std::pow (out.getRMSLevel (0, 0, total), 2.0f)
                                       + std::pow (out.getRMSLevel (1, 0, total), 2.0f)));
    std::printf ("  пик %.1f dBFS, RMS %.1f dBFS\n",
                 juce::Decibels::gainToDecibels (out.getMagnitude (0, total), -120.0f),
                 juce::Decibels::gainToDecibels (static_cast<float> (rms), -120.0f));
    return 0;
}

/** Замер стоимости процессора. Не тест — тесты не решают, что 12 % это много.
    Худший случай из ADR 0005: восемь голосов, все транспонированные (аккорд, а не
    одна нота — компенсация формант стоит только на сдвинутых голосах), 48 кГц,
    блок 512, окраска петли по умолчанию. Считается доля ядра: сколько реального
    времени ушло на обсчёт секунды звука.
    Запуск: ProcessorTest --bench [fast|hq] [hold|shift] */
static int benchmark (const juce::String& mode, const juce::String& formants)
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 512;
    constexpr double seconds = 20.0;

    MidiDelayProcessor proc;
    engineDefaults (proc);
    setParam (proc, "quality", mode == "fast" ? 0.0f : 1.0f);
    setParam (proc, "sync", 0.0f);          // цифра бенча обязана мериться на том же времени
    setParam (proc, "delayTime", 400.0f);
    setParam (proc, "mix", 100.0f);
    setParam (proc, "feedback", 35.0f);
    setParam (proc, "voices", 8.0f);
    setParam (proc, "release", 2000.0f);
    setParam (proc, "formants", formants == "shift" ? 0.0f : 1.0f);

    proc.setPlayConfigDetails (2, 2, sr, blockSize);
    proc.prepareToPlay (sr, blockSize);

    // Восемь разных нот, ни одной в унисон: на унисоне HQ не строит частотную карту,
    // и замер вышел бы вдвое оптимистичнее правды (риск 4 сессии 13).
    juce::MidiBuffer chord;
    for (int k = 0; k < 8; ++k)
        chord.addEvent (juce::MidiMessage::noteOn (1, 61 + k, 0.9f), 0);

    juce::AudioBuffer<float> block (2, blockSize);
    const int blocks = static_cast<int> (sr * seconds / blockSize);
    double phase = 0.0;

    // Прогрев: первый блок аллоцирует таблицы БПФ внутри библиотеки, и его время
    // к установившейся стоимости отношения не имеет.
    block.clear();
    proc.processBlock (block, chord);

    const auto started = std::chrono::steady_clock::now();

    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            phase += 220.0 / sr;
            if (phase >= 1.0) phase -= 1.0;

            const auto v = static_cast<float> (0.25 * (2.0 * phase - 1.0));
            block.setSample (0, i, v);
            block.setSample (1, i, v);
        }

        juce::MidiBuffer midi;
        proc.processBlock (block, midi);
    }

    const double elapsed = std::chrono::duration<double> (
        std::chrono::steady_clock::now() - started).count();

    std::printf ("%s, formants %s: %.1f %% ядра (%.2f с на %.0f с звука)\n",
                 mode == "fast" ? "fast" : "hq", formants.toRawUTF8(),
                 100.0 * elapsed / seconds, elapsed, seconds);
    return 0;
}

/** Регрессионные рендеры DSP (#34). Четыре сценария с фиксированным входом и MIDI —
    чистый дилей, голос с питчем, аккорд, петля обратной связи с окраской по умолчанию —
    сверяются с эталонами в tests/regression/. Ловят тихие поломки: каждый CHECK выше
    мерит одно свойство, а эталон держит весь сигнал целиком.

    Эталоны переписываются словом update — только когда звук изменён намеренно
    и изменение услышано (вердикт уха главнее замера, CLAUDE.md), и коммитятся вместе
    с правкой. Хвостом выставляется любой параметр поверх всех сценариев
    (outputGain=0.1): так проверяется, что тест вообще способен упасть.
    Запуск из корня репозитория: ProcessorTest --regress [update] [id=value ...] */
static int regression (bool update, const juce::StringPairArray& overrides)
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 512;

    // Допуск — насколько выход может отойти от эталона в окне, относительно уровня
    // эталона. Снизу его держит дрейф между платформами (x86_64 без FMA против arm64
    // с FMA, замерено через Rosetta), сверху — наименьшая поломка, которую обязан
    // поймать тест: +0,1 dB громкости дают -38,7 dB. Числа — ## [34].
    // Пол -100 dBFS — для тихих окон, где относительная мера теряет смысл: ниже шума
    // 16-битного файла расхождение не слышно ни в каком миксе.
    constexpr double tolerance_dB = -60.0;
    constexpr double absoluteFloor_dBFS = -100.0;
    const double relativeTolerance = std::pow (10.0, tolerance_dB / 20.0);
    const double absoluteFloor = std::pow (10.0, absoluteFloor_dBFS / 20.0);

    struct Case
    {
        const char* name;
        float quality;              // 0 Fast, 1 HQ
        std::vector<int> notes;
        float feedback;
        bool colour;                // окраска петли по умолчанию, а не выключенная
        double seconds;
        float character = 0.0f;     // Clean; Tape и Lo-Fi — сценарии после вердикта сессии 21
        float age = 0.0f;
    };

    // Оба движка, один голос и несколько, унисон и сдвиг. Нота 60 — унисон от Root Key C.
    // Петля — на Fast нарочно. HQ на окрашенной петле расходится между платформами тем
    // сильнее, чем тише хвост (-29 dB там, где хвост уже на -55 dBFS), и относительный
    // допуск на нём не держится никакой. Петле движок не нужен: feedback снимается
    // до питч-стадии. HQ покрыт сценарием voice.
    const std::vector<Case> cases {
        { "delay",    0.0f, { 60 },             0.0f,  false, 1.2 },
        { "voice",    1.0f, { 67 },             0.0f,  false, 1.2 },
        { "chord",    0.0f, { 60, 64, 67, 71 }, 0.0f,  false, 1.2 },
        { "feedback", 0.0f, { 72 },             60.0f, true,  2.4 },
        // Характеры (#56) заведены после вердикта уха, а не раньше: эталон держит звук,
        // который услышан. Tape 30 выбран для Sung Vocal и идёт петлёй, как feedback, —
        // характер красит каждый круг. Lo-Fi 50 звучал в рендере сессии 20 и идёт одним
        // проходом, и это замер, а не экономия: квантователь в петле превращает разницу
        // FMA ниже младшего бита в целый шаг, и круг за кругом она растёт — под x86_64
        // петля Lo-Fi разошлась на −50 dB к 2,1 с при допуске −60. Петлю Lo-Fi держит
        // раздел «Характер» (#56): хвост гаснет до нуля.
        { "tape",     0.0f, { 72 },             60.0f, true,  2.4, 1.0f, 30.0f },
        { "lofi",     0.0f, { 72 },             0.0f,  true,  1.2, 2.0f, 50.0f },
    };

    const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile ("tests/regression");
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    juce::FlacAudioFormat flac;
    bool failed = false;

    for (const auto& c : cases)
    {
        const int total = static_cast<int> (sr * c.seconds);

        MidiDelayProcessor proc;
        engineDefaults (proc);

        if (! c.colour)
            plainLoop (proc);

        setParam (proc, "sync", 0.0f);
        setParam (proc, "quality", c.quality);
        setParam (proc, "delayTime", 300.0f);
        setParam (proc, "feedback", c.feedback);
        setParam (proc, "mix", 50.0f);
        setParam (proc, "attack", 5.0f);
        setParam (proc, "release", 300.0f);
        setParam (proc, "character", c.character);
        setParam (proc, "age", c.age);

        // Stereo Ping-Pong (#55): одна нота в тишину — один голос в центре, бит-в-бит как
        // нулевой слот разлёта, на котором записаны эталоны. Хор пел бы её парой расстроенных
        // голосов. Аккорд в ping-pong расходится иначе, чем расходился по слотам, — эталон
        // chord переписан в сессии 22 вместе с уходом разлёта; прочие остались байт-в-байт.
        setParam (proc, "stereo", 2.0f);

        for (const auto& id : overrides.getAllKeys())
            setParam (proc, id.toRawUTF8(), overrides[id].getFloatValue());

        proc.setPlayConfigDetails (2, 2, sr, blockSize);
        proc.prepareToPlay (sr, blockSize);

        // Вход: пила 220 Гц на 0,4 с с фейдами по 5 мс, дальше тишина — повторы стоят
        // в выходе отдельно от сухого. Нота встаёт на 700-й сэмпл, внутрь второго блока:
        // смещение внутри блока тоже часть того, что держит эталон. Отпускается за 0,3 с
        // до конца, чтобы в эталон попал и спад.
        const int burst = static_cast<int> (sr * 0.4);
        const int fade = static_cast<int> (sr * 0.005);
        const int noteOn = 700;
        const int noteOff = total - static_cast<int> (sr * 0.3);

        juce::AudioBuffer<float> out (2, total);
        juce::AudioBuffer<float> block (2, blockSize);
        double phase = 0.0;

        for (int start = 0; start < total; start += blockSize)
        {
            const int n = juce::jmin (blockSize, total - start);

            for (int i = 0; i < n; ++i)
            {
                const int t = start + i;
                phase += 220.0 / sr;
                if (phase >= 1.0) phase -= 1.0;

                const double gain = t >= burst ? 0.0 : juce::jmin (1.0, juce::jmin (t, burst - t) / double (fade));
                const auto v = static_cast<float> (0.2 * gain * (2.0 * phase - 1.0));
                block.setSample (0, i, v);
                block.setSample (1, i, v);
            }

            juce::MidiBuffer midi;

            for (const int note : c.notes)
            {
                if (noteOn  >= start && noteOn  < start + n) midi.addEvent (juce::MidiMessage::noteOn  (1, note, 0.9f), noteOn  - start);
                if (noteOff >= start && noteOff < start + n) midi.addEvent (juce::MidiMessage::noteOff (1, note), noteOff - start);
            }

            // Последний блок короче: хост тоже так делает, и это часть сценария.
            juce::AudioBuffer<float> view (block.getArrayOfWritePointers(), 2, n);
            proc.processBlock (view, midi);

            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, start, block, ch, 0, n);
        }

        const auto file = dir.getChildFile (juce::String (c.name) + ".flac");

        if (update)
        {
            // 24 бита режут всё, что выше единицы, и эталон молча записал бы клип.
            std::printf ("  #34 %s: пик %.3f\n", c.name, out.getMagnitude (0, total));
            CHECK (out.getMagnitude (0, total) < 1.0f);

            dir.createDirectory();
            file.deleteFile();
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
            const auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                                  .withSampleRate (sr)
                                                                  .withNumChannels (2)
                                                                  .withBitsPerSample (24));
            CHECK (writer != nullptr);
            CHECK (writer->writeFromAudioSampleBuffer (out, 0, total));
            std::printf ("  #34 %s: эталон записан в %s\n", c.name, file.getFullPathName().toRawUTF8());
            continue;
        }

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr)
        {
            std::printf ("FAILED #34 %s: нет эталона %s\n"
                         "  запускать из корня репозитория; эталон заводится словом update\n",
                         c.name, file.getFullPathName().toRawUTF8());
            return 1;
        }

        if (reader->sampleRate != sr || reader->numChannels != 2 || reader->lengthInSamples != total)
        {
            std::printf ("FAILED #34 %s: эталон другой формы (%.0f Гц, %d кан., %lld сэмплов, ждали %.0f, 2, %d)\n"
                         "  сценарий изменён, а эталон нет\n",
                         c.name, reader->sampleRate, static_cast<int> (reader->numChannels),
                         static_cast<long long> (reader->lengthInSamples), sr, total);
            failed = true;
            continue;
        }

        juce::AudioBuffer<float> ref (2, total);
        reader->read (&ref, 0, total, 0, true, true);

        // Окна по 10 мс: одно число на файл утопило бы расхождение в тихом хвосте
        // под громким сухим. 10 мс короче любого повтора и длиннее периода 220 Гц.
        // Файл проходится целиком: первое расхождение показывает, где сломалось,
        // худшее окно — насколько.
        constexpr int window = 480;
        double worstMargin_dB = 1.0e9;
        int worstAt = 0, failAt = -1, failChannel = 0;
        double failError = 0.0, failRef = 0.0, failGot = 0.0;

        for (int from = 0; from + window <= total; from += window)
            for (int ch = 0; ch < 2; ++ch)
            {
                double e = 0.0, r = 0.0, g = 0.0;
                const auto* pr = ref.getReadPointer (ch, from);
                const auto* pg = out.getReadPointer (ch, from);

                for (int i = 0; i < window; ++i)
                {
                    const double d = static_cast<double> (pg[i]) - pr[i];
                    e += d * d;
                    r += static_cast<double> (pr[i]) * pr[i];
                    g += static_cast<double> (pg[i]) * pg[i];
                }

                const double errorRms = std::sqrt (e / window);
                const double refRms = std::sqrt (r / window);
                const double limit = std::max (refRms * relativeTolerance, absoluteFloor);
                const double margin_dB = 20.0 * std::log10 (limit / std::max (errorRms, 1.0e-30));

                if (margin_dB < worstMargin_dB)
                {
                    worstMargin_dB = margin_dB;
                    worstAt = from;
                }

                if (errorRms > limit && failAt < 0)
                {
                    failAt = from;
                    failChannel = ch;
                    failError = errorRms;
                    failRef = refRms;
                    failGot = std::sqrt (g / window);
                }
            }

        if (failAt < 0)
        {
            std::printf ("  #34 %s: совпадает с эталоном, запас до допуска %.0f dB (худшее окно %.2f с)\n",
                         c.name, worstMargin_dB, worstAt / sr);
            continue;
        }

        failed = true;
        const auto dB = [] (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-30)); };

        std::printf ("FAILED #34 %s: разошлось с %.3f с, канал %s\n"
                     "  ошибка %.1f dB от эталона при допуске %.0f dB; уровень эталона %.1f dBFS, сейчас %.1f dBFS\n"
                     "  худшее окно %.2f с, за допуском на %.1f dB\n",
                     c.name, failAt / sr, failChannel == 0 ? "L" : "R",
                     dB (failError / std::max (failRef, 1.0e-30)), tolerance_dB,
                     dB (failRef), dB (failGot), worstAt / sr, -worstMargin_dB);

        // Подсказка, что именно разошлось, — по 100 мс от места расхождения.
        // Коэффициент методом наименьших квадратов: если он один объясняет 99 % ошибки,
        // разошлась громкость или полярность, а не сам звук. Сдвиг по времени отдельно
        // не ищется: сухой идёт мимо дилея, целиком выход уехать не может, а сдвинутый
        // хвост под несдвинутым сухим — это и есть «форма». Проверено пробой delayTime.
        const int span = juce::jmin (static_cast<int> (sr * 0.1), total - failAt);
        double cross = 0.0, refEnergy = 0.0, errorEnergy = 0.0;

        for (int i = 0; i < span; ++i)
        {
            const double r = ref.getSample (failChannel, failAt + i);
            const double g = out.getSample (failChannel, failAt + i);
            cross += r * g;
            refEnergy += r * r;
            errorEnergy += (g - r) * (g - r);
        }

        const double gain = refEnergy > 0.0 ? cross / refEnergy : 0.0;
        double residual = 0.0;

        for (int i = 0; i < span; ++i)
        {
            const double d = out.getSample (failChannel, failAt + i) - gain * ref.getSample (failChannel, failAt + i);
            residual += d * d;
        }

        if (refEnergy < 1.0e-12)
            std::printf ("  в эталоне здесь тишина, а сейчас звук\n");
        else if (residual < 0.01 * errorEnergy)
            std::printf (gain < 0.0 ? "  перевернулась полярность, уровень %+.2f dB\n"
                                    : "  разошёлся уровень: %+.2f dB, звук тот же\n", dB (std::abs (gain)));
        else
            std::printf ("  разошлась форма, не уровень: высота, тайминг хвоста, фаза или окраска\n");
    }

    return failed ? 1 : 0;
}

int main (int argc, char* argv[])
{
    // Без буфера: в CI на Windows тест умер с кодом 127 через полминуты и не оставил
    // ни строки — буфер stdout при падении теряется, и не видно, на каком разделе.
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    countedThread = std::this_thread::get_id();

    juce::ScopedJuceInitialiser_GUI juceInit;

    // fromUTF8, а не конструктор из char*: тот трактует байты как ASCII, и путь
    // с кириллицей превращается в мохибейку ещё до открытия файла. Это та же ловушка,
    // что с текстами в окне плагина (CLAUDE.md), только с другой стороны — на входе.
    if (argc >= 3 && juce::String (argv[1]) == "--render")
    {
        // Хвост команды — набор слов без порядка: colour и shift ищутся где угодно
        // после имени файла. Позиционные флаги тут кончились бы «пустой строкой,
        // чтобы добраться до шестого аргумента».
        bool colour = false, formantShift = false, pluck = false;
        juce::StringPairArray overrides;

        for (int i = 5; i < argc; ++i)
        {
            const juce::String flag (juce::String::fromUTF8 (argv[i]));

            // Пробел внутри — это несколько ключей, слипшихся в один аргумент: так zsh
            // подставляет `$VAR` без кавычек. Рендер молча прочёл «preset=0 character=1
            // age=30» как пресет 0, и сессия 20 получила восемь одинаковых файлов.
            if (flag.containsChar (' '))
            {
                std::printf ("ключи слиплись в один аргумент: %s\n", argv[i]);
                return 1;
            }

            if (flag.contains ("="))
                overrides.set (flag.upToFirstOccurrenceOf ("=", false, false),
                               flag.fromFirstOccurrenceOf ("=", false, false));

            colour       = colour       || flag == "colour";
            formantShift = formantShift || flag == "shift";
            pluck        = pluck        || flag == "pluck";
        }

        return renderDemo (juce::String::fromUTF8 (argv[2]),
                           argc >= 4 ? juce::String::fromUTF8 (argv[3]) : juce::String ("hq"),
                           argc >= 5 ? juce::String::fromUTF8 (argv[4]) : juce::String(),
                           colour, formantShift, pluck, overrides);
    }

    if (argc >= 2 && juce::String (argv[1]) == "--bench")
        return benchmark (argc >= 3 ? juce::String (argv[2]) : juce::String ("hq"),
                          argc >= 4 ? juce::String (argv[3]) : juce::String ("hold"));

    if (argc >= 2 && juce::String (argv[1]) == "--regress")
    {
        bool update = false;
        juce::StringPairArray overrides;

        for (int i = 2; i < argc; ++i)
        {
            const juce::String arg (argv[i]);

            if (arg == "update")
                update = true;
            else if (arg.contains ("="))
                overrides.set (arg.upToFirstOccurrenceOf ("=", false, false),
                               arg.fromFirstOccurrenceOf ("=", false, false));
            else
            {
                std::printf ("непонятный аргумент: %s\n", argv[i]);
                return 1;
            }
        }

        return regression (update, overrides);
    }

    // Снимок окна в PNG (#26, #27). Не украшение: интерфейс иначе правится вслепую —
    // увидеть его можно только запустив хост, а хост в эстафету промптов не помещается.
    // Заодно это единственная проверка, что редактор создаётся, отрисовывается
    // и разрушается без падения, — pluginval то же самое делает дольше и молча.
    //   ./build/ProcessorTest_artefacts/Release/ProcessorTest --shot editor.png [пресет] [silent] [масштаб]
    if (argc >= 3 && juce::String (argv[1]) == "--shot")
    {
        MidiDelayProcessor proc;
        proc.prepareToPlay (48000.0, 512);

        if (argc >= 4)
            proc.setCurrentProgram (juce::String (argv[3]).getIntValue());

        // Аккорд без отпускания: снимок должен показывать занятые голоса, а не пустую
        // клавиатуру. Иначе половина того, ради чего #27 делалась, на картинке не видна.
        // Слово silent четвёртым аргументом снимает второе состояние — то, ради которого
        // #27 и заводилась: MIDI не доехал, и это должно быть видно с первого взгляда.
        // Хвостовые аргументы разбираются по виду, а не по месту: масштаб (#29)
        // нужен и с аккордом, и без него, а держать для этого пустышку в четвёртой
        // позиции — это способ однажды опечататься и не заметить.
        bool silent = false;
        float pixelScale = 1.0f;

        for (int i = 4; i < argc; ++i)
        {
            const juce::String arg (argv[i]);

            if (arg == "silent")
                silent = true;
            else if (arg == "retina")
                pixelScale = 2.0f;      // экран вдвое плотнее, окно того же размера (#29)
            else if (arg.getFloatValue() > 0.0f)
                proc.editorScale = arg.getFloatValue();
        }

        juce::AudioBuffer<float> block (2, 512);
        fillDC (block, 0.4f);
        juce::MidiBuffer chord;

        if (! silent)
            for (const int note : { 55, 58, 62, 65 })
                chord.addEvent (juce::MidiMessage::noteOn (1, note, 0.9f), 0);

        proc.processBlock (block, chord);

        // Шестьдесят блоков, а не пара: в Follow события едут в очереди на всю
        // латентность движка (ADR 0006), и на девяти блоках нота просто не успевала
        // прозвучать — снимок выходил с пустой клавиатурой и надписью «нет MIDI».
        for (int i = 0; i < 60; ++i)
        {
            juce::MidiBuffer none;
            fillDC (block, 0.4f);
            proc.processBlock (block, none);
        }

        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        CHECK (editor != nullptr);

        // Таймеры редактора должны успеть снять снимок голосов и разложить контекст
        // по режимам: без этого окно нарисовалось бы с пустой клавиатурой. Очередь
        // сообщений тут не крутится (модальные циклы в консольном приложении JUCE
        // выключены), поэтому таймеры дёргаются напрямую — подождав, пока настанет
        // их срок. Сто миллисекунд хватает обоим: 15 Гц у окна, 30 Гц у ленты.
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        juce::Timer::callPendingTimersSynchronously();

        // Плотность пикселей задаётся снимку, а не окну: Retina — это когда та же
        // точка раскладки рисуется четырьмя пикселями, а не когда окно становится
        // вдвое больше. Если бы что-то в оформлении было растровым или считалось
        // в целых пикселях, здесь оно поехало бы, а на 1.0 выглядело бы целым.
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(),
                                                            true, pixelScale);
        const auto file = juce::File::getCurrentWorkingDirectory()
                              .getChildFile (juce::String::fromUTF8 (argv[2]));
        file.deleteFile();

        std::unique_ptr<juce::FileOutputStream> out (file.createOutputStream());
        CHECK (out != nullptr);

        juce::PNGImageFormat png;
        CHECK (png.writeImageToStream (image, *out));

        std::printf ("wrote %s (%d x %d)\n", file.getFullPathName().toRawUTF8(),
                     image.getWidth(), image.getHeight());
        return 0;
    }

    // --- Раскладки шин ---------------------------------------------------------
    {
        MidiDelayProcessor proc;
        engineDefaults (proc);
        plainLoop (proc);
        using Set = juce::AudioChannelSet;

        const auto supported = [&proc] (const Set& in, const Set& out)
        {
            juce::AudioProcessor::BusesLayout l;
            l.inputBuses.add (in);
            l.outputBuses.add (out);
            return proc.checkBusesLayoutSupported (l);
        };

        CHECK (supported (Set::mono(),   Set::mono()));
        CHECK (supported (Set::stereo(), Set::stereo()));
        CHECK (supported (Set::mono(),   Set::stereo()));
        CHECK (! supported (Set::stereo(), Set::mono()));
        CHECK (! supported (Set::disabled(), Set::stereo()));
    }

    // --- Смещение дилея и ноль аллокаций ---------------------------------------
    {
        MidiDelayProcessor proc;
        engineDefaults (proc);
        plainLoop (proc);
        constexpr double sr = 48000.0;
        constexpr int blockSize = 16384;
        // 200 мс — ровно 9600 сэмплов на 48 кГц и ровно на сетке параметра (шаг 0.01 мс).
        // Некруглое время село бы между сэмплами, и импульс размазался бы интерполяцией.
        //
        // Не 10 мс, как было до #14: латентность движка вычитается из позиции чтения,
        // и при delay time меньше неё смещение уходит в минус, упирается в кламп,
        // а голос звучит позже заказанного. Минимальный осмысленный delay time равен
        // латентности движка; это #17. У Fast это 5762 сэмпла (120 мс), у HQ — 8640
        // (180 мс), и 200 мс с запасом больше обоих. Quality здесь не выставляется
        // намеренно: тест обязан проходить на движке по умолчанию.
        constexpr float delayMs = 200.0f;
        constexpr int delaySamples = 9600;

        // Параметры выставляются до prepareToPlay: тогда сглаживание стартует уже
        // в нужной точке и не размазывает импульс рампой.
        setParam (proc, "delayTime", delayMs);
        setParam (proc, "mix", 100.0f);
        setParam (proc, "feedback", 0.0f);
        setParam (proc, "outputGain", 0.0f);
        // Атака 1 мс — 48 сэмплов: к 480-му огибающая давно единица, и импульс
        // приходит неискажённым. Wet теперь собирается из голосов, без ноты его нет.
        setParam (proc, "attack", 1.0f);

        proc.setPlayConfigDetails (2, 2, sr, blockSize);
        proc.prepareToPlay (sr, blockSize);

        juce::AudioBuffer<float> buffer (2, blockSize);
        buffer.clear();
        buffer.setSample (0, 0, 1.0f);
        buffer.setSample (1, 0, 1.0f);

        runBlock (proc, buffer, noteOnAt (0));
        CHECK (allocations.load() == 0);

        // read() вызывается до записи текущего сэмпла, поэтому смещение ровно delaySamples,
        // а не delaySamples - 1, как было бы при чтении после записи (находка сессии 04).
        for (int ch = 0; ch < 2; ++ch)
        {
            int peak = -1;
            float peakValue = 0.0f;

            for (int i = 0; i < blockSize; ++i)
                if (std::abs (buffer.getSample (ch, i)) > peakValue)
                {
                    peakValue = std::abs (buffer.getSample (ch, i));
                    peak = i;
                }

            CHECK (peak == delaySamples);
            CHECK (peakValue > 0.99f);
        }

        // Смена sample rate на лету: не роняет, хвост уходит в тишину, а не в мусор.
        proc.prepareToPlay (96000.0, 256);
        juce::AudioBuffer<float> small (2, 256);
        small.clear();
        runBlock (proc, small);
        CHECK (allocations.load() == 0);

        for (int i = 0; i < 256; ++i)
            CHECK (small.getSample (0, i) == 0.0f);

        // Моно→стерео: правый канал приходит с мусором, dry обязан в нём появиться.
        // mix выставляется ДО prepareToPlay: иначе сглаживание поедет с прошлого
        // значения, и dry на выходе будет неполным весь первый блок.
        setParam (proc, "mix", 0.0f);
        proc.setPlayConfigDetails (1, 2, sr, blockSize);
        proc.prepareToPlay (sr, blockSize);

        juce::AudioBuffer<float> monoIn (2, blockSize);
        monoIn.clear();
        for (int i = 0; i < blockSize; ++i)
        {
            monoIn.setSample (0, i, 0.5f);
            monoIn.setSample (1, i, -99.0f);   // мусор от хоста
        }

        runBlock (proc, monoIn);
        CHECK (allocations.load() == 0);
        CHECK (std::abs (monoIn.getSample (1, blockSize - 1) - 0.5f) < 1.0e-4f);
    }

    // --- Микс, гейн и обход без щелчков (#11) ----------------------------------
    {
        MidiDelayProcessor proc;
        engineDefaults (proc);
        plainLoop (proc);
        constexpr double sr = 48000.0;
        constexpr int blockSize = 16384;
        constexpr int delaySamples = 7776;   // 162 мс на 48 кГц, ровно на сетке параметра

        // 250 Гц выбрано не случайно: период ровно 192 сэмпла, а 7776 — это 40,5 периода.
        // Значит wet приходит в противофазе к dry, и любой кроссфейд между ними —
        // настоящий переход, а не переход сигнала в самого себя. Нота — 60, то есть
        // ровно Root Key по умолчанию: ratio 1, и питчер вырождается в чистую задержку.
        constexpr float freq = 250.0f;
        constexpr float amp  = 0.25f;

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::AudioBuffer<float> reference (2, blockSize);
        int phase = 0;

        // Заполняет блок синусом с непрерывной фазой и двигает счётчик фазы.
        const auto nextSine = [&buffer, &phase]
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const float s = amp * std::sin (juce::MathConstants<float>::twoPi
                                                * freq * (float) (phase + i) / (float) sr);
                buffer.setSample (0, i, s);
                buffer.setSample (1, i, s);
            }
            phase += blockSize;
        };

        // prepareToPlay чистит и кольцо, и голоса, поэтому ноту надо давать заново
        // после каждого prepare: без живого голоса wet теперь ноль по определению.
        const auto prepare = [&proc]
        {
            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);
        };

        // Максимальный шаг между соседними сэмплами. Сам синус даёт 0,033 на сэмпл
        // при амплитуде 1,0, так что порог 0,1 не задевает сигнал, но ловит разрыв.
        float lastSample = 0.0f;
        float maxStep = 0.0f;
        const auto measureSteps = [&buffer, &lastSample, &maxStep]
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const float s = buffer.getSample (0, i);
                maxStep = juce::jmax (maxStep, std::abs (s - lastSample));
                lastSample = s;
            }
        };

        const auto sameAsReference = [&buffer, &reference]
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    if (buffer.getSample (ch, i) != reference.getSample (ch, i))
                        return false;

            return true;
        };

        // 1. mix = 0 — бит-в-бит. Первый блок холостой: пока сглаживание едет
        //    к своей цели, равенства нет, и это не баг.
        // 162 мс — ближайшее к латентности движка время, дающее половину периода
        // 250 Гц. Меньше нельзя: 120 мс это сама латентность, запас нужен.
        //
        // Quality = Fast явно. Этот тест про кроссфейды микса, гейна и обхода, а не
        // про питчинг, и держится он на противофазе wet к dry — то есть на конкретном
        // времени 162 мс. У HQ латентность 180 мс, 162 в неё не влезает, и хвост поехал
        // бы позже заказанного. Двигать время значило бы пересчитывать всю конструкцию
        // теста ради проверки, к движку не относящейся.
        setParam (proc, "quality", 0.0f);
        setParam (proc, "delayTime", 162.0f);
        setParam (proc, "feedback", 0.0f);
        setParam (proc, "outputGain", 0.0f);
        setParam (proc, "mix", 0.0f);
        setParam (proc, "bypass", 0.0f);
        setParam (proc, "attack", 1.0f);
        prepare();

        nextSine();
        runBlock (proc, buffer, noteOnAt (0));

        nextSine();
        reference.makeCopyOf (buffer);
        runBlock (proc, buffer);
        CHECK (allocations.load() == 0);
        CHECK (sameAsReference());

        // 2. Выравнивание dry и wet: при mix = 50 % пик dry на нуле, пик wet ровно
        //    на delaySamples. Именно здесь видно, что латентность питчера спрятана
        //    в delay time: движок задерживает на 5762 сэмпла, голос читает кольцо
        //    на 5762 ближе, и снаружи хвост приходит ровно на 7776-м.
        setParam (proc, "mix", 50.0f);
        prepare();

        buffer.clear();
        buffer.setSample (0, 0, 1.0f);
        buffer.setSample (1, 0, 1.0f);
        runBlock (proc, buffer, noteOnAt (0));

        CHECK (std::abs (buffer.getSample (0, 0) - 0.5f) < 1.0e-4f);
        CHECK (std::abs (buffer.getSample (0, delaySamples) - 0.5f) < 1.0e-4f);

        for (int i = 1; i < blockSize; ++i)
            if (i != delaySamples)
                CHECK (std::abs (buffer.getSample (0, i)) < 1.0e-4f);

        // 3. Щелчки: mix 0 -> 100 % и output gain -24 -> +12 dB посреди сигнала.
        //    Без сглаживания выход прыгнул бы с 0,016 до 1,0 за один сэмпл.
        setParam (proc, "mix", 0.0f);
        setParam (proc, "outputGain", -24.0f);
        prepare();

        phase = 0;
        for (int b = 0; b < 4; ++b)                                    // прогрев кольца
            { nextSine(); runBlock (proc, buffer, b == 0 ? noteOnAt (0) : juce::MidiBuffer {}); }

        lastSample = buffer.getSample (0, blockSize - 1);
        maxStep = 0.0f;
        setParam (proc, "mix", 100.0f);
        setParam (proc, "outputGain", 12.0f);

        // Сглаживание 50 мс — это 2400 сэмплов, то есть кончается внутри первого блока.
        for (int b = 0; b < 8; ++b) { nextSine(); runBlock (proc, buffer); measureSteps(); }

        CHECK (allocations.load() == 0);
        CHECK (maxStep < 0.1f);

        // 4. Обход и возврат. Гейн уже +12 dB — вернём в ноль, иначе сам обход
        //    даст честный скачок громкости, а проверяем мы не его.
        setParam (proc, "outputGain", 0.0f);
        prepare();

        phase = 0;
        for (int b = 0; b < 4; ++b)
            { nextSine(); runBlock (proc, buffer, b == 0 ? noteOnAt (0) : juce::MidiBuffer {}); }

        lastSample = buffer.getSample (0, blockSize - 1);
        maxStep = 0.0f;
        setParam (proc, "bypass", 1.0f);

        for (int b = 0; b < 4; ++b)
        {
            nextSine();
            reference.makeCopyOf (buffer);
            runBlock (proc, buffer);
            measureSteps();
        }

        CHECK (maxStep < 0.1f);
        CHECK (sameAsReference());   // обход доехал: выход бит-в-бит равен входу

        maxStep = 0.0f;
        setParam (proc, "bypass", 0.0f);

        for (int b = 0; b < 4; ++b) { nextSine(); runBlock (proc, buffer); measureSteps(); }

        CHECK (allocations.load() == 0);
        CHECK (maxStep < 0.1f);

        // Кольцо во время обхода продолжали писать, значит хвост вернулся без дыры:
        // wet в противофазе к dry, при mix = 100 % это ровно -amp * sin.
        for (int i = 0; i < blockSize; ++i)
        {
            const float expected = -amp * std::sin (juce::MathConstants<float>::twoPi
                                                    * freq * (float) (phase - blockSize + i) / (float) sr);
            CHECK (std::abs (buffer.getSample (0, i) - expected) < 1.0e-3f);
        }

        // 5. Длина хвоста: круг умножить на число кругов до -60 dB, с потолком.
        setParam (proc, "delayTime", 400.0f);
        setParam (proc, "feedback", 0.0f);
        CHECK (std::abs (proc.getTailLengthSeconds() - 0.4) < 1.0e-6);

        setParam (proc, "feedback", 50.0f);   // 0,5^n = 0,001 -> n = 9,97
        CHECK (std::abs (proc.getTailLengthSeconds() - 0.4 * 9.9658) < 0.01);

        setParam (proc, "delayTime", 2000.0f);
        setParam (proc, "feedback", 95.0f);   // 269 с честных -> потолок
        CHECK (proc.getTailLengthSeconds() == 20.0);
    }

    // --- Sample-accurate MIDI и голоса (#12, #13) ------------------------------
    {
        constexpr double sr = 48000.0;
        constexpr int delaySamples = 9600;   // 200 мс на 48 кГц, больше латентности питчера

        // Общая настройка: слышны только голоса (mix 100 %), огибающая короткая.
        // Параметры выставляются до prepareToPlay — тогда сглаживание стоит на цели
        // с первого сэмпла и не мешает сравнивать блоки разного размера.
        const auto setup = [] (MidiDelayProcessor& proc, int blockSize,
                               float attackMs = 1.0f, float releaseMs = 300.0f)
        {
            setParam (proc, "delayTime", 200.0f);
            setParam (proc, "feedback", 0.0f);
            setParam (proc, "mix", 100.0f);
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "bypass", 0.0f);
            setParam (proc, "attack", attackMs);
            setParam (proc, "release", releaseMs);
            setParam (proc, "voices", 8.0f);

            // Ширина в ноль: эти тесты меряют огибающие и кражу, а не панораму,
            // и разведённые по стерео голоса складывались бы в другое число.
            // Раскидка проверяется отдельно, ниже в этом же файле.
            setParam (proc, "width", 0.0f);
            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);
        };

        // 1. Голос стартует ровно на сэмпле события. Вход — постоянная единица, поэтому
        //    на выходе видно ровно огибающую: до события она обязана быть нулём, а на
        //    самом событии — уже нет. Точность проверки — один сэмпл.
        //
        //    Прогрев кольца теперь длиннее одного блока: голос читает не только на
        //    delay time назад, но и ещё на латентность движка сверх того — питчер при
        //    старте ноты заливает своё окно историей. Итого 9600 + 5762 сэмпла.
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            constexpr int blockSize = 512;
            juce::AudioBuffer<float> buffer (2, blockSize);

            setup (proc, blockSize);
            fillDC (buffer);
            runBlock (proc, buffer);

            // Без нот хвоста нет вообще: wet теперь это сумма голосов, а не отвод кольца.
            for (int i = 0; i < blockSize; ++i)
                CHECK (buffer.getSample (0, i) == 0.0f);

            for (const int onset : { 100, 300 })
            {
                setup (proc, blockSize);            // сброс кольца и голосов

                for (int b = 0; b < 40; ++b)        // прогрев: 20480 сэмплов истории
                    { fillDC (buffer); runBlock (proc, buffer); }

                fillDC (buffer); runBlock (proc, buffer, noteOnAt (onset));

                for (int i = 0; i < onset; ++i)
                    CHECK (buffer.getSample (0, i) == 0.0f);

                CHECK (buffer.getSample (0, onset) > 0.0f);
            }
        }

        // 2. Главный тест сессии: одна и та же партитура, нарезанная блоками 64, 128,
        //    512 и 2048, обязана дать бит-в-бит один и тот же выход. Сравнение точное,
        //    а не с порогом: всё, что едет во времени (сглаживание, огибающие, кольцо),
        //    считается по сэмплам, а delay time стоит ровно на 480 — дробной позиции
        //    чтения, которая округлялась бы по-разному, здесь взяться неоткуда.
        {
            constexpr int total = 32768;   // делится на все четыре размера блока
            // Партитура сдвинута вглубь прогона: голос читает на delay time плюс
            // латентность движка назад, и на первых 15 тысячах сэмплов ему достался
            // бы ноль. Сравнивать четыре тишины — не тест.
            constexpr int scoreBase = 16384;
            juce::AudioBuffer<float> source (2, total);

            for (int i = 0; i < total; ++i)
            {
                // Фаза в double: 3100 * 32767 уже не помещается в мантиссу float.
                const auto s = (float) (0.4 * std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / sr)
                                      + 0.1 * std::sin (juce::MathConstants<double>::twoPi * 3100.0 * i / sr));
                source.setSample (0, i, s);
                source.setSample (1, i, s * 0.5f);   // каналы разные: моно-сумма голоса тоже под проверкой
            }

            struct Event { int sample; juce::MidiMessage message; };
            const std::vector<Event> score
            {
                { scoreBase + 100,  juce::MidiMessage::noteOn (1, 60, 1.0f) },
                { scoreBase + 777,  juce::MidiMessage::noteOn (1, 64, 0.6f) },
                { scoreBase + 1500, juce::MidiMessage::controllerEvent (1, 64, 127) },   // педаль вниз
                { scoreBase + 1501, juce::MidiMessage::pitchWheel (1, 12000) },          // незнакомое — мимо
                { scoreBase + 2000, juce::MidiMessage::noteOff (1, 60) },                // держится педалью
                { scoreBase + 2500, juce::MidiMessage::controllerEvent (1, 64, 0) },     // педаль вверх
                { scoreBase + 3000, juce::MidiMessage::noteOn (1, 67, 0.9f) },
                { scoreBase + 3333, juce::MidiMessage::noteOn (1, 72, 0.0f) },           // velocity 0 = note off
                { scoreBase + 3500, juce::MidiMessage::allNotesOff (1) },
            };

            juce::AudioBuffer<float> reference (2, total);

            for (const int blockSize : { 64, 128, 512, 2048 })
            {
                MidiDelayProcessor proc;
                engineDefaults (proc);
                plainLoop (proc);
                setup (proc, blockSize);

                // mix и gain выставляются после prepareToPlay и едут к цели первые
                // 50 мс прогона: под сравнение попадает и сглаживание, а не только
                // статика. Сломать их сегментацией нельзя по построению — они считаются
                // одним проходом на весь блок, вне сегментов; проверено регрессом.
                setParam (proc, "mix", 80.0f);
                setParam (proc, "outputGain", 6.0f);

                juce::AudioBuffer<float> out (2, total);
                juce::AudioBuffer<float> block (2, blockSize);

                for (int start = 0; start < total; start += blockSize)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        block.copyFrom (ch, 0, source, ch, start, blockSize);

                    juce::MidiBuffer midi;
                    for (const auto& e : score)
                        if (e.sample >= start && e.sample < start + blockSize)
                            midi.addEvent (e.message, e.sample - start);

                    runBlock (proc, block, midi);

                    for (int ch = 0; ch < 2; ++ch)
                        out.copyFrom (ch, start, block, ch, 0, blockSize);
                }

                if (blockSize == 64)
                {
                    reference.makeCopyOf (out);

                    // Партитура обязана быть слышна, иначе сравнивались бы четыре тишины.
                    CHECK (reference.getMagnitude (0, total) > 0.1f);
                }
                else
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < total; ++i)
                            CHECK (out.getSample (ch, i) == reference.getSample (ch, i));
                }
            }

            CHECK (allocations.load() == 0);
        }

        // 3. Аккорд, кража и all-notes-off. Вход — постоянная единица, поэтому каждый
        //    живой голос добавляет к выходу ровно свою огибающую, и число голосов
        //    читается прямо с выхода. Атака 10 мс, чтобы шаг одновременных атак
        //    оставался мелким и порог на щелчки мерил кражу, а не игру аккордом.
        //
        //    Все ноты — 60, то есть Root Key: ratio 1, и постоянный вход доезжает
        //    до выхода нетронутым. С транспонированием так больше нельзя. Два ридера
        //    varispeed читают одно и то же, и на постоянном сигнале они полностью
        //    коррелированы: equal-power складывает их не в 1, а в gA + gB, то есть
        //    до +3 dB в середине кроссфейда. Это не баг — это цена equal-power на
        //    коррелированном входе, и DC как пробник для сдвинутого голоса не годится.
        //    Раздача голосов от номера ноты всё равно не зависит (findVoiceFor его
        //    игнорирует), так что унисон здесь ничего не теряет.
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            constexpr int blockSize = 512;
            setup (proc, blockSize, 10.0f, 300.0f);

            juce::AudioBuffer<float> buffer (2, blockSize);
            float lastSample = 0.0f;
            float maxStep = 0.0f;

            const auto dcBlock = [&] (const juce::MidiBuffer& midi = {}, bool measure = false)
            {
                fillDC (buffer);
                runBlock (proc, buffer, midi);

                for (int i = 0; i < blockSize; ++i)
                {
                    const float value = buffer.getSample (0, i);
                    if (measure)
                        maxStep = juce::jmax (maxStep, std::abs (value - lastSample));
                    lastSample = value;
                }
            };

            for (int b = 0; b < 40; ++b) dcBlock();   // прогрев кольца, 20480 сэмплов

            juce::MidiBuffer chord;
            for (int n = 0; n < 4; ++n)
                chord.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), n * 10);

            dcBlock (chord);
            dcBlock();
            CHECK (std::abs (buffer.getSample (0, blockSize - 1) - 4.0f) < 1.0e-3f);

            // Ещё пять нот: девятая обязана украсть голос, а не упасть и не потеряться.
            juce::MidiBuffer more;
            for (int n = 0; n < 5; ++n)
                more.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), n * 10);

            maxStep = 0.0f;
            dcBlock (more, true);
            dcBlock ({}, true);

            // Пул полон: восемь голосов, ни больше (девятая нота села в чужой), ни меньше.
            CHECK (std::abs (buffer.getSample (0, blockSize - 1) - 8.0f) < 1.0e-3f);
            // 5 одновременных атак по 10 мс дают 0,010 на сэмпл, fade-out кражи 5 мс —
            // ещё 0,004. Порог 0,05 оставляет запас втрое и ловит любой честный разрыв.
            CHECK (maxStep < 0.05f);

            // All-notes-off гасит через release, а не щелчком.
            juce::MidiBuffer panic;
            panic.addEvent (juce::MidiMessage::allNotesOff (1), 0);

            maxStep = 0.0f;
            dcBlock (panic, true);
            for (int b = 0; b < 30; ++b) dcBlock ({}, true);   // release 300 мс = 14400 сэмплов

            CHECK (maxStep < 0.05f);
            CHECK (buffer.getSample (0, blockSize - 1) == 0.0f);   // хвост доехал ровно до нуля
            CHECK (allocations.load() == 0);
        }

        // 4. Педаль сустейна: note off при нажатой педали ничего не гасит, гасит подъём.
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            constexpr int blockSize = 512;
            setup (proc, blockSize, 1.0f, 5.0f);   // release 5 мс — укладывается в один блок

            juce::AudioBuffer<float> buffer (2, blockSize);
            const auto dcBlock = [&] (const juce::MidiBuffer& midi = {})
            {
                fillDC (buffer);
                runBlock (proc, buffer, midi);
            };

            for (int b = 0; b < 40; ++b) dcBlock();   // прогрев кольца, 20480 сэмплов

            juce::MidiBuffer pedalAndNote;
            pedalAndNote.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);
            pedalAndNote.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 10);
            dcBlock (pedalAndNote);

            juce::MidiBuffer release;
            release.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            dcBlock (release);
            CHECK (std::abs (buffer.getSample (0, blockSize - 1) - 1.0f) < 1.0e-3f);

            juce::MidiBuffer pedalUp;
            pedalUp.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0);
            dcBlock (pedalUp);
            CHECK (buffer.getSample (0, blockSize - 1) == 0.0f);
        }

        // 5. Незнакомые сообщения не роняют плагин и не будят голоса.
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            constexpr int blockSize = 512;
            setup (proc, blockSize);

            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer junk;
            junk.addEvent (juce::MidiMessage::pitchWheel (1, 0), 0);
            junk.addEvent (juce::MidiMessage::channelPressureChange (1, 90), 5);
            junk.addEvent (juce::MidiMessage::aftertouchChange (1, 60, 40), 7);
            junk.addEvent (juce::MidiMessage::programChange (1, 12), 9);
            junk.addEvent (juce::MidiMessage::controllerEvent (1, 74, 55), 11);
            junk.addEvent (juce::MidiMessage::midiStart(), 13);
            const juce::uint8 sysexData[] = { 0x11, 0x22, 0x33 };
            junk.addEvent (juce::MidiMessage::createSysExMessage (sysexData, 3), 300);
            // Событие за пределами блока: хост так делать не должен, но кламп это ловит.
            junk.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), blockSize + 100);

            fillDC (buffer);
            runBlock (proc, buffer, junk);
            CHECK (allocations.load() == 0);

            for (int i = 0; i < blockSize; ++i)
                CHECK (std::isfinite (buffer.getSample (0, i)));
        }
    }

    // --- Маппинг ноты в pitch ratio (#15) --------------------------------------
    // Пробный тон — 500 Гц. Это не произвол: в полуокне varispeed (5760 сэмплов при
    // 48 кГц и окне 240 мс) укладывается ровно 60 его периодов, и только на таких
    // частотах он сдвигает без расстройки квантования. Разбор механизма —
    // в Source/DSP/test_pitch_shifter.cpp, раздел 4. HQ этой хитрости не требует,
    // но и не мешает ей: проверка идёт на движке по умолчанию.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 2048;
        constexpr int blocks = 32;
        constexpr int total = blockSize * blocks;
        constexpr int measureFrom = total / 2;
        constexpr double f0 = 500.0;

        /** Частота хвоста для ноты. rootKeyAfter крутится на лету посреди прогона,
            ровно в начале измеряемой половины: уже звучащий голос обязан этого не
            заметить — ratio считается в момент note on и живёт в голосе до её конца. */
        const auto tailFrequency = [&] (int note, float rootKey, float range,
                                        float rootKeyAfter, double expected)
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            setParam (proc, "delayTime", 200.0f);   // больше латентности обоих движков
            setParam (proc, "feedback", 0.0f);
            setParam (proc, "mix", 100.0f);        // на выходе только хвост, без dry
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "bypass", 0.0f);
            setParam (proc, "attack", 1.0f);
            setParam (proc, "release", 300.0f);
            setParam (proc, "rootKey", rootKey);
            setParam (proc, "pitchRange", range);

            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            juce::AudioBuffer<float> block (2, blockSize);
            juce::AudioBuffer<float> out (2, total);

            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    // Фаза считается в double нарочно: 500 * 65535 уже не помещается
                    // в мантиссу float, и синус на хвосте прогона поехал бы по частоте.
                    const auto value = (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi
                                                                * f0 * (b * blockSize + i) / sr));
                    block.setSample (0, i, value);
                    block.setSample (1, i, value);
                }

                runBlock (proc, block, b == 0 ? noteOnAt (0, note) : juce::MidiBuffer {});

                for (int ch = 0; ch < 2; ++ch)
                    out.copyFrom (ch, b * blockSize, block, ch, 0, blockSize);

                if (b * blockSize == measureFrom - blockSize)
                    setParam (proc, "rootKey", rootKeyAfter);
            }

            // Хвост обязан быть слышен: иначе искали бы пик в тишине.
            CHECK (out.getMagnitude (0, measureFrom, total - measureFrom) > 0.1f);

            return dominantFrequency (out, measureFrom, total - measureFrom, expected, 60.0, sr);
        };

        // Нота, равная Root Key: хвост без транспонирования.
        CHECK (std::abs (tailFrequency (60, 0.0f, 12.0f, 0.0f, f0) - f0) < 2.0);

        // Октава вверх — ровно удвоение частоты, октава вниз — ровно половина.
        CHECK (std::abs (tailFrequency (72, 0.0f, 12.0f, 0.0f, 2.0 * f0) - 2.0 * f0) < 2.0);
        CHECK (std::abs (tailFrequency (48, 0.0f, 12.0f, 0.0f, 0.5 * f0) - 0.5 * f0) < 2.0);

        // За границей Pitch Range — clamp: две октавы вверх при диапазоне 12 дают ту же
        // высоту, что и одна. Перенос октавами внутрь диапазона дал бы здесь f0.
        CHECK (std::abs (tailFrequency (84, 0.0f, 12.0f, 0.0f, 2.0 * f0) - 2.0 * f0) < 2.0);

        // Root Key реально участвует: та же нота от D (индекс 2) даёт +10 полутонов.
        const double tenSemitones = f0 * std::pow (2.0, 10.0 / 12.0);
        CHECK (std::abs (tailFrequency (72, 2.0f, 12.0f, 2.0f, tenSemitones) - tenSemitones) < 2.0);

        // И главное: смена Root Key на лету не трогает уже звучащий голос. Нота 72
        // взята от C, потом Root уезжает на F — от F она дала бы +7 полутонов, но
        // голос обязан продолжать петь свои +12.
        CHECK (std::abs (tailFrequency (72, 0.0f, 12.0f, 5.0f, 2.0 * f0) - 2.0 * f0) < 2.0);
    }

    // --- Форманты и предел тональности (#24) -----------------------------------
    // Две правки питчера, обе слышны только на больших сдвигах и обе молча
    // отваливаются, если перепутать аргумент. Предел тональности проверяется точным
    // числом: выше него частоты не умножаются, а сдвигаются на постоянную величину,
    // и на октаве вверх тон 6 кГц обязан прийти не на 12 кГц, а ниже — иначе шипящие
    // уезжают в свист. Форманты — центром тяжести спектра: при Hold спектральная
    // огибающая остаётся на месте, при Shift едет вместе с высотой.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 4096;
        constexpr int blocks = 24;
        constexpr int total = blockSize * blocks;
        constexpr int measureFrom = total / 2;

        /** Хвост на ноте 72 (ровно октава вверх от Root). Материал задаётся снаружи:
            синус меряет частоту, пила с наклоном спектра — центр тяжести. */
        const auto tail = [&] (bool formantHold, const std::function<float (int)>& material,
                               juce::AudioBuffer<float>& out)
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            setParam (proc, "quality", 1.0f);       // форманты живут только в HQ
            setParam (proc, "formants", formantHold ? 1.0f : 0.0f);
            setParam (proc, "delayTime", 200.0f);
            setParam (proc, "feedback", 0.0f);
            setParam (proc, "mix", 100.0f);
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "bypass", 0.0f);
            setParam (proc, "attack", 1.0f);
            setParam (proc, "release", 300.0f);
            setParam (proc, "width", 0.0f);

            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            juce::AudioBuffer<float> block (2, blockSize);
            out.setSize (2, total);

            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const float v = material (b * blockSize + i);
                    block.setSample (0, i, v);
                    block.setSample (1, i, v);
                }

                runBlock (proc, block, b == 0 ? noteOnAt (0, 72) : juce::MidiBuffer {});
                CHECK (allocations.load() == 0);   // форманты не аллоцируют в потоке

                for (int ch = 0; ch < 2; ++ch)
                    out.copyFrom (ch, b * blockSize, block, ch, 0, blockSize);
            }

            CHECK (out.getMagnitude (0, measureFrom, total - measureFrom) > 0.02f);
        };

        /** Доля энергии выше 1,2 кГц. Это и есть «бурундук», выраженный числом:
            транспонирование без компенсации тащит формантную область вверх, и энергия
            перетекает в верх спектра. Центр тяжести здесь хуже — его держат внизу
            первые две гармоники, и он почти не двигается. Считается по гармоникам
            хвоста, а не по равномерной сетке: сетка попадает между ними, и результат
            начинает зависеть от того, куда она легла. */
        const auto highEnergyShare = [&] (const juce::AudioBuffer<float>& x, double fundamental)
        {
            double low = 0.0, high = 0.0;

            // До 5 кГц: выше начинает работать предел тональности, и там гармоники
            // уже не кратны основному тону (проверка 1).
            for (double f = fundamental; f < 5000.0; f += fundamental)
            {
                const double a = amplitudeAt (x, measureFrom, total - measureFrom, f, sr);

                (f > 1200.0 ? high : low) += a * a;
            }

            return low > 0.0 ? high / low : 0.0;
        };

        juce::AudioBuffer<float> out;

        // 1. Предел тональности. Тон 6 кГц на октаве вверх: библиотека считает предел
        //    как 8000/sqrt(2) = 5657 Гц, и всё, что выше, переносится сдвигом на эту же
        //    величину. Значит 6000 -> 11657, а не 12000. Разница в 343 Гц — это и есть
        //    разница между «шипящая переехала» и «шипящая ушла в свист».
        {
            const double tone = 6000.0;
            const double limit = 8000.0 / std::sqrt (2.0);
            const double expected = tone + limit;

            tail (true, [tone] (int n) { return static_cast<float> (0.4 * std::sin (
                juce::MathConstants<double>::twoPi * tone * n / sr)); }, out);

            const double got = dominantFrequency (out, measureFrom, total - measureFrom,
                                                  expected, 500.0, sr);

            CHECK (std::abs (got - expected) < 60.0);
            CHECK (got < 2.0 * tone - 200.0);   // и это точно не честное удвоение
        }

        // 2. Форманты. Материал — модель гласной: пила 120 Гц через три резонатора
        //    на 700, 1200 и 2600 Гц. Пила через простой фильтр тут не годится вовсе:
        //    у неё нет формант, и компенсации нечего держать — замер на таком материале
        //    показывает что угодно, кроме того, что проверяется (проверено, сессия 13).
        {
            const auto vowel = [] (int n)
            {
                static double phase = 0.0;
                static double y1[3] {}, y2[3] {};

                if (n == 0) { phase = 0.0; y1[0] = y1[1] = y1[2] = y2[0] = y2[1] = y2[2] = 0.0; }

                phase += 120.0 / sr;
                if (phase >= 1.0) phase -= 1.0;

                const double source = 0.2 * (2.0 * phase - 1.0);
                static constexpr double formantHz[3] { 700.0, 1200.0, 2600.0 };
                static constexpr double weight[3] { 1.0, 0.6, 0.3 };
                double sum = 0.0;

                for (int k = 0; k < 3; ++k)
                {
                    constexpr double r = 0.97;
                    const double w = juce::MathConstants<double>::twoPi * formantHz[k] / sr;
                    const double y = source * (1.0 - r) + 2.0 * r * std::cos (w) * y1[k] - r * r * y2[k];

                    y2[k] = y1[k];
                    y1[k] = y;
                    sum += weight[k] * y;
                }

                return static_cast<float> (sum);
            };

            tail (true, vowel, out);
            const double held = highEnergyShare (out, 240.0);

            tail (false, vowel, out);
            const double shifted = highEnergyShare (out, 240.0);

            std::printf ("  форманты на октаве вверх: энергия выше 1,2 кГц — "
                         "Hold %.3f, Shift %.3f\n", held, shifted);

            // Замерено: Hold 0,06, Shift 0,27 — вчетверо. Порог вдвое ниже
            // измеренного, чтобы проверка ловила отвал компенсации, а не дрожала
            // от версии библиотеки.
            CHECK (shifted > held * 2.0);
        }
    }

    // --- Состояние -------------------------------------------------------------
    {
        MidiDelayProcessor proc;
        engineDefaults (proc);
        plainLoop (proc);
        setParam (proc, "delayTime", 250.0f);
        setParam (proc, "feedback", 40.0f);

        juce::MemoryBlock saved;
        proc.getStateInformation (saved);

        setParam (proc, "delayTime", 900.0f);
        proc.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        CHECK (std::abs (proc.apvts.getRawParameterValue ("delayTime")->load() - 250.0f) < 0.01f);

        // Номер версии обязан лежать в корне состояния.
        {
            auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(),
                                                               static_cast<int> (saved.getSize()));
            CHECK (xml != nullptr && xml->hasAttribute ("stateVersion"));
        }

        // Мусор вместо XML, обрезанные данные и пустота — молча игнорируются.
        const char garbage[] = "this is not a plugin state at all";
        proc.setStateInformation (garbage, sizeof (garbage));
        proc.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()) / 2);
        proc.setStateInformation (nullptr, 0);
        CHECK (std::abs (proc.apvts.getRawParameterValue ("delayTime")->load() - 250.0f) < 0.01f);

        // Состояние от будущей версии: незнакомый параметр и незнакомый атрибут.
        juce::ValueTree future ("MidiDelayState");
        future.setProperty ("stateVersion", 99, nullptr);
        future.setProperty ("somethingNew", "whatever", nullptr);
        juce::ValueTree param ("PARAM");
        param.setProperty ("id", "parameterFromTheFuture", nullptr);
        param.setProperty ("value", 0.5f, nullptr);
        future.appendChild (param, nullptr);

        juce::MemoryBlock futureBlock;
        if (auto xml = future.createXml())
            juce::AudioProcessor::copyXmlToBinary (*xml, futureBlock);

        proc.setStateInformation (futureBlock.getData(), static_cast<int> (futureBlock.getSize()));

        // Галка после восстановления — ровная единица, а не сырое число хоста (#54).
        // Шаги те же, что у pluginval: состояние снято при «включено», хост пишет в галку
        // 0,69 — тоже «включено», — и состояние возвращается.
        {
            auto* sync = proc.apvts.getParameter ("sync");
            sync->setValueNotifyingHost (1.0f);

            juce::MemoryBlock withSync;
            proc.getStateInformation (withSync);

            sync->setValue (0.69f);
            proc.setStateInformation (withSync.getData(), static_cast<int> (withSync.getSize()));
            CHECK (sync->getValue() == 1.0f);
        }
    }

    // --- Переключатель Quality (#38) -------------------------------------------
    // Движки различаются латентностью: Fast 5762 сэмпла, HQ 8640. Delay time 150 мс
    // (7200 сэмплов) лежит ровно между ними — у Fast смещение чтения остаётся
    // положительным и хвост приходит на 7200-м, у HQ упирается в кламп и хвост
    // приходит на 8640-м, то есть позже заказанного. Значит позиция пика прямо
    // показывает, какой движок реально отработал: это и есть проверка латча.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 16384;

        const auto tailPeak = [] (float quality)
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            setParam (proc, "quality", quality);
            setParam (proc, "delayTime", 150.0f);
            setParam (proc, "mix", 100.0f);
            setParam (proc, "feedback", 0.0f);
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "attack", 1.0f);

            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            juce::AudioBuffer<float> buffer (2, blockSize);
            buffer.clear();
            buffer.setSample (0, 0, 1.0f);
            buffer.setSample (1, 0, 1.0f);

            runBlock (proc, buffer, noteOnAt (0));
            CHECK (allocations.load() == 0);

            int peak = -1;
            float peakValue = 0.0f;

            for (int i = 0; i < blockSize; ++i)
                if (std::abs (buffer.getSample (0, i)) > peakValue)
                {
                    peakValue = std::abs (buffer.getSample (0, i));
                    peak = i;
                }

            CHECK (peakValue > 0.99f);
            return peak;
        };

        CHECK (tailPeak (0.0f) == 7200);
        CHECK (tailPeak (1.0f) == 8640);
    }

    // --- Смена Quality на лету не роняет звучащую ноту и не щёлкает (#38) -------
    // Звучащий голос обязан доиграть на своём движке: переключение латчится только
    // на старте ноты. Проверяется тем, что слышно, — непрерывностью сигнала.
    {
        MidiDelayProcessor proc;
        engineDefaults (proc);
        plainLoop (proc);
        constexpr double sr = 48000.0;
        constexpr int blockSize = 4096;
        constexpr float freq = 250.0f;

        setParam (proc, "quality", 0.0f);
        setParam (proc, "delayTime", 400.0f);
        setParam (proc, "mix", 100.0f);
        setParam (proc, "feedback", 0.0f);
        setParam (proc, "outputGain", 0.0f);
        setParam (proc, "attack", 1.0f);
        setParam (proc, "release", 300.0f);

        proc.setPlayConfigDetails (2, 2, sr, blockSize);
        proc.prepareToPlay (sr, blockSize);

        juce::AudioBuffer<float> buffer (2, blockSize);
        int phase = 0;
        float lastSample = 0.0f;
        float maxStep = 0.0f;
        double tailRms = 0.0;

        for (int b = 0; b < 24; ++b)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const float s = 0.25f * std::sin (juce::MathConstants<float>::twoPi
                                                  * freq * (float) (phase + i) / (float) sr);
                buffer.setSample (0, i, s);
                buffer.setSample (1, i, s);
            }

            phase += blockSize;

            // Нота на первом блоке, переключение движка — на двенадцатом, посреди неё.
            juce::MidiBuffer midi;
            if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            if (b == 12) setParam (proc, "quality", 1.0f);

            runBlock (proc, buffer, midi);
            CHECK (allocations.load() == 0);

            // Первые блоки пропускаем: там ещё разгон кольца и атака огибающей.
            if (b >= 6)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const float s = buffer.getSample (0, i);
                    maxStep = juce::jmax (maxStep, std::abs (s - lastSample));
                    lastSample = s;
                }

                if (b >= 13)
                    for (int i = 0; i < blockSize; ++i)
                        tailRms += (double) buffer.getSample (0, i) * buffer.getSample (0, i);
            }
        }

        // Синус 250 Гц амплитуды 0,25 даёт 0,008 на сэмпл. Порог 0,05 сигнала
        // не задевает, но щелчок от подмены движка посреди ноты поймал бы сразу.
        CHECK (maxStep < 0.05f);

        // И голос не умолк: после переключения хвост продолжает звучать.
        tailRms = std::sqrt (tailRms / (11.0 * blockSize));
        CHECK (tailRms > 0.1);
    }

    // --- Реальная задержка хвоста и нижний предел delay time (#17) --------------
    // Критерий #17 требует замера, а не одной точки: пик хвоста обязан стоять ровно
    // на выставленном времени, на обоих движках и на разных временах. Ниже предела
    // время подтягивается вверх — и хвост приходит ровно на пределе, а не позже него,
    // как приходил до клампа. Латентность хосту не репортится нигде и никогда.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 16384;
        constexpr int blocks = 4;   // 65536 сэмплов, хватает на секунду с лишним

        // Позиция пика хвоста от начала прогона. Вход — один импульс в нулевом сэмпле,
        // mix 100 %, feedback 0: во всём выходе ровно один всплеск, и это хвост.
        // Нота 60 — Root Key по умолчанию, ratio 1, питчер время не двигает.
        const auto tailPeak = [] (bool hq, float delayMs, double* minDelayMs = nullptr)
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            setParam (proc, "quality", hq ? 1.0f : 0.0f);
            setParam (proc, "delayTime", delayMs);
            setParam (proc, "mix", 100.0f);
            setParam (proc, "feedback", 0.0f);
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "bypass", 0.0f);
            setParam (proc, "attack", 1.0f);

            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            CHECK (proc.getLatencySamples() == 0);   // ANALYSIS §5: хосту — ноль

            if (minDelayMs != nullptr)
                *minDelayMs = proc.getMinDelayMs();

            juce::AudioBuffer<float> block (2, blockSize);
            int peak = -1;
            float peakValue = 0.0f;

            for (int b = 0; b < blocks; ++b)
            {
                block.clear();

                if (b == 0)
                {
                    block.setSample (0, 0, 1.0f);
                    block.setSample (1, 0, 1.0f);
                }

                runBlock (proc, block, b == 0 ? noteOnAt (0) : juce::MidiBuffer {});
                CHECK (allocations.load() == 0);

                for (int i = 0; i < blockSize; ++i)
                    if (std::abs (block.getSample (0, i)) > peakValue)
                    {
                        peakValue = std::abs (block.getSample (0, i));
                        peak = b * blockSize + i;
                    }
            }

            CHECK (peakValue > 0.99f);
            return peak;
        };

        for (const bool hq : { false, true })
        {
            double minDelayMs = 0.0;
            const int belowLimit = tailPeak (hq, 20.0f, &minDelayMs);
            const int latency = juce::roundToInt (minDelayMs * 0.001 * sr);

            // Предел взят у движка, а не написан числом здесь. Число в тесте при этом
            // остаётся и стережёт константу: Fast 5762 сэмпла, HQ 8640 (ADR 0005).
            CHECK (latency == (hq ? 8640 : 5762));

            // 20 мс подтянуты ровно до предела, а не куда-нибудь мимо.
            CHECK (belowLimit == latency);

            // Замер: четыре времени от «чуть выше предела» до секунды, допуск ±1 мс.
            for (const float delayMs : { hq ? 190.0f : 130.0f, 250.0f, 500.0f, 1000.0f })
                CHECK (std::abs (tailPeak (hq, delayMs)
                                 - juce::roundToInt (delayMs * 0.001 * sr)) <= 48);
        }

        // Кламп подтянул и петлю обратной связи, а не только первый хвост. Проверка
        // отдельная и нужная: позицию первого хвоста один только кламп внутри голоса
        // даёт ровно ту же, и на ней разницы не видно. А вот повторы её показывают —
        // кольцо, крутящееся на заказанных 20 мс под хвостом на 120, село бы гребёнкой.
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            setParam (proc, "quality", 0.0f);
            setParam (proc, "delayTime", 20.0f);     // втрое ниже предела Fast
            setParam (proc, "feedback", 50.0f);
            setParam (proc, "mix", 100.0f);
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "bypass", 0.0f);
            setParam (proc, "attack", 1.0f);

            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            const int limit = juce::roundToInt (proc.getMinDelayMs() * 0.001 * sr);
            std::vector<float> out (static_cast<size_t> (blockSize) * blocks, 0.0f);
            juce::AudioBuffer<float> block (2, blockSize);

            for (int b = 0; b < blocks; ++b)
            {
                block.clear();

                if (b == 0)
                {
                    block.setSample (0, 0, 1.0f);
                    block.setSample (1, 0, 1.0f);
                }

                runBlock (proc, block, b == 0 ? noteOnAt (0) : juce::MidiBuffer {});

                for (int i = 0; i < blockSize; ++i)
                    out[static_cast<size_t> (b) * blockSize + i] = block.getSample (0, i);
            }

            // Второй повтор ищется во второй трети прогона: он вдвое тише первого,
            // и глобальный максимум его не найдёт.
            int second = -1;
            float secondValue = 0.0f;

            for (int i = limit + limit / 2; i < 3 * limit; ++i)
                if (std::abs (out[static_cast<size_t> (i)]) > secondValue)
                {
                    secondValue = std::abs (out[static_cast<size_t> (i)]);
                    second = i;
                }

            CHECK (second == 2 * limit);              // интервал повтора — предел, а не 20 мс
            CHECK (std::abs (secondValue - 0.5f) < 0.01f);   // и это именно feedback 50 %
        }
    }

    // --- Tempo sync: нотные длительности от темпа хоста (#20) ------------------
    // Три вещи, которые тут ломаются молча. Первая: пунктир и триоль путаются местами
    // с полутора и двумя третями, и ошибка слышна только тем, кто играет в триолях.
    // Вторая: playhead есть не всегда — в Standalone его нет вовсе, а иной хост отдаёт
    // позицию без темпа, и без фолбэка это деление на ноль. Третья: сетка дерётся
    // с нижним пределом движка (#17) — на быстром темпе и мелком делителе заказанное
    // время уходит под латентность, плагин подтягивает его вверх, и «сетка не совпала».
    // Это не баг, а физика, но она обязана быть видна, а не случаться молча.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 512;

        // Ожидание считается из имени делителя, а не берётся из таблицы в заголовке:
        // иначе тест проверял бы, что таблица равна себе. Четверть принята за единицу:
        // "1/8" — половина четверти, "1/8." — полторы восьмых, "1/8T" — две трети.
        const auto beatsFromName = [] (juce::String name)
        {
            const bool dotted  = name.endsWithChar ('.');
            const bool triplet = name.endsWithChar ('T');

            if (dotted || triplet)
                name = name.dropLastCharacters (1);

            const double denominator = name.fromFirstOccurrenceOf ("/", false, false).getDoubleValue();
            CHECK (denominator > 0.0);

            double beats = 4.0 / denominator;   // 1/1 это четыре четверти, 1/4 — одна

            if (dotted)  beats *= 1.5;
            if (triplet) beats *= 2.0 / 3.0;

            return beats;
        };

        // Арифметика сетки на всех тринадцати делителях и трёх темпах. Проверяется
        // requestedDelayMs, а не звук: на звуке те же числа закрывает предел движка,
        // и промах в триоли можно не заметить.
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            setParam (proc, "sync", 1.0f);

            auto* division = proc.apvts.getParameter ("division");
            CHECK (division != nullptr);
            CHECK (division->getNumSteps() == 13);

            juce::AudioBuffer<float> block (2, blockSize);

            for (const double bpm : { 60.0, 120.0, 174.0 })
            {
                FixedTempoPlayHead playHead (bpm);
                proc.setPlayHead (&playHead);
                proc.setPlayConfigDetails (2, 2, sr, blockSize);
                proc.prepareToPlay (sr, blockSize);

                // Темп снимается в processBlock, а не в prepare: playhead в подготовке
                // хост отдавать не обязан. Один блок — и сетка уже та.
                block.clear();
                runBlock (proc, block);
                CHECK (allocations.load() == 0);
                CHECK (std::abs (proc.getSyncBpm() - bpm) < 1.0e-9);

                for (int i = 0; i < division->getNumSteps(); ++i)
                {
                    division->setValueNotifyingHost (division->convertTo0to1 (static_cast<float> (i)));

                    const double expected = beatsFromName (division->getCurrentValueAsText()) * 60000.0 / bpm;

                    CHECK (std::abs (proc.requestedDelayMs() - expected) < 1.0e-6);
                }
            }

            // Sync выключен — время снова берётся с ручки, и темп на него не влияет.
            setParam (proc, "sync", 0.0f);
            setParam (proc, "delayTime", 321.0f);
            CHECK (std::abs (proc.requestedDelayMs() - 321.0) < 0.01);
        }

        // Фолбэк. Три способа не получить темп, и все три обязаны дать 120 BPM,
        // а не ноль, не NaN и не падение.
        {
            const auto bpmWith = [] (juce::AudioPlayHead* playHead)
            {
                MidiDelayProcessor proc;
                engineDefaults (proc);
                setParam (proc, "sync", 1.0f);
                proc.setPlayHead (playHead);
                proc.setPlayConfigDetails (2, 2, sr, blockSize);
                proc.prepareToPlay (sr, blockSize);

                juce::AudioBuffer<float> block (2, blockSize);
                block.clear();
                runBlock (proc, block);

                return proc.getSyncBpm();
            };

            NoTempoPlayHead noTempo;
            FixedTempoPlayHead zero (0.0), negative (-120.0);

            CHECK (std::abs (bpmWith (nullptr)    - 120.0) < 1.0e-9);   // Standalone
            CHECK (std::abs (bpmWith (&noTempo)   - 120.0) < 1.0e-9);   // позиция без темпа
            CHECK (std::abs (bpmWith (&zero)      - 120.0) < 1.0e-9);   // хост врёт нулём
            CHECK (std::abs (bpmWith (&negative)  - 120.0) < 1.0e-9);   // и отрицательным

            // Абсурдный темп зажимается, а не уносит сетку в микросекунды. Верх и низ
            // проверяются оба: 5000 BPM дал бы 1/16 в три миллисекунды, а 1 BPM —
            // четверть в минуту, и обе цифры одинаково не бывают музыкой.
            FixedTempoPlayHead insane (5000.0), crawling (1.0);
            CHECK (std::abs (bpmWith (&insane)   - 999.0) < 1.0e-9);
            CHECK (std::abs (bpmWith (&crawling) -  20.0) < 1.0e-9);
        }

        // Замер на звуке: пик хвоста обязан стоять там, где велит сетка. Вход — один
        // импульс, feedback 0, mix 100 %: во всём выходе ровно один всплеск, и это он.
        {
            constexpr int longBlock = 16384;
            constexpr int blocks = 5;   // 81920 сэмплов, хватает на 1/1 при 174 BPM

            const auto syncTailPeak = [] (double bpm, int divisionIndex, double* requested)
            {
                MidiDelayProcessor proc;
                engineDefaults (proc);
                setParam (proc, "sync", 1.0f);
                setParam (proc, "division", static_cast<float> (divisionIndex));
                setParam (proc, "diffusion", 0.0f);
                setParam (proc, "modulation", 0.0f);
                setParam (proc, "ducking", 0.0f);
                setParam (proc, "filterLo", 20.0f);
                setParam (proc, "filterHi", 20000.0f);
                setParam (proc, "mix", 100.0f);
                setParam (proc, "feedback", 0.0f);
                setParam (proc, "outputGain", 0.0f);
                setParam (proc, "bypass", 0.0f);
                setParam (proc, "attack", 1.0f);

                FixedTempoPlayHead playHead (bpm);
                proc.setPlayHead (&playHead);
                proc.setPlayConfigDetails (2, 2, sr, longBlock);
                proc.prepareToPlay (sr, longBlock);

                // Sync ничего не просит у хоста: время прячется в дилее ровно так же,
                // как в Free. ANALYSIS §5 цел, исключений по-прежнему два (ADR 0006).
                CHECK (proc.getLatencySamples() == 0);

                juce::AudioBuffer<float> block (2, longBlock);
                int peak = -1;
                float peakValue = 0.0f;

                for (int b = 0; b < blocks; ++b)
                {
                    block.clear();

                    if (b == 0)
                    {
                        block.setSample (0, 0, 1.0f);
                        block.setSample (1, 0, 1.0f);
                    }

                    runBlock (proc, block, b == 0 ? noteOnAt (0) : juce::MidiBuffer {});
                    CHECK (allocations.load() == 0);

                    for (int i = 0; i < longBlock; ++i)
                        if (std::abs (block.getSample (0, i)) > peakValue)
                        {
                            peakValue = std::abs (block.getSample (0, i));
                            peak = b * longBlock + i;
                        }
                }

                // Порог мягче, чем у замера #17, и это не послабление, а физика: там все
                // времена были круглыми в сэмплах, а сетка круглой не бывает. Четверть
                // при 174 BPM — 16551,7 сэмпла, импульс садится между отсчётами, и
                // интерполяция кольца честно делит его на два соседних (0,84 и 0,26).
                // Проверяется, что хвост есть и он полноценный, а не что он попал в отсчёт.
                CHECK (peakValue > 0.8f);
                *requested = proc.requestedDelayMs();
                return peak;
            };

            // Четверть и восьмая на трёх темпах. Обе выше предела HQ (180 мс) на всех
            // трёх — кроме восьмой при 174 BPM (172 мс), и она тут нарочно: соседняя
            // строка показывает, что происходит на границе. Допуск ±1 мс.
            for (const double bpm : { 60.0, 120.0, 174.0 })
            {
                for (const int divisionIndex : { 5, 8 })   // 1/4 и 1/8
                {
                    double requested = 0.0;
                    const int peak = syncTailPeak (bpm, divisionIndex, &requested);

                    // Ожидание считается от темпа заново, а не берётся у плагина:
                    // иначе замер подтвердил бы любую ошибку сетки сам себе.
                    const double beats = divisionIndex == 5 ? 1.0 : 0.5;
                    const double gridMs = beats * 60000.0 / bpm;

                    // Ниже предела движка время подтягивается вверх (#17) — тогда пик
                    // стоит на пределе, и это ожидаемо, а не «сетка уехала».
                    const double expectedMs = juce::jmax (gridMs, 180.0);
                    const bool clamped = gridMs < 180.0;

                    CHECK (std::abs (requested - gridMs) < 0.01);
                    CHECK (clamped == (bpm > 170.0 && divisionIndex == 8));
                    CHECK (std::abs (peak - juce::roundToInt (expectedMs * 0.001 * sr)) <= 48);
                }
            }
        }

        // Смена делителя на звучащей ноте. Критерий #20 требует замера, а не веры,
        // и замер сразу показал разрыв 0,84 — голос был отводом кольца, который прыгал
        // вслед за ручкой, и прыжок вылезал через латентность питчера после самой смены.
        // Починка — в VoiceManager::setDelaySamples: время забирается в noteOn и держится
        // до конца ноты. Здесь feedback выключен нарочно: он меряет другое (см. ниже).
        {
            const auto runDivisionChange = [] (float feedbackPercent, float* worstStep,
                                               float* loudest, bool* finite)
            {
                MidiDelayProcessor proc;
                engineDefaults (proc);
                setParam (proc, "sync", 1.0f);
                setParam (proc, "division", 5.0f);      // 1/4
                setParam (proc, "diffusion", 0.0f);
                setParam (proc, "modulation", 0.0f);
                setParam (proc, "ducking", 0.0f);
                setParam (proc, "filterLo", 20.0f);
                setParam (proc, "filterHi", 20000.0f);
                setParam (proc, "mix", 100.0f);
                setParam (proc, "feedback", feedbackPercent);
                setParam (proc, "outputGain", 0.0f);
                setParam (proc, "bypass", 0.0f);
                setParam (proc, "attack", 10.0f);
                setParam (proc, "release", 2000.0f);

                FixedTempoPlayHead playHead (120.0);
                proc.setPlayHead (&playHead);
                proc.setPlayConfigDetails (2, 2, sr, blockSize);
                proc.prepareToPlay (sr, blockSize);

                juce::AudioBuffer<float> block (2, blockSize);
                double phase = 0.0;
                float lastSample = 0.0f;

                *worstStep = 0.0f;
                *loudest = 0.0f;
                *finite = true;

                // 200 блоков — 2,1 секунды: хватает и хвосту прийти, и рампе доехать.
                for (int b = 0; b < 200; ++b)
                {
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const auto value = static_cast<float> (0.5 * std::sin (phase));
                        phase += juce::MathConstants<double>::twoPi * 220.0 / sr;
                        block.setSample (0, i, value);
                        block.setSample (1, i, value);
                    }

                    // Делитель дёргается посреди звучания, и не один раз: 1/4 -> 1/8 -> 1/2.
                    // Один переход поймал бы половину случаев — вниз и вверх это разные
                    // концы кольца, и вверх вчетверо дальше.
                    if (b == 60)  setParam (proc, "division", 8.0f);
                    if (b == 120) setParam (proc, "division", 2.0f);

                    runBlock (proc, block, b == 10 ? noteOnAt (0, 67) : juce::MidiBuffer {});
                    CHECK (allocations.load() == 0);

                    for (int i = 0; i < blockSize; ++i)
                    {
                        const float value = block.getSample (0, i);

                        // Замер идёт с 40-го блока: до него хвоста ещё нет, и первый
                        // его фронт — это атака ноты, а не щелчок.
                        if (b >= 40)
                            *worstStep = juce::jmax (*worstStep, std::abs (value - lastSample));

                        *loudest = juce::jmax (*loudest, std::abs (value));
                        *finite = *finite && std::isfinite (value);
                        lastSample = value;
                    }
                }
            };

            float worstStep = 0.0f, loudest = 0.0f;
            bool finite = true;

            runDivisionChange (0.0f, &worstStep, &loudest, &finite);

            // Порог тот же, что у смены Quality на лету: шаг синуса 220 Гц при 48 кГц —
            // 0,014 на амплитуде 0,5, и 0,05 ловит разрыв, не трогая музыку.
            CHECK (finite);
            CHECK (loudest > 0.1f);      // хвост вообще есть, иначе тест ничего не мерил
            CHECK (worstStep < 0.05f);

            // А это — вторая половина правды, и её надо было отделить, а не «починить».
            // С обратной связью смена времени проезжает кольцо задом наперёд: время
            // едет 12000 -> 48000 сэмплов за 50 мс, то есть позиция чтения идёт назад
            // в четырнадцать раз быстрее, чем вперёд идёт запись. На слух это ленточный
            // зип, и он непрерывен — разрыва там нет, есть высокая частота, которую
            // детектор производной от щелчка не отличает. Механизм ровно тот же, что
            // у ручки Delay Time с сессии 05, и тот же, на котором стоит модуляция (#46):
            // отменить его — значит отменить ленточный характер. Проверяется поэтому
            // не гладкость, а то, что петля не разошлась и не залипла.
            runDivisionChange (50.0f, &worstStep, &loudest, &finite);

            CHECK (finite);
            CHECK (loudest > 0.1f);
            CHECK (loudest < 4.0f);      // петля не пошла вразнос на четырёхкратной смене
        }
    }

    // --- Диффузия и фильтры в петле (#45, #22) ---------------------------------
    // Три вещи, которые ломаются молча. Диффузор при g = 0 не исчезает, а становится
    // чистой задержкой на длину цепочки — и интервал повторов уезжает на 60 мс, не уронив
    // ни одного другого теста (проверено: без вычитания длины цепочки центр тяжести
    // второго повтора уходит на +3011 сэмплов). Фильтр, поставленный на выход, а не
    // в петлю, звучит почти так же на одном круге и совсем не так на пятом. И оба
    // вместе меняют усиление петли сильнее, чем каждый по отдельности.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 16384;
        constexpr int blocks = 4;
        constexpr float delayMs = 200.0f;   // выше предела Fast (120 мс)
        const int delaySamples = juce::roundToInt (delayMs * 0.001 * sr);

        // Импульсный отклик петли: импульс на входе, mix 100 %, нота 60 (ratio 1),
        // нота держится весь прогон. В выходе — цепочка повторов и больше ничего.
        const auto loopResponse = [] (float diffusion, float feedbackPercent,
                                      float loHz, float hiHz, bool follow = false,
                                      int* alignment = nullptr)
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            setParam (proc, "quality", 0.0f);
            setParam (proc, "sync", 0.0f);   // меряется тайминг петли: время задаёт ручка, не сетка
            setParam (proc, "timeMode", follow ? 1.0f : 0.0f);
            setParam (proc, "delayTime", delayMs);
            setParam (proc, "feedback", feedbackPercent);
            setParam (proc, "mix", 100.0f);
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "bypass", 0.0f);
            setParam (proc, "attack", 1.0f);
            setParam (proc, "diffusion", diffusion);
            setParam (proc, "modulation", 0.0f);   // замер тайминга: дрейф позиции чтения тут мешает
            setParam (proc, "ducking", 0.0f);      // и приседание: оно жмёт wet, а меряются уровни
            setParam (proc, "filterLo", loHz);
            setParam (proc, "filterHi", hiHz);

            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            if (alignment != nullptr)
                *alignment = proc.getLatencySamples();

            std::vector<float> out (static_cast<size_t> (blockSize) * blocks, 0.0f);
            juce::AudioBuffer<float> block (2, blockSize);

            for (int b = 0; b < blocks; ++b)
            {
                block.clear();

                if (b == 0)
                {
                    block.setSample (0, 0, 1.0f);
                    block.setSample (1, 0, 1.0f);
                }

                runBlock (proc, block, b == 0 ? noteOnAt (0) : juce::MidiBuffer {});
                CHECK (allocations.load() == 0);   // диффузор и фильтры не аллоцируют

                for (int i = 0; i < blockSize; ++i)
                    out[static_cast<size_t> (b) * blockSize + i] = block.getSample (0, i);
            }

            return out;
        };

        // Центр тяжести энергии в окне. Диффузия размывает повтор, и один только пик
        // про интервал больше ничего не говорит — а центр тяжести говорит.
        const auto centroid = [] (const std::vector<float>& x, int from, int to)
        {
            double sum = 0.0, weighted = 0.0;

            for (int i = juce::jmax (0, from); i < to && i < static_cast<int> (x.size()); ++i)
            {
                const double e = static_cast<double> (x[static_cast<size_t> (i)])
                               * static_cast<double> (x[static_cast<size_t> (i)]);
                sum += e;
                weighted += e * i;
            }

            return sum > 0.0 ? weighted / sum : -1.0;
        };

        // Diffusion 0 — цепочка обойдена, и это проверяется буквально: во втором
        // повторе ровно один ненулевой сэмпл ровно нужной величины, всё остальное
        // окно — точные нули. Утечка диффузора на нуле ручки сюда бы и попала.
        {
            const auto r = loopResponse (0.0f, 50.0f, 20.0f, 20000.0f);

            // Половина от импульса — это feedback 50 %. Точного 0,5f тут нет и без
            // диффузора: столько оставляет кроссфейд голов varispeed (0,49999997).
            CHECK (std::abs (r[static_cast<size_t> (2 * delaySamples)] - 0.5f) < 1.0e-6f);

            for (int i = delaySamples + 64; i < 3 * delaySamples; ++i)
                if (std::abs (i - 2 * delaySamples) > 2)
                    CHECK (r[static_cast<size_t> (i)] == 0.0f);
        }

        // Интервал повторов не зависит от положения Diffusion — в обоих режимах.
        // В Free повторы стоят на кратных delay time, в Follow — от выравнивания.
        for (const bool follow : { false, true })
        {
            int alignment = 0;
            loopResponse (0.0f, 50.0f, 20.0f, 20000.0f, follow, &alignment);

            const int expected = (follow ? alignment : delaySamples) + delaySamples;

            for (const float diffusion : { 0.0f, 25.0f, 60.0f, 100.0f })
            {
                const auto r = loopResponse (diffusion, 50.0f, 20.0f, 20000.0f, follow);
                const double c = centroid (r, expected - delaySamples / 2,
                                              expected + delaySamples / 2);

                // Допуск 15 мс. Без вычитания длины цепочки промах был бы +63 мс.
                CHECK (std::abs (c - expected) < 0.015 * sr);
            }
        }

        // Фильтры именно в петле, а не на выходе. Проверяется тем, чем эти две схемы
        // и различаются: сколько добавляет обратная связь. Синус 6 кГц, интервал
        // повтора — целое число периодов, повторы складываются в фазе, и установившийся
        // уровень равен A*|H| / (1 - fb*|H|). Фильтр на выходе дал бы одинаковый рост
        // с фильтром и без него; фильтр в петле режет каждый круг заново.
        {
            constexpr double tone = 6000.0;   // 8 сэмплов на период, 1200 периодов в петле

            const auto steadyLevel = [&] (float feedbackPercent, float hiHz)
            {
                MidiDelayProcessor proc;
                engineDefaults (proc);
                setParam (proc, "sync", 0.0f);   // замер уровней петли: время задаёт ручка, не сетка
                setParam (proc, "quality", 0.0f);
                setParam (proc, "delayTime", delayMs);
                setParam (proc, "feedback", feedbackPercent);
                setParam (proc, "mix", 100.0f);
                setParam (proc, "outputGain", 0.0f);
                setParam (proc, "bypass", 0.0f);
                setParam (proc, "attack", 1.0f);
                setParam (proc, "diffusion", 0.0f);
                setParam (proc, "modulation", 0.0f);
                setParam (proc, "ducking", 0.0f);
                setParam (proc, "filterLo", 20.0f);
                setParam (proc, "filterHi", hiHz);

                proc.setPlayConfigDetails (2, 2, sr, blockSize);
                proc.prepareToPlay (sr, blockSize);

                juce::AudioBuffer<float> block (2, blockSize);

                for (int b = 0; b < 12; ++b)   // 4 с: петля выходит на установившийся уровень
                {
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const auto v = static_cast<float> (0.25 * std::sin (
                            juce::MathConstants<double>::twoPi * tone * (b * blockSize + i) / sr));
                        block.setSample (0, i, v);
                        block.setSample (1, i, v);
                    }

                    runBlock (proc, block, b == 0 ? noteOnAt (0) : juce::MidiBuffer {});
                }

                return amplitudeAt (block, blockSize / 2, blockSize / 2, tone, sr);
            };

            const double openGrowth   = steadyLevel (80.0f, 20000.0f) / steadyLevel (0.0f, 20000.0f);
            const double filterGrowth = steadyLevel (80.0f, 2000.0f)  / steadyLevel (0.0f, 2000.0f);

            CHECK (openGrowth > 4.5);      // без фильтра 1/(1-0,8) = 5
            CHECK (filterGrowth < 2.0);    // с фильтром в петле 1/(1-0,8*0,32) = 1,35
        }

        // Значения по умолчанию (100 Гц / 12 кГц) слышно уже на первом хвосте:
        // подклад отодвигается назад, а не ждёт третьего круга. Это критерий #22.
        {
            const auto firstRepeatTone = [&] (double frequency, float loHz, float hiHz)
            {
                MidiDelayProcessor proc;
                engineDefaults (proc);
                setParam (proc, "sync", 0.0f);   // замер уровней петли: время задаёт ручка, не сетка
                setParam (proc, "quality", 0.0f);
                setParam (proc, "delayTime", delayMs);
                setParam (proc, "feedback", 0.0f);
                setParam (proc, "mix", 100.0f);
                setParam (proc, "outputGain", 0.0f);
                setParam (proc, "bypass", 0.0f);
                setParam (proc, "attack", 1.0f);
                setParam (proc, "diffusion", 0.0f);
                setParam (proc, "modulation", 0.0f);
                setParam (proc, "ducking", 0.0f);
                setParam (proc, "filterLo", loHz);
                setParam (proc, "filterHi", hiHz);

                proc.setPlayConfigDetails (2, 2, sr, blockSize);
                proc.prepareToPlay (sr, blockSize);

                juce::AudioBuffer<float> block (2, blockSize);

                for (int b = 0; b < 4; ++b)
                {
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const auto v = static_cast<float> (0.25 * std::sin (
                            juce::MathConstants<double>::twoPi * frequency * (b * blockSize + i) / sr));
                        block.setSample (0, i, v);
                        block.setSample (1, i, v);
                    }

                    runBlock (proc, block, b == 0 ? noteOnAt (0) : juce::MidiBuffer {});
                }

                return amplitudeAt (block, blockSize / 2, blockSize / 2, frequency, sr);
            };

            for (const double frequency : { 50.0, 16000.0 })
            {
                const double open = firstRepeatTone (frequency, 20.0f, 20000.0f);
                const double shaped = firstRepeatTone (frequency, 100.0f, 12000.0f);

                CHECK (shaped < open * 0.71);   // не меньше 3 dB уже на первом круге
            }
        }

        // Диффузия, фильтры и дрейф вместе при feedback 95 % не разгоняют петлю. Проверять
        // надо именно вместе: по отдельности усиление меняется слабее, а модуляция
        // ещё и меняет длину круга — то есть третий узел в той же петле (#46).
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            setParam (proc, "sync", 0.0f);   // замер уровней петли: время задаёт ручка, не сетка
            setParam (proc, "quality", 0.0f);
            setParam (proc, "delayTime", delayMs);
            setParam (proc, "feedback", 95.0f);
            setParam (proc, "mix", 100.0f);
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "bypass", 0.0f);
            setParam (proc, "attack", 1.0f);
            setParam (proc, "diffusion", 100.0f);
            setParam (proc, "modulation", 100.0f);
            setParam (proc, "ducking", 0.0f);       // разгон петли меряется без приседания
            setParam (proc, "filterLo", 100.0f);
            setParam (proc, "filterHi", 12000.0f);

            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            juce::AudioBuffer<float> block (2, blockSize);
            float peak = 0.0f, earlyTail = 0.0f, lateTail = 0.0f;

            // 2 с сигнала, потом 6 с тишины. Установившийся уровень при feedback 95 %
            // и единичном усилении петли — 0,25 / 0,05 = 5; выше него подниматься
            // нечему, а после конца сигнала уровень обязан падать, а не расти.
            for (int b = 0; b < 24; ++b)
            {
                const bool driven = b < 6;

                for (int i = 0; i < blockSize; ++i)
                {
                    const auto v = driven ? static_cast<float> (0.25 * std::sin (
                        juce::MathConstants<double>::twoPi * 1000.0 * (b * blockSize + i) / sr)) : 0.0f;
                    block.setSample (0, i, v);
                    block.setSample (1, i, v);
                }

                runBlock (proc, block, b == 0 ? noteOnAt (0) : juce::MidiBuffer {});

                const float blockPeak = block.getMagnitude (0, 0, blockSize);

                peak = juce::jmax (peak, blockPeak);

                if (b == 7)  earlyTail = blockPeak;   // первый блок после конца сигнала
                if (b == 23) lateTail = blockPeak;    // пять с половиной секунд спустя
            }

            // Потолок 5,5 — установившийся уровень при единичном усилении петли
            // (0,25 / 0,05 = 5) с запасом. И хвост обязан падать, а не расти.
            // Запас на затухание широкий намеренно: алл-пасс добавляет петле фазу,
            // то есть на части частот удлиняет её круг, и хвост с диффузией живёт
            // заметно дольше, чем даёт оценка feedback^n. Это не разгон, это
            // и есть та «глубина», ради которой диффузия и ставится.
            CHECK (peak < 5.5f);
            CHECK (lateTail < earlyTail * 0.6f);
        }
    }

    // --- Модуляция времени и ducking (#46, #47) ---------------------------------
    // Обе правки обязаны быть невидимы на нуле ручки — это уже доказано выше точным
    // числом в отклике петли (проверка «Diffusion 0» зовёт оба параметра в ноль).
    // Здесь проверяется то, ради чего они заведены: дрейф расцепляет круги, а хвост
    // приседает под сухим и возвращается в паузе, одинаково в обоих Time Mode.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 8192;
        constexpr float delayMs = 200.0f;
        constexpr double tone = 1000.0;   // 200 периодов в петле: круги складываются в фазе

        /** Прогон с тоном заданной длительности и тишиной до конца. Возвращает
            установившийся уровень тона под сигналом и уровень хвоста в паузе. */
        const auto run = [&] (float modulation, float ducking, bool follow,
                              int drivenBlocks, int totalBlocks,
                              double& underSignal, double& inPause)
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            setParam (proc, "quality", 0.0f);
            setParam (proc, "timeMode", follow ? 1.0f : 0.0f);
            setParam (proc, "delayTime", delayMs);
            setParam (proc, "feedback", 90.0f);
            setParam (proc, "mix", 100.0f);     // на выходе только хвост
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "bypass", 0.0f);
            setParam (proc, "attack", 1.0f);
            setParam (proc, "release", 2000.0f);
            setParam (proc, "width", 0.0f);
            setParam (proc, "modulation", modulation);
            setParam (proc, "ducking", ducking);

            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            juce::AudioBuffer<float> block (2, blockSize);
            underSignal = inPause = 0.0;

            for (int b = 0; b < totalBlocks; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const auto v = b < drivenBlocks
                        ? static_cast<float> (0.25 * std::sin (juce::MathConstants<double>::twoPi
                                                               * tone * (b * blockSize + i) / sr))
                        : 0.0f;
                    block.setSample (0, i, v);
                    block.setSample (1, i, v);
                }

                runBlock (proc, block, b == 0 ? noteOnAt (0) : juce::MidiBuffer {});
                CHECK (allocations.load() == 0);   // дрейф и детектор не аллоцируют

                // Последний блок под сигналом — там петля уже установилась.
                if (b == drivenBlocks - 1)
                    underSignal = amplitudeAt (block, blockSize / 2, blockSize / 2, tone, sr);

                // Первый блок после конца сигнала: детектор к этому моменту отпустил
                // (спад 250 мс, блок 170 мс — два блока с запасом), а хвост ещё звучит.
                if (b == drivenBlocks + 2)
                    inPause = amplitudeAt (block, blockSize / 2, blockSize / 2, tone, sr);
            }
        };

        double open = 0.0, drifting = 0.0, unused = 0.0;

        // 1. Дрейф расцепляет круги. Тон стоит ровно на резонансе петли: без модуляции
        //    двенадцать кругов складываются в фазе и уровень вырастает почти в 1/(1-0,9).
        //    Дрейф в 4 мс — это четыре периода тона, то есть круги перестают совпадать
        //    по фазе, и резонанс садится. Это и есть «металлический призвук слабее»,
        //    выраженное числом: металлика петли — и есть её резонансы.
        run (0.0f,   0.0f, false, 12, 16, open,     unused);
        run (100.0f, 0.0f, false, 12, 16, drifting, unused);

        std::printf ("  дрейф: резонанс петли на feedback 90 %% — без %.3f, с дрейфом %.3f\n",
                     open, drifting);

        CHECK (drifting < open * 0.7);

        // 1б. И дрейф не превращается в расстройку. Уход высоты в хвосте — это цена
        //     метода: скорость изменения задержки и есть сдвиг высоты, а сигнал,
        //     прошедший n кругов, испытал его n раз. Значит мерить надо не первый
        //     повтор, а длинный хвост на высоком feedback — то есть худший случай.
        {
            const auto pitchSpreadCents = [&] (float modulation, float feedbackPercent)
            {
                MidiDelayProcessor proc;
                engineDefaults (proc);
                plainLoop (proc);
                setParam (proc, "quality", 0.0f);
                setParam (proc, "delayTime", delayMs);
                setParam (proc, "feedback", feedbackPercent);
                setParam (proc, "mix", 100.0f);
                setParam (proc, "outputGain", 0.0f);
                setParam (proc, "bypass", 0.0f);
                setParam (proc, "attack", 1.0f);
                setParam (proc, "release", 2000.0f);
                setParam (proc, "width", 0.0f);
                setParam (proc, "modulation", modulation);

                proc.setPlayConfigDetails (2, 2, sr, 4096);
                proc.prepareToPlay (sr, 4096);

                juce::AudioBuffer<float> block (2, 4096);
                double lo = 1.0e9, hi = -1.0e9;

                for (int b = 0; b < 120; ++b)   // 10 с: 2 с сигнала, дальше только хвост
                {
                    for (int i = 0; i < 4096; ++i)
                    {
                        const auto v = b < 24
                            ? static_cast<float> (0.25 * std::sin (juce::MathConstants<double>::twoPi
                                                                   * tone * (b * 4096 + i) / sr))
                            : 0.0f;
                        block.setSample (0, i, v);
                        block.setSample (1, i, v);
                    }

                    runBlock (proc, block, b == 0 ? noteOnAt (0) : juce::MidiBuffer {});

                    // Только по хвосту и только пока он слышен: в затихшем блоке
                    // «самая сильная составляющая» — это уже шум округления.
                    if (b > 26 && block.getMagnitude (0, 0, 4096) > 0.05f)
                    {
                        const double f = dominantFrequency (block, 0, 4096, tone, 40.0, sr);
                        lo = juce::jmin (lo, f);
                        hi = juce::jmax (hi, f);
                    }
                }

                return hi > 0.0 ? 1200.0 * std::log2 (hi / lo) : 0.0;
            };

            // Глубина 0 — ровно ноль центов, а не «почти»: позиция чтения не тронута,
            // и это то же доказательство бит-в-бит, что и точное число в отклике петли.
            CHECK (pitchSpreadCents (0.0f, 90.0f) < 0.01);

            // Значение по умолчанию на значении по умолчанию: 3,5 цента при feedback 35 %.
            // Порог слышимости расстройки — единицы центов, и дрейф под ним. На полной
            // ручке и feedback 90 % замерено 40 центов, но это уже сознательно крайнее
            // положение обеих ручек, и там дрейф — заявленный характер, а не дефект.
            const double atDefaults = pitchSpreadCents (20.0f, 35.0f);

            std::printf ("  дрейф на значениях по умолчанию: %.1f центов\n", atDefaults);
            CHECK (atDefaults < 8.0);
        }

        // 2. Ducking. Под сухим хвост садится, в паузе возвращается — и это два разных
        //    числа, а не одно: детектор обязан отпускать, иначе приседание было бы просто
        //    тише сделанным wet.
        for (const bool follow : { false, true })
        {
            double flatUnder = 0.0, flatPause = 0.0, duckUnder = 0.0, duckPause = 0.0;

            run (0.0f, 0.0f,   follow, 12, 16, flatUnder, flatPause);
            run (0.0f, 100.0f, follow, 12, 16, duckUnder, duckPause);

            std::printf ("  ducking (%s): под сухим %.3f -> %.3f, в паузе %.3f -> %.3f\n",
                         follow ? "follow" : "free", flatUnder, duckUnder, flatPause, duckPause);

            // Полная ручка — это -12 dB, то есть четверть. Порог 0,45 берёт с запасом
            // на то, что детектор едет по огибающей, а не стоит на полке.
            CHECK (duckUnder < flatUnder * 0.45);

            // А в паузе хвост обязан вернуться: приседание ушло, осталось затухание петли.
            CHECK (duckPause > flatPause * 0.8);

            // И то же самое в обоих режимах Time Mode — детектор берёт сухой оттуда же,
            // откуда его берёт микс, и выравнивание Follow на приседание не влияет.
            CHECK (duckUnder / flatUnder > 0.05);
        }
    }

    // --- Stereo Mode: куда уходят ноты и повторы (#55, #23) ---------------------
    // Оба режима проверяются буквально. Ноты 48, 60 и 72 при Root Key C
    // поют синус 500 Гц на 250, 500 и 1000 Гц, и сторона каждой ноты читается своей
    // частотой: ноты звучат одновременно, и баланс суммы усреднился бы в центр. Fast:
    // варигонка на октаву даёт ровно эти частоты, и они ложатся в бины окна 0,2 с.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 8192;
        constexpr int window = 9600;

        const auto setUp = [&] (MidiDelayProcessor& proc, float stereo, float feedback)
        {
            engineDefaults (proc);
            plainLoop (proc);
            setParam (proc, "stereo", stereo);
            setParam (proc, "quality", 0.0f);
            setParam (proc, "delayTime", 200.0f);
            setParam (proc, "feedback", feedback);
            setParam (proc, "mix", 100.0f);
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "attack", 1.0f);
            setParam (proc, "release", 50.0f);
            setParam (proc, "width", 100.0f);
            proc.setPlayConfigDetails (2, 2, sr, blockSize);
        };

        // Прогон: вход — синус 500 Гц первые toneSamples сэмплов, дальше тишина; в начале
        // блока b берётся нота noteAt (b), если она не -1. Ноты не отпускаются.
        const auto run = [&] (MidiDelayProcessor& proc, int blocks, int toneSamples,
                              const std::function<int (int)>& noteAt)
        {
            proc.prepareToPlay (sr, blockSize);

            juce::AudioBuffer<float> out (2, blocks * blockSize);
            juce::AudioBuffer<float> block (2, blockSize);

            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const int t = b * blockSize + i;
                    const auto v = t < toneSamples
                        ? static_cast<float> (0.25 * std::sin (juce::MathConstants<double>::twoPi * 500.0 * t / sr))
                        : 0.0f;
                    block.setSample (0, i, v);
                    block.setSample (1, i, v);
                }

                const int note = noteAt (b);
                runBlock (proc, block, note >= 0 ? noteOnAt (0, note) : juce::MidiBuffer {});
                CHECK (allocations.load() == 0);

                for (int ch = 0; ch < 2; ++ch)
                    out.copyFrom (ch, b * blockSize, block, ch, 0, blockSize);
            }

            return out;
        };

        // Баланс составляющей частоты f: -1 целиком слева, +1 целиком справа.
        const auto balanceAt = [] (const juce::AudioBuffer<float>& x, int from, double f)
        {
            double amplitude[2] {};

            for (int ch = 0; ch < 2; ++ch)
            {
                double re = 0.0, im = 0.0;

                for (int i = 0; i < window; ++i)
                {
                    const double a = juce::MathConstants<double>::twoPi * f * i / sr;
                    re += x.getSample (ch, from + i) * std::cos (a);
                    im += x.getSample (ch, from + i) * std::sin (a);
                }

                amplitude[ch] = std::sqrt (re * re + im * im);
            }

            return (amplitude[1] - amplitude[0]) / std::max (amplitude[0] + amplitude[1], 1.0e-12);
        };

        const auto rmsBalance = [] (const juce::AudioBuffer<float>& x, int from, int count)
        {
            const double l = x.getRMSLevel (0, from, count);
            const double r = x.getRMSLevel (1, from, count);
            return (r - l) / std::max (l + r, 1.0e-12);
        };

        // 1. Ping-Pong по нотам. Три ноты по очереди, все держатся: 60 в тишину — в центр,
        //    72 при звучащей 60 — влево, 48 при двух — вправо, на полразмаха.
        const auto threeNotes = [&] (float stereo, double& note48, double& note60, double& note72)
        {
            MidiDelayProcessor proc;
            setUp (proc, stereo, 0.0f);

            const auto out = run (proc, 8, 8 * blockSize,
                                  [] (int b) { return b == 1 ? 60 : b == 2 ? 72 : b == 3 ? 48 : -1; });

            note48 = balanceAt (out, 5 * blockSize, 250.0);
            note60 = balanceAt (out, 5 * blockSize, 500.0);
            note72 = balanceAt (out, 5 * blockSize, 1000.0);
        };

        double note48 = 0.0, note60 = 0.0, note72 = 0.0;

        // Порог 0,2 — примерно четверть панорамы: меньше было бы «чуть шире», а не чередованием.
        threeNotes (2.0f, note48, note60, note72);
        std::printf ("  #55 ping-pong по нотам: нота 48 %+.2f, 60 %+.2f, 72 %+.2f (-1 слева, +1 справа)\n",
                     note48, note60, note72);
        CHECK (std::abs (note60) < 0.05 && note72 < -0.2 && note48 > 0.2);

        // 2. Хор: одна нота занимает два голоса, и они стоят симметрично по бортам — баланс
        //    ноль, как у одного голоса в центре, но голосов два. Контроль к строке выше.
        const auto votersOn = [] (MidiDelayProcessor& proc, int note)
        {
            int voices = 0;

            for (int slot = 0; slot < VoiceManager::maxVoices; ++slot)
                if (proc.voiceNote[slot].load() == note)
                    ++voices;

            return voices;
        };

        {
            MidiDelayProcessor proc;
            setUp (proc, 1.0f, 0.0f);

            const auto out = run (proc, 6, 6 * blockSize, [] (int b) { return b == 1 ? 60 : -1; });

            const int voices = votersOn (proc, 60);
            const double balance = rmsBalance (out, 4 * blockSize, window);
            const double level = out.getRMSLevel (0, 4 * blockSize, window);

            std::printf ("  #55 хор: голосов на ноту %d, баланс %+.3f, уровень %.3f\n",
                         voices, balance, level);

            CHECK (voices == 2);
            CHECK (std::abs (balance) < 0.05);
            CHECK (level > 0.05);
        }

        // 3. Полный пул не даёт паре украсть саму себя: при Voices 2 новая нота крадёт обе
        //    половины старой, а не первую свою половину. Свежий голос на нуле огибающей —
        //    самый тихий в пуле, и без запрета второй голос пары забирал его. При Voices 1
        //    пары нет вовсе — один голос.
        {
            MidiDelayProcessor proc;
            setUp (proc, 1.0f, 0.0f);
            setParam (proc, "voices", 2.0f);

            run (proc, 4, 4 * blockSize, [] (int b) { return b == 1 ? 60 : b == 2 ? 64 : -1; });
            const int stolen = votersOn (proc, 64);

            setParam (proc, "voices", 1.0f);
            run (proc, 3, 3 * blockSize, [] (int b) { return b == 1 ? 67 : -1; });
            const int single = votersOn (proc, 67);

            std::printf ("  #55 хор на полном пуле: Voices 2 — голосов на новой ноте %d, Voices 1 — %d\n",
                         stolen, single);

            CHECK (stolen == 2);
            CHECK (single == 1);
        }
    }

    // --- Моносовместимость: корреляция и уровень моно-суммы (#23) ---------------
    // Последний открытый критерий #23. Стерео мерили до диффузора, а он развёл каналы
    // по длинам звеньев нарочно (7/11/17/23 мс слева, 9/13/19/26 справа), и ширина
    // досталась даром. Даром она бывает ровно до того момента, как трек сложат в моно:
    // радио, клубный сабвуфер и телефонный динамик моно, и хвост, вычитающийся сам
    // из себя, там исчезает целиком. Поверх этого лёг ping-pong, который разводит
    // ноты по бортам. Мерится то и другое вместе, на значениях по умолчанию.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 4096;
        constexpr int blocks = 40;   // 3,4 с: хватает восьми нотам и хвосту за ними

        // Возвращает корреляцию каналов и проседание моно-суммы в децибелах.
        // Проседание считается против среднего уровня каналов, а не против громкого:
        // вопрос ведь «сколько потеряется при сложении», а не «какой борт был громче».
        const auto monoLoss = [&] (float width, float stereo, float diffusion, double& correlation)
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            setParam (proc, "sync", 0.0f);
            setParam (proc, "delayTime", 250.0f);
            setParam (proc, "mix", 100.0f);          // меряется хвост, а не сухой под ним
            setParam (proc, "feedback", 35.0f);      // умолчание: круги успевают разойтись
            setParam (proc, "diffusion", diffusion);
            setParam (proc, "modulation", 20.0f);    // умолчание: дрейф тоже разводит каналы
            setParam (proc, "ducking", 0.0f);        // жмёт wet по сухому, замеру уровней мешает
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "bypass", 0.0f);
            setParam (proc, "attack", 10.0f);
            setParam (proc, "release", 300.0f);
            setParam (proc, "width", width);
            setParam (proc, "stereo", stereo);

            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            juce::AudioBuffer<float> block (2, blockSize);
            double phase = 0.0;
            double sumL = 0.0, sumR = 0.0, sumLR = 0.0, sumMono = 0.0;
            long counted = 0;

            // Паттерн плака восьмыми: тот самый сценарий, ради которого плагин задуман,
            // и единственный, на котором ping-pong вообще что-то делает.
            static constexpr int pattern[] { 60, 64, 67, 64, 69, 67, 64, 60 };

            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    // Материал моно: вопрос ровно в том, что плагин делает с шириной сам.
                    const auto v = static_cast<float> (0.25 * std::sin (phase));
                    phase += juce::MathConstants<double>::twoPi * 330.0 / sr;
                    block.setSample (0, i, v);
                    block.setSample (1, i, v);
                }

                juce::MidiBuffer midi;

                // Нота каждые три блока, предыдущая отпускается: голоса обязаны
                // сменяться, иначе ping-pong усреднится в центр сам собой.
                if (b >= 2 && b % 3 == 2)
                {
                    const int index = (b - 2) / 3;

                    if (index > 0 && index <= 8)
                        midi.addEvent (juce::MidiMessage::noteOff (1, pattern[(index - 1) % 8]), 0);

                    if (index < 8)
                        midi.addEvent (juce::MidiMessage::noteOn (1, pattern[index % 8], 1.0f), 1);
                }

                runBlock (proc, block, midi);
                CHECK (allocations.load() == 0);

                if (b < 4)
                    continue;   // до четвёртого блока хвоста ещё нет, считать нечего

                for (int i = 0; i < blockSize; ++i)
                {
                    const double l = block.getSample (0, i);
                    const double r = block.getSample (1, i);
                    const double mono = 0.5 * (l + r);

                    sumL += l * l;
                    sumR += r * r;
                    sumLR += l * r;
                    sumMono += mono * mono;
                    ++counted;
                }
            }

            CHECK (counted > 0);

            const double rmsL = std::sqrt (sumL / counted);
            const double rmsR = std::sqrt (sumR / counted);
            const double rmsMono = std::sqrt (sumMono / counted);

            CHECK (rmsL > 1.0e-4 && rmsR > 1.0e-4);   // хвост есть, иначе замер пустой

            correlation = sumLR / std::sqrt (sumL * sumR);

            return 20.0 * std::log10 (rmsMono / (0.5 * (rmsL + rmsR)));
        };

        // Положения, которые пользователь реально выставит. Width 200 — край ручки,
        // ping-pong — то, что легло поверх стерео в сессии 13, хор — пара голосов на ноту (#55).
        struct Case { const char* name; float width; float stereo; float diffusion; };

        static constexpr Case cases[] {
            { "width 100, choir, diffusion 50",               100.0f, 1.0f, 50.0f },
            { "width 200, choir, diffusion 50",               200.0f, 1.0f, 50.0f },
            { "width 200, choir, diffusion 100",              200.0f, 1.0f, 100.0f },
            { "width 100, ping-pong, diffusion 50",           100.0f, 2.0f, 50.0f },
            // Строка, из-за которой Sung Vocal в сессии 17 переехал на Width 100:
            // между замеренными 100 (-0,69 dB) и 200 (-3,02 dB) лежит вся разница
            // между «незаметно» и «в моно вдвое тише», и 150 отдаёт больше половины
            // пути. В пресете этого значения больше нет, в шкале — остаётся.
            { "width 150, ping-pong, diffusion 50",           150.0f, 2.0f, 50.0f },
            { "width 200, ping-pong, diffusion 100",          200.0f, 2.0f, 100.0f },
        };

        for (const auto& c : cases)
        {
            double correlation = 0.0;
            const double loss = monoLoss (c.width, c.stereo, c.diffusion, correlation);

            std::printf ("  моно (%s): корреляция %+.3f, сумма %+.2f dB\n",
                         c.name, correlation, loss);

            // Фазового вычитания нет: отрицательная корреляция и означала бы, что каналы
            // гасят друг друга. Порог -0,1, а не ноль: слабый минус — это просто разные
            // длины звеньев диффузора, а не противофаза.
            CHECK (correlation > -0.1);

            // Порог -3 dB — из критерия задачи, и замер встал ровно на него: width 200
            // с ping-pong даёт корреляцию 0,000 и сумму -3,02 dB. Это не дефект, а пол:
            // две некоррелированные половины при сложении и должны дать -3 dB, ниже
            // может увести только противофаза, а её ловит проверка корреляции выше.
            // Крайняя цифра стоит здесь нарочно — если она поедет вниз, поедет
            // именно в вычитание, и тест это поймает.
            CHECK (loss > -3.1);
        }
    }

    // --- Stereo Mode: смена на хвосте и старые проекты (#55) --------------------
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 512;

        // 1. Смена Stereo на звучащем хвосте не щёлкает — критерий задачи. Детектор тот же,
        //    что в стрессе (#25): отношение худшего шага к 99,9-му процентилю, порог 8.
        //    Дёргается один Stereo: в общем стрессе его вклад тонет в рывках двадцати
        //    остальных. Аккорд
        //    держится, поверх идёт мелодия: раскладку голос берёт в noteOn, и без новых нот
        //    смена раскладки на голосах не проверялась бы совсем. Окраска петли по умолчанию.
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            setParam (proc, "sync", 0.0f);
            setParam (proc, "delayTime", 300.0f);
            setParam (proc, "feedback", 60.0f);
            setParam (proc, "mix", 100.0f);
            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            constexpr int blocks = 2820;   // 30 с
            juce::Random random (20260921);
            juce::AudioBuffer<float> block (2, blockSize);
            std::vector<float> steps;
            steps.reserve (static_cast<size_t> (blocks) * blockSize);
            float previous = 0.0f;
            double phase = 0.0;
            int melody = -1;

            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const auto v = static_cast<float> (0.3 * std::sin (phase) + 0.15 * std::sin (2.7 * phase));
                    phase += juce::MathConstants<double>::twoPi * 196.0 / sr;
                    block.setSample (0, i, v);
                    block.setSample (1, i, v);
                }

                if (b > 0 && b % 10 == 0)
                    setParam (proc, "stereo", static_cast<float> (random.nextInt (3)));

                juce::MidiBuffer midi;

                if (b == 0)
                    for (const int note : { 60, 64, 67 })
                        midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.9f), 0);

                // Нота мелодии раз в 25 блоков, прежняя отпускается. Ниже аккорда: note off
                // по номеру отпустил бы и голос аккорда с той же нотой.
                if (b % 25 == 12)
                {
                    if (melody >= 0)
                        midi.addEvent (juce::MidiMessage::noteOff (1, melody), 0);

                    melody = 48 + random.nextInt (12);
                    midi.addEvent (juce::MidiMessage::noteOn (1, melody, 0.8f), random.nextInt (blockSize));
                }

                runBlock (proc, block, midi);
                CHECK (allocations.load() == 0);

                for (int i = 0; i < blockSize; ++i)
                {
                    const float value = block.getSample (0, i);
                    CHECK (std::isfinite (value));
                    CHECK (std::abs (value) < 8.0f);
                    steps.push_back (std::abs (value - previous));
                    previous = value;
                }
            }

            const auto tail = steps.begin() + static_cast<long> (steps.size() * 999 / 1000);
            std::nth_element (steps.begin(), tail, steps.end());
            const double percentile = *tail;
            const double worst = *std::max_element (tail, steps.end());
            const double ratio = worst / juce::jmax (1.0e-6, percentile);

            std::printf ("  #55 смена Stereo на хвосте: худший шаг %.4f, "
                         "99,9%% %.4f, отношение %.1f\n", worst, percentile, ratio);

            CHECK (percentile > 1.0e-5);
            CHECK (ratio < 8.0);
        }

        // 2. Проект до сессии 21: галка pingPong вместо списка Stereo. Включённая открывается
        //    в Ping-Pong, выключенная — в Choir: звук сохранённого проекта не меняется,
        //    а Auto во Free поменял бы его. Старый узел из состояния уходит. И обратное:
        //    новое состояние миграцию не проходит — Auto переживает сохранение как Auto.
        {
            const auto legacyState = [] (float pingPong)
            {
                MidiDelayProcessor source;
                engineDefaults (source);
                setParam (source, "feedback", 42.0f);

                auto state = source.apvts.copyState();
                state.removeChild (state.getChildWithProperty ("id", "stereo"), nullptr);

                juce::ValueTree legacy ("PARAM");
                legacy.setProperty ("id", "pingPong", nullptr);
                legacy.setProperty ("value", pingPong, nullptr);
                state.appendChild (legacy, nullptr);
                state.setProperty ("stateVersion", 1, nullptr);

                juce::MemoryBlock block;
                if (auto xml = state.createXml())
                    juce::AudioProcessor::copyXmlToBinary (*xml, block);

                return block;
            };

            for (const float pingPong : { 1.0f, 0.0f })
            {
                const auto block = legacyState (pingPong);

                MidiDelayProcessor restored;
                engineDefaults (restored);   // Auto — заведомо не то, во что мигрирует
                restored.setStateInformation (block.getData(), static_cast<int> (block.getSize()));

                CHECK (getParam (restored, "stereo") == (pingPong > 0.5f ? 2.0f : 1.0f));
                CHECK (std::abs (getParam (restored, "feedback") - 42.0f) < 0.01f);
                CHECK (! restored.apvts.state.getChildWithProperty ("id", "pingPong").isValid());
            }

            MidiDelayProcessor source;
            engineDefaults (source);

            juce::MemoryBlock block;
            source.getStateInformation (block);

            MidiDelayProcessor restored;
            engineDefaults (restored);
            setParam (restored, "stereo", 2.0f);
            restored.setStateInformation (block.getData(), static_cast<int> (block.getSize()));
            CHECK (getParam (restored, "stereo") == 0.0f);

            std::printf ("  #55 старый проект: pingPong 1 -> Ping-Pong, 0 -> Choir, Auto пережил сохранение\n");
        }
    }

    // --- Режим Follow и MIDI Offset (#18, ADR 0006) -----------------------------
    // Три вещи, которые легко перепутать знаком и увидеть только на слух: хвост
    // в Follow обязан стоять на том же сэмпле, что сухой; положительный офсет
    // обязан двигать сам старт хвоста, а не кусок кольца под ним; отрицательный
    // обязан придерживать сухой и честно сказать об этом хосту.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 8192;
        constexpr int blocks = 6;                  // 49152 сэмпла
        constexpr int total = blockSize * blocks;
        // Латентность в Follow — это латентность движка, выбранного Quality. Отдельного
        // короткого движка под Follow нет: он был и оказался браком, замер в ADR 0006.
        constexpr int followLatency = 8640;        // HQ, окно 0,18 с при 48 кГц

        // Прогон с импульсом в pulse и нотой в note. Возвращает весь выход.
        const auto render = [] (std::vector<float>& out, bool follow, float delayMs,
                                float offsetMs, float mix, int note, int pulse,
                                int* latency = nullptr)
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            setParam (proc, "timeMode", follow ? 1.0f : 0.0f);
            setParam (proc, "quality", 1.0f);      // HQ: от него считается выравнивание
            setParam (proc, "delayTime", delayMs);
            setParam (proc, "midiOffset", offsetMs);
            setParam (proc, "mix", mix);
            setParam (proc, "feedback", 0.0f);
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "bypass", 0.0f);
            setParam (proc, "attack", 1.0f);
            setParam (proc, "release", 2000.0f);
            setParam (proc, "width", 0.0f);

            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            if (latency != nullptr)
                *latency = proc.getLatencySamples();

            juce::AudioBuffer<float> block (2, blockSize);
            out.assign (total, 0.0f);

            for (int b = 0; b < blocks; ++b)
            {
                block.clear();

                if (pulse >= 0 && pulse / blockSize == b)
                    for (int ch = 0; ch < 2; ++ch)
                        block.setSample (ch, pulse % blockSize, 1.0f);

                juce::MidiBuffer midi;
                if (note >= 0 && note / blockSize == b)
                    midi = noteOnAt (note % blockSize);

                runBlock (proc, block, midi);
                CHECK (allocations.load() == 0);

                for (int i = 0; i < blockSize; ++i)
                    out[static_cast<size_t> (b) * blockSize + i] = block.getSample (0, i);
            }
        };

        const auto peakIndex = [] (const std::vector<float>& x)
        {
            int at = -1;
            float value = 0.0f;

            for (size_t i = 0; i < x.size(); ++i)
                if (std::abs (x[i]) > value) { value = std::abs (x[i]); at = static_cast<int> (i); }

            return at;
        };

        std::vector<float> out;

        // 1. Follow: сухой и хвост приходят на один и тот же сэмпл. Нота открывается
        //    заранее и держится, импульс попадает на уже открытую огибающую. При
        //    mix 50 % оба слагаемых лежат друг на друге, и всплеск ровно один.
        {
            int latency = 0;
            constexpr int pulse = 20000;

            render (out, true, 400.0f, 0.0f, 50.0f, 12000, pulse, &latency);

            // Латентность в Follow репортится хосту — это и есть цена режима.
            CHECK (latency == followLatency);

            // Весь плагин опаздывает от своего входа ровно на неё, и сухой вместе с ним.
            CHECK (peakIndex (out) == pulse + followLatency);

            // И это именно сумма двух половин, а не один сухой: сухой при mix 50 %
            // дал бы 0,5, а здесь к нему прибавился хвост.
            CHECK (out[static_cast<size_t> (pulse + followLatency)] > 0.8f);
        }

        // 2. Free с тем же материалом: хвост уезжает на delay time, сухой стоит
        //    на месте, латентность хосту нулевая. Это контроль к проверке 1 —
        //    без него она прошла бы и на плагине, который просто всё задержал.
        {
            int latency = 0;
            constexpr int pulse = 20000;

            render (out, false, 400.0f, 0.0f, 0.0f, 12000, pulse, &latency);

            CHECK (latency == 0);
            CHECK (peakIndex (out) == pulse);          // mix 0: виден только сухой
        }

        // 3. Положительный MIDI Offset двигает старт хвоста, и ровно на себя. Вход
        //    постоянный, mix 100 %: на выходе видна одна огибающая, и её начало
        //    читается прямо. Нота стоит после прогрева кольца — раньше голосу
        //    нечего читать, и старт нашёлся бы не там, где событие.
        {
            const auto onset = [&] (float offsetMs)
            {
                MidiDelayProcessor proc;
                engineDefaults (proc);
                plainLoop (proc);
                setParam (proc, "timeMode", 0.0f);
                setParam (proc, "quality", 1.0f);
                setParam (proc, "delayTime", 200.0f);
                setParam (proc, "midiOffset", offsetMs);
                setParam (proc, "mix", 100.0f);
                setParam (proc, "feedback", 0.0f);
                setParam (proc, "outputGain", 0.0f);
                setParam (proc, "bypass", 0.0f);
                setParam (proc, "attack", 1.0f);
                setParam (proc, "release", 2000.0f);
                setParam (proc, "width", 0.0f);

                proc.setPlayConfigDetails (2, 2, sr, blockSize);
                proc.prepareToPlay (sr, blockSize);

                juce::AudioBuffer<float> block (2, blockSize);
                constexpr int notePos = 3 * blockSize + 1000;
                int first = -1;

                for (int b = 0; b < blocks; ++b)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < blockSize; ++i)
                            block.setSample (ch, i, 1.0f);

                    juce::MidiBuffer midi;
                    if (notePos / blockSize == b)
                        midi = noteOnAt (notePos % blockSize);

                    runBlock (proc, block, midi);
                    CHECK (allocations.load() == 0);

                    if (first < 0)
                        for (int i = 0; i < blockSize; ++i)
                            if (std::abs (block.getSample (0, i)) > 1.0e-4f)
                                { first = b * blockSize + i; break; }
                }

                CHECK (first >= 0);
                return first - notePos;
            };

            CHECK (onset (0.0f) == 0);                  // без офсета — ровно на событии
            CHECK (onset (20.0f) == 960);               // +20 мс это 960 сэмплов
            CHECK (onset (100.0f) == 4800);             // и на краю диапазона тоже

            // Отрицательный офсет событие не двигает: раньше собственного прихода
            // ноту не сыграть. Он двигает всё остальное — это проверка 4.
            CHECK (onset (-20.0f) == 0);
        }

        // 4. Отрицательный MIDI Offset придерживает сухой сигнал ровно на себя,
        //    говорит это число хосту и на столько же опускает нижний предел delay
        //    time: освободившийся бюджет позиции чтения достаётся ему (#17).
        {
            int latency = 0;

            render (out, false, 400.0f, -20.0f, 0.0f, 12000, 20000, &latency);

            CHECK (latency == 960);
            CHECK (peakIndex (out) == 20000 + 960);

            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            setParam (proc, "quality", 1.0f);
            setParam (proc, "timeMode", 0.0f);
            setParam (proc, "midiOffset", 0.0f);
            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            const double full = proc.getMinDelayMs();
            CHECK (std::abs (full - 180.0) < 0.5);

            setParam (proc, "midiOffset", -20.0f);
            CHECK (std::abs (proc.getMinDelayMs() - (full - 20.0)) < 0.01);

            // А в Follow предела нет вовсе: голос читает по выравниванию, и delay
            // time там задаёт только интервал повторов обратной связи.
            setParam (proc, "timeMode", 1.0f);
            CHECK (proc.getMinDelayMs() == 0.0);
        }
    }

    // --- Feedback без кумулятивного питч-дрейфа (#21) ---------------------------
    // Инвариант из CLAUDE.md: feedback снимается ДО питч-стадии. Если бы он снимался
    // после, каждый круг транспонировал бы хвост заново, и подклад уползал бы вверх
    // октавами — эффект Шепарда, только не как приём, а как дефект. Держится это
    // одной строкой в renderSegment, и одной строкой же ломается, поэтому замер.
    // Критерий «feedback = 1.0 не даёт самовозбуждения» проверить нельзя буквально:
    // потолок параметра 95 %, единицы у пользователя нет. Проверяется то, что за этим
    // критерием стоит, — что на самом верху ручки петля затухает, а не разгоняется.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 4096;
        constexpr int blocks = 16;                  // 65536 сэмплов, 1,37 с
        constexpr float delayMs = 200.0f;
        const int delaySamples = juce::roundToInt (delayMs * 0.001 * sr);

        // Вход — короткий тон 220 Гц в самом начале, дальше тишина. Тогда повторы
        // стоят в выходе по отдельности, и высоту каждого можно померить.
        // Нота 72 — октава вверх от Root Key, ratio ровно 2: хвост поёт 440 Гц.
        const auto tailRun = [&] (float feedbackPercent)
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            setParam (proc, "quality", 1.0f);
            setParam (proc, "delayTime", delayMs);
            setParam (proc, "feedback", feedbackPercent);
            setParam (proc, "mix", 100.0f);
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "bypass", 0.0f);
            setParam (proc, "attack", 1.0f);
            setParam (proc, "release", 2000.0f);

            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            juce::AudioBuffer<float> out (1, blockSize * blocks);
            juce::AudioBuffer<float> block (2, blockSize);
            const int burst = juce::roundToInt (0.12 * sr);

            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const int t = b * blockSize + i;
                    const auto v = t < burst
                        ? static_cast<float> (0.5 * std::sin (
                              juce::MathConstants<double>::twoPi * 220.0 * t / sr))
                        : 0.0f;
                    block.setSample (0, i, v);
                    block.setSample (1, i, v);
                }

                runBlock (proc, block, b == 0 ? noteOnAt (0, 72) : juce::MidiBuffer {});
                CHECK (allocations.load() == 0);

                out.copyFrom (0, b * blockSize, block, 0, 0, blockSize);
            }

            return out;
        };

        {
            const auto out = tailRun (90.0f);

            // Четыре круга подряд. Окно берётся внутри повтора, а не на его краю:
            // 4096 сэмплов от начала повтора — это 85 мс, целиком внутри 120-мс тона.
            double previousLevel = 0.0;

            for (int round = 1; round <= 4; ++round)
            {
                const int from = round * delaySamples + 512;

                const double frequency = dominantFrequency (out, from, 4096, 440.0, 200.0, sr);
                const double level = out.getRMSLevel (0, from, 4096);

                std::printf ("  #21 круг %d: высота %.1f Гц, уровень %.4f\n",
                             round, frequency, level);

                // Высота одна и та же на всех кругах. Допуск ±6 Гц — это 23 цента,
                // вчетверо меньше полутона: уползание на полутон за круг поймалось бы
                // сразу, а уползание на октаву (то, что дал бы feedback после питчера)
                // ушло бы за границу поиска вовсе.
                CHECK (std::abs (frequency - 440.0) < 6.0);

                // И затухает, а не растёт. Строгое убывание — это и есть «предсказуемо»:
                // питч-стадия в петле дала бы уровню гулять от круга к кругу.
                if (round > 1)
                    CHECK (level < previousLevel);

                previousLevel = level;
            }
        }

        // Верх ручки. Единицы у пользователя нет, но 95 % — есть, и на них петля
        // обязана затухать. Меряется на длинном прогоне: 0,95 за круг — это 135 кругов
        // до -60 dB, то есть 27 секунд, и здесь важно не «дошло до нуля», а «идёт вниз».
        {
            const auto out = tailRun (95.0f);
            const double early = out.getRMSLevel (0, 2 * delaySamples + 512, 4096);
            const double late  = out.getRMSLevel (0, 6 * delaySamples + 512, 4096);

            std::printf ("  #21 feedback 95 %%: круг 2 %.4f, круг 6 %.4f\n", early, late);

            CHECK (early > 1.0e-4);      // хвост есть, иначе замер пустой
            CHECK (late < early);        // и он идёт вниз, а не разгоняется
        }
    }

    // --- Анти-клик: стресс-тест всех мест разрыва (#25) -------------------------
    // Ревизия. Места, где сигнал может разорваться, и что с каждым сделано:
    //   старт и стоп голоса        — огибающая attack/release, замер в разделе #16
    //   voice stealing             — отдельная стадия stealing со спадом, замер в #13
    //   кража со сменой позиции    — время повернули, или второй голос хоровой пары:
    //                                перезапуск со сбросом и заливкой питчера (сессия 24),
    //                                замер в разделе сразу после стресса
    //   скачок delay time          — голос забирает время в noteOn и до конца ноты
    //                                не меняет (сессия 14, замер в разделе #20);
    //                                петля при этом проезжает кольцо задом наперёд,
    //                                и это ленточный зип, а не разрыв — механизм тот же,
    //                                на котором стоит модуляция (#46)
    //   смена делителя Sync        — то же самое, замер в разделе #20
    //   смена sample rate          — prepareToPlay обнуляет всё, хвост уходит в тишину,
    //                                а не в мусор; замер в разделе смещения дилея
    //   bypass                     — линейный кроссфейд 20 мс (ADR 0003), замер в #11
    //   wrap кольца                — DelayBuffer пишет по модулю, шва нет по построению,
    //                                офлайн-тест буфера гоняет через границу
    //   кроссфейд питчера          — equal-power внутри varispeed, офлайн-тест питчера
    //   переключатель Formants     — движок один и тот же, меняется только карта формант;
    //                                проверяется здесь рывками
    //   ввод дрейфа в петлю        — генераторы крутятся всегда, глубина подмешивается
    //                                (#46), на нуле ручки позиция чтения бит-в-бит прежняя
    //   срабатывание ducking       — множитель по сглаженной огибающей, разрыва нет
    //                                по построению; проверяется здесь рывками
    //   смена Stereo               — раскладку голос берёт в noteOn, кольцо классического
    //                                ping-pong вводится рампой (#55); рывками здесь
    //                                и отдельно в разделе #55, вместе с прототипами
    //   смена Time Mode            — единственное место, где разрыв есть и объявлен:
    //                                выравнивание прыгает на латентность движка разом,
    //                                и сухой сигнал читается с другого места. Это смена
    //                                репортуемой латентности, то есть хост в этот момент
    //                                и сам пересобирает конвейер. В стресс ниже не входит.
    //
    // Детектор: не просто максимальный шаг, а его отношение к 99,9-му процентилю шагов.
    // Разница принципиальная. Смена времени при обратной связи даёт сплошную высокую
    // частоту — шаги там велики все, и порог по максимуму краснел бы на исправном
    // плагине (проверено: 0,46 при пороге 0,05, и разрыва в осциллограмме нет).
    // Щелчок же — это выброс: один шаг много больше всех остальных. Отношение их
    // и различает, а голый максимум — нет.
    {
        struct Rates { double sr; double seconds; };

        // Пять минут на рабочей частоте — это критерий задачи. На 44,1 и 96 кГц
        // по полминуты: там проверяется, что коэффициенты и окна пересчитались,
        // а не выстаивается статистика заново.
        static constexpr Rates rates[] { { 48000.0, 300.0 }, { 44100.0, 30.0 }, { 96000.0, 30.0 } };

        for (const auto& rate : rates)
        {
            constexpr int blockSize = 512;
            const int totalBlocks = static_cast<int> (rate.seconds * rate.sr / blockSize);

            MidiDelayProcessor proc;
            engineDefaults (proc);
            proc.setPlayConfigDetails (2, 2, rate.sr, blockSize);
            proc.prepareToPlay (rate.sr, blockSize);

            juce::Random random (20260910);   // зерно фиксировано: провал обязан повторяться
            juce::AudioBuffer<float> block (2, blockSize);

            std::vector<float> steps;
            steps.reserve (static_cast<size_t> (totalBlocks) * blockSize);

            // Параметры, которые рвут звук, если рвут. Time Mode и sample rate сюда
            // не входят по причине выше; bypass входит — его кроссфейд обязан держать.
            static const char* const jerked[] {
                "delayTime", "division", "sync", "feedback", "mix", "outputGain",
                "diffusion", "modulation", "ducking", "filterLo", "filterHi",
                "width", "stereo", "quality", "formants", "midiOffset",
                "rootKey", "pitchRange", "voices", "attack", "release", "bypass",
                "character", "age",
            };

            // Шаг меряется за одно и то же ВРЕМЯ, а не за один отсчёт. Иначе замер
            // зависит от частоты дискретизации в обе стороны сразу: у гладкого сигнала
            // шаг между соседями падает вдвое с удвоением частоты, а у разрыва не падает
            // никак, и отношение раздувается на исправном плагине (проверено: 12,6
            // на 96 кГц против 4,4 на 48-и при одном и том же звуке).
            const int stride = juce::jmax (1, juce::roundToInt (rate.sr / 48000.0));

            double phase = 0.0;
            std::vector<float> recent (static_cast<size_t> (stride) + 1, 0.0f);
            int recentPos = 0;
            std::vector<int> sounding;
            sounding.reserve (16);

            // Журнал рывков и нот — чтобы про худший шаг было видно, что его вызвало,
            // а не гадать (сессия 24: на MSVC отношение 9,7, и неизвестно от чего).
            struct Jerk { int block; const char* id; float value; };
            struct NoteEvent { int block; int offset; int note; bool on; };
            std::vector<Jerk> jerks;
            std::vector<NoteEvent> notes;

            for (int b = 0; b < totalBlocks; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    // Материал — сумма двух тонов: один синус детектор шагов почти
                    // не нагружает, и щелчок на нём было бы видно даже без процентиля.
                    const auto v = static_cast<float> (
                        0.3 * std::sin (phase) + 0.15 * std::sin (2.7 * phase));
                    phase += juce::MathConstants<double>::twoPi * 196.0 / rate.sr;
                    block.setSample (0, i, v);
                    block.setSample (1, i, v);
                }

                // Рывок параметра примерно раз в пять блоков — это 15 раз в секунду,
                // заведомо чаще, чем крутит человек, и заведомо рывком, а не рампой.
                if (random.nextInt (5) == 0)
                {
                    const char* id = jerked[random.nextInt (static_cast<int> (std::size (jerked)))];
                    auto* p = proc.apvts.getParameter (id);

                    // MIDI Offset дёргается только в неотрицательной половине. Отрицательная
                    // придерживает сухой сигнал, то есть двигает выравнивание всего плагина
                    // и репортуемую хосту латентность — это второе из двух объявленных
                    // исключений ADR 0006, и разрыв там снять нельзя: в тот же момент
                    // пересобирает свою компенсацию и хост. Замер: шаг 1,53 при фоне 0,05.
                    const float v = juce::String (id) == "midiOffset"
                                        ? 0.5f + 0.5f * random.nextFloat() : random.nextFloat();
                    p->setValueNotifyingHost (v);
                    jerks.push_back ({ b, id, v });
                }

                juce::MidiBuffer midi;

                // Ноты случайные и по всему диапазону: восемь голосов переполняются,
                // и voice stealing попадает в замер сам собой.
                if (random.nextInt (3) == 0)
                {
                    const int note = 48 + random.nextInt (36);
                    const float velocity = random.nextFloat() * 0.9f + 0.1f;
                    const int offset = random.nextInt (blockSize);
                    midi.addEvent (juce::MidiMessage::noteOn (1, note, velocity), offset);
                    sounding.push_back (note);
                    notes.push_back ({ b, offset, note, true });
                }

                if (! sounding.empty() && random.nextInt (3) == 0)
                {
                    const int index = random.nextInt (static_cast<int> (sounding.size()));
                    const int offset = random.nextInt (blockSize);
                    midi.addEvent (juce::MidiMessage::noteOff (1, sounding[static_cast<size_t> (index)]), offset);
                    notes.push_back ({ b, offset, sounding[static_cast<size_t> (index)], false });
                    sounding.erase (sounding.begin() + index);
                }

                runBlock (proc, block, midi);
                CHECK (allocations.load() == 0);

                for (int i = 0; i < blockSize; ++i)
                {
                    const float value = block.getSample (0, i);
                    CHECK (std::isfinite (value));
                    CHECK (std::abs (value) < 8.0f);   // ничто не разошлось в бесконечность

                    const float earlier = recent[static_cast<size_t> ((recentPos + 1) % (stride + 1))];
                    steps.push_back (std::abs (value - earlier));
                    recentPos = (recentPos + 1) % (stride + 1);
                    recent[static_cast<size_t> (recentPos)] = value;
                }
            }

            // Три худших шага до того, как nth_element перемешает массив: где они
            // и что им предшествовало. Шаг под номером k относится к блоку k / blockSize.
            std::vector<size_t> order (steps.size());
            std::iota (order.begin(), order.end(), size_t { 0 });
            std::partial_sort (order.begin(), order.begin() + 3, order.end(),
                               [&] (size_t x, size_t y) { return steps[x] > steps[y]; });

            const auto tail = steps.begin() + static_cast<long> (steps.size() * 999 / 1000);
            std::vector<float> worstThree { steps[order[0]], steps[order[1]], steps[order[2]] };
            std::nth_element (steps.begin(), tail, steps.end());

            const double percentile = *tail;
            const double worst = *std::max_element (tail, steps.end());
            const double ratio = percentile > 1.0e-9 ? worst / percentile : 0.0;

            for (size_t k = 0; k < 3; ++k)
            {
                const int blk = static_cast<int> (order[k] / blockSize);
                std::printf ("    шаг %.4f: блок %d, отсчёт %d\n",
                             worstThree[k], blk, static_cast<int> (order[k] % blockSize));

                int shown = 0;
                for (auto j = jerks.rbegin(); j != jerks.rend() && shown < 3; ++j)
                    if (j->block <= blk)
                    {
                        std::printf ("      рывок %-10s = %.3f, %d бл. назад\n", j->id, j->value, blk - j->block);
                        ++shown;
                    }

                for (const auto& n : notes)
                    if (n.block >= blk - 2 && n.block <= blk)
                        std::printf ("      %s %d: блок %d, отсчёт %d\n",
                                     n.on ? "noteOn " : "noteOff", n.note, n.block, n.offset);
            }

            std::printf ("  анти-клик %.0f кГц, %.0f с: худший шаг %.4f, 99,9%% %.4f, отношение %.1f\n",
                         rate.sr / 1000.0, rate.seconds, worst, percentile, ratio);

            // Порог 8: на исправном плагине отношение держится около двух-трёх — это
            // разброс самого материала. Щелчок в тишине даёт десятки, и промахнуться
            // тут негде. Заодно проверяется, что мерили не тишину.
            CHECK (percentile > 1.0e-5);
            CHECK (ratio < 8.0);
        }
    }

    // --- Кража после поворота времени: без склейки в питчере (#25, сессия 24) ----
    // Стресс нашёл это только на траектории MSVC: голос украден, когда время уже другое,
    // и перезапускался с новой позицией чтения, но со старым окном питчера. Склейка
    // выходила из питчера через латентность, на полной огибающей, и HQ с Formants Hold
    // раздувал её до +16 dB. Один голос — чтобы кража была гарантированно.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 512;

        // Величина склейки зависит от того, в какую фазу материала попал прыжок чтения,
        // поэтому целевых времён несколько, шагом около четверти периода, и берётся худшее.
        double worst = 0.0;

        for (int t = 0; t < 8; ++t)
        {
            const float target = 1500.0f + 1.3f * static_cast<float> (t);

            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            setParam (proc, "quality", 1.0f);
            setParam (proc, "formants", 1.0f);
            setParam (proc, "voices", 1.0f);
            setParam (proc, "attack", 20.0f);   // короче латентности HQ: склейку огибающая не прячет
            setParam (proc, "delayTime", 200.0f);
            setParam (proc, "feedback", 0.0f);
            setParam (proc, "mix", 100.0f);
            setParam (proc, "outputGain", 0.0f);

            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            juce::AudioBuffer<float> block (2, blockSize);
            double phase = 0.0;
            float before = 0.0f, after = 0.0f, settled = 0.0f;
            const int stealBlock = static_cast<int> (1.5 * sr / blockSize);
            const int second = static_cast<int> (sr / blockSize);

            for (int b = 0; b < stealBlock + 2 * second; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const auto v = static_cast<float> (0.3 * std::sin (phase) + 0.15 * std::sin (2.7 * phase));
                    phase += juce::MathConstants<double>::twoPi * 196.0 / sr;
                    block.setSample (0, i, v);
                    block.setSample (1, i, v);
                }

                // Время поворачивается за треть секунды до кражи: рампа 50 мс доехала.
                if (b == stealBlock - second / 3)
                    setParam (proc, "delayTime", target);

                runBlock (proc, block, b == 0 ? noteOnAt (100, 67)
                                              : b == stealBlock ? noteOnAt (300, 72) : juce::MidiBuffer {});

                const float peak = block.getMagnitude (0, 0, blockSize);
                const int since = b - stealBlock;

                if (since < 0 && since >= -second / 2) before  = std::max (before, peak);
                if (since >= 0 && since < second)      after   = std::max (after, peak);
                if (since >= second)                   settled = std::max (settled, peak);
            }

            CHECK (before > 0.05f && settled > 0.05f);   // голос звучал и до, и после
            worst = std::max (worst, static_cast<double> (after / std::max (before, settled)));
        }

        std::printf ("  #25 кража после смены времени, HQ + Hold: пик после кражи к пику до и после — %.2f\n", worst);

        CHECK (worst < 1.5);
    }

    // --- Огибающая: velocity, длительность ноты, короткая нота (#16) -----------
    // Вход — постоянная единица при mix 100 %, ratio 1: на выходе видна ровно
    // огибающая голоса, и её можно читать прямо из буфера. Движок по умолчанию.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 512;

        const auto setup = [] (MidiDelayProcessor& proc, float attackMs, float releaseMs)
        {
            setParam (proc, "delayTime", 200.0f);
            setParam (proc, "feedback", 0.0f);
            setParam (proc, "mix", 100.0f);
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "bypass", 0.0f);
            setParam (proc, "attack", attackMs);
            setParam (proc, "release", releaseMs);
            setParam (proc, "voices", 8.0f);
            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);
        };

        // 1. Velocity через корень: мягче линейной, но слышимо. Уровень снимается
        //    с удержанной ноты, когда атака давно кончилась и огибающая стоит на пике.
        {
            const auto sustainLevel = [&setup] (int velocity)
            {
                MidiDelayProcessor proc;
                engineDefaults (proc);
                plainLoop (proc);
                setup (proc, 1.0f, 300.0f);

                juce::AudioBuffer<float> buffer (2, blockSize);
                for (int b = 0; b < 40; ++b) { fillDC (buffer); runBlock (proc, buffer); }

                juce::MidiBuffer midi;
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) velocity), 0);
                fillDC (buffer); runBlock (proc, buffer, midi);
                fillDC (buffer); runBlock (proc, buffer);

                return buffer.getSample (0, blockSize - 1);
            };

            for (const int velocity : { 1, 32, 64, 100, 127 })
                CHECK (std::abs (sustainLevel (velocity)
                                 - std::sqrt (velocity / 127.0f)) < 2.0e-3f);

            // Слышимость: полный размах velocity — это 21 dB, а не пара децибел.
            CHECK (sustainLevel (127) > sustainLevel (1) * 10.0f);
            // И мягче линейной: на velocity 64 хвост садится на 3 dB, а не на 6.
            CHECK (sustainLevel (64) > 0.6f);
        }

        // 2. Длина хвоста следует длительности ноты: держали дольше — звучало дольше
        //    ровно на столько же. Порог 1e-4 ловит конец спада с точностью до сэмплов.
        {
            const auto tailEnd = [&setup] (int holdBlocks)
            {
                MidiDelayProcessor proc;
                engineDefaults (proc);
                plainLoop (proc);
                setup (proc, 1.0f, 50.0f);   // release 50 мс = 2400 сэмплов

                juce::AudioBuffer<float> buffer (2, blockSize);
                for (int b = 0; b < 40; ++b) { fillDC (buffer); runBlock (proc, buffer); }

                int last = -1;

                for (int b = 0; b < holdBlocks + 20; ++b)
                {
                    juce::MidiBuffer midi;
                    if (b == 0)          midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
                    if (b == holdBlocks) midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);

                    fillDC (buffer);
                    runBlock (proc, buffer, midi);

                    for (int i = 0; i < blockSize; ++i)
                        if (std::abs (buffer.getSample (0, i)) > 1.0e-4f)
                            last = b * blockSize + i;
                }

                return last;
            };

            const int shortNote = tailEnd (4);
            const int longNote  = tailEnd (20);

            CHECK (std::abs ((longNote - shortNote) - 16 * blockSize) <= 1);

            // И хвост кончается спустя release после note off, а не раньше и не позже.
            CHECK (shortNote > 4 * blockSize);
            CHECK (shortNote < 4 * blockSize + 2400);
        }

        // 3. Нота короче атаки: огибающая уходит в спад с недобранного уровня.
        //    Ни щелчка, ни залипшего голоса — ровно то, чего требует #16.
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            setup (proc, 50.0f, 100.0f);   // атака 50 мс = 2400 сэмплов

            juce::AudioBuffer<float> buffer (2, blockSize);
            for (int b = 0; b < 40; ++b) { fillDC (buffer); runBlock (proc, buffer); }

            float lastSample = buffer.getSample (0, blockSize - 1);
            float maxStep = 0.0f;
            float maxLevel = 0.0f;

            for (int b = 0; b < 40; ++b)
            {
                juce::MidiBuffer midi;

                if (b == 0)   // нота длиной 32 сэмпла, в семьдесят пять раз короче атаки
                {
                    midi.addEvent (juce::MidiMessage::noteOn  (1, 60, 1.0f), 0);
                    midi.addEvent (juce::MidiMessage::noteOff (1, 60), 32);
                }

                fillDC (buffer);
                runBlock (proc, buffer, midi);
                CHECK (allocations.load() == 0);

                for (int i = 0; i < blockSize; ++i)
                {
                    const float s = buffer.getSample (0, i);
                    maxStep = juce::jmax (maxStep, std::abs (s - lastSample));
                    maxLevel = juce::jmax (maxLevel, std::abs (s));
                    lastSample = s;
                }
            }

            // Самый крутой участок вогнутой атаки — её начало: 2/2400 на сэмпл.
            // Порог 0,01 выше этого на порядок и поймал бы любой честный разрыв.
            CHECK (maxStep < 0.01f);

            // Звук был, но тихий: 32 сэмпла атаки из 2400 дают 0,026 уровня.
            CHECK (maxLevel > 1.0e-3f);
            CHECK (maxLevel < 0.1f);

            // Голос освободился: спад доехал ровно до нуля, а не завис на остатке.
            CHECK (buffer.getSample (0, blockSize - 1) == 0.0f);
        }
    }

    // --- Насыщение в петле (#44) -----------------------------------------------
    // Ограничитель стоит на отводе обратной связи, и проверяется ровно это: на feedback 0
    // плагин обязан остаться линейным (отвод равен нулю, ограничителю нечего трогать),
    // а на 95 % — перестать быть линейным, иначе он ничего не делает и его надо убрать.
    // Линейность меряется отношением уровней на двух амплитудах входа: у линейной
    // системы оно равно отношению амплитуд, у ограниченной — меньше.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 512;
        constexpr int blocks = 400;                  // 4,3 с: петля успевает набрать круги

        const auto tailLevel = [&] (float feedback, float amplitude)
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            setParam (proc, "sync", 0.0f);
            setParam (proc, "delayTime", 250.0f);
            setParam (proc, "mix", 100.0f);          // меряется хвост, а не сухой
            setParam (proc, "feedback", feedback);
            setParam (proc, "diffusion", 0.0f);
            setParam (proc, "modulation", 0.0f);
            setParam (proc, "ducking", 0.0f);        // жмёт wet по сухому, замеру мешает
            setParam (proc, "filterLo", 20.0f);
            setParam (proc, "filterHi", 20000.0f);
            setParam (proc, "attack", 1.0f);

            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            juce::AudioBuffer<float> block (2, blockSize);
            double phase = 0.0, sum = 0.0;
            long counted = 0;

            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const auto v = static_cast<float> (amplitude * std::sin (phase));
                    phase += juce::MathConstants<double>::twoPi * 196.0 / sr;
                    block.setSample (0, i, v);
                    block.setSample (1, i, v);
                }

                // Нота 60 — унисон при Root Key C: питчер ничего не транспонирует,
                // и в замер не лезет его собственная нелинейность.
                runBlock (proc, block, b == 0 ? noteOnAt (0) : juce::MidiBuffer {});
                CHECK (allocations.load() == 0);

                if (b < blocks / 2)
                    continue;                        // первая половина — разгон петли

                for (int i = 0; i < blockSize; ++i)
                {
                    const double x = block.getSample (0, i);
                    sum += x * x;
                    ++counted;
                }
            }

            CHECK (counted > 0);
            return std::sqrt (sum / counted);
        };

        // Отношение амплитуд входа шестикратное; у линейной системы такое же
        // отношение выходов.
        constexpr float quiet = 0.15f, loud = 0.9f;

        const double cleanRatio = tailLevel (0.0f, loud) / tailLevel (0.0f, quiet);
        const double hotRatio   = tailLevel (95.0f, loud) / tailLevel (95.0f, quiet);

        std::printf ("  #44 отношение выходов на входах 0,15 и 0,9 (линейное — 6,00):"
                     " feedback 0 %% — %.2f, feedback 95 %% — %.2f\n", cleanRatio, hotRatio);

        // Без обратной связи ограничителю нечего трогать: плагин линеен. Допуск 1 %
        // на фазовый вокодер — он решает по магнитудам, и на два порядка амплитуды
        // подряд бит-в-бит не обязан.
        CHECK (std::abs (cleanRatio - 6.0) < 0.06);

        // С обратной связью петля упирается в потолок: тихий вход проходит нетронутым,
        // громкий подрезан, отношение падает. Если однажды станет ровно шесть —
        // ограничитель перестал работать, и эта строка об этом скажет.
        CHECK (hotRatio < 5.5);

        // И потолок настоящий. Граница считается, а не выдумывается: в кольцо пишется
        // сухой плюс ограниченный отвод, то есть не больше amplitude + loopCeiling,
        // и следующий круг читает уже это. Потолок кольца — 0,9 + 1,0 = 1,9,
        // и среднеквадратичный уровень хвоста обязан лежать ниже него.
        const double hotLevel = tailLevel (95.0f, loud);
        std::printf ("  #44 уровень хвоста на входе 0,9 и feedback 95 %%: %.2f\n", hotLevel);
        CHECK (hotLevel < 1.9);
    }

    // --- Характер: Tape и Lo-Fi (#56) ------------------------------------------
    // Критерии задачи по одному. Clean не слушает Age, а любой характер на Age 0 — это
    // Clean бит-в-бит: на этом стоит смена характера через ноль. Характер слышен уже
    // на feedback 0. На 95 % петля не разгоняется, а шум после хвоста гаснет вместе с ним.
    // Смена характера и рывки Age на звучащем хвосте не щёлкают. Проект, сохранённый
    // без этих параметров, открывается в Clean. Везде ноль аллокаций.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 512;

        // Синус 196 Гц на ноте 60 — унисон при Root Key C: питчер не транспонирует,
        // и в замер не лезет его собственная нелинейность. Free, mix 100 — меряется хвост,
        // окраска петли выключена. Вход звучит первые burstBlocks блоков, дальше тишина;
        // нота держится до конца. Возвращается левый канал целиком.
        const auto run = [&] (float character, float age, float feedback, float delayMs,
                              float amplitude, int blocks, int burstBlocks)
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            plainLoop (proc);
            setParam (proc, "character", character);
            setParam (proc, "age", age);
            setParam (proc, "delayTime", delayMs);
            setParam (proc, "feedback", feedback);
            setParam (proc, "mix", 100.0f);
            setParam (proc, "attack", 1.0f);

            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            std::vector<float> out;
            out.reserve (static_cast<size_t> (blocks) * blockSize);
            juce::AudioBuffer<float> block (2, blockSize);
            double phase = 0.0;

            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const auto v = b < burstBlocks ? static_cast<float> (amplitude * std::sin (phase)) : 0.0f;
                    phase += juce::MathConstants<double>::twoPi * 196.0 / sr;
                    block.setSample (0, i, v);
                    block.setSample (1, i, v);
                }

                runBlock (proc, block, b == 0 ? noteOnAt (0) : juce::MidiBuffer {});
                CHECK (allocations.load() == 0);

                for (int i = 0; i < blockSize; ++i)
                    out.push_back (block.getSample (0, i));
            }

            return out;
        };

        // 1. Clean не слушает Age, и любой характер на Age 0 равен Clean бит-в-бит.
        //    Второе не формальность: смена характера идёт через ноль глубины, и останься
        //    на нуле хоть что-то, подмена была бы слышна. Петля на 60 %, чтобы сравнивались
        //    и круги обратной связи, где живёт wow, а не только первый хвост.
        {
            const auto clean     = run (0.0f,   0.0f, 60.0f, 250.0f, 0.5f, 200, 100);
            const auto cleanAged = run (0.0f, 100.0f, 60.0f, 250.0f, 0.5f, 200, 100);
            const auto tapeZero  = run (1.0f,   0.0f, 60.0f, 250.0f, 0.5f, 200, 100);
            const auto loFiZero  = run (2.0f,   0.0f, 60.0f, 250.0f, 0.5f, 200, 100);

            CHECK (clean == cleanAged);
            CHECK (clean == tapeZero);
            CHECK (clean == loFiZero);
        }

        // 2. Характер слышен на feedback 0 — ради этого он и стоит на входе кольца.
        //    Мера — всё, что в хвосте не основной тон, относительно основного (THD+N):
        //    у чистого хвоста это пол питчера, у окрашенного — призвуки, ступеньки и шум.
        //    Окно — последняя секунда из трёх, ровно 196 периодов: ДПФ в точке без утечки.
        {
            const auto distortion_dB = [] (const std::vector<float>& x)
            {
                constexpr int count = 48000;
                const size_t from = x.size() - count;
                double re = 0.0, im = 0.0, power = 0.0;

                for (int i = 0; i < count; ++i)
                {
                    const double s = x[from + static_cast<size_t> (i)];
                    const double a = juce::MathConstants<double>::twoPi * 196.0 * i / 48000.0;
                    re += s * std::cos (a);
                    im += s * std::sin (a);
                    power += s * s;
                }

                const double tone = 2.0 * (re * re + im * im) / (static_cast<double> (count) * count);
                return 10.0 * std::log10 (std::max (power / count - tone, 1.0e-30) / tone);
            };

            constexpr int blocks = 282;   // три секунды
            const double clean  = distortion_dB (run (0.0f,  0.0f, 0.0f, 250.0f, 0.5f, blocks, blocks));
            const double tape30 = distortion_dB (run (1.0f, 30.0f, 0.0f, 250.0f, 0.5f, blocks, blocks));
            const double tape70 = distortion_dB (run (1.0f, 70.0f, 0.0f, 250.0f, 0.5f, blocks, blocks));
            const double loFi50 = distortion_dB (run (2.0f, 50.0f, 0.0f, 250.0f, 0.5f, blocks, blocks));

            std::printf ("  #56 THD+N хвоста на feedback 0, синус 0,5: Clean %.1f dB, Tape 30 %.1f dB, "
                         "Tape 70 %.1f dB, Lo-Fi 50 %.1f dB\n", clean, tape30, tape70, loFi50);

            // Порог -40 dB — один процент по амплитуде: ниже него «характер» на синусе
            // был бы формальностью. Tape 70 обязан быть грязнее Tape 30 заметно, иначе
            // Age не делает того, что обещает подпись.
            CHECK (clean < -50.0);
            CHECK (tape30 > -40.0);
            CHECK (tape70 > tape30 + 6.0);
            CHECK (loFi50 > -40.0);
        }

        // 3. Feedback 95 %: петля не разгоняется, шум в тишине после хвоста гаснет. Худший
        //    случай для разгона — Age 100 и петля без фильтров: полосы петли снимают часть
        //    усиления, без них характеру помочь нечем. Вход горячий, 0,9, одну секунду,
        //    дальше тишина, нота держится. Петля 200 мс, а не короче: ниже 180 мс время
        //    в Free молча подтягивается до латентности HQ (#17), и первая версия замера,
        //    заказавшая 50 мс, насчитала триста кругов там, где их было восемьдесят.
        //    Сорок пять секунд — двести двадцать кругов, чистый хвост за них уходит ниже
        //    -90 dBFS. Потолок уровня тот же, что в #44: сухой 0,9 плюс потолок отвода 1,0.
        {
            constexpr int blocks = 4219, burst = 94;   // 45 с, звучит первая
            static constexpr float cases[][2] { { 0.0f, 0.0f }, { 1.0f, 100.0f }, { 2.0f, 100.0f } };

            const auto rms = [] (const std::vector<float>& x, size_t from, size_t count)
            {
                double sum = 0.0;
                for (size_t i = from; i < from + count; ++i)
                    sum += static_cast<double> (x[i]) * x[i];
                return std::sqrt (sum / static_cast<double> (count));
            };

            for (const auto& c : cases)
            {
                const auto x = run (c[0], c[1], 95.0f, 200.0f, 0.9f, blocks, burst);

                double loudest = 0.0;
                for (size_t from = 0; from + 4800 <= x.size(); from += 4800)
                    loudest = std::max (loudest, rms (x, from, 4800));

                const double afterBurst = rms (x, 72000, 48000);          // 1,5-2,5 с
                const double last = rms (x, x.size() - 48000, 48000);     // последняя секунда

                std::printf ("  #56 feedback 95 %%, характер %.0f, Age %.0f: пик окна %.3f, "
                             "после входа %.1f dBFS, в конце %.1f dBFS\n", c[0], c[1], loudest,
                             20.0 * std::log10 (std::max (afterBurst, 1.0e-12)),
                             20.0 * std::log10 (std::max (last, 1.0e-12)));

                CHECK (loudest < 1.9);
                CHECK (afterBurst > 1.0e-3);                // замер не пустой: хвост был
                CHECK (last < afterBurst * 1.0e-3);         // гаснет, а не держится шумом
                CHECK (last < 1.0e-4);                      // -80 dBFS: в тишине пусто
            }
        }

        // 4. Смена Character и рывки Age на звучащем хвосте. Детектор тот же, что в стресс-
        //    тесте (#25): отношение худшего шага к 99,9-му процентилю, порог 8. Дёргаются
        //    только эти два параметра — в общем стрессе их вклад тонет в рывках двадцати
        //    остальных. Раз в 106 мс: смена характера через ноль занимает 100, и следующий
        //    рывок приходит, когда предыдущий едва доехал. Окраска петли по умолчанию.
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            setParam (proc, "sync", 0.0f);
            setParam (proc, "delayTime", 300.0f);
            setParam (proc, "feedback", 60.0f);
            setParam (proc, "mix", 100.0f);
            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            constexpr int blocks = 2820;   // 30 с
            juce::Random random (20260911);
            juce::AudioBuffer<float> block (2, blockSize);
            std::vector<float> steps;
            steps.reserve (static_cast<size_t> (blocks) * blockSize);
            float previous = 0.0f;
            double phase = 0.0;

            juce::MidiBuffer chord;
            for (const int note : { 60, 64, 67 })
                chord.addEvent (juce::MidiMessage::noteOn (1, note, 0.9f), 0);

            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const auto v = static_cast<float> (0.3 * std::sin (phase) + 0.15 * std::sin (2.7 * phase));
                    phase += juce::MathConstants<double>::twoPi * 196.0 / sr;
                    block.setSample (0, i, v);
                    block.setSample (1, i, v);
                }

                if (b > 0 && b % 10 == 0)
                {
                    if (random.nextBool())
                        setParam (proc, "character", static_cast<float> (random.nextInt (3)));
                    else
                        proc.apvts.getParameter ("age")->setValueNotifyingHost (random.nextFloat());
                }

                runBlock (proc, block, b == 0 ? chord : juce::MidiBuffer {});
                CHECK (allocations.load() == 0);

                for (int i = 0; i < blockSize; ++i)
                {
                    const float value = block.getSample (0, i);
                    CHECK (std::isfinite (value));
                    CHECK (std::abs (value) < 8.0f);
                    steps.push_back (std::abs (value - previous));
                    previous = value;
                }
            }

            const auto tail = steps.begin() + static_cast<long> (steps.size() * 999 / 1000);
            std::nth_element (steps.begin(), tail, steps.end());
            const double percentile = *tail;
            const double worst = *std::max_element (tail, steps.end());
            const double ratio = worst / juce::jmax (1.0e-6, percentile);

            std::printf ("  #56 смена характера и рывки Age на хвосте: худший шаг %.4f, "
                         "99,9%% %.4f, отношение %.1f\n", worst, percentile, ratio);

            CHECK (percentile > 1.0e-5);
            CHECK (ratio < 8.0);
        }

        // 5. Проект, сохранённый до сессии 20, этих параметров не содержит — и обязан
        //    открыться в Clean, а не в том, что стояло в экземпляре до загрузки. Сессия 13
        //    уже ломала пользователю проект в FL сменой идентификаторов; здесь идентификаторы
        //    не меняются, но проверяется именно загрузка старого состояния, а не обещание.
        {
            MidiDelayProcessor source;
            engineDefaults (source);
            setParam (source, "feedback", 42.0f);

            auto legacy = source.apvts.copyState();

            for (const char* id : { "character", "age" })
                legacy.removeChild (legacy.getChildWithProperty ("id", id), nullptr);

            CHECK (! legacy.getChildWithProperty ("id", "character").isValid());

            juce::MemoryBlock block;
            if (auto xml = legacy.createXml())
                juce::AudioProcessor::copyXmlToBinary (*xml, block);

            MidiDelayProcessor restored;
            engineDefaults (restored);
            setParam (restored, "character", 2.0f);   // заведомо не Clean — чтобы было что вернуть
            setParam (restored, "age", 90.0f);
            restored.setStateInformation (block.getData(), static_cast<int> (block.getSize()));

            CHECK (getParam (restored, "character") == 0.0f);
            CHECK (getParam (restored, "age") == 30.0f);
            CHECK (std::abs (getParam (restored, "feedback") - 42.0f) < 0.01f);
        }
    }

    // --- Живой темп: сетка едет за хостом на ходу (#20) -------------------------
    // Заменяет ту часть ручной проверки в FL, которую офлайн-тест умеет: до сессии 15
    // темп подсовывался фиксированным на весь прогон, и «сетка едет за проектом»
    // проверялось тем, что при другом BPM другой прогон даёт другое время. Это не то же
    // самое: хост меняет темп в середине, между блоками, и плагин обязан пересчитать
    // сетку к следующей ноте, а не к следующему открытию проекта.
    {
        constexpr double sr = 48000.0;
        constexpr int longBlock = 16384;

        MidiDelayProcessor proc;
        engineDefaults (proc);
        setParam (proc, "sync", 1.0f);
        setParam (proc, "division", 5.0f);      // 1/4
        setParam (proc, "mix", 100.0f);
        setParam (proc, "feedback", 0.0f);
        setParam (proc, "diffusion", 0.0f);
        setParam (proc, "modulation", 0.0f);
        setParam (proc, "ducking", 0.0f);
        setParam (proc, "filterLo", 20.0f);
        setParam (proc, "filterHi", 20000.0f);
        setParam (proc, "attack", 1.0f);

        FixedTempoPlayHead playHead (120.0);
        proc.setPlayHead (&playHead);
        proc.setPlayConfigDetails (2, 2, sr, longBlock);
        proc.prepareToPlay (sr, longBlock);

        juce::AudioBuffer<float> block (2, longBlock);

        // Импульс с нотой, дальше тишина: ищется, на каком сэмпле вышел хвост.
        const auto tailPeak = [&]
        {
            // Сначала отпустить прошлую ноту и прогнать тишину: голос держится, пока
            // нота нажата, и без этого замер нашёл бы пик от предыдущего темпа.
            block.clear();
            juce::MidiBuffer off;
            off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            runBlock (proc, block, off);

            for (int b = 0; b < 3; ++b)
            {
                block.clear();
                runBlock (proc, block);
            }

            int peak = -1;
            float peakValue = 0.0f;

            for (int b = 0; b < 4; ++b)
            {
                block.clear();

                if (b == 0)
                {
                    block.setSample (0, 0, 1.0f);
                    block.setSample (1, 0, 1.0f);
                }

                runBlock (proc, block, b == 0 ? noteOnAt (0) : juce::MidiBuffer {});
                CHECK (allocations.load() == 0);

                for (int i = 0; i < longBlock; ++i)
                    if (std::abs (block.getSample (0, i)) > peakValue)
                    {
                        peakValue = std::abs (block.getSample (0, i));
                        peak = b * longBlock + i;
                    }
            }

            CHECK (peakValue > 0.8f);
            return peak;
        };

        // 120 BPM: четверть — ровно 500 мс, то же число, что стоит на ручке по умолчанию.
        // Это третий пункт ручной проверки: при 120 включение Sync не двигает звук.
        CHECK (std::abs (proc.requestedDelayMs() - 500.0) < 0.01);
        CHECK (std::abs (proc.getSyncBpm() - 120.0) < 0.01);
        const int peak120 = tailPeak();
        CHECK (std::abs (peak120 - juce::roundToInt (0.500 * sr)) <= 48);

        // Хост поменял темп в середине сессии. Сетка обязана поехать за ним.
        playHead.bpm = 150.0;

        const int peak150 = tailPeak();
        CHECK (std::abs (proc.getSyncBpm() - 150.0) < 0.01);
        CHECK (std::abs (proc.requestedDelayMs() - 400.0) < 0.01);
        CHECK (std::abs (peak150 - juce::roundToInt (0.400 * sr)) <= 48);

        playHead.bpm = 90.0;

        const int peak90 = tailPeak();
        CHECK (std::abs (proc.requestedDelayMs() - 666.67) < 0.01);
        CHECK (std::abs (peak90 - juce::roundToInt (0.66667 * sr)) <= 48);

        std::printf ("  живой темп 1/4: 120 BPM -> %d, 150 -> %d, 90 -> %d сэмплов\n",
                     peak120, peak150, peak90);

        // Ручка Delay Time при этом не двигалась: сетка меняет эффективное время,
        // а не значение параметра. Иначе смена темпа проекта дралась бы с автоматизацией
        // хоста и затирала выставленное пользователем число (тот же довод, что у #17).
        CHECK (getParam (proc, "delayTime") == 500.0f);
    }

    // Непрерывный разгон темпа при звучащем хвосте: так ведёт себя автоматизация темпа
    // в проекте. Заодно это второй пункт ручной проверки — смена делителя на ходу.
    //
    // Критерий тут относительный, и это не послабление. Абсолютный порог по отношению
    // выброса к процентилю зависит от материала: у стресс-теста фон 0,09 (два тона плюс
    // все ручки в рывках), здесь один тон и фон 0,02, и то же самое отношение получается
    // вчетверо больше на исправном плагине. Сравнивается поэтому едущий темп с тем же
    // прогоном на стоячем: разница между ними и есть цена движения темпа.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 512;
        constexpr int totalBlocks = 900;

        const auto run = [&] (bool movingTempo, double* percentileOut)
        {
            MidiDelayProcessor proc;
            engineDefaults (proc);
            setParam (proc, "sync", 1.0f);
            setParam (proc, "mix", 60.0f);

            FixedTempoPlayHead playHead (125.0);
            proc.setPlayHead (&playHead);
            proc.setPlayConfigDetails (2, 2, sr, blockSize);
            proc.prepareToPlay (sr, blockSize);

            juce::AudioBuffer<float> block (2, blockSize);
            std::vector<float> steps;
            steps.reserve (static_cast<size_t> (totalBlocks) * blockSize);

            double phase = 0.0;
            float previous = 0.0f;

            for (int b = 0; b < totalBlocks; ++b)
            {
                // Темп ползёт 90 -> 160 и обратно: и разгон, и торможение.
                if (movingTempo)
                    playHead.bpm = 125.0 + 35.0 * std::sin (b * 0.01);

                if (b % 120 == 60)   // делитель меняется поверх едущего темпа
                    setParam (proc, "division", static_cast<float> (2 + (b / 120) % 10));

                for (int i = 0; i < blockSize; ++i)
                {
                    const auto v = static_cast<float> (0.3 * std::sin (phase));
                    phase += juce::MathConstants<double>::twoPi * 196.0 / sr;
                    block.setSample (0, i, v);
                    block.setSample (1, i, v);
                }

                juce::MidiBuffer midi;

                if (b % 20 == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 60 + (b / 20) % 8, 0.9f), 0);

                runBlock (proc, block, midi);
                CHECK (allocations.load() == 0);

                for (int i = 0; i < blockSize; ++i)
                {
                    const float value = block.getSample (0, i);
                    CHECK (std::isfinite (value));
                    steps.push_back (std::abs (value - previous));
                    previous = value;
                }
            }

            const auto tail = steps.begin() + static_cast<long> (steps.size() * 999 / 1000);
            std::nth_element (steps.begin(), tail, steps.end());

            *percentileOut = *tail;
            return static_cast<double> (*std::max_element (steps.begin(), steps.end()));
        };

        double movingFloor = 0.0, steadyFloor = 0.0;
        const double moving = run (true,  &movingFloor);
        const double steady = run (false, &steadyFloor);

        const double movingRatio = moving / juce::jmax (1.0e-6, movingFloor);
        const double steadyRatio = steady / juce::jmax (1.0e-6, steadyFloor);

        std::printf ("  темп на ходу 90-160 BPM: отношение %.1f, тот же прогон на 125 BPM: %.1f\n",
                     movingRatio, steadyRatio);

        // Полтора раза — это запас на то, что едущая сетка сама по себе даёт больше
        // разных времён, а значит больше стыков голосов. Настоящий разрыв от смены темпа
        // выехал бы кратно: смена времени рвёт на всю амплитуду хвоста, а не на четверть.
        CHECK (movingRatio < 1.5 * steadyRatio);
    }

    // --- Фабричные пресеты (#48) -----------------------------------------------
    // Пресет — это не «набор красивых чисел», а обещание: выбрал имя — получил ровно
    // тот звук, независимо от того, что стояло раньше. Проверяется само обещание:
    // все названные значения доехали, всё неназванное вернулось к умолчанию,
    // выбор переживает сохранение проекта и не щёлкает на звучащем хвосте.
    {
        MidiDelayProcessor proc;

        // Плагин открывается на Chord Pad, а не на тихом подкладе — это и есть #48.
        CHECK (proc.getNumPrograms() == 6);
        CHECK (proc.getCurrentProgram() == 0);
        CHECK (proc.getProgramName (0) == "Chord Pad");
        CHECK (getParam (proc, "timeMode") > 0.5f);    // Follow
        CHECK (getParam (proc, "mix") == 70.0f);
        CHECK (getParam (proc, "feedback") == 0.0f);

        // Вердикт сессии 21 (#56): Chord Pad остался Clean. Stereo не назван — Auto,
        // и в Follow это хор (#55).
        CHECK (getParam (proc, "character") == 0.0f);
        CHECK (getParam (proc, "stereo") == 0.0f);
        CHECK (proc.isChoirStereo());

        // Ни один пресет не задаёт время миллисекундами: 857 мс верны ровно при
        // 140 BPM и врут при любом другом темпе проекта. Время — только делителем.
        // Проверяется тем, что delayTime после любого пресета равен умолчанию.
        const float defaultDelay = 500.0f;

        for (int i = 0; i < proc.getNumPrograms(); ++i)
        {
            proc.setCurrentProgram (i);
            CHECK (proc.getCurrentProgram() == i);
            CHECK (proc.getProgramName (i).isNotEmpty());
            CHECK (getParam (proc, "delayTime") == defaultDelay);

            // Имена в UI только ASCII (CLAUDE.md): кириллица в окне рассыпается.
            for (auto c : proc.getProgramName (i))
                CHECK (c >= 32 && c < 127);
        }

        // Пресет — снимок целиком. Ghost Choir поднимает release до 1500 и режет верх;
        // после него Arp Echo обязан звучать ровно так же, как на свежем экземпляре,
        // а не тащить чужой хвост. Это единственное место, где ошибка была бы не видна
        // глазами: параметр, который пресет не называет, просто остался бы чужим.
        proc.setCurrentProgram (3);                    // Ghost Choir
        CHECK (getParam (proc, "release") == 1500.0f);
        CHECK (getParam (proc, "filterHi") == 6000.0f);

        proc.setCurrentProgram (1);                    // Arp Echo
        CHECK (getParam (proc, "release") == 300.0f);  // умолчание вернулось
        CHECK (getParam (proc, "filterHi") == 12000.0f);
        CHECK (getParam (proc, "feedback") == 45.0f);  // а своё встало
        CHECK (getParam (proc, "division") == 7.0f);   // 1/8.
        CHECK (getParam (proc, "sync") > 0.5f);

        // Arp Echo во Free и Stereo не называет: Auto звучит ping-pong (#55).
        CHECK (! proc.isChoirStereo());

        // Sung Vocal — Tape 30 по вердикту сессии 21 (#56) и Ping-Pong явно. Ghost Choir
        // во Free, и Choir назван у него явно: Auto сделал бы из хора ping-pong (#55).
        proc.setCurrentProgram (4);
        CHECK (getParam (proc, "character") == 1.0f);
        CHECK (getParam (proc, "age") == 30.0f);
        CHECK (getParam (proc, "stereo") == 2.0f);

        proc.setCurrentProgram (3);
        CHECK (getParam (proc, "character") == 0.0f);  // Tape из Sung Vocal не утёк в соседа
        CHECK (getParam (proc, "stereo") == 1.0f);
        CHECK (proc.isChoirStereo());

        proc.setCurrentProgram (5);
        CHECK (getParam (proc, "stereo") == 2.0f);

        // Обход — состояние пользователя, а не звука: пресет его не трогает.
        // Иначе выбор пресета на выключенном плагине включал бы его молча.
        setParam (proc, "bypass", 1.0f);
        proc.setCurrentProgram (4);
        CHECK (getParam (proc, "bypass") > 0.5f);
        setParam (proc, "bypass", 0.0f);

        // Индекс за границами списка не должен ни падать, ни менять звук.
        proc.setCurrentProgram (99);
        CHECK (proc.getCurrentProgram() == 4);
        CHECK (proc.getProgramName (99).isEmpty());

        // Выбор переживает сохранение и загрузку проекта. Параметры едут сами через
        // APVTS, а вот индекс — отдельным полем, иначе окно после загрузки показывало бы
        // чужое имя. И наоборот: загрузка не имеет права звать applyPreset, иначе
        // подкрученное после выбора пресета затиралось бы при каждом открытии проекта.
        proc.setCurrentProgram (5);                    // Dense Rap
        setParam (proc, "mix", 33.0f);                 // и рука пользователя поверх

        juce::MemoryBlock state;
        proc.getStateInformation (state);

        MidiDelayProcessor restored;
        engineDefaults (restored);
        restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));

        CHECK (restored.getCurrentProgram() == 5);
        CHECK (getParam (restored, "mix") == 33.0f);   // рука пользователя пережила
        CHECK (getParam (restored, "ducking") == 80.0f);
        CHECK (getParam (restored, "division") == 0.0f);
    }

    // Масштаб окна переживает сохранение и не пускает внутрь мусор (#29).
    // Проверяется здесь, а не в редакторе: поле живёт в процессоре именно затем,
    // чтобы пережить закрытие окна, и ломается оно на загрузке состояния.
    {
        MidiDelayProcessor proc;
        engineDefaults (proc);
        proc.editorScale = 1.25f;

        juce::MemoryBlock state;
        proc.getStateInformation (state);

        MidiDelayProcessor restored;
        engineDefaults (restored);
        restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        CHECK (std::abs (restored.editorScale - 1.25f) < 1.0e-6f);

        // Состояние приходит из файла проекта, то есть извне: и ноль, и десятка
        // должны упереться в пределы редактора, а не доехать до setSize.
        for (const float bad : { 0.0f, -3.0f, 10.0f })
        {
            MidiDelayProcessor source;
            engineDefaults (source);
            source.editorScale = bad;

            juce::MemoryBlock block;
            source.getStateInformation (block);

            MidiDelayProcessor guarded;
            engineDefaults (guarded);
            guarded.setStateInformation (block.getData(), static_cast<int> (block.getSize()));
            CHECK (guarded.editorScale >= 0.75f && guarded.editorScale <= 1.5f);
        }

        // Проект, сохранённый до сессии 17, поля не содержит — и должен открыться
        // в базовом размере, а не в нулевом.
        MidiDelayProcessor old;
        engineDefaults (old);
        auto legacy = old.apvts.copyState();
        legacy.setProperty ("stateVersion", 1, nullptr);
        legacy.setProperty ("preset", 0, nullptr);

        juce::MemoryBlock block;

        if (auto xml = legacy.createXml())
            juce::AudioProcessor::copyXmlToBinary (*xml, block);

        MidiDelayProcessor fresh;
        engineDefaults (fresh);
        fresh.editorScale = 1.4f;                      // заведомо не 1.0, чтобы было что стереть
        fresh.setStateInformation (block.getData(), static_cast<int> (block.getSize()));
        CHECK (std::abs (fresh.editorScale - 1.0f) < 1.0e-6f);

        std::printf ("  масштаб окна: 1.25 пережил сохранение, мусор обрезан, "
                     "старый проект открывается в 1.0\n");
    }

    // Смена пресета на звучащем хвосте. Пресет двигает два десятка параметров разом —
    // это худший случай для всего, что включается по месту, и сессия 14 нашла ровно
    // такой разрыв у фильтров петли. Детектор тот же, что в стресс-тесте: отношение
    // худшего шага к 99,9-му процентилю, а не голый максимум.
    //
    // Chord Pad (пресет 0) в перебор не входит: он единственный меняет Time Mode,
    // а смена Time Mode — объявленный разрыв (ADR 0006, ревизия в разделе анти-клика).
    // Прятать его нечем: в этот момент меняется репортуемая хосту латентность,
    // и хост сам пересобирает конвейер.
    {
        constexpr double sr = 48000.0;
        constexpr int blockSize = 512;
        constexpr int totalBlocks = 900;               // ~9,6 с

        MidiDelayProcessor proc;
        engineDefaults (proc);
        proc.setPlayConfigDetails (2, 2, sr, blockSize);
        proc.prepareToPlay (sr, blockSize);

        juce::AudioBuffer<float> block (2, blockSize);
        std::vector<float> steps;
        steps.reserve (static_cast<size_t> (totalBlocks) * blockSize);

        double phase = 0.0;
        float previous = 0.0f;
        int nextPreset = 1;

        for (int b = 0; b < totalBlocks; ++b)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const auto v = static_cast<float> (0.3 * std::sin (phase));
                phase += juce::MathConstants<double>::twoPi * 196.0 / sr;
                block.setSample (0, i, v);
                block.setSample (1, i, v);
            }

            juce::MidiBuffer midi;

            if (b % 30 == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60 + (b / 30) % 12, 0.9f), 0);

            // Пресет меняется чаще, чем нота успевает отзвучать: хвост в этот момент
            // заведомо звучит, иначе замер мерил бы тишину.
            if (b % 50 == 25)
            {
                proc.setCurrentProgram (nextPreset);
                nextPreset = 1 + (nextPreset % 5);     // 1..5, без Chord Pad
            }

            runBlock (proc, block, midi);
            CHECK (allocations.load() == 0);

            for (int i = 0; i < blockSize; ++i)
            {
                const float value = block.getSample (0, i);
                CHECK (std::isfinite (value));
                steps.push_back (std::abs (value - previous));
                previous = value;
            }
        }

        const auto tail = steps.begin() + static_cast<long> (steps.size() * 999 / 1000);
        std::nth_element (steps.begin(), tail, steps.end());

        const double percentile = *tail;
        const double worst = *std::max_element (steps.begin(), steps.end());
        const double ratio = worst / juce::jmax (1.0e-6, percentile);

        std::printf ("  смена пресета на хвосте: худший шаг %.4f, 99,9%% %.4f, отношение %.1f\n",
                     worst, percentile, ratio);

        // Порог тот же, что в стресс-тесте всех разрывов: там на исправном плагине
        // отношение держится около 5, и 8 оставляет запас, но ловит настоящий выброс.
        CHECK (ratio < 8.0);
    }

    // --- Снимок голосов для окна (#27) -----------------------------------------
    // Индикатор нот — диагностический прибор, а не украшение: пользователь по нему
    // решает «плагин не работает» или «MIDI не доехал». Значит проверяется ровно то,
    // за что он отвечает: занятые слоты названы своими нотами, хвост виден уровнем
    // огибающей, отпущенная нота из картинки уходит, а снимок ничего не выделяет.
    {
        MidiDelayProcessor proc;
        engineDefaults (proc);
        plainLoop (proc);
        setParam (proc, "timeMode", 0.0f);            // Free: голос стартует сразу
        setParam (proc, "release", 50.0f);            // короткий спад, чтобы не ждать
        setParam (proc, "voices", 8.0f);

        constexpr double sr = 48000.0;
        constexpr int blockSize = 512;
        proc.prepareToPlay (sr, blockSize);

        juce::AudioBuffer<float> block (2, blockSize);

        auto busyVoices = [&proc]
        {
            int busy = 0;

            for (int slot = 0; slot < VoiceManager::maxVoices; ++slot)
                if (proc.voiceNote[slot].load() >= 0)
                    ++busy;

            return busy;
        };

        auto slotOf = [&proc] (int note)
        {
            for (int slot = 0; slot < VoiceManager::maxVoices; ++slot)
                if (proc.voiceNote[slot].load() == note)
                    return slot;

            return -1;
        };

        // Свежий процессор — пустой пул: окно, открытое до первой ноты, обязано
        // показывать тишину, а не мусор из неинициализированной памяти.
        CHECK (busyVoices() == 0);

        fillDC (block, 0.5f);
        juce::MidiBuffer chord;
        for (const int note : { 60, 64, 67 })
            chord.addEvent (juce::MidiMessage::noteOn (1, note, 0.9f), 0);

        runBlock (proc, block, chord);
        CHECK (allocations.load() == 0);   // снимок живёт в processBlock: аллокаций там нет

        CHECK (busyVoices() == 3);
        for (const int note : { 60, 64, 67 })
        {
            const int slot = slotOf (note);
            CHECK (slot >= 0);
            CHECK (proc.voiceLevel[slot].load() > 0.0f);
        }

        CHECK (proc.midiNoteCount.load() == 3);
        CHECK (proc.lastNote.load() == 67);

        // Хвост: нота отпущена, слот ещё занят, но уровень падает. Без уровня
        // «три голоса заняты» после отпускания выглядело бы как залипание.
        juce::MidiBuffer release;
        for (const int note : { 60, 64, 67 })
            release.addEvent (juce::MidiMessage::noteOff (1, note), 0);

        const float beforeRelease = proc.voiceLevel[slotOf (60)].load();
        runBlock (proc, block, release);

        const int decaying = slotOf (60);
        CHECK (decaying >= 0);
        CHECK (proc.voiceLevel[decaying].load() < beforeRelease);

        // Спад 50 мс: за полсекунды голоса обязаны освободиться все до одного.
        for (int i = 0; i < 50; ++i)
        {
            fillDC (block, 0.5f);
            runBlock (proc, block);
        }

        CHECK (busyVoices() == 0);

        // Сброс обязан чистить картинку: иначе окно после смены sample rate
        // показывало бы ноты, которых уже нет.
        runBlock (proc, block, chord);
        CHECK (busyVoices() == 3);
        proc.releaseResources();
        CHECK (busyVoices() == 0);

        // Подписи в окне — это имена параметров, и они уходят на экран как есть.
        // ASCII, как и имена пресетов (CLAUDE.md): кириллица в окне рассыпается.
        for (auto* param : proc.getParameters())
            if (auto* named = dynamic_cast<juce::AudioProcessorParameterWithID*> (param))
            {
                CHECK (named->getName (64).isNotEmpty());

                for (auto c : named->getName (64))
                    CHECK (c >= 32 && c < 127);
            }

        std::printf ("  #27: три ноты -> три слота, хвост гаснет, сброс чистит картинку\n");
    }

    // --- Регрессионные рендеры DSP (#34) ---------------------------------------
    // Последними: они сверяют весь сигнал, и если сломано что-то из проверенного выше,
    // внятнее упасть на CHECK с названным свойством, чем на «разошлось с эталоном».
    if (regression (false, {}) != 0)
        return 1;

    std::printf ("test_processor: OK\n");
    return 0;
}
