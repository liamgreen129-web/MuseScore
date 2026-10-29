/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * MuseScore Agent
 * Copyright (C) 2026 Liam Green and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include <memory>
#include <string>

#include "modularity/imodulesetup.h"

namespace mu::agent {
class AgentService;

class AgentModule : public muse::modularity::IModuleSetup
{
public:
    std::string moduleName() const override;

    muse::modularity::IContextSetup* newContext(const muse::modularity::ContextPtr& ctx) const override;
};

class AgentContext : public muse::modularity::IContextSetup
{
public:
    AgentContext(const muse::modularity::ContextPtr& ctx);
    ~AgentContext() override;

    void onAllInited(const muse::IApplication::RunMode& mode) override;
    void onDeinit() override;

private:
    std::unique_ptr<AgentService> m_service;
};
}
