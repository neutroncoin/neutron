// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2013 The NovaCoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdb.h"
#include "miner.h"
#include "kernel.h"
#include "utiltime.h"
#include "script/standard.h"

using namespace std;

extern unsigned int nMinerSleep;

int static FormatHashBlocks(void* pbuffer, unsigned int len)
{
    unsigned char* pdata = (unsigned char*)pbuffer;
    unsigned int blocks = 1 + ((len + 8) / 64);
    unsigned char* pend = pdata + 64 * blocks;

    memset(pdata + len, 0, 64 * blocks - len);
    pdata[len] = 0x80;

    unsigned int bits = len * 8;

    pend[-1] = (bits >> 0) & 0xff;
    pend[-2] = (bits >> 8) & 0xff;
    pend[-3] = (bits >> 16) & 0xff;
    pend[-4] = (bits >> 24) & 0xff;

    return blocks;
}

static const unsigned int pSHA256InitState[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

void SHA256Transform(void* pstate, void* pinput, const void* pinit)
{
    SHA256_CTX ctx;
    unsigned char data[64];

    SHA256_Init(&ctx);

    for (int i = 0; i < 16; i++)
        ((uint32_t*) data)[i] = ByteReverse(((uint32_t*) pinput)[i]);

    for (int i = 0; i < 8; i++)
        ctx.h[i] = ((uint32_t*) pinit)[i];

    SHA256_Update(&ctx, data, sizeof(data));

    for (int i = 0; i < 8; i++)
        ((uint32_t*) pstate)[i] = ctx.h[i];
}

// Some explaining would be appreciated
class COrphan
{
public:
    CTransaction* ptx;
    set<uint256> setDependsOn;
    double dPriority;
    double dFeePerKb;

    COrphan(CTransaction* ptxIn)
    {
        ptx = ptxIn;
        dPriority = dFeePerKb = 0;
    }

    void print() const
    {
        LogPrintf("COrphan(hash=%s, dPriority=%.1f, dFeePerKb=%.1f)\n",
               ptx->GetHash().ToString().substr(0,10).c_str(), dPriority, dFeePerKb);

        BOOST_FOREACH(uint256 hash, setDependsOn)
            LogPrintf("   setDependsOn %s\n", hash.ToString().substr(0,10).c_str());
    }
};


uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;
int64_t nLastCoinStakeSearchInterval = 0;

// We want to sort transactions by priority and fee, so:
typedef boost::tuple<double, double, CTransaction*> TxPriority;
class TxPriorityCompare
{
    bool byFee;

public:
    TxPriorityCompare(bool _byFee) : byFee(_byFee) { }

    bool operator()(const TxPriority& a, const TxPriority& b)
    {
        if (byFee)
        {
            if (a.get<1>() == b.get<1>())
                return a.get<0>() < b.get<0>();

            return a.get<1>() < b.get<1>();
        }
        else
        {
            if (a.get<0>() == b.get<0>())
                return a.get<1>() < b.get<1>();

            return a.get<0>() < b.get<0>();
        }
    }
};

// create new block (without proof-of-work/proof-of-stake)
CBlock* CreateNewBlock(CWallet* pwallet, bool fProofOfStake, int64_t* pFees)
{
    std::unique_ptr<CBlock> pblock(new CBlock());

    if (!pblock.get())
        return NULL;

    CBlockIndex* pindexPrev = pindexBest;

    // Create coinbase tx
    CTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);

    if (!fProofOfStake)
    {
        CReserveKey reservekey(pwallet);
        CPubKey pubkey;

        if (!reservekey.GetReservedKey(pubkey))
            return NULL;

        txNew.vout[0].scriptPubKey << pubkey << OP_CHECKSIG;
    }
    else
    {
        // Height first in coinbase required for block.version=2
        txNew.vin[0].scriptSig = (CScript() << pindexPrev->nHeight + 1) + COINBASE_FLAGS;
        assert(txNew.vin[0].scriptSig.size() <= 100);
        txNew.vout[0].SetEmpty();
    }

    // Add our coinbase tx as first transaction
    pblock->vtx.push_back(txNew);

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", MAX_BLOCK_SIZE_GEN / 2);

    // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity
    nBlockMaxSize = std::max((unsigned int) 1000, std::min((unsigned int)(MAX_BLOCK_SIZE - 1000),
                             nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", 27000);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", 0);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Fee-per-kilobyte amount considered the same as "free"
    // Be careful setting this: if you set it to zero then
    // a transaction spammer can cheaply fill blocks using
    // 1-satoshi-fee transactions. It should be set above the real
    // cost to you of processing a transaction.
    int64_t nMinTxFee = MIN_TX_FEE;

    if (mapArgs.count("-mintxfee"))
        ParseMoney(mapArgs["-mintxfee"], nMinTxFee);

    pblock->nBits = GetNextTargetRequired(pindexPrev, fProofOfStake);

    // Collect memory pool transactions into the block
    int64_t nFees = 0;
    {
        LOCK2(cs_main, mempool.cs);
        CTxDB txdb("r");

        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());

        for (map<uint256, CTransaction>::iterator mi = mempool.mapTx.begin(); mi != mempool.mapTx.end(); ++mi)
        {
            CTransaction& tx = (*mi).second;

            if (tx.IsCoinBase() || tx.IsCoinStake() || !tx.IsFinal())
                continue;

            COrphan* porphan = NULL;
            double dPriority = 0;
            int64_t nTotalIn = 0;
            bool fMissingInputs = false;

            BOOST_FOREACH(const CTxIn& txin, tx.vin)
            {
                // Read prev transaction
                CTransaction txPrev;
                CTxIndex txindex;

                if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
                {
                    // This should never happen; all transactions in the memory
                    // pool should connect to either transactions in the chain
                    // or other transactions in the memory pool.
                    if (!mempool.mapTx.count(txin.prevout.hash))
                    {
                        LogPrintf("%s : [ERROR] mempool transaction missing input\n", __func__);

                        if (fDebug)
                            assert("mempool transaction missing input" == 0);

                        fMissingInputs = true;

                        if (porphan)
                            vOrphan.pop_back();

                        break;
                    }

                    // Has to wait for dependencies
                    if (!porphan)
                    {
                        // Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }

                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);
                    nTotalIn += mempool.mapTx[txin.prevout.hash].vout[txin.prevout.n].nValue;
                    continue;
                }

                int64_t nValueIn = txPrev.vout[txin.prevout.n].nValue;
                nTotalIn += nValueIn;

                int nConf = txindex.GetDepthInMainChain();
                dPriority += (double)nValueIn * nConf;
            }

            if (fMissingInputs)
                continue;

            // Priority is sum(valuein * age) / txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority /= nTxSize;

            // This is a more accurate fee-per-kilobyte than is used by the client code, because the
            // client code rounds up the size to the nearest 1K. That's good, because it gives an
            // incentive to create smaller transactions.
            double dFeePerKb =  double(nTotalIn-tx.GetValueOut()) / (double(nTxSize) / 1000.0);

            if (porphan)
            {
                porphan->dPriority = dPriority;
                porphan->dFeePerKb = dFeePerKb;
            }
            else
                vecPriority.push_back(TxPriority(dPriority, dFeePerKb, &(*mi).second));
        }

        // Collect transactions into block
        map<uint256, CTxIndex> mapTestPool;
        uint64_t nBlockSize = 1000;
        uint64_t nBlockTx = 0;
        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while (!vecPriority.empty())
        {
            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            double dFeePerKb = vecPriority.front().get<1>();
            CTransaction& tx = *(vecPriority.front().get<2>());

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize)
                continue;

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = tx.GetLegacySigOpCount();
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            // Timestamp limit
            if (tx.nTime > GetAdjustedTime() || (fProofOfStake && tx.nTime > pblock->vtx[0].nTime))
                continue;

            // Transaction fee
            int64_t nMinFee = tx.GetMinFee(nBlockSize, GMF_BLOCK);

            // Skip free transactions if we're past the minimum block size:
            if (fSortedByFee && (dFeePerKb < nMinTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
                continue;

            // Prioritize by fee once past the priority size or we run out of high-priority
            // transactions:
            if (!fSortedByFee &&
                ((nBlockSize + nTxSize >= nBlockPrioritySize) || (dPriority < COIN * 144 / 250)))
            {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            // Connecting shouldn't fail due to dependency on other memory pool transactions
            // because we're already processing them in order of dependency
            map<uint256, CTxIndex> mapTestPoolTmp(mapTestPool);
            MapPrevTx mapInputs;
            bool fInvalid;

            if (!tx.FetchInputs(txdb, mapTestPoolTmp, false, true, mapInputs, fInvalid))
                continue;

            int64_t nTxFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();

            if (nTxFees < nMinFee)
                continue;

            nTxSigOps += tx.GetP2SHSigOpCount(mapInputs);

            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            if (!tx.ConnectInputs(txdb, mapInputs, mapTestPoolTmp, CDiskTxPos(1,1,1), pindexPrev, false, true))
                continue;

            mapTestPoolTmp[tx.GetHash()] = CTxIndex(CDiskTxPos(1,1,1), tx.vout.size());
            swap(mapTestPool, mapTestPoolTmp);

            // Added
            pblock->vtx.push_back(tx);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if (fDebug && GetBoolArg("-printpriority"))
            {
                LogPrintf("%s : priority %.1f feeperkb %.1f txid %s\n", __func__,
                       dPriority, dFeePerKb, tx.GetHash().ToString().c_str());
            }

            // Add transactions that depend on this one to the priority queue
            uint256 hash = tx.GetHash();

            if (mapDependers.count(hash))
            {
                BOOST_FOREACH(COrphan* porphan, mapDependers[hash])
                {
                    if (!porphan->setDependsOn.empty())
                    {
                        porphan->setDependsOn.erase(hash);

                        if (porphan->setDependsOn.empty())
                        {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->dFeePerKb, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;

        if (fDebug && GetBoolArg("-printpriority"))
            LogPrintf("%s : total size %u\n", __func__, nBlockSize);

        if (!fProofOfStake)
            pblock->vtx[0].vout[0].nValue = GetProofOfWorkReward(nFees, pindexPrev->nHeight + 1);

        if (pFees)
            *pFees = nFees;

        // Fill in header
        pblock->hashPrevBlock = pindexPrev->GetBlockHash();
        pblock->nTime = max(pindexPrev->GetPastTimeLimit()+1, pblock->GetMaxTransactionTime());
        pblock->nTime = max(pblock->GetBlockTime(), PastDrift(pindexPrev->GetBlockTime()));

        if (!fProofOfStake)
            pblock->UpdateTime(pindexPrev);

        pblock->nNonce = 0;
    }

    return pblock.release();
}


void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;

    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }

    ++nExtraNonce;

    unsigned int nHeight = pindexPrev->nHeight + 1; // Height first in coinbase required for block.version=2
    pblock->vtx[0].vin[0].scriptSig = (CScript() << nHeight << CBigNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(pblock->vtx[0].vin[0].scriptSig.size() <= 100);
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}


void FormatHashBuffers(CBlock* pblock, char* pmidstate, char* pdata, char* phash1)
{
    // Pre-build hash buffers
    struct
    {
        struct unnamed2
        {
            int nVersion;
            uint256 hashPrevBlock;
            uint256 hashMerkleRoot;
            unsigned int nTime;
            unsigned int nBits;
            unsigned int nNonce;
        }
        block;

        unsigned char pchPadding0[64];
        uint256 hash1;
        unsigned char pchPadding1[64];
    }
    tmp;

    memset(&tmp, 0, sizeof(tmp));

    tmp.block.nVersion       = pblock->nVersion;
    tmp.block.hashPrevBlock  = pblock->hashPrevBlock;
    tmp.block.hashMerkleRoot = pblock->hashMerkleRoot;
    tmp.block.nTime          = pblock->nTime;
    tmp.block.nBits          = pblock->nBits;
    tmp.block.nNonce         = pblock->nNonce;

    FormatHashBlocks(&tmp.block, sizeof(tmp.block));
    FormatHashBlocks(&tmp.hash1, sizeof(tmp.hash1));

    // Byte swap all the input buffer
    for (unsigned int i = 0; i < sizeof(tmp) / 4; i++)
        ((unsigned int*)&tmp)[i] = ByteReverse(((unsigned int*) &tmp)[i]);

    // Precalc the first half of the first hash, which stays constant
    SHA256Transform(pmidstate, &tmp.block, pSHA256InitState);

    memcpy(pdata, &tmp.block, 128);
    memcpy(phash1, &tmp.hash1, 64);
}


bool CheckWork(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
    uint256 hashBlock = pblock->GetHash();
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

    if(!pblock->IsProofOfWork())
        return error("CheckWork() : %s is not a proof-of-work block", hashBlock.GetHex().c_str());

    if (hashBlock > hashTarget)
    {
        LogPrintf("blockhash=%s target=%s\n", hashBlock.GetHex().c_str(), hashTarget.GetHex().c_str());
        return error("CheckWork() : proof-of-work not meeting target");
    }

    //// debug print
    LogPrintf("CheckWork() : new proof-of-work block found  \n  hash: %s  \ntarget: %s\n", hashBlock.GetHex().c_str(), hashTarget.GetHex().c_str());
    pblock->print();
    LogPrintf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue).c_str());

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != hashBestChain)
            return error("CheckWork() : generated block is stale");

        // Remove key from key pool
        reservekey.KeepKey();

        // Track how many getdata requests this block gets
        {
            LOCK(wallet.cs_wallet);
            wallet.mapRequestCount[hashBlock] = 0;
        }

        // Process this block the same as if we had received it from another node
        if (!ProcessNewBlock(NULL, pblock))
            return error("CheckWork() : ProcessNewBlock, block not accepted");
    }

    return true;
}

bool CheckStake(CBlock* pblock, CWallet& wallet)
{
    uint256 proofHash = 0, hashTarget = 0;
    uint256 hashBlock = pblock->GetHash();

    if(!pblock->IsProofOfStake())
        return error("%s : %s is not a proof-of-stake block", __func__, hashBlock.GetHex().c_str());

    // verify hash target and signature of coinstake tx
    if (!CheckProofOfStake(mapBlockIndex[pblock->hashPrevBlock], pblock->vtx[1], pblock->nBits, proofHash, hashTarget))
        return error("%s : proof-of-stake checking failed", __func__);

    //// debug print
    LogPrintf("%s : new proof-of-stake block found  \n  hash: %s \nproofhash: %s  \ntarget: %s\n", __func__,
              hashBlock.GetHex().c_str(), proofHash.GetHex().c_str(), hashTarget.GetHex().c_str());
    pblock->print();
    LogPrintf("out %s\n", FormatMoney(pblock->vtx[1].GetValueOut()).c_str());

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != hashBestChain)
            return error("CheckStake() : generated block is stale");

        // Track how many getdata requests this block gets
        {
            LOCK(wallet.cs_wallet);
            wallet.mapRequestCount[hashBlock] = 0;
        }

        // Process this block the same as if we had received it from another node
        if (!ProcessNewBlock(NULL, pblock))
            return error("CheckStake() : ProcessNewBlock, block not accepted");
    }

    return true;
}

