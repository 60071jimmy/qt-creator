/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2009 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at http://qt.nokia.com/contact.
**
**************************************************************************/

#include "coregdbadapter.h"

#include "debuggeractions.h"
#include "gdbengine.h"
#include "debuggerstringutils.h"

#include <utils/qtcassert.h>

#include <QtCore/QFileInfo>

namespace Debugger {
namespace Internal {

#define STRINGIFY_INTERNAL(x) #x
#define STRINGIFY(x) STRINGIFY_INTERNAL(x)
#define CB(callback) \
    static_cast<GdbEngine::AdapterCallback>(&CoreGdbAdapter::callback), \
    STRINGIFY(callback)

///////////////////////////////////////////////////////////////////////
//
// CoreGdbAdapter
//
///////////////////////////////////////////////////////////////////////

CoreGdbAdapter::CoreGdbAdapter(GdbEngine *engine, QObject *parent)
    : AbstractGdbAdapter(engine, parent)
{
    QTC_ASSERT(state() == DebuggerNotReady, qDebug() << state());
    connect(&m_gdbProc, SIGNAL(error(QProcess::ProcessError)),
        this, SLOT(handleGdbError(QProcess::ProcessError)));
    connect(&m_gdbProc, SIGNAL(readyReadStandardOutput()),
        this, SIGNAL(readyReadStandardOutput()));
    connect(&m_gdbProc, SIGNAL(readyReadStandardError()),
        this, SIGNAL(readyReadStandardError()));
    connect(&m_gdbProc, SIGNAL(started()),
        this, SLOT(handleGdbStarted()));
    connect(&m_gdbProc, SIGNAL(finished(int, QProcess::ExitStatus)),
        this, SLOT(handleGdbFinished(int, QProcess::ExitStatus)));
}

void CoreGdbAdapter::startAdapter()
{
    QTC_ASSERT(state() == EngineStarting, qDebug() << state());
    setState(AdapterStarting);
    debugMessage(_("TRYING TO START ADAPTER"));

    QStringList gdbArgs;
    gdbArgs.prepend(_("mi"));
    gdbArgs.prepend(_("-i"));

    if (!m_engine->m_outputCollector.listen()) {
        emit adapterStartFailed(tr("Cannot set up communication with child process: %1")
                .arg(m_engine->m_outputCollector.errorString()));
        return;
    }
    gdbArgs.prepend(_("--tty=") + m_engine->m_outputCollector.serverName());

    if (!startParameters().workingDir.isEmpty())
        setWorkingDirectory(startParameters().workingDir);
    if (!startParameters().environment.isEmpty())
        setEnvironment(startParameters().environment);

    QString location = theDebuggerStringSetting(GdbLocation);
    m_gdbProc.start(location, gdbArgs);
}

void CoreGdbAdapter::handleGdbStarted()
{
    QTC_ASSERT(state() == AdapterStarting, qDebug() << state());
    setState(AdapterStarted);
    emit adapterStarted();
}

void CoreGdbAdapter::handleGdbError(QProcess::ProcessError error)
{
    debugMessage(_("PLAIN ADAPTER, HANDLE GDB ERROR"));
    emit adapterCrashed(m_engine->errorMessage(error));
    shutdown();
}

void CoreGdbAdapter::prepareInferior()
{
    QTC_ASSERT(state() == AdapterStarted, qDebug() << state());
    setState(InferiorPreparing);
    setState(InferiorPrepared);
    emit inferiorPrepared();
}

void CoreGdbAdapter::startInferior()
{
    QTC_ASSERT(state() == InferiorStarting, qDebug() << state());
    QFileInfo fi2(startParameters().coreFile);
    // quoting core name below fails in gdb 6.8-debian
    m_executable.clear();
    QString coreName = fi2.absoluteFilePath();
    m_engine->postCommand(_("target core ") + coreName, CB(handleTargetCore));
}

void CoreGdbAdapter::handleTargetCore(const GdbResponse &response)
{
    QTC_ASSERT(state() == InferiorStarting, qDebug() << state());
    if (response.resultClass == GdbResultDone) {
        showStatusMessage(tr("Attached to core."));
        m_executable = startParameters().executable;
        if (m_executable.isEmpty()) {
            GdbMi console = response.data.findChild("consolestreamoutput");
            int pos1 = console.data().indexOf('`');
            int pos2 = console.data().indexOf('\'');
            if (pos1 != -1 && pos2 != -1)
                m_executable = console.data().mid(pos1 + 1, pos2 - pos1 - 1);
        }
        QFileInfo fi(m_executable);
        m_engine->postCommand(_("-file-exec-and-symbols \"%1\"")
            .arg(fi.absoluteFilePath()), CB(handleFileExecAndSymbols));
    } else {
        QTC_ASSERT(response.resultClass == GdbResultError, /**/);
        const QByteArray &msg = response.data.findChild("msg").data();
        setState(InferiorStartFailed);
        emit inferiorStartFailed(msg);
    }
}

void CoreGdbAdapter::handleFileExecAndSymbols(const GdbResponse &response)
{
    QTC_ASSERT(state() == InferiorStarting, qDebug() << state());
    if (response.resultClass == GdbResultDone) {
        showStatusMessage(tr("Symbols found."));
        setState(InferiorUnrunnable);
        m_engine->updateAll();
    } else if (response.resultClass == GdbResultError) {
        QString msg = tr("Symbols not found in \"%1\" failed:\n%2")
            .arg(__(response.data.findChild("msg").data()));
        setState(InferiorUnrunnable);
        m_engine->updateAll();
        //setState(InferiorStartFailed);
       // emit inferiorStartFailed(msg);
    }
}

void CoreGdbAdapter::interruptInferior()
{
    // A core should never 'run'
    QTC_ASSERT(false, /**/);
}

void CoreGdbAdapter::shutdown()
{
    switch (state()) {

    case DebuggerNotReady:
        return;

    case InferiorUnrunnable:
    case InferiorShutDown:
    case InferiorPreparationFailed:
        setState(AdapterShuttingDown);
        m_engine->postCommand(_("-gdb-exit"), CB(handleExit));
        return;

    default:
        QTC_ASSERT(false, qDebug() << state());
    }
}

void CoreGdbAdapter::handleExit(const GdbResponse &response)
{
    if (response.resultClass == GdbResultDone) {
        // don't set state here, this will be handled in handleGdbFinished()
    } else if (response.resultClass == GdbResultError) {
        QString msg = tr("Gdb process could not be stopped:\n") +
            __(response.data.findChild("msg").data());
        emit adapterShutdownFailed(msg);
    }
}

void CoreGdbAdapter::handleGdbFinished(int, QProcess::ExitStatus)
{
    debugMessage(_("GDB PROESS FINISHED"));
    setState(DebuggerNotReady);
    emit adapterShutDown();
}

} // namespace Internal
} // namespace Debugger
