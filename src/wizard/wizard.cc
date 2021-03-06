#include "wizard.h"

#include <QCommandLineOption>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDesktopServices>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkReply>
#include <QSerialPortInfo>
#include <QUuid>
#include <QUrlQuery>

#include "cc3200.h"
#include "esp8266.h"
#include "flasher.h"
#include "fw_bundle.h"
#include "fw_client.h"
#include "serial.h"
#include "status_qt.h"

#if (QT_VERSION < QT_VERSION_CHECK(5, 5, 0))
#define qInfo qWarning
#endif

// From build_info.cc (auto-generated).
extern const char *build_id;

namespace {

const char kReleaseInfoFile[] = ":/releases.json";
// In the future we may want to fetch release info from a server.
// const char kReleaseInfoURL[] = "https://backend.cesanta.com/...";

const char kWiFiStaEnableKey[] = "wifi.sta.enable";
const char kWiFiStaSsidKey[] = "wifi.sta.ssid";
const char kWiFiStaPassKey[] = "wifi.sta.pass";
const char kClubbyConnectOnBootKey[] = "clubby.connect_on_boot";
const char kClubbyServerAddressKey[] = "clubby.server_address";
const char kClubbyDeviceIdKey[] = "clubby.device_id";
const char kClubbyDevicePskKey[] = "clubby.device_psk";
const char kFwArchVar[] = "arch";
const char kMacAddressVar[] = "mac_address";
const char kFwBuildVar[] = "fw_id";

const char kCloudServerAddressOption[] = "cloud-server-address";
const char kCloudFrontendUrlOption[] = "cloud-frontend-url";
const char kCloudDeviceRegistrationPath[] = "/register_device";
const char kCloudDeviceClaimPath[] = "/claim";

QJsonValue jsonLookup(const QJsonObject &obj, const QString &key) {
  QJsonObject o = obj;
  const auto parts = key.split(".");
  for (int i = 0; i < parts.length() - 1; i++) {
    const QString &k = parts[i];
    if (!o[k].isObject()) return QJsonValue::Undefined;
    o = o[k].toObject();
  }
  return o[parts.back()];
}

}  // namespace

// static
void WizardDialog::addOptions(Config *config) {
  QList<QCommandLineOption> opts;
  opts.append(QCommandLineOption(kCloudServerAddressOption,
                                 "Cloud API server address", "host",
                                 "api.mongoose-iot.com"));
  opts.append(QCommandLineOption(kCloudFrontendUrlOption,
                                 "URL of the cloud frontend", "URL",
                                 "https://mongoose-iot.com"));
  config->addOptions(opts);
}

WizardDialog::WizardDialog(Config *config, QWidget *parent)
    : QMainWindow(parent),
      config_(config),
      prompter_(this),
      skipFlashingText_(tr("<Skip Flashing>")) {
  ui_.setupUi(this);
  restoreGeometry(settings_.value("wizard/geometry").toByteArray());

  connect(ui_.steps, &QStackedWidget::currentChanged, this,
          &WizardDialog::currentStepChanged);
  ui_.steps->setCurrentIndex(static_cast<int>(Step::Connect));
  connect(ui_.prevBtn, &QPushButton::clicked, this, &WizardDialog::prevStep);
  connect(ui_.nextBtn, &QPushButton::clicked, this, &WizardDialog::nextStep);

  portRefreshTimer_.start(1);
  connect(&portRefreshTimer_, &QTimer::timeout, this,
          &WizardDialog::updatePortList);

  connect(ui_.platformSelector, &QComboBox::currentTextChanged, this,
          &WizardDialog::updateFirmwareSelector);

  connect(ui_.s3_wifiName, &QComboBox::currentTextChanged, this,
          &WizardDialog::wifiNameChanged);

  connect(&prompter_, &GUIPrompter::showPrompt, this,
          &WizardDialog::showPrompt);
  connect(this, &WizardDialog::showPromptResult, &prompter_,
          &GUIPrompter::showPromptResult);

  connect(ui_.s5_claimBtn, &QPushButton::clicked, this,
          &WizardDialog::claimBtnClicked);

  connect(ui_.logLink, &QLabel::linkActivated, this,
          &WizardDialog::showLogViewer);

  QTimer::singleShot(10, this, &WizardDialog::currentStepChanged);
  QTimer::singleShot(10, this, &WizardDialog::updateReleaseInfo);
}

