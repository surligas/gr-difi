# Copyright (C) 2026 Libre Space Foundation <https://libre.space>
#
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Python3 COMPONENTS Interpreter)

# Build certification test tools
add_executable(difi_cert_tx difi_cert_tx.cpp)
target_link_libraries(difi_cert_tx gnuradio-difi)
target_include_directories(difi_cert_tx PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../lib>
)

add_executable(difi_cert_rx difi_cert_rx.cpp)
target_link_libraries(difi_cert_rx gnuradio-difi)
target_include_directories(difi_cert_rx PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../lib>
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/DIFI-Certification/certify_source.py" AND Python3_FOUND)
    set(TEST_VENV_DIR "${CMAKE_BINARY_DIR}/test_venv")

    # Custom target to set up the virtual environment with requirements
    add_custom_target(difi_cert_setup_venv
        COMMAND ${Python3_EXECUTABLE} -m venv ${TEST_VENV_DIR}
        COMMAND ${TEST_VENV_DIR}/bin/pip install -r ${CMAKE_CURRENT_SOURCE_DIR}/DIFI-Certification/requirements.txt
        COMMENT "Setting up Python virtual environment for DIFI certification tests"
    )

    if(EXISTS "${TEST_VENV_DIR}/bin/python")
        set(CERT_PYTHON_EXE "${TEST_VENV_DIR}/bin/python")
    else()
        set(CERT_PYTHON_EXE "${Python3_EXECUTABLE}")
    endif()

    add_test(NAME difi_qa_certification
        COMMAND ${CERT_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/run_difi_certification.py
            --cert-dir ${CMAKE_CURRENT_SOURCE_DIR}/DIFI-Certification
            --tx-bin $<TARGET_FILE:difi_cert_tx>
            --rx-bin $<TARGET_FILE:difi_cert_rx>
            --venv ${TEST_VENV_DIR}
    )
    set_tests_properties(difi_qa_certification PROPERTIES SKIP_RETURN_CODE 77)
else()
    message(STATUS "DIFI-Certification submodule or Python3 not found; skipping certification test target")
endif()

