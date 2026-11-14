cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# CMake 4 removed compatibility with projects that declare cmake_minimum_required(VERSION < 3.5),
# and several vendored dependencies (glad, glfw, ...) still declare 3.0. Setting the accepted
# minimum here - rather than only as a preset cache variable - means it also applies to CMake's
# automatic re-configure during a build, which any CMakeLists change triggers.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

project(Comet LANGUAGES C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    # Upstream needs CMake 3.16, which enables CMP0091: dependencies that default
    # CMAKE_MSVC_RUNTIME_LIBRARY (yaml-cpp, curl) would otherwise build with the static CRT and
    # then fail to link against Comet's /MD(d) objects. Pin the dynamic CRT for everything.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    # glfw owns its own switch: off (the default) means static CRT for glfw3.lib.
    set(USE_MSVC_RUNTIME_LIBRARY_DLL ON CACHE BOOL "" FORCE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP2")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG")
endif()

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
find_package(OpenGL REQUIRED)
set(FETCHCONTENT_QUIET OFF)

set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
else()
    set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
endif()

include(FetchContent)

FetchContent_Declare(
    hwy
    GIT_REPOSITORY https://github.com/google/highway.git
    GIT_TAG 1.4.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(hwy)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.3.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
    nfd
    GIT_REPOSITORY https://github.com/mlabbe/nativefiledialog.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(nfd)

FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(yaml-cpp)

FetchContent_Declare(
    glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG v0.1.36
)

FetchContent_MakeAvailable(glad)

FetchContent_Declare(
	glm
	GIT_REPOSITORY	https://github.com/g-truc/glm.git
	GIT_TAG 1.0.3
)

FetchContent_MakeAvailable(glm)

FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG master
)

FetchContent_MakeAvailable(miniz)

FetchContent_Declare(
    rtmidi
    GIT_REPOSITORY https://github.com/thestk/rtmidi.git
    GIT_TAG master
)

FetchContent_MakeAvailable(rtmidi)

FetchContent_Declare(
    imgui_knobs
    GIT_REPOSITORY https://github.com/altschuler/imgui-knobs.git
    GIT_TAG main
)

FetchContent_MakeAvailable(imgui_knobs)

set(_prev_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    curl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG curl-8_16_0
)

FetchContent_MakeAvailable(curl)
set(BUILD_SHARED_LIBS ${_prev_shared} CACHE BOOL "" FORCE)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
)

FetchContent_MakeAvailable(json)

# --- Archives and compressed files -------------------------------------------
# libarchive reads 7z, xz, zip, gz, bz2, zst, lz4, rar and tar. Each codec is built from source here
# too, so nothing has to be installed on the machine for a MIDI inside a .7z or .xz to load.
set(_prev_archive_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
    GIT_SHALLOW TRUE
)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zlib)

FetchContent_Declare(
    xz
    GIT_REPOSITORY https://github.com/tukaani-project/xz.git
    GIT_TAG v5.6.3
    GIT_SHALLOW TRUE
)
set(XZ_TOOL_XZ OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SCRIPTS OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SYMLINKS OFF CACHE BOOL "" FORCE)
set(XZ_NLS OFF CACHE BOOL "" FORCE)
set(XZ_DOC OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(xz)

FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.6
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake
)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zstd)

FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG v1.10.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake
)
set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(lz4)

FetchContent_Declare(
    bzip2
    URL https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
    URL_HASH SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
)
FetchContent_MakeAvailable(bzip2)
add_library(comet_bzip2 STATIC
    ${bzip2_SOURCE_DIR}/blocksort.c
    ${bzip2_SOURCE_DIR}/huffman.c
    ${bzip2_SOURCE_DIR}/crctable.c
    ${bzip2_SOURCE_DIR}/randtable.c
    ${bzip2_SOURCE_DIR}/compress.c
    ${bzip2_SOURCE_DIR}/decompress.c
    ${bzip2_SOURCE_DIR}/bzlib.c
)
target_include_directories(comet_bzip2 PUBLIC ${bzip2_SOURCE_DIR})

