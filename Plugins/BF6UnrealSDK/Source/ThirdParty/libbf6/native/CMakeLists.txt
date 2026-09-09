cmake_minimum_required(VERSION 3.20)
project(BF6ReaderForUnreal LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_library(bf6_core SHARED
src/bf6_core.cpp
    src/oodle.cpp
    src/cas.cpp
    src/dbobject.cpp
    src/caslocator.cpp
    src/toc.cpp
    src/bundle.cpp
    src/source.cpp
    src/armory.cpp
    src/meshset.cpp
    src/placeables.cpp
    src/types.cpp
    src/ebx.cpp
    src/walk.cpp
    src/terrain.cpp
    src/texture.cpp
    src/depot.cpp
    src/destruction.cpp
    src/decals.cpp
    src/splat.cpp
    src/terrainlayers.cpp
    src/terrainstatic.cpp
    src/terrainjoin.cpp
    src/terrainshader.cpp
    src/terrainmask.cpp
    src/terraintextures.cpp
    src/terrainpage.cpp
    src/terraincomposite.cpp
    src/groundsplat.cpp
    src/velighting.cpp
    src/levellights.cpp
    src/lightingzones.cpp
    src/fx.cpp
    src/expression_graph.cpp
    src/expression_registry.cpp
    src/expression_vm.cpp
)
target_include_directories(bf6_core PUBLIC include PRIVATE src)
target_compile_definitions(bf6_core PRIVATE BF6_BUILDING_DLL)
target_compile_options(bf6_core PRIVATE /W4)

target_sources(bf6_core PRIVATE ui_runtime/installed_sound_wave.cpp)
target_include_directories(bf6_core PRIVATE ui_runtime)
target_link_libraries(bf6_core PRIVATE xaudio2 ole32)
add_executable(pose_probe pose_probe.cpp)
target_link_libraries(pose_probe PRIVATE bf6_core)
