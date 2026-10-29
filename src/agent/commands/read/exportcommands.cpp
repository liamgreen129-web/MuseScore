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
// read group, part 2: score_export and score_render (spec: docs/agent/specs/read.md).
// Writers are called directly into a file the agent owns: no export dialog, no file picker,
// no view switch. Never the registered .mscx writer: it deletes the target's parent folder
// (DECISIONS 2026-10-29).

#include "readcommands.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include "agentenv.h"
#include "registry/commandregistry.h"
#include "registry/errors.h"

#include "global/iglobalconfiguration.h"
#include "global/io/file.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/page.h"
#include "engraving/dom/system.h"
#include "notation/imasternotation.h"
#include "notation/inotation.h"
#include "project/inotationproject.h"
#include "project/inotationwriter.h"
#include "project/inotationwritersregister.h"

using namespace muse;
using namespace mu::agent;
using mu::project::INotationWriter;

namespace {
Val str(const std::string& s) { return Val(s); }

QString exportsDir()
{
    GlobalInject<IGlobalConfiguration> config;
    const QString dir = config()->userAppDataPath().toQString() + "/agent/exports";
    QDir().mkpath(dir);
    // keep the folder small: anything older than a day goes
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-1);
    for (const QFileInfo& fi : QDir(dir).entryInfoList(QDir::Files)) {
        if (fi.lastModified() < cutoff) {
            QFile::remove(fi.absoluteFilePath());
        }
    }
    return dir;
}

QString safeName(const QString& name)
{
    QString out;
    for (const QChar c : name) {
        out += (c.isLetterOrNumber() || c == '-' || c == '_') ? c : QChar('_');
    }
    return out.isEmpty() ? QString("score") : out.left(60);
}

struct Open {
    mu::project::INotationProjectPtr project;
    mu::notation::INotationPtr notation; // the master's notation, never the current view
    mu::engraving::MasterScore* score = nullptr;
};

Ret openScore(AgentEnv& env, Open& o)
{
    o.project = env.globalContext() ? env.globalContext()->currentProject() : nullptr;
    const mu::notation::IMasterNotationPtr master = o.project ? o.project->masterNotation() : nullptr;
    o.notation = master ? master->notation() : nullptr;
    o.score = master ? master->masterScore() : nullptr;
    if (!o.notation || !o.score) {
        return makeErr(Err::NoScore, "no score is open in this instance");
    }
    return make_ok();
}

bool pageView(const Open& o)
{
    return o.notation->viewMode() == mu::engraving::LayoutMode::PAGE;
}

Ret writeTo(const QString& path, const mu::project::INotationWriterPtr& writer, const mu::notation::INotationPtr& notation,
            const INotationWriter::Options& options)
{
    io::File file { io::path_t(path) };
    if (!file.open(io::IODevice::WriteOnly)) {
        return makeErr(Err::Internal, "cannot write " + path.toStdString());
    }
    Ret ret = writer->write(notation, file, options);
    file.close();
    if (!ret) {
        QFile::remove(path);
        return makeErr(Err::Internal, "the " + QFileInfo(path).suffix().toStdString() + " writer failed: " + ret.text());
    }
    return make_ok();
}

Result scoreExport(const Params& p, AgentEnv& env)
{
    Open o;
    if (Ret r = openScore(env, o); !r) {
        return { r, {} };
    }
    const std::string format = p.at("format").toString();
    const std::string suffix = format == "midi" ? "mid" : format;
    if (format == "pdf" && !pageView(o)) {
        return { makeErr(Err::Busy, "PDF needs the score in Page View; the agent never changes your view. Switch to Page View and try again."), {} };
    }
    const mu::project::INotationWriterPtr writer = env.writers() ? env.writers()->writer(suffix) : nullptr;
    if (!writer) {
        return { makeErr(Err::Internal, "no writer registered for " + suffix), {} };
    }

    const QString path = exportsDir() + "/" + safeName(o.project->displayName()) + "-"
                         + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + "." + QString::fromStdString(suffix);
    INotationWriter::Options options;
    if (writer->supportsUnitType(INotationWriter::UnitType::PER_PART)) {
        options[INotationWriter::OptionKey::UNIT_TYPE] = Val(static_cast<int>(INotationWriter::UnitType::PER_PART));
    } else if (writer->supportsUnitType(INotationWriter::UnitType::MULTI_PART)) {
        options[INotationWriter::OptionKey::UNIT_TYPE] = Val(static_cast<int>(INotationWriter::UnitType::MULTI_PART));
    }
    if (Ret r = writeTo(path, writer, o.notation, options); !r) {
        return { r, {} };
    }

    Result res;
    res.values["path"] = str(path.toStdString());
    res.values["format"] = str(format);
    res.values["bytes"] = Val(static_cast<int64_t>(QFileInfo(path).size()));
    return res;
}