WizardDialog::Step WizardDialog::currentStep() const {
  return static_cast<Step>(ui_.steps->currentIndex());
}

util::Status WizardDialog::doConnect() {
  hal_.reset();
  port_.reset();
  auto sp = connectSerial(QSerialPortInfo(selectedPort_), 115200);
  if (!sp.ok()) {
    return QS(util::error::UNAVAILABLE,
              tr("Error opening %1: %2")
                  .arg(selectedPort_)
                  .arg(sp.status().ToString().c_str()));
  }
  port_.reset(sp.ValueOrDie());
  qInfo() << "Probing" << selectedPlatform_ << "@" << selectedPort_;
  if (selectedPlatform_ == "ESP8266") {
    hal_ = ESP8266::HAL(port_.get());
  } else if (selectedPlatform_ == "CC3200") {
    hal_ = CC3200::HAL(port_.get());
  } else {
    port_.reset();
    return QS(util::error::INVALID_ARGUMENT,
              tr("Unknown platform: %1").arg(selectedPlatform_));
  }
  util::Status st = hal_->probe();
  if (!st.ok()) {
    return QSP(
        tr("Did not find %1 @ %2").arg(selectedPlatform_).arg(selectedPort_),
        st);
    hal_.reset();
    port_.reset();
  }
  qInfo() << "Probe successful";
  return util::Status::OK;
}

void WizardDialog::nextStep() {
  const Step ci = currentStep();
  Step ni = Step::Invalid;
  switch (ci) {
    case Step::Connect: {
      selectedPlatform_ = ui_.platformSelector->currentText();
      selectedPort_ = ui_.portSelector->currentText();
      QString msg;
      do {
        const util::Status st = doConnect();
        if (st.ok()) {
          ni = Step::FirmwareSelection;
          break;
        }
        msg = st.ToString().c_str();
        qCritical() << msg;
      } while (
          QMessageBox::critical(this, tr("Error"), msg,
                                QMessageBox::Retry | QMessageBox::Cancel) ==
          QMessageBox::Retry);
      break;
    }
    case Step::FirmwareSelection: {
      const QString &fwName = ui_.firmwareSelector->currentText();
      settings_.setValue("wizard/selectedFw", fwName);
      selectedFirmwareURL_.clear();
      // Is it an item from the list?
      bool found = false;
      for (const auto &item : releases_) {
        if (fwName == item.toObject()["name"].toString() ||
            fwName == skipFlashingText_) {
          selectedFirmwareURL_ = ui_.firmwareSelector->currentData().toString();
          found = true;
          break;
        }
      }
      if (!found) {
        // user must've entered something manually.
        selectedFirmwareURL_ = fwName;
      }
      qInfo() << "Selected platform:" << selectedPlatform_
              << "fw:" << selectedFirmwareURL_;
      ni = Step::Flashing;
      break;
    }
    case Step::Flashing: {
      ni = Step::WiFiConfig;
      break;
    }
    case Step::WiFiConfig: {
      ni = Step::WiFiConnect;
      wifiName_ = ui_.s3_wifiName->currentText();
      wifiPass_ = ui_.s3_wifiPass->toPlainText();
      qInfo() << "Selected network:" << wifiName_;
      fwc_->setConfValue(kWiFiStaEnableKey, true);
      fwc_->setConfValue(kWiFiStaSsidKey, wifiName_);
      fwc_->setConfValue(kWiFiStaPassKey, wifiPass_);
      break;
    }
    case Step::WiFiConnect: {
      ni = Step::CloudRegistration;
      break;
    }
    case Step::CloudRegistration: {
      if (ui_.s4_newID->isChecked()) {
        ni = Step::CloudConnect;
      } else if (ui_.s4_existingID->isChecked()) {
        ni = Step::CloudCredentials;
      } else {
        ni = Step::ClaimDevice;
      }
      break;
    }
    case Step::CloudCredentials: {
      cloudId_ = ui_.s4_1_cloudID->toPlainText();
      cloudKey_ = ui_.s4_1_psk->toPlainText();
      ni = Step::CloudConnect;
      break;
    }
    case Step::CloudConnect: {
      ni = Step::ClaimDevice;
      break;
    }
    case Step::ClaimDevice: {
      QCoreApplication::quit();
      break;
    }
    case Step::Invalid: {
      break;
    }
  }
  if (ni != Step::Invalid) {
    qDebug() << "Step" << ci << "->" << ni;
    ui_.steps->setCurrentIndex(static_cast<int>(ni));
  }
}

