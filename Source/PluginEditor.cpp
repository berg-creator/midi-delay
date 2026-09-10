#include "PluginEditor.h"

namespace
{
    // Палитра. Один акцентный цвет на всё окно: второй акцент сразу порождает вопрос
    // «а что он значит», и ответа на него нет. Акцентом обозначено ровно то, что
    // сейчас живо: значение ручки, включённая галка, звучащая нота.
    const juce::Colour bgColour     { 0xff141418 };
    const juce::Colour panelColour  { 0xff1b1b21 };
    const juce::Colour cellColour   { 0xff26262e };
    const juce::Colour accentColour { 0xff56c4b0 };
    const juce::Colour textColour   { 0xffe6e6ec };
    const juce::Colour dimColour    { 0xff86868f };
    const juce::Colour warnColour   { 0xffe0a03c };

    constexpr int margin = 16;
    constexpr int columns = 5;
    constexpr int cellWidth = 128;
    constexpr int cellHeight = 88;
    constexpr int bandHeader = 26;
    constexpr int headerHeight = 94;
    constexpr int laneHeight = 100;
    constexpr int laneGap = 12;

    /** Порядок органов — сигнальный путь, а не список параметров: сначала то, что
        задаёт время и характер петли, потом высота хвоста, потом его огибающая,
        потом выход. Группа меняется — начинается новая полоса.

        Показывается только то, что подключено: орган, ни на что не влияющий,
        врёт сильнее, чем его отсутствие. */
    struct Slot { const char* group; const char* id; };

    const Slot layout[] {
        { "Delay",    "timeMode"   },
        { "Delay",    "sync"       },
        { "Delay",    "division"   },
        { "Delay",    "delayTime"  },
        { "Delay",    "midiOffset" },
        { "Delay",    "feedback"   },
        { "Delay",    "filterLo"   },
        { "Delay",    "filterHi"   },
        { "Delay",    "diffusion"  },
        { "Delay",    "modulation" },
        { "Pitch",    "rootKey"    },
        { "Pitch",    "pitchRange" },
        { "Pitch",    "voices"     },
        { "Pitch",    "quality"    },
        { "Pitch",    "formants"   },
        { "Envelope", "attack"     },
        { "Envelope", "release"    },
        { "Envelope", "ducking"    },
        { "Mix",      "width"      },
        { "Mix",      "pingPong"   },
        { "Mix",      "mix"        },
        { "Mix",      "outputGain" },
        { "Mix",      "bypass"     },
    };

    /** Сколько строк сетки займёт вся раскладка вместе с заголовками групп.
        Считается тем же обходом, что и расстановка в resized, — иначе окно
        и его содержимое разъезжаются при первой же правке таблицы. */
    int layoutHeight()
    {
        int height = 0;
        int inGroup = 0;
        const char* group = nullptr;

        auto closeGroup = [&]
        {
            if (group != nullptr)
                height += bandHeader + ((inGroup + columns - 1) / columns) * cellHeight;
        };

        for (const auto& slot : layout)
        {
            if (group == nullptr || juce::String (slot.group) != group)
            {
                closeGroup();
                group = slot.group;
                inGroup = 0;
            }

            ++inGroup;
        }

        closeGroup();
        return height;
    }

    bool isBlackKey (int note) noexcept
    {
        switch (note % 12)
        {
            case 1: case 3: case 6: case 8: case 10: return true;
            default: return false;
        }
    }

    /** Октава по-фловски: в FL Studio средняя до — C5, а не C3, и имя из другой
        конвенции сбивало бы с толку сильнее, чем помогало. */
    juce::String noteName (int note)
    {
        return juce::MidiMessage::getMidiNoteName (note, true, true, 5);
    }
}

