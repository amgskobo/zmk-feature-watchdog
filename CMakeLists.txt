zephyr_include_directories(include)
if(CONFIG_ZMK_WATCHDOG_NATIVE_SANITIZER_FIXTURE)
    include(tests/native-input/sanitizers.cmake)
endif()
if(CONFIG_ZMK_WATCHDOG_NATIVE_INPUT_FIXTURE)
    add_subdirectory(tests/native-input)
endif()
if(CONFIG_ZMK_WATCHDOG)
    if(CONFIG_ZMK_WATCHDOG_DIAGNOSTICS)
        set(WATCHDOG_ID_DIR ${CMAKE_CURRENT_BINARY_DIR}/diagnostics)
        file(GLOB_RECURSE WATCHDOG_ID_INPUTS CONFIGURE_DEPENDS src/*.c include/*.h proto/* tools/*.py)
        if(CONFIG_ZMK_WATCHDOG_TEST)
            list(APPEND WATCHDOG_ID_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/ztest/watchdog_test.c)
        endif()
        if(CONFIG_ZMK_WATCHDOG_STUDIO_RPC_TEST)
            list(APPEND WATCHDOG_ID_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/ztest/watchdog_rpc_test.c)
        endif()
        if(CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST)
            if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
                list(APPEND WATCHDOG_ID_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/watchdog_relay_assertions_1.inc)
            else()
                list(APPEND WATCHDOG_ID_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/watchdog_relay_assertions_0.inc)
                list(APPEND WATCHDOG_ID_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/context_relay_assertions.h)
            endif()
        endif()
        if(CONFIG_ZMK_WATCHDOG_TEST OR CONFIG_ZMK_WATCHDOG_STUDIO_RPC_TEST OR CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST)
            list(APPEND WATCHDOG_ID_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/init_result.h)
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WATCHDOG_ID_INPUTS})
        set(WATCHDOG_NATIVE_INPUT_ARGS)
        if(CONFIG_ZMK_WATCHDOG_NATIVE_INPUT_FIXTURE)
            list(APPEND WATCHDOG_NATIVE_INPUT_ARGS --native-input-dir ${ZMK_CONFIG})
        endif()
        execute_process(COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_identity.py
            --root ${CMAKE_CURRENT_SOURCE_DIR} --config ${PROJECT_BINARY_DIR}/.config
            --zephyr ${ZEPHYR_BASE} --zmk ${APPLICATION_SOURCE_DIR}/..
            --output ${WATCHDOG_ID_DIR} ${WATCHDOG_NATIVE_INPUT_ARGS} RESULT_VARIABLE WATCHDOG_ID_RESULT)
        if(NOT WATCHDOG_ID_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to generate watchdog build identity")
        endif()
        file(STRINGS ${WATCHDOG_ID_DIR}/watchdog-test-inputs.txt WATCHDOG_SELECTED_TEST_INPUTS)
        foreach(WATCHDOG_TEST_INPUT IN LISTS WATCHDOG_SELECTED_TEST_INPUTS)
            # Zephyr already tracks keymaps, quoted DTS includes and conf files.
            # Adding their normalized paths can duplicate Zephyr's ../ aliases
            # in Ninja's phony configure inputs. Keep hashing them in identity.
            if(NOT WATCHDOG_TEST_INPUT MATCHES "\\.(keymap|dtsi|conf)$")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                    ${CMAKE_CURRENT_SOURCE_DIR}/${WATCHDOG_TEST_INPUT})
            endif()
        endforeach()
        target_include_directories(app PRIVATE ${WATCHDOG_ID_DIR})
        target_sources(app PRIVATE src/watchdog_diagnostics.c)
        # The boot report only writes log records.
        if(CONFIG_LOG)
            target_sources(app PRIVATE src/watchdog_boot_report.c)
        endif()
    endif()
    if(CONFIG_ZMK_WATCHDOG_BLE_DIAGNOSTICS)
        target_sources(app PRIVATE src/watchdog_ble_diagnostics.c)
        zephyr_link_libraries(-Wl,--wrap=bt_unpair -Wl,--wrap=bt_conn_disconnect)
        if(NOT CONFIG_ZMK_SPLIT OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
            zephyr_link_libraries(-Wl,--wrap=zmk_ble_clear_bonds)
        endif()
    endif()
    if(CONFIG_ZMK_WATCHDOG_CAPTURE_CONTEXT)
        target_sources(app PRIVATE src/watchdog_context.c)
    endif()
    if(CONFIG_ZMK_WATCHDOG_ASSERT_CONTEXT)
        target_sources(app PRIVATE src/watchdog_assert_context.c)
        zephyr_link_libraries(-Wl,--wrap=assert_post_action)
    endif()
    target_sources(app PRIVATE src/watchdog_store.c)
    target_sources(app PRIVATE src/watchdog_pending.c)
    target_sources(app PRIVATE src/watchdog_capture.c)
    target_sources(app PRIVATE src/watchdog_reset_cause.c)

    if(CONFIG_ZMK_WATCHDOG_FREEZE_DETECT)
        target_sources(app PRIVATE src/watchdog_freeze.c)
        target_sources(app PRIVATE src/watchdog_trace.c)
    endif()

    if(CONFIG_ZMK_WATCHDOG_FATAL_DETECT)
        target_sources(app PRIVATE src/watchdog_fatal.c)
    endif()

    if(CONFIG_ZMK_WATCHDOG_TEST_INJECTION)
        target_sources(app PRIVATE src/watchdog_inject.c)
        target_sources(app PRIVATE src/watchdog_inject_autotrigger.c)
    endif()

    if(CONFIG_ZMK_WATCHDOG_TEST)
        target_sources(app PRIVATE tests/ztest/watchdog_test.c)
    endif()

    # Protobuf/nanopb generation and src/studio/watchdog_request_exec.c (the
    # transport-independent GetStatus/ListIncidents/DeleteIncidents executor
    # against the local store) are needed by BOTH the local Studio RPC
    # handler (CONFIG_ZMK_WATCHDOG_STUDIO_RPC, central/non-split only) and
    # the split relay peripheral responder (CONFIG_ZMK_WATCHDOG_SPLIT_RELAY,
    # which a split peripheral -- with no Studio of its own -- also needs),
    # so this is gated on either being enabled, not just the RPC one.
    if(CONFIG_ZMK_WATCHDOG_STUDIO_RPC OR CONFIG_ZMK_WATCHDOG_SPLIT_RELAY)
        target_sources(app PRIVATE src/studio/watchdog_request_exec.c)

        if(CONFIG_ZMK_WATCHDOG_STUDIO_RPC)
            target_sources(app PRIVATE src/studio/watchdog_handler.c)

            if(CONFIG_ZMK_WATCHDOG_STUDIO_RPC_TEST)
                target_sources(app PRIVATE tests/ztest/watchdog_rpc_test.c)
            endif()
        endif()

        if(CONFIG_ZMK_WATCHDOG_SPLIT_RELAY)
            if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
                target_sources(app PRIVATE src/split/watchdog_relay_central.c
                    src/split/watchdog_relay_page.c)
            else()
                target_sources(app PRIVATE src/split/watchdog_relay_peripheral.c)
                # The peripheral self-test also checks central page assembly.
                if(CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST)
                    target_sources(app PRIVATE src/split/watchdog_relay_page.c)
                endif()
            endif()
        endif()

        list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
        include(nanopb)
        set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
        set(NANOPB_GENERATE_CPP_STANDALONE OFF)

        # NOTE: adding to app target directly causes build issues, so we create a library instead
        zephyr_library()
        file(GLOB_RECURSE PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto)

        nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${CMAKE_CURRENT_SOURCE_DIR} ${PROTO_FILES})
        target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
        target_sources(${ZEPHYR_CURRENT_LIBRARY} PRIVATE ${proto_srcs} ${proto_hdrs})

        # app should depend on the generated proto files
        target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/proto)
        add_dependencies(app ${ZEPHYR_CURRENT_LIBRARY})
    endif()
endif()