void WizardDialog::currentStepChanged() {
  const Step ci = currentStep();
  qInfo() << "Step" << ci;
  if (ci == Step::Connect) {
    ui_.platformSelector->setFocus();
    hal_.reset();
    ui_.prevBtn->hide();
    ui_.nextBtn->setEnabled(ui_.portSelector->currentText() != "");
  } else {
    ui_.prevBtn->show();
  }
  if (ci == Step::FirmwareSelection) {
    ui_.firmwareSelector->setFocus();
    QTimer::singleShot(1, this, &WizardDialog::updateFirmwareSelector);
  }
  if (ci == Step::Flashing) {
    ui_.nextBtn->setFocus();
    fwc_.reset();
    ui_.s2_1_progress->hide();
    if (selectedFirmwareURL_.toString() == "") {
      hal_->reboot();
      emit flashingDone("skipped", true /* success */);
    } else if (selectedFirmwareURL_.scheme() == "") {
      flashFirmware(selectedFirmwareURL_.toString());
    } else {
      startFirmwareDownload(selectedFirmwareURL_);
    }
    ui_.nextBtn->setEnabled(false);
  }
  if (ci == Step::WiFiConfig) {
    ui_.s3_wifiName->setFocus();
    ui_.nextBtn->setEnabled(!ui_.s3_wifiName->currentText().isEmpty());
    if (fwc_ != nullptr) fwc_->doWifiScan();
  }
  if (ci == Step::WiFiConnect) {
    ui_.nextBtn->setFocus();
    updateWiFiStatus(wifiStatus_);
    fwc_->doWifiSetup(wifiName_, wifiPass_);
  }
  if (ci == Step::CloudRegistration) {
    ui_.s4_newID->setFocus();
    const QString &existingId = getDevConfKey(kClubbyDeviceIdKey).toString();
    if (existingId != "") {
      qInfo() << "Existing Clubby ID:" << existingId;
      ui_.s4_existingID->setChecked(true);
    } else {
      qInfo() << "No Clubby ID";
      ui_.s4_newID->setChecked(true);
    }
    ui_.nextBtn->setEnabled(true);
  }
  if (ci == Step::CloudCredentials) {
    ui_.s4_1_cloudID->setFocus();
    ui_.s4_1_cloudID->setText(getDevConfKey(kClubbyDeviceIdKey).toString());
    ui_.s4_1_psk->setText(getDevConfKey(kClubbyDevicePskKey).toString());
  }
  if (ci == Step::CloudConnect) {
    ui_.s4_2_circle->hide();
    ui_.s4_2_connected->hide();
    ui_.nextBtn->setEnabled(false);
    if (ui_.s4_newID->isChecked()) {
      registerDevice();
    } else {
      testCloudConnection(cloudId_, cloudKey_);
    }
  }
  if (ci == Step::ClaimDevice) {
    ui_.s5_claimBtn->setFocus();
    ui_.nextBtn->setText(tr("Finish"));
    ui_.nextBtn->setEnabled(false);
    if (fwc_ != nullptr) fwc_->doSaveConfig();
  } else {
    ui_.nextBtn->setText(tr("Next >"));
  }
}

