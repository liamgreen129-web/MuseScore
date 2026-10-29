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
    ${CMAKE_CURRENT_LIST_DIR}/core_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/notes_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/expression_lines_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/expression_marks_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.cpp
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.h
)

set(MODULE_TEST_LINK
    agent_notation
    agent_read
    agent_core
    agent_notes
    agent_address
    agent_lines
    agent_marks
    engraving
)

set(MODULE_TEST_DATA_ROOT ${CMAKE_CURRENT_LIST_DIR})

include(SetupGTest)
