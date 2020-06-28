// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2013 The NovaCoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef NEUTRON_MINER_H
#define NEUTRON_MINER_H

#include "main.h"
#include "wallet.h"
#include "txmempool.h"

extern double dHashesPerSec;
extern int64_t nHPSTimerStart;

CBlock* CreateNewBlock(CWallet* pwallet, bool fProofOfStake=false, int64_t* pFees = 0);
void IncrementExtraNonce(CBlock* pblock, CDiskBlockIndex* pindexPrev, unsigned int& nExtraNonce);
void FormatHashBuffers(CBlock* pblock, char* pmidstate, char* pdata, char* phash1);
bool CheckWork(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey);
bool CheckStake(CBlock* pblock, CWallet& wallet);
void SHA256Transform(void* pstate, void* pinput, const void* pinit);
void GenerateBitcoins(bool fGenerate, CWallet* pwallet);
void StakeMiner(CWallet *pwallet, bool fProofOfStake);

#endif // NEUTRON_MINER_H