void WizardDialog::prevStep() {
  const Step ci = currentStep();
  Step ni = Step::Invalid;
  switch (ci) {
    case Step::Connect: {
      break;
    }
    case Step::FirmwareSelection: {
      ni = Step::Connect;
      break;
    }
    case Step::Flashing: {
      if (fd_ != nullptr) fd_->abort();
      ni = Step::FirmwareSelection;
      break;
    }
    case Step::WiFiConfig: {
      ni = Step::Flashing;
      break;
    }
    case Step::WiFiConnect: {
      ni = Step::WiFiConfig;
      break;
    }
    case Step::CloudRegistration: {
      ni = Step::WiFiConnect;
      break;
    }
    case Step::CloudCredentials: {
      ni = Step::CloudRegistration;
      break;
    }
    case Step::CloudConnect: {
      if (ui_.s4_existingID->isChecked()) {
        ni = Step::CloudCredentials;
      } else {
        ni = Step::CloudRegistration;
      }
      break;
    }
    case Step::ClaimDevice: {
      ni = Step::CloudRegistration;
    }
    case Step::Invalid: {
      break;
    }
  }
  if (ni != Step::Invalid) {
    qDebug() << "Step" << ni << "<-" << ci;
    ui_.steps->setCurrentIndex(static_cast<int>(ni));
  }
}

void WizardDialog::updatePortList() {
  QSet<QString> ports;
  for (const auto &info : QSerialPortInfo::availablePorts()) {
#ifdef Q_OS_MAC
    if (info.portName().contains("Bluetooth")) continue;
#endif
    ports.insert(info.portName());
  }

  for (int i = 0; i < ui_.portSelector->count(); i++) {
    if (ui_.portSelector->itemData(i).type() != QVariant::String) continue;
    const QString &portName = ui_.portSelector->itemData(i).toString();
    if (!ports.contains(portName)) {
      ui_.portSelector->removeItem(i);
      qDebug() << "Removing port" << portName;
      i--;
    } else {
      ports.remove(portName);
    }
  }

  for (const auto &portName : ports) {
    qDebug() << "Adding port" << portName;
    ui_.portSelector->addItem(portName, portName);
  }

  portRefreshTimer_.start(500);
  if (currentStep() == Step::Connect) {
    ui_.nextBtn->setEnabled(ui_.portSelector->currentText() != "");
  }
}

void WizardDialog::updateReleaseInfo() {
  QFile f(kReleaseInfoFile);
  if (!f.open(QIODevice::ReadOnly)) {
    qFatal("Failed to open release info file");
  }
  QJsonParseError err;
  QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
  if (err.error != QJsonParseError::NoError) {
    qFatal("Failed to parse JSON");
  }
  if (!doc.isObject()) {
    qFatal("Release info is not an object");
  }
  if (!doc.object().contains("releases")) {
    qFatal("No release info in the object");
  }
  if (!doc.object()["releases"].isArray()) {
    qFatal("Release list is not an array");
  }
  releases_ = doc.object()["releases"].toArray();
}

