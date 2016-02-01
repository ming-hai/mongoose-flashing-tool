/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#ifndef DIALOG_H
#define DIALOG_H

#include <memory>

#include <QDir>
#include <QFile>
#include <QList>
#include <QMainWindow>
#include <QMessageBox>
#include <QMultiMap>
#include <QNetworkConfigurationManager>
#include <QPair>
#include <QSerialPort>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QThread>

#include <common/util/statusor.h>

#include "fw_bundle.h"
#include "hal.h"
#include "log_viewer.h"
#include "prompter.h"
#include "settings.h"
#include "ui_main.h"

class Config;
class PrompterImpl;
class QAction;
class QEvent;
class QSerialPort;

class Ui_MainWindow;

class MainDialog : public QMainWindow {
  Q_OBJECT

 public:
  MainDialog(Config *config, QWidget *parent = 0);

 protected:
  bool eventFilter(QObject *, QEvent *);  // used for input history navigation
  void closeEvent(QCloseEvent *event);

 private:
  enum State {
    NoPortSelected,
    NotConnected,
    Connected,
    Flashing,
    PortGoneWhileFlashing,
    Terminal,
  };

 public slots:
  void loadFirmware();
  void connectDisconnectTerminal();

 private slots:
  void updatePortList();
  void selectFirmwareFile();
  void flashingDone(QString msg, bool success);
  util::Status disconnectTerminal();
  void readSerial();
  void writeSerial();
  void reboot();
  void configureWiFi();
  void uploadFile();
  void resetHAL(QString name = QString());

  util::Status openSerial();
  util::Status closeSerial();
  void sendQueuedCommand();

  void setState(State);
  void enableControlsForCurrentState();
  void showLogViewer();
  void showAboutBox();

  void logViewerClosed();

  void showPrompt(QString text,
                  QList<QPair<QString, Prompter::ButtonRole>> buttons);

  void showSettings();
  void updateConfig(const QString &name);

signals:
  void gotPrompt();
  void showPromptResult(int clicked_button);
#if (QT_VERSION < QT_VERSION_CHECK(5, 4, 0))
  void updatePlatformSelector(int index);
#endif

 private:
  std::unique_ptr<FirmwareBundle> loadFirmwareBundle(const QString &fileName);

  Config *config_ = nullptr;
  bool skip_detect_warning_ = false;
  std::unique_ptr<QThread> worker_;
  QDir fwDir_;
  std::unique_ptr<QSerialPort> serial_port_;
  QMultiMap<QWidget *, State> enabled_in_state_;
  QMultiMap<QAction *, State> action_enabled_in_state_;
  QTimer *refresh_timer_;
  QStringList input_history_;
  QString incomplete_input_;
  int history_cursor_ = -1;
  QSettings settings_;
  QStringList command_queue_;
  std::unique_ptr<HAL> hal_;
  bool scroll_after_flashing_ = false;
  std::unique_ptr<QFile> console_log_;
  PrompterImpl *prompter_;
  SettingsDialog settingsDlg_;
  std::unique_ptr<LogViewer> log_viewer_;

  QNetworkConfigurationManager net_mgr_;

  State state_ = NoPortSelected;

  Ui::MainWindow ui_;
};

#endif  // DIALOG_H