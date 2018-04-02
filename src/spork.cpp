// Copyright (c) 2009-2012 The Darkcoin developers
// Copyright (c) 2015-2016 The NTRN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "darksend.h"
#include "spork.h"
#include "main.h"

#include <boost/lexical_cast.hpp>

class CSporkMessage;
class CSporkManager;

CSporkManager sporkManager;

std::map<uint256, CSporkMessage> mapSporks;

void CSporkManager::ProcessSpork(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if(fLiteMode) return; //disable all darksend/masternode related functionality

    if (strCommand == NetMsgType::SPORK)
    {
        //LogPrintf("ProcessSpork::spork\n");
        CDataStream vMsg(vRecv);
        CSporkMessage spork;
        vRecv >> spork;

        if(pindexBest == NULL) return;

        // Ignore spork messages about unknown/deleted sporks
        std::string strSpork = sporkManager.GetSporkNameByID(spork.nSporkID);
        if (strSpork == "Unknown") return;

        uint256 hash = spork.GetHash();
        if(mapSporksActive.count(spork.nSporkID)) {
            if(mapSporksActive[spork.nSporkID].nTimeSigned >= spork.nTimeSigned){
                if(fDebug) LogPrintf("spork - seen %s block %d \n", hash.ToString(), pindexBest->nHeight);
                return;
            } else {
                if(fDebug) LogPrintf("spork - got updated spork %s block %d \n", hash.ToString(), pindexBest->nHeight);
            }
        }

        // LogPrintf("spork - new %s ID %d Time %d bestHeight %d\n", hash.ToString(), spork.nSporkID, spork.nValue, pindexBest->nHeight);

        if(!spork.CheckSignature()) {
            LogPrintf("spork - invalid signature\n");
            pfrom->Misbehaving(100);
            return;
        }

        mapSporks[hash] = spork;
        mapSporksActive[spork.nSporkID] = spork;
        spork.Relay();

        //does a task if needed
        ExecuteSpork(spork.nSporkID, spork.nValue);

    } else if (strCommand == NetMsgType::GETSPORKS) {
        std::map<int, CSporkMessage>::iterator it = mapSporksActive.begin();

        while(it != mapSporksActive.end()) {
            pfrom->PushMessage(NetMsgType::SPORK, it->second);
            it++;
        }
    }
}

void CSporkManager::ExecuteSpork(int nSporkID, int nValue)
{
    //replay and process blocks (to sync to the longest chain after disabling sporks)
    //if(nSporkID == SPORK_3_REPLAY_BLOCKS){
        //DisconnectBlocksAndReprocess(nValue);
    //}
}

bool CSporkManager::UpdateSpork(int nSporkID, int64_t nValue)
{

    CSporkMessage spork = CSporkMessage(nSporkID, nValue, GetTime());

    if(spork.Sign(strMasterPrivKey)) {
        spork.Relay();
        mapSporks[spork.GetHash()] = spork;
        mapSporksActive[nSporkID] = spork;
        return true;
    }

    return false;
}

// grab the spork, otherwise say it's off
bool CSporkManager::IsSporkActive(int nSporkID)
{
    int64_t r = -1;

    if(mapSporksActive.count(nSporkID)){
        r = mapSporksActive[nSporkID].nValue;
    } else {
        switch (nSporkID) {
            case SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT:    r = SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT_DEFAULT; break;
            case SPORK_2_MASTERNODE_WINNER_ENFORCEMENT:      r = SPORK_2_MASTERNODE_WINNER_ENFORCEMENT_DEFAULT; break;
            case SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT:     r = SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT_DEFAULT; break;
            case SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE:      r = SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE_DEFAULT; break;
            case SPORK_5_ENFORCE_NEW_PROTOCOL_V200:          r = SPORK_5_ENFORCE_NEW_PROTOCOL_V200_DEFAULT; break;
            case SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT:   r = SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT_DEFAULT; break;
            case SPORK_7_PROTOCOL_V201_ENFORCEMENT:          r = SPORK_7_PROTOCOL_V201_ENFORCEMENT_DEFAULT; break;
            default:
                LogPrintf("CSporkManager::IsSporkActive -- Unknown Spork ID %d\n", nSporkID);
                r = 4070908800ULL; // 2099-1-1 i.e. off by default
                break;
        }
    }

    return r < GetTime();
}

