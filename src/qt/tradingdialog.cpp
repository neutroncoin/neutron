#include "tradingdialog.h"
#include "ui_tradingdialog.h"

TradingPage::TradingPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::TradingPage) {
    ui->setupUi(this);

    tradeOnBittrex = new TradingDialogBittrex();

    QString lblTradeOnBittrex = "Bittrex";

    ui->TradingPageTabWidget->addTab(tradeOnBittrex, lblTradeOnBittrex);
}

void TradingPage::setModel(ClientModel *model) {
    this->model = model;
}

TradingPage::~TradingPage() {
    delete ui;
}
