// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef KERNEL_H
#define KERNEL_H

#include "main.h"

extern unsigned int nModifierInterval; // time to elapse before new modifier is computed

static const int MODIFIER_INTERVAL_RATIO = 3;
static const int64_t POS_HASHCHECK_MAX_BLOCK_AGE = (60 * 60 * 24 * 2); // 2 days
static const int STAKE_TIMESTAMP_MASK = 15;

bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier,
		              bool& fGeneratedStakeModifier);

bool CheckStakeKernelHash(CBlockIndex* pindexPrev, unsigned int nBits, const CBlock& blockFrom,
		          unsigned int nTxPrevOffset, const CTransaction& txPrev, const COutPoint& prevout,
			  unsigned int nTimeTx, uint256& hashProofOfStake, uint256& targetProofOfStake,
			  bool fPrintProofOfStake=false);

bool CheckProofOfStake(CBlockIndex* pindexPrev, const CTransaction& tx, unsigned int nBits,
		       uint256& hashProofOfStake, uint256& targetProofOfStake);

bool CheckCoinStakeTimestamp(int nHeight, int64_t nTimeBlock, int64_t nTimeTx);
unsigned int GetStakeModifierChecksum(const CBlockIndex* pindex);
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum);
int64_t GetWeight(int64_t nIntervalBeginning, int64_t nIntervalEnd);

#endif // KERNEL_H
