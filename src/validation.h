// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2017-2020 The Neutron Developers
//
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef NEUTRON_VALIDATION_H
#define NEUTRON_VALIDATION_H

#include <stdint.h>
#include <string>

static const int MAX_INACTIVITY_IBD = 60 * 5; /* 5 minutes */
static const int64_t DEFAULT_MAX_TIP_AGE = 60 * 60 * 2;
extern int64_t nMaxTipAge;
class CBlockIndex;

FILE* OpenBlockFile(unsigned int nFile, unsigned int nBlockPos, const char* pszMode="rb");
FILE* AppendBlockFile(unsigned int& nFileRet);
void DelatchIsInitialBlockDownload();
bool IsInitialBlockDownload();

/* Verification progress as a fraction between 0.00 and 1.00 */
double GuessVerificationProgress(CBlockIndex* pindex);

#endif // NEUTRON_VALIDATION_H
