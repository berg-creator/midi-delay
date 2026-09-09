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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
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
    void setParam (MidiDelayProcessor& proc, const char* id, float value)
    {
        auto* p = proc.apvts.getParameter (id);
        CHECK (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        CHECK (std::abs (proc.apvts.getRawParameterValue (id)->load() - value) < 0.01f);
    }

    /** Выключает окраску петли: фильтры на краях диапазона, диффузия в нуле. Почти
        все замеры ниже меряют тайминг и уровни, и окраска в них только мешает; те
        тесты, что меряют её саму, ставят значения руками. Заодно замеры перестают
        зависеть от того, какие значения выбраны значениями по умолчанию. */
    void plainLoop (MidiDelayProcessor& proc)
    {
        setParam (proc, "diffusion", 0.0f);
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
    По умолчанию диффузия и фильтры петли выключены — рендер судит питчер, а не
    обвязку. Пятый аргумент colour включает окраску петли (#45, #22) и обратную
    связь: это A/B к тому же файлу без окраски, одной командой.
    Запуск: ProcessorTest --render out.wav [fast|hq|follow] [input.wav] [colour] */
static int renderDemo (const juce::String& path, const juce::String& mode,
                       const juce::String& inputPath, bool colour)
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

        juce::File source (inputPath);
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));

        if (reader == nullptr)
        {
            std::printf ("cannot read %s\n", source.getFullPathName().toRawUTF8());
            return 1;
        }

        sr = reader->sampleRate;
        const int frames = static_cast<int> (juce::jmin (reader->lengthInSamples,
                                                         static_cast<juce::int64> (sr * maxSeconds)));
        input.setSize (2, frames);
        reader->read (&input, 0, frames, 0, true, true);

        if (reader->numChannels < 2)
            input.copyFrom (1, 0, input, 0, 0, frames);
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

    const int total = input.getNumSamples();

    MidiDelayProcessor proc;
    setParam (proc, "delayTime", 400.0f);
    setParam (proc, "feedback", 0.0f);
    setParam (proc, "mix", 100.0f);
    setParam (proc, "outputGain", 0.0f);
    setParam (proc, "bypass", 0.0f);
    setParam (proc, "attack", 10.0f);
    setParam (proc, "release", 300.0f);
    setParam (proc, "rootKey", 0.0f);       // C
    setParam (proc, "pitchRange", 12.0f);
    setParam (proc, "quality", hq ? 1.0f : 0.0f);
    setParam (proc, "timeMode", follow ? 1.0f : 0.0f);
    setParam (proc, "diffusion", colour ? 60.0f : 0.0f);
    setParam (proc, "filterLo", colour ? 100.0f : 20.0f);
    setParam (proc, "filterHi", colour ? 12000.0f : 20000.0f);

    if (colour)
        setParam (proc, "feedback", 45.0f);   // без повторов диффузию не услышать

    proc.setPlayConfigDetails (2, 2, sr, blockSize);
    proc.prepareToPlay (sr, blockSize);

    // Мелодия крутится по кругу до конца материала: иначе на длинном файле хвост
    // молчал бы там, где как раз и интересно слушать согласные. Номера — MIDI 60,
    // 64, 67, 72, в терминах FL Studio C5, E5, G5, C6: унисон, терция, квинта, октава
    // от Root Key. Унисон в списке не случайно — на нём слышно прозрачность движка.
    const int notes[] { 60, 64, 67, 72 };
    const int noteLength = static_cast<int> (sr * 1.0);
    const int gap = static_cast<int> (sr * 0.05);
    const int firstNote = static_cast<int> (sr * 0.5);

    juce::AudioBuffer<float> out (2, total);
    juce::AudioBuffer<float> block (2, blockSize);

    for (int start = 0; start < total; start += blockSize)
    {
        const int n = juce::jmin (blockSize, total - start);

        for (int ch = 0; ch < 2; ++ch)
            block.copyFrom (ch, 0, input, ch, start, n);

        juce::MidiBuffer midi;

        for (int k = 0; firstNote + k * noteLength < total; ++k)
        {
            const int on  = firstNote + k * noteLength;
            const int off = on + noteLength - gap;
            const int note = notes[k % 4];

            if (on  >= start && on  < start + n) midi.addEvent (juce::MidiMessage::noteOn  (1, note, 0.9f), on  - start);
            if (off >= start && off < start + n) midi.addEvent (juce::MidiMessage::noteOff (1, note), off - start);
        }

        juce::AudioBuffer<float> view (block.getArrayOfWritePointers(), 2, n);
        proc.processBlock (view, midi);

        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, start, block, ch, 0, n);
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

    std::printf ("rendered %s (%.1f s, %s%s%s)\n", file.getFullPathName().toRawUTF8(),
                 total / sr, hq ? "hq" : "fast", follow ? ", follow" : "",
                 colour ? ", colour" : "");
    return 0;
}

int main (int argc, char* argv[])
{
    countedThread = std::this_thread::get_id();

    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc >= 3 && juce::String (argv[1]) == "--render")
        return renderDemo (juce::String (argv[2]),
                           argc >= 4 ? juce::String (argv[3]) : juce::String ("hq"),
                           argc >= 5 ? juce::String (argv[4]) : juce::String(),
                           argc >= 6 && juce::String (argv[5]) == "colour");

    // --- Раскладки шин ---------------------------------------------------------
    {
        MidiDelayProcessor proc;
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

    // --- Состояние -------------------------------------------------------------
    {
        MidiDelayProcessor proc;
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
            setParam (proc, "quality", 0.0f);
            setParam (proc, "timeMode", follow ? 1.0f : 0.0f);
            setParam (proc, "delayTime", delayMs);
            setParam (proc, "feedback", feedbackPercent);
            setParam (proc, "mix", 100.0f);
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "bypass", 0.0f);
            setParam (proc, "attack", 1.0f);
            setParam (proc, "diffusion", diffusion);
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
                setParam (proc, "quality", 0.0f);
                setParam (proc, "delayTime", delayMs);
                setParam (proc, "feedback", feedbackPercent);
                setParam (proc, "mix", 100.0f);
                setParam (proc, "outputGain", 0.0f);
                setParam (proc, "bypass", 0.0f);
                setParam (proc, "attack", 1.0f);
                setParam (proc, "diffusion", 0.0f);
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
                setParam (proc, "quality", 0.0f);
                setParam (proc, "delayTime", delayMs);
                setParam (proc, "feedback", 0.0f);
                setParam (proc, "mix", 100.0f);
                setParam (proc, "outputGain", 0.0f);
                setParam (proc, "bypass", 0.0f);
                setParam (proc, "attack", 1.0f);
                setParam (proc, "diffusion", 0.0f);
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

        // Диффузия и фильтры вместе при feedback 95 % не разгоняют петлю. Проверять
        // надо именно вместе: по отдельности усиление меняется слабее.
        {
            MidiDelayProcessor proc;
            setParam (proc, "quality", 0.0f);
            setParam (proc, "delayTime", delayMs);
            setParam (proc, "feedback", 95.0f);
            setParam (proc, "mix", 100.0f);
            setParam (proc, "outputGain", 0.0f);
            setParam (proc, "bypass", 0.0f);
            setParam (proc, "attack", 1.0f);
            setParam (proc, "diffusion", 100.0f);
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

    std::printf ("test_processor: OK\n");
    return 0;
}
