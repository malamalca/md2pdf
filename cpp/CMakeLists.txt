cmake_minimum_required(VERSION 3.21)
project(md2pdf C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# md4c: markdown parser (https://github.com/mity/md4c), vendored in md4c/src
add_executable(md2pdf
    md2pdf.cpp
    md4c/src/md4c.c
    md4c/src/entity.c
    md4c/src/md4c-html.c)

target_link_libraries(md2pdf PRIVATE ws2_32)

if(MSVC)
    target_compile_options(md2pdf PRIVATE /utf-8 /W3)
endif()
