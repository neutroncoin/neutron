#include "tradingstatsBittrex.h"
#include "ui_tradingstatsBittrex.h"
#include "main.h"
#include "wallet.h"
#include "base58.h"
#include "clientmodel.h"
#include "bitcoinrpc.h"

#include <sstream>
#include <string>

#include <QDesktopServices>
#include <QStackedWidget>
#include <QString>

using namespace json_spirit;
using namespace std;

#define QSTRING_DOUBLE(var) QString::number(var, 'f', 8)

//Coinbase API
const QString apiCoinbasePrice = "https://api.coinbase.com/v1/currencies/exchange_rates";

//Bittrex API
const QString apiBittrexMarketSummary = "https://bittrex.com/api/v1.1/public/getmarketsummary?market=BTC-LTC";
const QString apiBittrexTrades = "https://bittrex.com/api/v1.1/public/getmarkethistory?market=BTC-LTC&count=40";
const QString apiBittrexOrders = "https://bittrex.com/api/v1.1/public/getorderbook?market=BTC-LTC&type=both&depth=50";

//Common Globals
double _dScPriceLastBittrex = 0;
double _dBtcPriceCurrentBittrex = 0;
double _dBtcPriceLastBittrex = 0;

//Bittrex Globals
BittrexMarketSummary* _bittrexMarketSummary = new BittrexMarketSummary();
BittrexTrades* _bittrexTrades = new BittrexTrades();
BittrexOrders* _bittrexOrders = new BittrexOrders();

TradingStatsBittrex::TradingStatsBittrex(QWidget* parent) : QWidget(parent), ui(new Ui::TradingStatsBittrex)
{
    //TODO: Complete multi-threading so we don't have to call this as a primer
    getRequest(apiCoinbasePrice);

    ui->setupUi(this);

    ui->qCustomPlotBittrexTrades->addGraph();
    ui->qCustomPlotBittrexOrderDepth->addGraph();
    ui->qCustomPlotBittrexOrderDepth->addGraph();

    QObject::connect(&m_nam, SIGNAL(finished(QNetworkReply*)), this, SLOT(parseNetworkResponse(QNetworkReply*)), Qt::AutoConnection);

    //One time primer
    pollAPIs();
}

void TradingStatsBittrex::on_btnConvertNeutron_clicked() {
    double NeutronQty = ui->txtConvertNeutronQty->text().toDouble();
    double totalBtc = _bittrexMarketSummary->getLastCurrent(double()) * NeutronQty;
    double totalUsd = totalBtc * _dBtcPriceCurrentBittrex;

    ui->lblConvertNeutronResults->setText("$  " + QString::number(totalUsd, 'f', 2) +
                                           "  /  BTC  " + QString::number(totalBtc, 'f', 8));

}

void TradingStatsBittrex::on_btnUpdateMarketData_clicked()
{
    pollAPIs();
}

void TradingStatsBittrex::openBittrex()
{
    QDesktopServices::openUrl(QUrl("https://www.bittrex.com/Market/Index?MarketName=BTC-LTC"));
}

void TradingStatsBittrex::pollAPIs()
{
    ui->iconOverviewUpdateWait->setVisible(true);

    getRequest(apiCoinbasePrice);

    getRequest(apiBittrexMarketSummary);
    getRequest(apiBittrexTrades);
    getRequest(apiBittrexOrders);
}

void TradingStatsBittrex::processOverview()
{
}

void TradingStatsBittrex::getRequest(const QString &urlString)
{
    QUrl url(urlString);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=utf-8");
    m_nam.get(req);
}

void TradingStatsBittrex::parseNetworkResponse(QNetworkReply* response)
{
    QUrl apiCall = response->url();

    if (response->error() != QNetworkReply::NoError) {
        //Communication error has occurred
        emit networkError(response->error());
        return;
    }

    if (apiCall == apiCoinbasePrice) { coinbasePrice(response); }
    else if (apiCall == apiBittrexMarketSummary) { bittrexMarketSummary(response); }
    else if (apiCall == apiBittrexTrades) { bittrexTrades(response); }
    else if (apiCall == apiBittrexOrders) { bittrexOrders(response); }
    else { }  //Sould NEVER get here unless something went completely awry

    if (_bittrexMarketSummary->getLastPrev(double()) > 0)
    {
        ui->iconOverviewUpdateWait->setVisible(false);
    }

    processOverview();

    response->deleteLater();
}

