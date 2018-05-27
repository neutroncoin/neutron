#ifndef CLIENTMODEL_H
#define CLIENTMODEL_H

#include <QObject>

class OptionsModel;
class AddressTableModel;
class TransactionTableModel;
class CWallet;

QT_BEGIN_NAMESPACE
class QDateTime;
class QTimer;
QT_END_NAMESPACE

enum NumConnections {
    CONNECTIONS_NONE = 0,
    CONNECTIONS_IN   = (1U << 0),
    CONNECTIONS_OUT  = (1U << 1),
    CONNECTIONS_ALL  = (CONNECTIONS_IN | CONNECTIONS_OUT),
};

/** Model for Bitcoin network client. */
class ClientModel : public QObject
{
    Q_OBJECT
public:
    explicit ClientModel(OptionsModel *optionsModel, QObject *parent = 0);
    ~ClientModel();

    OptionsModel *getOptionsModel();

    //! Return number of connections, default is in- and outbound (total)
    int getNumConnections(unsigned int flags = CONNECTIONS_ALL) const;
    QString getMasternodeCountString() const;
    int getNumBlocks() const;
    int getNumBlocksAtStartup();

    //! Return number of transactions in the mempool
    long getMempoolSize() const;

    QDateTime getLastBlockDate() const;

    //! Return true if client connected to testnet
    bool isTestNet() const;
    //! Return true if core is doing initial block download
    bool inInitialBlockDownload() const;
    //! Return conservative estimate of total number of blocks, or 0 if unknown
    int getNumBlocksOfPeers() const;
    //! Return warnings to be displayed in status bar
    QString getStatusBarWarnings() const;

    QString formatFullVersion() const;
    QString formatBuildDate() const;
    QString clientName() const;
    QString formatClientStartupTime() const;

private:
    OptionsModel *optionsModel;
    QString cachedMasternodeCountString;

    int cachedNumBlocks;
    int cachedNumBlocksOfPeers;

    int numBlocksAtStartup;

    QTimer *pollTimer;
    QTimer *pollMnTimer;

    void subscribeToCoreSignals();
    void unsubscribeFromCoreSignals();
Q_SIGNALS:
    void numConnectionsChanged(int count);
    void strMasternodesChanged(const QString &strMasternodes);
    void numBlocksChanged(int count, int countOfPeers);
    void mempoolSizeChanged(long count);

    //! Asynchronous error notification
    void error(const QString &title, const QString &message, bool modal);

public slots:
    void updateTimer();
    void updateMnTimer();
    void updateNumConnections(int numConnections);
    void updateAlert(const QString &hash, int status);
};

#endif // CLIENTMODEL_H
