#pragma once

#include <QDir>
class LauncherInstaller {
public:
    LauncherInstaller(const QString& applicationFilePath);
    ~LauncherInstaller() = default;

    void install();
    void uninstall();
    bool runningOutsideOfInstallDir();
private:
    void createShortcuts();
    void deleteShortcuts();

    QDir _launcherInstallDir;
    QDir _launcherApplicationsDir;
    QString _launcherRunningFilePath;
    QString _launcherRunningDirPath;
};
