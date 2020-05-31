// Copyright (c) 2009-2012 The Darkcoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternode.h"
#include "activemasternode.h"
#include "darksend.h"
#include "main.h"
#include "script/standard.h"
#include "util.h"
#include "addrman.h"

#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <sstream>

CCriticalSection cs_masternodes;

CMasternodeMan mnodeman;
std::vector<CMasternode> vecMasternodes;
CMasternodePayments masternodePayments;
map<uint256, CMasternodePaymentWinner> mapSeenMasternodeVotes;
map<uint256, int> mapSeenMasternodeScanningErrors;
std::map<CNetAddr, int64_t> mAskedUsForMasternodeList;
std::map<COutPoint, int64_t> mWeAskedForMasternodeListEntry;
std::map<int64_t, uint256> mapCacheBlockHashes;

// manage the masternode connections
void ProcessMasternodeConnections()
{
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        //if it's our masternode, let it be
        if (darkSendPool.submittedToMasternode == pnode->addr)
            continue;

        if (pnode->fDarkSendMaster)
        {
            LogPrintf("%s : closing masternode connection %s\n", __func__, pnode->addr.ToString().c_str());
            pnode->CloseSocketDisconnect();
        }
    }
}

void ProcessMessageMasternode(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (IsInitialBlockDownload())
        return;

    if (strCommand == NetMsgType::DSEE)
    {
        CTxIn vin;
        CService addr;
        CPubKey pubkey;
        CPubKey pubkey2;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        int count;
        int current;
        int64_t lastUpdated;
        int protocolVersion;
        std::string strMessage;

        // 70047 and greater
        vRecv >> vin >> addr >> vchSig >> sigTime >> pubkey >> pubkey2 >> count >> current >>
                 lastUpdated >> protocolVersion;

        if (fDebug)
        {
            LogPrintf("%s : dsee - received: node: %s, vin: %s, addr: %s, sigTime: %lld, pubkey: %s, pubkey2: %s, "
                      "count: %d, current: %d, lastUpdated: %lld, protocol: %d\n", __func__,
                      pfrom->addr.ToString().c_str(),  vin.ToString().c_str(), addr.ToString(), sigTime,
                      pubkey.GetHash().ToString(), pubkey2.GetHash().ToString(), count, current, lastUpdated,
                      protocolVersion);
        }

        // make sure signature isn't in the future (past is OK)
        if (sigTime > GetAdjustedTime() + 60 * 60)
        {
            std::stringstream msg;
            msg << boost::format("%s : dsee - signature rejected, too far into the future %s") %
                __func__ % vin.prevout.hash.ToString();

            LogPrintf("%s\n", msg.str().c_str());
            pfrom->Misbehaving(msg.str(), 1);
            return;
        }

        bool isLocal = addr.IsRFC1918() || addr.IsLocal();
        // if(Params().MineBlocksOnDemand()) isLocal = false;

        std::string vchPubKey(pubkey.vchPubKey.begin(), pubkey.vchPubKey.end());
        std::string vchPubKey2(pubkey2.vchPubKey.begin(), pubkey2.vchPubKey.end());

        strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) + vchPubKey + vchPubKey2 +
                     boost::lexical_cast<std::string>(protocolVersion);

        if (protocolVersion < ActiveProtocol())
        {
            std::stringstream msg;
            msg << boost::format("%s : dsee - ignoring masternode %s using outdated protocol version %d") %
                __func__ % vin.ToString().c_str() % protocolVersion;

            LogPrintf("%s\n", msg.str().c_str());
            pfrom->Misbehaving(msg.str(), 15);
            return;
        }

        CScript pubkeyScript;
        pubkeyScript = GetScriptForDestination(pubkey.GetID());

        if (pubkeyScript.size() != 25)
        {
            std::stringstream msg;
            msg << boost::format("%s : dsee - pubkey wrong size") % __func__;

            LogPrintf("%s\n", msg.str().c_str());
            pfrom->Misbehaving(msg.str(), 100);
            return;
        }

        CScript pubkeyScript2;
        pubkeyScript2 =GetScriptForDestination(pubkey2.GetID());

        if (pubkeyScript2.size() != 25)
        {
            std::stringstream msg;
            msg << boost::format("%s : dsee - pubkey2 the wrong size") % __func__;

            LogPrintf("%s\n", msg.str().c_str());
            pfrom->Misbehaving(msg.str(), 100);
            return;
        }

        std::string errorMessage = "";

        if (!darkSendSigner.VerifyMessage(pubkey, vchSig, strMessage, errorMessage))
        {
            std::stringstream msg;
            msg << boost::format("%s : dsee - got bad masternode address signature") % __func__;

            LogPrintf("%s\n", msg.str().c_str());
            pfrom->Misbehaving(msg.str(), 100);
            return;
        }

        // search existing masternode list, this is where we update existing masternodes with new dsee broadcasts
        CMasternode* pmn = mnodeman.Find(vin);
        if (pmn != NULL)
        {
            if (fDebug)
            {
                LogPrintf("%s : dsee - found existing masternode %s - %s - %s\n", __func__,
                          pmn->addr.ToString().c_str(), vin.ToString().c_str(),
                          pmn->UpdatedWithin(MASTERNODE_MIN_DSEE_SECONDS));
            }

            // count == -1 when it's a new entry
            //   e.g. We don't want the entry relayed/time updated when we're syncing the list
            // mn.pubkey = pubkey, IsVinAssociatedWithPubkey is validated once below,
            //   after that they just need to match

            if (count == -1 && pmn->pubkey == pubkey && !pmn->UpdatedWithin(MASTERNODE_MIN_DSEE_SECONDS))
            {
                LogPrintf("%s : dsee - update masternode last seen for %s\n", __func__, addr.ToString().c_str());
                pmn->UpdateLastSeen();

                if (pmn->now < sigTime)
                {
                    LogPrintf("%s : dsee - Got updated entry for %s\n", __func__, addr.ToString().c_str());

                    pmn->pubkey2 = pubkey2;
                    pmn->now = sigTime;
                    pmn->sig = vchSig;
                    pmn->protocolVersion = protocolVersion;
                    pmn->addr = addr;

                    RelayDarkSendElectionEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);
                }
            }

            return;
        }

        // make sure the vout that was signed is related to the transaction that spawned the masternode
        //  - this is expensive, so it's only done once per masternode
        if (!darkSendSigner.IsVinAssociatedWithPubkey(vin, pubkey))
        {
            std::stringstream msg;
            msg << boost::format("%s : dsee - got mismatched pubkey and vin") % __func__;

            LogPrintf("%s\n", msg.str().c_str());
            pfrom->Misbehaving(msg.str(), 100);
            return;
        }

        if (fDebug)
            LogPrintf("%s : dsee - got new masternode entry %s\n", __func__, addr.ToString().c_str());

        // make sure it's still unspent
        //  - this is checked later by .check() in many places and by ThreadCheckDarkSend()

        CTransaction tx = CTransaction();
        CTxOut vout = CTxOut(24999*COIN, darkSendPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);
        bool pfMissingInputs = false;

        if (AcceptableInputs(mempool, tx, false, &pfMissingInputs))
        {
            if (fDebug)
                LogPrintf("%s : dsee - accepted masternode entry %i %i\n", __func__, count, current);

            if (GetInputAge(vin) < MASTERNODE_MIN_CONFIRMATIONS)
            {
                std::stringstream msg;
                msg << boost::format("%s : dsee - input must have at least %d confirmations") %
                    __func__ % MASTERNODE_MIN_CONFIRMATIONS;

                LogPrintf("%s\n", msg.str().c_str());
                pfrom->Misbehaving(msg.str(), 20);
                return;
            }

            // use this as a peer
            addrman.Add(CAddress(addr), pfrom->addr, 2 * 60 * 60);

            // add our masternode
            CMasternode mn(addr, vin, pubkey, vchSig, sigTime, pubkey2, protocolVersion);
            mn.UpdateLastSeen(lastUpdated);
            vecMasternodes.push_back(mn);

            // if it matches our masternodeprivkey, then we've been remotely activated
            if (pubkey2 == activeMasternode.pubKeyMasternode && protocolVersion == PROTOCOL_VERSION)
                activeMasternode.EnableHotColdMasterNode(vin, addr);

            if (count == -1 && !isLocal)
            {
                RelayDarkSendElectionEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count,
                                           current, lastUpdated, protocolVersion);
            }

        }
        else
        {
            LogPrintf("%s : dsee - rejected masternode entry %s\n", __func__, addr.ToString().c_str());

            /*int nDoS = 0;
            if (state.IsInvalid(nDoS))
            {
                LogPrintf("dsee - %s from %s %s was not accepted into the memory pool\n", tx.GetHash().ToString().c_str(),
                    pfrom->addr.ToString().c_str(), pfrom->cleanSubVer.c_str());
                if (nDoS > 0)
                    pfrom->Misbehaving(nDoS);
            }*/
        }
    }
    else if (strCommand == NetMsgType::DSEEP)
    {
        CTxIn vin;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        bool stop;

        vRecv >> vin >> vchSig >> sigTime >> stop;

        if (fDebug)
        {
            LogPrintf("%s : dseep - received: node: %s, vin: %s, sigTime: %lld, stop: %s\n", __func__,
                      pfrom->addr.ToString().c_str(), vin.ToString().c_str(), sigTime, stop ? "true" : "false");
        }

        if (sigTime > GetAdjustedTime() + 60 * 60)
        {
            LogPrintf("%s : dseep - signature rejected, too far into the future %s\n", __func__,
                      vin.ToString().c_str());

            // pfrom->Misbehaving(1);
            return;
        }
        else if (sigTime <= GetAdjustedTime() - 60 * 60)
        {
            LogPrintf("%s : dseep - signature rejected, too far into the past %s - %s - %ld less than %ld\n",
                      __func__, pfrom->addr.ToString().c_str(), vin.ToString().c_str(), sigTime,
                      GetAdjustedTime() - 60 * 60);

            // pfrom->Misbehaving(1);
            return;
        }

        // see if we have this masternode
        CMasternode* pmn = mnodeman.Find(vin);

        if (pmn != NULL)
        {
            if (fDebug)
            {
                LogPrintf("%s : dseep - found corresponding mn for vin=%s addr=%s\n", __func__,
                          vin.ToString().c_str(), pmn->addr.ToString());
            }

            // take this only if it's newer
            if (sigTime - pmn->lastDseep > MASTERNODE_MIN_DSEEP_SECONDS)
            {
                std::string strMessage = pmn->addr.ToString() + boost::lexical_cast<std::string>(sigTime) +
                                         boost::lexical_cast<std::string>(stop);

                if (fDebug)
                {
                    LogPrintf("%s : dseep - got newer sigTime, sigTime=%d lastDseep=%d\n",
                              __func__, sigTime, pmn->lastDseep);
                }

                std::string errorMessage = "";

                if (!darkSendSigner.VerifyMessage(pmn->pubkey2, vchSig, strMessage, errorMessage))
                {
                    std::stringstream msg;
                    msg << boost::format("%s : dseep - got bad masternode address signature %s") %
                        __func__ % vin.ToString().c_str();

                    LogPrintf("%s\n", msg.str().c_str());
                    pfrom->Misbehaving(msg.str(), 100);
                    return;
                }

                pmn->lastDseep = sigTime;
                pmn->Check();

                if (pmn->IsEnabled())
                {
                    if (fDebug)
                        LogPrintf("%s : dseep - masternode is enabled addr=%s\n", __func__, pmn->addr.ToString());

                    if (stop)
                        pmn->Disable();
                    else
                    {
                        if (fDebug)
                            LogPrintf("%s : dseep - updatingLastSeen addr=%s\n", __func__, pmn->addr.ToString());

                        pmn->UpdateLastSeen();
                    }

                    TRY_LOCK(cs_vNodes, lockNodes);

                    if (!lockNodes)
                        return;

                    if  (fDebug)
                    {
                        LogPrintf("%s : dseep - relaying %s - %s\n", __func__, pmn->addr.ToString(),
                                  vin.prevout.hash.ToString());
                    }

                    RelayDarkSendElectionEntryPing(vin, vchSig, sigTime, stop);
                }
            }

            return;
        }

        if (fDebug)
            LogPrintf("%s : dseep - couldn't find masternode entry %s\n", __func__, vin.ToString().c_str());

        mnodeman.AskForMN(pfrom, vin);

    }
    else if (strCommand == NetMsgType::DSEG) // Get masternode list or specific entry
    {
        CTxIn vin;
        vRecv >> vin;

        if (fDebug)
        {
            LogPrintf("%s : dseg - masternode list, vin=%s, prevout=%s\n", __func__, vin.ToString().c_str(),
                      vin.prevout.hash.ToString());
        }

        if (vin == CTxIn()) // Should only ask for this once
        {
            std::map<CNetAddr, int64_t>::iterator i = mAskedUsForMasternodeList.find(pfrom->addr);

            if (i != mAskedUsForMasternodeList.end())
            {
                // int64_t t = (*i).second;
                // if (GetTime() < t)
                // {
                //   LogPrintf("dseg - peer already asked me for the list, peer=%d (%s)\n", pfrom->id, pfrom->addr.ToString().c_str());
                //   pfrom->Misbehaving(34);
                //   return;
                // }
            }

            int64_t askAgain = GetTime() + MASTERNODE_DSEG_SECONDS;
            mAskedUsForMasternodeList[pfrom->addr] = askAgain;
        } // else, asking for a specific node which is ok

        LOCK(cs_masternodes);
        int count = vecMasternodes.size();
        int i = 0;

        BOOST_FOREACH(CMasternode mn, vecMasternodes)
        {
            if (mn.addr.IsRFC1918())
                continue; // local network

            if (vin == CTxIn())
            {
                mn.Check();

                if (mn.IsEnabled())
                {
                    if (fDebug)
                    {
                        LogPrintf("%s : dseg - sending masternode entry - %s\n", __func__,
                                  mn.addr.ToString().c_str());
                    }

                    pfrom->PushMessage(NetMsgType::DSEE, mn.vin, mn.addr, mn.sig, mn.now, mn.pubkey, mn.pubkey2,
                                       count, i, mn.lastTimeSeen, mn.protocolVersion);
                }
            }
            else if (vin == mn.vin)
            {
                if (fDebug)
                {
                    LogPrintf("%s : dseg - sending masternode entry - %s\n", __func__,
                              mn.addr.ToString().c_str());
                }

                pfrom->PushMessage(NetMsgType::DSEE, mn.vin, mn.addr, mn.sig, mn.now, mn.pubkey, mn.pubkey2,
                                   count, i, mn.lastTimeSeen, mn.protocolVersion);

                LogPrintf("%s : dseg - sent single masternode entry to peer %s (%s)\n",
                          __func__, pfrom->GetId(), pfrom->addr.ToString().c_str());
                return;
            }

            i++;
        }

        LogPrintf("%s : dseg - sent %d masternode entries to %s\n", __func__, count,
                  pfrom->addr.ToString().c_str());
    }
    else if (strCommand == NetMsgType::MASTERNODEPAYMENTSYNC) // Masternode Payments Request Sync
    {
        if (pfrom->HasFulfilledRequest(NetMsgType::MASTERNODEPAYMENTSYNC))
        {
            if (fDebug)
            {
                LogPrintf("%s : mnget - peer already asked me for the list, peer=%d (%s)\n",
                          __func__, pfrom->id, pfrom->addr.ToString().c_str());
            }

            return;
        }

        // Ignore such requests until we are fully synced.
        // We could start processing this after masternode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!isMasternodeListSynced)
            return;

        pfrom->FulfilledRequest(NetMsgType::MASTERNODEPAYMENTSYNC);
        masternodePayments.Sync(pfrom);

        LogPrintf("%s : mnget - sent masternode winners to peer %s (%s)\n", __func__,
                  pfrom->id, pfrom->addr.ToString().c_str());
    }
    else if (strCommand == NetMsgType::MASTERNODEPAYMENTVOTE) // Masternode payments declare winner
    {
        CMasternodePaymentWinner winner;
        int a = 0;
        vRecv >> winner >> a;

        if (pfrom->nVersion < ActiveProtocol())
        {
            LogPrintf("%s : mnw -- peer=%d (%s) using obsolete version %i\n", __func__,
                      pfrom->id, pfrom->addr.ToString().c_str(), pfrom->nVersion);

            pfrom->PushMessage(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE,
                               strprintf("version must be %d or greater", ActiveProtocol()));
            return;
        }

        if (pindexBest == NULL)
            return;

        CTxDestination address1;
        ExtractDestination(winner.payee, address1);
        CBitcoinAddress address2(address1);

        uint256 nHash = winner.GetHash();

        if (mapSeenMasternodeVotes.count(nHash))
        {
            if (fDebug)
            {
                LogPrintf("%s : mnw - seen vote %s address=%s nBlockHeight=%d nHeight=%d\n", __func__,
                          nHash.ToString(), address2.ToString(), winner.nBlockHeight, pindexBest->nHeight);
            }

            return;
        }

        int nFirstBlock = pindexBest->nHeight - (mnodeman.CountEnabled() * 1.25);

        if (winner.nBlockHeight < nFirstBlock || winner.nBlockHeight > pindexBest->nHeight + 20)
        {
            if (fDebug)
            {
                LogPrintf("%s : mnw - winner out of range - nFirstBlock=%d, nBlockHeight=%d, nHeight=%d\n",
                          __func__, nFirstBlock, winner.nBlockHeight, pindexBest->nHeight);
            }

            return;
        }

        if (winner.vin.nSequence != std::numeric_limits<unsigned int>::max())
        {
            std::stringstream msg;
            msg << boost::format("%s : mnw - invalid nSequence") % __func__;

            LogPrintf("%s\n", msg.str().c_str());
            pfrom->Misbehaving(msg.str(), 100);
            return;
        }

        int nDos = 0; //TODO: What's going on here? What's the point?

        if (!masternodePayments.CheckSignature(winner))
        {
            if (fDebug)
            {
                LogPrintf("%s : mnw - debug - address=%s nBlockHeight=%d nHeight=%d, prevout=%s\n", __func__,
                          address2.ToString(), winner.nBlockHeight, pindexBest->nHeight,
                          winner.vin.prevout.ToStringShort());
            }

            if (nDos)
            {
                std::stringstream msg;
                msg << boost::format("%s : mnw - [ERROR] invalid signature") % __func__;

                LogPrintf("%s\n", msg.str().c_str());
                pfrom->Misbehaving(msg.str(), nDos);
            }
            else
            {
                // only warn about anything non-critical (i.e. nDos == 0) in debug mode
                if (fDebug)
                    LogPrintf("%s : mnw - [WARNING] invalid signature\n", __func__);
            }

            // Either our info or vote info could be outdated.
            // In case our info is outdated, ask for an update,
            mnodeman.AskForMN(pfrom, winner.vin);

            // but there is nothing we can do if vote info itself is outdated
            // (i.e. it was signed by a mn which changed its key),
            // so just quit here.
            return;
        }

        LogPrintf("%s : mnw - new vote - address=%s, nBlockHeight=%d, nHeight=%d, prevout=%s, nHash=%s\n",
                  __func__, address2.ToString(), winner.nBlockHeight, pindexBest->nHeight,
                  winner.vin.prevout.ToStringShort(), nHash.ToString());

        mapSeenMasternodeVotes.insert(make_pair(nHash, winner));

        if (masternodePayments.AddWinningMasternode(winner))
            masternodePayments.Relay(winner);
    }
}