/*************************************************************************************
 * Method: TradingStatsBittrex::coinbasePrice
 * Parameter(s): QNetworkReply* response
 *
 * Unauthenticated resource that returns BTC to fiat (and vice versus) exchange rates in various currencies.
 * It has keys for both btc_to_xxx and xxx_to_btc so you can convert either way.
 * The key always contains downcase representations of the currency ISO.
 * Note that some small numbers may use E notation such as 2.8e-05.
 *
 * Response: {"btc_to_pgk":"28.152994","btc_to_gyd":"2743.906541","btc_to_mmk":"11611.550858", ... ,"brl_to_btc":"0.037652"}
 *************************************************************************************/
void TradingStatsBittrex::coinbasePrice(QNetworkReply* response) {
    try {
    mValue jsonResponse = new mValue();
    QString apiResponse = response->readAll();

    //Make sure the response is valid
    if(!read_string(apiResponse.toStdString(), jsonResponse)) { return; }

    mObject jsonObject = jsonResponse.get_obj();

    _dBtcPriceCurrentBittrex =  QString::fromStdString(getPairValue(jsonObject, "btc_to_usd").get_str()).toDouble();

    updateLabel(ui->lblOverviewBtcUsdPrice,
                _dBtcPriceCurrentBittrex,
                _dBtcPriceLastBittrex,
                QString::fromUtf8("$  "),
                2);

    _dBtcPriceLastBittrex = _dBtcPriceCurrentBittrex;
    _dScPriceLastBittrex = _dBtcPriceCurrentBittrex * _bittrexMarketSummary->getLastCurrent(double());
    } catch (exception ex) {
        printf("TradingStatsBittrex::coinbasePrice: %s\r\n", ex.what());
    }
}

/*************************************************************************************
 * Method: TradingStatsBittrex::bittrexMarketSummary
 * Parameter(s): QNetworkReply* response
 *
 * Used to get the last 24 hour summary of all active exchanges
 *
 * Parameter(s): None
 * Response:
 * {
 * 	"success" : true,
 * 	"message" : "",
 * 	"result" : [{
 * 	        "MarketName" : "BTC-LTC",
 * 	        "High" : 0.02590000,
 * 	        "Low" : 0.02400000,
 * 	        "Volume" : 114.84340665,
 * 	        "Last" : 0.02480000,
 * 	        "BaseVolume" : 2.85028800,
 * 	        "TimeStamp" : "2014-04-19T20:49:23.483"
 *         }, {
 * 	        "MarketName" : "BTC-WC",
 * 	        "High" : 0.00002456,
 * 	        "Low" : 0.00001352,
 * 	        "Volume" : 4574426.27271220,
 * 	        "Last" : 0.00002006,
 * 	        "BaseVolume" : 82.96629666,
 * 	        "TimeStamp" : "2014-04-19T20:49:50.053"
 *         }
 * 	]
 * }
 *************************************************************************************/
