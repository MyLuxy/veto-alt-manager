cmake_minimum_required(VERSION 3.21)
project(VetoAltManager VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_WIN32_EXECUTABLE ON)

# Force static library lookup before shared
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".lib")

find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick QuickControls2)

# Qt6's cmake config adds system libs (libtiff.a, libcrypto.a, etc.) as absolute-path
# imported targets but doesn't include their transitive static deps.
# We patch each imported target's INTERFACE_LINK_LIBRARIES so the deps follow
# the library in the final link command.
if(TARGET TIFF::TIFF)
    set_property(TARGET TIFF::TIFF APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "jbig;zstd;lzma;Lerc;jpeg;deflate;bz2;z")
endif()
if(TARGET Freetype::Freetype)
    set_property(TARGET Freetype::Freetype APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "bz2;harfbuzz")
endif()
if(TARGET OpenSSL::Crypto)
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "crypt32")
endif()
if(TARGET Qt6::QTiffPlugin)
    set_property(TARGET Qt6::QTiffPlugin APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "TIFF::TIFF")
endif()

qt_add_executable(VetoAltManager
    src/main_qt.cpp
    src/AltManagerBridge.cpp
    resources.rc
)

qt_add_qml_module(VetoAltManager
    URI "Veto"
    VERSION 1.0
    QML_FILES
        qml/main.qml
        qml/components/VapeButton.qml
    RESOURCES
        img/logo.png
)

qt_import_qml_plugins(VetoAltManager)
qt_finalize_executable(VetoAltManager)

target_link_libraries(VetoAltManager PRIVATE
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::QuickControls2
    ws2_32 psapi advapi32 dwmapi
    -Wl,--start-group
    tiff jbig zstd lzma Lerc jpeg deflate
    freetype harfbuzz harfbuzz-subset bz2 png16
    brotlidec brotlicommon
    graphite2 glib-2.0 intl
    pcre2-16 pcre2-8
    webp webpdemux webpmux sharpyuv
    ssl crypto crypt32
    z b2
    -Wl,--end-group
    ole32 winmm shlwapi uuid rpcrt4 dwrite usp10
)

target_link_options(VetoAltManager PRIVATE -static -static-libgcc -static-libstdc++)

target_include_directories(VetoAltManager PRIVATE src)
