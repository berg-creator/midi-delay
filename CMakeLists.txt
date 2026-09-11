cmake_minimum_required(VERSION 3.22)
project(MidiDelay VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Universal binary на macOS: плагин должен грузиться и в arm64-, и в x86_64-хостах.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

# В CI копировать плагин в системные каталоги не нужно и вредно: на Windows JUCE лезет
# в Program Files\Common Files\VST3, падает уже после успешной компиляции, и выглядит
# это как «сборка сломалась», хотя собралось всё.
option(MIDIDELAY_COPY_PLUGIN "Копировать собранный плагин в системные каталоги" ON)

add_subdirectory(libs/JUCE)

# HQ-движок питчинга (#38). Только заголовки, вендорены копией — почему копия, а не
# submodule, написано в libs/signalsmith-stretch/README.md. Своего CMakeLists у копии
# нет намеренно: одна INTERFACE-цель короче, чем два чужих файла с FetchContent.
add_library(signalsmith-stretch INTERFACE)
target_include_directories(signalsmith-stretch INTERFACE libs/signalsmith-stretch)

# Единственное, что взято из выброшенного CMakeLists библиотеки: без /bigobj MSVC
# не переваривает столько шаблонов и падает на «too many sections». На Windows это
# проверяется только в CI, поэтому флаг стоит здесь, а не выясняется красной сборкой.
if(MSVC)
    target_compile_options(signalsmith-stretch INTERFACE /bigobj)
endif()

# Имя компании, а не автора: под ним пойдут и бесплатный плагин, и платные следом
# (STRATEGY §1). В метаданных обычный регистр, капслок остаётся начертанию логотипа —
# в списке производителей DAW слово капсом читается как крик, а Berg не аббревиатура.
juce_add_plugin(MidiDelay
    COMPANY_NAME            "Berg"
    # Меняется вместе с именем: идентификатор AU это пара кодов, и до первого релиза
    # его менять бесплатно, а после — значит потерять чужие сохранённые проекты.
    # Валидация теперь: auval -v aumf Mdly Berg.
    PLUGIN_MANUFACTURER_CODE Berg
    PLUGIN_CODE             Mdly
    PRODUCT_NAME            "MIDI Delay"
    FORMATS                 VST3 AU Standalone

    IS_SYNTH                FALSE
    IS_MIDI_EFFECT          FALSE
    # Без этого FL Studio не покажет у плагина MIDI-вход, и вся идея не заработает.
    NEEDS_MIDI_INPUT        TRUE
    NEEDS_MIDI_OUTPUT       FALSE

    COPY_PLUGIN_AFTER_BUILD ${MIDIDELAY_COPY_PLUGIN}
    VST3_CATEGORIES         "Fx" "Delay"
    # aumf, а не aufx: AU-эффект, принимающий MIDI. С aufx Logic не даст роутить MIDI
    # на плагин, и auval ругается. Для VST3 разницы нет, для AU — принципиально.
    AU_MAIN_TYPE            kAudioUnitType_MusicEffect)

target_sources(MidiDelay PRIVATE
    Source/PluginProcessor.cpp
    Source/DSP/DelayBuffer.cpp
    Source/DSP/PitchShifter.cpp
    Source/DSP/Voice.cpp
    Source/DSP/VoiceManager.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(MidiDelay PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(MidiDelay PRIVATE
    juce::juce_audio_utils
    signalsmith-stretch
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Офлайн-тест процессора: аллокации, смещение дилея, состояние, регрессионные рендеры
# (#34). Из сборки по умолчанию исключён: у консольного приложения своя копия модулей
# JUCE, и пересобирать её при каждой правке плагина незачем. CI собирает его отдельным
# шагом — поломка звука обязана ловиться и там, а не только руками перед коммитом:
#   cmake --build build --target ProcessorTest && ./build/ProcessorTest_artefacts/Release/ProcessorTest
juce_add_console_app(ProcessorTest PRODUCT_NAME "ProcessorTest")
set_target_properties(ProcessorTest PROPERTIES EXCLUDE_FROM_ALL TRUE)

target_sources(ProcessorTest PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/DSP/DelayBuffer.cpp
    Source/DSP/PitchShifter.cpp
    Source/DSP/Voice.cpp
    Source/DSP/VoiceManager.cpp
    Source/test_processor.cpp)

target_compile_definitions(ProcessorTest PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1)

target_link_libraries(ProcessorTest PRIVATE
    juce::juce_audio_utils
    signalsmith-stretch
    juce::juce_recommended_config_flags)
