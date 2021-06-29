#include "coinstats.h"
#include "ui_coinstats.h"
#include "main.h"
#include "wallet.h"
#include "init.h"
#include "base58.h"
#include "clientmodel.h"
#include "bitcoinrpc.h"
#include "tradingstatsBittrex.h"
#include "tradingstatsBleutrade.h"
#include "bitcoingui.h"
#include <sstream>
#include <string>

using namespace json_spirit;

int heightPrevious = -1;
int connectionPrevious = -1;
int volumePrevious = -1;
double rewardPrevious = -1;
double netPawratePrevious = -1;
double pawratePrevious = -1;
double hardnessPrevious = -1;
double hardnessPrevious2 = -1;
int stakeminPrevious = -1;
int stakemaxPrevious = -1;
int64_t marketcapPrevious = -1;
QString stakecPrevious = "";


CoinStats::CoinStats(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::CoinStats) {

    ui->setupUi(this);

    connect(ui->startButton, SIGNAL(pressed()), this, SLOT(updateStatistics()));

}


void CoinStats::updateStatistics() {
    double pHardness = GetDifficulty();
    double pHardness2 = GetDifficulty(GetLastBlockIndex(pindexBest, true));
    int pPawrate = GetPoWMHashPS();
    double pPawrate2 = 0.000;
    int nHeight = pindexBest->nHeight;
    ui->progressBar->setMaximum(LAST_POW_BLOCK);
    double nSubsidy = 50;
    uint64_t nMinWeight = 0, nMaxWeight = 0, nWeight = 0;
    pwalletMain->GetStakeWeight(*pwalletMain, nMinWeight, nMaxWeight, nWeight);
    uint64_t nNetworkWeight = GetPoSKernelPS();
    int64_t volume = ((pindexBest->nMoneySupply) / 100000000);
    // Having two exchanges where to fetch current Neutron value, let's do the avarage
    int64_t marketcap = ((_dScPriceLastBittrex+_dScPriceLastBleutrade)/2) * volume;
    int peers = this->model->getNumConnections();
    pPawrate2 = (double)pPawrate;
    ui->progressBar->setValue(nHeight);
    QString height = QString::number(nHeight);
    QString stakemin = QString::number(nMinWeight);
    QString stakemax = QString::number(nNetworkWeight);
    QString phase = "";

    if (pindexBest->nHeight < LAST_POW_BLOCK) {
        phase = "<p align=\"center\">PoW/PoS</p>";
        ui->progressBar->setValue(pindexBest->nHeight);
    } else {
        ui->progressBar->hide();
        phase = "<p align=\"center\">Pure proof of stake</p>";
    }

    QString subsidy = QString::number(nSubsidy, 'f', 6);
    QString hardness = QString::number(pHardness, 'f', 6);
    QString hardness2 = QString::number(pHardness2, 'f', 6);
    QString pawrate = QString::number(pPawrate2, 'f', 3);
    QString Qlpawrate = model->getLastBlockDate().toString();

    QString QPeers = QString::number(peers);
    QString qVolume = QLocale(QLocale::English).toString((qlonglong)volume);

    if (nHeight > heightPrevious) {
        ui->progressBar->setValue(nHeight);
        ui->heightBox->setText("<font color=\"black\">" + height + "</font>");
    } else {
        ui->heightBox->setText(height);
    }

    if (0 > stakeminPrevious) {
        ui->minBox->setText("<font color=\"black\">" + stakemin + "</font>");
    } else {
        ui->minBox->setText(stakemin);
    }

    if (0 > stakemaxPrevious) {
        ui->maxBox->setText("<font color=\"black\">" + stakemax + "</font>");
    } else {
        ui->maxBox->setText(stakemax);
    }

    if (phase != stakecPrevious) {
        ui->cBox->setText("<font color=\"black\">" + phase + "</font>");
    } else {
        ui->cBox->setText(phase);
    }

    if (marketcap > marketcapPrevious) {
        ui->marketcap->setText("<font color=\"black\">$ " + QString::number(marketcap) + "</font>");
    } else if (marketcap < marketcapPrevious) {
        ui->marketcap->setText("<font color=\"black\">$ " + QString::number(marketcap) + "</font>");
    } else {
        ui->marketcap->setText("$" + QString::number(marketcap));
    }

    if (pHardness2 > hardnessPrevious2) {
        ui->diffBox2->setText("<font color=\"black\">" + hardness2 + "</font>");
    } else if (pHardness2 < hardnessPrevious2) {
        ui->diffBox2->setText("<font color=\"black\">" + hardness2 + "</font>");
    } else {
        ui->diffBox2->setText(hardness2);
    }

    if (Qlpawrate != pawratePrevious) {
        ui->localBox->setText("<font color=\"black\">" + Qlpawrate + "</font>");
    } else {
        ui->localBox->setText(Qlpawrate);
    }

    if (peers > connectionPrevious) {
        ui->connectionBox->setText("<font color=\"black\">" + QPeers + "</font>");
    } else if (peers < connectionPrevious) {
        ui->connectionBox->setText("<font color=\"black\">" + QPeers + "</font>");
    } else {
        ui->connectionBox->setText(QPeers);
    }

    if (volume > volumePrevious) {
        ui->volumeBox->setText("<font color=\"black\">" + qVolume + " NTRN" + "</font>");
    } else if (volume < volumePrevious) {
        ui->volumeBox->setText("<font color=\"black\">" + qVolume + " NTRN" + "</font>");
    } else {
        ui->volumeBox->setText(qVolume + " NTRN");
    }

    updatePrevious(nHeight, nMinWeight, nNetworkWeight, phase, nSubsidy, pHardness, pHardness2, pPawrate2, Qlpawrate, peers, volume, marketcap);
}

void CoinStats::updatePrevious(int nHeight, int nMinWeight, int nNetworkWeight, QString phase, double nSubsidy, double pHardness, double pHardness2, double pPawrate2, QString Qlpawrate, int peers, int volume, int64_t marketcap) {
    heightPrevious = nHeight;
    stakeminPrevious = nMinWeight;
    stakemaxPrevious = nNetworkWeight;
    stakecPrevious = phase;
    rewardPrevious = nSubsidy;
    hardnessPrevious = pHardness;
    hardnessPrevious2 = pHardness2;
    netPawratePrevious = pPawrate2;
    pawratePrevious = Qlpawrate;
    connectionPrevious = peers;
    volumePrevious = volume;
    marketcapPrevious = marketcap;
}

void CoinStats::setModel(ClientModel *model) {
    updateStatistics();
    this->model = model;
}


CoinStats::~CoinStats() {
    delete ui;
}
