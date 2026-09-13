#pragma once
#include <memory>
#include "PitchShifter.h"

class DelayBuffer;

/** Одна звучащая MIDI-нота: своя позиция чтения, ratio, огибающая, пан, свой питчер.
    Голос ничего не знает про feedback — feedback берётся до питч-стадии и живёт
    в процессоре (ADR 0001). */
class Voice
{
public:
    /** Создаёт питчер и выделяет его память. Только не из аудиопотока. */
    void prepare (double sampleRate, int maxBlockSamples);
    void reset();

    /** Длины фронтов огибающей в сэмплах. Уже идущий спад доезжает своим шагом,
        смена времён его не дёргает. */
    void setEnvelope (double attackSamples, double releaseSamples);

    /** Латентность движка в сэмплах, ноль до prepare. Нужна процессору, чтобы
        ограничить delay time снизу (#17) и чтобы посчитать выравнивание в режиме
        Follow (ADR 0006): зашивать её числом нельзя, у движков она разная. */
    int getLatencySamples (PitchEngine engine) const;

    /** Позиция чтения из кольца. Латентность питчера вычитает сам голос: только он
        знает свой движок, процессору её взять неоткуда (#17). Отсюда же расхождение
        с исходным комментарием интерфейса — снаружи передаётся честный delay time. */
    void setDelaySamples (double delaySamples);

    /** Возраст присваивает менеджер: счётчик один на весь пул, голосу его не вывести. */
    void setAge (unsigned newAge);

    /** Какой движок должен звучать со следующей ноты: выбирает Quality (#38).
        Режим Time Mode на это не влияет — почему, в ADR 0006. Латчится
        в момент старта ноты, звучащие голоса переключение не трогает — почему
        именно так, написано в Voice.cpp у самого латча. */
    void setEngine (PitchEngine engine);

    /** Форманты на месте или едут вместе с высотой (#24). В отличие от движка
        не латчится на старте ноты: это флаг спектральной огибающей, менять его
        на живом звуке безопасно, и слышать разницу переключателя надо сразу. */
    void setFormantHold (bool shouldHold);

    void noteOn (int midiNote, float velocity, float ratio, double delaySamples, float pan);
    void noteOff();

    /** Кража голоса (#13): fade-out 5 мс, после него голос сам перезапустится
        с параметрами pending-ноты. Щелчка нет, отдельной очереди нет. */
    void steal (int midiNote, float velocity, float ratio, double delaySamples, float pan);

    /** Отложенный note off: педаль сустейна нажата, клавиша уже отпущена, голос звучит.
        Флаг живёт в голосе, а не в менеджере, потому что noteOn его сбрасывает сам —
        иначе украденный голос унаследовал бы чужую педаль. */
    void setSustained (bool shouldHold);
    bool isSustained() const;

    bool isActive() const;
    bool isReleasing() const;
    float getEnvelopeLevel() const;   // для выбора самого тихого при краже
    unsigned getAge() const;          // порядковый номер запуска, меньше — старше
    int getMidiNote() const;

    /** Push: голос подмешивает себя в out, а не отдаёт свой блок наружу (ADR 0001).
        scratchIn/scratchOut — общие на все голоса моно-буферы длиной maxBlockSamples,
        владелец VoiceManager. startSample/numSamples режут блок по MIDI-событиям.

        Кольцо к этому моменту уже записано на весь сегмент, поэтому смещение чтения
        отсчитывается от его конца: сэмплу k соответствует readOffset + (numSamples - k). */
    void addTo (float* const* out, int numOutChannels, int startSample, int numSamples,
                const DelayBuffer& source, float* scratchIn, float* scratchOut);

private:
    enum class Stage { idle, attack, sustain, release, stealing };

    float nextEnvelope();
    void start (int midiNote, float velocity, float ratio, float pan);

    // Оба движка живут всё время работы плагина и оба готовы: создать нужный
    // в момент переключения нельзя, это аллокация из аудиопотока. Цена — память
    // неиспользуемого движка, около 250 КБ на голос.
    std::unique_ptr<PitchShifter> fastShifter, hqShifter;
    PitchShifter* engineFor (PitchEngine engine) const;

    PitchShifter* shifter = nullptr;   // активный; чей именно, решает старт ноты
    PitchEngine wantEngine = PitchEngine::hq;   // то, что просят параметры прямо сейчас

    Stage stage = Stage::idle;
    float level = 0.0f;        // текущий уровень огибающей, он же выходной гейн
    float peak = 0.0f;         // куда едет атака: velocity через кривую
    float startLevel = 0.0f;   // уровень на входе в спад: с него спад и едет
    float phase = 0.0f;        // положение внутри фронта, 0..1; форму даёт кривая
    float step = 0.0f;         // шаг фазы за сэмпл, всегда положительный
    double attackSamples = 1.0, releaseSamples = 1.0, stealSamples = 1.0;

    double readOffset = 2.0;
    double delaySamples = 2.0;
    int note = -1;
    unsigned age = 0;
    bool sustained = false;

    // Питчер держит своё окно истории, и после reset оно пустое. Без заливки голос
    // молчал бы первые getLatencySamples() сэмплов — 30 мс дырки на каждой ноте.
    bool needsPrime = false;
    float gain[2] { 1.0f, 1.0f };

    // Нота, ждущая конца fade-out внутри этого же голоса. Очередь на один элемент.
    int pendingNote = -1;
    float pendingVelocity = 0.0f, pendingRatio = 1.0f, pendingPan = 0.0f;
};