//==============================================================================
BergLookAndFeel::BergLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, bgColour);

    setColour (juce::Label::textColourId, textColour);

    setColour (juce::Slider::textBoxTextColourId, textColour);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, accentColour.withAlpha (0.30f));

    setColour (juce::ComboBox::backgroundColourId, cellColour);
    setColour (juce::ComboBox::textColourId, textColour);
    setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ComboBox::arrowColourId, dimColour);

    setColour (juce::PopupMenu::backgroundColourId, panelColour);
    setColour (juce::PopupMenu::textColourId, textColour);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accentColour.withAlpha (0.35f));
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);

    setColour (juce::TextEditor::backgroundColourId, cellColour);
    setColour (juce::TextEditor::textColourId, textColour);
    setColour (juce::TextEditor::highlightColourId, accentColour.withAlpha (0.30f));
    setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TextEditor::focusedOutlineColourId, accentColour);
    setColour (juce::CaretComponent::caretColourId, accentColour);
}

void BergLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                        float sliderPos, float startAngle, float endAngle,
                                        juce::Slider& slider)
{
    // Дуга, а не «шайба с рисочкой»: положение ручки читается с расстояния,
    // а именно с расстояния на неё и смотрят, когда сравнивают два соседних значения.
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto angle = startAngle + sliderPos * (endAngle - startAngle);
    const auto thickness = juce::jmax (2.5f, radius * 0.18f);
    const auto arcRadius = radius - thickness * 0.5f;

    const auto live = slider.isEnabled();
    const auto value = live ? accentColour : dimColour.withAlpha (0.35f);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                         startAngle, endAngle, true);
    g.setColour (cellColour);
    g.strokePath (track, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // Дуга нулевой длины ломает addCentredArc, поэтому ручка на самом минимуме
    // рисуется одним только указателем.
    if (sliderPos > 0.001f)
    {
        juce::Path filled;
        filled.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                              startAngle, angle, true);
        g.setColour (value);
        g.strokePath (filled, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    }

    const auto knobRadius = arcRadius - thickness * 0.5f - 3.0f;

    g.setColour (panelColour);
    g.fillEllipse (juce::Rectangle<float> (knobRadius * 2.0f, knobRadius * 2.0f)
                       .withCentre (centre));

    juce::Path pointer;
    pointer.startNewSubPath (0.0f, -knobRadius + 3.0f);
    pointer.lineTo (0.0f, -knobRadius * 0.35f);
    pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre));

    g.setColour (live ? textColour : dimColour.withAlpha (0.45f));
    g.strokePath (pointer, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
}

void BergLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                        bool isHighlighted, bool)
{
    // Тумблер, а не квадратик с галкой: состояние читается формой и цветом сразу,
    // без чтения подписи рядом.
    auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
    const auto height = juce::jmin (bounds.getHeight(), 20.0f);
    auto track = bounds.withSizeKeepingCentre (height * 2.0f, height);

    const auto on = button.getToggleState();
    const auto live = button.isEnabled();

    g.setColour (on ? (live ? accentColour : dimColour.withAlpha (0.4f)) : cellColour);
    g.fillRoundedRectangle (track, height * 0.5f);

    if (isHighlighted && live)
    {
        g.setColour (textColour.withAlpha (0.12f));
        g.fillRoundedRectangle (track, height * 0.5f);
    }

    const auto knob = track.reduced (2.5f).withWidth (height - 5.0f)
                           .translated (on ? height : 0.0f, 0.0f);

    g.setColour (on ? panelColour : dimColour);
    g.fillEllipse (knob);
}

void BergLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                    int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);
    const auto live = box.isEnabled();

    g.setColour (cellColour.withAlpha (live ? 1.0f : 0.4f));
    g.fillRoundedRectangle (bounds, 4.0f);

    if (box.hasKeyboardFocus (false) && live)
    {
        g.setColour (accentColour.withAlpha (0.6f));
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
    }

    juce::Path arrow;
    const auto cx = static_cast<float> (width) - 13.0f;
    const auto cy = static_cast<float> (height) * 0.5f;
    arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
    arrow.lineTo (cx, cy + 2.5f);
    arrow.lineTo (cx + 4.0f, cy - 2.0f);

    g.setColour (live ? dimColour : dimColour.withAlpha (0.4f));
    g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
}

//==============================================================================
NoteLane::NoteLane (MidiDelayProcessor& p) : proc (p)
{
    setInterceptsMouseClicks (false, false);
    startTimerHz (30);
}

