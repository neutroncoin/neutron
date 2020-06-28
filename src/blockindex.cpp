// Copyright (c) 2020 The Neutron Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <map>
#include <stdlib.h>

#include "blockindex.h"
#include "txdb-leveldb.h"

CDiskBlockIndex *BlockIndex::find(const uint256& hash)
{
    if (hash == 0)
        return nullptr;

    const std::lock_guard<std::mutex> lock(mutexBlockIndex);
    std::map<uint256, CDiskBlockIndex *>::iterator mi = mapBlockIndex.find(hash);

    if (mi == mapBlockIndex.end())
    {
        CTxDB txdb("r");
        CDiskBlockIndex *pindex = new CDiskBlockIndex();

        if (!pindex)
            throw runtime_error(strprintf("%s : new CDiskBlockIndex failed", __func__));

        if (txdb.ReadBlockIndex(hash, *pindex))
        {
            map<uint256, CDiskBlockIndex *>::iterator mi;
            mi = mapBlockIndex.insert(make_pair(hash, pindex)).first;
            pindex->phashBlock = &((*mi).first);
            mapBlockIndexByRecentlyUsed.push_front(mi->first);

            return pindex;
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
    CTxDB txdb("r");

    return mapBlockIndex.count(hash) > 0 || txdb.ContainsBlockIndex(hash);
}

bool BlockIndex::persist(const CDiskBlockIndex& blockIndexToWrite)
{
    std::lock_guard<std::mutex> lock(mutexBlockIndex);

    CTxDB txdb;
    return txdb.WriteBlockIndex(blockIndexToWrite);
}
