/*
 * Copyright (C) by Duncan Mac-Vicar P. <duncan@kde.org>
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "application.h"


#include "account.h"
#include "accountmanager.h"
#include "accountstate.h"
#include "common/vfs.h"
#include "configfile.h"
#include "folder.h"
#include "folderman.h"
#include "settingsdialog.h"
#include "socketapi/socketapi.h"
#include "theme.h"

#ifdef WITH_AUTO_UPDATER
#include "updater/ocupdater.h"
#endif

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

#include <QApplication>
#include <QDesktopServices>
#include <QFileOpenEvent>

namespace OCC {

Q_LOGGING_CATEGORY(lcApplication, "gui.application", QtInfoMsg)

QString Application::displayLanguage() const
{
    return _displayLanguage;
}

ownCloudGui *Application::gui() const
{
    return _gui;
}

Application *Application::_instance = nullptr;

Application::Application(Platform *platform, const QString &displayLanguage, bool debugMode)
    : _debugMode(debugMode)
    , _displayLanguage(displayLanguage)
{
    platform->migrate();

    qCInfo(lcApplication) << "Plugin search paths:" << qApp->libraryPaths();

    // Check vfs plugins
    if (Theme::instance()->showVirtualFilesOption() && VfsPluginManager::instance().bestAvailableVfsMode() == Vfs::Off) {
        qCWarning(lcApplication) << "Theme wants to show vfs mode, but no vfs plugins are available";
    }
    if (VfsPluginManager::instance().isVfsPluginAvailable(Vfs::WindowsCfApi))
        qCInfo(lcApplication) << "VFS windows plugin is available";

    ConfigFile cfg;

    // this should be called once during application startup to make sure we don't miss any messages
    cfg.configureHttpLogging();

    // The timeout is initialized with an environment variable, if not, override with the value from the config
    if (AbstractNetworkJob::httpTimeout == AbstractNetworkJob::DefaultHttpTimeout) {
        AbstractNetworkJob::httpTimeout = cfg.timeout();
    }

    qApp->setQuitOnLastWindowClosed(false);

    Theme::instance()->setSystrayUseMonoIcons(cfg.monoIcons());
    connect(Theme::instance(), &Theme::systrayUseMonoIconsChanged, this, &Application::slotUseMonoIconsChanged);

    // Setting up the gui class will allow tray notifications for the
    // setup that follows, like folder setup
    _gui = new ownCloudGui(this);

    connect(AccountManager::instance(), &AccountManager::accountAdded, this, &Application::slotAccountStateAdded);
    connect(AccountManager::instance(), &AccountManager::lastAccountRemoved, this, &Application::lastAccountStateRemoved);
    for (const auto &ai : AccountManager::instance()->accounts()) {
        slotAccountStateAdded(ai);
    }

    connect(FolderMan::instance()->socketApi(), &SocketApi::shareCommandReceived, _gui.data(), &ownCloudGui::slotShowShareDialog);

    // Refactoring example: this is oversimplified and really belongs in a dedicated app builder impl but the idea is illustrated:
    // don't handling everything "locally" -> request that the best entity for the job do it. Then make the proper connections between
    // requestor and responsible handler in a clearly defined, central location (e.g. an app builder, but for now, this will do)
    connect(_gui, &ownCloudGui::requestSetUpSyncFoldersForAccount, FolderMan::instance(), &FolderMan::setUpInitialSyncFolders);

#ifdef WITH_AUTO_UPDATER
    // Update checks
    UpdaterScheduler *updaterScheduler = new UpdaterScheduler(this, this);
    // the updater scheduler takes care of connecting its GUI bits to other components
    (void)updaterScheduler;
#endif

    // Cleanup at Quit.
    connect(qApp, &QCoreApplication::aboutToQuit, this, &Application::slotCleanup);
    qApp->installEventFilter(this);
}

Application::~Application()
{
    // Make sure all folders are gone, otherwise removing the
    // accounts will remove the associated folders from the settings.
    FolderMan::instance()->unloadAndDeleteAllFolders();
}

void Application::lastAccountStateRemoved() const
{
    // auto run the wizard if there are no existing accounts
    if (_gui && AccountManager::instance()->accounts().isEmpty()) {
        gui()->runAccountWizard();
    }
}

void Application::slotAccountStateAdded(AccountStatePtr accountState) const
{
    // Hook up the GUI slots to the account state's Q_SIGNALS:
    connect(accountState.data(), &AccountState::stateChanged,
        _gui.data(), &ownCloudGui::slotAccountStateChanged);
    connect(accountState->account().data(), &Account::serverVersionChanged,
        _gui.data(), [account = accountState->account().data(), this] {
            _gui->slotTrayMessageIfServerUnsupported(account);
        });

    // Hook up the folder manager slots to the account state's Q_SIGNALS:
    connect(accountState.data(), &AccountState::isConnectedChanged, FolderMan::instance(), &FolderMan::slotIsConnectedChanged);
    connect(accountState->account().data(), &Account::serverVersionChanged, FolderMan::instance(),
        [account = accountState->account().data()] { FolderMan::instance()->slotServerVersionChanged(account); });
}

void Application::slotCleanup()
{
    // unload the ui to make sure we no longer react to signals
    _gui->slotShutdown();
    delete _gui;

    // by now the credentials are supposed to be persisted
    // don't start async credentials jobs during shutdown
    AccountManager::instance()->save(false);

    FolderMan::instance()->unloadAndDeleteAllFolders();

    // Remove the account from the account manager so it can be deleted.
    AccountManager::instance()->shutdown();
}

void Application::updateAutoRun(bool firstRun)
{
    if (!firstRun)
        return;

    bool shouldSetAutoStart = firstRun;

    if (Utility::isMac()) {
        // Don't auto start when not being 'installed'
        shouldSetAutoStart = shouldSetAutoStart && QCoreApplication::applicationDirPath().startsWith(QLatin1String("/Applications/"));
    }

    if (shouldSetAutoStart) {
        Utility::setLaunchOnStartup(Theme::instance()->appName(), Theme::instance()->appNameGUI(), true);
    }
}

void Application::slotUseMonoIconsChanged(bool)
{
    _gui->slotComputeOverallSyncStatus();
}

bool Application::debugMode()
{
    return _debugMode;
}

std::unique_ptr<Application> Application::createInstance(Platform *platform, const QString &displayLanguage, bool debugMode)
{
    Q_ASSERT(!_instance);
    _instance = new Application(platform, displayLanguage, debugMode);
    return std::unique_ptr<Application>(_instance);
}

} // namespace OCC
