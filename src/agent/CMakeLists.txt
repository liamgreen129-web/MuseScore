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
    commands/read/readcommands.cpp
    commands/read/exportcommands.cpp
    commands/read/readcommands.h

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

target_link_libraries(agent PRIVATE Qt::Network agent_notation agent_read)

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