// grab the value of the spork on the network, or the default
int64_t CSporkManager::GetSporkValue(int nSporkID)
{
    int64_t r = -1;

    if(mapSporksActive.count(nSporkID)){
        r = mapSporksActive[nSporkID].nValue;
    } else {
        if(nSporkID == SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT) r = SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT_DEFAULT;
        if(nSporkID == SPORK_2_MASTERNODE_WINNER_ENFORCEMENT) r = SPORK_2_MASTERNODE_WINNER_ENFORCEMENT_DEFAULT;
        if(nSporkID == SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT) r = SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT_DEFAULT;
        if(nSporkID == SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE) r = SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE_DEFAULT;
        if(nSporkID == SPORK_5_ENFORCE_NEW_PROTOCOL_V200) r = SPORK_5_ENFORCE_NEW_PROTOCOL_V200_DEFAULT;
        if(nSporkID == SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT) r = SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT_DEFAULT;
        if(nSporkID == SPORK_7_PROTOCOL_V201_ENFORCEMENT) r = SPORK_7_PROTOCOL_V201_ENFORCEMENT_DEFAULT;

        if(r == -1 && fDebug) LogPrintf("GetSpork::Unknown Spork %d\n", nSporkID);
    }

    return r;
}

int CSporkManager::GetSporkIDByName(std::string strName)
{
    if(strName == "SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT") return SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT;
    if(strName == "SPORK_2_MASTERNODE_WINNER_ENFORCEMENT") return SPORK_2_MASTERNODE_WINNER_ENFORCEMENT;
    if(strName == "SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT") return SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT;
    if(strName == "SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE") return SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE;
    if(strName == "SPORK_5_ENFORCE_NEW_PROTOCOL_V200") return SPORK_5_ENFORCE_NEW_PROTOCOL_V200;
    if(strName == "SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT") return SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT;
    if(strName == "SPORK_7_PROTOCOL_V201_ENFORCEMENT") return SPORK_7_PROTOCOL_V201_ENFORCEMENT;

    LogPrint("spork", "CSporkManager::GetSporkIDByName -- Unknown Spork name '%s'\n", strName);

    return -1;
}

std::string CSporkManager::GetSporkNameByID(int id)
{
    if(id == SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT) return "SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT";
    if(id == SPORK_2_MASTERNODE_WINNER_ENFORCEMENT) return "SPORK_2_MASTERNODE_WINNER_ENFORCEMENT";
    if(id == SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT) return "SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT";
    if(id == SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE) return "SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE";
    if(id == SPORK_5_ENFORCE_NEW_PROTOCOL_V200) return "SPORK_5_ENFORCE_NEW_PROTOCOL_V200";
    if(id == SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT) return "SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT";
    if(id == SPORK_7_PROTOCOL_V201_ENFORCEMENT) return "SPORK_7_PROTOCOL_V201_ENFORCEMENT";

    LogPrint("spork", "CSporkManager::GetSporkNameByID -- Unknown Spork id '%s'\n", id);

    return "Unknown";
}

bool CSporkManager::SetPrivKey(std::string strPrivKey)
{
    CSporkMessage spork;

    spork.Sign(strPrivKey);

    if(spork.CheckSignature()){
        // Test signing successful, proceed
        LogPrintf("CSporkManager::SetPrivKey -- Successfully initialized as spork signer\n");
        strMasterPrivKey = strPrivKey;
        return true;
    } else {
        return false;
    }
}

bool CSporkMessage::Sign(std::string strSignKey)
{
    CKey key;
    CPubKey pubkey;
    std::string strError = "";
    std::string strMessage = boost::lexical_cast<std::string>(nSporkID) + boost::lexical_cast<std::string>(nValue) + boost::lexical_cast<std::string>(nTimeSigned);

    if(!darkSendSigner.SetKey(strSignKey, strError, key, pubkey))
    {
        LogPrintf("CSporkMessage::Sign - SetKey() failed, invalid spork key: '%s'\n", strSignKey);
        return false;
    }

    if(!darkSendSigner.SignMessage(strMessage, strError, vchSig, key)) {
        LogPrintf("CMasternodePayments::Sign - Sign message failed");
        return false;
    }

    if(!darkSendSigner.VerifyMessage(pubkey, vchSig, strMessage, strError)) {
        LogPrintf("CMasternodePayments::Sign - Verify message failed");
        return false;
    }

    return true;
}

bool CSporkMessage::CheckSignature()
{
    //note: need to investigate why this is failing
    std::string strError = "";
    std::string strMessage = boost::lexical_cast<std::string>(nSporkID) + boost::lexical_cast<std::string>(nValue) + boost::lexical_cast<std::string>(nTimeSigned);
    std::string strPubKey = (!fTestNet ? sporkManager.strMainPubKey : sporkManager.strTestPubKey);
    CPubKey pubkey(ParseHex(strPubKey));

    if(!darkSendSigner.VerifyMessage(pubkey, vchSig, strMessage, strError)) {
        LogPrintf("CSporkMessage::CheckSignature -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

void CSporkMessage::Relay()
{
    CInv inv(MSG_SPORK, GetHash());

    vector<CInv> vInv;
    vInv.push_back(inv);
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes){
        pnode->PushMessage(NetMsgType::INV, vInv);
    }
}
