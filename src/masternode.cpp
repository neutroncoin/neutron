#include "masternode.h"
#include "activemasternode.h"
#include "darksend.h"
//#include "primitives/transaction.h"
#include "main.h"
#include "util.h"
#include "addrman.h"
#include <boost/lexical_cast.hpp>


CCriticalSection cs_masternodes;

/** Masternode manager */
CMasternodeMan mnodeman;

/** The list of active masternodes */
std::vector<CMasterNode> vecMasternodes;
/** Object for who's going to get paid on which blocks */
CMasternodePayments masternodePayments;
// keep track of masternode votes I've seen
map<uint256, CMasternodePaymentWinner> mapSeenMasternodeVotes;
// keep track of the scanning errors I've seen
map<uint256, int> mapSeenMasternodeScanningErrors;
// who's asked for the masternode list and the last time
std::map<CNetAddr, int64_t> askedForMasternodeList;
// which masternodes we've asked for
std::map<COutPoint, int64_t> mWeAskedForMasternodeListEntry;
// cache block hashes as we calculate them
std::map<int64_t, uint256> mapCacheBlockHashes;

// manage the masternode connections
void ProcessMasternodeConnections(){
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        //if it's our masternode, let it be
        if(darkSendPool.submittedToMasternode == pnode->addr) continue;

        if(pnode->fDarkSendMaster){
            LogPrintf("Closing masternode connection %s \n", pnode->addr.ToString().c_str());
            pnode->CloseSocketDisconnect();
        }
    }
}

