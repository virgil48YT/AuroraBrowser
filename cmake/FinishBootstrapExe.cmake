# Post-build steps for the Windows sandbox build (src/app/CMakeLists.txt),
# after CEF's bootstrap.exe has been copied in as aurora_browser.exe:
#   1. set Aurora's icon on it (rcedit ships in the repository root);
#   2. grant "ALL RESTRICTED APPLICATION PACKAGES" (S-1-15-2-2) read/execute
#      on the output directory — CEF's SET_LPAC_ACLS — so the network
#      service, which Chromium sandboxes in an LPAC AppContainer, can load
#      the runtime from a developer build tree. Installed builds grant it at
#      runtime (ensure_lpac_read_access() in src/app/main.cpp).
# Both are best effort by design: a build must not fail because an icon could
# not be patched or a filesystem does not support ACLs.
#   cmake -DRCEDIT=... -DEXE=... -DICON=... -DLPAC_DIR=... -P this
if(EXISTS "${RCEDIT}" AND EXISTS "${EXE}" AND EXISTS "${ICON}")
    execute_process(COMMAND "${RCEDIT}" "${EXE}" --set-icon "${ICON}"
        RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
    if(NOT _rc EQUAL 0)
        message(STATUS "bootstrap icon: rcedit failed (${_rc}); keeping CEF's icon")
    endif()
else()
    message(STATUS "bootstrap icon: skipped (rcedit, exe or icon missing)")
endif()
if(LPAC_DIR)
    execute_process(COMMAND icacls "${LPAC_DIR}" /grant "*S-1-15-2-2:(OI)(CI)(RX)" /Q
        RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
    if(NOT _rc EQUAL 0)
        message(STATUS "LPAC ACL: icacls failed (${_rc}) on ${LPAC_DIR}")
    endif()
endif()
