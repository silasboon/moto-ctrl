cmake_minimum_required(VERSION 3.16)
project(moto_ctrl_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Host build of the MOTO-CTRL core (state machines + protocol + auth) against
# a fake BLE transport (a self-contained WebSocket server), so the app and CI
# can be developed and tested without hardware attached. See AGENTS.md /
# docs/PROTOCOL.md.
#
# The core (mc_*) is plain C99 with no ESP-IDF dependency, so it is compiled
# here from the same sources firmware/components/core/ registers as an
# ESP-IDF component for the on-target build — one implementation, two build
# systems, no copy.

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/core)

add_library(moto_ctrl_core STATIC
    ${CORE_DIR}/mc_output.c
    ${CORE_DIR}/mc_input.c
    ${CORE_DIR}/mc_config.c
    ${CORE_DIR}/mc_persist.c
    ${CORE_DIR}/mc_crypto.c
    ${CORE_DIR}/mc_keystore.c
    ${CORE_DIR}/mc_status.c
    ${CORE_DIR}/mc_config_json.c
    ${CORE_DIR}/mc_session.c
    ${CORE_DIR}/mc_lock.c
    ${CORE_DIR}/mc_diag.c
    ${CORE_DIR}/mc_ota.c
    ${CORE_DIR}/mc_event_log.c
    ${CORE_DIR}/vendor/tweetnacl/tweetnacl.c
    ${CORE_DIR}/vendor/cjson/cJSON.c
)
target_include_directories(moto_ctrl_core PUBLIC
    ${CORE_DIR}/include
    ${CORE_DIR}/vendor/tweetnacl
    ${CORE_DIR}/vendor/cjson
)

# Vendored third-party sources build unmodified; don't subject them to the
# project's strict warning flags.
set_source_files_properties(
    ${CORE_DIR}/vendor/tweetnacl/tweetnacl.c
    ${CORE_DIR}/vendor/cjson/cJSON.c
    PROPERTIES COMPILE_OPTIONS "-w"
    TARGET_DIRECTORY moto_ctrl_core
)

# WebSocket sim server + entry point. Includes a fake-NVS blob store
# (sim_nvs) and the sim-only debug/fault-injection channel (sim_debug,
# sim_protocol.h) that the browser GUI (gui/) drives — see docs/TESTING.md.
add_executable(moto_ctrl_sim
    src/main.c
    src/ws_server.c
    src/sim_nvs.c
    src/sim_debug.c
)
target_link_libraries(moto_ctrl_sim PRIVATE moto_ctrl_core Threads::Threads)

enable_testing()
add_subdirectory(tests)
