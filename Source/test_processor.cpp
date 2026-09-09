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

/** Офлайн-рендер для проверки на слух: пила 220 Гц на вход, короткая мелодия в MIDI,
    результат в WAV. Не тест — тесты не умеют сказать «звучит убедительно». Это #19.
    Движок выбирается третьим аргументом, чтобы можно было сравнить их на одном
    и том же материале; по умолчанию — HQ, то есть то, что услышит пользователь.
    Рендер воспроизводим: зерно случайной фазы у HQ-движка фиксировано.
    Запуск: ProcessorTest --render out.wav [fast|hq] */
static int renderDemo (const juce::String& path, bool hq)
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 512;
    constexpr double seconds = 8.0;
    const int total = static_cast<int> (sr * seconds);

    MidiDelayProcessor proc;
    setParam (proc, "delayTime", 400.0f);
    setParam (proc, "feedback", 0.0f);
    setParam (proc, "mix", 60.0f);
    setParam (proc, "outputGain", 0.0f);
    setParam (proc, "bypass", 0.0f);
    setParam (proc, "attack", 10.0f);
    setParam (proc, "release", 300.0f);
    setParam (proc, "rootKey", 0.0f);       // C
    setParam (proc, "pitchRange", 12.0f);
    setParam (proc, "quality", hq ? 1.0f : 0.0f);

    proc.setPlayConfigDetails (2, 2, sr, blockSize);
    proc.prepareToPlay (sr, blockSize);

    // Пила 220 Гц: у неё много гармоник, и транспонирование на ней слышно сразу.
    // Ноты — C5, E5, G5, C6 по номерам FL Studio, то есть MIDI 60, 64, 67, 72:
    // унисон, большая терция, квинта, октава от Root Key.
    const int notes[] { 60, 64, 67, 72 };
    const int noteLength = static_cast<int> (sr * 1.5);

    juce::AudioBuffer<float> out (2, total);
    juce::AudioBuffer<float> block (2, blockSize);
    double phase = 0.0;

    for (int start = 0; start < total; start += blockSize)
    {
        const int n = juce::jmin (blockSize, total - start);

        for (int i = 0; i < n; ++i)
        {
            phase += 220.0 / sr;
            if (phase >= 1.0) phase -= 1.0;

            const auto value = static_cast<float> (0.25 * (2.0 * phase - 1.0));
            block.setSample (0, i, value);
            block.setSample (1, i, value);
        }

        juce::MidiBuffer midi;

        for (int k = 0; k < 4; ++k)
        {
            const int on  = static_cast<int> (sr * 0.5) + k * noteLength;
            const int off = on + noteLength - static_cast<int> (sr * 0.1);

            if (on  >= start && on  < start + n) midi.addEvent (juce::MidiMessage::noteOn  (1, notes[k], 0.9f), on  - start);
            if (off >= start && off < start + n) midi.addEvent (juce::MidiMessage::noteOff (1, notes[k]), off - start);
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

    std::printf ("rendered %s (%.1f s, %s)\n", file.getFullPathName().toRawUTF8(),
                 seconds, hq ? "hq" : "fast");
    return 0;
}

int main (int argc, char* argv[])
{
    countedThread = std::this_thread::get_id();

    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc >= 3 && juce::String (argv[1]) == "--render")
        return renderDemo (juce::String (argv[2]),
                           argc < 4 || juce::String (argv[3]) != "fast");

    // --- Раскладки шин ---------------------------------------------------------
    {
        MidiDelayProcessor proc;
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

    std::printf ("test_processor: OK\n");
    return 0;
}