void WizardDialog::updateFirmwareSelector() {
  const QString platform = ui_.platformSelector->currentText().toUpper();
  ui_.firmwareSelector->clear();
  for (const auto &item : releases_) {
    if (!item.isObject()) continue;
    const QJsonObject &r = item.toObject();
    if (!r["name"].isString() || !r["locs"].isObject()) continue;
    const QString &name = r["name"].toString();
    const QJsonObject &locs = r["locs"].toObject();
    if (!locs[platform].isString()) continue;
    const QString &loc = locs[platform].toString();
    ui_.firmwareSelector->addItem(name, loc);
  }
  ui_.firmwareSelector->addItem(skipFlashingText_, "");
  const QString &selected = settings_.value("wizard/selectedFw").toString();
  for (int i = 0; i < ui_.firmwareSelector->count(); i++) {
    if (ui_.firmwareSelector->itemText(i) == selected) {
      ui_.firmwareSelector->setCurrentIndex(i);
      break;
    }
  }
  if (currentStep() == Step::FirmwareSelection) {
    ui_.nextBtn->setEnabled(ui_.firmwareSelector->currentText() != "");
  }
}

void WizardDialog::startFirmwareDownload(const QUrl &url) {
  ui_.s2_1_title->setText(tr("DOWNLOADING ..."));
  if (fd_ == nullptr || fd_->url() != url) {
    fd_.reset(new FileDownloader(url));
    connect(fd_.get(), &FileDownloader::progress, this,
            &WizardDialog::downloadProgress);
    connect(fd_.get(), &FileDownloader::finished, this,
            &WizardDialog::downloadFinished);
  }
  fd_->start();
}

void WizardDialog::downloadProgress(qint64 recd, qint64 total) {
  qDebug() << "downloadProgress" << recd << total;
  if (currentStep() != Step::Flashing) return;
  ui_.s2_1_progress->show();
  ui_.s2_1_progress->setProgress(recd, total);
}

void WizardDialog::downloadFinished() {
  util::Status st = fd_->status();
  qDebug() << "downloadFinished" << st;
  if (!st.ok()) {
    QMessageBox::critical(this, tr("Error"), st.ToString().c_str());
    prevStep();
    return;
  }
  ui_.s2_1_progress->hide();
  flashFirmware(fd_->fileName());
}

void WizardDialog::flashFirmware(const QString &fileName) {
  qInfo() << "Loading" << fileName;
  ui_.s2_1_title->setText(tr("LOADING ..."));

  auto fwbs = NewZipFWBundle(fileName);
  if (!fwbs.ok()) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to load %1: %2")
                              .arg(fileName)
                              .arg(fwbs.status().ToString().c_str()));
    return;
  }
  std::unique_ptr<FirmwareBundle> fwb = fwbs.MoveValueOrDie();
  if (fwb->platform().toUpper() != selectedPlatform_) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Platform mismatch: want %1, got %2")
                              .arg(selectedPlatform_)
                              .arg(fwb->platform()));
    return;
  }
  qInfo() << "Flashing" << fwb->name() << fwb->buildId();
  ui_.s2_1_title->setText(tr("FLASHING ..."));

  std::unique_ptr<Flasher> f(hal_->flasher(&prompter_));
  auto s = f->setOptionsFromConfig(*config_);
  if (!s.ok()) {
    QMessageBox::critical(
        this, tr("Error"),
        tr("Invalid command line flag setting: %1").arg(s.ToString().c_str()));
    return;
  }
  s = f->setFirmware(fwb.get());
  if (!s.ok()) {
    QMessageBox::critical(this, tr("Error"),
                          tr("Invalid firmware: %1").arg(s.ToString().c_str()));
    return;
  }
  bytesToFlash_ = f->totalBytes();
  connect(f.get(), &Flasher::progress, this, &WizardDialog::flashingProgress);
  connect(f.get(), &Flasher::done,
          [this]() { port_->moveToThread(this->thread()); });
  /* TODO(rojer)
    connect(f.get(), &Flasher::statusMessage,
            [this](QString msg, bool important) {
              setStatusMessage(MsgType::INFO, msg);
              (void) important;
            });
  */
  connect(f.get(), &Flasher::done, this, &WizardDialog::flashingDone);

  // Can't go back while flashing thread is running.
  ui_.prevBtn->setEnabled(false);
  worker_.reset(new QThread);
  connect(worker_.get(), &QThread::finished, f.get(), &QObject::deleteLater);
  f->moveToThread(worker_.get());
  port_->moveToThread(worker_.get());
  worker_->start();
  QTimer::singleShot(0, f.release(), &Flasher::run);
}

