// Copyright (c) 2009-2012 The Darkcoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_H
#define MASTERNODE_H

#include "uint256.h"
#include "uint256.h"
#include "sync.h"
#include "net.h"
#include "key.h"
//#include "primitives/transaction.h"
//#include "primitives/block.h"
#include "util.h"
//#include "script/script.h"
#include "base58.h"
#include "main.h"
#include "timedata.h"
#include "script.h"
#include "spork.h"

#include <boost/lexical_cast.hpp>
#include <map>
#include <vector>

class CMasternode;
class CMasternodePayments;
class CMasternodeMan;
class uint256;

#define MASTERNODE_NOT_PROCESSED               0 // initial state
#define MASTERNODE_IS_CAPABLE                  1
#define MASTERNODE_NOT_CAPABLE                 2
#define MASTERNODE_STOPPED                     3
#define MASTERNODE_INPUT_TOO_NEW               4
#define MASTERNODE_PORT_NOT_OPEN               6
#define MASTERNODE_PORT_OPEN                   7
#define MASTERNODE_SYNC_IN_PROCESS             8
#define MASTERNODE_REMOTELY_ENABLED            9

#define MASTERNODE_MIN_CONFIRMATIONS           15
#define MASTERNODE_MIN_DSEEP_SECONDS           (30*60)
#define MASTERNODE_MIN_DSEE_SECONDS            (5*60)
#define MASTERNODE_PING_SECONDS                (1*60)
#define MASTERNODE_EXPIRATION_SECONDS          (120*60)
#define MASTERNODE_REMOVAL_SECONDS             (130*60)
#define MASTERNODE_CHECK_SECONDS               5
#define MASTERNODE_DSEG_SECONDS                (5*60) // 5 minutes

#define MASTERNODE_BLOCK_OFFSET                50

using namespace std;

class CMasternodePaymentWinner;

extern CCriticalSection cs_masternodes;
extern std::vector<CMasternode> vecMasternodes;
extern CMasternodePayments masternodePayments;
extern CMasternodeMan mnodeman;
extern std::vector<CTxIn> vecMasternodeAskedFor;
extern map<uint256, CMasternodePaymentWinner> mapSeenMasternodeVotes;
extern map<int64_t, uint256> mapCacheBlockHashes;

// manage the masternode connections
void ProcessMasternodeConnections();
int CountMasternodesAboveProtocol(int protocolVersion);


void ProcessMessageMasternode(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);

//
// The Masternode Class. For managing the Darksend process. It contains the input of the 25000 NTRN, signature to prove
// it's the one who own that ip address and code for calculating the payment election.
//
class CMasternode
{
private:
    int64_t lastTimeChecked;

public:
    enum state {
        MASTERNODE_ENABLED = 1,
        MASTERNODE_EXPIRED = 2,
        MASTERNODE_VIN_SPENT = 3,
        MASTERNODE_REMOVE = 4,
        MASTERNODE_POS_ERROR = 5
    };

    CTxIn vin;
    CService addr;
    int64_t lastTimeSeen;
    CPubKey pubkey;
    CPubKey pubkey2;
    std::vector<unsigned char> sig;
    int64_t now; //dsee message times
    int64_t lastDseep;
    int cacheInputAge;
    int cacheInputAgeBlock;
    int nActiveState;
    bool unitTest;
    bool allowFreeTx;
    int protocolVersion;

    int64_t nLastDsq; //the dsq count from the last dsq broadcast of this node

    CMasternode(CService newAddr, CTxIn newVin, CPubKey newPubkey, std::vector<unsigned char> newSig, int64_t newNow, CPubKey newPubkey2, int protocolVersionIn)
    {
        addr = newAddr;
        vin = newVin;
        pubkey = newPubkey;
        pubkey2 = newPubkey2;
        sig = newSig;
        now = newNow;
        nActiveState = MASTERNODE_ENABLED;
        lastTimeSeen = 0;
        unitTest = false;
        cacheInputAge = 0;
        cacheInputAgeBlock = 0;
        nLastDsq = 0;
        lastDseep = 0;
        allowFreeTx = true;
        protocolVersion = protocolVersionIn;
        lastTimeChecked = 0;
    }

    uint256 CalculateScore(unsigned int nBlockHeight);

    void UpdateLastSeen(int64_t override=0)
    {
        if(override == 0){
            lastTimeSeen = GetAdjustedTime();
            if(fDebug) LogPrintf("UpdateLastSeen - addr=%s lastTimeSeen=%s using GetAdjustedTime\n", addr.ToString(), lastTimeSeen);
        } else {
            lastTimeSeen = override;
            if(fDebug) LogPrintf("UpdateLastSeen - addr=%s lastTimeSeen=%s using override\n", addr.ToString(), lastTimeSeen);
        }
    }

    inline uint64_t SliceHash(uint256& hash, int slice)
    {
        uint64_t n = 0;
        memcpy(&n, &hash+slice*64, 64);
        return n;
    }

    void Check();

    bool UpdatedWithin(int seconds)
    {
        // LogPrintf("UpdatedWithin %d, %d --  %d \n", GetAdjustedTime() , lastTimeSeen, (GetAdjustedTime() - lastTimeSeen) < seconds);

        return (GetAdjustedTime() - lastTimeSeen) < seconds;
    }

    void Disable()
    {
        lastTimeSeen = 0;
    }