void NoteLane::timerCallback()
{
    const auto count = proc.midiNoteCount.load (std::memory_order_relaxed);
    const auto unison = proc.getUnisonNote();
    const auto limit = static_cast<int> (proc.apvts.getRawParameterValue ("voices")
                                             ->load (std::memory_order_relaxed));

    if (count != lastSeenCount)
    {
        lastSeenCount = count;
        blinkFrames = 6;             // ~200 мс: короче не замечаешь, длиннее сливается
    }
    else if (blinkFrames > 0)
    {
        --blinkFrames;
    }

    bool changed = count != shownCount || unison != shownUnison || limit != shownLimit
                || blinkFrames > 0;

    for (int slot = 0; slot < VoiceManager::maxVoices; ++slot)
    {
        const auto note = proc.voiceNote[slot].load (std::memory_order_relaxed);
        const auto level = proc.voiceLevel[slot].load (std::memory_order_relaxed);

        // Порог, а не точное сравнение: огибающая в сустейне дрожит в шестом знаке,
        // и без него окно перерисовывалось бы тридцать раз в секунду вечно.
        if (note != shownNote[slot] || std::abs (level - shownLevel[slot]) > 0.004f)
        {
            shownNote[slot] = note;
            shownLevel[slot] = level;
            changed = true;
        }
    }

    shownCount = count;
    shownUnison = unison;
    shownLimit = limit;

    if (changed)
        repaint();
}

