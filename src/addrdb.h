// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ADDRDB_H
#define ADDRDB_H

#include "serialize.h"

#include <string>
#include <map>
#include <boost/filesystem/path.hpp>

class CSubNet;
class CAddrMan;
class CDataStream;

typedef enum BanReason
{
    BanReasonUnknown          = 0,
    BanReasonNodeMisbehaving  = 1,
    BanReasonManuallyAdded    = 2
} BanReason;

class CBanEntry
{
public:
    static const int CURRENT_VERSION=1;
    int nVersion;
    int64_t nCreateTime;
    int64_t nBanUntil;
    uint8_t banReason;

    CBanEntry()
    {
        SetNull();
    }

    CBanEntry(int64_t nCreateTimeIn)
    {
        SetNull();
        nCreateTime = nCreateTimeIn;
    }

    IMPLEMENT_SERIALIZE(
        READWRITE(this->nVersion);
        READWRITE(nCreateTime);
        READWRITE(nBanUntil);
        READWRITE(banReason);
    )

    void SetNull()
    {
        nVersion = CBanEntry::CURRENT_VERSION;
        nCreateTime = 0;
        nBanUntil = 0;
        banReason = BanReasonUnknown;
    }

    std::string banReasonToString()
    {
        switch (banReason)
        {
            case BanReasonNodeMisbehaving:
                return "node misbehaving";
            case BanReasonManuallyAdded:
                return "manually added";
            default:
                return "unknown";
        }
    }
};

typedef std::map<CSubNet, CBanEntry> banmap_t;

class CAddrDB
{
private:
    boost::filesystem::path pathAddr;
public:
    CAddrDB();
    bool Write(const CAddrMan& addr);
    bool Read(CAddrMan& addr);
    bool Read(CAddrMan& addr, CDataStream& ssPeers);
};

class CBanDB
{
private:
    boost::filesystem::path pathBanlist;
public:
    CBanDB();
    bool Write(const banmap_t& banSet);
    bool Read(banmap_t& banSet);
};

#endif // ADDRDB_H
