#ifndef TRADINGSTATSPAGE_H
#define TRADINGSTATSPAGE_H

#include "clientmodel.h"
#include "main.h"
#include "wallet.h"
#include "base58.h"
#include "tradingstatsBittrex.h"
#include "tradingstatsBleutrade.h"

#include <QWidget>
#include <QStackedWidget>
#include <QComboBox>

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTime>
#include <QTimer>
#include <QStringList>
#include <QMap>
#include <QSettings>
#include <QSlider>

class TradingStatsBittrex;
class TradingStatsBleutrade;
class QTabWidget;
class QString;
class QToolBar;
class QAction;
class QStackedWidget;

namespace Ui {
class TradingstatsPage;
}

class ClientModel;

class TradingstatsPage : public QWidget {
    Q_OBJECT

public:
    explicit TradingstatsPage(QWidget *parent = 0);
    ~TradingstatsPage();

    void setModel(ClientModel *model);

    void UpdateMarketData();

private:
    TradingStatsBittrex *BittrexStats;
    TradingStatsBleutrade *BleutradeStats;

    Ui::TradingstatsPage *ui;
    ClientModel *model;

};

#endif // TRADINGSTATSPAGE_H
