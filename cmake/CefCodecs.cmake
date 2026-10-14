# CEF codecs are compiled into Chromium/FFmpeg, not linked by Aurora and not
# enabled by a CefSettings or command-line switch. This checks the GN arguments
# recorded alongside a custom CEF distribution; it cannot prove that the
# resulting binary actually decodes media (use the runtime smoke test too).
function(aurora_cef_codec_args_enabled args_file result)
    set(${result} FALSE PARENT_SCOPE)
    if(NOT EXISTS "${args_file}")
        return()
    endif()
    file(STRINGS "${args_file}" branding_lines
        REGEX "^[ \t]*ffmpeg_branding[ \t]*=[ \t]*\"Chrome\"[ \t]*(#.*)?$")
    file(STRINGS "${args_file}" proprietary_lines
        REGEX "^[ \t]*proprietary_codecs[ \t]*=[ \t]*true[ \t]*(#.*)?$")
    if(branding_lines AND proprietary_lines)
        set(${result} TRUE PARENT_SCOPE)
    endif()
endfunction()
