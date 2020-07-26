// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CLIENTVERSION_H
#define BITCOIN_CLIENTVERSION_H

// client versioning

// These need to be macros, as clientversion.cpp's and neutron*-res.rc's voodoo requires it
#define CLIENT_VERSION_MAJOR       4
#define CLIENT_VERSION_MINOR       1
#define CLIENT_VERSION_REVISION    1
#define CLIENT_VERSION_BUILD       0

#define COPYRIGHT_YEAR 2020

// Converts the parameter X to a string after macro replacement on X has been performed.
// Don't merge these into one macro!
#define STRINGIZE(X) DO_STRINGIZE(X)
#define DO_STRINGIZE(X) #X

#if !defined(WINDRES_PREPROC)

#include <string>
#include <vector>

static const int CLIENT_VERSION =
    1000000 * CLIENT_VERSION_MAJOR  ///
    + 10000 * CLIENT_VERSION_MINOR  ///
    + 100 * CLIENT_VERSION_REVISION ///
    + 1 * CLIENT_VERSION_BUILD;

extern const std::string CLIENT_NAME;
extern const std::string CLIENT_BUILD;
extern const std::string CLIENT_DATE;


std::string FormatFullVersion();
std::string FormatSubVersion(const std::string& name, int nClientVersion, const std::vector<std::string>& comments);

#endif // WINDRES_PREPROC

#endif // BITCOIN_CLIENTVERSION_H