void WizardDialog::flashingProgress(int bytesWritten) {
  qDebug() << "Flashed" << bytesWritten << "of" << bytesToFlash_;
  if (currentStep() != Step::Flashing) return;
  ui_.s2_1_progress->show();
  ui_.s2_1_progress->setProgress(bytesWritten, bytesToFlash_);
}

void WizardDialog::flashingDone(QString msg, bool success) {
  if (worker_ != nullptr) worker_->quit();
  ui_.prevBtn->setEnabled(true);
  ui_.s2_1_progress->hide();
  if (success) {
    ui_.s2_1_title->setText(tr("FIRMWARE IS BOOTING ..."));
    fwc_.reset(new FWClient(port_.get()));
    connect(fwc_.get(), &FWClient::connectResult, this,
            &WizardDialog::fwConnectResult);
    fwc_->doConnect();
  } else {
    QMessageBox::critical(this, tr("Error"), tr("Flashing error: %1").arg(msg));
  }
}

void WizardDialog::fwConnectResult(util::Status st) {
  if (!st.ok()) {
    QMessageBox::critical(
        this, tr("Error"),
        tr("Failed to communicate to firmware: %1").arg(st.ToString().c_str()));
    return;
  }
  ui_.s2_1_title->setText(tr("CONNECTED"));
  ui_.nextBtn->setEnabled(true);
  connect(fwc_.get(), &FWClient::wifiScanResult, this,
          &WizardDialog::updateWiFiNetworks);
  connect(fwc_.get(), &FWClient::getConfigResult, this,
          &WizardDialog::updateSysConfig);
  connect(fwc_.get(), &FWClient::wifiStatusChanged, this,
          &WizardDialog::updateWiFiStatus);
  connect(fwc_.get(), &FWClient::clubbyStatus, this,
          &WizardDialog::clubbyStatus);
  gotConfig_ = gotNetworks_ = false;
  scanResults_.clear();
  ui_.s3_wifiName->clear();
  ui_.s3_wifiPass->clear();
  fwc_->doGetConfig();
  fwc_->doWifiScan();
}

void WizardDialog::updateSysConfig(QJsonObject config) {
  qInfo() << "Sys config:" << config;
  devConfig_ = config;
  gotConfig_ = true;
  if (currentStep() == Step::WiFiConfig) {
    ui_.nextBtn->setEnabled(gotConfig_ &&
                            !ui_.s3_wifiName->currentText().isEmpty());
  }
}

void WizardDialog::updateWiFiNetworks(QStringList networks) {
  qInfo() << "WiFi networks:" << networks;
  networks.sort();
  networks.removeDuplicates();
  // We don't replace the list of networks because it may not be complete
  // on each scan (ESP8266 is known to return incomplete results sometimes).
  // Instead, we age out entries from the list.
  for (const QString &n : networks) {
    scanResults_[n] = 5;
  }
  for (const QString &n : scanResults_.keys()) {
    scanResults_[n]--;
  }
  qDebug() << scanResults_;
  // Prune our dropdown: remove networks that were once in the scan results but
  // have now become stale.
  for (int i = 0; i < ui_.s3_wifiName->count();) {
    const QString &n = ui_.s3_wifiName->itemText(i);
    if (scanResults_[n] < 0) {
      ui_.s3_wifiName->removeItem(i);
      scanResults_.remove(n);
    } else {
      networks.removeOne(n);
      i++;
    }
  }
  // Add new networks.
  for (const QString &n : networks) {
    ui_.s3_wifiName->addItem(n, n);
  }

  // Select configured network and populate password, but only if user has not
  // entered stuff manually.
  if (scanResults_.contains(ui_.s3_wifiName->currentText())) {
    const QString &currentName = getDevConfKey(kWiFiStaSsidKey).toString();
    const QString &currentPass = getDevConfKey(kWiFiStaPassKey).toString();
    int currentIndex = -1;
    for (int i = 0; i < ui_.s3_wifiName->count(); i++) {
      if (ui_.s3_wifiName->itemText(i) == currentName) {
        currentIndex = i;
        break;
      }
    }
    if (currentIndex >= 0) {
      ui_.s3_wifiName->setCurrentIndex(currentIndex);
      ui_.s3_wifiPass->setText(currentPass);
    }
  }
  gotNetworks_ = true;
  if (currentStep() == Step::Flashing || currentStep() == Step::WiFiConfig) {
    /* Always be scanning. */
    if (fwc_ != nullptr) fwc_->doWifiScan();
  }
}