void NoteLane::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();

    g.setColour (panelColour);
    g.fillRoundedRectangle (area, 6.0f);

    auto inner = area.reduced (10.0f, 8.0f);
    auto strip = inner.removeFromTop (18.0f);
    inner.removeFromTop (6.0f);

    // --- Голоса справа: восемь ячеек, залитых по уровню огибающей ------------
    int busy = 0;
    for (const auto& note : shownNote)
        if (note >= 0)
            ++busy;

    auto pips = strip.removeFromRight (VoiceManager::maxVoices * 9.0f);
    strip.removeFromRight (6.0f);

    for (int slot = 0; slot < VoiceManager::maxVoices; ++slot)
    {
        juce::Rectangle<float> pip (pips.getX() + static_cast<float> (slot) * 9.0f, pips.getY() + 3.0f, 6.0f, 12.0f);
        const auto beyondLimit = slot >= juce::jmax (1, shownLimit);

        g.setColour (cellColour.withAlpha (beyondLimit ? 0.4f : 1.0f));
        g.fillRect (pip);

        if (shownNote[slot] >= 0)
        {
            const auto level = juce::jlimit (0.0f, 1.0f, shownLevel[slot]);
            g.setColour (accentColour);
            g.fillRect (pip.withTrimmedTop (pip.getHeight() * (1.0f - level)));
        }
    }

    g.setColour (dimColour);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (juce::String (busy) + "/" + juce::String (juce::jmax (1, shownLimit)),
                strip.removeFromRight (34.0f).toNearestInt(),
                juce::Justification::centredRight, false);

    // --- Индикатор входа слева -----------------------------------------------
    // Отдельная лампа, а не только текст: «MIDI приходит» и «MIDI приходил когда-то»
    // одним счётчиком не различаются, а различить их и есть весь смысл #27.
    auto led = strip.removeFromLeft (16.0f);
    const auto alive = shownCount > 0;

    g.setColour (blinkFrames > 0 ? accentColour
                                 : (alive ? accentColour.withAlpha (0.35f)
                                          : dimColour.withAlpha (0.35f)));
    g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (led.getCentre()));

    // Текст интерфейса — только ASCII: juce::String трактует обычный литерал
    // как ASCII, и любая кириллица превращается в мусор. См. CLAUDE.md.
    g.setColour (alive ? textColour : warnColour);
    g.setFont (juce::FontOptions (12.0f));

    if (alive)
    {
        const auto ratio = proc.lastRatio.load (std::memory_order_relaxed);
        const auto last = proc.lastNote.load (std::memory_order_relaxed);

        g.drawText ("MIDI in - " + juce::String (shownCount) + " notes"
                        + (last >= 0 ? "   last " + noteName (last)
                                         + "   ratio " + juce::String (ratio, 3)
                                     : juce::String()),
                    strip.toNearestInt(), juce::Justification::centredLeft, false);
    }
    else
    {
        g.drawText ("No MIDI input - set the same port in MIDI Out and in the wrapper",
                    strip.toNearestInt(), juce::Justification::centredLeft, false);
    }

    // --- Клавиатура: что звучит прямо сейчас ---------------------------------
    // Пять октав от до под унисоном. Привязка к унисону, а не к фиксированному C2:
    // Root Key двигает всю картину, и нота унисона обязана остаться на виду.
    const int first = 12 * ((shownUnison - 24) / 12);
    constexpr int span = 60;
    constexpr int whiteCount = 35;

    const auto keyWidth = inner.getWidth() / static_cast<float> (whiteCount);
    const auto blackWidth = keyWidth * 0.62f;

    auto levelFor = [this] (int note)
    {
        float level = 0.0f;

        for (int slot = 0; slot < VoiceManager::maxVoices; ++slot)
            if (shownNote[slot] == note)
                level = juce::jmax (level, shownLevel[slot]);

        return juce::jlimit (0.0f, 1.0f, level);
    };

    // Подсветка не гаснет в ноль вместе с огибающей: у самого хвоста уровень уходит
    // за порог видимости раньше, чем звук — за порог слышимости.
    auto litColour = [] (float level)
    {
        return accentColour.withAlpha (0.35f + 0.65f * level);
    };

    int white = 0;

    for (int note = first; note < first + span; ++note)
    {
        if (isBlackKey (note))
            continue;

        juce::Rectangle<float> key (inner.getX() + static_cast<float> (white) * keyWidth,
                                    inner.getY(),
                                    keyWidth - 1.0f, inner.getHeight());
        const auto level = levelFor (note);

        g.setColour (level > 0.0f ? litColour (level) : cellColour);
        g.fillRoundedRectangle (key, 2.0f);

        if (note == shownUnison)
        {
            g.setColour (accentColour);
            g.fillRect (key.removeFromBottom (2.5f));
        }

        if (note % 12 == 0)
        {
            g.setColour (level > 0.0f ? bgColour : dimColour);
            g.setFont (juce::FontOptions (9.0f));
            g.drawText (noteName (note), key.removeFromBottom (12.0f).toNearestInt(),
                        juce::Justification::centred, false);
        }

        ++white;
    }

    white = 0;

    for (int note = first; note < first + span; ++note)
    {
        if (! isBlackKey (note))
        {
            ++white;
            continue;
        }

        juce::Rectangle<float> key (inner.getX() + static_cast<float> (white) * keyWidth
                                        - blackWidth * 0.5f,
                                    inner.getY(), blackWidth, inner.getHeight() * 0.62f);
        const auto level = levelFor (note);

        g.setColour (level > 0.0f ? litColour (level) : bgColour);
        g.fillRoundedRectangle (key, 2.0f);

        if (note == shownUnison)
        {
            g.setColour (accentColour);
            g.fillRect (key.removeFromBottom (2.5f));
        }
    }
}

//==============================================================================
MidiDelayEditor::MidiDelayEditor (MidiDelayProcessor& p)
    : AudioProcessorEditor (&p), proc (p), lane (p)
{
    setLookAndFeel (&lookAndFeel);

    for (int i = 0; i < proc.getNumPrograms(); ++i)
        presetBox.addItem (proc.getProgramName (i), i + 1);

    presetBox.setSelectedItemIndex (proc.getCurrentProgram(), juce::dontSendNotification);
    presetBox.onChange = [this]
    {
        // Пресет ставится через штатный интерфейс программ, а не записью в APVTS:
        // тогда хост узнаёт о смене программы и его собственное меню пресетов
        // не расходится с этим списком.
        proc.setCurrentProgram (presetBox.getSelectedItemIndex());
    };
    addAndMakeVisible (presetBox);
    addAndMakeVisible (lane);

    for (const auto& slot : layout)
        if (addControl (slot.id))
            controlGroups.add (slot.group);

    setSize (columns * cellWidth + 2 * margin,
             headerHeight + laneHeight + laneGap + layoutHeight() + margin);

    // Разложить контекст сразу, а не ждать первого тика: окно открывается уже верным.
    refreshContext();
    startTimerHz (15);
}

