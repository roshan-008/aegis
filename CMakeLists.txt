cmake_minimum_required(VERSION 3.16)
project(aegis CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=native")
# Treat our own C++ as warning-clean. Scoped via add_compile_options so it
# applies to the C++ targets below, not to the imported Rust artifacts.
add_compile_options(-Wall -Wextra)
find_package(Threads REQUIRED)
find_program(CARGO_EXECUTABLE cargo REQUIRED)

set(AEGIS_RUST_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml)
set(AEGIS_CARGO_TARGET ${CMAKE_CURRENT_BINARY_DIR}/cargo)
set(AEGIS_RUST_LIBRARY ${AEGIS_CARGO_TARGET}/release/libaegis_rust.a)
set(AEGIS_RUST_CLI ${CMAKE_CURRENT_BINARY_DIR}/aegis)
set(AEGIS_RUST_FEED_SERVER ${CMAKE_CURRENT_BINARY_DIR}/feed_server)
set(AEGIS_RUST_LOCK ${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.lock)

file(GLOB_RECURSE AEGIS_RUST_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/rust/src/*.rs)
add_custom_command(
    OUTPUT ${AEGIS_RUST_LIBRARY} ${AEGIS_RUST_CLI} ${AEGIS_RUST_FEED_SERVER}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${AEGIS_CARGO_TARGET}
            ${CARGO_EXECUTABLE} build --manifest-path ${AEGIS_RUST_MANIFEST} --release
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${AEGIS_CARGO_TARGET}/release/aegis ${AEGIS_RUST_CLI}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${AEGIS_CARGO_TARGET}/release/feed_server ${AEGIS_RUST_FEED_SERVER}
    DEPENDS ${AEGIS_RUST_SOURCES} ${AEGIS_RUST_MANIFEST} ${AEGIS_RUST_LOCK}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/rust
    COMMENT "Building Rust storage, WAL, CLI, and feed server"
    VERBATIM)
add_custom_target(aegis_rust_artifacts ALL
                  DEPENDS ${AEGIS_RUST_LIBRARY} ${AEGIS_RUST_CLI}
                          ${AEGIS_RUST_FEED_SERVER})
add_library(aegis_rust STATIC IMPORTED GLOBAL)
set_target_properties(aegis_rust PROPERTIES IMPORTED_LOCATION ${AEGIS_RUST_LIBRARY})
add_dependencies(aegis_rust aegis_rust_artifacts)

add_library(aegis_runtime INTERFACE)
target_include_directories(aegis_runtime INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aegis_runtime INTERFACE Threads::Threads aegis_rust)
if(UNIX AND NOT APPLE)
    target_link_libraries(aegis_runtime INTERFACE dl m)
endif()

add_executable(bench_rolling bench/bench_rolling.cpp)
add_executable(test_rolling test/test_rolling.cpp)
add_executable(test_fast test/test_fast.cpp)
add_executable(bench_ring bench/bench_ring.cpp)
target_link_libraries(bench_ring PRIVATE Threads::Threads)
add_executable(bench_streaming bench/bench_streaming.cpp)
target_link_libraries(bench_streaming PRIVATE Threads::Threads)
add_executable(bench_parallel bench/bench_parallel.cpp)
target_link_libraries(bench_parallel PRIVATE Threads::Threads)
add_executable(bench_mmap bench/bench_mmap.cpp)
add_executable(test_expr test/test_expr.cpp)
target_link_libraries(test_fast PRIVATE Threads::Threads)

add_executable(bench_kernels bench/bench_kernels.cpp)
add_executable(bench_runtime bench/bench_runtime.cpp)
target_link_libraries(bench_runtime PRIVATE Threads::Threads)
add_executable(bench_scheduler bench/bench_scheduler.cpp)
target_link_libraries(bench_scheduler PRIVATE Threads::Threads)
add_executable(analyze app/analyze.cpp)
target_link_libraries(analyze PRIVATE Threads::Threads)
add_executable(bench_mem bench/bench_mem.cpp)
add_executable(bench_storage bench/bench_storage.cpp)
target_link_libraries(bench_storage PRIVATE aegis_runtime)

add_executable(test_runtime test/test_runtime.cpp)
target_link_libraries(test_runtime PRIVATE aegis_runtime)
add_custom_target(aegis_cli DEPENDS ${AEGIS_RUST_CLI})
add_custom_target(feed_server_cli DEPENDS ${AEGIS_RUST_FEED_SERVER})

enable_testing()
add_test(NAME rolling COMMAND test_rolling)
add_test(NAME fast COMMAND test_fast)
add_test(NAME expressions COMMAND test_expr)
add_test(NAME runtime_r1_r4 COMMAND test_runtime)
add_test(NAME rust_unit
         COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${AEGIS_CARGO_TARGET}
                 ${CARGO_EXECUTABLE} test --manifest-path ${AEGIS_RUST_MANIFEST})
