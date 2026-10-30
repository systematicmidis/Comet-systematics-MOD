# Invoked as a PRE_LINK step for Comet (see CMakeLists.txt).
#
# Windows refuses to overwrite a running .exe, so rebuilding while the app is open otherwise dies
# with a cryptic "LNK1104: cannot open file 'Comet.exe'" or "Permission denied" from the asset
# copy. Detect that up front and say what to actually do about it.

if(NOT DEFINED EXE OR EXE STREQUAL "")
    return()
endif()

get_filename_component(EXE_NAME "${EXE}" NAME)

execute_process(
    COMMAND tasklist /FI "IMAGENAME eq ${EXE_NAME}" /NH
    OUTPUT_VARIABLE TASKLIST_OUTPUT
    ERROR_VARIABLE TASKLIST_ERROR
    RESULT_VARIABLE TASKLIST_RESULT
)

string(FIND "${TASKLIST_OUTPUT}${TASKLIST_ERROR}" "${EXE_NAME}" EXE_IS_RUNNING)

if(NOT EXE_IS_RUNNING EQUAL -1)
    message(FATAL_ERROR
        "${EXE_NAME} is still running.\n"
        "Windows locks a running executable, so the linker cannot rewrite it and the build stops.\n"
        "Close ${EXE_NAME}, then build again.")
endif()