struct CompareValueOnly
{
    bool operator()(const pair<int64_t, CTxIn>& t1,
                    const pair<int64_t, CTxIn>& t2) const
    {
        return t1.first < t2.first;
    }
};

struct CompareValueOnly2
{
    bool operator()(const pair<int64_t, int>& t1,
                    const pair<int64_t, int>& t2) const
    {
        return t1.first < t2.first;
    }
};

int CountMasternodesAboveProtocol(int protocolVersion)
{
    int i = 0;
    LOCK(cs_masternodes);

    BOOST_FOREACH(CMasternode& mn, vecMasternodes)
    {
        if (mn.protocolVersion < protocolVersion)
            continue;

        i++;
    }

    return i;
}

int GetMasternodeByVin(CTxIn& vin)
{
    int i = 0;
    LOCK(cs_masternodes);

    BOOST_FOREACH(CMasternode& mn, vecMasternodes)
    {
        if (mn.vin == vin)
            return i;

        i++;
    }

    return -1;
}

int GetCurrentMasterNode(int mod, int64_t nBlockHeight, int minProtocol)
{
    int i = 0;
    unsigned int score = 0;
    int winner = -1;
    LOCK(cs_masternodes);

    // scan for winner
    BOOST_FOREACH(CMasternode mn, vecMasternodes)
    {
        mn.Check();

        if (mn.protocolVersion < minProtocol || !mn.IsEnabled())
        {
            i++;
            continue;
        }

        // calculate the score for each masternode
        uint256 n = mn.CalculateScore(nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        // determine the winner
        if (n2 > score)
        {
            score = n2;
            winner = i;
        }

        i++;
    }

    return winner;
}

int GetMasternodeByRank(int findRank, int64_t nBlockHeight, int minProtocol)
{
    LOCK(cs_masternodes);
    int i = 0;
    std::vector<pair<unsigned int, int> > vecMasternodeScores;

    BOOST_FOREACH(CMasternode mn, vecMasternodes)
    {
        mn.Check();

        if (mn.protocolVersion < minProtocol)
            continue;

        if (!mn.IsEnabled())
        {
            i++;
            continue;
        }

        uint256 n = mn.CalculateScore(nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, i));
        i++;
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnly2());
    int rank = 0;

    BOOST_FOREACH (PAIRTYPE(unsigned int, int)& s, vecMasternodeScores)
    {
        rank++;

        if (rank == findRank)
            return s.second;
    }

    return -1;
}

