// Copyright (c) 2017-2019 The Swipp developers
// Copyright (c) 2019-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING.daemon or http://www.opensource.org/licenses/mit-license.php.

#ifndef NEUTRON_COLLECTIONHASHING_H
#define NEUTRON_COLLECTIONHASHING_H

#include <cstddef>
#include <utility>
#include <functional>
#include <boost/functional/hash.hpp>

#include "robinhood.h"
#include "uint256.h"

namespace std
{
    template<> struct hash<uint256>
    {
        size_t operator()(const uint256& v) const
        {
            return robin_hood::hash_bytes(v.begin(), v.size());
        }
    };
}

#endif
