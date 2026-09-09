#include "PluginEditor.h"

MidiDelayEditor::MidiDelayEditor (MidiDelayProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setSize (420, 180);
    startTimerHz (15);
}

void MidiDelayEditor::timerCallback()
{
    const auto count = proc.midiNoteCount.load (std::memory_order_relaxed);
    const auto note  = proc.lastNote.load (std::memory_order_relaxed);
    const auto quality = static_cast<int> (proc.apvts.getRawParameterValue ("quality")
                                               ->load (std::memory_order_relaxed));
    const auto clamped = proc.apvts.getRawParameterValue ("delayTime")
                             ->load (std::memory_order_relaxed) < proc.getMinDelayMs();
    const auto follow = proc.isFollowMode();
    const auto alignment = juce::roundToInt (proc.getAlignmentMs());

    if (count != lastCount || note != shownNote || quality != shownQuality
        || clamped != shownClamped || follow != shownFollow || alignment != shownAlignment)
    {
        lastCount = count;
        shownNote = note;
        shownQuality = quality;
        shownClamped = clamped;
        shownFollow = follow;
        shownAlignment = alignment;
        repaint();
    }
}

void MidiDelayEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1a1e));

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (22.0f));
    g.drawText ("MIDI Delay", getLocalBounds().removeFromTop (90),
                juce::Justification::centredBottom, false);

    // Текст интерфейса — только ASCII: juce::String трактует обычный литерал
    // как ASCII, и любая кириллица превращается в мусор. См. CLAUDE.md.
    const auto count = juce::jmax (0, lastCount);
    auto area = getLocalBounds().withTrimmedTop (95);

    // Движок питчинга и его минимальный delay time (ADR 0005). Своего переключателя
    // тут нет — параметр крутится из панели плагина в хосте, а показ нужен затем,
    // чтобы не гадать, какой движок звучит. Настоящий интерфейс — веха M4.
    //
    // Предел спрашивается у процессора, а не написан здесь числом (#17): он равен
    // латентности движка и меняется вместе с его окном. Когда выставленное время
    // ниже предела, строка про это и говорит — иначе хвост молча приходил бы позже
    // заказанного, и понять причину было бы неоткуда.
    // В Follow движок свой и предела на время нет: там показывается то, что важно
    // именно в этом режиме, — сколько плагин просит скомпенсировать у хоста.
    // Если хост этого не делает, хвост уедет от сухого ровно на это число.
    const auto minDelay = proc.getMinDelayMs();
    auto engineText = juce::String (shownFollow ? "Mode: Follow - tail sits on the note"
                                    : shownQuality > 0 ? "Engine: HQ (Signalsmith)"
                                                       : "Engine: Fast (varispeed)");

    if (shownFollow)
        engineText += " - host must compensate " + juce::String (shownAlignment) + " ms";
    else if (minDelay > 0.0)
        engineText += (shownClamped ? " - delay time raised to min "
                                    : " - min delay ") + juce::String (minDelay, 0) + " ms";

    if (! shownFollow && shownAlignment > 0)
        engineText += " - offset " + juce::String (shownAlignment) + " ms to host";

    g.setColour (shownClamped && ! shownFollow ? juce::Colours::orange : juce::Colours::grey);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (engineText, getLocalBounds().removeFromBottom (26),
                juce::Justification::centredTop, false);

    g.setColour (count > 0 ? juce::Colours::limegreen : juce::Colours::grey);
    g.setFont (juce::FontOptions (15.0f));
    g.drawText (count > 0 ? "MIDI OK - notes received: " + juce::String (count)
                          : "No MIDI input",
                area.removeFromTop (24), juce::Justification::centredTop, false);

    if (count == 0)
    {
        g.setColour (juce::Colours::grey.withAlpha (0.7f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Set the same MIDI port in MIDI Out and in the wrapper settings",
                    area.removeFromTop (20), juce::Justification::centredTop, false);
        return;
    }

    // Последняя нота и её ratio: видно, что транспонирование посчиталось и каким.
    // Ratio 1.00 на ноте, равной Root Key, — это норма, а не отсутствие эффекта.
    const auto note = juce::jmax (-1, shownNote);

    if (note >= 0)
    {
        const auto ratio = proc.lastRatio.load (std::memory_order_relaxed);
        const auto semitones = juce::roundToInt (12.0f * std::log2 (ratio));

        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Last note " + juce::String (note)
                        // Октава по-фловски: в FL Studio средняя до — C5, а не C3,
                        // и имя из другой конвенции сбивало бы с толку сильнее, чем помогало.
                        + " (" + juce::MidiMessage::getMidiNoteName (note, true, true, 5) + ")"
                        + "   ratio " + juce::String (ratio, 3)
                        + "   " + (semitones >= 0 ? "+" : "") + juce::String (semitones) + " st",
                    area.removeFromTop (20), juce::Justification::centredTop, false);
    }
}
