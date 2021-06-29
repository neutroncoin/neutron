#ifndef STAISYBIT
#define STAISYBIT

#include "clientmodel.h"
#include <QWidget>

namespace Ui {
    class StaisybitPage;
}

class ClientModel;

class StaisybitPage : public QWidget {
    Q_OBJECT

public:
    explicit StaisybitPage(QWidget *parent = 0);
    ~StaisybitPage();

    void setModel(ClientModel *model);

private:
    Ui::StaisybitPage *ui;
    ClientModel *model;

};

#endif // STAISYBIT

