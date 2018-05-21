// Copyright (c) 2009-2012 Bitcoin Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "net.h"
#include "bitcoinrpc.h"
#include "alert.h"
#include "wallet.h"
#include "db.h"
#include "walletdb.h"
#include "spork.h"

using namespace json_spirit;
using namespace std;

Value getconnectioncount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getconnectioncount\n"
            "Returns the number of connections to other nodes.");

    LOCK(cs_vNodes);
    return (int)vNodes.size();
}

static void CopyNodeStats(std::vector<CNodeStats>& vstats)
{
    vstats.clear();

    LOCK(cs_vNodes);
    vstats.reserve(vNodes.size());
    BOOST_FOREACH(CNode* pnode, vNodes) {
        CNodeStats stats;
        pnode->copyStats(stats);
        vstats.push_back(stats);
    }
}

Value getpeerinfo(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getpeerinfo\n"
            "Returns data about each connected network node as a json array of objects.");

    vector<CNodeStats> vstats;
    CopyNodeStats(vstats);

    Array ret;

    BOOST_FOREACH(const CNodeStats& stats, vstats) {
        Object obj;

        obj.push_back(Pair("id", stats.nodeid));
        obj.push_back(Pair("addr", stats.addrName));
        obj.push_back(Pair("services", strprintf("%016x", stats.nServices)));
        obj.push_back(Pair("lastsend", (int64_t)stats.nLastSend));
        obj.push_back(Pair("lastrecv", (int64_t)stats.nLastRecv));
        obj.push_back(Pair("conntime", stats.nTimeConnected));
        obj.push_back(Pair("version", stats.nVersion));
        obj.push_back(Pair("subver", stats.strSubVer));
        obj.push_back(Pair("inbound", stats.fInbound));
        obj.push_back(Pair("startingheight", stats.nStartingHeight));
        obj.push_back(Pair("banscore", stats.nMisbehavior));

        ret.push_back(obj);
    }

    return ret;
}

Value addnode(const Array& params, bool fHelp)
{
    string strCommand;
    if (params.size() == 2)
        strCommand = params[1].get_str();
    if (fHelp || params.size() != 2 ||
        (strCommand != "onetry" && strCommand != "add" && strCommand != "remove"))
        throw runtime_error(
            "addnode \"node\" \"add|remove|onetry\"\n"
            "\nAttempts add or remove a node from the addnode list.\n"
            "Or try a connection to a node once.\n"
            "\nArguments:\n"
            "1. \"node\"     (string, required) The node (see getpeerinfo for nodes)\n"
            "2. \"command\"  (string, required) 'add' to add a node to the list, 'remove' to remove a node from the list, 'onetry' to try a connection to the node once\n"
            "\nExamples:\n"
            + HelpExampleCli("addnode", "\"192.168.0.6:32001\" \"onetry\"")
            + HelpExampleRpc("addnode", "\"192.168.0.6:32001\", \"onetry\"")
        );

    string strNode = params[0].get_str();

    if (strCommand == "onetry")
    {
        CAddress addr;
        ConnectNode(addr, strNode.c_str());
        return Value::null;
    }

    CNode* connectedNode = NULL;
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes) {
        if (strNode == pnode->addrName) {
            connectedNode = pnode;
        }
    }

    if (strCommand == "add")
    {
        if (connectedNode != NULL) {
            throw JSONRPCError(RPC_CLIENT_NODE_ALREADY_ADDED, "Error: Node already added");
        }

        CService addr = CService(strNode);
        if(ConnectNode((CAddress)addr, NULL, true)){
            LogPrintf("successfully connected to node\n");
        } else {
            LogPrintf("error connecting to node\n");
        }
    }
    else if(strCommand == "remove")
    {
        if (connectedNode == NULL) {
            throw JSONRPCError(RPC_CLIENT_NODE_NOT_ADDED, "Error: Node has not been added.");
        }

        connectedNode->CloseSocketDisconnect();
    }

    return Value::null;
}

