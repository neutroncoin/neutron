#ifndef BITCOIN_QT_RPCCONSOLE_H
#define BITCOIN_QT_RPCCONSOLE_H

#include <QDialog>
#include <QTimer>

namespace Ui {
    class RPCConsole;
}
class ClientModel;

/** Local Bitcoin RPC console. */
class RPCConsole: public QDialog
{
    Q_OBJECT

public:
    explicit RPCConsole(QWidget *parent = 0);
    ~RPCConsole();

    void setClientModel(ClientModel *model);

    enum MessageClass {
        MC_ERROR,
        MC_DEBUG,
        CMD_REQUEST,
        CMD_REPLY,
        CMD_ERROR
    };

    enum TabTypes {
        TAB_INFO = 0,
        TAB_CONSOLE = 1
    };

protected:
    virtual bool eventFilter(QObject* obj, QEvent *event);

private Q_SLOTS:
    void on_lineEdit_returnPressed();
    void on_tabWidget_currentChanged(int index);
    /** open the debug.log from the current datadir */
    void on_openDebugLogfileButton_clicked();
    /** display messagebox with program parameters (same as bitcoin-qt --help) */
    void on_showCLOptionsButton_clicked();

public Q_SLOTS:
    void clear();
    void message(int category, const QString &message, bool html = false);
    /** Set number of connections shown in the UI */
    void setNumConnections(int count);
    /** Set number of masternodes shown in the UI */
    void setMasternodeCount(const QString &strMasternodes);
    /** Set number of blocks shown in the UI */
    void setNumBlocks(int count);
    /** Set size (number of transactions) of the mempool in the UI */
    void setMempoolSize(long numberOfTxs);
    /** Go forward or back in history */
    void browseHistory(int offset);
    /** Scroll console view to end */
    void scrollToEnd();
    void refreshDebugInfo();
    void updateLastBlockSeen();
    /** set which tab has the focus (is visible) */
    void setTabFocus(enum TabTypes tabType);

Q_SIGNALS:
    // For RPC command executor
    void stopExecutor();
    void cmdRequest(const QString &command);

private:
    Ui::RPCConsole *ui;
    ClientModel *clientModel;
    QStringList history;
    int historyPtr;
    QTimer *timer;

    void startExecutor();

    /** Update UI with latest network info from model. */
    void updateNetworkState();
};

#endif // BITCOIN_QT_RPCCONSOLE_H
