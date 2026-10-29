# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent: in-process MCP server and score command set (see docs/agent/).

# The compact notation (NOTATION.md): plain C++, no Qt or MuseScore dependencies,
# so the parser and writer are tested on their own.
add_library(agent_notation STATIC
    notation/ast.cpp
    notation/ast.h
    notation/parser.cpp
    notation/parser.h
    notation/validate.cpp
    notation/validate.h
    notation/writer.cpp
    notation/writer.h
)
target_include_directories(agent_notation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Score -> notation (read side). Depends on the engraving model.
muse_create_module(agent_read NO_UNITY)
target_sources(agent_read PRIVATE
    read/scoreprinter.cpp
    read/scoreprinter.h
)
target_link_libraries(agent_read PUBLIC agent_notation engraving)

# The transaction core (P2.1, frozen as core-v1): transactions, selection guard, handles,
# post-condition checks. Engraving only, so it is unit-tested without the app.
muse_create_module(agent_core NO_UNITY)
target_sources(agent_core PRIVATE
    core/handles.cpp
    core/handles.h
    core/postconditions.cpp
    core/postconditions.h
    core/selectionguard.cpp
    core/selectionguard.h
    core/transaction.cpp
    core/transaction.h
    core/undobackend.cpp
    core/undobackend.h
)
target_include_directories(agent_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_core PUBLIC engraving)

# Addresses shared by every write command (staff, position, target).
muse_create_module(agent_address NO_UNITY)
target_sources(agent_address PRIVATE
    address/address.cpp
    address/address.h
)
target_include_directories(agent_address PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_address PUBLIC agent_core engraving)

# Notes and rhythm (P2.3): write_voice's engine, engraving-level and unit-tested without the app.
muse_create_module(agent_notes NO_UNITY)
target_sources(agent_notes PRIVATE
    commands/notes/clearrange.cpp
    commands/notes/clearrange.h
    commands/notes/notehelpers.cpp
    commands/notes/notehelpers.h
    commands/notes/noteops.cpp
    commands/notes/noteops.h
    commands/notes/writevoice.cpp
    commands/notes/writevoice.h
)
target_include_directories(agent_notes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_notes PUBLIC agent_address agent_core agent_read agent_notation engraving)

# Slurs and lines (P2.4 expression-lines): add_slur/add_line handlers, engraving-level and
# unit-tested without the app.
muse_create_module(agent_lines NO_UNITY)
target_sources(agent_lines PRIVATE
    commands/expression-lines/lineops.cpp
    commands/expression-lines/lineops.h
)
target_include_directories(agent_lines PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_lines PUBLIC agent_core agent_read engraving)

# Expression marks (P2.4): the six write operations, engraving only, so they are unit-tested
# without the app. Registration (register.cpp) stays in the agent module.
muse_create_module(agent_marks NO_UNITY)
target_sources(agent_marks PRIVATE
    commands/expression-marks/expressionmarks.cpp
    commands/expression-marks/expressionmarks.h
)
target_include_directories(agent_marks PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_marks PUBLIC agent_core agent_read engraving)

muse_create_module(agent NO_UNITY) # small module; unity merging makes mcp:: and agent:: names collide

target_sources(agent PRIVATE
    agentmodule.cpp
    agentmodule.h
    agentenv.h
    agentservice.cpp
    agentservice.h

    registry/errors.h
    registry/commandtypes.h
    registry/commandregistry.cpp
    registry/commandregistry.h

    discovery/discoveryrecords.cpp
    discovery/discoveryrecords.h

    commands/allcommands.cpp
    commands/allcommands.h
    commands/app/appcommands.cpp
    commands/app/appcommands.h
    commands/notes/notescommands.cpp
    commands/notes/notescommands.h
    commands/expression-lines/expressionlines.h
    commands/expression-lines/register.cpp
    commands/read/readcommands.cpp
    commands/read/exportcommands.cpp
    commands/read/readcommands.h
    commands/session/sessioncommands.cpp
    commands/session/sessioncommands.h
    commands/test/testcommands.cpp
    commands/test/testcommands.h
    commands/expression-marks/expressionmarkscommands.h
    commands/expression-marks/register.cpp

    writes/notationundobackend.cpp
    writes/notationundobackend.h
    writes/writerunner.cpp
    writes/writerunner.h

    platform/background.h

    server/itransport.h
    server/mcptypes.h
    server/mcpserver.cpp
    server/mcpserver.h
    server/httptransport.cpp
    server/httptransport.h
    server/tokenstore.cpp
    server/tokenstore.h
)

target_link_libraries(agent PRIVATE Qt::Network agent_notation agent_read agent_core agent_notes agent_lines agent_marks)

target_compile_definitions(agent PRIVATE AGENT_BUNDLE_ID="${MUSE_APP_GUI_IDENTIFIER}")

if (OS_IS_MAC)
    target_sources(agent PRIVATE platform/background_mac.mm)
    set_source_files_properties(platform/background_mac.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON) # the PCH is C++ only
    target_link_libraries(agent PRIVATE "-framework AppKit")
else()
    target_sources(agent PRIVATE platform/background_other.cpp)
endif()

if (MUSE_ENABLE_UNIT_TESTS)
    add_subdirectory(tests)
endif()
