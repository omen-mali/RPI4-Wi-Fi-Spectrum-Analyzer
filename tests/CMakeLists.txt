# Host unit tests (Linux only). Configure from the repo root with
#   cmake -S . -B build/host
# or use scripts/host-test.sh.

set(WSA_SANITIZE "none" CACHE STRING "Sanitizer for the host tests: none, asan or tsan")
set_property(CACHE WSA_SANITIZE PROPERTY STRINGS none asan tsan)

if(WSA_SANITIZE STREQUAL "asan")
    # No recovery from UBSan findings, so one fails the test instead of only
    # printing a diagnostic.
    set(WSA_SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer)
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

# Link-time fake of libwpactrl (fakes/fake_wpa_ctrl.h). It links wsa_core for
# the core's compile flags (POSIX level, threads, sanitizer) and ts_add_ms().
# wsa_add_test lists wsa_core before the fake, so the core's wpa_ctrl_* calls
# resolve into the fake, which the wsa_core CMake adds after the fake resolves
# the fake's ts_add_ms() call.
add_library(fake_wpa_ctrl STATIC fakes/fake_wpa_ctrl.c)
target_include_directories(fake_wpa_ctrl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/fakes)
target_compile_definitions(fake_wpa_ctrl PRIVATE FIXTURE_DIR="${FIXTURE_DIR}")
target_link_libraries(fake_wpa_ctrl PUBLIC wsa_core)

# wsa_add_test(name [extra sources...]): build tests/<name>.c (plus any extra
# sources) against the core library and the fake libwpactrl, and register it
# with CTest.
function(wsa_add_test name)
    add_executable(${name} ${name}.c ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE FIXTURE_DIR="${FIXTURE_DIR}")
    target_link_libraries(${name} PRIVATE wsa_core fake_wpa_ctrl)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

wsa_add_test(test_regdomain)
wsa_add_test(test_channel_analyzer)
wsa_add_test(test_scan_parse)
wsa_add_test(test_schedule)

# The scanner test compiles its own wifi_scanner.c with a 300 ms scan-event
# timeout, so timeouts are tested in well under a second. Its object file
# defines every wifi_scanner_* symbol, so the linker never pulls the copy in
# wsa_core (archive members are only pulled for unresolved symbols).
wsa_add_test(test_scanner ${CMAKE_SOURCE_DIR}/src/wifi_scanner.c)
target_compile_definitions(test_scanner PRIVATE SCAN_COMPLETE_TIMEOUT_MS=300)

wsa_add_test(test_logger)

# The level filter is compile-time (LOG_MIN_LEVEL), so this test builds its
# own logger.c with LOG_WARN.
wsa_add_test(test_logger_level ${CMAKE_SOURCE_DIR}/src/logger.c)
target_compile_definitions(test_logger_level PRIVATE LOG_MIN_LEVEL=LOG_WARN)

