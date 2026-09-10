#pragma once
#include <array>
#include <vector>
#include "Voice.h"

class DelayBuffer;

/** Пул голосов: раздача, кража, рендер сегмента. Память только в prepare. Задача #13. */
class VoiceManager
{
public:
    /** ponytail: 8 хватает фоновому слою. Поднимать до 16 после профилирования (#31),
        а не заранее — стоимость линейна по голосам. */
    static constexpr int maxVoices = 8;

    /** numOutChannels убран из сигнатуры: раскладку голос узнаёт в addTo, дублировать
        её ещё и здесь значило бы держать два источника правды об одном и том же. */
    void prepare (double sampleRate, int maxBlockSamples);
    void reset();

    /** Время дилея для голосов, которые ещё не начались. Звучащих не трогает нарочно —
        голос это отвод кольца, и сдвинуть его посреди ноты значит склеить два куска
        звука встык. Замерено: смена делителя 1/8 -> 1/2 на звучащей ноте давала разрыв
        0,84 при пороге 0,05, и вылезал он через латентность питчера после самой смены.
        Новое время подхватывает следующая нота (#20, #25). Латентность питчера
        вычитает голос. */
    void setDelaySamples (double delaySamples);
    void setEnvelope (float attackMs, float releaseMs);
    void setVoiceLimit (int numVoices);           // параметр Voices, не больше maxVoices
    void setEngine (PitchEngine engine);          // Quality и режим Follow вместе
    void setFormantHold (bool shouldHold);        // параметр Formants (#24)

    /** Раскидка голосов по стерео (#23): 0 — все в центре, 100 % — умеренно,
        200 % — крайние голоса в упор влево и вправо. Параметр Width. */
    void setWidth (float widthPercent);

    /** Ping-pong (#23): ноты уходят попеременно влево и вправо, вместо раскидки
        по номеру слота. Классический ping-pong — чередование повторов обратной связи —
        в этой архитектуре не слышен вовсе: наружу хвост выходит только через голоса,
        а голос читает моно-сумму кольца (ADR 0001) и усреднил бы чередование обратно.
        Чередовать ноты для MIDI-ведомого дилея и осмысленнее: мелодия монофонная,
        и разлёт попадает в её ритм, а не в интервал повторов. */
    void setPingPong (bool shouldPingPong);

    /** Латентность движка в сэмплах, она же нижний предел delay time (#17) и
        выравнивание режима Follow (ADR 0006). Движок передаётся явно, а не берётся
        свой: процессору нужен предел того движка, который выбран параметрами прямо
        сейчас, а не того, на котором доигрывают голоса. */
    int getLatencySamples (PitchEngine engine) const;

    /** Пан не передаётся: его определяет номер слота, а слот выбирает пул (#23). */
    void noteOn (int midiNote, float velocity, float ratio);
    void noteOff (int midiNote);
    void allNotesOff();

    /** Педаль сустейна (CC 64): отпущенные клавиши держатся до её подъёма. */
    void setSustain (bool down);

    /** Рендер одного сегмента между MIDI-событиями: каждый живой голос подмешивается в out. */
    void process (float* const* out, int numOutChannels, int startSample, int numSamples,
                  const DelayBuffer& source);

private:
    /** Индекс свободного голоса, иначе кража: самый старый в release, иначе самый
        тихий. Индекс, а не ссылка: от номера слота считается пан (#23). */
    int findVoiceFor (int midiNote);

    /** Пан голоса по номеру слота: нулевой в центре, дальше через одного вправо
        и влево. Порядок именно такой, потому что одиночная нота почти всегда
        попадает в нулевой слот — и обязана остаться по центру. */
    float panForSlot (int slot) const;

    std::array<Voice, maxVoices> voices;
    std::vector<float> scratch;   // два моно-буфера подряд, общие на все голоса
    unsigned nextAge = 0;
    int blockSize = 0;
    int voiceLimit = maxVoices;
    float spread = 0.5f;          // 0..1, из параметра Width
    bool pingPong = false;
    bool pingPongRight = false;   // сторона следующей ноты
    PitchEngine engine = PitchEngine::hq;
    bool formantHold = true;
    double sampleRate = 44100.0;
    double delaySamples = 2.0;
    bool sustainDown = false;
};
