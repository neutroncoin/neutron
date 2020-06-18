// Copyright (c) 2017-2020 The Swipp developers
// Copyright (c) 2020 The Neutron Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <stdlib.h>
#ifdef __linux__
#include <execinfo.h>
#endif

#include "backtrace.h"
#include "util.h"

#define BACKTRACE_SIZE 20

void Backtrace::output()
{
#ifdef __linux__
    //if (fDebugBacktrace)
    {
        void *bt[BACKTRACE_SIZE];
        size_t size = backtrace(bt, BACKTRACE_SIZE);
        char **symbols;

        if ((symbols = backtrace_symbols(bt, size)) == NULL)
        {
            LogPrintf("Backtrace::output() : failed\n");
            exit(1);
        }

        LogPrintf("Backtrace::output() : \n");

        for(unsigned int i = 0; i < size; i++)
            LogPrintf("  %d: %X (%s)\n", i, bt[i], symbols[i]);

        free(symbols);
    }
#endif
}
