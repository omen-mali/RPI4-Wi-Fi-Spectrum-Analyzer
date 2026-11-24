# Host unit tests (Linux only). Configure from the repo root with
#   cmake -S . -B build/host -DWIFI_HOST_TESTS=ON
# or use scripts/host-test.sh.

set(WSA_SANITIZE "none" CACHE STRING "Sanitizer for the host tests: none, asan or tsan")
set_property(CACHE WSA_SANITIZE PROPERTY STRINGS none asan tsan)

if(WSA_SANITIZE STREQUAL "asan")
    set(WSA_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
elseif(WSA_SANITIZE STREQUAL "tsan")
    set(WSA_SAN_FLAGS -fsanitize=thread)
elseif(NOT WSA_SANITIZE STREQUAL "none")
    message(FATAL_ERROR "WSA_SANITIZE must be none, asan or tsan (got '${WSA_SANITIZE}')")
endif()
if(WSA_SAN_FLAGS)
    # PUBLIC on the core library, so every test that links it gets the same flags.
    target_compile_options(wsa_core PUBLIC ${WSA_SAN_FLAGS})
    target_link_options(wsa_core PUBLIC ${WSA_SAN_FLAGS})
endif()

set(FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# wsa_add_test(name): build tests/<name>.c against the core library (and the
# fake libwpactrl, once it exists) and register it with CTest.
function(wsa_add_test name)
    add_executable(${name} ${name}.c)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE FIXTURE_DIR="${FIXTURE_DIR}")
    target_link_libraries(${name} PRIVATE wsa_core)
    if(TARGET fake_wpa_ctrl)
        target_link_libraries(${name} PRIVATE fake_wpa_ctrl)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

wsa_add_test(test_regdomain)
wsa_add_test(test_channel_analyzer)
