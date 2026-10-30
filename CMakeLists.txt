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
    src/log_lock.cpp
    src/source_snapshot.cpp
    src/command_processor.cpp
    src/command_parser.cpp
    src/partition.cpp
    src/hash_ring.cpp
    src/cluster_layout.cpp
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
    src/replica_connection.cpp
)

add_executable(kv_client
    src/client_main.cpp
)

add_executable(kv_benchmark
    src/benchmark_main.cpp
)

add_executable(kv_snapshot src/snapshot_main.cpp)
target_link_libraries(kv_snapshot PRIVATE kv_store)

add_executable(kv_layout src/layout_main.cpp)
target_link_libraries(kv_layout PRIVATE kv_store)

add_executable(kv_partition_experiment src/partition_experiment.cpp)
target_link_libraries(kv_partition_experiment PRIVATE kv_store)

target_link_libraries(kv_benchmark PRIVATE kv_store)
target_link_libraries(kv_client PRIVATE kv_store)

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

    add_executable(kv_partition_tests tests/partition_test.cpp)
    target_link_libraries(kv_partition_tests PRIVATE kv_store)
    add_test(NAME kv_partition_tests COMMAND kv_partition_tests)
    set_tests_properties(kv_partition_tests PROPERTIES TIMEOUT 30)

    add_executable(kv_hash_ring_tests tests/hash_ring_test.cpp)
    target_link_libraries(kv_hash_ring_tests PRIVATE kv_store)
    add_test(NAME kv_hash_ring_tests COMMAND kv_hash_ring_tests)
    set_tests_properties(kv_hash_ring_tests PROPERTIES TIMEOUT 30)

    add_executable(kv_cluster_layout_tests tests/cluster_layout_test.cpp)
    target_link_libraries(kv_cluster_layout_tests PRIVATE kv_store)
    add_test(NAME kv_cluster_layout_tests COMMAND kv_cluster_layout_tests)
    set_tests_properties(kv_cluster_layout_tests PROPERTIES TIMEOUT 30)

    add_executable(kv_source_snapshot_tests tests/source_snapshot_test.cpp)
    target_link_libraries(kv_source_snapshot_tests PRIVATE kv_store)
    add_test(NAME kv_source_snapshot_tests COMMAND kv_source_snapshot_tests)
    set_tests_properties(kv_source_snapshot_tests PROPERTIES TIMEOUT 30)

    add_executable(kv_command_parser_tests tests/command_parser_test.cpp)
    target_link_libraries(kv_command_parser_tests PRIVATE kv_store)
    add_test(NAME kv_command_parser_tests COMMAND kv_command_parser_tests)
    set_tests_properties(kv_command_parser_tests PROPERTIES TIMEOUT 30)

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

    add_test(NAME kv_snapshot_integration_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/kv_snapshot_integration_test.py
            $<TARGET_FILE:kv_server> $<TARGET_FILE:kv_client> $<TARGET_FILE:kv_snapshot>)
    set_tests_properties(kv_snapshot_integration_tests PROPERTIES TIMEOUT 30)

    add_test(NAME kv_layout_integration_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/kv_layout_integration_test.py
            $<TARGET_FILE:kv_server> $<TARGET_FILE:kv_client>)
    set_tests_properties(kv_layout_integration_tests PROPERTIES TIMEOUT 30)

    add_test(NAME kv_distribution_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/kv_distribution_test.py
            $<TARGET_FILE:kv_server> $<TARGET_FILE:kv_client>)
    set_tests_properties(kv_distribution_tests PROPERTIES TIMEOUT 30)

    add_test(
        NAME kv_replication_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/kv_replication_test.py
            $<TARGET_FILE:kv_server>
    )
    set_tests_properties(kv_replication_tests PROPERTIES TIMEOUT 30)
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
    target_compile_options(kv_partition_experiment PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(kv_layout PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(kv_snapshot PRIVATE -Wall -Wextra -Wpedantic)

    if(BUILD_TESTING)
        target_compile_options(kv_store_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
        target_compile_options(kv_partition_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(kv_hash_ring_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(kv_cluster_layout_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(kv_source_snapshot_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(kv_command_parser_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
