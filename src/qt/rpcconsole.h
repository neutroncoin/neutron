#ifndef BITCOIN_QT_RPCCONSOLE_H
#define BITCOIN_QT_RPCCONSOLE_H

#include <QDialog>
#include <QCompleter>
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

    static bool RPCParseCommandLine(std::string &strResult, const std::string &strCommand, bool fExecute, std::string * const pstrFilteredOut = NULL);
    static bool RPCExecuteCommandLine(std::string &strResult, const std::string &strCommand, std::string * const pstrFilteredOut = NULL) {
        return RPCParseCommandLine(strResult, strCommand, true, pstrFilteredOut);
    }

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
        TAB_CONSOLE = 1,
        TAB_REPAIR = 2
    };

protected:
    virtual bool eventFilter(QObject* obj, QEvent *event);

private Q_SLOTS:
    void on_lineEdit_returnPressed();
    void on_tabWidget_currentChanged(int index);
    /** open the debug.log from the current datadir */
    void on_openDebugLogfileButton_clicked();

    /** open the discord link */
    void on_discordButton_clicked();
    /** open the block explorer link */
    void on_explorerButton_clicked();
    /** open the Neutron Support Guides link */
    void on_supportButton_clicked();
    /** open the Bootstrap Download link */
    void on_bootstrapButton_clicked();
    /** open the Neutron Github link */
    void on_repoButton_clicked();
    /** open the Neutron CoinMarketCap link */
    void on_cmcButton_clicked();

    /** display messagebox with program parameters (same as bitcoin-qt --help) */
    void on_showCLOptionsButton_clicked();

public Q_SLOTS:
    void clear();

    /** Wallet repair options */
    void walletSalvage();
    void walletRescan();
    void walletZaptxes1();
    void walletZaptxes2();
    void walletUpgrade();
    void walletReindex();

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
    /** Get restart command-line parameters and handle restart */
    void handleRestart(QStringList args);

private:
    Ui::RPCConsole *ui;
    ClientModel *clientModel;
    QStringList history;
    int historyPtr;
    QString cmdBeforeBrowsing;
    QTimer *timer;
    QCompleter *autoCompleter;

    void startExecutor();
    /** Build parameter list for restart */
    void buildParameterlist(QString arg);
    /** Update UI with latest network info from model. */
    void updateNetworkState();
};

#endif // BITCOIN_QT_RPCCONSOLE_H