void WizardDialog::wifiNameChanged() {
  if (currentStep() == Step::WiFiConfig) {
    ui_.nextBtn->setEnabled(gotConfig_ &&
                            !ui_.s3_wifiName->currentText().isEmpty());
  }
}

void WizardDialog::updateWiFiStatus(FWClient::WifiStatus ws) {
  if (wifiStatus_ != ws) {
    qInfo() << "WiFi status:" << static_cast<int>(ws);
    wifiStatus_ = ws;
  }
  if (currentStep() == Step::WiFiConnect) {
    const bool isConnected = (ws == FWClient::WifiStatus::IP_Acquired);
    ui_.s3_1_circle->setVisible(isConnected);
    ui_.s3_1_connected->setVisible(isConnected);
    ui_.nextBtn->setEnabled(isConnected);
  }
}

void WizardDialog::registerDevice() {
  ui_.s4_2_title->setText(tr("REGISTERING DEVICE ..."));
  const QString arch = getDevVar(kFwArchVar).toString();
  const QString mac = getDevVar(kMacAddressVar).toString();
  const QString fwBuild = getDevVar(kFwBuildVar).toString();
  const QString url =
      config_->value(kCloudFrontendUrlOption) + kCloudDeviceRegistrationPath;
  qInfo() << "registerDevice" << url << arch << mac << fwBuild;
  if (mac == "" || arch == "") {
    const QString msg = tr("Did not find device arch and MAC address");
    qCritical() << msg;
    QMessageBox::critical(this, tr("Error"), msg);
    return;
  }
  QNetworkRequest req(url);
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                "application/x-www-form-urlencoded");
  QUrlQuery q;
  q.addQueryItem("arch", arch);
  q.addQueryItem("mac", mac);
  q.addQueryItem("fw", fwBuild);
  q.addQueryItem("fnc", build_id);
  const QByteArray params =
      QString("arch=%1&mac=%2&fw=%3").arg(arch).arg(mac).arg(fwBuild).toUtf8();
  registerDeviceReply_ = nam_.post(req, params);
  connect(registerDeviceReply_, &QNetworkReply::finished, this,
          &WizardDialog::registerDeviceRequestFinished);
}

void WizardDialog::registerDeviceRequestFinished() {
  const int code =
      registerDeviceReply_->attribute(QNetworkRequest::HttpStatusCodeAttribute)
          .toInt();
  const QByteArray response = registerDeviceReply_->readAll();
  qDebug() << "registerDeviceRequestFinished" << code << response;
  if (!registerDeviceReply_->error()) {
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(response, &err);
    if (err.error == QJsonParseError::NoError &&
        doc.object()["device_id"].toString() != "" &&
        doc.object()["device_psk"].toString() != "") {
      cloudId_ = doc.object()["device_id"].toString();
      cloudKey_ = doc.object()["device_psk"].toString();
      testCloudConnection(cloudId_, cloudKey_);
    } else {
      const QString msg(tr("Invalid response: %1").arg(QString(response)));
      qCritical() << msg;
      QMessageBox::critical(this, tr("Error"), msg);
    }
  } else {
    const QString msg = tr("Error registering device: %1")
                            .arg(registerDeviceReply_->errorString());
    qCritical() << msg;
    QMessageBox::critical(this, tr("Error"), msg);
  }
  registerDeviceReply_->deleteLater();
  registerDeviceReply_ = nullptr;
}