# libarchive probes for its codecs with try_compile(), which cannot link imported target names, so
# its detection is switched off and the codecs are wired in directly afterwards instead.
set(ENABLE_ZLIB OFF CACHE BOOL "" FORCE)
set(ENABLE_BZip2 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZMA OFF CACHE BOOL "" FORCE)
set(ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
set(ENABLE_LZ4 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZO OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBB2 OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(ENABLE_NETTLE OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)
set(ENABLE_EXPAT OFF CACHE BOOL "" FORCE)
set(ENABLE_ICONV OFF CACHE BOOL "" FORCE)
set(ENABLE_ACL OFF CACHE BOOL "" FORCE)
set(ENABLE_XATTR OFF CACHE BOOL "" FORCE)
set(ENABLE_TAR OFF CACHE BOOL "" FORCE)
set(ENABLE_CPIO OFF CACHE BOOL "" FORCE)
set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
set(ENABLE_UNZIP OFF CACHE BOOL "" FORCE)
set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    libarchive
    GIT_REPOSITORY https://github.com/libarchive/libarchive.git
    GIT_TAG v3.7.7
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libarchive)

target_include_directories(archive_static PRIVATE
    ${zlib_SOURCE_DIR}
    ${zlib_BINARY_DIR}
    ${bzip2_SOURCE_DIR}
    ${xz_SOURCE_DIR}/src/liblzma/api
    ${zstd_SOURCE_DIR}/lib
    ${lz4_SOURCE_DIR}/lib
)
target_compile_definitions(archive_static PRIVATE
    HAVE_LIBZ=1
    HAVE_ZLIB_H=1
    HAVE_LIBBZ2=1
    HAVE_BZLIB_H=1
    HAVE_LIBLZMA=1
    HAVE_LZMA_H=1
    HAVE_LIBZSTD=1
    HAVE_ZSTD_H=1
    HAVE_LIBLZ4=1
    HAVE_LZ4_H=1
    HAVE_LZ4HC_H=1
    LZMA_API_STATIC
    # MSVC provides these as intrinsics; libarchive's configure check misses them there, and its
    # fallback definitions then collide with the CRT (C2169).
    HAVE_WCSCPY=1
    HAVE_WCSLEN=1
    HAVE_WCSCMP=1
)
# The codec libraries are linked into Comet instead of archive_static: adding link libraries to a
# target from another directory needs CMP0079, and the final link resolves them just the same.

set(BUILD_SHARED_LIBS ${_prev_archive_shared} CACHE BOOL "" FORCE)

file(GLOB IMGUI_SRC
    ${imgui_SOURCE_DIR}/*.cpp
)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

add_library(nfd STATIC
    ${nfd_SOURCE_DIR}/src/nfd_common.c)

if (WIN32)
    target_sources(nfd PRIVATE ${nfd_SOURCE_DIR}/src/nfd_win.cpp)
elseif(APPLE)
    target_sources(nfd PRIVATE ${nfd_SOURCE_DIR}/src/nfd_cocoa.m)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    target_sources(nfd PRIVATE ${nfd_SOURCE_DIR}/src/nfd_gtk.c)
    target_include_directories(nfd PRIVATE ${GTK3_INCLUDE_DIRS})
    target_link_libraries(nfd PRIVATE ${GTK3_LIBRARIES})
endif()

target_include_directories(nfd PUBLIC
    ${nfd_SOURCE_DIR}/src/include
)

add_executable(Comet
    "Comet.cpp"
    App/MainWindow.cpp
    App/MIDIApp.cpp
    MIDI/SavedInputStream.cpp
    MIDI/ProgressInputStream.cpp
    Inter.hpp
    FiraCode.hpp
    icon32.hpp
    MIDI/Diagnosis/ADiagnosis.h
    MIDI/Diagnosis/ADiagnosis.cpp
    MIDI/Diagnosis/BasicDiagnosis.cpp
    App/Diagnosis/DiagnosisDialog.cpp
    App/Diagnosis/DiagnosisDialog.h
    App/UI/Dialog.h
    stb_image_write.h
    stb_image.cpp
    stb_image_write.cpp
    tiny_obj_loader.h
    ColorPalette/ColorPaletteList.h
    MIDI/Audio/MaestroEngine.cpp
    "MIDI/Diagnosis/DiagnosisField.cpp" "Utils.cpp" "MIDI/Diagnosis/Diagnoses.h" "MIDI/Diagnosis/Diagnoses.cpp" "App/Diagnosis/DiagnosisPanel.h" "App/Diagnosis/DiagnosisPanel.cpp" "App/Diagnosis/DiagnosisFieldView.h" "App/Diagnosis/DiagnosisFieldView.cpp" "IntervalThread.h" "IntervalThread.cpp" "MIDI/AbstractMIDILoader.h" "MIDI/MIDISequence.h" "MIDI/Events/MIDIMessageEvent.h" "MIDI/Events/TrackEvent.h" "MIDI/Events/TempoEvent.h" "MIDI/Events/NoteEvent.h" "MIDI/MIDISequence.cpp" "MIDI/Diagnosis/BlackMIDIDiagnosis.h" "MIDI/Diagnosis/BlackMIDIDiagnosis.cpp" "MIDI/Diagnosis/DiagnosisMessage.h" "MIDI/Diagnosis/DiagnosisException.h" "IO/BufferedByteReader.h" "IO/BufferedByteReader.cpp" "IO/InputStream.h" "IO/InputStream.cpp" "ResourcePack/ResourcePack.h" "Render/RenderUtils.h" "ResourcePack/ResourcePackInfo.h" "ResourcePack/SignatureState.h" "ResourcePack/ResourcePack.cpp" "ResourcePack/FolderResourcePack.h" "ResourcePack/FolderResourcePack.cpp" "Config/ConfigSection.h" "Config/ConfigSection.cpp" "ResourcePack/ResourcePackVerifier.h" "ResourcePack/DefaultResourcePack.h" "ResourcePack/DefaultResourcePack.cpp" "Config/MIDIPlayerConfig.h" "Render/ColorAsset.h" "Render/GPUImage.h" "Render/GPUImage.cpp" "Render/ColorAsset.cpp" "Render/MIDIRenderer.h" "Render/MIDIRenderer.cpp" "Render/RenderEngine/Shaders.h" "Render/RenderEngine/Shaders.cpp" "Render/RenderEngine/Buffers.h" "Render/RenderEngine/Buffers.cpp" "Render/Renderer/QuadDrawer.h" "Render/Renderer/PrimitiveDrawer.h" "Render/Renderer/PrimitiveDrawer.cpp" "MIDI/MIDILoader.h" "MIDI/MIDILoader.cpp" "App/Dialog/LoadingDialog.h" "App/Dialog/LoadingDialog.cpp" "MIDI/Sequence/SequenceFuncs.h" "MIDI/Sequence/SequenceFuncs.cpp" "Render/RenderView.h" "MIDI/MIDIDefs.h" "MIDI/Timer/MIDITimer.h" "MIDI/Timer/MIDITimer.cpp" "MIDI/TempoMap.h" "MIDI/TempoMap.cpp" "App/UI/NavigationBar.h" "App/UI/NavigationBar.cpp" "Render/NoteCounter/NoteCounterInfo.h" "Render/NoteCounter/NoteCounterRenderer.h" "Render/NoteCounter/NoteCounterStyles.h" "Render/NoteCounter/NoteCounterRenderer.cpp" "Render/Renderer/PrimitiveShaders.h" "Render/BlurredQuadRenderer.h" "App/Dialog/RenderVideoDialog.h" "App/Dialog/RenderVideoDialog.cpp" "App/VideoRender/RenderSettings.h" "FFmpeg/FFmpegPipe.h" "Render/NoteCounter/NoteCounterInfo.cpp" "App/Dialog/SettingsDialog.h" "App/Dialog/SettingsDialog.cpp" "ResourcePack/ZipResourcePack.h" "ResourcePack/ZipResourcePack.cpp" "ResourcePack/ResourcePackList.h" "ResourcePack/ResourcePackList.cpp" "FFmpeg/FFmpegGPU.h" "FFmpeg/FFmpegGPU.cpp" "FFmpeg/FFmpegCommandBuilder.h" "FFmpeg/FFmpegCommandBuilder.cpp" "App/Fonts.h" "App/Fonts.cpp" "ColorPalette/ColorPaletteList.cpp" "MIDI/Audio/MIDIOut.h" "MIDI/Audio/MIDIOut.cpp" "MIDI/Audio/AudioThread.h" "MIDI/Audio/AudioThread.cpp" "Config/MIDIPlayerConfig.cpp" "MIDI/MultithreadedMIDILoader.h" "MIDI/MultithreadedMIDILoader.cpp" "Render/MIDIRendererEnhanced.h" "Render/MIDIRendererEnhanced.cpp" "Render/Renderer/AbstractMIDIRenderer.cpp" "Render/RenderEngine/OBJMesh.h" "Render/RenderEngine/OBJMesh.cpp" "App/Models.h" "App/Models.cpp" "App/UI/Themes/Themes.h" "App/UI/Themes/Themes.cpp" "Render/MIDIRendererMIDITrail.h" "Render/MIDIRendererMIDITrail.cpp" "MIDI/Events/TimeSignatureEvent.h" "Render/MIDIRendererChannels.h" "Render/MIDIRendererChannels.cpp" "Render/MIDIRendererPFA.h" "Render/MIDIRendererPFA.cpp" "Render/MIDIRendererVelocities.h" "Render/MIDIRendererVelocities.cpp" "Render/MIDIRendererEnhancedShaders.h" "MIDI/Audio/MIDIAudio.h" "MIDI/Audio/MIDIAudio.cpp" "MIDI/Audio/PrerenderEngine/BASSMIDI.h" "MIDI/Audio/PrerenderEngine/BASSMIDI.cpp" "MIDI/Audio/PrerenderEngine/PrerenderedEngine.h" "MIDI/Audio/PrerenderEngine/PrerenderedEngine.cpp" "MIDI/Audio/SoundfontList.cpp" "App/FontList.h" "App/FontList.cpp" "App/Dialog/DialogMacros.h" "Concurrency/ThreadPool.h" "Concurrency/ThreadPool.cpp" "Render/MIDIRendererSynthesia.h" "Render/MIDIRendererSynthesia.cpp" "Render/RenderEngine/TextureArray.h" "Render/RenderEngine/TextureArray.cpp" "Render/Renderer/NoteCuller.h" "Render/Renderer/NoteCuller.cpp" "RLEvec.h" "Render/MIDIRendererCounter.cpp" "Render/NoteCounter/AbstractOverlayRenderer.h" "Render/NoteCounter/AbstractOverlayRenderer.cpp" "Render/NoteCounter/StatsOverlayRenderer.h" "Render/NoteCounter/StatsOverlayRenderer.cpp" "App/UI/Widgets/TimeRange.h" "App/UI/Widgets/TimeRange.cpp" "App/UI/Widgets/ThinSlider.h" "App/UI/Widgets/ThinSlider.cpp" "UpdateChecker.h" "UpdateChecker.cpp" "UpdateBuilder.h" "UpdateBuilder.cpp" "App/Dialog/MessageDialog.h" "App/Dialog/MessageDialog.cpp" "MIDI/CJCMCGChart.h" "MIDI/CJCMCGChart.cpp" "Render/NoteCounter/NoteMultiplier.h" "Render/NoteCounter/GraphOverlayRenderer.h" "Render/NoteCounter/GraphOverlayRenderer.cpp" "Render/NoteCounter/CounterChartScan.h" "App/Dialog/UpdateDialog.h" "App/Dialog/UpdateDialog.cpp" "Telemetry/CometLog.h" "Telemetry/CometLog.cpp" "Telemetry/CrashReporter.h" "Telemetry/CrashReporter.cpp")

target_precompile_headers(Comet PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/pch.h"
)

set_source_files_properties(
    ResourcePack/FolderResourcePack.cpp
    PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON
)

 target_include_directories(Comet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
 )

 target_sources(Comet PRIVATE
    ${imgui_knobs_SOURCE_DIR}/imgui-knobs.cpp
)

target_sources(Comet PRIVATE
    IO/MappedFile.h
    IO/MappedFile.cpp
    IO/ArchiveLoader.h
    IO/ArchiveLoader.cpp
)

# libarchive's headers import their functions as DLL symbols unless this is set for a static build.
target_compile_definitions(Comet PRIVATE LIBARCHIVE_STATIC)
target_include_directories(Comet PRIVATE ${libarchive_SOURCE_DIR}/libarchive)

  target_include_directories(Comet PRIVATE
    ${imgui_knobs_SOURCE_DIR}
 )

target_link_libraries(Comet PRIVATE
    hwy
    imgui
    nfd
    glad
    glfw
    OpenGL::GL
    yaml-cpp
    glm::glm
    miniz
    rtmidi
    CURL::libcurl
    nlohmann_json::nlohmann_json
    archive_static
    zlibstatic
    comet_bzip2
    liblzma
    libzstd_static
    lz4_static
)

if(WIN32)
    target_include_directories(Comet PRIVATE
        ${CMAKE_SOURCE_DIR}/external/bass
        ${CMAKE_SOURCE_DIR}/external/bassmidi
    )

    # Link directly to the import .lib files via absolute paths
    target_link_libraries(Comet PRIVATE
        "${CMAKE_SOURCE_DIR}/external/bass/bass.lib"
        "${CMAKE_SOURCE_DIR}/external/bassmidi/bassmidi.lib"
    )

    add_custom_command(TARGET Comet POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/external/bass/bass.dll"
            $<TARGET_FILE_DIR:Comet>

        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/external/bassmidi/bassmidi.dll"
            $<TARGET_FILE_DIR:Comet>
    )
endif()

# A Release build imports MSVCP140.dll / VCRUNTIME140.dll from the compiler's release CRT, so a
# machine without the VC++ Redistributable fails to launch with "...dll was not found". Shipping
# those DLLs beside the exe lets the build run on a clean Windows install with nothing installed.
# A Debug build cannot be fixed this way at all: its ...140D.dll imports come from the debug CRT,
# which ships only inside Visual Studio and is not redistributable - that is what broke the copy
# that was sent out.
#
# The build already needs a Visual Studio environment (the preset invokes bare cl.exe with Ninja),
# so VCToolsRedistDir - which vcvars sets - is the reliable way to find the redist.
if(WIN32 AND MSVC AND CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_vc_crt_dirs "")

    if(DEFINED ENV{VCToolsRedistDir})
        file(GLOB _vc_crt_dirs "$ENV{VCToolsRedistDir}/x64/Microsoft.VC*.CRT")
    endif()

    if(NOT _vc_crt_dirs)
        message(WARNING
            "Could not locate the Visual C++ Redistributable (VCToolsRedistDir is unset), so the "
            "CRT DLLs will not be deployed beside Comet.exe. Recipients may need the "
            "VC++ Redistributable installed.")
    else()
        file(GLOB _vc_crt_dlls "${_vc_crt_dirs}/*.dll")

        add_custom_command(TARGET Comet POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_vc_crt_dlls}
                $<TARGET_FILE_DIR:Comet>
            COMMENT "Deploying the release CRT beside Comet.exe (no VC++ Redistributable needed)"
        )
    endif()
endif()

# <Windows.h> defines min/max as function-like macros, which mangle any
# std::min()/std::max() call in a TU that (transitively) includes it.
# NOMINMAX suppresses those macros; nothing in Comet relies on them now.
if(WIN32)
    target_compile_definitions(Comet PRIVATE NOMINMAX)
endif()

# Crash reports name the configuration that produced them, which is the first thing
# you want to know about a crash you didn't build yourself.
if(CMAKE_BUILD_TYPE)
    target_compile_definitions(Comet PRIVATE COMET_BUILD_CONFIG="${CMAKE_BUILD_TYPE}")
else()
    target_compile_definitions(Comet PRIVATE COMET_BUILD_CONFIG="unknown")
endif()

# SymFromAddr/SymGetLineFromAddr64 come from DbgHelp; the crash reporter links it directly
# as well through a #pragma comment, so standalone builds of the same sources work too.
if(WIN32)
    target_link_libraries(Comet PRIVATE dbghelp)
endif()

# libstdc++ parallel algorithms (std::execution) require linking Intel TBB.
# ${CMAKE_DL_LIBS} provides dlopen/dlsym used to load OmniMIDI at runtime.
if(NOT WIN32)
    find_package(TBB QUIET)
    if(TBB_FOUND)
        target_link_libraries(Comet PRIVATE TBB::tbb)
    else()
        target_link_libraries(Comet PRIVATE tbb)
    endif()
    target_link_libraries(Comet PRIVATE ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    target_compile_options(Comet PRIVATE
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/GL>
        $<$<CONFIG:Release>:/Ot>
        $<$<CONFIG:Release>:/Ob3>
    )

    # target_link_options(Comet PRIVATE
    #     $<$<CONFIG:Release>:/LTCG>
    # )
else()
    target_compile_options(Comet PRIVATE
        $<$<CONFIG:Release>:-O3>
    )
endif()

if (WIN32)
    target_sources(Comet PRIVATE Comet.rc)

    # Windows locks a running .exe, so a rebuild while the app is open otherwise fails with a
    # cryptic LNK1104 / "Permission denied" instead of telling you what to do about it.
    add_custom_command(TARGET Comet PRE_LINK
        COMMAND ${CMAKE_COMMAND}
            -DEXE=$<TARGET_FILE:Comet>
            -P "${CMAKE_SOURCE_DIR}/cmake/CheckExeNotRunning.cmake"
    )
endif()

add_custom_command(
    TARGET Comet POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:Comet>/assets
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET Comet PROPERTY CXX_STANDARD 20)
endif()

# The windowed merge replaced a heap that combined every track at once; this keeps the old one
# honest by checking both produce the same per-key note order.
enable_testing()
add_executable(CometMergeEquivalence
    Tests/SequenceMergeEquivalence.cpp
    MIDI/Sequence/SequenceFuncs.cpp
    MIDI/TempoMap.cpp
    MIDI/MIDISequence.cpp
)
target_include_directories(CometMergeEquivalence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(CometMergeEquivalence PRIVATE yaml-cpp)
set_property(TARGET CometMergeEquivalence PROPERTY CXX_STANDARD 20)
add_test(NAME merge_equivalence COMMAND CometMergeEquivalence)
