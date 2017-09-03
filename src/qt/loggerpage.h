#ifndef LOGGERPAGE_H
#define LOGGERPAGE_H

#include "sync.h"
#include "util.h"

#include <QMenu>
#include <QTimer>
#include <QWidget>
#include <QCheckBox>
#include <QIcon>
#include <QDateTime>
#include <QMessageBox>
#include <QFile>
#include <QTextStream>
#include <QRegExp>
#include <QStringList>
#include <QStandardPaths>
#include <QDebug>
#include <QDesktopServices>
#include <QUrl>
#include <QClipboard>
#include <QComboBox>
#include <QFileInfo>

#define LOGGER_UPDATE_SECONDS 1

#if defined(Q_OS_WIN)
    // Windows: GetDefaultDataDir()\Neutron
    #define DEBUG_FILEPATH "\\.Neutron\\debug.log"
#else
#if defined(Q_OS_MAC)
    // Mac: GetDefaultDataDir()/Neutron
    #define DEBUG_FILEPATH "Neutron/debug.log"
#else
    // Unix: GetDefaultDataDir()/.neutron
    #define DEBUG_FILEPATH ".neutron/debug.log"
#endif


namespace Enums
{
    namespace Logger
    {
        enum LoggerCommand {
            Unknown = 0,
            Activate,
            Deactivate
        };
    }
}
namespace Ui {
    class LoggerPage;
}

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

/** Logger page widget */
class LoggerPage : public QWidget
{
    Q_OBJECT

public:
    explicit LoggerPage(QWidget *parent = 0);
    ~LoggerPage();
    void SendCommand(Enums::Logger::LoggerCommand cmd = Enums::Logger::LoggerCommand::Activate);

private:
    QMenu *contextMenu;
    int64_t nTimeFilterUpdated;
    qint64 qiLastPos = 0;
    std::map<QString,QString> searchEngines;
    QString selectedQuery;

public Q_SLOTS:
    void updateLogTable();

Q_SIGNALS:

private:
    QTimer *timer;
    Ui::LoggerPage *ui;

    // Protects logtable
    CCriticalSection cs_loglist;

private Q_SLOTS:
    void showContextMenu(const QPoint &);
    void on_copyEntry_selected();
    void on_webSearch_selected();
    void on_cbxSearchEngine_currentIndexChanged(int index);
    void on_btnResetLogger_clicked();
};
#endif // LOGGERPAGE_H

