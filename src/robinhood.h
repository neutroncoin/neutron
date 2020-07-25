// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING.daemon or http://www.opensource.org/licenses/mit-license.php.

#ifndef NEUTRON_ROBINHOOD_H
#define NEUTRON_ROBINHOOD_H

// Having these defined with the mingw compiler forces usage of BMI instructions
#pragma push_macro("_M_IX86")
#undef _M_IX86
#pragma push_macro("_M_X64")
#undef _M_X64

#include "robin-hood-hashing/src/include/robin_hood.h"

#pragma pop_macro("_M_X64")
#pragma pop_macro("_M_IX86")

#endif
