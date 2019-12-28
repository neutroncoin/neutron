// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_INIT_H
#define BITCOIN_INIT_H

#include "wallet.h"

#include <string>

class CScheduler;

namespace boost {
    class thread_group;
} // namespace boost

extern CWallet* pwalletMain;
extern CConnman* shared_connman;
extern CCriticalSection cs_Shutdown;

void StartShutdown();
bool ShutdownRequested();
/** Interrupt threads */
void Interrupt(boost::thread_group& threadGroup);
void Shutdown();
bool AppInit2(boost::thread_group& threadGroup, CScheduler& scheduler);
bool PrepareShutdown();

/** Help for options shared between UI and daemon (for -help) */
std::string HelpMessage();

#endif // BITCOIN_INIT_H
