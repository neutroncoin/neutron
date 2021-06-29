#include "tradingstats.h"
#include "ui_tradingstats.h"

TradingstatsPage::TradingstatsPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::TradingstatsPage) {
    ui->setupUi(this);

    BittrexStats = new TradingStatsBittrex();
    BleutradeStats = new TradingStatsBleutrade();

    QString lblBittrexStats = "Bittrex";
    QString lblBleutradeStats = "Bleutrade";        

    ui->TradingStatsTabWidget->addTab(BittrexStats, lblBittrexStats);
    ui->TradingStatsTabWidget->addTab(BleutradeStats, lblBleutradeStats);   
}

void TradingstatsPage::UpdateMarketData() {
    BittrexStats->pollAPIs();
    BleutradeStats->pollAPIs();
}

void TradingstatsPage::setModel(ClientModel *model) {
    this->model = model;
}

TradingstatsPage::~TradingstatsPage() {
    delete ui;
}
