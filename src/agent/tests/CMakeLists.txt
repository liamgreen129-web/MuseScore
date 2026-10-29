# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent unit tests. Built in the separate build.tests tree (scripts/test.sh),
# because MUSE_ENABLE_UNIT_TESTS changes the framework config header for every module.

set(MODULE_TEST agent_tests)

set(MODULE_TEST_SRC
    ${CMAKE_CURRENT_LIST_DIR}/environment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/notation_parser_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/notation_writer_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/notation_validate_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/scoreprinter_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.cpp
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.h
)

set(MODULE_TEST_LINK
    agent_notation
    agent_read
    engraving
)

set(MODULE_TEST_DATA_ROOT ${CMAKE_CURRENT_LIST_DIR})

include(SetupGTest)
