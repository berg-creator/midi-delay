#include "PluginEditor.h"

namespace
{
    /** Порядок органов в окне и он же порядок чтения слева направо. Список явный,
        а не «все параметры подряд»: показывать орган, ни к чему не подключённый, —
        врать. Sync и Note Division появились здесь в сессии 14 вместе с #20;
        до этого division висел в APVTS мёртвым и потому в окно не выводился.

        Строки сгруппированы по смыслу: время, характер хвоста, выход, высота,
        огибающая. Это ещё не интерфейс (#26, #27), но покрутить уже можно осмысленно. */
    const char* const layout[] {
        "timeMode",  "delayTime",  "sync",       "division",
        "midiOffset","feedback",   "diffusion",  "modulation",
        "filterLo",  "filterHi",   "width",      "pingPong",
        "ducking",   "mix",        "outputGain", "quality",
        "formants",  "rootKey",    "pitchRange", "voices",
        "attack",    "release",    "bypass",
    };

    constexpr int headerHeight = 132;
    constexpr int cellWidth = 152;
    constexpr int cellHeight = 96;
    constexpr int columns = 4;
    constexpr int margin = 12;
}

MidiDelayEditor::MidiDelayEditor (MidiDelayProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    for (const auto* id : layout)
        addControl (id);

    const int rows = (static_cast<int> (std::size (layout)) + columns - 1) / columns;

    setSize (columns * cellWidth + 2 * margin,
             headerHeight + rows * cellHeight + margin);

    startTimerHz (15);
}

void MidiDelayEditor::addControl (const juce::String& parameterId)
{
    auto* param = proc.apvts.getParameter (parameterId);

    if (param == nullptr)
        return;   // параметр переименовали — лучше дырка в сетке, чем падение

    auto* caption = captions.add (new juce::Label ({}, param->getName (24)));
    caption->setJustificationType (juce::Justification::centred);
    caption->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.75f));
    caption->setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (caption);

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param))
    {
        auto* box = new juce::ComboBox();
        box->addItemList (choice->choices, 1);
        controls.add (box);
        addAndMakeVisible (box);
        comboLinks.add (new juce::AudioProcessorValueTreeState::ComboBoxAttachment (
            proc.apvts, parameterId, *box));
        return;
    }

    if (dynamic_cast<juce::AudioParameterBool*> (param) != nullptr)
    {
        auto* toggle = new juce::ToggleButton ("on");
        controls.add (toggle);
        addAndMakeVisible (toggle);
        buttonLinks.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (
            proc.apvts, parameterId, *toggle));
        return;
    }

    // Ручка с числом под ней: величины тут читаются глазами (миллисекунды, проценты,
    // децибелы), и без числа их не выставить, а только подвигать.
    auto* slider = new juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag,
                                     juce::Slider::TextBoxBelow);
    slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 68, 16);
    controls.add (slider);
    addAndMakeVisible (slider);
    sliderLinks.add (new juce::AudioProcessorValueTreeState::SliderAttachment (
        proc.apvts, parameterId, *slider));
}

void MidiDelayEditor::resized()
{
    for (int i = 0; i < controls.size(); ++i)
    {
        const int column = i % columns;
        const int row = i / columns;

        juce::Rectangle<int> cell (margin + column * cellWidth,
                                   headerHeight + row * cellHeight,
                                   cellWidth, cellHeight);

        captions[i]->setBounds (cell.removeFromTop (16));

        // Список и галка занимают одну строку по центру ячейки, ручка — всю ячейку.
        if (dynamic_cast<juce::Slider*> (controls[i]) != nullptr)
            controls[i]->setBounds (cell.reduced (6, 2));
        else
            controls[i]->setBounds (cell.withSizeKeepingCentre (cellWidth - 28, 24));
    }
}

void MidiDelayEditor::timerCallback()
{
    const auto count = proc.midiNoteCount.load (std::memory_order_relaxed);
    const auto note  = proc.lastNote.load (std::memory_order_relaxed);
    const auto quality = static_cast<int> (proc.apvts.getRawParameterValue ("quality")
                                               ->load (std::memory_order_relaxed));
    // Сравнивается заказанное время, а не ручка: на Sync ручка стоит на месте,
    // а под предел движка уезжает именно нотная длительность (#20 против #17).
    const auto clamped = proc.requestedDelayMs() < proc.getMinDelayMs();
    const auto follow = proc.isFollowMode();
    const auto alignment = juce::roundToInt (proc.getAlignmentMs());
    const auto sync = proc.isSyncMode();
    const auto division = static_cast<int> (proc.apvts.getRawParameterValue ("division")
                                                ->load (std::memory_order_relaxed));
    const auto bpm = juce::roundToInt (proc.getSyncBpm());

    if (count != lastCount || note != shownNote || quality != shownQuality
        || clamped != shownClamped || follow != shownFollow || alignment != shownAlignment
        || sync != shownSync || division != shownDivision || bpm != shownBpm)
    {
        lastCount = count;
        shownNote = note;
        shownQuality = quality;
        shownClamped = clamped;
        shownFollow = follow;
        shownAlignment = alignment;
        shownSync = sync;
        shownDivision = division;
        shownBpm = bpm;
        repaint();
    }
}

void MidiDelayEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1a1e));

    auto header = getLocalBounds().removeFromTop (headerHeight).reduced (margin, 8);

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (20.0f));
    g.drawText ("MIDI Delay", header.removeFromTop (26),
                juce::Justification::centredLeft, false);

    // Текст интерфейса — только ASCII: juce::String трактует обычный литерал
    // как ASCII, и любая кириллица превращается в мусор. См. CLAUDE.md.
    const auto count = juce::jmax (0, lastCount);

    g.setColour (count > 0 ? juce::Colours::limegreen : juce::Colours::grey);
    g.setFont (juce::FontOptions (13.0f));

    auto midiLine = header.removeFromTop (20);

    if (count > 0)
    {
        const auto shown = juce::jmax (-1, shownNote);
        const auto ratio = proc.lastRatio.load (std::memory_order_relaxed);
        auto text = "MIDI OK - notes received: " + juce::String (count);

        if (shown >= 0)
            // Октава по-фловски: в FL Studio средняя до — C5, а не C3, и имя из другой
            // конвенции сбивало бы с толку сильнее, чем помогало.
            text += "   last " + juce::MidiMessage::getMidiNoteName (shown, true, true, 5)
                  + "   ratio " + juce::String (ratio, 3);

        g.drawText (text, midiLine, juce::Justification::centredLeft, false);

    }
    else
    {
        g.drawText ("No MIDI input - set the same port in MIDI Out and in the wrapper",
                    midiLine, juce::Justification::centredLeft, false);
    }

    // Где унисон. Без этой строки узнать неоткуда: Root Key показывает класс высоты
    // без октавы, и на живом прогоне в FL мелодия была нарисована октавой выше —
    // хвост пел на октаву вверх, а слышалось это как расслоение голоса.
    g.setColour (juce::Colours::grey);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("Unison (ratio 1.000) at "
                    + juce::MidiMessage::getMidiNoteName (proc.getUnisonNote(), true, true, 5)
                    + " - notes above it transpose the tail up",
                header.removeFromTop (17), juce::Justification::centredLeft, false);

    // В Follow движок свой и предела на время нет: там показывается то, что важно
    // именно в этом режиме, — сколько плагин просит скомпенсировать у хоста.
    // Если хост этого не делает, хвост уедет от сухого ровно на это число.
    // Сетка. Без этой строки нотная длительность — это число, которое не с чем сверить:
    // ни темпа не видно, ни того, во что он превратился в миллисекундах. Отдельная
    // строка, а не хвост к строке движка, потому что предел движка (#17) на быстрых
    // темпах и мелких делителях подтягивает время вверх, и оба числа надо видеть сразу.
    g.setColour (juce::Colours::grey);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (shownSync
                    ? "Sync: " + proc.apvts.getParameter ("division")->getCurrentValueAsText()
                        + " at " + juce::String (proc.getSyncBpm(), 1) + " BPM = "
                        + juce::String (proc.requestedDelayMs(), 1) + " ms"
                    : "Sync: off - delay time set in ms",
                header.removeFromTop (17), juce::Justification::centredLeft, false);

    const auto minDelay = proc.getMinDelayMs();
    auto engineText = juce::String (shownFollow ? "Mode: Follow - tail sits on the note"
                                    : shownQuality > 0 ? "Mode: Free - HQ (Signalsmith)"
                                                       : "Mode: Free - Fast (varispeed)");

    if (shownFollow)
        engineText += " - host must compensate " + juce::String (shownAlignment) + " ms";
    else if (minDelay > 0.0)
        engineText += (shownClamped ? " - delay time raised to min "
                                    : " - min delay ") + juce::String (minDelay, 0) + " ms";

    if (! shownFollow && shownAlignment > 0)
        engineText += " - offset costs " + juce::String (shownAlignment) + " ms to host";

    g.setColour (shownClamped && ! shownFollow ? juce::Colours::orange : juce::Colours::grey);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (engineText, header.removeFromTop (18), juce::Justification::centredLeft, false);
}
