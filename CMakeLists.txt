cmake_minimum_required(VERSION 3.24)
project(LinSpice VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg PrintSupport Concurrent)
find_package(ZLIB REQUIRED)

set(CORE_SOURCES
    src/core/Units.cpp
    src/core/TextIO.cpp
    src/core/Symbol.cpp
    src/core/SymbolLibrary.cpp
    src/core/Schematic.cpp
    src/core/Netlister.cpp
    src/core/NetlistTranslator.cpp
    src/core/StepSweep.cpp
    src/core/RawFile.cpp
    src/core/Expression.cpp
    src/core/Fft.cpp
    src/core/PlotSettings.cpp
    src/core/ZipReader.cpp
    src/core/ModelLibrary.cpp
    src/core/Measure.cpp
    src/core/AppSettings.cpp
)

add_library(linspice_core STATIC ${CORE_SOURCES} resources/resources.qrc)
target_include_directories(linspice_core PUBLIC src)
target_link_libraries(linspice_core PUBLIC Qt6::Core Qt6::Gui ZLIB::ZLIB)

option(LINSPICE_CORE_ONLY "Build only the core library and tests" OFF)

if(NOT LINSPICE_CORE_ONLY)
set(SIM_SOURCES
    src/sim/SimWorker.cpp
    src/sim/SimController.cpp
)
# The batch mode and worker live in the same binary as the GUI.

set(GUI_SOURCES
    src/gui/Document.h
    src/gui/MainWindow.cpp
    src/gui/Icons.cpp
    src/gui/SchematicView.cpp
    src/gui/SchematicPainter.cpp
    src/gui/SymbolEditor.cpp
    src/gui/WaveformView.cpp
    src/gui/WaveformPane.cpp
    src/gui/NetlistEditor.cpp
    src/gui/dialogs/ComponentBrowser.cpp
    src/gui/dialogs/AnalysisDialog.cpp
    src/gui/dialogs/SourceDialog.cpp
    src/gui/dialogs/PassiveDialog.cpp
    src/gui/dialogs/AttributeDialog.cpp
    src/gui/dialogs/TextDialog.cpp
    src/gui/dialogs/NetLabelDialog.cpp
    src/gui/dialogs/SettingsDialog.cpp
    src/gui/dialogs/AddTraceDialog.cpp
    src/gui/dialogs/AxisDialog.cpp
    src/gui/dialogs/FftDialog.cpp
    src/gui/dialogs/MiscDialogs.cpp
)

add_executable(linspice src/main.cpp ${SIM_SOURCES} ${GUI_SOURCES})
target_compile_definitions(linspice PRIVATE LINSPICE_VERSION="${PROJECT_VERSION}")
target_link_libraries(linspice PRIVATE linspice_core Qt6::Widgets Qt6::Svg Qt6::PrintSupport Qt6::Concurrent ${CMAKE_DL_LIBS})
target_include_directories(linspice PRIVATE src)

include(GNUInstallDirs)
install(TARGETS linspice RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/linspice.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/linspice.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES packaging/linspice.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
endif()

enable_testing()
add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE linspice_core)
add_test(NAME core_tests COMMAND core_tests)
