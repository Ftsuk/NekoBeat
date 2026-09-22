cmake_minimum_required(VERSION 3.24)

project(NekoBeat
    VERSION 1.0.0
    DESCRIPTION "NekoBeat - multi-axis funscript video player"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(NEKOBEAT_BUILD_TESTS "Build unit tests" ON)

if(WIN32)
    # Needed for the executable icon resource below.
    enable_language(RC)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR}/src/assets")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets SerialPort Test OpenGL OpenGLWidgets Concurrent)

# Every intermediate build reports the same project version, so the log carries
# the revision it was built from. Without this a report of "it still freezes"
# cannot be traced back to the code that produced it.
set(NEKOBEAT_BUILD_TAG "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE NEKOBEAT_BUILD_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT NEKOBEAT_BUILD_TAG)
        set(NEKOBEAT_BUILD_TAG "unknown")
    endif()
    execute_process(
        COMMAND ${GIT_EXECUTABLE} status --porcelain
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE NEKOBEAT_BUILD_DIRTY
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NEKOBEAT_BUILD_DIRTY)
        string(APPEND NEKOBEAT_BUILD_TAG "+dirty")
    endif()
endif()
message(STATUS "NekoBeat build tag: ${NEKOBEAT_BUILD_TAG}")

set(NEKOBEAT_CORE_SOURCES
    src/core/AppLogger.cpp
    src/core/AxisProfileStore.cpp
    src/core/DurationFormat.cpp
    src/core/FavoritesStore.cpp
    src/core/LoopClip.cpp
    src/core/Loc.cpp
    src/core/LocTable.cpp
    src/core/LegacyMigration.cpp
    src/core/LoopStore.cpp
    src/core/MediaCleanup.cpp
    src/core/MediaLibraryStore.cpp
    src/core/MediaReferenceIndex.cpp
    src/core/MediaRelink.cpp
    src/core/PathUtils.cpp
    src/core/StoreBackup.cpp
    src/core/Track.cpp
    src/core/ScriptFormat.cpp
    src/core/ScriptLoader.cpp
)

set(NEKOBEAT_DEVICE_SOURCES
    src/device/DeviceLink.cpp
    src/device/IDeviceControl.h
    src/device/ITCodeTransport.h
    src/device/TCodeEncoder.cpp
    src/device/SerialTransport.cpp
    src/device/SerialPortScanner.cpp
    src/device/DeviceSession.cpp
)

set(NEKOBEAT_SYNC_SOURCES
    src/sync/IMediaClock.h
    src/sync/MotionScheduler.cpp
)

add_library(nekobeat_core STATIC
    ${NEKOBEAT_CORE_SOURCES}
    ${NEKOBEAT_DEVICE_SOURCES}
    ${NEKOBEAT_SYNC_SOURCES}
)

target_include_directories(nekobeat_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(nekobeat_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::SerialPort
)

if(WIN32)
    # SerialPortScanner walks the Windows port class (SetupAPI / CfgMgr32) to
    # report devices whose driver failed to start.
    target_link_libraries(nekobeat_core PUBLIC setupapi cfgmgr32)
endif()

add_subdirectory(src/media)

add_executable(NekoBeat WIN32
    src/main.cpp
    src/assets/icons.qrc
    $<$<PLATFORM_ID:Windows>:src/NekoBeat.rc>
    src/ui/AxisLimitPanel.cpp
    src/ui/AxisLimitRow.cpp
    src/ui/CalibrationDialog.cpp
    src/ui/MainWindow.cpp
    src/ui/SeekSafetyDialog.cpp
    src/ui/MediaLibrary.cpp
    src/ui/MediaLibraryDialog.cpp
    src/ui/MediaCleanupPrompt.cpp
    src/ui/HeatmapWidget.cpp
    src/ui/LibraryDragHint.cpp
    src/ui/LoopClipDialog.cpp
    src/ui/LoopGridWidget.cpp
    src/ui/LoopListPanel.cpp
    src/ui/LoopManagerDialog.cpp
    src/ui/MediaGridWidget.cpp
    src/ui/NativeFullScreen.cpp
    src/ui/RangeSlider.cpp
    src/ui/RelinkDialog.cpp
    src/ui/SerialPortDialog.cpp
    src/ui/SidePanelFold.cpp
    src/ui/Theme.cpp
)

target_link_libraries(NekoBeat PRIVATE
    nekobeat_core
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    # The port picker probes ports on a worker: opening a Bluetooth port brings the
    # radio link up first, and doing that on the GUI thread froze the dialog.
    Qt6::Concurrent
)

target_link_libraries(NekoBeat PRIVATE nekobeat_media)

target_compile_definitions(NekoBeat PRIVATE
    NEKOBEAT_VERSION="${PROJECT_VERSION}"
    NEKOBEAT_BUILD="${NEKOBEAT_BUILD_TAG}"
)

if(NEKOBEAT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS NekoBeat RUNTIME DESTINATION .)