void ProcessMessageMasternode(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (fLiteMode and strCommand != NetMsgType::MASTERNODEPAYMENTVOTE) return; //disable all Darksend/Masternode related functionality
    if(IsInitialBlockDownload()) return;

    if (strCommand == NetMsgType::DSEE) { //DarkSend Election Entry
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
        vRecv >> vin >> addr >> vchSig >> sigTime >> pubkey >> pubkey2 >> count >> current >> lastUpdated >> protocolVersion;

        if(fDebug) LogPrintf("dsee - Received: node: %s, vin: %s, addr: %s, sigTime: %lld, pubkey: %s, pubkey2: %s, count: %d, current: %d, lastUpdated: %lld, protocol: %d\n", pfrom->addr.ToString().c_str(), vin.ToString().c_str(), addr.ToString(), sigTime, pubkey.GetHash().ToString(), pubkey2.GetHash().ToString(), count, current, lastUpdated, protocolVersion);

        // make sure signature isn't in the future (past is OK)
        if (sigTime > GetAdjustedTime() + 60 * 60) {
            LogPrintf("dsee - Signature rejected, too far into the future %s\n", vin.prevout.hash.ToString()); // vin.ToString().c_str());
            pfrom->Misbehaving(1);
            return;
        }

        bool isLocal = addr.IsRFC1918() || addr.IsLocal();
        //if(Params().MineBlocksOnDemand()) isLocal = false;

        std::string vchPubKey(pubkey.vchPubKey.begin(), pubkey.vchPubKey.end());
        std::string vchPubKey2(pubkey2.vchPubKey.begin(), pubkey2.vchPubKey.end());

        strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) + vchPubKey + vchPubKey2 + boost::lexical_cast<std::string>(protocolVersion);

        if(protocolVersion < ActiveProtocol()) {
            LogPrintf("dsee - ignoring masternode %s using outdated protocol version %d\n", vin.ToString().c_str(), protocolVersion);
            pfrom->Misbehaving(15);
            return;
        }

        CScript pubkeyScript;
        pubkeyScript = GetScriptForDestination(pubkey.GetID());

        if(pubkeyScript.size() != 25) {
            LogPrintf("dsee - pubkey the wrong size\n");
            pfrom->Misbehaving(100);
            return;
        }

        CScript pubkeyScript2;
        pubkeyScript2 =GetScriptForDestination(pubkey2.GetID());

        if(pubkeyScript2.size() != 25) {
            LogPrintf("dsee - pubkey2 the wrong size\n");
            pfrom->Misbehaving(100);
            return;
        }

        std::string errorMessage = "";
        if(!darkSendSigner.VerifyMessage(pubkey, vchSig, strMessage, errorMessage)){
            LogPrintf("dsee - Got bad masternode address signature\n");
            pfrom->Misbehaving(100);
            return;
        }

        //search existing masternode list, this is where we update existing masternodes with new dsee broadcasts
        CMasterNode* pmn = mnodeman.Find(vin);
        if (pmn != NULL) {
            LogPrintf("dsee - Found existing masternode %s - %s - %s\n", pmn->addr.ToString().c_str(), vin.ToString().c_str(), pmn->UpdatedWithin(MASTERNODE_MIN_DSEE_SECONDS));

            // count == -1 when it's a new entry
            //   e.g. We don't want the entry relayed/time updated when we're syncing the list
            // mn.pubkey = pubkey, IsVinAssociatedWithPubkey is validated once below,
            //   after that they just need to match
            if(count == -1 && pmn->pubkey == pubkey && !pmn->UpdatedWithin(MASTERNODE_MIN_DSEE_SECONDS)){
                LogPrintf("dsee - Update masternode last seen for %s\n", addr.ToString().c_str());

                pmn->UpdateLastSeen();

                if(pmn->now < sigTime){ //take the newest entry
                    LogPrintf("dsee - Got updated entry for %s\n", addr.ToString().c_str());
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
        if(!darkSendSigner.IsVinAssociatedWithPubkey(vin, pubkey)) {
            LogPrintf("dsee - Got mismatched pubkey and vin\n");
            pfrom->Misbehaving(100);
            return;
        }

        LogPrintf("dsee - Got NEW masternode entry %s\n", addr.ToString().c_str());


        // make sure it's still unspent
        //  - this is checked later by .check() in many places and by ThreadCheckDarkSendPool()


        CTransaction tx = CTransaction();
        CTxOut vout = CTxOut(24999*COIN, darkSendPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);

        bool pfMissingInputs = false;
        if(AcceptableInputs(mempool, tx, false, &pfMissingInputs)){
            LogPrintf("dsee - Accepted masternode entry %i %i\n", count, current);

            if(GetInputAge(vin) < MASTERNODE_MIN_CONFIRMATIONS){
                LogPrintf("dsee - Input must have least %d confirmations\n", MASTERNODE_MIN_CONFIRMATIONS);
                pfrom->Misbehaving(20);
                return;
            }

            // use this as a peer
            addrman.Add(CAddress(addr), pfrom->addr, 2*60*60);

            // add our masternode
            CMasterNode mn(addr, vin, pubkey, vchSig, sigTime, pubkey2, protocolVersion);
            mn.UpdateLastSeen(lastUpdated);
            vecMasternodes.push_back(mn);

            // if it matches our masternodeprivkey, then we've been remotely activated
            if(pubkey2 == activeMasternode.pubKeyMasternode && protocolVersion == PROTOCOL_VERSION){
                activeMasternode.EnableHotColdMasterNode(vin, addr);
            }

            if(count == -1 && !isLocal)
                RelayDarkSendElectionEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);

        } else {
            LogPrintf("dsee - Rejected masternode entry %s\n", addr.ToString().c_str());

            int nDoS = 0;
           /* if (state.IsInvalid(nDoS))
            {
                LogPrintf("dsee - %s from %s %s was not accepted into the memory pool\n", tx.GetHash().ToString().c_str(),
                    pfrom->addr.ToString().c_str(), pfrom->cleanSubVer.c_str());
                if (nDoS > 0)
                    pfrom->Misbehaving(nDoS);
            }*/
        }
    }

    else if (strCommand == NetMsgType::DSEEP) { //DarkSend Election Entry Ping
        CTxIn vin;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        bool stop;

        vRecv >> vin >> vchSig >> sigTime >> stop;

        if(fDebug) LogPrintf("dseep - Received: node: %s, vin: %s, sigTime: %lld, stop: %s\n", pfrom->addr.ToString().c_str(), vin.ToString().c_str(), sigTime, stop ? "true" : "false");

        if (sigTime > GetAdjustedTime() + 60 * 60) {
            LogPrintf("dseep - Signature rejected, too far into the future %s\n", vin.ToString().c_str());
            // pfrom->Misbehaving(1);
            return;
        }

        if (sigTime <= GetAdjustedTime() - 60 * 60) {
            LogPrintf("dseep - Signature rejected, too far into the past %s - %s - %ld less than %ld\n", pfrom->addr.ToString().c_str(), vin.ToString().c_str(), sigTime, GetAdjustedTime() - 60 * 60);
            // pfrom->Misbehaving(1);
            return;
        }

        // see if we have this masternode
        CMasterNode* pmn = mnodeman.Find(vin);
        if (pmn != NULL) {
            if(fDebug) LogPrintf("dseep - Found corresponding mn for vin=%s addr=%s\n", vin.ToString().c_str(), pmn->addr.ToString());

            // take this only if it's newer
            if(sigTime - pmn->lastDseep > MASTERNODE_MIN_DSEEP_SECONDS) {
                std::string strMessage = pmn->addr.ToString() + boost::lexical_cast<std::string>(sigTime) + boost::lexical_cast<std::string>(stop);

                if(fDebug) LogPrintf("dseep - Got newer sigTime - sigTime=%d lastDseep=%d\n", sigTime, pmn->lastDseep);

                std::string errorMessage = "";
                if(!darkSendSigner.VerifyMessage(pmn->pubkey2, vchSig, strMessage, errorMessage)){
                    LogPrintf("dseep - Got bad masternode address signature %s \n", vin.ToString().c_str());
                    pfrom->Misbehaving(33);
                    return;
                }

                pmn->lastDseep = sigTime;
                pmn->Check();

                if(pmn->IsEnabled()) {
                    if(fDebug) LogPrintf("dseep - Masternode is enabled addr=%s\n", pmn->addr.ToString());

                    if(stop) {
                        pmn->Disable();
                    } else {
                        if(fDebug) LogPrintf("dseep - UpdatingLastSeen addr=%s\n", pmn->addr.ToString());
                        pmn->UpdateLastSeen();
                    }
                    TRY_LOCK(cs_vNodes, lockNodes);
                    if (!lockNodes) return;
                    if(fDebug) LogPrintf("dseep - relaying %s - %s \n", pmn->addr.ToString(), vin.prevout.hash.ToString());
                    RelayDarkSendElectionEntryPing(vin, vchSig, sigTime, stop);
                }
            }

            return;
        }

        if(fDebug) LogPrintf("dseep - Couldn't find masternode entry %s\n", vin.ToString().c_str());

        mnodeman.AskForMN(pfrom, vin);

    } else if (strCommand == NetMsgType::DSEG) { //Get masternode list or specific entry
        CTxIn vin;
        vRecv >> vin;

        if(fDebug) LogPrintf("dseg - Masternode list, vin=%s, prevout=%s\n", vin.ToString().c_str(), vin.prevout.hash.ToString());

        if(vin == CTxIn()) { //only should ask for this once
            //local network
            //Note tor peers show up as local proxied addrs //if(!pfrom->addr.IsRFC1918())//&& !Params().MineBlocksOnDemand())
            //{
                std::map<CNetAddr, int64_t>::iterator i = askedForMasternodeList.find(pfrom->addr);
                if (i != askedForMasternodeList.end())
                {
                    int64_t t = (*i).second;
                    if (GetTime() < t) {
                        // pfrom->Misbehaving(34);
                        // LogPrintf("dseg - peer already asked me for the list, peer=%d\n", pfrom->id);
                        // return;
                    }
                }

                int64_t askAgain = GetTime() + MASTERNODE_DSEG_SECONDS;
                askedForMasternodeList[pfrom->addr] = askAgain;
            //}
        } //else, asking for a specific node which is ok

        LOCK(cs_masternodes);
        int count = vecMasternodes.size();
        int i = 0;

        BOOST_FOREACH(CMasterNode mn, vecMasternodes) {

            if(mn.addr.IsRFC1918()) continue; //local network

            if(vin == CTxIn()){
                mn.Check();
                if(mn.IsEnabled()) {
                    if(fDebug) LogPrintf("dseg - Sending masternode entry - %s \n", mn.addr.ToString().c_str());
                    pfrom->PushMessage(NetMsgType::DSEE, mn.vin, mn.addr, mn.sig, mn.now, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen, mn.protocolVersion);
                }
            } else if (vin == mn.vin) {
                if(fDebug) LogPrintf("dseg - Sending masternode entry - %s \n", mn.addr.ToString().c_str());
                pfrom->PushMessage(NetMsgType::DSEE, mn.vin, mn.addr, mn.sig, mn.now, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen, mn.protocolVersion);
                LogPrintf("dseg - Sent 1 masternode entries to peer %s (%s)\n", pfrom->GetId(), pfrom->addr.ToString().c_str());
                return;
            }
            i++;
        }

        LogPrintf("dseg - Sent %d masternode entries to %s\n", count, pfrom->addr.ToString().c_str());
    }

    else if (strCommand == NetMsgType::MASTERNODEPAYMENTSYNC) { //Masternode Payments Request Sync
        if(pfrom->HasFulfilledRequest(NetMsgType::MASTERNODEPAYMENTSYNC)) {
            // Asking for the payments list multiple times in a short period of time is no good
            LogPrintf("mnget - peer already asked me for the list, peer=%d\n", pfrom->id);
            // TODO: maybe enable this later -- Misbehaving(pfrom->GetId(), 20);
            return;
        }

        pfrom->FulfilledRequest(NetMsgType::MASTERNODEPAYMENTSYNC);
        masternodePayments.Sync(pfrom);
        LogPrintf("mnget - Sent masternode winners to peer %s\n", pfrom->addr.ToString().c_str());
    }

    else if (strCommand == NetMsgType::MASTERNODEPAYMENTVOTE) { //Masternode Payments Declare Winner
        //this is required in litemode
        CMasternodePaymentWinner winner;
        int a = 0;
        vRecv >> winner >> a;

        if(pindexBest == NULL) return;

        CTxDestination address1;
        ExtractDestination(winner.payee, address1);
        CBitcoinAddress address2(address1);

        uint256 nHash = winner.GetHash();

        if(mapSeenMasternodeVotes.count(nHash)) {
            if(fDebug) LogPrintf("mnw - seen vote %s address=%s nBlockHeight=%d nHeight=%d\n", nHash.ToString(), address2.ToString(), winner.nBlockHeight, pindexBest->nHeight);
            return;
        }

        if(winner.nBlockHeight < pindexBest->nHeight - 10 || winner.nBlockHeight > pindexBest->nHeight+20){
            LogPrintf("mnw - winner out of range %s address=%s nBlockHeight=%d nHeight=%d\n", winner.vin.ToString(), address2.ToString(), winner.nBlockHeight, pindexBest->nHeight);
            return;
        }

        if(winner.vin.nSequence != std::numeric_limits<unsigned int>::max()){
            LogPrintf("mnw - invalid nSequence\n");
            pfrom->Misbehaving(100);
            return;
        }

        int nDos = 0;
        if(!masternodePayments.CheckSignature(winner)){
            if(fDebug) LogPrintf("mnw - debug - address=%s nBlockHeight=%d nHeight=%d, prevout=%s\n", address2.ToString(), winner.nBlockHeight, pindexBest->nHeight, winner.vin.prevout.ToStringShort());

            if(nDos) {
                LogPrintf("mnw - ERROR: invalid signature\n");
                pfrom->Misbehaving(nDos);
                // pfrom->Misbehaving(20);
            } else {
                // only warn about anything non-critical (i.e. nDos == 0) in debug mode
                if(fDebug) LogPrintf("mnw - WARNING: invalid signature\n");
            }

            // Either our info or vote info could be outdated.
            // In case our info is outdated, ask for an update,
            mnodeman.AskForMN(pfrom, winner.vin);
            // but there is nothing we can do if vote info itself is outdated
            // (i.e. it was signed by a mn which changed its key),
            // so just quit here.
            return;
        }

        LogPrintf("mnw - new vote - address=%s, nBlockHeight=%d, nHeight=%d, prevout=%s, nHash=%s\n",
                    address2.ToString(), winner.nBlockHeight, pindexBest->nHeight, winner.vin.prevout.ToStringShort(), nHash.ToString());

        mapSeenMasternodeVotes.insert(make_pair(nHash, winner));

        if(masternodePayments.AddWinningMasternode(winner)){
            masternodePayments.Relay(winner);
        }
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
    BOOST_FOREACH(CMasterNode& mn, vecMasternodes) {
        if(mn.protocolVersion < protocolVersion) continue;
        i++;
    }

    return i;

}


int GetMasternodeByVin(CTxIn& vin)
{
    int i = 0;
    LOCK(cs_masternodes);
    BOOST_FOREACH(CMasterNode& mn, vecMasternodes) {
        if (mn.vin == vin) return i;
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
    BOOST_FOREACH(CMasterNode mn, vecMasternodes) {
        mn.Check();
        if(mn.protocolVersion < minProtocol || !mn.IsEnabled()) {
            i++;
            continue;
        }

        // calculate the score for each masternode
        uint256 n = mn.CalculateScore(nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        // determine the winner
        if(n2 > score){
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

    i = 0;
    BOOST_FOREACH(CMasterNode mn, vecMasternodes) {
        mn.Check();
        if(mn.protocolVersion < minProtocol) continue;
        if(!mn.IsEnabled()) {
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
    BOOST_FOREACH (PAIRTYPE(unsigned int, int)& s, vecMasternodeScores){
        rank++;
        if(rank == findRank) return s.second;
    }

    return -1;
}

int GetMasternodeRank(CTxIn& vin, int64_t nBlockHeight, int minProtocol)
{
    LOCK(cs_masternodes);
    std::vector<pair<unsigned int, CTxIn> > vecMasternodeScores;

    BOOST_FOREACH(CMasterNode& mn, vecMasternodes) {
        mn.Check();

        if(mn.protocolVersion < minProtocol) continue;
        if(!mn.IsEnabled()) {
            continue;
        }

        uint256 n = mn.CalculateScore(nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, mn.vin));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnly());

    unsigned int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, CTxIn)& s, vecMasternodeScores){
        rank++;
        if(s.second == vin) {
            return rank;
        }
    }

    return -1;
}

//Get the last hash that matches the modulus given. Processed in reverse order
bool GetBlockHash(uint256& hash, int nBlockHeight)
{
    if (pindexBest == NULL || nBlockHeight < 0 || nBlockHeight > nBestHeight) {
        LogPrintf("%s : failed to get block %d\n", __func__, nBlockHeight);
        return false;
    }

    if(nBlockHeight == 0)
        nBlockHeight = pindexBest->nHeight;

    hash = FindBlockByHeight(nBlockHeight)->GetBlockHash();

    return true;
}

//
// Deterministically calculate a given "score" for a masternode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
uint256 CMasterNode::CalculateScore(unsigned int nBlockHeight)
{
    if(pindexBest == NULL) {
        LogPrintf("%s : pindexbest is null\n", __func__);
        return 0;
    }

    uint256 hash = 0;
    uint256 aux = vin.prevout.hash + vin.prevout.n;

    if(!GetBlockHash(hash, nBlockHeight - MASTERNODE_BLOCK_OFFSET)) {
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

void CMasterNode::Check()
{
    if(GetTime() - lastTimeChecked < MASTERNODE_CHECK_SECONDS) return;
    lastTimeChecked = GetTime();


    //once spent, stop doing the checks
    if(enabled==MASTERNODE_VIN_SPENT) return;

    //Only accept p2p port for mainnet and testnet
    if (addr.GetPort() != GetDefaultPort()) {
        enabled = MASTERNODE_POS_ERROR;
        return;
    }


    if(!UpdatedWithin(MASTERNODE_REMOVAL_SECONDS)){
        enabled = MASTERNODE_REMOVE;
        return;
    }

    if(!UpdatedWithin(MASTERNODE_EXPIRATION_SECONDS)){
        enabled = MASTERNODE_EXPIRED;
        return;
    }

    if(!unitTest){
        CTransaction tx = CTransaction();
        CTxOut vout = CTxOut(24999*COIN, darkSendPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);

        //if(!AcceptableInputs(mempool, state, tx)){
        bool pfMissingInputs = false;
        if(!AcceptableInputs(mempool, tx, false, &pfMissingInputs)){
            enabled = MASTERNODE_VIN_SPENT;
            return;
        }
    }

    enabled = MASTERNODE_ENABLED; // OK
}

bool CMasternodePayments::CheckSignature(CMasternodePaymentWinner& winner)
{
    //note: need to investigate why this is failing
    std::string strMessage = winner.vin.ToString().c_str() + boost::lexical_cast<std::string>(winner.nBlockHeight) + winner.payee.ToString();
    std::string strPubKey = !fTestNet ? strMainPubKey: strTestPubKey;
    CPubKey pubkey(ParseHex(strPubKey));

    std::string errorMessage = "";
    if(!darkSendSigner.VerifyMessage(pubkey, winner.vchSig, strMessage, errorMessage)){
        if(!errorMessage.empty()) {
            LogPrintf("CMasternodePayments::CheckSignature -- VerifyMessage() failed, error: %s\n", errorMessage);
        } else {
            LogPrintf("CMasternodePayments::CheckSignature -- VerifyMessage() failed\n");
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

    if(!darkSendSigner.SetKey(strMasterPrivKey, errorMessage, key2, pubkey2))
    {
        LogPrintf("CMasternodePayments::Sign - ERROR: Invalid masternodeprivkey: '%s'\n", errorMessage.c_str());
        return false;
    }

    if(!darkSendSigner.SignMessage(strMessage, errorMessage, winner.vchSig, key2)) {
        LogPrintf("CMasternodePayments::Sign - Sign message failed");
        return false;
    }

    if(!darkSendSigner.VerifyMessage(pubkey2, winner.vchSig, strMessage, errorMessage)) {
        LogPrintf("CMasternodePayments::Sign - Verify message failed");
        return false;
    }

    return true;
}

bool CMasternodePayments::GetBlockPayee(int nBlockHeight, CScript& payee)
{
    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning){
        if(winner.nBlockHeight == nBlockHeight) {
            payee = winner.payee;
            return true;
        }
    }

    return false;
}

bool CMasternodePayments::GetWinningMasternode(int nBlockHeight, CTxIn& vinOut)
{
    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning){
        if(winner.nBlockHeight == nBlockHeight) {
            vinOut = winner.vin;
            return true;
        }
    }

    return false;
}

bool CMasternodePayments::AddWinningMasternode(CMasternodePaymentWinner& winnerIn)
{
    //check to see if there is already a winner set for this block.
    //if a winner is set, compare scores and update if new winner is higher score
    bool foundBlock = false;
    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning){
        if(winner.nBlockHeight == winnerIn.nBlockHeight) {
            foundBlock = true;
            if(winner.score < winnerIn.score){
                winner.score = winnerIn.score;
                winner.vin = winnerIn.vin;
                winner.payee = winnerIn.payee;
                winner.vchSig = winnerIn.vchSig;

                return true;
            }
        }
    }

    // if it's not in the vector
    if(!foundBlock){
        LogPrintf("CMasternodePayments::AddWinningMasternode() Adding block %d\n", winnerIn.nBlockHeight);
        vWinning.push_back(winnerIn);
        mapSeenMasternodeVotes.insert(make_pair(winnerIn.GetHash(), winnerIn));

        return true;
    }

    return false;
}

void CMasternodePayments::CleanPaymentList()
{
    LOCK(cs_masternodes);
    if(pindexBest == NULL) return;

    int nLimit = std::max(((int)vecMasternodes.size())*2, 1000);

    vector<CMasternodePaymentWinner>::iterator it;
    for(it=vWinning.begin();it<vWinning.end();it++){
        if(pindexBest->nHeight - (*it).nBlockHeight > nLimit){
            if(fDebug) LogPrintf("CMasternodePayments::CleanPaymentList - Removing old masternode payment - block %d\n", (*it).nBlockHeight);
            vWinning.erase(it);
            break;
        }
    }
}

bool CMasternodePayments::ProcessBlock(int nBlockHeight)
{
    CMasternodePaymentWinner winner;
    {
        LOCK(cs_masternodes);
        // scan for winner
        unsigned int score = 0;
        for(CMasterNode mn : vecMasternodes) {
            mn.Check();

            if(!mn.IsEnabled()) {
                continue;
            }

            // calculate the score for each masternode
            uint256 nScore_256 = mn.CalculateScore(nBlockHeight);
            unsigned int n2 = static_cast<unsigned int>(nScore_256.Get64());

            // determine the winner
            if(n2 > score) {
                score = n2;
                winner.score = n2;
                winner.nBlockHeight = nBlockHeight;
                winner.vin = mn.vin;
                winner.payee = GetScriptForDestination(mn.pubkey.GetID());
            }
        }
    }

    //if we can't find someone to get paid, pick randomly
    if(winner.nBlockHeight == 0 && vecMasternodes.size() > 0) {
        LogPrintf("CMasternodePayments::ProcessBlock -- Using random mn as winner\n");
        winner.score = 0;
        winner.nBlockHeight = nBlockHeight;

        int nHeightOffset = nBlockHeight;
        if (nHeightOffset > vecMasternodes.size() - 1)
            nHeightOffset = (vecMasternodes.size() - 1) % nHeightOffset;
        winner.vin = vecMasternodes[nHeightOffset].vin;
        winner.payee = GetScriptForDestination(vecMasternodes[nHeightOffset].pubkey.GetID());
    }

    CTxDestination address1;
    ExtractDestination(winner.payee, address1);
    CBitcoinAddress address2(address1);

    LogPrintf("CMasternodePayments::ProcessBlock -- Winner: payee=%s, nBlockHeight=%d\n", address2.ToString(), nBlockHeight);

    // LogPrintf("CMasternodePayments::ProcessBlock -- Signing Winner\n");
    // if (Sign(winner)) {
    //     LogPrintf("CMasternodePayments::ProcessBlock -- AddWinningMasternode\n");

    //     if (AddWinningMasternode(winner)) {
    //         LogPrintf("CMasternodePayments::ProcessBlock -- Relay Winner\n");
    //         Relay(winner);
    //         return true;
    //     }
    // }

    LogPrintf("CMasternodePayments::ProcessBlock -- AddWinningMasternode\n");
    if (AddWinningMasternode(winner)) {
        if (enabled) { // only if active masternode
            LogPrintf("CMasternodePayments::ProcessBlock -- Signing Winner\n");
            if (Sign(winner))
                LogPrintf("CMasternodePayments::ProcessBlock -- Relay Winner\n");
                Relay(winner);
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
    BOOST_FOREACH(CNode* pnode, vNodes){
        pnode->PushMessage(NetMsgType::INV, vInv);
    }
}

void CMasternodePayments::Sync(CNode* node)
{
    int a = 0;
    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning)
        if(winner.nBlockHeight >= pindexBest->nHeight-10 && winner.nBlockHeight <= pindexBest->nHeight + 20)
            node->PushMessage(NetMsgType::MASTERNODEPAYMENTVOTE, winner, a);
}


bool CMasternodePayments::SetPrivKey(std::string strPrivKey)
{
    CMasternodePaymentWinner winner;

    // Test signing successful, proceed
    strMasterPrivKey = strPrivKey;

    Sign(winner);

    if(CheckSignature(winner)){
        LogPrintf("CMasternodePayments::SetPrivKey - Successfully initialized as masternode payments master\n");
        enabled = true;
        return true;
    } else {
        return false;
    }
}


void CMasternodeMan::AskForMN(CNode* pnode, CTxIn& vin)
{
    std::map<COutPoint, int64_t>::iterator i = mWeAskedForMasternodeListEntry.find(vin.prevout);
    if (i != mWeAskedForMasternodeListEntry.end()){
        int64_t t = (*i).second;
        if (GetTime() < t) {
            // we've asked recently
            return;
        }
    }

    // ask for the mnb info once from the node that sent mnp
    LogPrintf("CMasternodeMan::AskForMN - Asking for missing masternode entry, peer: %s vin: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
    pnode->PushMessage(NetMsgType::DSEG, vin);
    int64_t askAgain = GetTime() + MASTERNODE_DSEG_SECONDS;
    mWeAskedForMasternodeListEntry[vin.prevout] = askAgain;
}

void CMasternodeMan::Check()
{
    LOCK(cs_masternodes);

    BOOST_FOREACH (CMasterNode& mn, vecMasternodes) {
        mn.Check();
    }
}

void CMasternodeMan::CheckAndRemove()
{
    Check();

    LOCK(cs_masternodes);

    //remove inactive and outdated
    vector<CMasterNode>::iterator it = vecMasternodes.begin();
    while (it != vecMasternodes.end()) {
        if((*it).enabled == CMasterNode::MASTERNODE_REMOVE || (*it).enabled == CMasterNode::MASTERNODE_VIN_SPENT){
            LogPrintf("CMasternodeMan::CheckAndRemove - Removing inactive masternode %s - %s -- reason: %d\n", (*it).addr.ToString().c_str(), (*it).vin.prevout.hash.ToString(), (*it).enabled);
            it = vecMasternodes.erase(it);
        } else {
            ++it;
        }
    }

    // TODO: NTRN - do more checks here
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

    BOOST_FOREACH (CMasterNode& mn, vecMasternodes) {
        mn.Check();
        if (mn.protocolVersion < protocolVersion || !mn.IsEnabled()) continue;
        i++;
    }

    return i;
}

CMasterNode* CMasternodeMan::Find(const CTxIn& vin)
{
    LOCK(cs_masternodes);

    BOOST_FOREACH (CMasterNode& mn, vecMasternodes) {
        if (mn.vin.prevout == vin.prevout)
            return &mn;
    }
    return NULL;
}