void WizardDialog::testCloudConnection(const QString &cloudId,
                                       const QString &cloudKey) {
  ui_.s4_2_title->setText(tr("CONNECTING TO CLOUD ..."));
  ui_.nextBtn->setEnabled(false);
  ui_.s4_2_circle->hide();
  ui_.s4_2_connected->hide();
  const QString &serverAddress = config_->value(kCloudServerAddressOption);
  qInfo() << "testCloudConnection" << serverAddress << cloudId << cloudKey;
  QJsonObject cfg;
  cfg["device_id"] = cloudId_;
  cfg["device_psk"] = cloudKey_;
  cfg["server_address"] = serverAddress;
  fwc_->testClubbyConfig(cfg);
}

void WizardDialog::clubbyStatus(int status) {
  qInfo() << "clubbyStatus" << status;
  if (status == 1) {
    ui_.nextBtn->setEnabled(true);
    ui_.s4_2_circle->show();
    ui_.s4_2_connected->show();
    fwc_->setConfValue(kClubbyConnectOnBootKey, true);
    fwc_->setConfValue(kClubbyServerAddressKey,
                       config_->value(kCloudServerAddressOption));
    fwc_->setConfValue(kClubbyDeviceIdKey, cloudId_);
    fwc_->setConfValue(kClubbyDevicePskKey, cloudKey_);
  } else {
    const QString msg(tr("Cloud connection failed"));
    qCritical() << msg;
    QMessageBox::critical(this, tr("Error"), msg);
  }
}

void WizardDialog::claimBtnClicked() {
  QUrl url(config_->value(kCloudFrontendUrlOption) + kCloudDeviceClaimPath);
  QUrlQuery q;
  q.addQueryItem("id", cloudId_);
  {
    const QByteArray salt =
        QCryptographicHash::hash(QUuid::createUuid().toByteArray(),
                                 QCryptographicHash::Sha256)
            .toBase64()
            .mid(0, 16);
    const QByteArray h = QCryptographicHash::hash(salt + cloudKey_.toUtf8(),
                                                  QCryptographicHash::Sha256);
    const QString tok =
        QString("$%1$%2$").arg(QString(salt)).arg(QString(h.toHex()));
    q.addQueryItem("token", tok);
  }
  url.setQuery(q);
  qInfo() << url.toEncoded();
  QDesktopServices::openUrl(url);
  ui_.nextBtn->setEnabled(true);
}

void WizardDialog::showLogViewer() {
  if (log_viewer_ == nullptr) {
    log_viewer_.reset(new LogViewer(nullptr));
    log_viewer_->show();
    connect(log_viewer_.get(), &LogViewer::closed, this,
            &WizardDialog::logViewerClosed);
  } else {
    log_viewer_->raise();
    log_viewer_->activateWindow();
  }
}

void WizardDialog::logViewerClosed() {
  log_viewer_.reset();
}

void WizardDialog::showPrompt(
    QString text, QList<QPair<QString, Prompter::ButtonRole>> buttons) {
  emit showPromptResult(prompter_.doShowPrompt(text, buttons));
}

QJsonValue WizardDialog::getDevConfKey(const QString &key) {
  return jsonLookup(devConfig_, QString("conf.%1").arg(key));
}

QJsonValue WizardDialog::getDevVar(const QString &var) {
  return jsonLookup(devConfig_, QString("ro_vars.%1").arg(var));
}

void WizardDialog::closeEvent(QCloseEvent *event) {
  settings_.setValue("wizard/geometry", saveGeometry());
  QMainWindow::closeEvent(event);
}

QDebug &operator<<(QDebug &d, const WizardDialog::Step s) {
  return d << static_cast<int>(s);
}
