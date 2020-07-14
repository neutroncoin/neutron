// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PRIMITIVES_BLOCK_H
#define BITCOIN_PRIMITIVES_BLOCK_H

#include "main.h"
#include "serialize.h"
#include "uint256.h"
#include "collectionhashing.h"

#include <unordered_map>

/** Describes a place in the block chain to another node such that if the
 * other node doesn't have the same branch, it can find a recent common trunk.
 * The further back it is, the further before the fork it may be.
 */
class CBlockLocator
{
protected:
    std::vector<uint256> vHave;
public:

    CBlockLocator()
    {
    }

    explicit CBlockLocator(const CBlockIndex* pindex)
    {
        Set(pindex);
    }

    explicit CBlockLocator(uint256 hashBlock)
    {
        auto mi = mapBlockIndex.find(hashBlock);

        if (mi != mapBlockIndex.end())
            Set((*mi).second);
    }

    CBlockLocator(const std::vector<uint256>& vHaveIn)
    {
        vHave = vHaveIn;
    }

    IMPLEMENT_SERIALIZE
    (
        if (!(nType & SER_GETHASH))
            READWRITE(nVersion);

        READWRITE(vHave);
    )

    void SetNull()
    {
        vHave.clear();
    }

    bool IsNull()
    {
        return vHave.empty();
    }

    void Set(const CBlockIndex* pindex)
    {
        vHave.clear();
        int nStep = 1;

        while (pindex)
        {
            vHave.push_back(pindex->GetBlockHash());

            // Exponentially larger steps back
            for (int i = 0; pindex && i < nStep; i++)
                pindex = pindex->pprev;

            if (vHave.size() > 10)
                nStep *= 2;
        }

        vHave.push_back((!fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet));
    }

    int GetDistanceBack()
    {
        // Retrace how far back it was in the sender's branch
        int nDistance = 0;
        int nStep = 1;

        BOOST_FOREACH(const uint256& hash, vHave)
        {
            auto mi = mapBlockIndex.find(hash);

            if (mi != mapBlockIndex.end())
            {
                CBlockIndex* pindex = (*mi).second;

                if (pindex->IsInMainChain())
                    return nDistance;
            }

            nDistance += nStep;

            if (nDistance > 10)
                nStep *= 2;
        }

        return nDistance;
    }

    CBlockIndex* GetBlockIndex()
    {
        // Find the first block the caller has in the main chain
        BOOST_FOREACH(const uint256& hash, vHave)
        {
            auto mi = mapBlockIndex.find(hash);

            if (mi != mapBlockIndex.end())
            {
                CBlockIndex* pindex = (*mi).second;

                if (pindex->IsInMainChain())
                    return pindex;
            }
        }

        return pindexGenesisBlock;
    }

    uint256 GetBlockHash()
    {
        // Find the first block the caller has in the main chain
        BOOST_FOREACH(const uint256& hash, vHave)
        {
            auto mi = mapBlockIndex.find(hash);

            if (mi != mapBlockIndex.end())
            {
                CBlockIndex* pindex = (*mi).second;

                if (pindex->IsInMainChain())
                    return hash;
            }
        }

        return (!fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet);
    }

    int GetHeight()
    {
        CBlockIndex* pindex = GetBlockIndex();

        if (!pindex)
            return 0;

        return pindex->nHeight;
    }
};

#endif // BITCOIN_PRIMITIVES_BLOCK_H
