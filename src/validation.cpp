// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2016-2020 The Neutron Developers
//
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "validation.h"
#include "main.h"
#include "checkpoints.h"
#include "timedata.h"
#include "tinyformat.h"
#include "script/standard.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

using namespace std;
using namespace boost;

int64_t nMaxTipAge = DEFAULT_MAX_TIP_AGE;

static filesystem::path BlockFilePath(unsigned int nFile)
{
    string strBlockFn = strprintf("blk%04u.dat", nFile);
    return GetDataDir() / strBlockFn;
}

FILE* OpenBlockFile(unsigned int nFile, unsigned int nBlockPos, const char* pszMode)
{
    if ((nFile < 1) || (nFile == (unsigned int) -1))
        return NULL;

    FILE* file = fopen(BlockFilePath(nFile).string().c_str(), pszMode);

    if (!file)
        return NULL;

    if (nBlockPos != 0 && !strchr(pszMode, 'a') && !strchr(pszMode, 'w'))
    {
        if (fseek(file, nBlockPos, SEEK_SET) != 0)
        {
            fclose(file);
            return NULL;
        }
    }

    return file;
}

static unsigned int nCurrentBlockFile = 1;

FILE* AppendBlockFile(unsigned int& nFileRet)
{
    nFileRet = 0;

    while (true)
    {
        FILE* file = OpenBlockFile(nCurrentBlockFile, 0, "ab");

        if (!file)
            return NULL;

        if (fseek(file, 0, SEEK_END) != 0)
            return NULL;

        // FAT32 file size max 4GB, fseek and ftell max 2GB, so we must stay under 2GB
        if (ftell(file) < (long) (0x7F000000 - MAX_SIZE))
        {
            nFileRet = nCurrentBlockFile;
            return file;
        }

        fclose(file);
        nCurrentBlockFile++;
    }
}

// Once this function has returned false it should remain so most of the time
static std::atomic<bool> latchToFalse{false};

void DelatchIsInitialBlockDownload()
{
    latchToFalse.store(false, std::memory_order_relaxed);
}

bool IsInitialBlockDownload()
{
    // Optimization: pre-test latch before taking the lock.
    if (latchToFalse.load(std::memory_order_relaxed))
        return false;

    LOCK(cs_main);

    if (pindexBest == NULL)
        return true;

    if (nBestHeight < Checkpoints::GetTotalBlocksEstimate())
        return true;

    /* TODO: look into verifying chain work */

    static int64_t nLastUpdate;
    static CBlockIndex* pindexLastBest;

    if (pindexBest != pindexLastBest)
    {
        pindexLastBest = pindexBest;
        nLastUpdate = GetTime();
    }

    if (GetTime() - nLastUpdate <= MAX_INACTIVITY_IBD)
    {
        if (GetTime() - nLastUpdate < 15 && pindexBest->GetBlockTime() < (GetTime() - nMaxTipAge))
            return true;

        if (pindexBest->GetBlockTime() < (GetTime() - nMaxTipAge))
            return true;
    }

    LogPrintf("[InitialBlockDownload] Leaving InitialBlockDownload (latching to false)\n");
    latchToFalse.store(true, std::memory_order_relaxed);

    return false;
}

double GuessVerificationProgress(CBlockIndex *pindex)
{
    if (pindex == NULL || pindexBest == NULL)
        return 0.0;

    return (float) pindex->nHeight / (float) pindexBest->nHeight;
}
