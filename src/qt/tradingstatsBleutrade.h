#ifndef TRADINGSTATSBLEUTRADE_H
#define TRADINGSTATSBLEUTRADE_H

#include "clientmodel.h"
#include "main.h"
#include "wallet.h"
#include "base58.h"

#include "json_spirit.h"

#include <QWidget>
#include <QObject>
#include <QLabel>
#include <QtNetwork/QtNetwork>
#include <qcustomplot.h>
#include <QCoreApplication>
#include <QDebug>
#include <QApplication>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <QJsonValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>
#include <QJsonArray>

#ifndef BOOST_SPIRIT_THREADSAFE
#define BOOST_SPIRIT_THREADSAFE
#endif

using namespace json_spirit;
using namespace std;

extern double _dScPriceLastBleutrade;
extern double _dBtcPriceCurrentBleutrade;
extern double _dBtcPriceLastBleutrade;

namespace Ui {
class TradingStatsBleutrade;
}

class ClientModel;

class TradingStatsBleutrade : public QWidget
{
    Q_OBJECT

public:
    explicit TradingStatsBleutrade(QWidget* parent = 0);
    ~TradingStatsBleutrade();

    void openBleutrade();

    void pollAPIs();

    void processOverview();

    //API Call Methods
    void coinbasePrice(QNetworkReply* response);
    void bleutradeMarketSummary(QNetworkReply* response);
    void bleutradeTrades(QNetworkReply* response);
    void bleutradeOrders(QNetworkReply* response);

    const mValue& getPairValue(const mObject& obj, const std::string& name);

    void updateLabel(QLabel* qLabel, double d1, double d2, QString prefix, int decimalPlaces);
    void updateLabel(QLabel* qLabel, double d1, double d2, QString prefix, QString suffix, int decimalPlaces);

    void setModel(ClientModel* model);

private:
    void getRequest(const QString& url);

signals:
    void networkError(QNetworkReply::NetworkError err);

public slots:
    void parseNetworkResponse(QNetworkReply* response);

private slots:
    void on_btnConvertNeutron_clicked();
    void on_btnUpdateMarketData_clicked();

private:
    QNetworkAccessManager m_nam;
    Ui::TradingStatsBleutrade* ui;
    ClientModel* model;
};

class BleutradeMarketSummary {
private:
    double _askCurrent;
    double _askPrev;
    double _baseVolumeCurrent;
    double _baseVolumePrev;
    double _bidCurrent;
    double _bidPrev;
    double _highCurrent;
    double _highPrev;
    double _lastCurrent;
    double _lastPrev;
    double _lowCurrent;
    double _lowPrev;
    double _prevDayCurrent;
    double _prevDayPrev;
    double _volumeCurrent;
    double _volumePrev;

    QString _created;
    QString _displayMarketName;
    QString _marketName;
    QString _timeStamp;

public:
    BleutradeMarketSummary()
    {
        _askCurrent = 0;
        _askPrev = 0;
        _baseVolumeCurrent = 0;
        _baseVolumePrev = 0;
        _bidCurrent = 0;
        _bidPrev = 0;
        _highCurrent = 0;
        _highPrev = 0;
        _lastCurrent = 0;
        _lastPrev = 0;
        _lowCurrent = 0;
        _lowPrev = 0;
        _prevDayCurrent = 0;
        _prevDayPrev = 0;
        _volumeCurrent = 0;
        _volumePrev = 0;

        _created = "";
        _displayMarketName = "";
        _marketName = "";
        _timeStamp = "";
    }

    double getAskCurrent(double) { return _askCurrent; }
    QString getAskCurrent(QString) { return QString::number(_askCurrent, 'f', 8); }
    void setAskCurrent(double value) { _askCurrent = value; }

    double getAskPrev(double) { return _askPrev; }
    QString getAskPrev(QString) { return QString::number(_askPrev, 'f', 8); }
    void setAskPrev(double value) { _askPrev = value; }

    double getBaseVolumeCurrent(double) { return _baseVolumeCurrent; }
    QString getBaseVolumeCurrent(QString) { return QString::number(_baseVolumeCurrent, 'f', 8); }
    void setBaseVolumeCurrent(double value) { _baseVolumeCurrent = value; }

    double getBaseVolumePrev(double) { return _baseVolumePrev; }
    QString getBaseVolumePrev(QString) { return QString::number(_baseVolumePrev, 'f', 8); }
    void setBaseVolumePrev(double value) { _baseVolumePrev = value; }

    double getBidCurrent(double) { return _bidCurrent; }
    QString getBidCurrent(QString) { return QString::number(_bidCurrent, 'f', 8); }
    void setBidCurrent(double value) { _bidCurrent = value; }

    double getBidPrev(double) { return _bidPrev; }
    QString getBidPrev(QString) { return QString::number(_bidPrev, 'f', 8); }
    void setBidPrev(double value) { _bidPrev = value; }

    double getHighCurrent(double) { return _highCurrent; }
    QString getHighCurrent(QString) { return QString::number(_highCurrent, 'f', 8); }
    void setHighCurrent(double value) { _highCurrent = value; }

