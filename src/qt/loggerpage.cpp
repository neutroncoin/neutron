#include "loggerpage.h"
#include "ui_loggerpage.h"
#include "guiutil.h"
#include "sync.h"

LoggerPage::LoggerPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::LoggerPage)
{
    ui->setupUi(this);

    int columnDateWidth = 100;
    int columnTimeWIdth = 100;
    int columnMsgWidth = 500;

    ui->tblLogs->setColumnWidth(0, columnDateWidth);
    ui->tblLogs->setColumnWidth(1, columnTimeWIdth);
    ui->tblLogs->setColumnWidth(2, columnMsgWidth);
    ui->tblLogs->setHorizontalHeaderItem(0, new QTableWidgetItem(tr("Date")));
    ui->tblLogs->setHorizontalHeaderItem(1, new QTableWidgetItem(tr("Time")));
    ui->tblLogs->setHorizontalHeaderItem(2, new QTableWidgetItem(tr("Message")));
    ui->tblLogs->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->tblLogs->horizontalHeader()->setStretchLastSection(true);

    searchEngines[QString("DuckDuckGo")] = QString("https://duckduckgo.com/?q=%s");
    searchEngines[QString("Google")] = QString("https://www.google.com/search?q=%s");
    selectedQuery = searchEngines[QString("DuckDuckGo")];

    QIcon copyEntryIcon(":/icons/editcopy");
    QIcon webSearchIcon(":/icons/websearch");

    QAction *copyEntryAction = new QAction(copyEntryIcon, tr("Copy"), this);
    QAction *webSearchAction = new QAction(webSearchIcon, tr("Web-search"), this);

    contextMenu = new QMenu();
    contextMenu->addAction(copyEntryAction);
    contextMenu->addAction(webSearchAction);
    connect(ui->tblLogs, SIGNAL(customContextMenuRequested(const QPoint&)), this, SLOT(showContextMenu(const QPoint&)));
    connect(copyEntryAction, SIGNAL(triggered()), this, SLOT(on_copyEntry_selected()));
    connect(webSearchAction, SIGNAL(triggered()), this, SLOT(on_webSearch_selected()));


    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(updateLogTable()));

    timer->start(1000);

    nTimeFilterUpdated = GetTime();
    updateLogTable();
}

LoggerPage::~LoggerPage()
{
    delete ui;
}

void LoggerPage::SendCommand(Enums::Logger::LoggerCommand cmd)
{
    // TODO
}

void LoggerPage::showContextMenu(const QPoint &point)
{
    QTableWidgetItem *item = ui->tblLogs->itemAt(point);
    if(item) contextMenu->exec(QCursor::pos());
}

void LoggerPage::updateLogTable()
{
    int nNewRow = ui->tblLogs->rowCount();
    TRY_LOCK(cs_loglist, fLockAcquired);
    if(!fLockAcquired) {
        return;
    }

    static int64_t nTimeListUpdated = GetTime();
    int64_t nSecondsToWait = nTimeListUpdated - GetTime() + LOGGER_UPDATE_SECONDS;
    if(nSecondsToWait > 0) return;

    nTimeListUpdated = GetTime();

#if defined(Q_OS_WIN)
    QString home = QStandardPaths::locate(QStandardPaths::DataLocation, QString::fromStdString(DEBUG_FILEPATH), QStandardPaths::LocateFile);
#else
    QString path = QStandardPaths::locate(QStandardPaths::HomeLocation, QString::fromStdString(DEBUG_FILEPATH), QStandardPaths::LocateFile);
#endif
    QFile debugLog(path);
    ui->lblLoggerStatus->setText(QString().sprintf("Reading from %s", path.toStdString().c_str()));
    if(debugLog.open(QIODevice::ReadOnly))
    {
        if (qiLastPos > debugLog.size())
        {
            qiLastPos = 0;
        }

        debugLog.seek(qiLastPos);
        QString line;
        QTextStream inStream(&debugLog);
        if (qiLastPos == 0)
        {
           inStream.readAll();
        }
        else
        {
            line = inStream.readLine();
        }

        if (!line.isEmpty())
        {
            QString date = line.section(" ", 0, 0, QString::SectionSkipEmpty);
            QString time = line.section(" ", 1, 1, QString::SectionSkipEmpty);
            QString message = line.section(" ", 2, -1, QString::SectionSkipEmpty);
            if (!date.isEmpty() &&
                !time.isEmpty() &&
                !message.isEmpty())
            {
                QTableWidgetItem *dateItem = new QTableWidgetItem(date);
                QTableWidgetItem *timeItem = new QTableWidgetItem(time);
                QTableWidgetItem *msgItem = new QTableWidgetItem(message);

                ui->tblLogs->insertRow(nNewRow);
                ui->tblLogs->setItem(nNewRow, 0, dateItem);
                ui->tblLogs->setItem(nNewRow, 1, timeItem);
                ui->tblLogs->setItem(nNewRow, 2, msgItem);
            }
        }
        qiLastPos = debugLog.pos();
        debugLog.close();
    }
}

void LoggerPage::on_copyEntry_selected()
{
    LOCK(cs_loglist);
    QItemSelectionModel* selectionModel = ui->tblLogs->selectionModel();
    QModelIndexList selected = selectionModel->selectedIndexes();

    if(selected.count() == 0) return;

    QModelIndex index = selected.at(0);
    int nSelectedRow = index.row();
    QString message = ui->tblLogs->item(nSelectedRow, 2)->text();
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(message);
}

void LoggerPage::on_webSearch_selected()
{
    LOCK(cs_loglist);
    QItemSelectionModel* selectionModel = ui->tblLogs->selectionModel();
    QModelIndexList selected = selectionModel->selectedIndexes();

    if(selected.count() == 0) return;

    QModelIndex index = selected.at(0);
    int nSelectedRow = index.row();
    QString searchTerm = ui->tblLogs->item(nSelectedRow, 2)->text();
    QString url = QString::fromStdString(strprintf(selectedQuery.toStdString(), searchTerm.toStdString().c_str()));
    QUrl _url(url);
    if (!QDesktopServices::openUrl(_url))
    {
        QMessageBox::warning(NULL, tr("Warning"), "Could not open a web page", QMessageBox::Ok);
    }

}

void LoggerPage::on_cbxSearchEngine_currentIndexChanged(int index)
{
    selectedQuery = searchEngines[ui->cbxSearchEngine->currentText()];
}

void LoggerPage::on_btnResetLogger_clicked()
{
    qiLastPos = 0;
}
