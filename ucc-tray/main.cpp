/*
 * Copyright (C) 2026 Uniwill Control Center Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <QApplication>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QMessageBox>
#include <QProcess>
#include <memory>
#include "UccdClient.hpp"

class TrayController : public QObject
{
  Q_OBJECT

public:
  TrayController( QObject *parent = nullptr )
    : QObject( parent )
    , m_client( std::make_unique< ucc::UccdClient >() )
  {
    createTrayIcon();
    updateTrayStatus();
  }

private slots:
  void showMainWindow()
  {
    QProcess::startDetached( "ucc-gui", QStringList() );
  }

  void toggleWebcam()
  {
    if ( auto enabled = m_client->getWebcamEnabled() )
    {
      m_client->setWebcamEnabled( !*enabled );
      updateTrayStatus();
    }
  }

  void toggleFnLock()
  {
    if ( auto enabled = m_client->getFnLock() )
    {
      m_client->setFnLock( !*enabled );
      updateTrayStatus();
    }
  }

  void setProfileQuick( const QString &profileId )
  {
    m_client->setActiveProfile( profileId.toStdString() );
    updateTrayStatus();
  }

  void updateTrayStatus()
  {
    // Update webcam action

    if ( auto enabled = m_client->getWebcamEnabled() )
    {
      m_webcamAction->setChecked( *enabled );
    }

    // Update Fn Lock action

    if ( auto enabled = m_client->getFnLock() )
    {
      m_fnLockAction->setChecked( *enabled );
    }

    // Update active profile in menu

    if ( auto json = m_client->getActiveProfileJSON() )
    {
      // TODO: Parse JSON and update profile submenu
    }
  }

  void showAbout()
  {
    QMessageBox::about( nullptr, 
      tr( "About UCC Tray" ),
      tr( "Uniwill Control Center System Tray\n"
          "Version 0.1.0\n\n"
          "Quick access to system controls." ) );
  }

private:
  void createTrayIcon()
  {
    m_trayIcon = new QSystemTrayIcon( this );
    m_trayIcon->setIcon( QIcon::fromTheme( "preferences-system" ) );
    m_trayIcon->setToolTip( tr( "Uniwill Control Center" ) );

    // Create context menu
    auto *menu = new QMenu();

    // Main window action
    auto *openAction = menu->addAction( tr( "Open Control Center" ) );
    connect( openAction, &QAction::triggered, this, &TrayController::showMainWindow );

    menu->addSeparator();

    // Profile submenu
    auto *profileMenu = menu->addMenu( tr( "Profiles" ) );
    auto *defaultProfile = profileMenu->addAction( tr( "Default" ) );
    connect( defaultProfile, &QAction::triggered, [this]()
    {
      setProfileQuick( "__legacy_default__" );
    } );
    auto *coolProfile = profileMenu->addAction( tr( "Cool and breezy" ) );
    connect( coolProfile, &QAction::triggered, [this]()
    {
      setProfileQuick( "__legacy_cool_and_breezy__" );
    } );
    auto *powersaveProfile = profileMenu->addAction( tr( "Powersave extreme" ) );
    connect( powersaveProfile, &QAction::triggered, [this]()
    {
      setProfileQuick( "__legacy_powersave_extreme__" );
    } );

    menu->addSeparator();

    // Quick controls
    m_webcamAction = menu->addAction( tr( "Webcam" ) );
    m_webcamAction->setCheckable( true );
    connect( m_webcamAction, &QAction::triggered, this, &TrayController::toggleWebcam );

    m_fnLockAction = menu->addAction( tr( "Fn Lock" ) );
    m_fnLockAction->setCheckable( true );
    connect( m_fnLockAction, &QAction::triggered, this, &TrayController::toggleFnLock );

    menu->addSeparator();

    // About and quit
    auto *aboutAction = menu->addAction( tr( "About" ) );
    connect( aboutAction, &QAction::triggered, this, &TrayController::showAbout );

    auto *quitAction = menu->addAction( tr( "Quit" ) );
    connect( quitAction, &QAction::triggered, qApp, &QApplication::quit );

    m_trayIcon->setContextMenu( menu );
    m_trayIcon->show();

    // Double-click to open main window
    connect( m_trayIcon, &QSystemTrayIcon::activated, [this]( QSystemTrayIcon::ActivationReason reason )
    {
      if ( reason == QSystemTrayIcon::DoubleClick )
      {
        showMainWindow();
      }
    } );
  }

  std::unique_ptr< ucc::UccdClient > m_client;
  QSystemTrayIcon *m_trayIcon = nullptr;
  QAction *m_webcamAction = nullptr;
  QAction *m_fnLockAction = nullptr;
};

int main( int argc, char *argv[] )
{
  QApplication app( argc, argv );
  app.setOrganizationName( "UniwillControlCenter" );
  app.setOrganizationDomain( "uniwill.local" );
  app.setApplicationName( "ucc-tray" );
  app.setApplicationVersion( "0.1.0" );
  app.setQuitOnLastWindowClosed( false );

  if ( !QSystemTrayIcon::isSystemTrayAvailable() )
  {
    QMessageBox::critical( nullptr, 
      QObject::tr( "System Tray Error" ),
      QObject::tr( "No system tray detected on this system." ) );
    return 1;
  }

  TrayController controller;

  return app.exec();
}

#include "main.moc"
