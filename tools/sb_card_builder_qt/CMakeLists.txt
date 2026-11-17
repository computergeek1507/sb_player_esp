cmake_minimum_required(VERSION 3.21)
project(sb_card_builder VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Qt 6: set Qt6_DIR or CMAKE_PREFIX_PATH to the kit, e.g. D:/Qt/6.8.3/msvc2022_64
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

# Fetched at configure time: zstd (xLights compresses sequences with it by default),
# nlohmann/json for the card's JSON files and pugixml for xlights_networks.xml.
include(FetchContent)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.7
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake)
FetchContent_MakeAvailable(zstd)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE)
FetchContent_Declare(pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG v1.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(nlohmann_json pugixml)

add_library(sbcore STATIC
    src/core/json.h
    src/core/networks.cpp src/core/networks.h
    src/core/fseq.cpp src/core/fseq.h
    src/core/card.cpp src/core/card.h)
target_include_directories(sbcore PUBLIC src ${zstd_SOURCE_DIR}/lib)
target_link_libraries(sbcore PUBLIC Qt6::Core nlohmann_json::nlohmann_json PRIVATE libzstd_static pugixml::pugixml)

# Command line tool: same options as tools/sb_card_builder.py
add_executable(sb_card_cli src/cli.cpp)
target_link_libraries(sb_card_cli PRIVATE sbcore)

add_library(sbgui STATIC src/mainwindow.cpp src/mainwindow.h)
target_link_libraries(sbgui PUBLIC sbcore Qt6::Widgets)

# Icon (drawn by tools/icon/make_icon.py): app.rc for the .exe, app.png for the window
qt_add_executable(sb_card_builder WIN32 src/main.cpp $<$<PLATFORM_ID:Windows>:res/app.rc>)
qt_add_resources(sb_card_builder "icon" PREFIX "/" FILES res/app.png)
target_link_libraries(sb_card_builder PRIVATE sbgui)

# Drives the window without a person (see tests/selftest.cpp)
add_executable(sb_card_selftest tests/selftest.cpp)
target_link_libraries(sb_card_selftest PRIVATE sbgui)

# Copy the Qt DLLs next to the executables so they run from the build folder.
if(WIN32 AND TARGET Qt6::windeployqt)
    add_custom_command(TARGET sb_card_builder POST_BUILD
        COMMAND Qt6::windeployqt --no-translations --no-system-d3d-compiler --no-opengl-sw
                --no-compiler-runtime $<TARGET_FILE:sb_card_builder>
        COMMENT "Deploying Qt runtime")
endif()
