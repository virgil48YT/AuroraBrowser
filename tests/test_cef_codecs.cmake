include("${AURORA_SOURCE_DIR}/cmake/CefCodecs.cmake")

set(args "${CMAKE_CURRENT_BINARY_DIR}/test-codec-args.gn")
aurora_cef_codec_args_enabled("${CMAKE_CURRENT_BINARY_DIR}/test-missing-codec-args.gn" enabled)
if(enabled)
    message(FATAL_ERROR "Missing GN arguments must not pass")
endif()

file(WRITE "${args}" "# ffmpeg_branding = \"Chrome\"\n"
                     "# proprietary_codecs = true\n")
aurora_cef_codec_args_enabled("${args}" enabled)
if(enabled)
    message(FATAL_ERROR "Commented-out GN arguments must not pass")
endif()

file(WRITE "${args}" "ffmpeg_branding = \"Chromium\"\n"
                     "proprietary_codecs = true\n")
aurora_cef_codec_args_enabled("${args}" enabled)
if(enabled)
    message(FATAL_ERROR "Chromium FFmpeg branding must not pass")
endif()

file(WRITE "${args}" "ffmpeg_branding = \"Chrome\"\n"
                     "proprietary_codecs = false\n")
aurora_cef_codec_args_enabled("${args}" enabled)
if(enabled)
    message(FATAL_ERROR "Disabled proprietary codecs must not pass")
endif()

file(WRITE "${args}" "ffmpeg_branding = \"Chrome\"\n"
                     "proprietary_codecs = true\n")
aurora_cef_codec_args_enabled("${args}" enabled)
if(NOT enabled)
    message(FATAL_ERROR "Codec-enabled GN arguments should pass")
endif()
