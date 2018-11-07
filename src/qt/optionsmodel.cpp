#include "optionsmodel.h"
#include "bitcoinunits.h"
#include <QSettings>

#include "init.h"
#include "walletdb.h"
#include "guiutil.h"

bool fUseDarkTheme;

OptionsModel::OptionsModel(QObject *parent) : QAbstractListModel(parent)
{
    Init();
}

void OptionsModel::addOverriddenOption(const std::string& option)
{
    strOverriddenByCommandLine += QString::fromStdString(option) + "=" + QString::fromStdString(mapArgs[option]) + " ";
}

bool static ApplyProxySettings()
{
    QSettings settings;
    CService addrProxy(settings.value("addrProxy", "127.0.0.1:9050").toString().toStdString());
    int nSocksVersion = 5;
    if (!settings.value("fUseProxy", false).toBool()) {
        addrProxy = CService();
        nSocksVersion = 0;
        return false;
    }
    if (nSocksVersion && !addrProxy.IsValid())
        return false;
    if (!IsLimited(NET_IPV4))
        SetProxy(NET_IPV4, addrProxy);
    if (nSocksVersion) {
        if (!IsLimited(NET_IPV6))
            SetProxy(NET_IPV6, addrProxy);
        SetNameProxy(addrProxy);
    }
    return true;
}

void OptionsModel::Init()
{
    QSettings settings;

    // These are Qt-only settings:
    nDisplayUnit = settings.value("nDisplayUnit", BitcoinUnits::BTC).toInt();
    bDisplayAddresses = settings.value("bDisplayAddresses", false).toBool();
    fMinimizeToTray = settings.value("fMinimizeToTray", false).toBool();
    fMinimizeOnClose = settings.value("fMinimizeOnClose", false).toBool();
    fCoinControlFeatures = settings.value("fCoinControlFeatures", false).toBool();
    nTransactionFee = settings.value("nTransactionFee").toLongLong();
    nReserveBalance = settings.value("nReserveBalance").toLongLong();
    language = settings.value("language", "").toString();
    fUseDarkTheme = settings.value("fUseDarkTheme", false).toBool();

    if (!settings.contains("nDarksendRounds"))
        settings.setValue("nDarksendRounds", 2);

    if (!settings.contains("nAnonymizeNeutronAmount"))
        settings.setValue("nAnonymizeNeutronAmount", 1000);

    nDarksendRounds = settings.value("nDarksendRounds").toLongLong();
    nAnonymizeNeutronAmount = settings.value("nAnonymizeNeutronAmount").toLongLong();

    // These are shared with core Bitcoin; we want
    // command-line options to override the GUI settings:
    if (settings.contains("fUseUPnP"))
        SoftSetBoolArg("-upnp", settings.value("fUseUPnP").toBool());

    if (!settings.contains("fUseUPnP"))
        settings.setValue("fUseUPnP", DEFAULT_UPNP);
    if (!SoftSetBoolArg("-upnp", settings.value("fUseUPnP").toBool()))
        addOverriddenOption("-upnp");

    if (settings.contains("addrProxy") && settings.value("fUseProxy").toBool())
        SoftSetArg("-proxy", settings.value("addrProxy").toString().toStdString());
    if (settings.contains("nSocksVersion") && settings.value("fUseProxy").toBool())
        SoftSetArg("-socks", settings.value("nSocksVersion").toString().toStdString());
    if (settings.contains("detachDB"))
        SoftSetBoolArg("-detachdb", settings.value("detachDB").toBool());
    if (!language.isEmpty())
        SoftSetArg("-lang", language.toStdString());
    if (settings.contains("nDarksendRounds"))
        SoftSetArg("-darksendrounds", settings.value("nDarksendRounds").toString().toStdString());
    if (settings.contains("nAnonymizeNeutronAmount"))
        SoftSetArg("-anonymizeneutronamount", settings.value("nAnonymizeNeutronAmount").toString().toStdString());
 }

int OptionsModel::rowCount(const QModelIndex & parent) const
{
    return OptionIDRowCount;
}

QVariant OptionsModel::data(const QModelIndex & index, int role) const
{
    if(role == Qt::EditRole)
    {
        QSettings settings;
        switch(index.row())
        {
        case StartAtStartup:
            return QVariant(GUIUtil::GetStartOnSystemStartup());
        case MinimizeToTray:
            return QVariant(fMinimizeToTray);
        case MapPortUPnP:
#ifdef USE_UPNP
            return settings.value("fUseUPnP");
#else
            return false;
#endif
        case MinimizeOnClose:
            return QVariant(fMinimizeOnClose);
        case ProxyUse:
            return settings.value("fUseProxy", false);
        case ProxyIP: {
            proxyType proxy;
            if (GetProxy(NET_IPV4, proxy))
                return QVariant(QString::fromStdString(proxy.proxy.ToStringIP()));
            else
                return QVariant(QString::fromStdString("127.0.0.1"));
        }
        case ProxyPort: {
            proxyType proxy;
            if (GetProxy(NET_IPV4, proxy))
                return QVariant(proxy.proxy.GetPort());
            else
                return QVariant(9050);
        }
        case Fee:
            return QVariant((qint64) nTransactionFee);
        case ReserveBalance:
            return QVariant((qint64) nReserveBalance);
        case DisplayUnit:
            return QVariant(nDisplayUnit);
        case DisplayAddresses:
            return QVariant(bDisplayAddresses);
        case DetachDatabases:
            return QVariant(bitdb.GetDetach());
        case Language:
            return settings.value("language", "");
        case CoinControlFeatures:
            return QVariant(fCoinControlFeatures);
        case UseDarkTheme:
            return QVariant(fUseDarkTheme);
        default:
            return QVariant();
        }
    }
    return QVariant();
}

