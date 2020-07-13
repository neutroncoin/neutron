// Copyright (c) 2009-2012 The Darkcoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "darksend.h"
#include "spork.h"
#include "main.h"

#include <sstream>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>

class CSporkMessage;
class CSporkManager;

CSporkManager sporkManager;
std::map<uint256, CSporkMessage> mapSporks;

void CSporkManager::ProcessSpork(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (strCommand == NetMsgType::SPORK)
    {
        CDataStream vMsg(vRecv);
        CSporkMessage spork;
        vRecv >> spork;

        if (pindexBest == NULL)
            return;

        // Ignore spork messages about unknown/deleted sporks
        std::string strSpork = sporkManager.GetSporkNameByID(spork.nSporkID);

        if (strSpork == "Unknown")
            return;

        uint256 hash = spork.GetHash();

        if (mapSporksActive.count(spork.nSporkID))
        {
            if (mapSporksActive[spork.nSporkID].nTimeSigned >= spork.nTimeSigned)
            {
                if (fDebug)
                    LogPrintf("%s : seen %s block %d \n", __func__, hash.ToString(), pindexBest->nHeight);

                return;
            }
            else if (fDebug)
                LogPrintf("%s : got updated spork %s block %d \n", __func__, hash.ToString(), pindexBest->nHeight);
        }

        if (!spork.CheckSignature())
        {
            std::stringstream msg;
            msg << boost::format("%s : invalid spork signature") % __func__;

            LogPrintf("%s\n", msg.str().c_str());
            pfrom->Misbehaving(msg.str(), 100);
            return;
        }

        mapSporks[hash] = spork;
        mapSporksActive[spork.nSporkID] = spork;
        spork.Relay();

        // Does a task if needed
        ExecuteSpork(spork.nSporkID, spork.nValue);
    }
    else if (strCommand == NetMsgType::GETSPORKS)
    {
        std::map<int, CSporkMessage>::iterator it = mapSporksActive.begin();

        while(it != mapSporksActive.end())
        {
            pfrom->PushMessage(NetMsgType::SPORK, it->second);
            it++;
        }
    }
}

void CSporkManager::ExecuteSpork(int nSporkID, int nValue)
{
    /* Intentionally left empty */
}

bool CSporkManager::UpdateSpork(int nSporkID, int64_t nValue)
{

    CSporkMessage spork = CSporkMessage(nSporkID, nValue, GetTime());

    if(spork.Sign(strMasterPrivKey))
    {
        spork.Relay();
        mapSporks[spork.GetHash()] = spork;
        mapSporksActive[nSporkID] = spork;

        return true;
    }

    return false;
}

bool CSporkManager::IsSporkActive(int nSporkID)
{
    int64_t r = -1;

    if(mapSporksActive.count(nSporkID))
        r = mapSporksActive[nSporkID].nValue;
    else
    {
        switch (nSporkID)
        {
            case SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT:  r = SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT_DEFAULT; break;
            case SPORK_2_MASTERNODE_WINNER_ENFORCEMENT:    r = SPORK_2_MASTERNODE_WINNER_ENFORCEMENT_DEFAULT; break;
            case SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT:   r = SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT_DEFAULT; break;
            case SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE:    r = SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE_DEFAULT; break;
            case SPORK_5_ENFORCE_NEW_PROTOCOL_V200:        r = SPORK_5_ENFORCE_NEW_PROTOCOL_V200_DEFAULT; break;
            case SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT: r = SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT_DEFAULT; break;
            case SPORK_7_PROTOCOL_V201_ENFORCEMENT:        r = SPORK_7_PROTOCOL_V201_ENFORCEMENT_DEFAULT; break;
            case SPORK_8_PROTOCOL_V210_ENFORCEMENT:        r = SPORK_8_PROTOCOL_V210_ENFORCEMENT_DEFAULT; break;
            case SPORK_9_PROTOCOL_V3_ENFORCEMENT:          r = SPORK_9_PROTOCOL_V3_ENFORCEMENT_DEFAULT; break;
            case SPORK_10_V3_DEV_PAYMENTS_ENFORCEMENT:     r = SPORK_10_V3_DEV_PAYMENTS_ENFORCEMENT_DEFAULT; break;
            case SPORK_11_PROTOCOL_V301_ENFORCEMENT:       r = SPORK_11_PROTOCOL_V301_ENFORCEMENT_DEFAULT; break;
            case SPORK_12_PAYMENT_ENFORCEMENT_THRESHOLD:   r = SPORK_12_PAYMENT_ENFORCEMENT_THRESHOLD_DEFAULT; break;
            case SPORK_13_PROTOCOL_V4_ENFORCEMENT:         r = SPORK_13_PROTOCOL_V4_ENFORCEMENT_DEFAULT; break;
            case SPORK_14_MASTERNODE_DISTRIBUTION_TICK:    r = SPORK_14_MASTERNODE_DISTRIBUTION_TICK_DEFAULT; break;
            default:
                LogPrintf("%s : unknown spork ID %d\n", __func__, nSporkID);
                r = 4070908800ULL; // off by default
                break;
        }
    }

    return r < GetTime();
}