MidiDelayEditor::~MidiDelayEditor()
{
    // Оформление снимается раньше, чем разрушается: LookAndFeel, уходящий
    // из-под живого компонента, — это падение, и его ловит pluginval.
    setLookAndFeel (nullptr);
}

bool MidiDelayEditor::addControl (const juce::String& parameterId)
{
    auto* param = proc.apvts.getParameter (parameterId);

    if (param == nullptr)
        return false;   // параметр переименовали — лучше дырка в сетке, чем падение

    controlIds.add (parameterId);

    auto* caption = captions.add (new juce::Label ({}, param->getName (24)));
    caption->setJustificationType (juce::Justification::centred);
    caption->setColour (juce::Label::textColourId, textColour);
    caption->setFont (juce::FontOptions (11.5f));
    addAndMakeVisible (caption);

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param))
    {
        auto* box = new juce::ComboBox();
        box->addItemList (choice->choices, 1);
        controls.add (box);
        addAndMakeVisible (box);
        comboLinks.add (new juce::AudioProcessorValueTreeState::ComboBoxAttachment (
            proc.apvts, parameterId, *box));
        return true;
    }

    if (dynamic_cast<juce::AudioParameterBool*> (param) != nullptr)
    {
        // Подпись у тумблера пустая: имя параметра уже стоит над ячейкой,
        // а «on» рядом с ним — это то же слово дважды.
        auto* toggle = new juce::ToggleButton();
        controls.add (toggle);
        addAndMakeVisible (toggle);
        buttonLinks.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (
            proc.apvts, parameterId, *toggle));
        return true;
    }

    // Ручка с числом под ней: величины тут читаются глазами (миллисекунды, проценты,
    // децибелы), и без числа их не выставить, а только подвигать.
    auto* slider = new juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag,
                                     juce::Slider::TextBoxBelow);
    slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 15);
    // Рамку вокруг числа рисует Label внутри слайдера, и цвет он берёт у самого
    // слайдера, а не у LookAndFeel: поставленный там transparent до него не доезжает.
    slider->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    controls.add (slider);
    addAndMakeVisible (slider);
    sliderLinks.add (new juce::AudioProcessorValueTreeState::SliderAttachment (
        proc.apvts, parameterId, *slider));
    return true;
}

void MidiDelayEditor::refreshContext()
{
    // Три органа, у которых смысл зависит от режима, — та самая причина, по которой
    // одинаковые ручки врали. Теперь они не врут: неработающий гаснет, а тот, у кого
    // сменился смысл, меняет подпись.
    //
    // Quality в этот список не входит, хотя ROADMAP до сегодня утверждал обратное:
    // ADR 0006 отменил свой движок Follow, и качество там выбирается ровно как в Free.
    const auto sync = proc.isSyncMode();
    const auto follow = proc.isFollowMode();
    const auto pingPong = proc.apvts.getRawParameterValue ("pingPong")
                              ->load (std::memory_order_relaxed) > 0.5f;

    setContext ("delayTime", sync, follow ? "Repeat Time" : "Delay Time");
    setContext ("division", ! sync, "Note Division");
    setContext ("width", false, pingPong ? "Width - ping-pong" : "Width - spread");
}

void MidiDelayEditor::setContext (const juce::String& parameterId, bool inert,
                                  const juce::String& caption)
{
    const auto index = controlIds.indexOf (parameterId);

    if (index < 0)
        return;

    controls[index]->setEnabled (! inert);
    captions[index]->setColour (juce::Label::textColourId,
                                inert ? dimColour.withAlpha (0.45f) : textColour);

    // Число под ручкой гаснет вместе с ней: яркая цифра под погашенной ручкой
    // читается как «значение всё-таки действует», то есть ровно как та ложь,
    // ради которой всё это и делалось. Component::setEnabled цвета не трогает.
    if (auto* slider = dynamic_cast<juce::Slider*> (controls[index]))
        slider->setColour (juce::Slider::textBoxTextColourId,
                           inert ? dimColour.withAlpha (0.45f) : textColour);

    if (caption.isNotEmpty() && captions[index]->getText() != caption)
        captions[index]->setText (caption, juce::dontSendNotification);

    captions[index]->repaint();
}

