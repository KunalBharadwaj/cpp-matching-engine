cmake_minimum_required(VERSION 3.20)  #minimum cmake that understands this file
project(matching_engine CXX)          #project name; CXX = we commpile C++ (not C)

set(CMAKE_CXX_STANDARD 20)            #set C++ standard to C++ 20
set(CMAKE_CXX_STANDARD_REQUIRED ON)   #require C++ 20
set(CMAKE_CXX_EXTENSIONS OFF)         #standard C++20 and not GNU extensions(portable)

#An executable target named 'engine', built from these source files
add_executable(engine src/main.cpp)

#Tell the compiler where to find our headers, so '#include <engine/foo.hpp>' works.
target_include_directories(engine PRIVATE include)