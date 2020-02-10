cmake_minimum_required(VERSION 3.15)
project(geekos_1 C)

set(CMAKE_C_STANDARD 11)

set(HEADERS ./include)


include_directories(${HEADERS})

file(GLOB _SRC
        "include/geekos/*.h"
        "include/geekos/net/*.h"
        "include/libc/*.h"
        "src/geekos/*.c"
        "src/libc/*.c"
        "src/common/*.c"
        )

add_executable(geekos ${_SRC})