Value disconnectnode(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "disconnectnode \"node\" \n"
            "\nImmediately disconnects from the specified node.\n"
            "\nArguments:\n"
            "1. \"node\"     (string, required) The node (see getpeerinfo for nodes)\n"
            "\nExamples:\n"
            + HelpExampleCli("disconnectnode", "\"192.168.0.6:32001\"")
            + HelpExampleRpc("disconnectnode", "\"192.168.0.6:32001\"")
        );

    // CService addr = CService(params[0].get_str());
    // CNode* pnode = FindNode(addr);
    CNode* pnode = FindNode(params[0].get_str());
    if (pnode == NULL)
        throw JSONRPCError(RPC_CLIENT_NODE_NOT_CONNECTED, "Node not found in connected nodes");

    pnode->CloseSocketDisconnect();

    return Value::null;
}

Value setban(const Array& params, bool fHelp)
{
    string strCommand;
    if (params.size() >= 2)
        strCommand = params[1].get_str();
    if (fHelp || params.size() < 2 ||
        (strCommand != "add" && strCommand != "remove"))
        throw runtime_error(
                            "setban \"ip(/netmask)\" \"add|remove\" (bantime) (absolute)\n"
                            "\nAttempts add or remove a IP/Subnet from the banned list.\n"
                            "\nArguments:\n"
                            "1. \"ip(/netmask)\" (string, required) The IP/Subnet (see getpeerinfo for nodes ip) with a optional netmask (default is /32 = single ip)\n"
                            "2. \"command\"      (string, required) 'add' to add a IP/Subnet to the list, 'remove' to remove a IP/Subnet from the list\n"
                            "3. \"bantime\"      (numeric, optional) time in seconds how long (or until when if [absolute] is set) the ip is banned (0 or empty means using the default time of 24h which can also be overwritten by the -bantime startup argument)\n"
                            "4. \"absolute\"     (boolean, optional) If set, the bantime must be a absolute timestamp in seconds since epoch (Jan 1 1970 GMT)\n"
                            "\nExamples:\n"
                            + HelpExampleCli("setban", "\"192.168.0.6\" \"add\" 86400")
                            + HelpExampleCli("setban", "\"192.168.0.0/24\" \"add\"")
                            + HelpExampleRpc("setban", "\"192.168.0.6\", \"add\" 86400")
                            );

    // NTRN TODO - need to finish implementing this function
    throw runtime_error(
            "Not implemented yet\n");


    CSubNet subNet;
    CNetAddr netAddr;
    bool isSubnet = false;

    if (params[0].get_str().find("/") != string::npos)
        isSubnet = true;

    if (!isSubnet)
        netAddr = CNetAddr(params[0].get_str());
    else
        subNet = CSubNet(params[0].get_str());

    if (! (isSubnet ? subNet.IsValid() : netAddr.IsValid()) )
        throw JSONRPCError(RPC_CLIENT_NODE_ALREADY_ADDED, "Error: Invalid IP/Subnet");

    if (strCommand == "add")
    {
        // TODO
    }
    else if(strCommand == "remove")
    {
        // TODO
    }


    return Value::null;
}

Value listbanned(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
                            "listbanned\n"
                            "\nList all banned IPs/Subnets.\n"
                            "\nExamples:\n"
                            + HelpExampleCli("listbanned", "")
                            + HelpExampleRpc("listbanned", "")
                            );

    banmap_t banMap;
    CNode::GetBanned(banMap);

    Array bannedAddresses;
    for (banmap_t::iterator it = banMap.begin(); it != banMap.end(); it++)
    {
        Object obj;

        int64_t t = (*it).second;
        obj.push_back(Pair("address", (*it).first.ToString()));
        obj.push_back(Pair("banned_until", DateTimeStrFormat("%Y-%m-%d %H:%M:%S", t).c_str()));

        bannedAddresses.push_back(obj);
    }

    return bannedAddresses;
}

Value clearbanned(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
                            "clearbanned\n"
                            "\nClear all banned IPs.\n"
                            "\nExamples:\n"
                            + HelpExampleCli("clearbanned", "")
                            + HelpExampleRpc("clearbanned", "")
                            );

    CNode::ClearBanned();
    // NTRN TODO - store banlist to disk

    return Value::null;
}

