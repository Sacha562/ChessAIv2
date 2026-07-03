cmake_minimum_required(VERSION 3.16)
project(ChessAIv2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

file(GLOB SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(chessai ${SOURCES})
target_include_directories(chessai PRIVATE include src)

# --- Threads (std::thread / pthread) ---
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(chessai PRIVATE Threads::Threads)

# --- Unit tests (doctest) -------------------------------------------------
# chessai-tests links every engine source except main.cpp (doctest supplies its
# own main via tests/test_main.cpp) plus all tests/*.cpp. Run with `ctest` or by
# executing the binary directly.
enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
set(ENGINE_SOURCES ${SOURCES})
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/main\\.cpp$")
add_executable(chessai-tests ${TEST_SOURCES} ${ENGINE_SOURCES})
target_include_directories(chessai-tests PRIVATE include src tests)
target_link_libraries(chessai-tests PRIVATE Threads::Threads)
if(USE_PEXT)
    target_compile_definitions(chessai-tests PRIVATE CHESS_USE_PEXT)
endif()
add_test(NAME unit COMMAND chessai-tests)

# --- BMI2 PEXT sliding move generation (fast on Zen 3 / modern Intel) ---
option(USE_PEXT "Use BMI2 PEXT for sliding move generation" ON)
if(USE_PEXT)
    target_compile_definitions(chessai PRIVATE CHESS_USE_PEXT)
endif()

# --- Optimization flags ---
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(chessai PRIVATE
        $<$<CONFIG:Release>:-O3;-march=native;-funroll-loops>
        -Wall -Wextra -Wno-unused-parameter)

    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
    if(_ipo_ok)
        set_property(TARGET chessai PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
elseif(MSVC)
    target_compile_options(chessai PRIVATE $<$<CONFIG:Release>:/O2;/arch:AVX2>)
endif()
