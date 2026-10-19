// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QEventLoop>
#include <QProcess>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QTimer>
#include "mm3d/game_folder.h"

namespace MM3D::GameFolder {
// Prepare or safely upgrade portable resources before the game window opens.
// Return errors to the caller; successful setup has no separate window.
inline QString Prepare() {
    const auto rom=Rom();
    if(rom.isEmpty())return MissingGameMessage();
    const auto helper=PackageRoot()+QStringLiteral("/tools/mm3d/graphical_setup.py");
    if(!QFileInfo::exists(helper))return QStringLiteral("Setup files are missing. Extract the entire Dekui download and keep the app beside its tools and resources folders.");
#ifdef Q_OS_WIN
    const auto python=Root()+QStringLiteral("/runtime/python/python.exe");
    if(!QFileInfo(python).isFile())return QStringLiteral("The bundled setup runtime is missing. Extract the complete Majora ReMasked ZIP again, keeping its runtime folder beside Majora-ReMasked.exe.");
#elif defined(Q_OS_MACOS)
    const auto python=QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../Resources/runtime/python/bin/python3"));
    if(!QFileInfo(python).isExecutable())return QStringLiteral("The bundled setup runtime is missing. Extract the complete Majora ReMasked ZIP again; do not remove files inside Majora ReMasked.app.");
#else
    const auto python=QStandardPaths::findExecutable(QStringLiteral("python3"));
    if(python.isEmpty())return QStringLiteral("Python 3.9 or newer is required for setup and resource checks.");
#endif
    QProgressDialog progress(QStringLiteral("Preparing your game and checking installed resources…"),QString{},0,0);
    progress.setWindowTitle(QStringLiteral("Majora ReMasked"));progress.setCancelButton(nullptr);progress.setMinimumDuration(0);progress.setWindowModality(Qt::ApplicationModal);progress.show();
    QProcess process;
    QEventLoop loop;
    QString error;
    QObject::connect(&process,&QProcess::errorOccurred,&loop,[&](QProcess::ProcessError failure){
        if(failure==QProcess::FailedToStart) {
            error=QStringLiteral("Could not start the setup helper. Check that the full Dekui package was extracted and its setup runtime is available.");
            loop.quit();
        }
    });
    QObject::connect(&process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),&loop,[&](int code,QProcess::ExitStatus exit){
        if(code!=0 || exit!=QProcess::NormalExit) {
            error=QString::fromUtf8(process.readAllStandardError()).trimmed().right(4000);
            if(error.isEmpty())error=QStringLiteral("Setup stopped unexpectedly. Your existing saves have not been replaced.");
        }
        loop.quit();
    });
    process.setWorkingDirectory(Root());
    QTimer::singleShot(0,&loop,[&]{
        process.start(python,{QStringLiteral("-E"),QStringLiteral("-s"),QStringLiteral("-B"),helper,QStringLiteral("--root"),Root(),QStringLiteral("--rom"),rom,QStringLiteral("--in-place"),QStringLiteral("--reference-game"),QStringLiteral("--upgrade-existing")});
    });
    loop.exec();
    return error;
}
}
