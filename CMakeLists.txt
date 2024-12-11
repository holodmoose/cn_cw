cmake_minimum_required(VERSION 3.6)
project(server)

file(GLOB_RECURSE sources
    src/server.c
    src/handler.c
    src/http.c
    src/pg_list.c
    src/memory.c
)

add_executable(server ${sources} src/main.c)
target_compile_options(server PUBLIC -std=c99 -Wall -Wextra -pedantic -Wshadow -march=native)

if (${ASAN})
    target_compile_options(server PUBLIC -fsanitize=address)
    target_link_libraries(server PUBLIC -fsanitize=address)
endif()