int64_t CSporkManager::GetSporkValue(int nSporkID)
{
    int64_t r = -1;

    if (mapSporksActive.count(nSporkID))
        r = mapSporksActive[nSporkID].nValue;
    else
    {
        if (nSporkID == SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT)       r = SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT_DEFAULT;
        else if (nSporkID == SPORK_2_MASTERNODE_WINNER_ENFORCEMENT)    r = SPORK_2_MASTERNODE_WINNER_ENFORCEMENT_DEFAULT;
        else if (nSporkID == SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT)   r = SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT_DEFAULT;
        else if (nSporkID == SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE)    r = SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE_DEFAULT;
        else if (nSporkID == SPORK_5_ENFORCE_NEW_PROTOCOL_V200)        r = SPORK_5_ENFORCE_NEW_PROTOCOL_V200_DEFAULT;
        else if (nSporkID == SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT) r = SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT_DEFAULT;
        else if (nSporkID == SPORK_7_PROTOCOL_V201_ENFORCEMENT)        r = SPORK_7_PROTOCOL_V201_ENFORCEMENT_DEFAULT;
        else if (nSporkID == SPORK_8_PROTOCOL_V210_ENFORCEMENT)        r = SPORK_8_PROTOCOL_V210_ENFORCEMENT_DEFAULT;
        else if (nSporkID == SPORK_9_PROTOCOL_V3_ENFORCEMENT)          r = SPORK_9_PROTOCOL_V3_ENFORCEMENT_DEFAULT;
        else if (nSporkID == SPORK_10_V3_DEV_PAYMENTS_ENFORCEMENT)     r = SPORK_10_V3_DEV_PAYMENTS_ENFORCEMENT_DEFAULT;
        else if (nSporkID == SPORK_11_PROTOCOL_V301_ENFORCEMENT)       r = SPORK_11_PROTOCOL_V301_ENFORCEMENT_DEFAULT;
        else if (nSporkID == SPORK_12_PAYMENT_ENFORCEMENT_THRESHOLD)   r = SPORK_12_PAYMENT_ENFORCEMENT_THRESHOLD_DEFAULT;
        else if (nSporkID == SPORK_13_PROTOCOL_V4_ENFORCEMENT)         r = SPORK_13_PROTOCOL_V4_ENFORCEMENT_DEFAULT;
        else if (nSporkID == SPORK_14_MASTERNODE_DISTRIBUTION_TICK)    r = SPORK_14_MASTERNODE_DISTRIBUTION_TICK_DEFAULT;

        if (r == -1 && fDebug)
            LogPrintf("%s : unknown spork %d\n", __func__, nSporkID);
    }

    return r;
}

int CSporkManager::GetSporkIDByName(std::string strName)
{
    if (strName == "SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT")       return SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT;
    else if (strName == "SPORK_2_MASTERNODE_WINNER_ENFORCEMENT")    return SPORK_2_MASTERNODE_WINNER_ENFORCEMENT;
    else if (strName == "SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT")   return SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT;
    else if (strName == "SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE")    return SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE;
    else if (strName == "SPORK_5_ENFORCE_NEW_PROTOCOL_V200")        return SPORK_5_ENFORCE_NEW_PROTOCOL_V200;
    else if (strName == "SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT") return SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT;
    else if (strName == "SPORK_7_PROTOCOL_V201_ENFORCEMENT")        return SPORK_7_PROTOCOL_V201_ENFORCEMENT;
    else if (strName == "SPORK_8_PROTOCOL_V210_ENFORCEMENT")        return SPORK_8_PROTOCOL_V210_ENFORCEMENT;
    else if (strName == "SPORK_9_PROTOCOL_V3_ENFORCEMENT")          return SPORK_9_PROTOCOL_V3_ENFORCEMENT;
    else if (strName == "SPORK_10_V3_DEV_PAYMENTS_ENFORCEMENT")     return SPORK_10_V3_DEV_PAYMENTS_ENFORCEMENT;
    else if (strName == "SPORK_11_PROTOCOL_V301_ENFORCEMENT")       return SPORK_11_PROTOCOL_V301_ENFORCEMENT;
    else if (strName == "SPORK_12_PAYMENT_ENFORCEMENT_THRESHOLD")   return SPORK_12_PAYMENT_ENFORCEMENT_THRESHOLD;
    else if (strName == "SPORK_13_PROTOCOL_V4_ENFORCEMENT")         return SPORK_13_PROTOCOL_V4_ENFORCEMENT;
    else if (strName == "SPORK_14_MASTERNODE_DISTRIBUTION_TICK")    return SPORK_14_MASTERNODE_DISTRIBUTION_TICK;

    LogPrintf("%s : unknown spork name '%s'\n", __func__, strName);
    return -1;
}