Result scoreRender(const Params& p, AgentEnv& env)
{
    Open o;
    if (Ret r = openScore(env, o); !r) {
        return { r, {} };
    }
    if (!pageView(o)) {
        return { makeErr(Err::Busy, "rendering needs the score in Page View; the agent never changes your view. Switch to Page View and try again."), {} };
    }
    const int pages = static_cast<int>(o.score->npages());
    if (pages == 0) {
        return { makeErr(Err::Range, "the score has no laid-out pages"), {} };
    }

    int page = 1;
    auto pit = p.find("page");
    auto bit = p.find("bar");
    if (bit != p.end() && !bit->second.isNull()) {
        const int bar = static_cast<int>(bit->second.toDouble());
        int idx = 0;
        const mu::engraving::Measure* found = nullptr;
        for (const mu::engraving::Measure* m = o.score->firstMeasure(); m; m = m->nextMeasure()) {
            if (++idx == bar) {
                found = m;
                break;
            }
        }
        if (!found) {
            return { makeErr(Err::Range, "bar " + std::to_string(bar) + " is outside 1-" + std::to_string(idx)), {} };
        }
        if (!found->system() || !found->system()->page()) {
            return { makeErr(Err::Range, "bar " + std::to_string(bar) + " is not laid out on a page"), {} };
        }
        page = static_cast<int>(found->system()->page()->pageNumber()) + 1;
    } else if (pit != p.end() && !pit->second.isNull()) {
        page = static_cast<int>(pit->second.toDouble());
    }
    if (page < 1 || page > pages) {
        return { makeErr(Err::Range, "page " + std::to_string(page) + " is outside 1-" + std::to_string(pages)), {} };
    }

    const mu::project::INotationWriterPtr writer = env.writers() ? env.writers()->writer("png") : nullptr;
    if (!writer) {
        return { makeErr(Err::Internal, "no PNG writer registered"), {} };
    }
    const QString path = exportsDir() + "/" + safeName(o.project->displayName()) + "-p" + QString::number(page) + "-"
                         + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".png";
    INotationWriter::Options options;
    options[INotationWriter::OptionKey::UNIT_TYPE] = Val(static_cast<int>(INotationWriter::UnitType::PER_PAGE));
    options[INotationWriter::OptionKey::PAGE_NUMBER] = Val(page - 1);
    if (Ret r = writeTo(path, writer, o.notation, options); !r) {
        return { r, {} };
    }

    Result res;
    res.values["path"] = str(path.toStdString());
    res.values["page"] = Val(page);
    res.values["pages"] = Val(pages);
    return res;
}
}

void mu::agent::registerExportCommands(CommandRegistry& registry)
{
    {
        CommandInfo c;
        c.uri = "command://notation/score_export";
        c.name = "score_export";
        c.title = "Export the score";
        c.description = "Write the open score (all parts, as the full score) to a file in MuseScore Agent's own exports "
                        "folder and return its path: musicxml, mxl, midi, pdf (needs Page View) or mscz. Never asks "
                        "questions, never changes the view, never writes anywhere else. Files older than a day are removed.";
        c.input["format"] = { DataType::String, "musicxml, mxl, midi, pdf or mscz", true };
        c.input["format"].enumValues = { "musicxml", "mxl", "midi", "pdf", "mscz" };
        c.output["path"] = { DataType::String, "Absolute path of the written file" };
        c.output["format"] = { DataType::String, "The format written" };
        c.output["bytes"] = { DataType::Integer, "File size" };
        c.handler = scoreExport;
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/score_render";
        c.name = "score_render";
        c.title = "Render a page";
        c.description = "Render one page of the open score to a PNG in MuseScore Agent's exports folder and return its "
                        "path, so you can look at the engraving. Give `page`, or `bar` to render the page that bar is on. "
                        "Needs the score in Page View (the agent never changes your view).";
        c.input["page"] = { DataType::Integer, "Page number, 1-based (default 1)" };
        c.input["page"].min = Val(1);
        c.input["bar"] = { DataType::Integer, "Render the page this bar is on (overrides page)" };
        c.input["bar"].min = Val(1);
        c.output["path"] = { DataType::String, "Absolute path of the PNG" };
        c.output["page"] = { DataType::Integer, "The page rendered" };
        c.output["pages"] = { DataType::Integer, "Pages in the score" };
        c.handler = scoreRender;
        registry.add(c);
    }
}
