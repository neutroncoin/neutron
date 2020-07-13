// Copyright (c) 2014-2015 The Darkcoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "darksend.h"
#include "main.h"
#include "init.h"
#include "util.h"
#include "utiltime.h"
#include "masternode.h"
#include "ui_interface.h"
#include "txdb.h"

#include <openssl/rand.h>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/lexical_cast.hpp>
#include <algorithm>
#include <boost/assign/list_of.hpp>

using namespace std;
using namespace boost;

CCriticalSection cs_darksend;

// The main object for accessing darksend
CDarkSendPool darkSendPool;
// A helper object for signing messages from masternodes
CDarkSendSigner darkSendSigner;
// The current darksends in progress on the network
std::vector<CDarksendQueue> vecDarksendQueue;
// Keep track of the used masternodes
std::vector<CTxIn> vecMasternodesUsed;
// Keep track of the scanning errors I've seen
map<uint256, CDarksendBroadcastTx> mapDarksendBroadcastTxes;

CActiveMasternode activeMasternode;

// count peers we've requested the list from
int requestedMasterNodeList = 0;
bool isMasternodeListSynced = false;

void ProcessMessageDarksend(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (strCommand == "dsf") // DarkSend Final tx
    {
        if (pfrom->nVersion < darkSendPool.MIN_PEER_PROTO_VERSION)
            return;

        if((CNetAddr) darkSendPool.submittedToMasternode != (CNetAddr) pfrom->addr)
        {
            if (fDebug)
            {
                LogPrintf("dsf - message doesn't match current masternode - %s != %s\n",
                          darkSendPool.submittedToMasternode.ToString().c_str(), pfrom->addr.ToString().c_str());
            }

            return;
        }

        int sessionID;
        CTransaction txNew;
        vRecv >> sessionID >> txNew;

        if(darkSendPool.sessionID != sessionID)
        {
            if (fDebug)
            {
                LogPrintf("dsf - message doesn't match current darksend session %d %d\n",
                          darkSendPool.sessionID, sessionID);
            }

            return;
        }

        // TODO: Remove me
        // check to see if input is spent already? (and probably not confirmed)
        // darkSendPool.SignFinalTransaction(txNew, pfrom);
    }

    else if (strCommand == "dsc") // DarkSend Complete
    {
        if (pfrom->nVersion < darkSendPool.MIN_PEER_PROTO_VERSION)
            return;

        if((CNetAddr)darkSendPool.submittedToMasternode != (CNetAddr)pfrom->addr)
        {
            if (fDebug)
            {
                LogPrintf("dsc - message doesn't match current masternode - %s != %s\n",
                          darkSendPool.submittedToMasternode.ToString().c_str(), pfrom->addr.ToString().c_str());
            }

            return;
        }

        int sessionID;
        bool error;
        std::string lastMessage;
        vRecv >> sessionID >> error >> lastMessage;

        if(darkSendPool.sessionID != sessionID)
        {
            if (fDebug)
            {
                LogPrintf("dsc - message doesn't match current darksend session %d %d\n",
                          darkSendPool.sessionID, sessionID);
            }

            return;
        }

        // TODO: Remove me
        // darkSendPool.CompletedTransaction(error, lastMessage);
    }

    else if (strCommand == "dsa") //DarkSend Acceptable
    {
        if (pfrom->nVersion < darkSendPool.MIN_PEER_PROTO_VERSION)
        {
            std::string strError = _("Incompatible version.");
            LogPrintf("dsa -- incompatible version! \n");

            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                               darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, strError);
            return;
        }

        if(!fMasterNode)
        {
            std::string strError = _("This is not a masternode.");
            LogPrintf("dsa -- not a masternode! \n");

            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                               darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, strError);
            return;
        }

        int nDenom;
        CTransaction txCollateral;
        vRecv >> nDenom >> txCollateral;

        std::string error = "";
        int mn = GetMasternodeByVin(activeMasternode.vin);

        if (mn == -1)
        {
            std::string strError = _("Not in the masternode list.");
            LogPrintf("dsa -- not in the masternode list! \n");

            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                               darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, strError);
            return;
        }

        if (darkSendPool.sessionUsers == 0)
        {
            if (vecMasternodes[mn].nLastDsq != 0 && vecMasternodes[mn].nLastDsq +
                CountMasternodesAboveProtocol(darkSendPool.MIN_PEER_PROTO_VERSION) / 5 > darkSendPool.nDsqCount)
            {
                if (fDebug)
                    LogPrintf("dsa -- last dsq too recent, must wait. %s \n", vecMasternodes[mn].addr.ToString().c_str());

                std::string strError = _("Last Darksend was too recent.");

                pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                                   darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, strError);
                return;
            }
        }

        // TODO: Remove me
        // if (!darkSendPool.IsCompatibleWithSession(nDenom, txCollateral, error))
        // {
        //     LogPrintf("dsa -- not compatible with existing transactions! \n");
        //     pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
        //                        darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
        //     return;
        // }
        // else
        {
            LogPrintf("dsa -- is compatible, please submit! \n");
            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_ACCEPTED, error);
            return;
        }
    }
    else if (strCommand == "dsq") //DarkSend Queue
    {
        if (pfrom->nVersion < darkSendPool.MIN_PEER_PROTO_VERSION)
            return;

        CDarksendQueue dsq;
        vRecv >> dsq;
        CService addr;

        if (!dsq.GetAddress(addr))
            return;

        if (!dsq.CheckSignature())
            return;

        if (dsq.IsExpired())
            return;

        int mn = GetMasternodeByVin(dsq.vin);

        if (mn == -1)
            return;

        // if the queue is ready, submit if we can
        if (dsq.ready)
        {
            if((CNetAddr) darkSendPool.submittedToMasternode != (CNetAddr) addr)
            {
                if (fDebug)
                {
                    LogPrintf("dsq - message doesn't match current masternode - %s != %s\n",
                              darkSendPool.submittedToMasternode.ToString().c_str(), pfrom->addr.ToString().c_str());
                }

                return;
            }

            if (fDebug)
                LogPrintf("darksend queue is ready - %s\n", addr.ToString().c_str());

            // TODO: Remove me
            // darkSendPool.PrepareDarksendDenominate();
        }
        else
        {
            BOOST_FOREACH(CDarksendQueue q, vecDarksendQueue)
            {
                if(q.vin == dsq.vin)
                    return;
            }

            if(fDebug)
            {
                LogPrintf("dsq last %d last2 %d count %d\n", vecMasternodes[mn].nLastDsq,
                          vecMasternodes[mn].nLastDsq + (int)vecMasternodes.size()  /5, darkSendPool.nDsqCount);
            }

            // don't allow a few nodes to dominate the queuing process
            if (vecMasternodes[mn].nLastDsq != 0 && vecMasternodes[mn].nLastDsq +
                CountMasternodesAboveProtocol(darkSendPool.MIN_PEER_PROTO_VERSION)/5 > darkSendPool.nDsqCount)
            {
                if (fDebug)
                {
                    LogPrintf("dsq -- masternode sending too many dsq messages. %s \n",
                              vecMasternodes[mn].addr.ToString().c_str());
                }

                return;
            }

            darkSendPool.nDsqCount++;
            vecMasternodes[mn].nLastDsq = darkSendPool.nDsqCount;
            vecMasternodes[mn].allowFreeTx = true;

            if (fDebug)
                LogPrintf("dsq - new darksend queue object - %s\n", addr.ToString().c_str());

            vecDarksendQueue.push_back(dsq);
            dsq.Relay();
            dsq.time = GetTime();
        }
    }
    else if (strCommand == "dsi") //DarkSend vIn
    {
        std::string error = "";

        if (pfrom->nVersion < darkSendPool.MIN_PEER_PROTO_VERSION)
        {
            LogPrintf("dsi -- incompatible version! \n");
            error = _("Incompatible version.");

            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                               darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
            return;
        }

        if(!fMasterNode)
        {
            LogPrintf("dsi -- not a masternode! \n");
            error = _("This is not a masternode.");

            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                               darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
            return;
        }

        std::vector<CTxIn> in;
        int64_t nAmount;
        CTransaction txCollateral;
        std::vector<CTxOut> out;
        vRecv >> in >> nAmount >> txCollateral >> out;

        // do we have enough users in the current session?
        if(!darkSendPool.IsSessionReady())
        {
            LogPrintf("dsi -- session not complete! \n");
            error = _("Session not complete!");

            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                               darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
            return;
        }

        // TODO: Remove me
        // do we have the same denominations as the current session?
        // if(!darkSendPool.IsCompatibleWithEntries(out))
        // {
        //    LogPrintf("dsi -- not compatible with existing transactions! \n");
        //    error = _("Not compatible with existing transactions.");
        //
        //    pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
        //                        darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
        //    return;
        // }

        // check it like a transaction
        {
            int64_t nValueIn = 0;
            int64_t nValueOut = 0;
            bool missingTx = false;
            CTransaction tx;

            BOOST_FOREACH(CTxOut o, out)
            {
                nValueOut += o.nValue;
                tx.vout.push_back(o);

                if (o.scriptPubKey.size() != 25)
                {
                    LogPrintf("dsi -- non-standard pubkey detected! %s\n", o.scriptPubKey.ToString().c_str());
                    error = _("Non-standard public key detected.");

                    pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                                       darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
                    return;
                }

                if (!o.scriptPubKey.IsNormalPaymentScript())
                {
                    LogPrintf("dsi - invalid script! %s\n", o.scriptPubKey.ToString().c_str());
                    error = _("Invalid script detected.");

                    pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                                       darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
                    return;
                }
            }

            BOOST_FOREACH(const CTxIn i, in)
            {
                tx.vin.push_back(i);

                if(fDebug)
                    LogPrintf("dsi -- tx in %s\n", i.ToString().c_str());

                CTransaction tx2;
                uint256 hash;

                // if (GetTransaction(i.prevout.hash, tx2, hash, true)) {
                if (GetTransaction(i.prevout.hash, tx2, hash))
                {
                    if(tx2.vout.size() > i.prevout.n)
                        nValueIn += tx2.vout[i.prevout.n].nValue;
                }
                else
                    missingTx = true;
            }

            if (nValueIn > DARKSEND_POOL_MAX)
            {
                LogPrintf("dsi -- more than darksend pool max! %s\n", tx.ToString().c_str());
                error = _("Value more than Darksend pool maximum allows.");

                pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                                   darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
                return;
            }

            if (!missingTx)
            {
                if (nValueIn-nValueOut > nValueIn*.01)
                {
                    LogPrintf("dsi -- fees are too high! %s\n", tx.ToString().c_str());
                    error = _("Transaction fees are too high.");

                    pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                                       darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
                    return;
                }
            }
            else
            {
                LogPrintf("dsi -- missing input tx! %s\n", tx.ToString().c_str());
                error = _("Missing input transaction information.");

                pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                                   darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
                return;
            }

            // i f(!AcceptableInputs(mempool, state, tx)) {
            bool pfMissingInputs = false;

            if (!AcceptableInputs(mempool, tx, false, &pfMissingInputs))
            {
                LogPrintf("dsi -- transaction not valid! \n");
                error = _("Transaction not valid.");

                pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                                   darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
                return;
            }
        }

        // if (darkSendPool.AddEntry(in, nAmount, txCollateral, out, error))
        {
            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                               darkSendPool.GetEntriesCount(), MASTERNODE_ACCEPTED, error);

            // TODO: Remove me
            RelayDarkSendStatus(darkSendPool.sessionID, darkSendPool.GetState(),
                                darkSendPool.GetEntriesCount(), MASTERNODE_RESET);
        }
        // else
        // {
        //   pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
        //                       darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
        // }
    }

    else if (strCommand == "dssub") // DarkSend Subscribe To
    {
        if (pfrom->nVersion < darkSendPool.MIN_PEER_PROTO_VERSION)
            return;

        if (!fMasterNode)
            return;

        std::string error = "";

        pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(),
                           darkSendPool.GetEntriesCount(), MASTERNODE_RESET, error);
        return;
    }

    else if (strCommand == "dssu") // DarkSend status update
    {
        if (pfrom->nVersion < darkSendPool.MIN_PEER_PROTO_VERSION)
            return;

        if ((CNetAddr)darkSendPool.submittedToMasternode != (CNetAddr)pfrom->addr)
        {
            if (fDebug)
            {
                LogPrintf("dssu - message doesn't match current masternode - %s != %s\n",
                          darkSendPool.submittedToMasternode.ToString().c_str(), pfrom->addr.ToString().c_str());
            }

            return;
        }

        int sessionID;
        int state;
        int entriesCount;
        int accepted;
        std::string error;

        vRecv >> sessionID >> state >> entriesCount >> accepted >> error;

        if (fDebug)
        {
            LogPrintf("dssu - state: %i entriesCount: %i accepted: %i error: %s \n",
                      state, entriesCount, accepted, error.c_str());
        }

        if ((accepted != 1 && accepted != 0) && darkSendPool.sessionID != sessionID)
        {
            LogPrintf("dssu - message doesn't match current darksend session %d %d\n", darkSendPool.sessionID, sessionID);
            return;
        }

        // TODO: Remove me
        // darkSendPool.StatusUpdate(state, entriesCount, accepted, error, sessionID);

    }
    else if (strCommand == "dss") // DarkSend Sign Final Tx
    {
        if (pfrom->nVersion < darkSendPool.MIN_PEER_PROTO_VERSION)
            return;

        vector<CTxIn> sigs;
        vRecv >> sigs;
        bool success = true;
        int count = 0;

        LogPrintf(" -- sigs count %d %d\n", (int)sigs.size(), count);

        BOOST_FOREACH(const CTxIn item, sigs)
        {
            // if (darkSendPool.AddScriptSig(item))
            //    success = true;

            if(fDebug)
                LogPrintf(" -- sigs count %d %d\n", (int)sigs.size(), count);

            count++;
        }

        if (success)
        {
            // TODO: Remove me
            RelayDarkSendStatus(darkSendPool.sessionID, darkSendPool.GetState(),
                                darkSendPool.GetEntriesCount(), MASTERNODE_RESET);
        }
    }

}

