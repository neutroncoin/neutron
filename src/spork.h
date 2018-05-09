// Copyright (c) 2009-2012 The Darkcoin developers
// Copyright (c) 2015-2017 The NTRN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SPORK_H
#define SPORK_H

#include "bignum.h"
#include "sync.h"
#include "net.h"
#include "key.h"

#include "util.h"
#include "script.h"
#include "base58.h"
#include "main.h"

using namespace std;
using namespace boost;

class CSporkMessage;
class CSporkManager;

#include "bignum.h"
#include "net.h"
#include "key.h"
#include "util.h"
#include "protocol.h"
#include "darksend.h"
#include <boost/lexical_cast.hpp>

using namespace std;
using namespace boost;

/*
    Don't ever reuse these IDs for other sporks
    - This would result in old clients getting confused about which spork is for what
*/
#define SPORK_START                                           10001
#define SPORK_END                                             10007

#define SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT               10001
#define SPORK_2_MASTERNODE_WINNER_ENFORCEMENT                 10002
#define SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT                10003
#define SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE                 10004
#define SPORK_5_ENFORCE_NEW_PROTOCOL_V200                     10005 // DEPRECATED at v2.0.2
#define SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT              10006 // DEPRECATED at v2.0.2
#define SPORK_7_PROTOCOL_V201_ENFORCEMENT                     10007 // DEPRECATED at v2.0.2

#define SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT_DEFAULT       4070908800 // OFF
#define SPORK_2_MASTERNODE_WINNER_ENFORCEMENT_DEFAULT         4070908800 // OFF
#define SPORK_3_DEVELOPER_PAYMENTS_ENFORCEMENT_DEFAULT        4070908800 // OFF
#define SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE_DEFAULT         0          // By default do not add to peer banscore
#define SPORK_5_ENFORCE_NEW_PROTOCOL_V200_DEFAULT             4070908800 // OFF
#define SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT_DEFAULT      4070908800 // OFF
#define SPORK_7_PROTOCOL_V201_ENFORCEMENT_DEFAULT             4070908800 // OFF

extern std::map<uint256, CSporkMessage> mapSporks;
extern CSporkManager sporkManager;

//
// Spork Class
// Keeps track of all of the network spork settings
//

class CSporkMessage
{
private:
    std::vector<unsigned char> vchSig;

public:
    int nSporkID;
    int64_t nValue;
    int64_t nTimeSigned;

    CSporkMessage(int nSporkID, int64_t nValue, int64_t nTimeSigned) :
        nSporkID(nSporkID),
        nValue(nValue),
        nTimeSigned(nTimeSigned)
        {}

    CSporkMessage() :
        nSporkID(0),
        nValue(0),
        nTimeSigned(0)
        {}


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
	unsigned int nSerSize = 0;
        READWRITE(nSporkID);
        READWRITE(nValue);
        READWRITE(nTimeSigned);
        READWRITE(vchSig);
	}

    uint256 GetHash(){
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
    std::string strTestPubKey;
    std::string strMainPubKey;

    CSporkManager() {
        strMainPubKey = "04cc53cdd3e788d3ea9ca63468b9f2bcc2838af920d8e72985739e8ac4159d518d1a1597da13b1854d8331def51778aa6a01951cef7763fa4300341f34431bad49";
        strTestPubKey = "042E0E340B40681EEFB7C67B7CBE968E3AB47F4A393E3626E13309CFDC5A1C5D5B9537CD3CEBA3B5B1656D2949355CADA0F5EE74C4EDCCBEF84BF80151EF3B0C0A";
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
