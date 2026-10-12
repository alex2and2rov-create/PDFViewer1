cmake_minimum_required(VERSION 3.20)
project(PDFViewer VERSION 1.0.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# PDF Viewer — FFGL source plugin for Resolume Arena / Avenue 7.x
#
#   Windows : cmake -B build -A x64            && cmake --build build --config Release
#   macOS   : cmake -B build -G Xcode          && cmake --build build --config Release
#             (builds a universal arm64 + x86_64 bundle)
#
# Output lands in build/dist/ ready to copy to Documents/Resolume Arena/Extra Effects.
# Dependencies (FFGL SDK, GLEW, PDFium binaries) are downloaded automatically.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "" FORCE)
endif()
if(MSVC)
    # Static CRT: no VC++ redistributable needed on show machines.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)

# --- FFGL SDK -------------------------------------------------------------
set(FFGL_SDK_DIR "" CACHE PATH "Path to a checkout of github.com/resolume/ffgl (downloaded if empty)")
if(NOT FFGL_SDK_DIR)
    FetchContent_Declare(ffgl GIT_REPOSITORY https://github.com/resolume/ffgl.git GIT_TAG master GIT_SHALLOW TRUE
        SOURCE_SUBDIR _populate_only_)
    FetchContent_MakeAvailable(ffgl)
    set(FFGL_SDK_DIR ${ffgl_SOURCE_DIR})
endif()
set(FFGL_LIB ${FFGL_SDK_DIR}/source/lib)

# --- GLEW (Windows/Linux only; macOS uses the system OpenGL framework) ------
if(NOT APPLE)
    set(GLEW_DIR "" CACHE PATH "Path to an extracted glew-2.2.0 source release (downloaded if empty)")
    if(NOT GLEW_DIR)
        FetchContent_Declare(glew URL https://github.com/nigels-com/glew/releases/download/glew-2.2.0/glew-2.2.0.tgz
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR _populate_only_)
        FetchContent_MakeAvailable(glew)
        set(GLEW_DIR ${glew_SOURCE_DIR})
    endif()
endif()

# --- PDFium prebuilt binaries (github.com/bblanchon/pdfium-binaries) --------
if(WIN32)
    set(_pdfium_pkg pdfium-win-x64)
elseif(APPLE)
    set(_pdfium_pkg pdfium-mac-univ)
else()
    set(_pdfium_pkg pdfium-linux-x64)
endif()
set(PDFIUM_DIR "" CACHE PATH "Path to an extracted ${_pdfium_pkg} package (downloaded if empty)")
if(NOT PDFIUM_DIR)
    FetchContent_Declare(pdfium URL https://github.com/bblanchon/pdfium-binaries/releases/latest/download/${_pdfium_pkg}.tgz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR _populate_only_)
    FetchContent_MakeAvailable(pdfium)
    set(PDFIUM_DIR ${pdfium_SOURCE_DIR})
endif()
if(WIN32)
    set(PDFIUM_BINARY ${PDFIUM_DIR}/bin/pdfium.dll)
elseif(APPLE)
    set(PDFIUM_BINARY ${PDFIUM_DIR}/lib/libpdfium.dylib)
else()
    set(PDFIUM_BINARY ${PDFIUM_DIR}/lib/libpdfium.so)
endif()

# --- Plugin ---------------------------------------------------------------
set(PLUGIN_SOURCES
    src/FFGLPdfViewer.cpp
    src/FFGLPdfViewer.h
    src/PdfRenderer.cpp
    src/PdfRenderer.h
    src/PdfiumLoader.cpp
    src/PdfiumLoader.h
    ${FFGL_LIB}/FFGLSDK.cpp
)
if(NOT APPLE)
    list(APPEND PLUGIN_SOURCES ${GLEW_DIR}/src/glew.c)
endif()
if(WIN32)
    list(APPEND PLUGIN_SOURCES src/PDFViewer.def)
endif()

if(APPLE)
    add_library(PDFViewer MODULE ${PLUGIN_SOURCES})
    set_target_properties(PDFViewer PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION bundle
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/src/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME PDFViewer
        MACOSX_BUNDLE_GUI_IDENTIFIER com.pdfviewer.ffgl
        XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH NO)
    target_link_libraries(PDFViewer PRIVATE "-framework OpenGL" "-framework Cocoa")
else()
    add_library(PDFViewer SHARED ${PLUGIN_SOURCES})
    set_target_properties(PDFViewer PROPERTIES PREFIX "")
    target_compile_definitions(PDFViewer PRIVATE GLEW_STATIC GLEW_NO_GLU)
    target_include_directories(PDFViewer PRIVATE ${GLEW_DIR}/include)
endif()

target_include_directories(PDFViewer PRIVATE ${FFGL_LIB} ${PDFIUM_DIR}/include src)

if(WIN32)
    target_link_libraries(PDFViewer PRIVATE opengl32)
    target_compile_definitions(PDFViewer PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    if(MINGW)
        # Self-contained DLL: no libstdc++/libc++/libwinpthread DLLs next to it.
        target_link_options(PDFViewer PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
elseif(NOT APPLE)
    # Linux is only a compile check (Resolume has no Linux version); GL symbols stay unresolved.
    target_link_libraries(PDFViewer PRIVATE dl pthread)
endif()

# --- Collect a ready-to-install folder in build/dist ----------------------
set(DIST_DIR ${CMAKE_BINARY_DIR}/dist)
if(WIN32)
    add_custom_command(TARGET PDFViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:PDFViewer> ${DIST_DIR}/PDFViewer.dll
        # Unique file name on purpose — see src/PdfiumLoader.h
        COMMAND ${CMAKE_COMMAND} -E copy ${PDFIUM_BINARY} ${DIST_DIR}/PDFViewer_pdfium.dll)
elseif(APPLE)
    add_custom_command(TARGET PDFViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:PDFViewer>/Frameworks
        COMMAND ${CMAKE_COMMAND} -E copy ${PDFIUM_BINARY} $<TARGET_BUNDLE_CONTENT_DIR:PDFViewer>/Frameworks/libpdfium.dylib
        COMMAND codesign --force --sign - $<TARGET_BUNDLE_CONTENT_DIR:PDFViewer>/Frameworks/libpdfium.dylib
        COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:PDFViewer>
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_BUNDLE_DIR:PDFViewer> ${DIST_DIR}/PDFViewer.bundle)
else()
    add_custom_command(TARGET PDFViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${PDFIUM_BINARY} $<TARGET_FILE_DIR:PDFViewer>/libpdfium.so)
endif()

# --- Optional: offline render test (no GPU / host needed) -----------------
option(PDFVIEWER_BUILD_TEST "Build the command-line render test" OFF)
if(PDFVIEWER_BUILD_TEST)
    add_executable(pdfviewer_test tests/render_test.cpp src/PdfRenderer.cpp src/PdfiumLoader.cpp)
    target_include_directories(pdfviewer_test PRIVATE ${PDFIUM_DIR}/include src)
    if(NOT WIN32)
        target_link_libraries(pdfviewer_test PRIVATE dl pthread)
    endif()
    add_custom_command(TARGET pdfviewer_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${PDFIUM_BINARY} $<TARGET_FILE_DIR:pdfviewer_test>/)
endif()
