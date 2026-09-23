cmake_minimum_required(VERSION 3.20)

project(
    key_value_store
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(kv_store
    src/key_value_store.cpp
)

target_include_directories(kv_store
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(kv_cli
    src/main.cpp
)

target_link_libraries(kv_cli
    PRIVATE
        kv_store
)

include(CTest)

if(BUILD_TESTING)
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

    if(BUILD_TESTING)
        target_compile_options(kv_store_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()
endif()
