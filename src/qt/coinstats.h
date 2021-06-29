#ifndef COINSTATS_H
#define COINSTATS_H

#include "clientmodel.h"
#include "main.h"
#include "wallet.h"
#include "base58.h"

#include <QWidget>

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTime>
#include <QTimer>
#include <QStringList>
#include <QMap>
#include <QSettings>
#include <QSlider>

namespace Ui {
class CoinStats;
}
class ClientModel;

class CoinStats : public QWidget {
    Q_OBJECT

  public:
    explicit CoinStats(QWidget *parent = 0);
    ~CoinStats();

    void setModel(ClientModel *model);

    int heightPrevious;
    int connectionPrevious;
    int volumePrevious;
    int stakeminPrevious;
    int stakemaxPrevious;
    QString stakecPrevious;
    double rewardPrevious;
    double netPawratePrevious;
    QString pawratePrevious;
    double hardnessPrevious;
    double hardnessPrevious2;
    int64_t marketcapPrevious;

  public slots:

    void updateStatistics();
    void updatePrevious(int, int, int, QString, double, double, double, double, QString, int, int, int64_t);

  private slots:

  private:
    Ui::CoinStats *ui;
    ClientModel *model;

};

#endif // COINSTATS_H