void TradingStatsBittrex::bittrexMarketSummary(QNetworkReply* response)
{
    QString apiResponse = response->readAll();

    apiResponse = apiResponse.replace("{\"success\":true,\"message\":\"\",\"result\":[", "").replace("]}","");

    //for(int i = 0; i < qslApiResponse.count(); i++){
        mValue jsonResponse = new mValue();

    //json_spirit does not handle null so make it "null"
    apiResponse.replace("null", "\"null\"");

    //Make sure the response is valid
    if(read_string(apiResponse.toStdString(), jsonResponse)) {
        mObject jsonObject = jsonResponse.get_obj();

        try {
            _bittrexMarketSummary->setHighCurrent(getPairValue(jsonObject, "High").get_real());
            _bittrexMarketSummary->setLowCurrent(getPairValue(jsonObject, "Low").get_real());
            _bittrexMarketSummary->setVolumeCurrent(getPairValue(jsonObject, "Volume").get_real());
            _bittrexMarketSummary->setLastCurrent(getPairValue(jsonObject, "Last").get_real());
            _bittrexMarketSummary->setBaseVolumeCurrent(getPairValue(jsonObject, "BaseVolume").get_real());
            _bittrexMarketSummary->setTimeStamp(getPairValue(jsonObject, "TimeStamp").get_str());
            _bittrexMarketSummary->setBidCurrent(getPairValue(jsonObject, "Bid").get_real());
            _bittrexMarketSummary->setAskCurrent(getPairValue(jsonObject, "Ask").get_real());
            _bittrexMarketSummary->setPrevDayCurrent(getPairValue(jsonObject, "PrevDay").get_real());
        }
        catch (std::exception) {} //API did not return all needed data so skip processing market summary
    }

    updateLabel(ui->lblBittrexHighBtc,
                _bittrexMarketSummary->getHighCurrent(double()),
                _bittrexMarketSummary->getHighPrev(double()),
                QString("BTC "),
                8);

    updateLabel(ui->lblBittrexLowBtc,
                _bittrexMarketSummary->getLowCurrent(double()),
                _bittrexMarketSummary->getLowPrev(double()),
                QString("BTC "),
                8);

    updateLabel(ui->lblBittrexCloseBtc,
                _bittrexMarketSummary->getPrevDayCurrent(double()),
                _bittrexMarketSummary->getPrevDayPrev(double()),
                QString("BTC "),
                8);

    double changeCurrent = (_bittrexMarketSummary->getLastCurrent(double()) - _bittrexMarketSummary->getPrevDayCurrent(double())) / _bittrexMarketSummary->getPrevDayCurrent(double()) * 100;
    double changeLast  = (_bittrexMarketSummary->getLastPrev(double()) - _bittrexMarketSummary->getPrevDayCurrent(double())) / _bittrexMarketSummary->getPrevDayCurrent(double()) * 100;

    QString changeDirection = _bittrexMarketSummary->getLastCurrent(double()) > _bittrexMarketSummary->getPrevDayCurrent(double())
            ? QString("+") : _bittrexMarketSummary->getLastCurrent(double()) < _bittrexMarketSummary->getPrevDayCurrent(double())
            ? QString("") : QString("");

    updateLabel(ui->lblBittrexChangePerc,
                changeCurrent,
                changeLast,
                changeDirection,
                QString("% "),
                2);

    updateLabel(ui->lblBittrexVolumeUsd,
                _bittrexMarketSummary->getBaseVolumeCurrent(double()) * _dBtcPriceCurrentBittrex,
                _bittrexMarketSummary->getBaseVolumePrev(double()) * _dBtcPriceCurrentBittrex,
                QString(""),
                2);

    updateLabel(ui->lblBittrexVolumeSc,
                _bittrexMarketSummary->getVolumeCurrent(double()),
                _bittrexMarketSummary->getVolumePrev(double()),
                QString(""),
                4);

    updateLabel(ui->lblBittrexVolumeBtc,
                _bittrexMarketSummary->getBaseVolumeCurrent(double()),
                _bittrexMarketSummary->getBaseVolumePrev(double()),
                QString(""),
                4);

    updateLabel(ui->lblBittrexLastBtc,
                _bittrexMarketSummary->getLastCurrent(double()),
                _bittrexMarketSummary->getLastPrev(double()),
                QString("BTC "),

                8);

    updateLabel(ui->lblBittrexLastUsd,
                _bittrexMarketSummary->getLastCurrent(double()) * _dBtcPriceCurrentBittrex,
                _bittrexMarketSummary->getLastPrev(double()) * _dBtcPriceCurrentBittrex,
                QString::fromUtf8("$ "),
                8);

    updateLabel(ui->lblBittrexAskBtc,
                _bittrexMarketSummary->getAskCurrent(double()),
                _bittrexMarketSummary->getAskPrev(double()),
                QString("BTC "),
                8);

    updateLabel(ui->lblBittrexAskUsd,
                _bittrexMarketSummary->getAskCurrent(double()) * _dBtcPriceCurrentBittrex,
                _bittrexMarketSummary->getAskPrev(double()) * _dBtcPriceCurrentBittrex,
                QString::fromUtf8("$ "),
                8);

    updateLabel(ui->lblBittrexBidBtc,
                _bittrexMarketSummary->getBidCurrent(double()),
                _bittrexMarketSummary->getBidPrev(double()),
                QString("BTC "),
                8);

    updateLabel(ui->lblBittrexBidUsd,
                _bittrexMarketSummary->getBidCurrent(double()) * _dBtcPriceCurrentBittrex,
                _bittrexMarketSummary->getBidPrev(double()) * _dBtcPriceCurrentBittrex,
                QString::fromUtf8("$ "),
                8);

    _bittrexMarketSummary->setAskPrev(_bittrexMarketSummary->getAskCurrent(double()));
    _bittrexMarketSummary->setBaseVolumePrev(_bittrexMarketSummary->getBaseVolumeCurrent(double()));
    _bittrexMarketSummary->setBidPrev(_bittrexMarketSummary->getBidCurrent(double()));
    _bittrexMarketSummary->setHighPrev(_bittrexMarketSummary->getHighCurrent(double()));
    _bittrexMarketSummary->setLowPrev(_bittrexMarketSummary->getLowCurrent(double()));
    _bittrexMarketSummary->setPrevDayPrev(_bittrexMarketSummary->getPrevDayCurrent(double()));
    _bittrexMarketSummary->setLastPrev(_bittrexMarketSummary->getLastCurrent(double()));
    _bittrexMarketSummary->setVolumePrev(_bittrexMarketSummary->getVolumeCurrent(double()));

    _dScPriceLastBittrex = _dBtcPriceCurrentBittrex * _bittrexMarketSummary->getLastCurrent(double());
}
/*************************************************************************************
 * Method: TradingStatsBittrex::bittrexTrades
 * Parameter(s): QNetworkReply* response
 *
 * Used to retrieve the latest trades that have occurred for a specific market
 * Parameter(s):
 * market (requiblack): a string literal for the market (ex: BTC-LTC)
 * count (optional): a number between 1-100 for the number of entries to return (default = 20)
 *
 *     {
 * 	"success" : true,
 * 	"message" : "",
 * 	"result" : [{
 * 			"OrderId" : "12323",
 * 			"TimeStamp" : "2014-02-25T07:40:08.68",
 * 			"Quantity" : 185.06100000,
 * 			"Price" : 0.00000174,
 * 			"Total" : 0.00032200
 * 		}, {
 * 			"OrderUuid" : "12322",
 * 			"TimeStamp" : "2014-02-25T07:39:18.603",
 * 			"Quantity" : 10.74500000,
 * 			"Price" : 0.00000172,
 * 			"Total" : 0.00001848
 * 		}, {
 * 			"OrderUuid" : "12321",
 * 			"TimeStamp" : "2014-02-25T07:39:18.6",
 * 			"Quantity" : 5.62100000,
 * 			"Price" : 0.00000172,
 * 			"Total" : 0.00000966
 * 		}, {
 * 			"OrderUuid" : "12319",
 * 			"TimeStamp" : "2014-02-25T07:39:18.6",
 * 			"Quantity" : 76.23000000,
 * 			"Price" : 0.00000173,
 * 			"Total" : 0.00013187
 * 		}, {
 * 			"OrderUuid" : "12317",
 * 			"TimeStamp" : "2014-02-25T07:39:18.6",
 * 			"Quantity" : 52.47500000,
 * 			"Price" : 0.00000174,
 * 			"Total" : 0.00009130
 * 		}
 * 	]
 * }
 *************************************************************************************/
