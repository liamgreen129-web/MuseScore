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
// Same environment as src/engraving/tests/environment.cpp, so score-level tests can load
// fixtures with the engraving module and a mocked configuration.

#include "testing/environment.h"

#include "draw/drawmodule.h"
#include "engraving/engravingmodule.h"

#include "engraving/dom/instrtemplate.h"
#include "engraving/dom/mscore.h"

#include "engraving/tests/mocks/engravingconfigurationmock.h"

#include "log.h"

static const mu::engraving::IEngravingConfiguration::DebuggingOptions debugOpt {};

static muse::testing::SuiteEnvironment agent_se(
{
    new muse::draw::DrawModule(),
    new mu::engraving::EngravingModule()
},
    nullptr,
    []() {
    mu::engraving::MScore::testMode = true;
    mu::engraving::MScore::noGui = true;

    mu::engraving::loadInstrumentTemplates(":/engraving/instruments/instruments.xml");

    using ECMock = ::testing::NiceMock<mu::engraving::EngravingConfigurationMock>;
    std::shared_ptr<ECMock> configurator(new ECMock(), [](ECMock*) {});
    ON_CALL(*configurator, isAccessibleEnabled()).WillByDefault(::testing::Return(false));
    ON_CALL(*configurator, defaultColor()).WillByDefault(::testing::Return(muse::draw::Color::BLACK));
    ON_CALL(*configurator, debuggingOptions()).WillByDefault(::testing::ReturnRef(debugOpt));
    ON_CALL(*configurator, allowReadingImagesFromOutsideMscz()).WillByDefault(::testing::Return(true));

    muse::modularity::globalIoc()->unregister<mu::engraving::IEngravingConfiguration>("utests");
    muse::modularity::globalIoc()->registerExport<mu::engraving::IEngravingConfiguration>("utests", configurator);
},
    []() {
    std::shared_ptr<mu::engraving::IEngravingConfiguration> mock
        = muse::modularity::globalIoc()->resolve<mu::engraving::IEngravingConfiguration>("utests");
    muse::modularity::globalIoc()->unregister<mu::engraving::IEngravingConfiguration>("utests");
    delete mock.get(); // as in the engraving tests: live pointers keep the mock otherwise
}
    );