int GetMasternodeRank(CTxIn& vin, int64_t nBlockHeight, int minProtocol)
{
    LOCK(cs_masternodes);
    std::vector<pair<unsigned int, CTxIn> > vecMasternodeScores;

    BOOST_FOREACH(CMasternode& mn, vecMasternodes)
    {
        mn.Check();

        if (mn.protocolVersion < minProtocol)
            continue;

        if (!mn.IsEnabled())
            continue;

        uint256 n = mn.CalculateScore(nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, mn.vin));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnly());
    unsigned int rank = 0;

    BOOST_FOREACH (PAIRTYPE(unsigned int, CTxIn)& s, vecMasternodeScores)
    {
        rank++;

        if (s.second == vin)
            return rank;
    }

    return -1;
}

//Get the last hash that matches the modulus given. Processed in reverse order
bool GetBlockHash(uint256& hash, int nBlockHeight)
{
    if (pindexBest == NULL || nBlockHeight < 0 || nBlockHeight > nBestHeight)
    {
        LogPrintf("%s : failed to get block %d\n", __func__, nBlockHeight);
        return false;
    }

    if (nBlockHeight == 0)
        nBlockHeight = pindexBest->nHeight;

    hash = FindBlockByHeight(nBlockHeight)->GetBlockHash();
    return true;
}