void TradingStatsBittrex::bittrexTrades(QNetworkReply* response)
{
    int z = 0;
    double high = 0, low = 0;

    ui->tblBittrexTrades->clear();
    ui->tblBittrexTrades->setColumnWidth(0, 60);
    ui->tblBittrexTrades->setColumnWidth(1, 110);
    ui->tblBittrexTrades->setColumnWidth(2, 110);
    ui->tblBittrexTrades->setColumnWidth(3, 100);
    ui->tblBittrexTrades->setColumnWidth(4, 160);
    ui->tblBittrexTrades->setSortingEnabled(false);

    QString apiResponse = response->readAll();

    apiResponse = apiResponse.replace("{\"success\":true,\"message\":\"\",\"result\":[", "").replace("]}","").replace("},{", "}{");

    QStringList qslApiResponse = apiResponse.split("{", QString::SkipEmptyParts);

    int tradeCount = qslApiResponse.count();
    QVector<double> xAxis(tradeCount), yAxis(tradeCount);

    for(int i = 0; i < tradeCount; i++){
        mValue jsonResponse = new mValue();

        //Fix missing leading brace caused by split string, otherwise it will not be recognized an an mObject
        qslApiResponse[i].replace("\"Id", "{\"Id");

        //json_spirit does not handle null so make it "null"
        qslApiResponse[i].replace("null", "\"null\"");

        //Make sure the response is valid
        if(read_string(qslApiResponse[i].toStdString(), jsonResponse)) {
            mObject jsonObject = jsonResponse.get_obj();

            try
            {
                _bittrexTrades->setId(getPairValue(jsonObject, "Id").get_real());
                _bittrexTrades->setTimeStamp(getPairValue(jsonObject, "TimeStamp").get_str());
                _bittrexTrades->setQuantity(getPairValue(jsonObject, "Quantity").get_real());
                _bittrexTrades->setPrice(getPairValue(jsonObject, "Price").get_real());
                _bittrexTrades->setTotal(getPairValue(jsonObject, "Total").get_real());
                _bittrexTrades->setFillType(getPairValue(jsonObject, "FillType").get_str());
                _bittrexTrades->setOrderType(getPairValue(jsonObject, "OrderType").get_str());
            }
            catch (std::exception) {} //API did not return all needed data so skip this trade

            QTreeWidgetItem * qtTrades = new QTreeWidgetItem();

            qtTrades->setText(0, _bittrexTrades->getOrderType());
            qtTrades->setText(1, _bittrexTrades->getPrice(QString()));
            qtTrades->setText(2, _bittrexTrades->getQuantity(QString()));
            qtTrades->setText(3, _bittrexTrades->getTotal(QString()));
            qtTrades->setText(4, _bittrexTrades->getTimeStamp());

            ui->tblBittrexTrades->addTopLevelItem(qtTrades);

            xAxis[z] = tradeCount - z;
            yAxis[z] = _bittrexTrades->getPrice(double()) * 100000000;

            high = _bittrexTrades->getPrice(double()) > high ? _bittrexTrades->getPrice(double()) : high;
            low = _bittrexTrades->getPrice(double()) < low ? _bittrexTrades->getPrice(double()) : low;

            z++;
        }
    }

    high *=  100000000;
    low *=  100000000;

    ui->qCustomPlotBittrexTrades->graph(0)->setData(xAxis, yAxis);
    ui->qCustomPlotBittrexTrades->graph(0)->setPen(QPen(QColor(34, 177, 76)));
    ui->qCustomPlotBittrexTrades->graph(0)->setBrush(QBrush(QColor(34, 177, 76, 20)));

    ui->qCustomPlotBittrexTrades->xAxis->setRange(1, tradeCount);
    ui->qCustomPlotBittrexTrades->yAxis->setRange(low, high);

    ui->qCustomPlotBittrexTrades->replot();

}
/*************************************************************************************
 * Method: TradingStatsBittrex::bittrexOrders
 * Parameter(s): QNetworkReply* response
 *
 * Used to get retrieve the orderbook for a given market
 *
 * Parameters:
 * market	(requiblack)	a string literal for the market (ex: BTC-LTC)
 * type	(requiblack)	buy, sell or both to identify the type of orderbook to return.
 * depth	(optional)	defaults to 20 - how deep of an order book to retrieve. Max is 100
 *
 * Response
 *     {
 * 	"success" : true,
 * 	"message" : "",
 * 	"result" : {
 * 		"buy" : [{
 * 				"Quantity" : 12.37000000,
 * 				"Rate" : 0.02525000
 * 			}
 * 		],
 * 		"sell" : [{
 * 				"Quantity" : 32.55412402,
 * 				"Rate" : 0.02540000
 * 			}, {
 * 				"Quantity" : 60.00000000,
 * 				"Rate" : 0.02550000
 * 			}, {
 * 				"Quantity" : 60.00000000,
 * 				"Rate" : 0.02575000
 * 			}, {
 * 				"Quantity" : 84.00000000,
 * 				"Rate" : 0.02600000
 * 			}
 * 		]
 * 	}
 * }
 ************************************************************************************/
