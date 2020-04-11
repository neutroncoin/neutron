// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "addrdb.h"
#include "addrman.h"
#include "chainparams.h"
#include "clientversion.h"
#include "hash.h"
#include "random.h"
#include "streams.h"
#include "tinyformat.h"
#include "util.h"

#include <boost/filesystem.hpp>

CBanDB::CBanDB()
{
    pathBanlist = GetDataDir() / "banlist.dat";
}

bool CBanDB::Write(const banmap_t& banSet)
{
    // Generate random temporary filename
    unsigned short randv = 0;
    GetRandBytes((unsigned char*)&randv, sizeof(randv));
    std::string tmpfn = strprintf("banlist.dat.%04x", randv);

    // Serialize banlist, checksum data up to that point, then append csum
    CDataStream ssBanlist(SER_DISK, CLIENT_VERSION);
    ssBanlist << FLATDATA(pchMessageStart);
    ssBanlist << banSet;
    uint256 hash = Hash(ssBanlist.begin(), ssBanlist.end());
    ssBanlist << hash;

    // Open temp output file, and associate with CAutoFile
    boost::filesystem::path pathTmp = GetDataDir() / tmpfn;
    FILE *file = fopen(pathTmp.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);

    if (fileout.IsNull())
        return error("%s : failed to open file %s", __func__, pathTmp.string());

    // Write and commit header, data
    try
    {
        fileout << ssBanlist;
    }
    catch (const std::exception& e)
    {
        return error("%s : serialize or I/O error - %s", __func__, e.what());
    }

    FileCommit(fileout.Get());
    fileout.fclose();

    // Replace existing banlist.dat, if any, with new banlist.dat.XXXX
    if (!RenameOver(pathTmp, pathBanlist))
        return error("%s : rename-into-place failed", __func__);

    return true;
}

bool CBanDB::Read(banmap_t& banSet)
{
    // Open input file, and associate with CAutoFile
    FILE *file = fopen(pathBanlist.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);

    if (filein.IsNull())
        return error("%s : failed to open file %s", __func__, pathBanlist.string());

    // Use file size to size memory buffer
    uint64_t fileSize = boost::filesystem::file_size(pathBanlist);
    uint64_t dataSize = 0;

    // Don't try to resize to a negative number if file is small
    if (fileSize >= sizeof(uint256))
        dataSize = fileSize - sizeof(uint256);

    std::vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    try
    {
        filein.read((char *)&vchData[0], dataSize);
        filein >> hashIn;
    }
    catch (const std::exception& e)
    {
        return error("%s : deserialize or I/O error - %s", __func__, e.what());
    }

    filein.fclose();
    CDataStream ssBanlist(vchData, SER_DISK, CLIENT_VERSION);

    // Verify stored checksum matches input data
    uint256 hashTmp = Hash(ssBanlist.begin(), ssBanlist.end());

    if (hashIn != hashTmp)
        return error("%s : checksum mismatch, data corrupted", __func__);

    unsigned char pchMsgTmp[4];
    try
    {
        // De-serialize file header (network specific magic number) and ..
        ssBanlist >> FLATDATA(pchMsgTmp);

        // ...verify the network matches ours
        if (memcmp(pchMsgTmp, pchMessageStart, sizeof(pchMsgTmp)))
            return error("%s : invalid network magic number", __func__);

        // De-serialize ban data
        ssBanlist >> banSet;
    }
    catch (const std::exception& e)
    {
        return error("%s : deserialize or I/O error - %s", __func__, e.what());
    }

    return true;
}

CAddrDB::CAddrDB()
{
    pathAddr = GetDataDir() / "peers.dat";
}

bool CAddrDB::Write(const CAddrMan& addr)
{
    // Generate random temporary filename
    unsigned short randv = 0;
    GetRandBytes((unsigned char*)&randv, sizeof(randv));
    std::string tmpfn = strprintf("peers.dat.%04x", randv);

    // Serialize addresses, checksum data up to that point, then append csum
    CDataStream ssPeers(SER_DISK, CLIENT_VERSION);
    ssPeers << FLATDATA(pchMessageStart);
    ssPeers << addr;
    uint256 hash = Hash(ssPeers.begin(), ssPeers.end());
    ssPeers << hash;

    // Open temp output file, and associate with CAutoFile
    boost::filesystem::path pathTmp = GetDataDir() / tmpfn;
    FILE *file = fopen(pathTmp.string().c_str(), "wb");
    CAutoFile fileout = CAutoFile(file, SER_DISK, CLIENT_VERSION);

    if (!fileout)
        return error("%s : open failed", __func__);

    // Write and commit header, data
    try
    {
        fileout << ssPeers;
    }
    catch (const std::exception& e)
    {
        return error("%s : serialize or I/O error - %s", __func__, e.what());
    }

    FileCommit(fileout);
    fileout.fclose();

    // Replace existing peers.dat, if any, with new peers.dat.XXXX
    if (!RenameOver(pathTmp, pathAddr))
        return error("%s : rename-into-place failed", __func__);

    return true;
}

bool CAddrDB::Read(CAddrMan& addr)
{
    // Open input file, and associate with CAutoFile
    FILE *file = fopen(pathAddr.string().c_str(), "rb");
    CAutoFile filein = CAutoFile(file, SER_DISK, CLIENT_VERSION);

    if (!filein)
        return error("%s : open failed", __func__);

    // Use file size to size memory buffer
    uint64_t fileSize = boost::filesystem::file_size(pathAddr);
    uint64_t dataSize = 0;

    // Dont try to resize to a negative number if file is small
    if (fileSize >= sizeof(uint256))
        dataSize = fileSize - sizeof(uint256);

    std::vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // Read data and checksum from file
    try
    {
        filein.read((char *)&vchData[0], dataSize);
        filein >> hashIn;
    }
    catch (const std::exception& e)
    {
        return error("%s : deserialize or I/O error - %s", __func__, e.what());
    }

    filein.fclose();
    CDataStream ssPeers(vchData, SER_DISK, CLIENT_VERSION);

    // Verify stored checksum matches input data
    uint256 hashTmp = Hash(ssPeers.begin(), ssPeers.end());

    if (hashIn != hashTmp)
        return error("%s : checksum mismatch, data corrupted", __func__);

    return Read(addr, ssPeers);
}

bool CAddrDB::Read(CAddrMan& addr, CDataStream& ssPeers)
{
    unsigned char pchMsgTmp[4];

    try
    {
        // De-serialize file header (network specific magic number) and ..
        ssPeers >> FLATDATA(pchMsgTmp);

        // ...verify the network matches ours
        if (memcmp(pchMsgTmp, pchMessageStart, sizeof(pchMsgTmp)))
            return error("%s : invalid network magic number", __func__);

        // De-serialize address data into one CAddrMan object
        ssPeers >> addr;
    }
    catch (const std::exception& e)
    {
        // De-serialization has failed, ensure addrman is left in a clean state
        // addr.Clear();
        return error("%s : deserialize or I/O error - %s", __func__, e.what());
    }

    return true;
}
