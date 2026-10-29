# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent: in-process MCP server and score command set (see docs/agent/).

muse_create_module(agent)

target_sources(agent PRIVATE
    agentmodule.cpp
    agentmodule.h

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