void MidiDelayEditor::resized()
{
    // Список пресетов — в шапке справа, на одной строке с названием плагина:
    // это первое, что трогают, и оно не должно теряться в сетке одинаковых ручек.
    presetBox.setBounds (getWidth() - margin - 210, margin, 210, 26);

    lane.setBounds (margin, headerHeight, getWidth() - 2 * margin, laneHeight);

    bands.clearQuick();

    int y = headerHeight + laneHeight + laneGap;
    int index = 0;

    while (index < controls.size())
    {
        const auto group = controlGroups[index];
        int count = 0;

        while (index + count < controls.size() && controlGroups[index + count] == group)
            ++count;

        const int rows = (count + columns - 1) / columns;

        bands.add (GroupBand { juce::String (bands.size() + 1) + "  " + group.toUpperCase(),
                               { margin, y, getWidth() - 2 * margin,
                                 bandHeader + rows * cellHeight } });

        y += bandHeader;

        for (int i = 0; i < count; ++i)
        {
            juce::Rectangle<int> cell (margin + (i % columns) * cellWidth,
                                       y + (i / columns) * cellHeight,
                                       cellWidth, cellHeight);

            auto* control = controls[index + i];
            captions[index + i]->setBounds (cell.removeFromTop (16));

            // Список и тумблер занимают одну строку по центру ячейки, ручка — всю ячейку.
            if (dynamic_cast<juce::Slider*> (control) != nullptr)
                control->setBounds (cell.reduced (7, 2));
            else if (dynamic_cast<juce::ToggleButton*> (control) != nullptr)
                control->setBounds (cell.withSizeKeepingCentre (44, 22));
            else
                control->setBounds (cell.withSizeKeepingCentre (cellWidth - 24, 24));
        }

        y += rows * cellHeight;
        index += count;
    }
}

void MidiDelayEditor::timerCallback()
{
    const auto quality = static_cast<int> (proc.apvts.getRawParameterValue ("quality")
                                               ->load (std::memory_order_relaxed));
    // Сравнивается заказанное время, а не ручка: на Sync ручка стоит на месте,
    // а под предел движка уезжает именно нотная длительность (#20 против #17).
    const auto clamped = proc.requestedDelayMs() < proc.getMinDelayMs();
    const auto follow = proc.isFollowMode();
    const auto alignment = juce::roundToInt (proc.getAlignmentMs());
    const auto sync = proc.isSyncMode();
    const auto pingPong = proc.apvts.getRawParameterValue ("pingPong")
                              ->load (std::memory_order_relaxed) > 0.5f;
    const auto division = static_cast<int> (proc.apvts.getRawParameterValue ("division")
                                                ->load (std::memory_order_relaxed));
    const auto bpm = juce::roundToInt (proc.getSyncBpm());

    // Программу может сменить и хост — своим меню пресетов или автоматизацией.
    if (const auto preset = proc.getCurrentProgram(); preset != shownPreset)
    {
        shownPreset = preset;
        presetBox.setSelectedItemIndex (preset, juce::dontSendNotification);
    }

    if (quality != shownQuality || clamped != shownClamped || follow != shownFollow
        || alignment != shownAlignment || sync != shownSync || pingPong != shownPingPong
        || division != shownDivision || bpm != shownBpm)
    {
        shownQuality = quality;
        shownClamped = clamped;
        shownFollow = follow;
        shownAlignment = alignment;
        shownSync = sync;
        shownPingPong = pingPong;
        shownDivision = division;
        shownBpm = bpm;

        refreshContext();
        repaint();
    }
}

