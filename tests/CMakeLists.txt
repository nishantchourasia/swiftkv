# Catch2 is vendored as a single header (third_party/catch2) rather than
# fetched at configure time. The build then needs no network and produces the
# same result on any machine, which matters because the benchmark numbers in
# docs/ must be reproducible.
add_library(catch_main STATIC catch_main.cpp)
target_include_directories(catch_main PUBLIC ${CMAKE_SOURCE_DIR}/third_party/catch2)
# The vendored header is third-party code; its warnings are not ours to fix.
target_compile_options(catch_main PRIVATE -w)

function(swiftkv_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE swiftkv_core catch_main)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/third_party/catch2)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

swiftkv_add_test(test_lru)
swiftkv_add_test(test_store)
swiftkv_add_test(test_protocol)
