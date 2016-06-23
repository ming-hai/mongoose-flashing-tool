/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#ifndef CS_MFT_SRC_SLIP_H_
#define CS_MFT_SRC_SLIP_H_

#include <QSerialPort>
#include <QByteArray>

#include <common/util/statusor.h>

namespace SLIP {

util::StatusOr<QByteArray> recv(QSerialPort *in, int timeout = 500);
util::Status send(QSerialPort *out, const QByteArray &bytes,
                  int timeoutMs = 500);

}  // namespace SLIP

#endif /* CS_MFT_SRC_SLIP_H_ */