void MidiDelayEditor::paint (juce::Graphics& g)
{
    g.fillAll (bgColour);

    auto header = getLocalBounds().removeFromTop (headerHeight).reduced (margin, 10);

    g.setColour (textColour);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText ("MIDI DELAY", header.removeFromTop (24),
                juce::Justification::centredLeft, false);

    header.removeFromTop (4);

    // Состояние читается у процессора прямо здесь, а не из полей shown*. Те поля —
    // только детектор изменений для таймера: окно, нарисованное до первого его тика
    // (а таковое бывает — снимок редактора, первый кадр после открытия), обязано
    // показывать правду, а не значения, с которых поля начинались.
    const auto follow = proc.isFollowMode();
    const auto sync = proc.isSyncMode();
    const auto quality = proc.apvts.getRawParameterValue ("quality")
                             ->load (std::memory_order_relaxed) > 0.5f;
    const auto clamped = proc.requestedDelayMs() < proc.getMinDelayMs();
    const auto alignment = juce::roundToInt (proc.getAlignmentMs());

    // Три строки состояния: где унисон, что с сеткой, что с движком. Всё, что
    // невозможно показать картинкой, но без чего не понять, почему звучит именно так.
    // Текст интерфейса — только ASCII (CLAUDE.md).
    g.setColour (dimColour);
    g.setFont (juce::FontOptions (11.5f));

    // Где унисон. Без этой строки узнать неоткуда: Root Key показывает класс высоты
    // без октавы, и на живом прогоне в FL мелодия была нарисована октавой выше —
    // хвост пел на октаву вверх, а слышалось это как расслоение голоса.
    g.drawText ("Unison (ratio 1.000) at " + noteName (proc.getUnisonNote())
                    + " - notes above it transpose the tail up",
                header.removeFromTop (15), juce::Justification::centredLeft, false);

    // Сетка. Без этой строки нотная длительность — это число, которое не с чем сверить:
    // ни темпа не видно, ни того, во что он превратился в миллисекундах.
    g.drawText (sync
                    ? "Sync: " + proc.apvts.getParameter ("division")->getCurrentValueAsText()
                        + " at " + juce::String (proc.getSyncBpm(), 1) + " BPM = "
                        + juce::String (proc.requestedDelayMs(), 1) + " ms"
                    : "Sync: off - delay time set in ms",
                header.removeFromTop (15), juce::Justification::centredLeft, false);

    // В Follow движок свой и предела на время нет: там показывается то, что важно
    // именно в этом режиме, — сколько плагин просит скомпенсировать у хоста.
    // Если хост этого не делает, хвост уедет от сухого ровно на это число.
    const auto minDelay = proc.getMinDelayMs();
    auto engineText = juce::String (follow ? "Mode: Follow - tail sits on the note"
                                    : quality ? "Mode: Free - HQ (Signalsmith)"
                                              : "Mode: Free - Fast (varispeed)");

    if (follow)
        engineText += " - host must compensate " + juce::String (alignment) + " ms";
    else if (minDelay > 0.0)
        engineText += (clamped ? " - delay time raised to min "
                               : " - min delay ") + juce::String (minDelay, 0) + " ms";

    if (! follow && alignment > 0)
        engineText += " - offset costs " + juce::String (alignment) + " ms to host";

    g.setColour (clamped && ! follow ? warnColour : dimColour);
    g.drawText (engineText, header.removeFromTop (15), juce::Justification::centredLeft, false);

    // Полосы групп. Рамка и заголовок, а не только пустое место между рядами:
    // сигнальный путь должен быть виден как путь, а не угадываться по расстояниям.
    for (const auto& band : bands)
    {
        g.setColour (panelColour);
        g.fillRoundedRectangle (band.bounds.toFloat(), 6.0f);

        g.setColour (dimColour);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (band.title, band.bounds.withHeight (bandHeader).reduced (12, 0),
                    juce::Justification::centredLeft, false);

        g.setColour (cellColour);
        g.fillRect (band.bounds.getX() + 12, band.bounds.getY() + bandHeader - 5,
                    band.bounds.getWidth() - 24, 1);
    }
}
