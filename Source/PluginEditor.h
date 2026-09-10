#pragma once
#include "PluginProcessor.h"

/** Оформление плагина (#26). Основа — LookAndFeel_V4, а не голый LookAndFeel:
    списки, всплывающие меню и текстовые поля у него уже нарисованы прилично,
    и переписывать их значило бы менять работающее на своё такое же. Своими
    остаются три вещи, которые в стандартном виде выдают демо из туториала:
    ручка, галка и список. */
class BergLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    BergLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float startAngle, float endAngle,
                           juce::Slider&) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool isHighlighted, bool isDown) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;
};

/** Лента нот и хвостов (#27). Отвечает на вопрос «плагин не работает?» без слов:
    доехал ли MIDI, какие ноты звучат сейчас, сколько голосов занято и где унисон.

    Отдельный компонент, а не кусок paint редактора: хвост гаснет плавно, и его
    надо перерисовывать чаще и мельче, чем окно с двумя десятками ручек.

    Обмен с аудиопотоком — только атомики процессора, ни локов, ни очередей. */
class NoteLane final : public juce::Component,
                       private juce::Timer
{
public:
    explicit NoteLane (MidiDelayProcessor&);

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    MidiDelayProcessor& proc;

    /** Снимок, который сейчас нарисован. Таймер сравнивает с ним свежий и будит
        перерисовку только когда что-то изменилось: гаснущий хвост меняется каждый
        кадр, стоящее окно — ни одного. */
    int shownNote[VoiceManager::maxVoices] {};
    float shownLevel[VoiceManager::maxVoices] {};
    int shownCount = -1;
    int shownUnison = -1;
    int shownLimit = -1;

    /** Счётчик нот на прошлом кадре: по его приросту мигает индикатор входа.
        Без мигания «MIDI приходит» неотличимо от «MIDI приходил когда-то». */
    int lastSeenCount = 0;
    int blinkFrames = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteLane)
};

/** Рабочее окно: шапка с пресетом и состоянием, лента нот (#27), ниже органы
    управления, сгруппированные по сигнальному пути (#26).

    Группы — не украшение. Пятнадцать параметров в одинаковой сетке 4x4 читались
    как список, а у трёх из них смысл зависит от режима: Delay Time в Follow задаёт
    интервал повторов, Note Division живёт только при включённом Sync, Width при
    ping-pong означает разлёт нот, а не раскидку голосов. Одинаковые ручки в таком
    положении врут, поэтому неработающие гаснут, а подписи меняются по режиму.

    Органы строятся по типу параметра, а не расписаны по одному: пятнадцать почти
    одинаковых блоков по шесть строк каждый — это сто строк, которые нечего читать. */
class MidiDelayEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    explicit MidiDelayEditor (MidiDelayProcessor&);
    ~MidiDelayEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    /** Слой, на котором лежит вся раскладка (#29). Заведён ради одной строки —
        AffineTransform на нём: окно тянут мышью, а сетка остаётся посчитанной
        в базовых 672 x 766 и просто масштабируется целиком вместе с кеглями.

        Пересчитывать раскладку под каждый размер значило бы заново подбирать
        колонки и шрифты, а сетка 5 x N по сигнальному пути осмысленного узкого
        варианта не имеет: ради него пришлось бы переделывать #26.

        Трансформ именно на дочернем слое, а не на самом редакторе, — требование
        JUCE: `AudioProcessorEditor::editorResized` проверяет ассертом, что
        собственный трансформ редактора равен хостовому, потому что иначе правка
        затирает масштаб DPI, пришедший от хоста. */
    struct Content final : juce::Component
    {
        explicit Content (MidiDelayEditor& e) : owner (e) {}

        void paint (juce::Graphics& g) override { owner.paintContent (g); }
        void resized() override                 { owner.layoutContent(); }

        MidiDelayEditor& owner;
    };

    /** Рисует и раскладывает содержимое в базовых координатах. Вызываются со слоя,
        поэтому размер берут у него, а не у редактора: у редактора он масштабный. */
    void paintContent (juce::Graphics&);
    void layoutContent();

    /** Заводит орган под параметр: список для выбора, галку для флага, ручку
        для всего остального. Привязка к APVTS живёт в соответствующем массиве. */
    bool addControl (const juce::String& parameterId);

    /** Прогоняет по органам правила режимов: что сейчас мертво и у чего сменился
        смысл. Зовётся из конструктора и из таймера при смене режима. */
    void refreshContext();

    /** Гасит орган, который в текущем режиме ни на что не влияет, и переписывает
        подпись там, где режим меняет смысл. Пустая подпись — оставить прежнюю. */
    void setContext (const juce::String& parameterId, bool inert,
                     const juce::String& caption);

    MidiDelayProcessor& proc;

    /** Объявлен раньше всех компонентов, поэтому разрушается позже них: LookAndFeel,
        уходящий из-под живого компонента, — это падение в деструкторе. */
    BergLookAndFeel lookAndFeel;

    /** Объявлен раньше органов управления: они его дети, и разрушаться должны
        первыми, пока родитель ещё жив. */
    Content content { *this };

    NoteLane lane;

    /** Список фабричных пресетов (#48). Отдельным полем, а не через addControl:
        пресет — не параметр APVTS, он выставляет сразу все. */
    juce::ComboBox presetBox;

    /** Заголовок группы и рамка под неё, посчитанные в resized и нарисованные
        в paint. Компонентов под них не заводится: рисовать четыре надписи дешевле,
        чем держать четыре Label ради того же результата. */
    struct GroupBand { juce::String title; juce::Rectangle<int> bounds; };
    juce::Array<GroupBand> bands;

    juce::StringArray controlIds;      // параллельно controls: по нему ищется орган
    juce::StringArray controlGroups;   // и по нему же режется на полосы
    juce::OwnedArray<juce::Component> controls;
    juce::OwnedArray<juce::Label> captions;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::SliderAttachment> sliderLinks;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboLinks;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonLinks;

    int shownPreset = -1;         // индекс пресета, показанный в списке
    int shownQuality = -1;
    bool shownClamped = false;   // delay time сейчас подтянут до предела движка (#17)
    bool shownFollow = false;    // режим Follow: хвост стоит на ноте (ADR 0006)
    int shownAlignment = -1;     // сколько мс плагин просит скомпенсировать у хоста
    bool shownSync = false;      // время задано нотной длительностью, а не ручкой (#20)
    bool shownPingPong = false;  // ноты уходят попеременно влево и вправо (#23)
    int shownDivision = -1;      // индекс делителя
    int shownBpm = -1;           // темп хоста, округлённый: перерисовывать окно
                                 // от дрожания в сотых долях BPM незачем

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiDelayEditor)
};