    bool IsEnabled() { return nActiveState == MASTERNODE_ENABLED; }

    int GetMasternodeInputAge()
    {
        if(pindexBest == NULL) return 0;

        if(cacheInputAge == 0){
            cacheInputAge = GetInputAge(vin);
            cacheInputAgeBlock = pindexBest->nHeight;
        }

        return cacheInputAge+(pindexBest->nHeight-cacheInputAgeBlock);
    }

    static std::string StateToString(int nStateIn);
    std::string GetStateString() const;
    std::string GetStatus() const;
};

// Get the current winner for this block
int GetCurrentMasterNode(int mod=1, int64_t nBlockHeight=0, int minProtocol=0);

int GetMasternodeByVin(CTxIn& vin);
int GetMasternodeRank(CTxIn& vin, int64_t nBlockHeight=0, int minProtocol=0);
int GetMasternodeByRank(int findRank, int64_t nBlockHeight=0, int minProtocol=0);

// for storing the winning payments
class CMasternodePaymentWinner
{
public:
    int nBlockHeight;
    CTxIn vin;
    CScript payee;
    std::vector<unsigned char> vchSig;
    uint64_t score;

    CMasternodePaymentWinner() {
        nBlockHeight = 0;
        score = 0;
        vin = CTxIn();
        payee = CScript();
    }

    CMasternodePaymentWinner(CTxIn vinIn)
    {
        nBlockHeight = 0;
        score = 0;
        vin = vinIn;
        payee = CScript();
    }

    uint256 GetHash(){
        uint256 n2 = Hash(BEGIN(nBlockHeight), END(nBlockHeight));
        uint256 n3 = vin.prevout.hash > n2 ? (vin.prevout.hash - n2) : (n2 - vin.prevout.hash);

        return n3;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion){
        unsigned int nSerSize = 0;
        READWRITE(nBlockHeight);
        READWRITE(payee);
        READWRITE(vin);
        READWRITE(score);
        READWRITE(vchSig);
     }

    std::string ToString()
    {
        std::string ret = "";
        ret += vin.ToString();
        ret += ", " + boost::lexical_cast<std::string>(nBlockHeight);
        ret += ", " + payee.ToString();
        ret += ", " + boost::lexical_cast<std::string>((int)vchSig.size());
        return ret;
    }

};

class CMasternodePayments
{
private:
    std::map<int, CMasternodePaymentWinner> vWinning;
    int nSyncedFromPeer;
    std::string strMasterPrivKey;
    std::string strTestPubKey;
    std::string strMainPubKey;
    bool enabled;

public:
    CMasternodePayments() {
      if (sporkManager.IsSporkActive(SPORK_9_PROTOCOL_V3_ENFORCEMENT)){
        strMainPubKey = "0452218a26fde81130c8b4930c897c19d21c4bab6ad03f17f522376500b0b86ce547e3975fbe886bea7583a3b05c6f1bb4f303f141aa282da1cf35e9cb71bbf279";
        strTestPubKey = "0452218a26fde81130c8b4930c897c19d21c4bab6ad03f17f522376500b0b86ce547e3975fbe886bea7583a3b05c6f1bb4f303f141aa282da1cf35e9cb71bbf279";
        } else {
        strMainPubKey = "0435c38ffb14441df9894dca8741921d67a8130ff3c2fb81e2b0503b31722bae8f1a450865036c63044e0aa4708b205c575c7ddc18c73bd36641e20eceef8d095d";
        strTestPubKey = "0435c38ffb14441df9894dca8741921d67a8130ff3c2fb81e2b0503b31722bae8f1a450865036c63044e0aa4708b205c575c7ddc18c73bd36641e20eceef8d095d";
      }

        enabled = false;
    }

    bool SetPrivKey(std::string strPrivKey);
    bool CheckSignature(CMasternodePaymentWinner& winner);
    bool Sign(CMasternodePaymentWinner& winner);

    // Deterministically calculate a given "score" for a masternode depending on how close it's hash is
    // to the blockHeight. The further away they are the better, the furthest will win the election
    // and get paid this block
    //

    uint64_t CalculateScore(uint256 blockHash, CTxIn& vin);
    bool GetWinningMasternode(int nBlockHeight, CTxIn& vinOut);
    bool AddPastWinningMasternode(std::vector<CTransaction>& vtx, int64_t amount, int height);
    bool AddWinningMasternode(CMasternodePaymentWinner& winner, bool reorganize=false);
    bool ProcessBlock(int nBlockHeight, bool reorganize=false);
    bool ProcessManyBlocks(int nBlockHeight);
    void Relay(CMasternodePaymentWinner& winner);
    void Sync(CNode* node);
    void CleanPaymentList();
    int LastPayment(CMasternode& mn);

    //slow
    bool GetBlockPayee(int nBlockHeight, CScript& payee);
};


class CMasternodeMan
{
private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

public:
    /// Ask (source) node for mnb
    void AskForMN(CNode* pnode, CTxIn& vin);

    /// Check all Masternodes
    void Check();

    /// Check all Masternodes and remove inactive
    void CheckAndRemove();

    /// Clear Masternode vector
    void Clear();

    int CountEnabled(int protocolVersion = -1);

    /// Find an entry
    CMasternode* Find(const CTxIn& vin);

    /// Return the number of (unique) Masternodes
    int size() { return vecMasternodes.size(); }
};

#endif