int randomizeList(int i)
{
    return std::rand() % i;
}

void CDarkSendPool::Reset(){
    cachedLastSuccess = 0;
    vecMasternodesUsed.clear();
}

bool CDarkSendPool::SetCollateralAddress(std::string strAddress){
   CBitcoinAddress address;
   if (!address.SetString(strAddress))
   {
       LogPrintf("CDarkSendPool::SetCollateralAddress - Invalid DarkSend collateral address\n");
       return false;
   }
   collateralPubKey= GetScriptForDestination(address.Get());
    return true;
}

bool CDarkSendPool::IsDenominatedAmount(int64_t nInputAmount)
{
    BOOST_FOREACH(int64_t d, darkSendDenominations)
    {
        if(nInputAmount == d)
            return true;
    }

    return false;
}

bool CDarkSendSigner::IsVinAssociatedWithPubkey(CTxIn& vin, CPubKey& pubkey){
    CScript payee2;
    payee2= GetScriptForDestination(pubkey.GetID());

    CTransaction txVin;
    uint256 hash;

    // if (GetTransaction(vin.prevout.hash, txVin, hash, true)){
    if (GetTransaction(vin.prevout.hash, txVin, hash))
    {
        BOOST_FOREACH(CTxOut out, txVin.vout)
        {
            if(out.nValue == 25000 * COIN)
            {
                if(out.scriptPubKey == payee2)
                    return true;
            }
        }
    }

    return false;
}

