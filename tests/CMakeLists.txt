add_executable(aurora_tests
    test_url.cpp
    test_tab_manager.cpp
    test_tab_suspension.cpp
    test_storage.cpp
    test_history.cpp
    test_bookmarks.cpp
    test_bookmark_io.cpp
    test_session.cpp
    test_session_restore.cpp
    test_bulk_restore.cpp
    test_settings.cpp
    test_settings_presentation.cpp
    test_site_prefs.cpp
    test_internal_url.cpp
    test_chrome_nav.cpp
    test_downloads.cpp
    test_adblock.cpp
    test_list_updater.cpp
    test_gpu_policy.cpp
    test_media_policy.cpp
    test_incognito.cpp
    test_privacy.cpp
    test_cookie_site.cpp
    test_extensions.cpp
    test_extension_load_list.cpp
    test_search_engines.cpp
    test_sha256.cpp
    test_build_id.cpp
    test_cef_layout.cpp
)

target_include_directories(aurora_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/doctest)

target_link_libraries(aurora_tests PRIVATE aurora_core)

# doctest's main() is provided by test_url.cpp, which defines
# DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN before including the header. Other test
# files include doctest.h normally (implementation shared across TUs).

include(CTest)
add_test(NAME aurora_tests COMMAND aurora_tests)
add_test(NAME aurora_cef_codec_args
    COMMAND "${CMAKE_COMMAND}" -DAURORA_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test_cef_codecs.cmake")
