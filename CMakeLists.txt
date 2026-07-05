cmake_minimum_required(VERSION 3.22)
project(Tenganisha VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

# ---------------------------------------------------------------- JUCE
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------- Eigen (header-only)
FetchContent_Declare(eigen
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG        3.4.0
    GIT_SHALLOW    TRUE)
FetchContent_Populate(eigen)   # header-only; skip Eigen's own CMake

# ---------------------------------------------------------------- demucs.cpp (inference backend)
FetchContent_Declare(demucscpp
    GIT_REPOSITORY https://github.com/sevagh/demucs.cpp.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE)
FetchContent_Populate(demucscpp)  # we compile its sources ourselves

add_library(demucs_inference STATIC
    ${demucscpp_SOURCE_DIR}/src/model_load.cpp
    ${demucscpp_SOURCE_DIR}/src/model_apply.cpp
    ${demucscpp_SOURCE_DIR}/src/model_inference.cpp
    ${demucscpp_SOURCE_DIR}/src/dsp.cpp
    ${demucscpp_SOURCE_DIR}/src/encdec.cpp
    ${demucscpp_SOURCE_DIR}/src/layers.cpp
    ${demucscpp_SOURCE_DIR}/src/lstm.cpp
    ${demucscpp_SOURCE_DIR}/src/crosstransformer.cpp)

target_include_directories(demucs_inference PUBLIC
    ${demucscpp_SOURCE_DIR}/src
    ${eigen_SOURCE_DIR})

# Fast math for inference, but keep it local to this target.
if(MSVC)
    target_compile_options(demucs_inference PRIVATE /O2 /fp:fast)
else()
    target_compile_options(demucs_inference PRIVATE -O3 -ffast-math -fno-finite-math-only)
endif()

find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
    target_link_libraries(demucs_inference PUBLIC OpenMP::OpenMP_CXX)
endif()

# ---------------------------------------------------------------- Plugin
juce_add_plugin(Tenganisha
    COMPANY_NAME        "DN"
    PLUGIN_MANUFACTURER_CODE Dnkt
    PLUGIN_CODE         Tgsh
    FORMATS             VST3 AU Standalone
    IS_SYNTH            FALSE
    NEEDS_MIDI_INPUT    FALSE
    NEEDS_MIDI_OUTPUT   FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PRODUCT_NAME        "Tenganisha")

target_sources(Tenganisha PRIVATE
    src/PluginProcessor.cpp
    src/SeparationEngine.cpp
    src/PluginEditor.cpp)

target_compile_definitions(Tenganisha PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Tenganisha
    PRIVATE
        demucs_inference
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