// Deterministically calculate a given "score" for a masternode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block

uint256 CMasternode::CalculateScore(unsigned int nBlockHeight)
{
    if (pindexBest == NULL)
    {
        LogPrintf("%s : pindexbest is null\n", __func__);
        return 0;
    }

    uint256 hash = 0;
    uint256 aux = vin.prevout.hash + vin.prevout.n;

    if(!GetBlockHash(hash, nBlockHeight - MASTERNODE_BLOCK_OFFSET))
    {
        LogPrintf("%s : failed to get blockhash\n", __func__);
        return 0;
    }
    CDataStream ss(SER_GETHASH, 0);
    ss << hash;
    uint256 hash2 = Hash(ss.begin(), ss.end());

    ss << aux;
    uint256 hash3 = Hash(ss.begin(), ss.end());

    uint256 r = (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);
    return r;
}

void CMasternode::Check()
{
    if (GetTime() - lastTimeChecked < MASTERNODE_CHECK_SECONDS)
        return;

    lastTimeChecked = GetTime();

    // once spent, stop doing the checks
    if (nActiveState==MASTERNODE_VIN_SPENT)
        return;

    // only accept p2p port for mainnet and testnet
   // if (addr.GetPort() != GetDefaultPort())
   // {
   //     nActiveState = MASTERNODE_POS_ERROR;
   //     return;
   // }

    if (!UpdatedWithin(MASTERNODE_REMOVAL_SECONDS))
    {
        nActiveState = MASTERNODE_REMOVE;
        return;
    }

    if (!UpdatedWithin(MASTERNODE_EXPIRATION_SECONDS))
    {
        nActiveState = MASTERNODE_EXPIRED;
        return;
    }

    if (!unitTest)
    {
        CTransaction tx = CTransaction();
        CTxOut vout = CTxOut(24999 * COIN, darkSendPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);

        // if(!AcceptableInputs(mempool, state, tx)){
        bool pfMissingInputs = false;

        if (!AcceptableInputs(mempool, tx, false, &pfMissingInputs))
        {
            nActiveState = MASTERNODE_VIN_SPENT;
            return;
        }
    }

    nActiveState = MASTERNODE_ENABLED; // OK
}