void TradingStatsBittrex::bittrexOrders(QNetworkReply* response)
{
    int z = 0;
    double high = 0;
    double low = 100000;
    double sumBuys = 0;
    double sumSells = 0;
    double sumHighest = 0;

    ui->qTreeWidgetBittrexBuy->clear();
    ui->qTreeWidgetBittrexBuy->sortByColumn(0, Qt::DescendingOrder);
    ui->qTreeWidgetBittrexBuy->setSortingEnabled(true);

    ui->qTreeWidgetBittrexSell->clear();
    ui->qTreeWidgetBittrexSell->sortByColumn(0, Qt::AscendingOrder);
    ui->qTreeWidgetBittrexSell->setSortingEnabled(true);

    QString apiResponse = response->readAll();

    apiResponse = apiResponse.replace("{\"success\":true,\"message\":\"\",\"result\":{\"buy\":[", "");
    QStringList qslApiResponse = apiResponse.split("],\"sell\":[");

    QStringList qslApiResponseBuys = qslApiResponse[0].replace("},{", "}{").split("{", QString::SkipEmptyParts);
    QStringList qslApiResponseSells = qslApiResponse[1].replace("]}}","").replace("},{", "}{").split("{", QString::SkipEmptyParts);

    //Use shorest depth as limit and use buy length if they are the same
    int depth = qslApiResponseBuys.length() > qslApiResponseSells.length()
            ? qslApiResponseSells.length() : qslApiResponseSells.length() > qslApiResponseBuys.length()
            ? qslApiResponseBuys.length() : qslApiResponseBuys.length();

    //Prevent overflow by limiting depth to 50
    //Also check for odd number of orders and drop the last one
    //To avoid an overflow when there are less than 50 orders
    depth = depth > 50
            ? 50 : depth % 2 == 1
            ? depth - 1 : depth;

    QVector<double> xAxisBuys(depth), yAxisBuys(depth);
    QVector<double> xAxisSells(depth), yAxisSells(depth);

    for(int i = 0; i < depth; i++){
        mValue jsonResponse = new mValue();

        //Fix missing leading brace caused by split string, otherwise it will not be recognized an an mObject
        qslApiResponseBuys[i].replace("\"Quantity", "{\"Quantity");
        qslApiResponseSells[i].replace("\"Quantity", "{\"Quantity");

        //json_spirit does not handle null so make it "null"
        qslApiResponseBuys[i].replace("null", "\"null\"");
        qslApiResponseSells[i].replace("null", "\"null\"");

        //Make sure the response is valid
        if(read_string(qslApiResponseBuys[i].toStdString(), jsonResponse)) {
            mObject jsonObjectBuys = jsonResponse.get_obj();

            try
            {
                _bittrexOrders->setQuantity(getPairValue(jsonObjectBuys, "Quantity").get_real());
                _bittrexOrders->setPrice(getPairValue(jsonObjectBuys, "Rate").get_real());
                _bittrexOrders->setOrderType("Buy");
            }
            catch (std::exception) {} //API did not return all needed data so skip this order

            QTreeWidgetItem * qtBuys = new QTreeWidgetItem();

            qtBuys->setText(0, _bittrexOrders->getPrice(QString()));
            qtBuys->setText(1, _bittrexOrders->getQuantity(QString()));

            ui->qTreeWidgetBittrexBuy->addTopLevelItem(qtBuys);

            sumBuys += _bittrexOrders->getQuantity(double());
            xAxisBuys[z] = _bittrexOrders->getPrice(double()) * 100000000;
            yAxisBuys[z] = sumBuys;
        }

        high = _bittrexOrders->getPrice(double()) > high ? _bittrexOrders->getPrice(double()) : high;
        low = _bittrexOrders->getPrice(double()) < low ? _bittrexOrders->getPrice(double()) : low;

        //Make sure the response is valid
        if(read_string(qslApiResponseSells[i].toStdString(), jsonResponse)) {
            mObject jsonObjectSells = jsonResponse.get_obj();

            try
            {
                _bittrexOrders->setQuantity(getPairValue(jsonObjectSells, "Quantity").get_real());
                _bittrexOrders->setPrice(getPairValue(jsonObjectSells, "Rate").get_real());
                _bittrexOrders->setOrderType("Sell");
            }
            catch (std::exception) {} //API did not return all needed data so skip this order

            QTreeWidgetItem * qtSells = new QTreeWidgetItem();

            qtSells->setText(0, _bittrexOrders->getPrice(QString()));
            qtSells->setText(1, _bittrexOrders->getQuantity(QString()));

            ui->qTreeWidgetBittrexSell->addTopLevelItem(qtSells);

            sumSells += _bittrexOrders->getQuantity(double());
            xAxisSells[z] = _bittrexOrders->getPrice(double()) * 100000000;
            yAxisSells[z] = sumSells;
        }

        high = _bittrexOrders->getPrice(double()) > high ? _bittrexOrders->getPrice(double()) : high;
        low = _bittrexOrders->getPrice(double()) < low ? _bittrexOrders->getPrice(double()) : low;

        z++;
    }

    high *=  100000000;
    low *=  100000000;

    sumHighest = sumBuys > sumSells ? sumBuys : sumBuys < sumSells ? sumSells : sumBuys;

    ui->qCustomPlotBittrexOrderDepth->graph(0)->setData(xAxisBuys, yAxisBuys);
    ui->qCustomPlotBittrexOrderDepth->graph(1)->setData(xAxisSells, yAxisSells);

    ui->qCustomPlotBittrexOrderDepth->graph(0)->setPen(QPen(QColor(34, 177, 76)));
    ui->qCustomPlotBittrexOrderDepth->graph(0)->setBrush(QBrush(QColor(34, 177, 76, 20)));
    ui->qCustomPlotBittrexOrderDepth->graph(1)->setPen(QPen(QColor(237, 24, 35)));
    ui->qCustomPlotBittrexOrderDepth->graph(1)->setBrush(QBrush(QColor(237, 24, 35, 20)));

    ui->qCustomPlotBittrexOrderDepth->xAxis->setRange(low, high);
    ui->qCustomPlotBittrexOrderDepth->yAxis->setRange(low, sumHighest);

    ui->qCustomPlotBittrexOrderDepth->replot();
}

