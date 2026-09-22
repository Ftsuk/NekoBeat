find_package(Qt6 REQUIRED COMPONENTS Test)

set(NEKOBEAT_TEST_FIXTURES "${CMAKE_SOURCE_DIR}/../测试脚本"
    CACHE PATH "Folder containing the optional funscript regression media")

add_executable(test_script_loader test_script_loader.cpp)
target_link_libraries(test_script_loader PRIVATE nekobeat_core Qt6::Test)
add_test(NAME script_loader COMMAND test_script_loader)

add_executable(test_tcode_encoder test_tcode_encoder.cpp)
target_link_libraries(test_tcode_encoder PRIVATE nekobeat_core Qt6::Test)
add_test(NAME tcode_encoder COMMAND test_tcode_encoder)

add_executable(test_axis_profiles test_axis_profiles.cpp)
target_link_libraries(test_axis_profiles PRIVATE nekobeat_core Qt6::Test)
add_test(NAME axis_profiles COMMAND test_axis_profiles)

add_executable(test_duration_format test_duration_format.cpp)
target_link_libraries(test_duration_format PRIVATE nekobeat_core Qt6::Test)
add_test(NAME duration_format COMMAND test_duration_format)

add_executable(test_app_logger test_app_logger.cpp)
target_link_libraries(test_app_logger PRIVATE nekobeat_core Qt6::Test)
add_test(NAME app_logger COMMAND test_app_logger)

add_executable(test_device_session test_device_session.cpp)
target_link_libraries(test_device_session PRIVATE nekobeat_core Qt6::Test)
add_test(NAME device_session COMMAND test_device_session)

# The facade that owns the device worker thread: proves the interface can never
# block the machine and that a blocked GUI thread no longer stops it.
add_executable(test_device_link test_device_link.cpp)
target_link_libraries(test_device_link PRIVATE nekobeat_core Qt6::Test)
add_test(NAME device_link COMMAND test_device_link)

add_executable(test_scheduler test_scheduler.cpp)
target_link_libraries(test_scheduler PRIVATE nekobeat_core Qt6::Test)
add_test(NAME scheduler COMMAND test_scheduler)

set_tests_properties(script_loader tcode_encoder scheduler PROPERTIES
    ENVIRONMENT "NEKOBEAT_TEST_FIXTURES=${NEKOBEAT_TEST_FIXTURES}"
)

add_executable(test_media_playback test_media_playback.cpp)
target_link_libraries(test_media_playback PRIVATE nekobeat_media Qt6::Test)
add_custom_command(TARGET test_media_playback POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/mpv/libmpv-2.dll"
        "$<TARGET_FILE_DIR:test_media_playback>"
)
add_test(NAME media_playback COMMAND test_media_playback)
set_tests_properties(media_playback PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "NEKOBEAT_TEST_FIXTURES=${NEKOBEAT_TEST_FIXTURES}"
)

add_executable(test_media_library_store test_media_library_store.cpp)
target_link_libraries(test_media_library_store PRIVATE nekobeat_core Qt6::Test)
add_test(NAME media_library_store COMMAND test_media_library_store)

add_executable(test_favorites_store test_favorites_store.cpp)
target_link_libraries(test_favorites_store PRIVATE nekobeat_core Qt6::Test)
add_test(NAME favorites_store COMMAND test_favorites_store)

add_executable(test_loop_store test_loop_store.cpp)
target_link_libraries(test_loop_store PRIVATE nekobeat_core Qt6::Test)
add_test(NAME loop_store COMMAND test_loop_store)

add_executable(test_loop_clip test_loop_clip.cpp)
target_link_libraries(test_loop_clip PRIVATE nekobeat_core Qt6::Test)
add_test(NAME loop_clip COMMAND test_loop_clip)

add_executable(test_media_reference_index test_media_reference_index.cpp)
target_link_libraries(test_media_reference_index PRIVATE nekobeat_core Qt6::Test)
add_test(NAME media_reference_index COMMAND test_media_reference_index)

add_executable(test_media_cleanup test_media_cleanup.cpp)
target_link_libraries(test_media_cleanup PRIVATE nekobeat_core Qt6::Test)
add_test(NAME media_cleanup COMMAND test_media_cleanup)

add_executable(test_media_relink test_media_relink.cpp)
target_link_libraries(test_media_relink PRIVATE nekobeat_core Qt6::Test)
add_test(NAME media_relink COMMAND test_media_relink)