std::string CMasternode::StateToString(int nStateIn)
{
    switch(nStateIn)
    {
        case MASTERNODE_ENABLED:   return "ENABLED";
        case MASTERNODE_EXPIRED:   return "EXPIRED";
        case MASTERNODE_VIN_SPENT: return "VIN_SPENT";
        case MASTERNODE_REMOVE:    return "REMOVE";
        case MASTERNODE_POS_ERROR: return "POS_ERROR";
        default:                   return "UNKNOWN";
    }
}

std::string CMasternode::GetStateString() const
{
    return StateToString(nActiveState);
}

std::string CMasternode::GetStatus() const
{
    // TODO: return soemthing a bit more human readable here
    return GetStateString();
}

bool CMasternodePayments::CheckSignature(CMasternodePaymentWinner& winner)
{
    //NOTE: need to investigate why this is failing
    std::string strMessage = winner.vin.ToString().c_str() +
                             boost::lexical_cast<std::string>(winner.nBlockHeight) +
                             winner.payee.ToString();

    std::string strPubKey = !fTestNet ? strMainPubKey: strTestPubKey;
    CPubKey pubkey(ParseHex(strPubKey));

    if (fDebug)
       LogPrintf("%s : strMessage: %s, strPubKey: %s\n", __func__, strMessage, strPubKey);

    std::string errorMessage = "";

    if (!darkSendSigner.VerifyMessage(pubkey, winner.vchSig, strMessage, errorMessage))
    {
        if (fDebug)
        {
            if (!errorMessage.empty())
                LogPrintf("%s : VerifyMessage() failed, error: %s\n", __func__, errorMessage);
            else
                LogPrintf("%s : VerifyMessage() failed\n", __func__);
        }

        return false;
    }

    return true;
}

