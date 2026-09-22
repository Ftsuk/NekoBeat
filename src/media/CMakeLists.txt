add_library(nekobeat_media STATIC
    MediaProbe.h
    MediaProbe.cpp
    MediaProbeQueue.h
    MediaProbeQueue.cpp
    MpvEngine.h
    MpvVideoWidget.h
    MpvEngine.cpp
    MpvVideoWidget.cpp
)

target_include_directories(nekobeat_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nekobeat_media PUBLIC
    nekobeat_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
)

set(MPV_ROOT "${CMAKE_SOURCE_DIR}/third_party/mpv" CACHE PATH "libmpv development root")
find_path(MPV_INCLUDE_DIR
    NAMES mpv/client.h
    HINTS ${MPV_ROOT}/include
    NO_DEFAULT_PATH
)
find_library(MPV_LIBRARY
    NAMES mpv libmpv
    HINTS ${MPV_ROOT}
    NO_DEFAULT_PATH
)

if(NOT MPV_INCLUDE_DIR OR NOT MPV_LIBRARY)
    message(FATAL_ERROR "libmpv not found. Run scripts/fetch_deps.ps1 first.")
endif()

# MinGW Makefiles cannot reliably consume non-ASCII absolute paths. Stage the
# libmpv headers and import library inside the ASCII build tree.
get_filename_component(MPV_LIBRARY_NAME "${MPV_LIBRARY}" NAME)
set(MPV_STAGE_ROOT "${CMAKE_BINARY_DIR}/third_party/mpv")
file(MAKE_DIRECTORY "${MPV_STAGE_ROOT}/include")
file(COPY "${MPV_INCLUDE_DIR}/mpv" DESTINATION "${MPV_STAGE_ROOT}/include")
file(COPY "${MPV_LIBRARY}" DESTINATION "${MPV_STAGE_ROOT}")
if(EXISTS "${MPV_ROOT}/libmpv-2.dll")
    file(COPY "${MPV_ROOT}/libmpv-2.dll" DESTINATION "${MPV_STAGE_ROOT}")
endif()

target_include_directories(nekobeat_media PUBLIC "${MPV_STAGE_ROOT}/include")
target_link_libraries(nekobeat_media PUBLIC "${MPV_STAGE_ROOT}/${MPV_LIBRARY_NAME}")
