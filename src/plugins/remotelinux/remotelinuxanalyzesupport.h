/****************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://www.qt.io/licensing.  For further information
** use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file.  Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#ifndef REMOTELINUXANALYZESUPPORT_H
#define REMOTELINUXANALYZESUPPORT_H

#include "abstractremotelinuxrunsupport.h"

#include <projectexplorer/projectexplorerconstants.h>
#include <utils/outputformat.h>

namespace Analyzer {
class AnalyzerStartParameters;
class AnalyzerRunControl;
}

namespace RemoteLinux {
class AbstractRemoteLinuxRunConfiguration;

namespace Internal { class RemoteLinuxAnalyzeSupportPrivate; }

class REMOTELINUX_EXPORT RemoteLinuxAnalyzeSupport : public AbstractRemoteLinuxRunSupport
{
    Q_OBJECT
public:
    static Analyzer::AnalyzerStartParameters startParameters(const AbstractRemoteLinuxRunConfiguration *runConfig,
                                                             ProjectExplorer::RunMode runMode);

    RemoteLinuxAnalyzeSupport(AbstractRemoteLinuxRunConfiguration *runConfig,
            Analyzer::AnalyzerRunControl *engine, ProjectExplorer::RunMode runMode);
    ~RemoteLinuxAnalyzeSupport();

protected:
    void startExecution();
    void handleAdapterSetupFailed(const QString &error);

private slots:
    void handleRemoteSetupRequested();
    void handleAppRunnerError(const QString &error);
    void handleRemoteOutput(const QByteArray &output);
    void handleRemoteErrorOutput(const QByteArray &output);
    void handleAppRunnerFinished(bool success);
    void handleProgressReport(const QString &progressOutput);

    void handleRemoteProcessStarted();
    void handleProfilingFinished();

    void remoteIsRunning();

private:
    void showMessage(const QString &, Utils::OutputFormat);

    Internal::RemoteLinuxAnalyzeSupportPrivate * const d;
};

} // namespace RemoteLinux

#endif // REMOTELINUXANALYZESUPPORT_H