bool CMasternodePayments::Sign(CMasternodePaymentWinner& winner)
{
    std::string strMessage = winner.vin.ToString().c_str() + boost::lexical_cast<std::string>(winner.nBlockHeight) + winner.payee.ToString();

    CKey key2;
    CPubKey pubkey2;
    std::string errorMessage = "";

    if (!darkSendSigner.SetKey(strMasterPrivKey, errorMessage, key2, pubkey2))
    {
        LogPrintf("%s : [ERROR] invalid masternodeprivkey: '%s'\n", __func__, errorMessage.c_str());
        return false;
    }

    if (!darkSendSigner.SignMessage(strMessage, errorMessage, winner.vchSig, key2))
    {
        LogPrintf("%s : sign message failed\n", __func__);
        return false;
    }

    if (!darkSendSigner.VerifyMessage(pubkey2, winner.vchSig, strMessage, errorMessage))
    {
        LogPrintf("%s : verify message failed\n", __func__);
        return false;
    }

    return true;
}

bool CMasternodePayments::GetBlockPayee(int nBlockHeight, CScript& payee)
{
    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning)
    {
        if (winner.nBlockHeight == nBlockHeight)
        {
            payee = winner.payee;
            return true;
        }
    }

    return false;
}

bool CMasternodePayments::GetWinningMasternode(int nBlockHeight, CTxIn& vinOut)
{
    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning)
    {
        if(winner.nBlockHeight == nBlockHeight)
        {
            vinOut = winner.vin;
            return true;
        }
    }

    return false;
}

