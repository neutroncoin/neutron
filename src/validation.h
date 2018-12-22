// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_VALIDATION_H
#define BITCOIN_VALIDATION_H

#include <stdint.h>
#include <string>

static const int64_t DEFAULT_MAX_TIP_AGE = 1 * 60 * 60; // ~45 blocks behind -> 2 x fork detection time, was 24 * 60 * 60 in bitcoin

extern int64_t nMaxTipAge;

class CBlockIndex;

FILE* OpenBlockFile(unsigned int nFile, unsigned int nBlockPos, const char* pszMode="rb");
FILE* AppendBlockFile(unsigned int& nFileRet);

bool IsInitialBlockDownload();

/** Guess verification progress (as a fraction between 0.0=genesis and 1.0=current tip). */
double GuessVerificationProgress(CBlockIndex* pindex);

#endif // BITCOIN_VALIDATION_H