bool fGenerateBitcoins = false;
static bool fLimitProcessors = false;
static int nLimitProcessors = -1;
double dHashesPerSec;
int64_t nHPSTimerStart;

void StakeMiner(CWallet *pwallet, bool fProofOfStake)
{
    if (fDebug)
    {
        LogPrintf("StakeMiner: starting\n");
        LogPrintf("StakeMiner: detected total wallet balance is roughly %Lf\n", ((long double) pwallet->GetBalance()) / 100000000);
    }

    SetThreadPriority(THREAD_PRIORITY_LOWEST);

    // Make this thread recognisable as the mining thread
    RenameThread("Neutron-miner");

    // Each thread has its own key and counter
    CReserveKey reservekey(pwallet);
    unsigned int nExtraNonce = 0;
    bool fTryToSync = true;

    //control the amount of times the client will check for mintable coins
    static bool fMintableCoins = false;
    static int nMintableLastCheck = 0;

    if (fProofOfStake && (GetTime() - nMintableLastCheck > 5 * 60))
    {
        nMintableLastCheck = GetTime();
        fMintableCoins = pwallet->MintableCoins();
    }

    while (fGenerateBitcoins || fProofOfStake)
    {
        if (fShutdown)
            return;

        while (pwallet->IsLocked())
        {
            nLastCoinStakeSearchInterval = 0;
            MilliSleep(500);

            if (fShutdown)
                return;
        }

        while (vNodes.empty() || IsInitialBlockDownload())
        {
            nLastCoinStakeSearchInterval = 0;
            fTryToSync = true;
            MilliSleep(500);

            if (fShutdown)
                return;
        }

        while (!isMasternodeListSynced)
        {
            if (fDebug)
                LogPrintf("StakeMiner: waiting for mn list sync...\n");

            MilliSleep(1000);

            if (fShutdown)
                return;
        }

        while (!fMintableCoins)
        {
            if (fDebug)
                LogPrintf("StakeMiner: found no suitable inputs to stake...\n");

            nLastCoinStakeSearchInterval = 0;
            MilliSleep(5000);

            if (fShutdown)
                return;

            if (!fGenerateBitcoins && !fProofOfStake)
                continue;
        }

        if (fTryToSync)
        {
            fTryToSync = false;

            if (vNodes.empty() || nBestHeight < GetNumBlocksOfPeers())
            {
                MilliSleep(3000);
                continue;
            }
        }

        // Create new block
        unsigned int nTransactionsUpdatedLast = nTransactionsUpdated;
        CBlockIndex* pindexPrev = pindexBest;
        int64_t nFees;
        unique_ptr<CBlock> pblock(CreateNewBlock(pwallet, fProofOfStake, &nFees));

        if (!pblock.get())
            return;

        IncrementExtraNonce(pblock.get(), pindexPrev, nExtraNonce);

        if (fProofOfStake)
        {
            // Trying to sign a block
            if (pblock->SignBlock(*pwallet, nFees))
            {
                SetThreadPriority(THREAD_PRIORITY_NORMAL);
                CheckStake(pblock.get(), *pwallet);
                SetThreadPriority(THREAD_PRIORITY_LOWEST);
                MilliSleep(500);
            }
            else
                MilliSleep(nMinerSleep);

            continue;
        }

        // Pre-build hash buffers
        char pmidstatebuf[32+16]; char* pmidstate = alignup<16>(pmidstatebuf);
        char pdatabuf[128+16];    char* pdata     = alignup<16>(pdatabuf);
        char phash1buf[64+16];    char* phash1    = alignup<16>(phash1buf);

        FormatHashBuffers(pblock.get(), pmidstate, pdata, phash1);

        unsigned int& nBlockTime = *(unsigned int*)(pdata + 64 + 4);
        unsigned int& nBlockBits = *(unsigned int*)(pdata + 64 + 8);

        // Search
        int64_t nStart = GetTime();
        uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

        while (true)
        {
            unsigned int nHashesDone = 0;
            uint256 thash;

            while (true)
            {
                thash = pblock->GetHash();

                if (thash <= hashTarget)
                {
                    if (!pblock->SignBlock_POW(*pwallet))
                        break;

                    SetThreadPriority(THREAD_PRIORITY_NORMAL);
                    CheckWork(pblock.get(), *pwallet, reservekey);
                    SetThreadPriority(THREAD_PRIORITY_LOWEST);
                    break;
                }

                pblock->nNonce += 1;
                nHashesDone += 1;

                if ((pblock->nNonce & 0xFF) == 0)
                    break;
            }

            // Meter hashes/sec
            static int64_t nHashCounter;

            if (nHPSTimerStart == 0)
            {
                nHPSTimerStart = GetTimeMillis();
                nHashCounter = 0;
            }
            else
                nHashCounter += nHashesDone;

            if (GetTimeMillis() - nHPSTimerStart > 4000)
            {
                static CCriticalSection cs;
                {
                    LOCK(cs);

                    if (GetTimeMillis() - nHPSTimerStart > 4000)
                    {
                        dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
                        nHPSTimerStart = GetTimeMillis();
                        nHashCounter = 0;
                        static int64_t nLogTime;

                        if (GetTime() - nLogTime > 30 * 60)
                        {
                            nLogTime = GetTime();
                            LogPrintf("hashmeter %6.0f khash/s\n", dHashesPerSec/1000.0);
                        }
                    }
                }
            }

            // Check for stop or if block needs to be rebuilt
            boost::this_thread::interruption_point();

            if (vNodes.empty())
                break;

            if (pblock->nNonce >= 0xffff0000)
                break;

            if (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                break;

            if (pindexPrev != pindexBest)
                break;

            if (fShutdown)
                break;

            // Update nTime every few seconds
            pblock->UpdateTime(pindexPrev);
            nBlockTime = ByteReverse(pblock->nTime);

            if (fTestNet)
            {
                // Changing pblock->nTime can change work required on testnet:
                nBlockBits = ByteReverse(pblock->nBits);
                hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
            }
        }
    }
}

void static ThreadBitcoinMiner(void* parg)
{
    CWallet* pwallet = (CWallet*)parg;

    try
    {
        vnThreadsRunning[THREAD_MINER]++;
        StakeMiner(pwallet, false);
        vnThreadsRunning[THREAD_MINER]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_MINER]--;
        PrintException(&e, "ThreadBitcoinMiner()");
    } catch (...) {
        vnThreadsRunning[THREAD_MINER]--;
        PrintException(NULL, "ThreadBitcoinMiner()");
    }
    nHPSTimerStart = 0;
    if (vnThreadsRunning[THREAD_MINER] == 0)
        dHashesPerSec = 0;
    LogPrintf("ThreadBitcoinMiner exiting, %d threads remaining\n", vnThreadsRunning[THREAD_MINER]);
}

void GenerateBitcoins(bool fGenerate, CWallet* pwallet)
{
    fGenerateBitcoins = fGenerate;
    nLimitProcessors = GetArg("-genproclimit", -1);

    if (nLimitProcessors == 0)
        fGenerateBitcoins = false;

    fLimitProcessors = (nLimitProcessors != -1);

    if (fGenerate)
    {
        int nProcessors = boost::thread::hardware_concurrency();
        LogPrintf("%d processors\n", nProcessors);

        if (nProcessors < 1)
            nProcessors = 1;

        if (fLimitProcessors && nProcessors > nLimitProcessors)
            nProcessors = nLimitProcessors;

        int nAddThreads = nProcessors - vnThreadsRunning[THREAD_MINER];
        LogPrintf("Starting %d BitcoinMiner threads\n", nAddThreads);

        for (int i = 0; i < nAddThreads; i++)
        {
            if (!NewThread(ThreadBitcoinMiner, pwallet))
                LogPrintf("Error: NewThread(ThreadBitcoinMiner) failed\n");

            MilliSleep(1000);
        }
    }
}