bool CMasternodePayments::AddWinningMasternode(CMasternodePaymentWinner& winnerIn, bool reorganize)
{
    // check to see if there is already a winner set for this block.
    // if a winner is set, compare scores and update if new winner is higher score
    bool foundBlock = false;

    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning)
    {
        if (winner.nBlockHeight == winnerIn.nBlockHeight)
        {
            foundBlock = true;

            if (reorganize || winner.score <= winnerIn.score)
            {
                LogPrintf("%s : new masternode winner %s - replacing\n", __func__,
                          reorganize ? "during reorganize" : "has an equal or higher score");

                winner.score = winnerIn.score;
                winner.vin = winnerIn.vin;
                winner.payee = winnerIn.payee;
                winner.vchSig = winnerIn.vchSig;

                return true;
            }
            else
                LogPrintf("%s : new masternode winner has a lower score - ignoring\n", __func__);
        }
    }

    if (!foundBlock)
    {
        LogPrintf("%s : adding block %d\n", __func__, winnerIn.nBlockHeight);
        vWinning.push_back(winnerIn);
        mapSeenMasternodeVotes.insert(make_pair(winnerIn.GetHash(), winnerIn));

        return true;
    }

    return false;
}

void CMasternodePayments::CleanPaymentList()
{
    LOCK(cs_masternodes);

    if (pindexBest == NULL)
        return;

    int nLimit = std::max(((int) vecMasternodes.size()) * 2, 1000);
    vector<CMasternodePaymentWinner>::iterator it;

    for (it = vWinning.begin(); it < vWinning.end(); it++)
    {
        if (pindexBest->nHeight - (*it).nBlockHeight > nLimit)
        {
            if (fDebug)
            {
                LogPrintf("%s : removing old masternode payment - block %d\n",
                          __func__, (*it).nBlockHeight);
            }

            vWinning.erase(it);
            break;
        }
    }
}

bool CMasternodePayments::ProcessBlock(int nBlockHeight, bool reorganize)
{
    CMasternodePaymentWinner winner;

    {
        LOCK(cs_masternodes);

        // scan for winner
        unsigned int score = 0;

        for (CMasternode mn : vecMasternodes)
        {
            mn.Check();

            if (!mn.IsEnabled())
                continue;

            // calculate the score for each masternode
            uint256 nScore_256 = mn.CalculateScore(nBlockHeight);
            unsigned int n2 = static_cast<unsigned int>(nScore_256.Get64());

            // determine the winner
            if (n2 > score)
            {
                score = n2;
                winner.score = n2;
                winner.nBlockHeight = nBlockHeight;
                winner.vin = mn.vin;
                winner.payee = GetScriptForDestination(mn.pubkey.GetID());
            }
        }
    }

    // if we can't find someone to get paid, pick randomly
    if (winner.nBlockHeight == 0 && vecMasternodes.size() > 0)
    {
        LogPrintf("%s : using random mn as winner\n", __func__);
        winner.score = 0;
        winner.nBlockHeight = nBlockHeight;
        unsigned int nHeightOffset = nBlockHeight;

        if (nHeightOffset > vecMasternodes.size() - 1)
            nHeightOffset = (vecMasternodes.size() - 1) % nHeightOffset;

        winner.vin = vecMasternodes[nHeightOffset].vin;
        winner.payee = GetScriptForDestination(vecMasternodes[nHeightOffset].pubkey.GetID());
    }

    CTxDestination address1;
    ExtractDestination(winner.payee, address1);
    CBitcoinAddress address2(address1);

    LogPrintf("%s : winner, payee=%s, nBlockHeight=%d\n", __func__,
              address2.ToString(), nBlockHeight);

    // LogPrintf("CMasternodePayments::ProcessBlock -- Signing Winner\n");
    // if (Sign(winner)) {
    //     LogPrintf("CMasternodePayments::ProcessBlock -- AddWinningMasternode\n");

    //     if (AddWinningMasternode(winner)) {
    //         LogPrintf("CMasternodePayments::ProcessBlock -- Relay Winner\n");
    //         Relay(winner);
    //         return true;
    //     }
    // }

    if (AddWinningMasternode(winner, reorganize))
    {
        if (enabled)
        {
            LogPrintf("%s : signing winner\n", __func__);

            if (Sign(winner))
            {
                LogPrintf("%s : relay winner\n", __func__);
                Relay(winner);
            }
        }

        return true;
    }

    return false;
}

