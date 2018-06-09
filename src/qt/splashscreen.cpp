// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "splashscreen.h"

#include "clientversion.h"
#include "init.h"
#include "ui_interface.h"
#include "util.h"
#ifdef ENABLE_WALLET
#include "wallet.h"
#endif

#include <QApplication>
#include <QPainter>

SplashScreen::SplashScreen(const QPixmap &pixmap, Qt::WindowFlags f) :
    QSplashScreen(pixmap, f)
{
    setAutoFillBackground(true);

    // set reference point, paddings
    // int paddingLeft             = 14;
    // int paddingTop              = 26;
    // int titleVersionVSpace      = 17;
    // int titleCopyrightVSpace    = 32;

    // float fontFactor            = 1.0;

    // define text to place
    // QString titleText       = tr("Neutron Core");
    // QString versionText     = QString(tr("Version %1")).arg(QString::fromStdString(FormatFullVersion()));
    // QString copyrightTextBtc   = QChar(0xA9)+QString(" 2009-2015 ").arg(COPYRIGHT_YEAR) + QString(tr("The Bitcoin Core developers"));

    // QString font            = "Arial";

    // QPainter pixPaint(&newPixmap);
    // pixPaint.setPen(QColor(232,186,163));

    // // check font size and drawing with
    // pixPaint.setFont(QFont(font, 28*fontFactor));
    // QFontMetrics fm = pixPaint.fontMetrics();
    // int titleTextWidth  = fm.width(titleText);
    // if(titleTextWidth > 160) {
    //     // strange font rendering, Arial probably not found
    //     fontFactor = 0.75;
    // }

    // pixPaint.setFont(QFont(font, 28*fontFactor));
    // fm = pixPaint.fontMetrics();
    // titleTextWidth  = fm.width(titleText);
    // pixPaint.drawText(paddingLeft,paddingTop,titleText);

    // pixPaint.setFont(QFont(font, 15*fontFactor));
    // pixPaint.drawText(paddingLeft,paddingTop+titleVersionVSpace,versionText);

    // // draw copyright stuff
    // pixPaint.setFont(QFont(font, 10*fontFactor));
    // pixPaint.drawText(paddingLeft,paddingTop+titleCopyrightVSpace,copyrightTextBtc);

    // pixPaint.end();


    // overlay the logo bitmap on a colored background
    QPixmap overlay = QPixmap(":/images/splash");

    // set logo paddings
    int logoPaddingTop          = 30;
    int logoPaddingLeft         = 20;
    int logoPaddingText         = 15;

    int baseWidth               = overlay.width()+logoPaddingLeft*2;
    int baseHeight              = overlay.height()+logoPaddingTop*2+logoPaddingText;
    int pixmapWidth             = baseWidth;
    int pixmapHeight            = baseHeight;

    QPixmap base(baseWidth, baseHeight);
    base.fill(Qt::white);

    QPixmap newPixmap(pixmapWidth, pixmapHeight);
    newPixmap.fill(Qt::transparent); // force alpha channel
    {
        QPainter painter(&newPixmap);
        painter.drawPixmap(0, 0, base);
        painter.drawPixmap(logoPaddingLeft, logoPaddingTop, overlay);
    }


    this->setPixmap(newPixmap);

    subscribeToCoreSignals();
}

SplashScreen::~SplashScreen()
{
    unsubscribeFromCoreSignals();
}

void SplashScreen::slotFinish(QWidget *mainWin)
{
    finish(mainWin);

    /* If the window is minimized, hide() will be ignored. */
    /* Make sure we de-minimize the splashscreen window before hiding */
    if (isMinimized())
        showNormal();
    hide();
    deleteLater(); // No more need for this
}

static void InitMessage(SplashScreen *splash, const std::string &message)
{
    QMetaObject::invokeMethod(splash, "showMessage",
        Qt::QueuedConnection,
        Q_ARG(QString, QString::fromStdString(message)),
        Q_ARG(int, Qt::AlignBottom|Qt::AlignHCenter),
        Q_ARG(QColor, QColor(232,186,163)));
}

static void ShowProgress(SplashScreen *splash, const std::string &title, int nProgress)
{
    InitMessage(splash, title + strprintf("%d", nProgress) + "%");
}

#ifdef ENABLE_WALLET
static void ConnectWallet(SplashScreen *splash, CWallet* wallet)
{
    wallet->ShowProgress.connect(boost::bind(ShowProgress, splash, _1, _2));
}
#endif

void SplashScreen::subscribeToCoreSignals()
{
    // Connect signals to client
    uiInterface.InitMessage.connect(boost::bind(InitMessage, this, _1));
    uiInterface.ShowProgress.connect(boost::bind(ShowProgress, this, _1, _2));
#ifdef ENABLE_WALLET
    uiInterface.LoadWallet.connect(boost::bind(ConnectWallet, this, _1));
#endif
}

void SplashScreen::unsubscribeFromCoreSignals()
{
    // Disconnect signals from client
    uiInterface.InitMessage.disconnect(boost::bind(InitMessage, this, _1));
    uiInterface.ShowProgress.disconnect(boost::bind(ShowProgress, this, _1, _2));
#ifdef ENABLE_WALLET
    if(pwalletMain)
        pwalletMain->ShowProgress.disconnect(boost::bind(ShowProgress, this, _1, _2));
#endif
}
