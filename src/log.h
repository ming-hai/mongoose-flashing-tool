/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#ifndef CS_MFT_SRC_LOG_H_
#define CS_MFT_SRC_LOG_H_

#include <iostream>

#include <QList>
#include <QObject>
#include <QString>
#include <QtGlobal>

namespace Log {

void init();
void setVerbosity(int v);

// setFile redirects the output to a given file. Old file will be closed,
// unless it's std::cerr.
void setFile(std::ostream *file);

struct Entry {
  QtMsgType type;
  QString file;
  int line;
  QString msg;
};

QList<Entry> getBufferedLines();

class EntrySource : public QObject {
  Q_OBJECT
signals:
  void newLogEntry(const Log::Entry e);
};

EntrySource *entrySource();

}  // namespace Log

#endif /* CS_MFT_SRC_LOG_H_ */