bool CDarkSendSigner::SetKey(std::string strSecret, std::string& errorMessage, CKey& key, CPubKey& pubkey)
{
    CBitcoinSecret vchSecret;
    bool fGood = vchSecret.SetString(strSecret);

    if (!fGood)
    {
        errorMessage = _("Invalid private key.");
        return false;
    }

    bool fCompressedOut;
    CSecret scrt = vchSecret.GetSecret(fCompressedOut);
    key.SetSecret(scrt, fCompressedOut);
    pubkey = key.GetPubKey();

    return true;
}

bool CDarkSendSigner::SignMessage(std::string strMessage, std::string& errorMessage, vector<unsigned char>& vchSig, CKey key)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    if (!key.Sign(ss.GetHash(), vchSig))
    {
        errorMessage = _("Signing failed.");
        return false;
    }

    return true;
}

bool CDarkSendSigner::VerifyMessage(CPubKey pubkey, vector<unsigned char>& vchSig, std::string strMessage, std::string& errorMessage)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CKey key;
    key.SetPubKey(pubkey);
    return key.Verify(ss.GetHash(), vchSig);
}

bool CDarksendQueue::Sign()
{
    if (!fMasterNode)
        return false;

    std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nDenom) + boost::lexical_cast<std::string>(time) +
                             boost::lexical_cast<std::string>(ready);
    CKey key2;
    CPubKey pubkey2;
    std::string errorMessage = "";

    if (!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, key2, pubkey2))
    {
        LogPrintf("CDarksendQueue():Relay - ERROR: Invalid masternodeprivkey: '%s'\n", errorMessage.c_str());
        return false;
    }

    if (!darkSendSigner.SignMessage(strMessage, errorMessage, vchSig, key2))
    {
        LogPrintf("CDarksendQueue():Relay - Sign message failed");
        return false;
    }

    if (!darkSendSigner.VerifyMessage(pubkey2, vchSig, strMessage, errorMessage))
    {
        LogPrintf("CDarksendQueue():Relay - Verify message failed");
        return false;
    }

    return true;
}

