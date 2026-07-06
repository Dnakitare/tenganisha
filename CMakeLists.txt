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

# ---------------------------------------------------------------- demucs.cpp (inference backend)
# Its vendor/eigen submodule (Eigen master pin) comes along in the clone; the
# demucs.cpp headers need that revision — Eigen 3.4.0 is too old for them.
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

# SYSTEM: their headers are compiled into our TUs (SeparationEngine.cpp) and
# must not trip juce_recommended_warning_flags there.
target_include_directories(demucs_inference SYSTEM PUBLIC
    ${demucscpp_SOURCE_DIR}/src
    ${demucscpp_SOURCE_DIR}/vendor/eigen)

# Fast math for inference, but keep it local to this target.
if(MSVC)
    target_compile_options(demucs_inference PRIVATE /O2 /fp:fast)
else()
    target_compile_options(demucs_inference PRIVATE -O3 -ffast-math -fno-finite-math-only)
endif()

# Threaded Eigen GEMM. Apple Clang has no native OpenMP; point FindOpenMP at
# Homebrew's libomp. Note the dylib dependency this creates on brew's libomp —
# fine for local dev, wrong for distribution (see TENGANISHA_OPENMP below).
option(TENGANISHA_OPENMP "Thread Eigen GEMM with OpenMP" ON)
if(NOT TENGANISHA_OPENMP)
    set(OpenMP_CXX_FOUND FALSE)
else()
find_package(OpenMP QUIET)
if(NOT OpenMP_CXX_FOUND AND APPLE)
    execute_process(COMMAND brew --prefix libomp
        OUTPUT_VARIABLE LIBOMP_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(LIBOMP_PREFIX AND EXISTS ${LIBOMP_PREFIX}/lib/libomp.dylib)
        set(OpenMP_CXX_FLAGS "-Xpreprocessor -fopenmp -I${LIBOMP_PREFIX}/include")
        set(OpenMP_CXX_LIB_NAMES omp)
        set(OpenMP_omp_LIBRARY ${LIBOMP_PREFIX}/lib/libomp.dylib)
        find_package(OpenMP QUIET)
    endif()
endif()
endif()
if(OpenMP_CXX_FOUND)
    message(STATUS "Tenganisha: OpenMP enabled for demucs_inference")
    if(APPLE)
        # Compile flags PRIVATE: Eigen's parallel GEMM instantiates inside this
        # target's TUs, and the flags must not leak into JUCE/plugin TUs
        # (they re-trigger warnings JUCE suppresses). Link stays PUBLIC so the
        # plugin binary gets libomp.
        separate_arguments(TENGANISHA_OMP_FLAGS NATIVE_COMMAND "${OpenMP_CXX_FLAGS}")
        target_compile_options(demucs_inference PRIVATE ${TENGANISHA_OMP_FLAGS})
        target_link_libraries(demucs_inference PUBLIC ${OpenMP_omp_LIBRARY})
    else()
        target_link_libraries(demucs_inference PUBLIC OpenMP::OpenMP_CXX)
    endif()
else()
    message(STATUS "Tenganisha: OpenMP NOT found — inference GEMM single-threaded")
endif()

# Route Eigen's big matrix products through Accelerate (AMX) on macOS.
# PUBLIC define: every TU that includes Eigen must agree on EIGEN_USE_BLAS.
option(TENGANISHA_ACCELERATE "Use Apple Accelerate BLAS for inference" ON)
if(APPLE AND TENGANISHA_ACCELERATE)
    target_compile_definitions(demucs_inference PUBLIC EIGEN_USE_BLAS)
    target_link_libraries(demucs_inference PUBLIC "-framework Accelerate")
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

# ---------------------------------------------------------------- Offline test
# Console harness driving the real SeparationEngine: model load -> separate ->
# stems summed at unity must reconstruct the input (null test, RMS gate).
juce_add_console_app(tenganisha_offline_test PRODUCT_NAME "tenganisha_offline_test")

target_sources(tenganisha_offline_test PRIVATE
    test/offline_test.cpp
    src/SeparationEngine.cpp)

target_include_directories(tenganisha_offline_test PRIVATE src)

# runDispatchLoopUntil (used to pump engine callbacks) needs modal loops.
target_compile_definitions(tenganisha_offline_test PRIVATE
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(tenganisha_offline_test
    PRIVATE
        demucs_inference
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_events
    PUBLIC
        # config only: recommended warning flags stay on the plugin target;
        # they make JUCE's own module TUs noisy in console-app targets.
        juce::juce_recommended_config_flags)

# ---------------------------------------------------------------- Model-manager test
juce_add_console_app(tenganisha_model_manager_test PRODUCT_NAME "tenganisha_model_manager_test")

target_sources(tenganisha_model_manager_test PRIVATE
    test/model_manager_test.cpp
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/SeparationEngine.cpp)

target_include_directories(tenganisha_model_manager_test PRIVATE src)

target_compile_definitions(tenganisha_model_manager_test PRIVATE
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(tenganisha_model_manager_test
    PRIVATE
        demucs_inference
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)

# ---------------------------------------------------------------- UI snapshots
# Renders the editor offscreen to PNGs per engine state (visual review, README).
juce_add_console_app(tenganisha_ui_snapshot PRODUCT_NAME "tenganisha_ui_snapshot")

target_sources(tenganisha_ui_snapshot PRIVATE
    test/ui_snapshot.cpp
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/SeparationEngine.cpp)

target_include_directories(tenganisha_ui_snapshot PRIVATE src)

target_compile_definitions(tenganisha_ui_snapshot PRIVATE
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(tenganisha_ui_snapshot
    PRIVATE
        demucs_inference
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)

# ---------------------------------------------------------------- Host-sim test
# Drives the full processor with a scripted AudioPlayHead: record -> separate ->
# playback, checking sample alignment across loop/jump plus state transitions.
juce_add_console_app(tenganisha_host_sim_test PRODUCT_NAME "tenganisha_host_sim_test")

target_sources(tenganisha_host_sim_test PRIVATE
    test/host_sim_test.cpp
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/SeparationEngine.cpp)

target_include_directories(tenganisha_host_sim_test PRIVATE src)

target_compile_definitions(tenganisha_host_sim_test PRIVATE
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(tenganisha_host_sim_test
    PRIVATE
        demucs_inference
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        # config only: recommended warning flags stay on the plugin target;
        # they make JUCE's own module TUs noisy in console-app targets.
        juce::juce_recommended_config_flags)
