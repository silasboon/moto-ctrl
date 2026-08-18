# Unit tests for the portable core state machines + protocol/auth. Each is a
# small standalone C program using plain assert() — no test framework
# dependency — registered with CTest.

foreach(test_name
        test_output
        test_input
        test_config
        test_persist
        test_crypto
        test_keystore
        test_config_json
        test_session
        test_lock
        test_diag
        test_power
        test_ota
        test_event_log)
    add_executable(${test_name} ${test_name}.c)
    target_link_libraries(${test_name} PRIVATE moto_ctrl_core)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
