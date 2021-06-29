#include "statisticspage.h"
#include "ui_statisticspage.h"

StatisticsPage::StatisticsPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::StatisticsPage) {
    ui->setupUi(this);

    coinStatsWidget = new CoinStats();
    tradingStatsWidget = new TradingstatsPage();

    QString coinStatsTabLabel = "Coin statistics";
    QString tradingStatsTabLabel = "Trading statistics";

    ui->tabWidget->addTab(coinStatsWidget, coinStatsTabLabel);
    ui->tabWidget->addTab(tradingStatsWidget, tradingStatsTabLabel);
}

void StatisticsPage::setModel(ClientModel *model) {
    this->model = model;
}

StatisticsPage::~StatisticsPage() {
    delete ui;
}