bool CMasternodePayments::ProcessManyBlocks(int nBlockHeight)
{
    if (vecMasternodes.empty())
        return false;

    for (int i = nBlockHeight + 1; i < nBlockHeight + 10; i++)
        ProcessBlock(i);

   return true;
}

void CMasternodePayments::Relay(CMasternodePaymentWinner& winner)
{
    CInv inv(MSG_MASTERNODE_WINNER, winner.GetHash());
    vector<CInv> vInv;

    vInv.push_back(inv);
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
        pnode->PushMessage(NetMsgType::INV, vInv);
}

void CMasternodePayments::Sync(CNode* node)
{
    int a = 0;

    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning)
    {
        if (winner.nBlockHeight >= pindexBest->nHeight-10 && winner.nBlockHeight <= pindexBest->nHeight + 20)
            node->PushMessage(NetMsgType::MASTERNODEPAYMENTVOTE, winner, a);
    }
}


bool CMasternodePayments::SetPrivKey(std::string strPrivKey)
{
    CMasternodePaymentWinner winner;

    // test signing successful, proceed
    strMasterPrivKey = strPrivKey;

    Sign(winner);

    if (CheckSignature(winner))
    {
        LogPrintf("%s : successfully initialized as masternode payments master\n", __func__);
        enabled = true;

        return true;
    }
    else
        return false;
}

void CMasternodeMan::AskForMN(CNode* pnode, CTxIn& vin)
{
    std::map<COutPoint, int64_t>::iterator i = mWeAskedForMasternodeListEntry.find(vin.prevout);

    if (i != mWeAskedForMasternodeListEntry.end())
    {
        int64_t t = (*i).second;

        // we have asked recently?
        if (GetTime() < t)
            return;
    }

    // ask for the mnb info once from the node that sent mnp
    LogPrintf("%s : asking for missing masternode entry, peer: %s vin: %s\n", __func__,
              pnode->addr.ToString(), vin.prevout.ToStringShort());

    pnode->PushMessage(NetMsgType::DSEG, vin);
    int64_t askAgain = GetTime() + MASTERNODE_DSEG_SECONDS;
    mWeAskedForMasternodeListEntry[vin.prevout] = askAgain;
}

void CMasternodeMan::Check()
{
    LOCK(cs_masternodes);

    BOOST_FOREACH (CMasternode& mn, vecMasternodes)
        mn.Check();
}

void CMasternodeMan::CheckAndRemove()
{
    LogPrintf("%s : started\n", __func__);

    {
        LOCK(cs_masternodes);

        Check();
        LogPrintf("%s : remove masternodes\n", __func__);

        // remove inactive and outdated
        vector<CMasternode>::iterator it = vecMasternodes.begin();

        while (it != vecMasternodes.end())
        {
            if ((*it).nActiveState == CMasternode::MASTERNODE_REMOVE ||
                (*it).nActiveState == CMasternode::MASTERNODE_VIN_SPENT)
            {
                LogPrintf("%s : removing inactive masternode %s - %s, reason: %d\n", __func__,
                          (*it).addr.ToString().c_str(), (*it).vin.prevout.hash.ToString(), (*it).nActiveState);

                it = vecMasternodes.erase(it);
            }
            else
                ++it;
        }
    }

    // TODO: NTRN - do more checks here

    {
        // no need for cm_main below
        LOCK(cs);

        LogPrintf("%s : remove asked\n", __func__);

        // check who's asked for the Masternode list
        auto it1 = mAskedUsForMasternodeList.begin();

        while(it1 != mAskedUsForMasternodeList.end())
        {
            if ((*it1).second < GetTime())
                mAskedUsForMasternodeList.erase(it1++);
            else
                ++it1;
        }
    }

    LogPrintf("%s : finished\n", __func__);
}

void CMasternodeMan::Clear()
{
    LOCK(cs_masternodes);
    vecMasternodes.clear();
}

int CMasternodeMan::CountEnabled(int protocolVersion)
{
    int i = 0;
    protocolVersion = protocolVersion == -1 ? ActiveProtocol() : protocolVersion;

    BOOST_FOREACH (CMasternode& mn, vecMasternodes)
    {
        mn.Check();

        if (mn.protocolVersion < protocolVersion || !mn.IsEnabled())
            continue;

        i++;
    }

    return i;
}

CMasternode* CMasternodeMan::Find(const CTxIn& vin)
{
    LOCK(cs_masternodes);

    BOOST_FOREACH (CMasternode& mn, vecMasternodes)
    {
        if (mn.vin.prevout == vin.prevout)
            return &mn;
    }

    return NULL;
}
