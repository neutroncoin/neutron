// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ADDRDB_H
#define BITCOIN_ADDRDB_H

#include "serialize.h"

#include <string>
#include <map>
#include <boost/filesystem/path.hpp>

class CSubNet;
class CAddrMan;
// class CDataStream;


typedef std::map<CSubNet, int64_t> banmap_t;

/** Access to the (IP) address database (peers.dat) */
class CAddrDB
{
private:
    boost::filesystem::path pathAddr;
public:
    CAddrDB();
    bool Write(const CAddrMan& addr);
    bool Read(CAddrMan& addr);
    // bool Read(CAddrMan& addr, CDataStream& ssPeers);
};

/** Access to the banlist database (banlist.dat) */
class CBanDB
{
private:
    boost::filesystem::path pathBanlist;
public:
    CBanDB();
    bool Write(const banmap_t& banSet);
    bool Read(banmap_t& banSet);
};

#endif // BITCOIN_ADDRDB_H
