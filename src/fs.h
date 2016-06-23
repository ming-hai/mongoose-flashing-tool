/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#ifndef CS_MFT_SRC_FS_H_
#define CS_MFT_SRC_FS_H_

#include <map>
#include <memory>

#include <QMap>
#include <QByteArray>
#include <QString>

#include "prompter.h"

#include <spiffs.h>

#include <common/util/statusor.h>

// conf taken from ESP8266 fw config
#define LOG_PAGE_SIZE 256
#define FLASH_BLOCK_SIZE (4 * 1024)

// in memory spiffs implementation
class SPIFFS {
  friend class Mounter;

 public:
  SPIFFS(QByteArray image);
  SPIFFS(int size);

  QByteArray image() const;

  spiffs *fs();

  // For use by C callbacks only.
  QByteArray *mounted_image() {
    return &image_;
  }

  util::StatusOr<std::map<QString, QByteArray>> files();

 protected:
  util::Status mount();
  void unmount();

 private:
  SPIFFS(const SPIFFS &) = delete;
  SPIFFS &operator=(const SPIFFS &) = delete;

  void initConfig(spiffs_config *cfg);

  QByteArray image_;
  spiffs fs_;

  uint8_t spiffs_work_buf_[LOG_PAGE_SIZE * 2];
  uint8_t spiffs_fds_[32 * 4];
};

util::StatusOr<QByteArray> mergeFiles(QByteArray old_fs_image,
                                      QMap<QString, QByteArray> new_files);
util::StatusOr<QByteArray> mergeFilesystems(QByteArray old_fs_image,
                                            QByteArray new_fs_image);

#endif /* CS_MFT_SRC_FS_H_ */
