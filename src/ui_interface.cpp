// Copyright (c) 2010-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <ui_interface.h>
#include <util.h>

#include <boost/signals2/last_value.hpp>
#include <boost/signals2/signal.hpp>

CClientUIInterface uiInterface;

/**
 * Translation function: Call Translate signal on UI interface, which returns a boost::optional result.
 * If no translation slot is registered, nothing is returned, and simply return the input.
 */
std::string _(const char* psz)
{
    boost::optional<std::string> rv = uiInterface.Translate(psz);
    return rv ? (*rv) : psz;
}

bool InitError(const std::string &str)
{
    LogPrintf("%s\n", str.c_str());
    uiInterface.InitMessage(str);
    uiInterface.ThreadSafeMessageBox(str, _("Neutron"), CClientUIInterface::OK | CClientUIInterface::MODAL);
    return false;
}

void InitWarning(const std::string &str)
{
    uiInterface.ThreadSafeMessageBox(str, _("Neutron"), CClientUIInterface::OK | CClientUIInterface::ICON_EXCLAMATION | CClientUIInterface::MODAL);
}
