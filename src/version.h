// Copyright (c) 2012 The Bitcoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_VERSION_H
#define BITCOIN_VERSION_H

#include <sstream>
#include <string>

// database format versioning
static const int DATABASE_VERSION = 70509;

// network protocol versioning
static const int PROTOCOL_VERSION = 60025;

// intial proto version, to be increased after version/verack negotiation
static const int INIT_PROTO_VERSION = 209;

// disconnect from peers older than this proto version
static const int MIN_PEER_PROTO_VERSION_BEFORE_V200_ENFORCEMENT = 60015;
static const int MIN_PEER_PROTO_VERSION_AFTER_V200_ENFORCEMENT = 60016;
static const int MIN_PEER_PROTO_VERSION_AFTER_V201_ENFORCEMENT = 60017;
static const int MIN_PEER_PROTO_VERSION_AFTER_V210_ENFORCEMENT = 60018;
static const int MIN_PEER_PROTO_VERSION_AFTER_V3_ENFORCEMENT = 60019;
static const int MIN_PEER_PROTO_VERSION_AFTER_V301_ENFORCEMENT = 60020;
static const int MIN_PEER_PROTO_VERSION_AFTER_V301_ENFORCEMENT_AND_MNENFORCE = 60021;
static const int MIN_PEER_PROTO_VERSION_AFTER_V4_ENFORCEMENT = 60025;

// nTime field added to CAddress, starting with this version;
// if possible, avoid requesting addresses nodes older than this
static const int CADDR_TIME_VERSION = 31402;

// only request blocks from nodes outside this range of versions
static const int NOBLKS_VERSION_START = 0;
static const int NOBLKS_VERSION_END = 60015;

// BIP 0031, pong message, is enabled for all versions AFTER this one
static const int BIP0031_VERSION = 60000;

// "mempool" command, enhanced "getdata" behavior starts with this version:
static const int MEMPOOL_GD_VERSION = 60002;

//struct ComparableVersion
//{
//    int major = 0, minor = 0, revision = 0, build = 0;
//
//    ComparableVersion(std::string version)
//    {
//        std::sscanf(version.c_str(), "%d.%d.%d.%d", &major, &minor, &revision, &build);
//    }
//
//    bool operator < (const ComparableVersion& other) const
//    {
//        if (major < other.major)
//            return true;
//        else if (minor < other.minor)
//            return true;
//        else if (revision < other.revision)
//            return true;
//        else if (build < other.build)
//            return true;
//        return false;
//    }
//
//    bool operator == (const ComparableVersion& other)
//    {
//        return major == other.major
//            && minor == other.minor
//            && revision == other.revision
//            && build == other.build;
//    }
//
//    friend std::ostream& operator << (std::ostream& stream, const ComparableVersion& ver)
//    {
//        stream << ver.major;
//        stream << '.';
//        stream << ver.minor;
//        stream << '.';
//        stream << ver.revision;
//        stream << '.';
//        stream << ver.build;
//        return stream;
//    }
//
//    std::string ToString() const
//    {
//        std::stringstream s;
//        s << major << "." << minor << "." << revision << "." << build;
//        return s.str();
//    }
//};

#endif
