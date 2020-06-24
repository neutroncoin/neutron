// Copyright (c) 2020 The Neutron Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef NEUTRON_BLOCK_INDEX_H
#define NEUTRON_BLOCK_INDEX_H

#include <map>
#include <mutex>

#include "main.h"
#include "uint256.h"

class BlockIndex
{
private:
    const int BLOCK_INDEX_SIZE = 1024;
    std::map<uint256, CBlockIndex *> mapBlockIndex;
    std::list<uint256> mapBlockIndexByRecentlyUsed;
    std::mutex mutexBlockIndex;
    CBlockIndex *get_or_create(const uint256& hash);
public:
    CBlockIndex *find(const uint256& hash);
    bool contains(const uint256& hash);
};

#endif
