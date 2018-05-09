// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_VALIDATION_H
#define BITCOIN_VALIDATION_H

#include <stdint.h>
#include <string>

FILE* OpenBlockFile(unsigned int nFile, unsigned int nBlockPos, const char* pszMode="rb");
FILE* AppendBlockFile(unsigned int& nFileRet);

bool IsInitialBlockDownload();

#endif // BITCOIN_VALIDATION_H
