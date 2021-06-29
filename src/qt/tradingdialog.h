#ifndef TRADINGPAGE_H
#define TRADINGPAGE_H

#include "clientmodel.h"
#include "main.h"
#include "wallet.h"
#include "base58.h"
#include "tradingdialogBittrex.h"

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

class TradingDialogBittrex;

namespace Ui {
class TradingPage;
}

class ClientModel;

class TradingPage : public QWidget {
    Q_OBJECT

public:
    explicit TradingPage(QWidget *parent = 0);
    ~TradingPage();

    void setModel(ClientModel *model);

private:
    TradingDialogBittrex *tradeOnBittrex;

    Ui::TradingPage *ui;
    ClientModel *model;

};

#endif // TRADINGPAGE_H
