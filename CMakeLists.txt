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
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(MidiDelay PRIVATE
    juce::juce_audio_utils
    signalsmith-stretch
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Тексты лицензий стороннего кода — внутрь бандлов (#42). Только силами сборки: файл,
# положенный в бандл после подписи, ломает подпись. Тексты читаются из самих зависимостей,
# а не переписываются: пропал файл — конфигурация падает. Список — это модули JUCE,
# которые линкует juce_audio_utils, сверенные с libs/JUCE/JUCE.spdx.json; ASIO, Oboe,
# LV2 и AAX не собираются, Box2D и QuickJS не линкуются. FLAC и Ogg линкер на macOS
# выбрасывает, но на Windows это не проверено, и лишняя строка дешевле недостающей.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/libs/JUCE/JUCE.spdx.json" juceSpdx)
string(JSON juceVersion GET "${juceSpdx}" packages 0 versionInfo)
if(NOT juceVersion STREQUAL "9.0.2")
    message(FATAL_ERROR "JUCE обновилась до ${juceVersion}: сверить список лицензий ниже "
        "с libs/JUCE/JUCE.spdx.json (#42) и поднять версию в этой проверке")
endif()

set(notices "${CMAKE_CURRENT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt")
set(noticesText "MIDI Delay includes the following third-party software.\n")
foreach(entry IN ITEMS
        "Signalsmith Stretch|libs/signalsmith-stretch/LICENSE.txt"
        "Signalsmith Linear|libs/signalsmith-stretch/signalsmith-linear/LICENSE.txt"
        "JUCE (used under the JUCE 9 EULA, Starter licence)|libs/JUCE/LICENSE.md"
        "zlib|libs/JUCE/modules/juce_core/zip/zlib/LICENSE"
        "HarfBuzz|libs/JUCE/modules/juce_graphics/fonts/harfbuzz/COPYING"
        "SheenBidi|libs/JUCE/modules/juce_graphics/unicode/sheenbidi/LICENSE"
        "LunaSVG|libs/JUCE/modules/juce_graphics/drawables/lunasvg/LICENSE"
        "PlutoVG|libs/JUCE/modules/juce_graphics/drawables/lunasvg/plutovg/LICENSE"
        "libpng|libs/JUCE/modules/juce_graphics/image_formats/pnglib/LICENSE"
        "libjpeg. This software is based in part on the work of the Independent JPEG Group|libs/JUCE/modules/juce_graphics/image_formats/jpglib/README"
        "FLAC|libs/JUCE/modules/juce_audio_formats/codecs/flac/Flac Licence.txt"
        "libogg and libvorbis|libs/JUCE/modules/juce_audio_formats/codecs/oggvorbis/Ogg Vorbis Licence.txt"
        "VST3 SDK|libs/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt"
        "AudioUnitSDK|libs/JUCE/modules/juce_audio_plugin_client/AU/AudioUnitSDK/LICENSE.txt")
    string(FIND "${entry}" "|" bar)
    string(SUBSTRING "${entry}" 0 ${bar} name)
    math(EXPR bar "${bar} + 1")
    string(SUBSTRING "${entry}" ${bar} -1 path)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${path}")
        message(FATAL_ERROR "Нет файла лицензии ${path} (#42)")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${path}" text)
    string(APPEND noticesText "\n\n==== ${name} ====\n\n${text}")
endforeach()
# Через COPYONLY, чтобы файл не перезаписывался без изменений и не пересобирал бандлы.
file(WRITE "${notices}.in" "${noticesText}")
configure_file("${notices}.in" "${notices}" COPYONLY)

set_source_files_properties("${notices}" PROPERTIES
    HEADER_FILE_ONLY TRUE
    MACOSX_PACKAGE_LOCATION Resources)
# Подписывает бандл JUCE, шагом после линковки. Изменись один только файл — линковки
# нет, подписи нет, и бандл выходит с битой печатью: отсюда LINK_DEPENDS.
foreach(format VST3 AU)
    if(TARGET MidiDelay_${format})
        target_sources(MidiDelay_${format} PRIVATE "${notices}")
        set_property(TARGET MidiDelay_${format} APPEND PROPERTY LINK_DEPENDS "${notices}")
    endif()
endforeach()
# На Windows MACOSX_PACKAGE_LOCATION молчит, а VST3 там тоже бандл-каталог.
if(WIN32)
    add_custom_command(TARGET MidiDelay_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MidiDelay_VST3>/../Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${notices}" "$<TARGET_FILE_DIR:MidiDelay_VST3>/../Resources/")
endif()

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