const mValue& TradingStatsBittrex::getPairValue(const mObject& obj, const std::string& name)
{
    mObject::const_iterator iter = obj.find(name);

    assert(iter != obj.end());
    assert(iter->first == name);

    return iter->second;
}

void TradingStatsBittrex::updateLabel(QLabel* qLabel, double d1, double d2, QString prefix, int decimalPlaces)
{
    qLabel->setText("");

    if (d1 > d2) {
        qLabel->setText(prefix + "<font color=\"black\"><b>" + QString::number(d1, 'f', decimalPlaces) + "</b></font>");
    }
    else if (d1 < d2) {
        qLabel->setText(prefix + "<font color=\"black\"><b><u>" + QString::number(d1, 'f', decimalPlaces) + "</u></b></font>");
    }
    else {
        qLabel->setText(prefix + QString::number(d1, 'f', decimalPlaces));
    }
}
void TradingStatsBittrex::updateLabel(QLabel* qLabel, double d1, double d2, QString prefix, QString suffix, int decimalPlaces)
{
    qLabel->setText("");

    if (d1 > d2) {
        qLabel->setText(prefix + "<font color=\"black\"><b>" + QString::number(d1, 'f', decimalPlaces) + suffix + "</b></font>");
    }
    else if (d1 < d2) {
        qLabel->setText(prefix + "<font color=\"black\"><b><u>" + QString::number(d1, 'f', decimalPlaces) + suffix + "</u></b></font>");
    }
    else {
        qLabel->setText(prefix + QString::number(d1, 'f', decimalPlaces) + suffix);
    }
}

void TradingStatsBittrex::setModel(ClientModel *model)
{
    this->model = model;
}

TradingStatsBittrex::~TradingStatsBittrex()
{
    delete ui;
}