// ppcoin: send alert.
// There is a known deadlock situation with ThreadMessageHandler
// ThreadMessageHandler: holds cs_vSend and acquiring cs_main in SendMessages()
// ThreadRPCServer: holds cs_main and acquiring cs_vSend in alert.RelayTo()/PushMessage()/BeginMessage()
Value sendalert(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 6)
        throw runtime_error(
            "sendalert <message> <privatekey> <minver> <maxver> <priority> <id> [cancelupto]\n"
            "<message> is the alert text message\n"
            "<privatekey> is hex string of alert master private key\n"
            "<minver> is the minimum applicable internal client version\n"
            "<maxver> is the maximum applicable internal client version\n"
            "<priority> is integer priority number\n"
            "<id> is the alert id\n"
            "[cancelupto] cancels all alert id's up to this number\n"
            "Returns true or false.");

    CAlert alert;
    CKey key;

    alert.strStatusBar = params[0].get_str();
    alert.nMinVer = params[2].get_int();
    alert.nMaxVer = params[3].get_int();
    alert.nPriority = params[4].get_int();
    alert.nID = params[5].get_int();
    if (params.size() > 6)
        alert.nCancel = params[6].get_int();
    alert.nVersion = PROTOCOL_VERSION;
    alert.nRelayUntil = GetAdjustedTime() + 365*24*60*60;
    alert.nExpiration = GetAdjustedTime() + 365*24*60*60;

    CDataStream sMsg(SER_NETWORK, PROTOCOL_VERSION);
    sMsg << (CUnsignedAlert)alert;
    alert.vchMsg = vector<unsigned char>(sMsg.begin(), sMsg.end());

    vector<unsigned char> vchPrivKey = ParseHex(params[1].get_str());
    key.SetPrivKey(CPrivKey(vchPrivKey.begin(), vchPrivKey.end())); // if key is not correct openssl may crash
    if (!key.Sign(Hash(alert.vchMsg.begin(), alert.vchMsg.end()), alert.vchSig))
        throw runtime_error(
            "Unable to sign alert, check private key?\n");
    if(!alert.ProcessAlert())
        throw runtime_error(
            "Failed to process alert.\n");
    // Relay alert
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            alert.RelayTo(pnode);
    }

    Object result;
    result.push_back(Pair("strStatusBar", alert.strStatusBar));
    result.push_back(Pair("nVersion", alert.nVersion));
    result.push_back(Pair("nMinVer", alert.nMinVer));
    result.push_back(Pair("nMaxVer", alert.nMaxVer));
    result.push_back(Pair("nPriority", alert.nPriority));
    result.push_back(Pair("nID", alert.nID));
    if (alert.nCancel > 0)
        result.push_back(Pair("nCancel", alert.nCancel));
    return result;
}

/*
+    Used for updating/reading spork settings on the network
+*/
Value spork(const Array& params, bool fHelp)
{
    if (params.size() == 1 && params[0].get_str() == "show") {
        Object ret;
        for (int nSporkID = SPORK_START; nSporkID <= SPORK_END; nSporkID++) {
            if (sporkManager.GetSporkNameByID(nSporkID) != "Unknown")
                ret.push_back(Pair(sporkManager.GetSporkNameByID(nSporkID), sporkManager.GetSporkValue(nSporkID)));
        }
        return ret;
    } else if (params.size() == 1 && params[0].get_str() == "active") {
        Object ret;
        for (int nSporkID = SPORK_START; nSporkID <= SPORK_END; nSporkID++) {
            if (sporkManager.GetSporkNameByID(nSporkID) != "Unknown")
                ret.push_back(Pair(sporkManager.GetSporkNameByID(nSporkID), sporkManager.IsSporkActive(nSporkID)));
        }
        return ret;
    } else if (params.size() == 2) {
        int nSporkID = sporkManager.GetSporkIDByName(params[0].get_str());
        if (nSporkID == -1) {
            return "Invalid spork name";
        }

        // SPORK VALUE
        int64_t nValue = boost::lexical_cast<int64_t>(params[1].get_str());

        //broadcast new spork
        if (sporkManager.UpdateSpork(nSporkID, nValue)) {
            sporkManager.ExecuteSpork(nSporkID, nValue);
            return "success";
        } else {
            return "failure";
        }
    }

    throw runtime_error(
        "spork <name> [<value>]\n"
        "<name> is the corresponding spork name, or 'show' to show all current spork settings, active to show which sporks are active"
        "<value> is a epoch datetime to enable or disable spork" +
        HelpRequiringPassphrase());
}
