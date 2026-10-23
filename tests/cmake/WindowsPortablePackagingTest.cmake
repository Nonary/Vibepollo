# PE fixtures exercise the Windows ZIP stage on any host, without an SDK or
# linking the application. Register when the small fixture toolchain is present.
find_program(SUNSHINE_TEST_PORTABLE_PYTHON NAMES python3 python)
find_program(SUNSHINE_TEST_PORTABLE_CLANG NAMES clang)
find_program(SUNSHINE_TEST_PORTABLE_LINK NAMES lld-link)
find_program(SUNSHINE_TEST_PORTABLE_OBJDUMP NAMES objdump llvm-objdump)
if(SUNSHINE_TEST_PORTABLE_PYTHON AND SUNSHINE_TEST_PORTABLE_CLANG AND
        SUNSHINE_TEST_PORTABLE_LINK AND SUNSHINE_TEST_PORTABLE_OBJDUMP)
    add_test(NAME test_component_windows_portable_packaging
        COMMAND "${SUNSHINE_TEST_PORTABLE_PYTHON}"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/tests/integration/test_windows_portable_packaging.py"
            --repository "${SUNSHINE_TEST_REPOSITORY_ROOT}"
            --cmake "${CMAKE_COMMAND}" --cpack "${CMAKE_CPACK_COMMAND}"
            --clang "${SUNSHINE_TEST_PORTABLE_CLANG}" --link "${SUNSHINE_TEST_PORTABLE_LINK}"
            --objdump "${SUNSHINE_TEST_PORTABLE_OBJDUMP}")
    set_tests_properties(test_component_windows_portable_packaging PROPERTIES LABELS "component;packaging" TIMEOUT 180)
    add_test(NAME test_component_windows_portable_packaging_x86
        COMMAND "${SUNSHINE_TEST_PORTABLE_PYTHON}"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/tests/integration/test_windows_portable_packaging.py"
            --repository "${SUNSHINE_TEST_REPOSITORY_ROOT}" --architecture x86
            --cmake "${CMAKE_COMMAND}" --cpack "${CMAKE_CPACK_COMMAND}"
            --clang "${SUNSHINE_TEST_PORTABLE_CLANG}" --link "${SUNSHINE_TEST_PORTABLE_LINK}"
            --objdump "${SUNSHINE_TEST_PORTABLE_OBJDUMP}")
    set_tests_properties(test_component_windows_portable_packaging_x86 PROPERTIES LABELS "component;packaging" TIMEOUT 180)

    # GNU objdump installations may support x86 only; use LLVM for ARM64 PE.
    find_program(SUNSHINE_TEST_PORTABLE_LLVM_OBJDUMP NAMES llvm-objdump)
    if(SUNSHINE_TEST_PORTABLE_LLVM_OBJDUMP)
        add_test(NAME test_component_windows_portable_packaging_arm64
            COMMAND "${SUNSHINE_TEST_PORTABLE_PYTHON}"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/tests/integration/test_windows_portable_packaging.py"
                --repository "${SUNSHINE_TEST_REPOSITORY_ROOT}" --architecture ARM64
                --cmake "${CMAKE_COMMAND}" --cpack "${CMAKE_CPACK_COMMAND}"
                --clang "${SUNSHINE_TEST_PORTABLE_CLANG}" --link "${SUNSHINE_TEST_PORTABLE_LINK}"
                --objdump "${SUNSHINE_TEST_PORTABLE_LLVM_OBJDUMP}")
        set_tests_properties(test_component_windows_portable_packaging_arm64 PROPERTIES LABELS "component;packaging" TIMEOUT 180)
    endif()
endif()

# Native CMD execution complements archive/PE checks. Wine runs are explicit
# developer probes, since a sandbox may forbid Wine's local Unix sockets.
if(CMAKE_HOST_WIN32 AND SUNSHINE_TEST_PORTABLE_PYTHON AND
        SUNSHINE_TEST_PORTABLE_CLANG AND SUNSHINE_TEST_PORTABLE_LINK)
    find_file(SUNSHINE_TEST_PORTABLE_KERNEL32
        NAMES kernel32.lib libkernel32.a
        HINTS ENV LIB ${CMAKE_C_IMPLICIT_LINK_DIRECTORIES})
    if(SUNSHINE_TEST_PORTABLE_KERNEL32)
        add_test(NAME test_component_windows_portable_launcher
            COMMAND "${SUNSHINE_TEST_PORTABLE_PYTHON}"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/tests/integration/test_windows_portable_launcher.py"
                --repository "${SUNSHINE_TEST_REPOSITORY_ROOT}"
                --clang "${SUNSHINE_TEST_PORTABLE_CLANG}" --link "${SUNSHINE_TEST_PORTABLE_LINK}"
                --kernel32 "${SUNSHINE_TEST_PORTABLE_KERNEL32}")
        set_tests_properties(test_component_windows_portable_launcher PROPERTIES
            LABELS "component;packaging" TIMEOUT 180)
    endif()
endif()
