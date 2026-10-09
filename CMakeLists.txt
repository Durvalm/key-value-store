cmake_minimum_required(VERSION 3.20)

project(
    key_value_store
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(kv_store
    src/key_value_store.cpp
    src/command_processor.cpp
)

target_link_libraries(kv_store PUBLIC Threads::Threads)

target_include_directories(kv_store
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(kv_cli
    src/main.cpp
)

add_executable(kv_server
    src/server_main.cpp
)

add_executable(kv_client
    src/client_main.cpp
)

add_executable(kv_benchmark
    src/benchmark_main.cpp
)

target_link_libraries(kv_benchmark PRIVATE Threads::Threads)

target_link_libraries(kv_cli
    PRIVATE
        kv_store
)

target_link_libraries(kv_server
    PRIVATE
        kv_store
)


include(CTest)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_executable(kv_store_tests
        tests/key_value_store_test.cpp
    )

    target_link_libraries(kv_store_tests
        PRIVATE
            kv_store
    )

    add_test(
        NAME kv_store_tests
        COMMAND kv_store_tests
    )

    set_tests_properties(kv_store_tests PROPERTIES TIMEOUT 30)

    add_test(
        NAME kv_server_integration_tests
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/kv_server_integration_test.py
            $<TARGET_FILE:kv_server>
            $<TARGET_FILE:kv_client>
    )

    set_tests_properties(kv_server_integration_tests PROPERTIES
        TIMEOUT 30
    )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(kv_store PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    target_compile_options(kv_cli PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    target_compile_options(kv_server PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    target_compile_options(kv_client PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    target_compile_options(kv_benchmark PRIVATE -Wall -Wextra -Wpedantic)

    if(BUILD_TESTING)
        target_compile_options(kv_store_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()
endif()