bool CDarksendQueue::Relay()
{
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        // always relay to everyone
        pnode->PushMessage("dsq", (*this));
    }

    return true;
}

bool CDarksendQueue::CheckSignature()
{
    BOOST_FOREACH(CMasternode& mn, vecMasternodes)
    {
        if (mn.vin == vin)
        {
            std::string errorMessage = "";
            std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nDenom) +
                                     boost::lexical_cast<std::string>(time) + boost::lexical_cast<std::string>(ready);

            if (!darkSendSigner.VerifyMessage(mn.pubkey2, vchSig, strMessage, errorMessage))
                return error("CDarksendQueue::CheckSignature() - Got bad masternode address signature %s \n", vin.ToString().c_str());

            return true;
        }
    }

    return false;
}

// TODO: Rename and move to core
void ThreadCheckDarkSend(CConnman& connman)
{
    if (fDebug)
        LogPrintf("%s : Started\n", __func__);

    static bool fOneThread;

    if (fOneThread)
        return;

    fOneThread = true;

    // Make this thread recognisable as the wallet flushing thread
    RenameThread("neutron-darksend");

    unsigned int nTick = 0;

    bool waitMnSyncStarted = false;
    int64_t nMnSyncWaitTime = GetTime();

    while (true)
    {
        if (!IsInitialBlockDownload())
        {
            nTick++;

            {
                if (nTick % 60 == 0)
                {
                    LOCK(cs_main);

                    // cs_main is required for doing CMasternode.Check because something
                    // is modifying the coins view without a mempool lock. It causes
                    // segfaults from this code without the cs_main lock.

                    if (fDebug)
                        LogPrintf("%s : Check timeout\n", __func__);

                    mnodeman.CheckAndRemove();
                    masternodePayments.CleanPaymentList();
                }
            }

            if (fDebug)
                LogPrintf("%s : %d, %d\n", nTick % 5, __func__, requestedMasterNodeList);

            // every X ticks we try to send some requests (as controlled by the spork)
            if (nTick % sporkManager.GetSporkValue(SPORK_14_MASTERNODE_DISTRIBUTION_TICK) == 0)
            {
                LOCK(cs_vNodes);

                if (!vNodes.empty())
                {
                    // randomly clear a node in order to get constant syncing of the lists
                    int index = GetRandInt(vNodes.size());

                    vNodes[index]->ClearFulfilledRequest("getspork");
                    vNodes[index]->ClearFulfilledRequest("mnsync");
                    vNodes[index]->ClearFulfilledRequest("mnwsync");
                }

                if (fDebug)
                    LogPrintf("%s : Asking peers for sporks and masternode list\n", __func__);

                int sentRequests = 0;

                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    if (!pnode->HasFulfilledRequest("getspork"))
                    {
                        pnode->FulfilledRequest("getspork");
                        pnode->PushMessage(NetMsgType::GETSPORKS); // get current network sporks
                        sentRequests++;
                    }

                    if (!pnode->HasFulfilledRequest("mnsync"))
                    {
                        pnode->FulfilledRequest("mnsync");
                        pnode->PushMessage(NetMsgType::DSEG, CTxIn()); // request full mn list
                        sentRequests++;
                    }

                    if (pnode->HasFulfilledRequest("mnwsync"))
                    {
                        pnode->FulfilledRequest("mnwsync");
                        pnode->PushMessage(NetMsgType::MASTERNODEPAYMENTSYNC); // sync payees (winners list)
                        sentRequests++;
                    }

                    if (fDebug)
                        LogPrintf("%s : Synced with peer=%s\n", __func__, pnode->id);

                    requestedMasterNodeList++;

                    if (sentRequests >= MAX_REQUESTS_PER_TICK_CYCLE)
                        break;
                }

                if (!isMasternodeListSynced)
                {
                    if (!waitMnSyncStarted && (requestedMasterNodeList > 5 && mnodeman.CountEnabled() > 3))
                    {
                        waitMnSyncStarted = true;
                        nMnSyncWaitTime = GetTime() + 20;
                        LogPrintf("%s : Started waiting for mnsync", __func__);
                    }

                    LogPrintf("%s : waiting... requested=%d, enabled=%d, time_remaining=%d\n", __func__,
                              requestedMasterNodeList, mnodeman.CountEnabled(), nMnSyncWaitTime-GetTime());

                    if (waitMnSyncStarted && (GetTime() >= nMnSyncWaitTime))
                    {
                        LogPrintf("%s : complete... setting isMasternodeListSynced - requested=%d, enabled=%d\n",
                                  __func__, requestedMasterNodeList, mnodeman.CountEnabled());

                        // Calculate a few masternode winners first
                        masternodePayments.ProcessBlock(pindexBest->nHeight);
                        masternodePayments.ProcessBlock(pindexBest->nHeight + 1);
                        masternodePayments.ProcessBlock(pindexBest->nHeight + 2);

                        // ... then also fill in previous winners on this chain
                        CBlockIndex *pindex = pindexBest;
                        CTxDB txdb("r");

                        for (int i = 0; i < 30; i++)
                        {
                            CBlock block;
                            pindex = pindex->pprev;

                            if (block.ReadFromDisk(pindex->nFile, pindex->nBlockPos, true))
                            {
                                uint64_t nCoinAge;

                                if (block.vtx[1].GetCoinAge(txdb, nCoinAge))
                                {

                                    map<uint256, CTxIndex> mapQueuedChanges;
                                    int64_t nFees = 0;
                                    int64_t nValueIn = 0;
                                    int64_t nValueOut = 0;
                                    int64_t nStakeReward = 0;

                                    if (block.CalculateBlockAmounts(txdb, pindex, mapQueuedChanges, nFees, nValueIn,
                                                                    nValueOut, nStakeReward, true, true, false))

                                    {
                                        int64_t nCalculatedStakeReward = GetProofOfStakeReward(
                                              nCoinAge, nFees, pindex->nHeight
                                        );

                                        masternodePayments.AddPastWinningMasternode(block.vtx,
                                            GetMasternodePayment(pindex->nHeight, nCalculatedStakeReward),
                                            pindex->nHeight
                                        );
                                    }
                                }
                            }
                        }

                        isMasternodeListSynced = true;
                    }
                }
            }

            if(nTick % MASTERNODE_PING_SECONDS == 0)
                activeMasternode.ManageStatus(*g_connman);

            // TODO: NTRN - disabled for now
            // darkSendPool.CheckTimeout();
            // darkSendPool.CheckForCompleteQueue();

            // TODO: NTRN - disabled for now
            // if(nTick % (60*5) == 0){
            //     int nMnCountEnabled = mnodeman.CountEnabled(ActiveProtocol());

            //     // If we've used 90% of the Masternode list then drop the oldest first ~30%
            //     int nThreshold_high = nMnCountEnabled * 0.9;
            //     int nThreshold_low = nThreshold_high * 0.7;
            //     LogPrintf("ThreadCheckDarkSend::Checking vecMasternodesUsed: size: %d, threshold: %d\n",
            //               (int) vecMasternodesUsed.size(), nThreshold_high);

            //     if((int)vecMasternodesUsed.size() > nThreshold_high) {
            //         vecMasternodesUsed.erase(vecMasternodesUsed.begin(), vecMasternodesUsed.begin() +
            //                                  vecMasternodesUsed.size() - nThreshold_low);
            //         LogPrintf("ThreadCheckDarkSend::Cleaning vecMasternodesUsed: new size: %d, threshold: %d\n",
            //                   (int) vecMasternodesUsed.size(), nThreshold_high);
            //     }
            // }
        }

	MilliSleep(500); // Sleep for half a second before the next tick
    }
}