add_executable(test_media_probe test_media_probe.cpp)
target_link_libraries(test_media_probe PRIVATE nekobeat_media Qt6::Test)
add_custom_command(TARGET test_media_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/mpv/libmpv-2.dll"
        "$<TARGET_FILE_DIR:test_media_probe>"
)
add_test(NAME media_probe COMMAND test_media_probe)
set_tests_properties(media_probe PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "NEKOBEAT_TEST_FIXTURES=${NEKOBEAT_TEST_FIXTURES}"
)

add_executable(test_video_widget test_video_widget.cpp)
target_link_libraries(test_video_widget PRIVATE nekobeat_media Qt6::Test)
add_custom_command(TARGET test_video_widget POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/mpv/libmpv-2.dll"
        "$<TARGET_FILE_DIR:test_video_widget>"
)
add_test(NAME video_widget COMMAND test_video_widget)

# The cover wall layout rule lives in the two grid widgets, which belong to the
# application target; compiling the two sources directly keeps the rule under
# test without pulling in the whole window.
add_executable(test_grid_layout
    test_grid_layout.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/MediaGridWidget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/LoopGridWidget.cpp
)
target_link_libraries(test_grid_layout PRIVATE nekobeat_core Qt6::Test)
add_test(NAME grid_layout COMMAND test_grid_layout)

# The folder scan belongs in a worker; this pins that it is not done inline.
add_executable(test_media_library_scan
    test_media_library_scan.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/MediaLibrary.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/MediaGridWidget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.cpp
)
target_link_libraries(test_media_library_scan PRIVATE
    nekobeat_core
    nekobeat_media
    Qt6::Test
)
add_custom_command(TARGET test_media_library_scan POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/mpv/libmpv-2.dll"
        "$<TARGET_FILE_DIR:test_media_library_scan>"
)
add_test(NAME media_library_scan COMMAND test_media_library_scan)
set_tests_properties(media_library_scan PROPERTIES TIMEOUT 120)

# The limit slider reports the touched edge when an interaction ends, including
# when the value did not change (see the 0 % / 100 % handle bug). Compiling the
# widget directly keeps the rule under test without the whole window.
add_executable(test_range_slider
    test_range_slider.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/RangeSlider.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.cpp
)
target_link_libraries(test_range_slider PRIVATE nekobeat_core Qt6::Test)
add_test(NAME range_slider COMMAND test_range_slider)

# Scrubbing the heat map has three phases (press / real move / release) because
# the machine is only held still for a real drag.
add_executable(test_heatmap_scrub
    test_heatmap_scrub.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/HeatmapWidget.cpp
)
target_link_libraries(test_heatmap_scrub PRIVATE nekobeat_core Qt6::Test)
add_test(NAME heatmap_scrub COMMAND test_heatmap_scrub)

# One axis of the limit panel: tapping the end of a full-travel limit has to ask
# the device to move there, which is the user-visible half of the same bug.
add_executable(test_axis_limit_row
    test_axis_limit_row.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/AxisLimitRow.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/RangeSlider.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.cpp
)
target_link_libraries(test_axis_limit_row PRIVATE nekobeat_core Qt6::Test)
add_test(NAME axis_limit_row COMMAND test_axis_limit_row)

# The calibration dialog had no entry point; this pins what the menu action now
# relies on, plus the panel's ability to store calibrated values unchanged.
add_executable(test_axis_calibration
    test_axis_calibration.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/AxisLimitPanel.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/AxisLimitRow.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/CalibrationDialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/RangeSlider.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/Theme.cpp
)
target_link_libraries(test_axis_calibration PRIVATE nekobeat_core Qt6::Test)
add_test(NAME axis_calibration COMMAND test_axis_calibration)

# Tests write through the same AppLogger as the program, so without this their
# output lands in the user's real diagnostic log - the file people read when
# something goes wrong. Send every test's log into the build tree instead.
# (test_app_logger sets its own directory and overrides this on purpose.)
set_property(TEST
    script_loader tcode_encoder axis_profiles duration_format app_logger
    device_session device_link scheduler media_playback
    media_library_store favorites_store loop_store loop_clip
    media_reference_index media_cleanup media_relink media_probe
    video_widget grid_layout media_library_scan range_slider
    heatmap_scrub axis_limit_row axis_calibration
    APPEND PROPERTY ENVIRONMENT "NEKOBEAT_LOG_DIR=${CMAKE_BINARY_DIR}/test-logs")