bool OptionsModel::setData(const QModelIndex & index, const QVariant & value, int role)
{
    bool successful = true; /* set to false on parse error */
    if(role == Qt::EditRole)
    {
        QSettings settings;
        switch(index.row())
        {
        case StartAtStartup:
            successful = GUIUtil::SetStartOnSystemStartup(value.toBool());
            break;
        case MinimizeToTray:
            fMinimizeToTray = value.toBool();
            settings.setValue("fMinimizeToTray", fMinimizeToTray);
            break;
        case MapPortUPnP:
            settings.setValue("fUseUPnP", value.toBool());
            MapPort();
            break;
        case MinimizeOnClose:
            fMinimizeOnClose = value.toBool();
            settings.setValue("fMinimizeOnClose", fMinimizeOnClose);
            break;
        case ProxyUse:
            settings.setValue("fUseProxy", value.toBool());
            ApplyProxySettings();
            break;
        case ProxyIP: {
            // contains current IP at index 0 and current port at index 1
            QStringList strlIpPort = settings.value("addrProxy").toString().split(":", QString::SkipEmptyParts);
            // if that key doesn't exist or has a changed IP
            if (!settings.contains("addrProxy") || strlIpPort.at(0) != value.toString()) {
                // construct new value from new IP and current port
                QString strNewValue = value.toString() + ":" + strlIpPort.at(1);
                settings.setValue("addrProxy", strNewValue);
                successful = ApplyProxySettings();
            }
        }
        break;
        case ProxyPort: {
            // contains current IP at index 0 and current port at index 1
            QStringList strlIpPort = settings.value("addrProxy").toString().split(":", QString::SkipEmptyParts);
            // if that key doesn't exist or has a changed port
            if (!settings.contains("addrProxy") || strlIpPort.at(1) != value.toString()) {
                // construct new value from current IP and new port
                QString strNewValue = strlIpPort.at(0) + ":" + value.toString();
                settings.setValue("addrProxy", strNewValue);
                successful = ApplyProxySettings();
            }
        }
        break;

        case Fee:
            nTransactionFee = value.toLongLong();
            settings.setValue("nTransactionFee", (qint64) nTransactionFee);
            emit transactionFeeChanged(nTransactionFee);
            break;
        case ReserveBalance:
            nReserveBalance = value.toLongLong();
            settings.setValue("nReserveBalance", (qint64) nReserveBalance);
            emit reserveBalanceChanged(nReserveBalance);
            break;
        case DisplayUnit:
            nDisplayUnit = value.toInt();
            settings.setValue("nDisplayUnit", nDisplayUnit);
            emit displayUnitChanged(nDisplayUnit);
            break;
        case DisplayAddresses:
            bDisplayAddresses = value.toBool();
            settings.setValue("bDisplayAddresses", bDisplayAddresses);
            break;
        case DetachDatabases: {
            bool fDetachDB = value.toBool();
            bitdb.SetDetach(fDetachDB);
            settings.setValue("detachDB", fDetachDB);
            }
            break;
        case Language:
            settings.setValue("language", value);
            break;
        case CoinControlFeatures: {
            fCoinControlFeatures = value.toBool();
            settings.setValue("fCoinControlFeatures", fCoinControlFeatures);
            emit coinControlFeaturesChanged(fCoinControlFeatures);
            }
            break;
        case DarksendRounds:
            nDarksendRounds = value.toInt();
            settings.setValue("nDarksendRounds", nDarksendRounds);
            emit darksendRoundsChanged(nDarksendRounds);
            break;
        case anonymizeNeutronAmount:
            nAnonymizeNeutronAmount = value.toInt();
            settings.setValue("nAnonymizeNeutronAmount", nAnonymizeNeutronAmount);
            emit anonymizeNeutronAmountChanged(nAnonymizeNeutronAmount);
            break;
        case UseDarkTheme:
            fUseDarkTheme = value.toBool();
            settings.setValue("fUseDarkTheme", fUseDarkTheme);
            break;
        default:
            break;
        }
    }
    emit dataChanged(index, index);

    return successful;
}

qint64 OptionsModel::getTransactionFee()
{
    return nTransactionFee;
}

qint64 OptionsModel::getReserveBalance()
{
    return nReserveBalance;
}

bool OptionsModel::getCoinControlFeatures()
{
    return fCoinControlFeatures;
}

bool OptionsModel::getMinimizeToTray()
{
    return fMinimizeToTray;
}

bool OptionsModel::getMinimizeOnClose()
{
    return fMinimizeOnClose;
}

int OptionsModel::getDisplayUnit()
{
    return nDisplayUnit;
}

bool OptionsModel::getDisplayAddresses()
{
    return bDisplayAddresses;
}
