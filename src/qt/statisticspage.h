#ifndef STATISTICSPAGE_H
#define STATISTICSPAGE_H

#include "clientmodel.h"
#include "main.h"
#include "wallet.h"
#include "base58.h"
#include "coinstats.h"
#include "tradingstats.h"

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

class CoinStats;
class TradingstatsPage;
class QTabWidget;
class QString;
class QToolBar;
class QAction;
class QStackedWidget;

namespace Ui {
class StatisticsPage;
}

class ClientModel;

class StatisticsPage : public QWidget {
    Q_OBJECT

public:
    explicit StatisticsPage(QWidget *parent = 0);
    ~StatisticsPage();

    void setModel(ClientModel *model);

    CoinStats *coinStatsWidget;
    TradingstatsPage *tradingStatsWidget;

private:    
    Ui::StatisticsPage *ui;
    ClientModel *model;

};

#endif // STATISTICSPAGE_H
