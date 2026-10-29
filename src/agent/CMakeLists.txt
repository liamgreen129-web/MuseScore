# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent: in-process MCP server and score command set (see docs/agent/).

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

    server/itransport.h
    server/mcptypes.h
    server/mcpserver.cpp
    server/mcpserver.h
    server/httptransport.cpp
    server/httptransport.h
    server/tokenstore.cpp
    server/tokenstore.h
)

target_link_libraries(agent PRIVATE Qt::Network)

target_compile_definitions(agent PRIVATE AGENT_BUNDLE_ID="${MUSE_APP_GUI_IDENTIFIER}")
