cmake_minimum_required(VERSION 3.20)  #minimum cmake that understands this file
project(matching_engine CXX)          #project name; CXX = we commpile C++ (not C)

set(CMAKE_CXX_STANDARD 20)            #set C++ standard to C++ 20
set(CMAKE_CXX_STANDARD_REQUIRED ON)   #require C++ 20
set(CMAKE_CXX_EXTENSIONS OFF)         #standard C++20 and not GNU extensions(portable)

option(ENABLE_SANITIZERS "Build with AddressSanitizer + UBSan" OFF)   # off by default
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -g -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()


#An executable target named 'engine', built from these source files
add_executable(engine src/main.cpp)

#Tell the compiler where to find our headers, so '#include <engine/foo.hpp>' works.
target_include_directories(engine PRIVATE include)

# ---------------Tests---------------
include(FetchContent)          # enable Cmake's download-a-dependency feature
FetchContent_Declare(
    googletest                 # a name that we pick for this dependency
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0     # pin an exact version -> reproducible builds
)
FetchContent_MakeAvailable(googletest)  # download + build it, expose its targets

enable_testing()               # turn on Cmake's test-runner (ctest)

add_executable(unit_tests tests/test_types.cpp tests/test_order_book.cpp tests/test_engine.cpp tests/test_fast_order_book.cpp)     # s SECOND executable, just for tests
target_include_directories(unit_tests PRIVATE include) # so tests can # include
target_link_libraries(unit_tests PRIVATE GTest::gtest_main)  # link GTest + its default main

include(GoogleTest)            # enable Cmake's GTest integration
gtest_discover_tests(unit_tests)  # auto-discover tests in the unit_tests executable