    double getHighPrev(double) { return _highPrev; }
    QString getHighPrev(QString) { return QString::number(_highPrev, 'f', 8); }
    void setHighPrev(double value) { _highPrev = value; }

    double getLastCurrent(double) { return _lastCurrent; }
    QString getLastCurrent(QString) { return QString::number(_lastCurrent, 'f', 8); }
    void setLastCurrent(double value) { _lastCurrent = value; }

    double getLastPrev(double) { return _lastPrev; }
    QString getLastPrev(QString) { return QString::number(_lastPrev, 'f', 8); }
    void setLastPrev(double value) { _lastPrev = value; }

    double getLowCurrent(double) { return _lowCurrent; }
    QString getLowCurrent(QString) { return QString::number(_lowCurrent, 'f', 8); }
    void setLowCurrent(double value) { _lowCurrent = value; }

    double getLowPrev(double) { return _lowPrev; }
    QString getLowPrev(QString) { return QString::number(_lowPrev, 'f', 8); }
    void setLowPrev(double value) { _lowPrev = value; }

    double getPrevDayCurrent(double) { return _prevDayCurrent; }
    QString getPrevDayCurrent(QString) { return QString::number(_prevDayCurrent, 'f', 8); }
    void setPrevDayCurrent(double value) { _prevDayCurrent = value; }

    double getPrevDayPrev(double) { return _prevDayPrev; }
    QString getPrevDayPrev(QString) { return QString::number(_prevDayPrev, 'f', 8); }
    void setPrevDayPrev(double value) { _prevDayPrev = value; }

    double getVolumeCurrent(double) { return _volumeCurrent; }
    QString getVolumeCurrent(QString) { return QString::number(_volumeCurrent, 'f', 8); }
    void setVolumeCurrent(double value) { _volumeCurrent = value; }

    double getVolumePrev(double) { return _volumePrev; }
    QString getVolumePrev(QString) { return QString::number(_volumePrev, 'f', 8); }
    void setVolumePrev(double value) { _volumePrev = value; }

    QString getCreated() { return _created; }
    void setCreated(std::string value)
    {
        QString ret = QString::fromStdString(value);

        ret.replace("T", " ");
        ret.truncate(ret.indexOf("."));

        _created = ret;
    }

    QString _getDisplayMarketName() { return _displayMarketName; }
    void setDisplayMarketName(std::string value) { _displayMarketName = QString::fromStdString(value); }

    QString getMarketName() { return _marketName; }
    void setMarketName(std::string value) { _marketName = QString::fromStdString(value); }

    QString getTimeStamp() { return _timeStamp; }
    void setTimeStamp(std::string value)
    {
        QString ret = QString::fromStdString(value);

        ret.replace("T", " ");
        ret.truncate(ret.indexOf("."));

        _timeStamp = ret;
    }
};

class BleutradeTrades {
private:
    double _id;
    QString _timeStamp;
    double _quantity;
    double _price;
    double _total;
    QString _fillType;
    QString _orderType;

public:
    BleutradeTrades()
    {
        _id = 0;
        _timeStamp = "";
        _quantity = 0;
        _price = 0;
        _total = 0;
        _fillType = "";
        _orderType = "";
    }

    double getId(double) { return _id; }
    QString getId(QString) { return QString::number(_id, 'f', 8); }
    void setId(double value) { _id = value; }

    QString getTimeStamp() { return _timeStamp; }
    void setTimeStamp(std::string value)
    {
        QString ret = QString::fromStdString(value);

        ret.replace("T", " ");
        ret.truncate(ret.indexOf("."));

        _timeStamp = ret;
    }

    double getQuantity(double) { return _quantity; }
    QString getQuantity(QString) { return QString::number(_quantity, 'f', 8); }
    void setQuantity(double value) { _quantity = value; }

    double getPrice(double) { return _price; }
    QString getPrice(QString) { return QString::number(_price, 'f', 8); }
    void setPrice(double value) { _price = value; }

    double getTotal(double) { return _total; }
    QString getTotal(QString) { return QString::number(_total, 'f', 8); }
    void setTotal(double value) { _total = value; }

    QString getFillType() { return _fillType; }
    void setFillType(std::string value) { _fillType = QString::fromStdString(value); }

    QString getOrderType() { return _orderType; }
    void setOrderType(std::string value)
    {
        _orderType = QString::fromStdString(value) == "BUY"
                ? "Buy" : QString::fromStdString(value) == "SELL"
                ? "Sell" : "Unknown";
    }
};

class BleutradeOrders {
private:
    double _quantity;
    double _price;
    QString _orderType;

public:
    BleutradeOrders()
    {
        _quantity = 0;
        _price = 0;
        _orderType = "";
    }

    double getQuantity(double) { return _quantity; }
    QString getQuantity(QString) { return QString::number(_quantity, 'f', 8); }
    void setQuantity(double value) { _quantity = value; }

    double getPrice(double) { return _price; }
    QString getPrice(QString) { return QString::number(_price, 'f', 8); }
    void setPrice(double value) { _price = value; }

    QString getOrderType() { return _orderType; }
    void setOrderType(std::string value) { _orderType = QString::fromStdString(value); }
};

#endif // TRADINGSTATSBLEUTRADE_H
