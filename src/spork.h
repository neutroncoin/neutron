// Copyright (c) 2009-2012 The Darkcoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SPORK_H
#define SPORK_H

#include "base58.h"
#include "key.h"
#include "main.h"
#include "net.h"
#include "sync.h"
#include "util.h"

#include "bignum.h"
#include "script.h"

using namespace std;
using namespace boost;

/* Dont ever reuse these IDs for other sporks. This would result in old clients
   getting confused about which spork is for what */

#define SPORK_START                                           10001
#define SPORK_END                                             10014

#define SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT               10001 // DEPRECATED
#define SPORK_2_MASTERNODE_WINNER_ENFORCEMENT                 10002
#define SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT                10003 // DEPRECATED
#define SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE                 10004
#define SPORK_5_ENFORCE_NEW_PROTOCOL_V200                     10005 // DEPRECATED
#define SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT              10006 // DEPRECATED
#define SPORK_7_PROTOCOL_V201_ENFORCEMENT                     10007 // DEPRECATED
#define SPORK_8_PROTOCOL_V210_ENFORCEMENT                     10008 // DEPRECATED
#define SPORK_9_PROTOCOL_V3_ENFORCEMENT                       10009
#define SPORK_10_V3_DEV_PAYMENTS_ENFORCEMENT                  10010 // DEPRECATED
#define SPORK_11_PROTOCOL_V301_ENFORCEMENT                    10011
#define SPORK_12_PAYMENT_ENFORCEMENT_THRESHOLD                10012
#define SPORK_13_PROTOCOL_V4_ENFORCEMENT                      10013
#define SPORK_14_MASTERNODE_DISTRIBUTION_TICK                 10014

#define SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT_DEFAULT       1525030000 // OFF
#define SPORK_2_MASTERNODE_WINNER_ENFORCEMENT_DEFAULT         1567330017 // OFF
#define SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT_DEFAULT        1525030000 // OFF
#define SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE_DEFAULT         10
#define SPORK_5_ENFORCE_NEW_PROTOCOL_V200_DEFAULT             1513466452 // OFF
#define SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT_DEFAULT      1524890000 // OFF
#define SPORK_7_PROTOCOL_V201_ENFORCEMENT_DEFAULT             1524890000 // OFF
#define SPORK_8_PROTOCOL_V210_ENFORCEMENT_DEFAULT             1544940000 // OFF
#define SPORK_9_PROTOCOL_V3_ENFORCEMENT_DEFAULT               1562561521 // OFF
#define SPORK_10_V3_DEV_PAYMENTS_ENFORCEMENT_DEFAULT          1562995301 // OFF
#define SPORK_11_PROTOCOL_V301_ENFORCEMENT_DEFAULT            1567330017 // 2019-09-01 00:00 GMT
#define SPORK_12_PAYMENT_ENFORCEMENT_THRESHOLD_DEFAULT        5
#define SPORK_13_PROTOCOL_V4_ENFORCEMENT_DEFAULT              1591488000 // 2020-06-7 00:00 GMT
#define SPORK_14_MASTERNODE_DISTRIBUTION_TICK_DEFAULT         10

#define REJECT_OLD_SPORKKEY_TIME                              1567330017 // 2019-09-01 00:00 GMT

class CSporkMessage;
class CSporkManager;

extern std::map<uint256, CSporkMessage> mapSporks;
extern CSporkManager sporkManager;

class CSporkMessage
{
private:
    std::vector<unsigned char> vchSig;

public:
    int nSporkID;
    int64_t nValue;
    int64_t nTimeSigned;

    CSporkMessage(int nSporkID, int64_t nValue, int64_t nTimeSigned) :
        nSporkID(nSporkID), nValue(nValue), nTimeSigned(nTimeSigned) { }

    CSporkMessage() : nSporkID(0), nValue(0), nTimeSigned(0) { }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        unsigned int nSerSize = 0;
        READWRITE(nSporkID);
        READWRITE(nValue);
        READWRITE(nTimeSigned);
        READWRITE(vchSig);
    }

    uint256 GetHash()
    {
        uint256 n = Hash(BEGIN(nSporkID), END(nTimeSigned));
        return n;
    }

    bool Sign(std::string strSignKey);
    bool CheckSignature();
    void Relay();
};


class CSporkManager
{
private:
    std::vector<unsigned char> vchSig;
    std::string strMasterPrivKey;
    std::map<int, CSporkMessage> mapSporksActive;

public:
    std::string strTestPubKeyNew;
    std::string strMainPubKeyNew;
    std::string strTestPubKeyOld;
    std::string strMainPubKeyOld;

    CSporkManager() {
        strMainPubKeyNew = "042b98d4150746cc5ee1b5a991244f8a2b155630efbfa490fee76202912ed2d6e"
                           "9b6e5c62d424b9f5878ee7aff68e9aa84d10821a33e99de27fed2d77f57247954";
        strTestPubKeyNew = "042b98d4150746cc5ee1b5a991244f8a2b155630efbfa490fee76202912ed2d6e"
                           "9b6e5c62d424b9f5878ee7aff68e9aa84d10821a33e99de27fed2d77f57247954";
        strMainPubKeyOld = "04cc53cdd3e788d3ea9ca63468b9f2bcc2838af920d8e72985739e8ac4159d518"
                           "d1a1597da13b1854d8331def51778aa6a01951cef7763fa4300341f34431bad49";
        strTestPubKeyOld = "042E0E340B40681EEFB7C67B7CBE968E3AB47F4A393E3626E13309CFDC5A1C5D5"
                           "B9537CD3CEBA3B5B1656D2949355CADA0F5EE74C4EDCCBEF84BF80151EF3B0C0A";
    }

    void ProcessSpork(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);
    void ExecuteSpork(int nSporkID, int nValue);
    bool UpdateSpork(int nSporkID, int64_t nValue);
    bool IsSporkActive(int nSporkID);
    int64_t GetSporkValue(int nSporkID);
    int GetSporkIDByName(std::string strName);
    std::string GetSporkNameByID(int id);
    bool SetPrivKey(std::string strPrivKey);
};

#endif
