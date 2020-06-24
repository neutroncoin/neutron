// Copyright (c) 2020 The Neutron Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <map>
#include <stdlib.h>

#include "blockindex.h"
#include "txdb-leveldb.h"

CBlockIndex *BlockIndex::get_or_create(const uint256& hash)
{
    if (hash != 0)
    {
        map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);

        if (mi != mapBlockIndex.end())
        {
            mapBlockIndexByRecentlyUsed.remove(mi->first);
            mapBlockIndexByRecentlyUsed.push_front(mi->first);
            return (*mi).second;
        }

        // Create new
        CBlockIndex* pindexNew = new CBlockIndex();

        if (!pindexNew)
            throw runtime_error(strprintf("%s : new CBlockIndex failed", __func__));

        mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
	pindexNew->phashBlock = &((*mi).first);
        mapBlockIndexByRecentlyUsed.push_front(mi->first);

	return pindexNew;
    }

    return nullptr;
}

CBlockIndex *BlockIndex::find(const uint256& hash)
{
    const std::lock_guard<std::mutex> lock(mutexBlockIndex);
    std::map<uint256, CBlockIndex *>::iterator mi = mapBlockIndex.find(hash);

    if (mi == mapBlockIndex.end())
    {
        CTxDB txdb("r");
        CDiskBlockIndex diskindex;

        if (txdb.ReadBlockIndex(hash, &diskindex))
        {
            // Construct block index object
            CBlockIndex* pindexNew    = get_or_create(hash);
            pindexNew->pprev          = get_or_create(diskindex.hashPrev);
            pindexNew->pnext          = get_or_create(diskindex.hashNext);

            pindexNew->nFile          = diskindex.nFile;
            pindexNew->nBlockPos      = diskindex.nBlockPos;
            pindexNew->nHeight        = diskindex.nHeight;
            pindexNew->nMint          = diskindex.nMint;
            pindexNew->nMoneySupply   = diskindex.nMoneySupply;
            pindexNew->nFlags         = diskindex.nFlags;
            pindexNew->nStakeModifier = diskindex.nStakeModifier;
            pindexNew->prevoutStake   = diskindex.prevoutStake;
            pindexNew->nStakeTime     = diskindex.nStakeTime;
            pindexNew->hashProof      = diskindex.hashProof;
            pindexNew->nVersion       = diskindex.nVersion;
            pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
            pindexNew->nTime          = diskindex.nTime;
            pindexNew->nBits          = diskindex.nBits;
            pindexNew->nNonce         = diskindex.nNonce;

            return pindexNew;
        }
        else
            return nullptr;
    }
    else
    {
	mapBlockIndexByRecentlyUsed.remove(mi->first);
	mapBlockIndexByRecentlyUsed.push_front(mi->first);
	return mi->second;
    }

    return nullptr;
}

bool BlockIndex::contains(const uint256& hash)
{
    std::lock_guard<std::mutex> lock(mutexBlockIndex);
    CTxDB txdb();

    return mapBlockIndex.count(hash) > 0 || txdb.ContainsBlockIndex(hash);
}