std::string CSporkManager::GetSporkNameByID(int id)
{
    if (id == SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT)       return "SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT";
    else if (id == SPORK_2_MASTERNODE_WINNER_ENFORCEMENT)    return "SPORK_2_MASTERNODE_WINNER_ENFORCEMENT";
    else if (id == SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT)   return "SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT";
    else if (id == SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE)    return "SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE";
    else if (id == SPORK_5_ENFORCE_NEW_PROTOCOL_V200)        return "SPORK_5_ENFORCE_NEW_PROTOCOL_V200";
    else if (id == SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT) return "SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT";
    else if (id == SPORK_7_PROTOCOL_V201_ENFORCEMENT)        return "SPORK_7_PROTOCOL_V201_ENFORCEMENT";
    else if (id == SPORK_8_PROTOCOL_V210_ENFORCEMENT)        return "SPORK_8_PROTOCOL_V210_ENFORCEMENT";
    else if (id == SPORK_9_PROTOCOL_V3_ENFORCEMENT)          return "SPORK_9_PROTOCOL_V3_ENFORCEMENT";
    else if (id == SPORK_10_V3_DEV_PAYMENTS_ENFORCEMENT)     return "SPORK_10_V3_DEV_PAYMENTS_ENFORCEMENT";
    else if (id == SPORK_11_PROTOCOL_V301_ENFORCEMENT)       return "SPORK_11_PROTOCOL_V301_ENFORCEMENT";
    else if (id == SPORK_12_PAYMENT_ENFORCEMENT_THRESHOLD)   return "SPORK_12_PAYMENT_ENFORCEMENT_THRESHOLD";
    else if (id == SPORK_13_PROTOCOL_V4_ENFORCEMENT)         return "SPORK_13_PROTOCOL_V4_ENFORCEMENT";
    else if (id == SPORK_14_MASTERNODE_DISTRIBUTION_TICK)    return "SPORK_14_MASTERNODE_DISTRIBUTION_TICK";

    LogPrintf("%s : unknown spork id '%s'\n", __func__, id);
    return "Unknown";
}

bool CSporkManager::SetPrivKey(std::string strPrivKey)
{
    CSporkMessage spork;
    spork.Sign(strPrivKey);

    if (spork.CheckSignature())
    {
        // Test signing successful, proceed
        LogPrintf("%s : successfully initialized as spork signer\n", __func__);
        strMasterPrivKey = strPrivKey;

        return true;
    }
    else
        return false;
}

bool CSporkMessage::Sign(std::string strSignKey)
{
    CKey key;
    CPubKey pubkey;
    std::string strError = "";
    std::string strMessage = boost::lexical_cast<std::string>(nSporkID) +
                boost::lexical_cast<std::string>(nValue) +
                boost::lexical_cast<std::string>(nTimeSigned);

    if (!darkSendSigner.SetKey(strSignKey, strError, key, pubkey))
    {
        LogPrintf("%s : SetKey() failed, invalid spork key: '%s'\n", __func__, strSignKey);
        return false;
    }

    if (!darkSendSigner.SignMessage(strMessage, strError, vchSig, key))
    {
        LogPrintf("%s : sign message failed\n", __func__);
        return false;
    }

    if (!darkSendSigner.VerifyMessage(pubkey, vchSig, strMessage, strError))
    {
        LogPrintf("%s : verify message failed\n", __func__);
        return false;
    }

    return true;
}

bool CSporkMessage::CheckSignature()
{
    // NOTE: need to investigate why this is failing
    std::string strError = "";
    std::string strMessage = boost::lexical_cast<std::string>(nSporkID) +
                boost::lexical_cast<std::string>(nValue) +
                boost::lexical_cast<std::string>(nTimeSigned);
    std::string strPubKeyNew = (!fTestNet ? sporkManager.strMainPubKeyNew : sporkManager.strTestPubKeyNew);
    CPubKey pubkeyNew(ParseHex(strPubKeyNew));

    if (!darkSendSigner.VerifyMessage(pubkeyNew, vchSig, strMessage, strError))
    {
        if (GetAdjustedTime() < REJECT_OLD_SPORKKEY_TIME)
        {
            std::string strPubKeyOld = !fTestNet ? sporkManager.strMainPubKeyOld : sporkManager.strTestPubKeyOld;
            CPubKey pubkeyOld(ParseHex(strPubKeyOld));

            if (!darkSendSigner.VerifyMessage(pubkeyOld, vchSig, strMessage, strError))
            {
                LogPrintf("%s : VerifyMessage() failed (old key), error: %s\n", __func__, strError);
                return false;
            }
        }
        else
        {
            LogPrintf("%s : VerifyMessage() failed (new key), error: %s\n", __func__, strError);
            return false;
        }
    }

    return true;
}

void CSporkMessage::Relay()
{
    CInv inv(MSG_SPORK, GetHash());
    vector<CInv> vInv;
    vInv.push_back(inv);

    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        pnode->PushMessage(NetMsgType::INV, vInv);
    }
}
