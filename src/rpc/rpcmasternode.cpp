#include "masternode.h"
#include "bitcoinrpc.h"
#include "univalue.h"


UniValue masternodelist(const UniValue& params, bool fHelp)
{
    std::string strMode = "json";
    std::string strFilter = "";

    if (params.size() >= 1) strMode = params[0].get_str();
    if (params.size() == 2) strFilter = params[1].get_str();

    if (fHelp || (
                strMode != "activeseconds" && strMode != "addr" && strMode != "daemon" && strMode != "full" && strMode != "info" && strMode != "json" &&
                strMode != "lastseen" &&
                strMode != "protocol" && strMode != "payee" && strMode != "pubkey" &&
                strMode != "rank" && strMode != "status"))
    {
        throw std::runtime_error(
                "masternodelist ( \"mode\" \"filter\" )\n"
                "Get a list of masternodes in different modes\n"
                "\nArguments:\n"
                "1. \"mode\"      (string, optional/required to use filter, defaults = json) The mode to run list in\n"
                "2. \"filter\"    (string, optional) Filter results. Partial match by outpoint by default in all modes,\n"
                "                                    additional matches in some modes are also available\n"
                "\nAvailable modes:\n"
                "  activeseconds  - Print number of seconds masternode recognized by the network as enabled\n"
                "                   (since latest issued \"masternode start/start-many/start-alias\")\n"
                "  addr           - Print ip address associated with a masternode (can be additionally filtered, partial match)\n"
                "  daemon         - Print daemon version of a masternode (can be additionally filtered, exact match)\n"
                "  full           - Print info in format 'status protocol payee lastseen activeseconds IP'\n"
                "                   (can be additionally filtered, partial match)\n"
                "  info           - Print info in format 'status protocol payee lastseen activeseconds IP'\n"
                "                   (can be additionally filtered, partial match)\n"
                "  json           - Print info in JSON format (can be additionally filtered, partial match)\n"
                "  lastseen       - Print timestamp of when a masternode was last seen on the network\n"
                "  payee          - Print Dash address associated with a masternode (can be additionally filtered,\n"
                "                   partial match)\n"
                "  protocol       - Print protocol of a masternode (can be additionally filtered, exact match)\n"
                "  pubkey         - Print the masternode (not collateral) public key\n"
                "  rank           - Print rank of a masternode based on current block\n"
                "  status         - Print masternode status: PRE_ENABLED / ENABLED / EXPIRED / NEW_START_REQUIRED /\n"
                "                   UPDATE_REQUIRED / POSE_BAN / OUTPOINT_SPENT (can be additionally filtered, partial match)\n"
                );
    }

    // NTRN TODO: implement mnodeman.UpdateLastPaid(pindex)
    // NTRN TODO: implement mnodeman.UpdateLastPaid(pindex)
    // NTRN TODO: implement mnodeman.UpdateLastPaid(pindex)

    // NTRN TODO: implement mn.lastPing and mn.lastPing.nDaemonVersion
    // NTRN TODO: implement mn.lastPing and mn.lastPing.nDaemonVersion
    // NTRN TODO: implement mn.lastPing and mn.lastPing.nDaemonVersion

    // NTRN TODO: rename mn.pubkey to mn.pubKeyCollateralAddress
    // NTRN TODO: rename mn.pubkey to mn.pubKeyCollateralAddress
    // NTRN TODO: rename mn.pubkey to mn.pubKeyCollateralAddress

    UniValue obj(UniValue::VOBJ);
    if (strMode == "rank") {
        BOOST_FOREACH(CMasternode mn, vecMasternodes) {
            mn.Check();
            obj.push_back(Pair(mn.addr.ToString().c_str(), (int)(GetMasternodeRank(mn.vin, pindexBest->nHeight))));
        }
    } else {
        BOOST_FOREACH(CMasternode mn, vecMasternodes) {
            std::string strOutpoint = mn.addr.ToString().c_str();
            if (strMode == "activeseconds") {
                if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, (int64_t)(mn.lastTimeSeen - mn.now)));
            } else if (strMode == "addr") {
                std::string strAddress = mn.addr.ToString();
                if (strFilter !="" && strAddress.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strAddress));
            } else if (strMode == "daemon") {
                std::string strDaemon = "Unknown";
                if (strFilter !="" && strDaemon.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strDaemon));
            } else if (strMode == "full") {
                std::ostringstream streamFull;
                streamFull << std::setw(18) <<
                               mn.GetStatus() << " " <<
                               mn.protocolVersion << " " <<
                               CBitcoinAddress(mn.pubkey.GetID()).ToString() << " " <<
                               (int64_t)mn.lastTimeSeen << " " << std::setw(8) <<
                               (int64_t)(mn.lastTimeSeen - mn.now) << " " << std::setw(10) <<
                               mn.addr.ToString();
                std::string strFull = streamFull.str();
                if (strFilter !="" && strFull.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strFull));
            } else if (strMode == "info") {
                std::ostringstream streamInfo;
                streamInfo << std::setw(18) <<
                               mn.GetStatus() << " " <<
                               mn.protocolVersion << " " <<
                               CBitcoinAddress(mn.pubkey.GetID()).ToString() << " " <<
                               (int64_t)mn.lastTimeSeen << " " << std::setw(8) <<
                               (int64_t)(mn.lastTimeSeen - mn.now) << " " <<
                               mn.addr.ToString();
                std::string strInfo = streamInfo.str();
                if (strFilter !="" && strInfo.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strInfo));
            } else if (strMode == "json") {
                std::ostringstream streamInfo;
                streamInfo <<  mn.addr.ToString() << " " <<
                               CBitcoinAddress(mn.pubkey.GetID()).ToString() << " " <<
                               mn.GetStatus() << " " <<
                               mn.protocolVersion << " " <<
                               (int64_t)mn.lastTimeSeen << " " <<
                               (int64_t)(mn.lastTimeSeen - mn.now) << " ";
                std::string strInfo = streamInfo.str();
                if (strFilter !="" && strInfo.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                UniValue objMN(UniValue::VOBJ);
                objMN.push_back(Pair("address", mn.addr.ToString()));
                objMN.push_back(Pair("payee", CBitcoinAddress(mn.pubkey.GetID()).ToString()));
                objMN.push_back(Pair("status", mn.GetStatus()));
                objMN.push_back(Pair("protocol", mn.protocolVersion));
                objMN.push_back(Pair("daemonversion", "Unknown"));
                objMN.push_back(Pair("lastseen", (int64_t)mn.lastTimeSeen));
                objMN.push_back(Pair("activeseconds", (int64_t)(mn.lastTimeSeen - mn.now)));
                obj.push_back(Pair(strOutpoint, objMN));
            } else if (strMode == "lastseen") {
                if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, (int64_t)mn.lastTimeSeen));
            } else if (strMode == "payee") {
                CBitcoinAddress address(mn.pubkey.GetID());
                std::string strPayee = address.ToString();
                if (strFilter !="" && strPayee.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strPayee));
            } else if (strMode == "protocol") {
                if (strFilter !="" && strFilter != strprintf("%d", mn.protocolVersion) &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, mn.protocolVersion));
            } else if (strMode == "pubkey") {
                if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, CBitcoinAddress(mn.pubkey.GetID()).ToString().c_str()));
            } else if (strMode == "status") {
                std::string strStatus = mn.GetStatus();
                if (strFilter !="" && strStatus.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strStatus));
            }
        }
    }
    return obj;
}

UniValue masternodecount(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "masternodecount\n"
            "Returns the synced number of MNs on the Network.");

    return (int)vecMasternodes.size();;
}

static const CRPCCommand commands[] =
{ //  name                      actor (function)         okSafeMode  unlocked
  //  ------------------------  -----------------------  ----------  --------
    { "masternodelist",         &masternodelist,         true,       true },
    { "masternodecount",        &masternodecount,        true,       true },
};

void RegisterMasternodeRPCCommands(CRPCTable &t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
