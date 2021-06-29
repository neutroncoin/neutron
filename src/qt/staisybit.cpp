#include "staisybit.h"
#include "ui_staisybit.h"

StaisybitPage::StaisybitPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::StaisybitPage) {
    ui->setupUi(this);
}

void StaisybitPage::setModel(ClientModel *model) {
    this->model = model;
}

StaisybitPage::~StaisybitPage() {
    delete ui;
}

