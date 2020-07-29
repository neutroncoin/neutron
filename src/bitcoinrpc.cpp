// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "bitcoinrpc.h"
#include "base58.h"
#include "db.h"
#include "init.h"
#include "sync.h"
#include "ui_interface.h"
#include "util.h"
#include "utiltime.h"

#include <boost/asio.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/foreach.hpp>
#include <boost/iostreams/concepts.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/scope_exit.hpp>
#include <boost/shared_ptr.hpp>
#include <list>
#include <unordered_map>
#include <boost/algorithm/string/case_conv.hpp>

using namespace std;
using namespace boost;
using namespace boost::asio;

static bool fRPCRunning = false;
void ThreadRPCServer2(void* parg);
static std::string strRPCUserColonPass;
const json_spirit::Object emptyobj;
void ThreadRPCServer3(void* parg);

static inline unsigned short GetDefaultRPCPort()
{
    return GetBoolArg("-testnet", false) ? 25715 : 32000;
}

UniValue JSONRPCRequestObj(const std::string& strMethod, const UniValue& params, const UniValue& id)
{
    UniValue request(UniValue::VOBJ);

    request.push_back(Pair("method", strMethod));
    request.push_back(Pair("params", params));
    request.push_back(Pair("id", id));

    return request;
}

UniValue JSONRPCReplyObj(const UniValue& result, const UniValue& error, const UniValue& id)
{
    UniValue reply(UniValue::VOBJ);

    if (!error.isNull())
        reply.push_back(Pair("result", NullUniValue));
    else
        reply.push_back(Pair("result", result));

    reply.push_back(Pair("error", error));
    reply.push_back(Pair("id", id));

    return reply;
}

std::string JSONRPCReply(const UniValue& result, const UniValue& error, const UniValue& id)
{
    UniValue reply = JSONRPCReplyObj(result, error, id);
    return reply.write() + "\n";
}

UniValue JSONRPCError(int code, const std::string& message)
{
    UniValue error(UniValue::VOBJ);

    error.push_back(Pair("code", code));
    error.push_back(Pair("message", message));

    return error;
}

class JSONRequest
{
public:
    json_spirit::Value id;
    string strMethod;
    json_spirit::Array params;

    JSONRequest() { id = json_spirit::Value::null; }
    void parse(const json_spirit::Value& valRequest);
};

void JSONRequest::parse(const json_spirit::Value& valRequest)
{
    if (valRequest.type() != json_spirit::obj_type)
        throw JSONRPCError(RPC_INVALID_REQUEST, "Invalid Request object");

    const json_spirit::Object& request = valRequest.get_obj();

    id = find_value(request, "id");
    json_spirit::Value valMethod = find_value(request, "method");

    if (valMethod.type() == json_spirit::null_type)
        throw JSONRPCError(RPC_INVALID_REQUEST, "Missing method");

    if (valMethod.type() != json_spirit::str_type)
        throw JSONRPCError(RPC_INVALID_REQUEST, "Method must be a string");

    strMethod = valMethod.get_str();

    if (strMethod != "getwork" && strMethod != "getblocktemplate")
        LogPrintf("ThreadRPCServer method=%s\n", strMethod.c_str());

    json_spirit::Value valParams = find_value(request, "params");

    if (valParams.type() == json_spirit::array_type)
        params = valParams.get_array();
    else if (valParams.type() == json_spirit::null_type)
        params = json_spirit::Array();
    else
        throw JSONRPCError(RPC_INVALID_REQUEST, "Params must be an array");
}

bool IsRPCRunning()
{
    return fRPCRunning;
}

void RPCTypeCheck(const UniValue& params, const std::list<UniValue::VType>& typesExpected, bool fAllowNull)
{
    unsigned int i = 0;

    BOOST_FOREACH(UniValue::VType t, typesExpected)
    {
        if (params.size() <= i)
            break;

        const UniValue& v = params[i];

        if (!(fAllowNull && v.isNull()))
            RPCTypeCheckArgument(v, t);

        i++;
    }
}

void RPCTypeCheckArgument(const UniValue& value, UniValue::VType typeExpected)
{
    if (value.type() != typeExpected)
        throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Expected type %s, got %s", uvTypeName(typeExpected), uvTypeName(value.type())));
}

void RPCTypeCheckObj(const UniValue& o, const std::map<std::string, UniValueType>& typesExpected,
                     bool fAllowNull, bool fStrict)
{
    for (const auto& t : typesExpected)
    {
        const UniValue& v = find_value(o, t.first);

        if (!fAllowNull && v.isNull())
            throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Missing %s", t.first));

        if (!(t.second.typeAny || v.type() == t.second.type || (fAllowNull && v.isNull())))
        {
            std::string err = strprintf("Expected type %s for %s, got %s",
                                        uvTypeName(t.second.type), t.first, uvTypeName(v.type()));
            throw JSONRPCError(RPC_TYPE_ERROR, err);
        }
    }

    if (fStrict)
    {
        BOOST_FOREACH(const std::string& k, o.getKeys())
        {
            if (typesExpected.count(k) == 0)
            {
                std::string err = strprintf("Unexpected key %s", k);
                throw JSONRPCError(RPC_TYPE_ERROR, err);
            }
        }
    }
}

static inline int64_t roundint64(double d)
{
    return (int64_t)(d > 0 ? d + 0.5 : d - 0.5);
}

CAmount AmountFromValue(const UniValue& value)
{
    if (!value.isNum())
        throw JSONRPCError(RPC_TYPE_ERROR, "Amount is not a number");

    double dAmount = value.get_real();

    if (dAmount <= 0.0 || dAmount > MAX_MONEY)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");

    CAmount nAmount = roundint64(dAmount * COIN);

    if (!MoneyRange(nAmount))
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");

    return nAmount;
}

UniValue ValueFromAmount(const CAmount& amount)
{
    bool sign = amount < 0;
    int64_t n_abs = (sign ? -amount : amount);
    int64_t quotient = n_abs / COIN;
    int64_t remainder = n_abs % COIN;

    return UniValue(UniValue::VNUM, strprintf("%s%d.%08d", sign ? "-" : "", quotient, remainder));
}

std::string HexBits(unsigned int nBits)
{
    union
    {
        int32_t nBits;
        char cBits[4];
    } uBits;

    uBits.nBits = htonl((int32_t)nBits);
    return HexStr(BEGIN(uBits.cBits), END(uBits.cBits));
}

// Utilities: convert hex-encoded Values
// (throws error if not hex).

uint256 ParseHashV(const UniValue& v, string strName)
{
    string strHex;

    if (v.isStr())
        strHex = v.get_str();

    if (!IsHex(strHex)) // NOTE: IsHex("") is false
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName+" must be hexadecimal string (not '"+strHex+"')");

    uint256 result;
    result.SetHex(strHex);
    return result;
}

uint256 ParseHashO(const UniValue& o, string strKey)
{
    return ParseHashV(find_value(o, strKey), strKey);
}

vector<unsigned char> ParseHexV(const UniValue& v, string strName)
{
    string strHex;

    if (v.isStr())
        strHex = v.get_str();

    if (!IsHex(strHex))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName+" must be hexadecimal string (not '"+strHex+"')");

    return ParseHex(strHex);
}

vector<unsigned char> ParseHexO(const UniValue& o, string strKey)
{
    return ParseHexV(find_value(o, strKey), strKey);
}

string CRPCTable::help(string strCommand) const
{
    string strRet;
    set<rpcfn_type> setDone;

    for (map<string, const CRPCCommand*>::const_iterator mi = mapCommands.begin();
         mi != mapCommands.end(); ++mi)
    {
        const CRPCCommand *pcmd = mi->second;
        string strMethod = mi->first;

        // We already filter duplicates, but these deprecated screw up the sort order
        if (strMethod.find("label") != string::npos)
            continue;

        if (strCommand != "" && strMethod != strCommand)
            continue;

        try
        {
            UniValue params;
            rpcfn_type pfn = pcmd->actor;

            if (setDone.insert(pfn).second)
                (*pfn)(params, true);
        }
        catch (std::exception& e)
        {
            // Help text is returned in an exception
            string strHelp = string(e.what());

            if (strCommand == "")
            {
                if (strHelp.find('\n') != string::npos)
                    strHelp = strHelp.substr(0, strHelp.find('\n'));
            }

            strRet += strHelp + "\n";
        }
    }

    if (strRet == "")
        strRet = strprintf("help: unknown command: %s\n", strCommand.c_str());

    strRet = strRet.substr(0,strRet.size() -  1);
    return strRet;
}

UniValue debug(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
    {
        throw runtime_error("debug <enabled>\n"
                            "<enabled> is true or false and controls if debug messages should be enabled.");
    }

    fDebug = params[0].get_bool();
    return true;
}

UniValue help(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error("help [command]\nList commands, or get help for a command.");

    string strCommand;

    if (params.size() > 0)
        strCommand = params[0].get_str();

    return tableRPC.help(strCommand);
}

UniValue stop(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
    {
        throw runtime_error("stop <detach>\n"
                            "<detach> is true or false to detach the database or not for this stop only\n"
                            "Stop Neutron server (and possibly override the detachdb config value).");
    }

    // Shutdown will take long enough that the response should get back
    if (params.size() > 0)
        bitdb.SetDetach(params[0].get_bool());

    StartShutdown();
    return "Neutron server stopping";
}

static const CRPCCommand vRPCCommands[] =
{ //  name                      actor (function)         okSafeMode  unlocked
  //  ------------------------  -----------------------  ----------  --------
    /* Overall control/query calls */
    { "getinfo",                &getinfo,                true,       false },
    { "getdebuginfo",           &getdebuginfo,           true,       false },
    { "debug",                  &debug,                  true,       true },
    { "help",                   &help,                   true,       true },
    { "stop",                   &stop,                   true,       true },

    /* P2P networking */
    { "addnode",                &addnode,                true,       false },
    { "disconnectnode",         &disconnectnode,         true,       false },
    { "getconnectioncount",     &getconnectioncount,     true,       false },
    { "getpeerinfo",            &getpeerinfo,            true,       false },
    { "setban",                 &setban,                 true,       false },
    { "listbanned",             &listbanned,             true,       false },
    { "clearbanned",            &clearbanned,            true,       false },

    /* Block chain and UTXO */
    { "getbestblockhash",       &getbestblockhash,       true,       false },
    { "getblockcount",          &getblockcount,          true,       false },
    { "getblock",               &getblock,               true,       false },
    { "getblockhash",           &getblockhash,           true,       false },
    { "getdifficulty",          &getdifficulty,          true,       false },
    { "getrawmempool",          &getrawmempool,          true,       false },

    /* Mining */
    { "getblocktemplate",       &getblocktemplate,       true,       false },
    { "getmininginfo",          &getmininginfo,          true,       false },
    { "getstakinginfo",         &getstakinginfo,         true,       false },
    { "submitblock",            &submitblock,            false,      false },
    { "reservebalance",         &reservebalance,         false,      true },

    /* Coin generation */
    { "setgenerate",            &setgenerate,            true,       false },

    /* Raw transactions */
    { "createrawtransaction",   &createrawtransaction,   false,      false },
    { "decoderawtransaction",   &decoderawtransaction,   false,      false },
    { "decodescript",           &decodescript,           false,      false },
    { "getrawtransaction",      &getrawtransaction,      false,      false },
    { "sendrawtransaction",     &sendrawtransaction,     false,      false },
    { "signrawtransaction",     &signrawtransaction,     false,      false },

    /* Utility functions */
    { "validateaddress",        &validateaddress,        true,       false },
    { "verifymessage",          &verifymessage,          true,       false },

    /* Neutron features */
    { "masternode",             &masternode,             true,       true },
    { "spork",                  &spork,                  true,       false },
    { "getminingreport",        &getminingreport,        false,      false},

    /* Wallet */
    { "addmultisigaddress",     &addmultisigaddress,     false,      false },
    { "backupwallet",           &backupwallet,           true,       false },
    { "dumpprivkey",            &dumpprivkey,            false,      false },
    { "dumpwallet",             &dumpwallet,             true,       false },
    { "encryptwallet",          &encryptwallet,          false,      false },
    { "getaccountaddress",      &getaccountaddress,      true,       false },
    { "getaccount",             &getaccount,             false,      false },
    { "getaddressesbyaccount",  &getaddressesbyaccount,  true,       false },
    { "getbalance",             &getbalance,             false,      false },
    { "getnewaddress",          &getnewaddress,          true,       false },
    { "getreceivedbyaccount",   &getreceivedbyaccount,   false,      false },
    { "getreceivedbyaddress",   &getreceivedbyaddress,   false,      false },
    { "gettransaction",         &gettransaction,         false,      false },
    { "importprivkey",          &importprivkey,          false,      false },
    { "importwallet",           &importwallet,           false,      false },
    { "keypoolrefill",          &keypoolrefill,          true,       false },
    { "listaccounts",           &listaccounts,           false,      false },
    { "listaddressgroupings",   &listaddressgroupings,   false,      false },
    { "listlockunspent",        &listlockunspent,        false,      false },
    { "listreceivedbyaccount",  &listreceivedbyaccount,  false,      false },
    { "listreceivedbyaddress",  &listreceivedbyaddress,  false,      false },
    { "listsinceblock",         &listsinceblock,         false,      false },
    { "listtransactions",       &listtransactions,       false,      false },
    { "listunspent",            &listunspent,            false,      false },
    { "lockunspent",            &lockunspent,            false,      false },
    { "move",                   &movecmd,                false,      false },
    { "sendfrom",               &sendfrom,               false,      false },
    { "sendmany",               &sendmany,               false,      false },
    { "sendtoaddress",          &sendtoaddress,          false,      false },
    { "setaccount",             &setaccount,             true,       false },
    { "settxfee",               &settxfee,               false,      false },
    { "signmessage",            &signmessage,            false,      false },
    { "walletlock",             &walletlock,             true,       false },
    { "walletpassphrasechange", &walletpassphrasechange, false,      false },
    { "walletpassphrase",       &walletpassphrase,       true,       false },

    // TODO: NTRN - still need to categorize
    { "addredeemscript",        &addredeemscript,        false,      false },
    { "checkwallet",            &checkwallet,            false,      true },
    { "getblockbynumber",       &getblockbynumber,       false,      false },
    { "getblockbyrange",        &getblockbyrange,        false,      false },
    { "getblockversionstats",   &getblockversionstats,   true,       false },
    { "getcheckpoint",          &getcheckpoint,          true,       false },
    { "gethashespersec",        &gethashespersec,        true,       false },
    { "getnewpubkey",           &getnewpubkey,           true,       false },
    { "getsubsidy",             &getsubsidy,             true,       false },
    { "getwork",                &getwork,                true,       false },
    { "getworkex",              &getworkex,              true,       false },
    { "makekeypair",            &makekeypair,            false,      true },
    { "repairwallet",           &repairwallet,           false,      true },
    { "resendtx",               &resendtx,               false,      true },
    { "sendalert",              &sendalert,              false,      false },
    { "validatepubkey",         &validatepubkey,         true,       false },
    { "invalidateblock",        &invalidateblock,        true,       false },
};

CRPCTable::CRPCTable()
{
    unsigned int vcidx;

    for (vcidx = 0; vcidx < (sizeof(vRPCCommands) / sizeof(vRPCCommands[0])); vcidx++)
    {
        const CRPCCommand *pcmd;
        pcmd = &vRPCCommands[vcidx];
        mapCommands[pcmd->name] = pcmd;
    }
}

const CRPCCommand *CRPCTable::operator[](string name) const
{
    map<string, const CRPCCommand*>::const_iterator it = mapCommands.find(name);

    if (it == mapCommands.end())
        return NULL;

    return (*it).second;
}

// HTTP protocol
//
// This ain't Apache.  We're just using HTTP header for the length field
// and to be compatible with other JSON-RPC implementations.

string HTTPPost(const string& strMsg, const map<string,string>& mapRequestHeaders)
{
    ostringstream s;

    s << "POST / HTTP/1.1\r\n"
      << "User-Agent: Neutron-json-rpc/" << FormatFullVersion() << "\r\n"
      << "Host: 127.0.0.1\r\n"
      << "Content-Type: application/json\r\n"
      << "Content-Length: " << strMsg.size() << "\r\n"
      << "Connection: close\r\n"
      << "Accept: application/json\r\n";

    BOOST_FOREACH(const PAIRTYPE(string, string)& item, mapRequestHeaders)
        s << item.first << ": " << item.second << "\r\n";

    s << "\r\n" << strMsg;
    return s.str();
}

string rfc1123Time()
{
    char buffer[64];
    time_t now;
    time(&now);
    struct tm* now_gmt = gmtime(&now);
    string locale(setlocale(LC_TIME, NULL));

    setlocale(LC_TIME, "C"); // we want POSIX (aka "C") weekday/month strings
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S +0000", now_gmt);
    setlocale(LC_TIME, locale.c_str());

    return string(buffer);
}

static string HTTPReply(int nStatus, const string& strMsg, bool keepalive)
{
    if (nStatus == HTTP_UNAUTHORIZED)
    {
        return strprintf("HTTP/1.0 401 Authorization Required\r\n"
                         "Date: %s\r\n"
                         "Server: Neutron-json-rpc/%s\r\n"
                         "WWW-Authenticate: Basic realm=\"jsonrpc\"\r\n"
                         "Content-Type: text/html\r\n"
                         "Content-Length: 296\r\n"
                         "\r\n"
                         "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\"\r\n"
                         "\"http://www.w3.org/TR/1999/REC-html401-19991224/loose.dtd\">\r\n"
                         "<HTML>\r\n"
                         "<HEAD>\r\n"
                         "<TITLE>Error</TITLE>\r\n"
                         "<META HTTP-EQUIV='Content-Type' CONTENT='text/html; charset=ISO-8859-1'>\r\n"
                         "</HEAD>\r\n"
                         "<BODY><H1>401 Unauthorized.</H1></BODY>\r\n"
                         "</HTML>\r\n", rfc1123Time().c_str(), FormatFullVersion().c_str());
    }

    const char *cStatus;

    if (nStatus == HTTP_OK)
        cStatus = "OK";
    else if (nStatus == HTTP_BAD_REQUEST)
        cStatus = "Bad Request";
    else if (nStatus == HTTP_FORBIDDEN)
        cStatus = "Forbidden";
    else if (nStatus == HTTP_NOT_FOUND)
        cStatus = "Not Found";
    else if (nStatus == HTTP_INTERNAL_SERVER_ERROR)
        cStatus = "Internal Server Error";
    else
        cStatus = "";

    return strprintf("HTTP/1.1 %d %s\r\n"
                     "Date: %s\r\n"
                     "Connection: %s\r\n"
                     "Content-Length: %u\r\n"
                     "Content-Type: application/json\r\n"
                     "Server: Neutron-json-rpc/%s\r\n"
                     "\r\n"
                     "%s",
                     nStatus, cStatus, rfc1123Time().c_str(), keepalive ? "keep-alive" : "close",
                     strMsg.size(), FormatFullVersion().c_str(), strMsg.c_str());
}

int ReadHTTPStatus(std::basic_istream<char>& stream, int &proto)
{
    string str;
    getline(stream, str);
    vector<string> vWords;
    boost::split(vWords, str, boost::is_any_of(" "));

    if (vWords.size() < 2)
        return HTTP_INTERNAL_SERVER_ERROR;

    proto = 0;
    const char *ver = strstr(str.c_str(), "HTTP/1.");

    if (ver != NULL)
        proto = atoi(ver+7);

    return atoi(vWords[1].c_str());
}

int ReadHTTPHeader(std::basic_istream<char>& stream, map<string, string>& mapHeadersRet)
{
    int nLen = 0;

    while (true)
    {
        string str;
        std::getline(stream, str);

        if (str.empty() || str == "\r")
            break;

        string::size_type nColon = str.find(":");

        if (nColon != string::npos)
        {
            string strHeader = str.substr(0, nColon);
            boost::trim(strHeader);
            boost::to_lower(strHeader);
            string strValue = str.substr(nColon + 1);

            boost::trim(strValue);
            mapHeadersRet[strHeader] = strValue;

            if (strHeader == "content-length")
                nLen = atoi(strValue.c_str());
        }
    }

    return nLen;
}

int ReadHTTP(std::basic_istream<char>& stream, map<string, string>& mapHeadersRet, string& strMessageRet)
{
    mapHeadersRet.clear();
    strMessageRet = "";

    int nProto = 0;
    int nStatus = ReadHTTPStatus(stream, nProto);
    int nLen = ReadHTTPHeader(stream, mapHeadersRet);

    if (nLen < 0 || nLen > (int)MAX_SIZE)
        return HTTP_INTERNAL_SERVER_ERROR;

    if (nLen > 0)
    {
        vector<char> vch(nLen);
        stream.read(&vch[0], nLen);
        strMessageRet = string(vch.begin(), vch.end());
    }

    string sConHdr = mapHeadersRet["connection"];

    if ((sConHdr != "close") && (sConHdr != "keep-alive"))
    {
        if (nProto >= 1)
            mapHeadersRet["connection"] = "keep-alive";
        else
            mapHeadersRet["connection"] = "close";
    }

    return nStatus;
}

bool HTTPAuthorized(map<string, string>& mapHeaders)
{
    string strAuth = mapHeaders["authorization"];

    if (strAuth.substr(0,6) != "Basic ")
        return false;

    string strUserPass64 = strAuth.substr(6); boost::trim(strUserPass64);
    string strUserPass = DecodeBase64(strUserPass64);

    return TimingResistantEqual(strUserPass, strRPCUserColonPass);
}

void JSONErrorReply(std::ostream& stream, const UniValue& objError, const UniValue& id)
{
    int nStatus = HTTP_INTERNAL_SERVER_ERROR;
    int code = find_value(objError, "code").get_int();

    if (code == RPC_INVALID_REQUEST)
        nStatus = HTTP_BAD_REQUEST;
    else if (code == RPC_METHOD_NOT_FOUND)
        nStatus = HTTP_NOT_FOUND;

    string strReply = JSONRPCReply(NullUniValue, objError, id);
    stream << HTTPReply(nStatus, strReply, false) << std::flush;
}

bool ClientAllowed(const boost::asio::ip::address& address)
{
    // Make sure that IPv4-compatible and IPv4-mapped IPv6 addresses are treated as IPv4 addresses
    if (address.is_v6() && (address.to_v6().is_v4_compatible() || address.to_v6().is_v4_mapped()))
        return ClientAllowed(address.to_v6().to_v4());

    // Loopback subnet check
    if (address == asio::ip::address_v4::loopback() || address == asio::ip::address_v6::loopback() ||
        (address.is_v4() && (address.to_v4().to_ulong() & 0xff000000) == 0x7f000000))
        return true;

    const string strAddress = address.to_string();
    const vector<string>& vAllow = mapMultiArgs["-rpcallowip"];

    BOOST_FOREACH(string strAllow, vAllow)
    {
        if (WildcardMatch(strAddress, strAllow))
            return true;
    }

    return false;
}

template<typename Protocol> class SSLIOStreamDevice : public iostreams::device<iostreams::bidirectional>
{
public:
    SSLIOStreamDevice(asio::ssl::stream<typename Protocol::socket> &streamIn, bool fUseSSLIn) : stream(streamIn)
    {
        fUseSSL = fUseSSLIn;
        fNeedHandshake = fUseSSLIn;
    }

    void handshake(ssl::stream_base::handshake_type role)
    {
        if (!fNeedHandshake)
            return;

        fNeedHandshake = false;
        stream.handshake(role);
    }

    std::streamsize read(char* s, std::streamsize n)
    {
        handshake(ssl::stream_base::server); // HTTPS servers read first

        if (fUseSSL)
            return stream.read_some(asio::buffer(s, n));

        return stream.next_layer().read_some(asio::buffer(s, n));
    }

    std::streamsize write(const char* s, std::streamsize n)
    {
        handshake(ssl::stream_base::client); // HTTPS clients write first

        if (fUseSSL)
            return asio::write(stream, asio::buffer(s, n));

        return asio::write(stream.next_layer(), asio::buffer(s, n));
    }

    bool connect(const std::string& server, const std::string& port)
    {
        ip::tcp::resolver resolver(stream.get_io_service());
        ip::tcp::resolver::query query(server.c_str(), port.c_str());
        ip::tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);
        ip::tcp::resolver::iterator end;
        boost::system::error_code error = asio::error::host_not_found;

        while (error && endpoint_iterator != end)
        {
            stream.lowest_layer().close();
            stream.lowest_layer().connect(*endpoint_iterator++, error);
        }

        if (error)
            return false;

        return true;
    }

private:
    bool fNeedHandshake;
    bool fUseSSL;
    asio::ssl::stream<typename Protocol::socket>& stream;
};

class AcceptedConnection
{
public:
    virtual ~AcceptedConnection() {}

    virtual std::iostream& stream() = 0;
    virtual std::string peer_address_to_string() const = 0;
    virtual void close() = 0;
};

template<typename Protocol> class AcceptedConnectionImpl : public AcceptedConnection
{
public:
    AcceptedConnectionImpl(asio::io_service& io_service, ssl::context &context, bool fUseSSL) :
        sslStream(io_service, context), _d(sslStream, fUseSSL), _stream(_d) { /* Intentionally left empty */ }

    virtual std::iostream& stream()
    {
        return _stream;
    }

    virtual std::string peer_address_to_string() const
    {
        return peer.address().to_string();
    }

    virtual void close()
    {
        _stream.close();
    }

    typename Protocol::endpoint peer;
    asio::ssl::stream<typename Protocol::socket> sslStream;

private:
    SSLIOStreamDevice<Protocol> _d;
    iostreams::stream< SSLIOStreamDevice<Protocol> > _stream;
};

void ThreadRPCServer(void* parg)
{
    RenameThread("Neutron-rpclist");
    fRPCRunning = true;

    try
    {
        vnThreadsRunning[THREAD_RPCLISTENER]++;
        ThreadRPCServer2(parg);
        vnThreadsRunning[THREAD_RPCLISTENER]--;
    }
    catch (std::exception& e)
    {
        vnThreadsRunning[THREAD_RPCLISTENER]--;
        PrintException(&e, "ThreadRPCServer()");
    }
    catch (...)
    {
        vnThreadsRunning[THREAD_RPCLISTENER]--;
        PrintException(NULL, "ThreadRPCServer()");
    }

    fRPCRunning = false;
    LogPrintf("ThreadRPCServer exited\n");
}

// Forward declaration required for RPCListen
template <typename Protocol>
static void RPCAcceptHandler(boost::shared_ptr< basic_socket_acceptor<Protocol> > acceptor, ssl::context& context,
                             bool fUseSSL, AcceptedConnection* conn, const boost::system::error_code& error);

// Sets up I/O resources to accept and handle a new connection.
template <typename Protocol>
static void RPCListen(boost::shared_ptr< basic_socket_acceptor<Protocol> > acceptor,
                      ssl::context& context, const bool fUseSSL)
{
    // Accept connection
    AcceptedConnectionImpl<Protocol>* conn = new AcceptedConnectionImpl<Protocol>(acceptor->get_io_service(),
                                                                                  context, fUseSSL);
    acceptor->async_accept(conn->sslStream.lowest_layer(), conn->peer,
                           boost::bind(&RPCAcceptHandler<Protocol>, acceptor,
                           boost::ref(context), fUseSSL, conn,
                           boost::asio::placeholders::error));
}

// Accept and handle incoming connection.
template <typename Protocol>
static void RPCAcceptHandler(boost::shared_ptr< basic_socket_acceptor<Protocol> > acceptor,
                             ssl::context& context,
                             const bool fUseSSL,
                             AcceptedConnection* conn,
                             const boost::system::error_code& error)
{
    vnThreadsRunning[THREAD_RPCLISTENER]++;

    // Immediately start accepting new connections, except when we're cancelled or our socket is closed.
    if (error != asio::error::operation_aborted && acceptor->is_open())
        RPCListen(acceptor, context, fUseSSL);

    AcceptedConnectionImpl<ip::tcp>* tcp_conn = dynamic_cast< AcceptedConnectionImpl<ip::tcp>* >(conn);

    // TODO: Actually handle errors
    if (error)
    {
        delete conn;
    }
    // Restrict callers by IP.  It is important to do this before starting client thread, to filter out
    // certain DoS and misbehaving clients.
    else if (tcp_conn && !ClientAllowed(tcp_conn->peer.address()))
    {
        // Only send a 403 if we're not using SSL to prevent a DoS during the SSL handshake.
        if (!fUseSSL)
            conn->stream() << HTTPReply(HTTP_FORBIDDEN, "", false) << std::flush;

        delete conn;
    }
    // start HTTP client thread
    else if (!NewThread(ThreadRPCServer3, conn))
    {
        LogPrintf("Failed to create RPC server client thread\n");
        delete conn;
    }

    vnThreadsRunning[THREAD_RPCLISTENER]--;
}

void ThreadRPCServer2(void* parg)
{
    LogPrintf("ThreadRPCServer started\n");

    strRPCUserColonPass = mapArgs["-rpcuser"] + ":" + mapArgs["-rpcpassword"];
    if ((mapArgs["-rpcpassword"] == "") ||
        (mapArgs["-rpcuser"] == mapArgs["-rpcpassword"]))
    {
        unsigned char rand_pwd[32];
        RAND_bytes(rand_pwd, 32);
        string strWhatAmI = "To use neutrond";
        if (mapArgs.count("-server"))
            strWhatAmI = strprintf(_("To use the %s option"), "\"-server\"");
        else if (mapArgs.count("-daemon"))
            strWhatAmI = strprintf(_("To use the %s option"), "\"-daemon\"");
        uiInterface.ThreadSafeMessageBox(strprintf(
            _("%s, you must set a rpcpassword in the configuration file:\n %s\n"
              "It is recommended you use the following random password:\n"
              "rpcuser=Neutronrpc\n"
              "rpcpassword=%s\n"
              "(you do not need to remember this password)\n"
              "The username and password MUST NOT be the same.\n"
              "If the file does not exist, create it with owner-readable-only file permissions.\n"
              "It is also recommended to set alertnotify so you are notified of problems;\n"
              "for example: alertnotify=echo %%s | mail -s \"Neutron Alert\" admin@foo.com\n"),
                strWhatAmI.c_str(),
                GetConfigFile().string().c_str(),
                EncodeBase58(&rand_pwd[0],&rand_pwd[0]+32).c_str()),
            _("Error"), CClientUIInterface::OK | CClientUIInterface::MODAL);
        StartShutdown();
        return;
    }

    const bool fUseSSL = GetBoolArg("-rpcssl");

    asio::io_service io_service;

    ssl::context context(ssl::context::sslv23);
    if (fUseSSL)
    {
        context.set_options(ssl::context::no_sslv2);

        filesystem::path pathCertFile(GetArg("-rpcsslcertificatechainfile", "server.cert"));
        if (!pathCertFile.is_complete()) pathCertFile = filesystem::path(GetDataDir()) / pathCertFile;
        if (filesystem::exists(pathCertFile)) context.use_certificate_chain_file(pathCertFile.string());
        else LogPrintf("ThreadRPCServer ERROR: missing server certificate file %s\n", pathCertFile.string().c_str());

        filesystem::path pathPKFile(GetArg("-rpcsslprivatekeyfile", "server.pem"));
        if (!pathPKFile.is_complete()) pathPKFile = filesystem::path(GetDataDir()) / pathPKFile;
        if (filesystem::exists(pathPKFile)) context.use_private_key_file(pathPKFile.string(), ssl::context::pem);
        else LogPrintf("ThreadRPCServer ERROR: missing server private key file %s\n", pathPKFile.string().c_str());

        string strCiphers = GetArg("-rpcsslciphers", "TLSv1+HIGH:!SSLv2:!aNULL:!eNULL:!AH:!3DES:@STRENGTH");
        SSL_CTX_set_cipher_list(context.native_handle(), strCiphers.c_str());
    }

    // Try a dual IPv6/IPv4 socket, falling back to separate IPv4 and IPv6 sockets
    const bool loopback = !mapArgs.count("-rpcallowip");
    asio::ip::address bindAddress = loopback ? asio::ip::address_v6::loopback() : asio::ip::address_v6::any();
    ip::tcp::endpoint endpoint(bindAddress, GetArg("-rpcport", GetDefaultRPCPort()));
    boost::system::error_code v6_only_error;
    boost::shared_ptr<ip::tcp::acceptor> acceptor(new ip::tcp::acceptor(io_service));

    boost::signals2::signal<void ()> StopRequests;
    bool fListening = false;
    std::string strerr;

    try
    {
        acceptor->open(endpoint.protocol());
        acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

        // Try making the socket dual IPv6/IPv4 (if listening on the "any" address)
        acceptor->set_option(boost::asio::ip::v6_only(loopback), v6_only_error);

        acceptor->bind(endpoint);
        acceptor->listen(socket_base::max_connections);
        RPCListen(acceptor, context, fUseSSL);

        // Cancel outstanding listen-requests for this acceptor when shutting down
        StopRequests.connect(signals2::slot<void ()>(
                    static_cast<void (ip::tcp::acceptor::*)()>(&ip::tcp::acceptor::close), acceptor.get())
                .track(acceptor));

        fListening = true;
    }
    catch(boost::system::system_error &e)
    {
        strerr = strprintf(_("An error occurred while setting up the RPC port %u for listening on IPv6, "
                           "falling back to IPv4: %s"), endpoint.port(), e.what());
    }

    try
    {
        // If dual IPv6/IPv4 failed (or we're opening loopback interfaces only), open IPv4 separately
        if (!fListening || loopback || v6_only_error)
        {
            bindAddress = loopback ? asio::ip::address_v4::loopback() : asio::ip::address_v4::any();
            endpoint.address(bindAddress);

            acceptor.reset(new ip::tcp::acceptor(io_service));
            acceptor->open(endpoint.protocol());
            acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
            acceptor->bind(endpoint);
            acceptor->listen(socket_base::max_connections);

            RPCListen(acceptor, context, fUseSSL);

            // Cancel outstanding listen-requests for this acceptor when shutting down
            StopRequests.connect(signals2::slot<void ()>(
                static_cast<void (ip::tcp::acceptor::*)()>(&ip::tcp::acceptor::close), acceptor.get()).track(acceptor)
            );

            fListening = true;
        }
    }
    catch(boost::system::system_error &e)
    {
        strerr = strprintf(_("An error occurred while setting up the RPC port %u for listening on IPv4: %s"), endpoint.port(), e.what());
    }

    if (!fListening)
    {
        uiInterface.ThreadSafeMessageBox(strerr, _("Error"), CClientUIInterface::OK | CClientUIInterface::MODAL);
        StartShutdown();
        return;
    }

    vnThreadsRunning[THREAD_RPCLISTENER]--;

    while (!fShutdown)
        io_service.run_one();

    vnThreadsRunning[THREAD_RPCLISTENER]++;
    StopRequests();
}

void JSONRPCRequest::parse(const UniValue& valRequest)
{
    if (!valRequest.isObject())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Invalid Request object");

    const UniValue& request = valRequest.get_obj();
    id = find_value(request, "id");
    UniValue valMethod = find_value(request, "method");

    if (valMethod.isNull())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Missing method");

    if (!valMethod.isStr())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Method must be a string");

    strMethod = valMethod.get_str();

    if (strMethod != "getblocktemplate")
        LogPrint("rpc", "ThreadRPCServer method=%s\n", SanitizeString(strMethod));

    UniValue valParams = find_value(request, "params");

    if (valParams.isArray() || valParams.isObject())
        params = valParams;
    else if (valParams.isNull())
        params = UniValue(UniValue::VARR);
    else
        throw JSONRPCError(RPC_INVALID_REQUEST, "Params must be an array or object");
}

static UniValue JSONRPCExecOne(const UniValue& req)
{
    UniValue rpc_result(UniValue::VOBJ);
    JSONRPCRequest jreq;

    try
    {
        jreq.parse(req);
        UniValue result = tableRPC.execute(jreq);
        rpc_result = JSONRPCReplyObj(result, NullUniValue, jreq.id);
    }
    catch (const UniValue& objError)
    {
        rpc_result = JSONRPCReplyObj(NullUniValue, objError, jreq.id);
    }
    catch (const std::exception& e)
    {
        rpc_result = JSONRPCReplyObj(NullUniValue,
                                     JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
    }

    return rpc_result;
}

std::string JSONRPCExecBatch(const UniValue& vReq)
{
    UniValue ret(UniValue::VARR);

    for (unsigned int reqIdx = 0; reqIdx < vReq.size(); reqIdx++)
        ret.push_back(JSONRPCExecOne(vReq[reqIdx]));

    return ret.write() + "\n";
}

static CCriticalSection cs_THREAD_RPCHANDLER;

/**
 * Process named arguments into a vector of positional arguments, based on the
 * passed-in specification for the RPC call's arguments.
 */
static inline JSONRPCRequest transformNamedArguments(const JSONRPCRequest& in, const std::vector<std::string>& argNames)
{
    JSONRPCRequest out = in;
    out.params = UniValue(UniValue::VARR);

    // Build a map of parameters, and remove ones that have been processed, so that we can throw a focused error if
    // there is an unknown one.
    const std::vector<std::string>& keys = in.params.getKeys();
    const std::vector<UniValue>& values = in.params.getValues();
    std::unordered_map<std::string, const UniValue*> argsIn;

    for (size_t i=0; i<keys.size(); ++i)
        argsIn[keys[i]] = &values[i];

    int hole = 0;

    for (const std::string &argName: argNames)
    {
        auto fr = argsIn.find(argName);

        if (fr != argsIn.end())
        {
            for (int i = 0; i < hole; ++i)
            {
                // Fill hole between specified parameters with JSON nulls,
                // but not at the end (for backwards compatibility with calls
                // that act based on number of specified parameters).
                out.params.push_back(UniValue());
            }

            hole = 0;
            out.params.push_back(*fr->second);
            argsIn.erase(fr);
        }
        else
            hole += 1;
    }

    // If there are still arguments in the argsIn map, this is an error.
    if (!argsIn.empty())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown named parameter " + argsIn.begin()->first);

    // Return request with named arguments transformed to positional arguments
    return out;
}

void ThreadRPCServer3(void* parg)
{
    // Make this thread recognisable as the RPC handler
    RenameThread("Neutron-rpchand");
    {
        LOCK(cs_THREAD_RPCHANDLER);
        vnThreadsRunning[THREAD_RPCHANDLER]++;
    }

    AcceptedConnection *conn = (AcceptedConnection *) parg;
    bool fRun = true;

    while (true)
    {
        if (fShutdown || !fRun)
        {
            conn->close();
            delete conn;

            {
                LOCK(cs_THREAD_RPCHANDLER);
                --vnThreadsRunning[THREAD_RPCHANDLER];
            }

            return;
        }

        map<string, string> mapHeaders;
        string strRequest;
        ReadHTTP(conn->stream(), mapHeaders, strRequest);

        // Check authorization
        if (mapHeaders.count("authorization") == 0)
        {
            conn->stream() << HTTPReply(HTTP_UNAUTHORIZED, "", false) << std::flush;
            break;
        }

        if (!HTTPAuthorized(mapHeaders))
        {
            LogPrintf("ThreadRPCServer incorrect password attempt from %s\n", conn->peer_address_to_string().c_str());

            /* Deter brute-forcing short passwords.
               If this results in a DOS the user really
               shouldn't have their RPC port exposed.*/
            if (mapArgs["-rpcpassword"].size() < 20)
                MilliSleep(250);

            conn->stream() << HTTPReply(HTTP_UNAUTHORIZED, "", false) << std::flush;
            break;
        }

        if (mapHeaders["connection"] == "close")
            fRun = false;

        JSONRPCRequest jreq;

        try
        {
            UniValue valRequest;

            if (!valRequest.read(strRequest))
                throw JSONRPCError(RPC_PARSE_ERROR, "Parse error");

            // // Set the URI
            // jreq.URI = req->GetURI();
            // TODO: Why was this disabled ?

            string strReply;

            if (valRequest.isObject())
            {
                jreq.parse(valRequest);
                UniValue result = tableRPC.execute(jreq);
                strReply = JSONRPCReply(result, NullUniValue, jreq.id);
            }
            else if (valRequest.isArray())
                strReply = JSONRPCExecBatch(valRequest.get_array());
            else
                throw JSONRPCError(RPC_PARSE_ERROR, "Top-level object parse error");

            conn->stream() << HTTPReply(HTTP_OK, strReply, fRun) << std::flush;
        }
        catch (const UniValue& objError)
        {
            JSONErrorReply(conn->stream(), objError, jreq.id);
            break;
        }
        catch (const std::exception& e)
        {
            JSONErrorReply(conn->stream(), JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
            break;
        }
    }

    delete conn;

    {
        LOCK(cs_THREAD_RPCHANDLER);
        vnThreadsRunning[THREAD_RPCHANDLER]--;
    }
}

UniValue CRPCTable::execute(const JSONRPCRequest &request) const
{
    const CRPCCommand *pcmd = tableRPC[request.strMethod];

    if (!pcmd)
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found");

    // Observe safe mode
    string strWarning = GetWarnings("rpc");

    if (strWarning != "" && !GetBoolArg("-disablesafemode") && !pcmd->okSafeMode)
        throw JSONRPCError(RPC_FORBIDDEN_BY_SAFE_MODE, string("Safe mode: ") + strWarning);

    if (fDebug)
        LogPrintf("%s : [RPC] - %s\n", __func__, request.strMethod);

    try
    {
        if (pcmd->unlocked)
            return pcmd->actor(request.params, false);
        else
        {
            LOCK(cs_main);

            BOOST_SCOPE_EXIT(pwalletMain) {
                LEAVE_CRITICAL_SECTION(pwalletMain->cs_wallet);
            } BOOST_SCOPE_EXIT_END

            ENTER_CRITICAL_SECTION(pwalletMain->cs_wallet);
            return pcmd->actor(request.params, false);
        }
    }
    catch (std::exception& e)
    {
        throw JSONRPCError(RPC_MISC_ERROR, e.what());
    }
}

std::vector<std::string> CRPCTable::listCommands() const
{
    std::vector<std::string> commandList;
    typedef std::map<std::string, const CRPCCommand*> commandMap;

    std::transform(mapCommands.begin(), mapCommands.end(), std::back_inserter(commandList),
                   boost::bind(&commandMap::value_type::first,_1));

    return commandList;
}

bool CRPCTable::appendCommand(const std::string& name, const CRPCCommand* pcmd)
{
    if (IsRPCRunning())
        return false;

    // Don't allow overwriting for now
    std::map<std::string, const CRPCCommand*>::const_iterator it = mapCommands.find(name);

    if (it != mapCommands.end())
        return false;

    mapCommands[name] = pcmd;
    return true;
}

UniValue CallRPC(const std::string& strMethod, const UniValue& params)
{
    if (mapArgs["-rpcuser"] == "" && mapArgs["-rpcpassword"] == "")
    {
        LogPrintf(
            "You must set rpcpassword=<password> in the configuration file:\n%s\n"
              "If the file does not exist, create it with owner-readable-only file permissions.",
                GetConfigFile().string().c_str());

        throw runtime_error(strprintf(
            _("You must set rpcpassword=<password> in the configuration file:\n%s\n"
              "If the file does not exist, create it with owner-readable-only file permissions."),
                GetConfigFile().string().c_str()));
    }

    // Connect to localhost
    bool fUseSSL = GetBoolArg("-rpcssl");
    asio::io_service io_service;
    ssl::context context(ssl::context::sslv23);
    context.set_options(ssl::context::no_sslv2);
    asio::ssl::stream<asio::ip::tcp::socket> sslStream(io_service, context);
    SSLIOStreamDevice<asio::ip::tcp> d(sslStream, fUseSSL);
    iostreams::stream< SSLIOStreamDevice<asio::ip::tcp> > stream(d);

    if (!d.connect(GetArg("-rpcconnect", "127.0.0.1"), GetArg("-rpcport", itostr(GetDefaultRPCPort()))))
        throw runtime_error("couldn't connect to server");

    // HTTP basic authentication
    string strUserPass64 = EncodeBase64(mapArgs["-rpcuser"] + ":" + mapArgs["-rpcpassword"]);
    map<string, string> mapRequestHeaders;
    mapRequestHeaders["Authorization"] = string("Basic ") + strUserPass64;

    // Send request
    std::string strRequest = JSONRPCRequestObj(strMethod, params, 1).write() + "\n";
    std::string strPost = HTTPPost(strRequest, mapRequestHeaders);
    stream << strPost << std::flush;

    // Receive reply
    map<string, string> mapHeaders;
    string strReply;
    int nStatus = ReadHTTP(stream, mapHeaders, strReply);

    if (nStatus == HTTP_UNAUTHORIZED)
        throw runtime_error("incorrect rpcuser or rpcpassword (authorization failed)");
    else if (nStatus >= 400 && nStatus != HTTP_BAD_REQUEST && nStatus != HTTP_NOT_FOUND && nStatus != HTTP_INTERNAL_SERVER_ERROR)
        throw runtime_error(strprintf("server returned HTTP error %d", nStatus));
    else if (strReply.empty())
        throw runtime_error("no response from server");

    // Parse reply
    UniValue valReply(UniValue::VSTR);

    if (!valReply.read(strReply))
        throw runtime_error("couldn't parse reply from server");

    const UniValue& reply = valReply.get_obj();

    if (reply.empty())
        throw runtime_error("expected reply to have result, error and id properties");

    return reply;
}



class CRPCConvertParam
{
public:
    std::string methodName; //!< method whose params want conversion
    int paramIdx;           //!< 0-based idx of param to convert
    std::string paramName;  //!< parameter name
};

/**
 * Specifiy a (method, idx, name) here if the argument is a non-string RPC
 * argument and needs to be converted from JSON.
 *
 * @note Parameter indexes start from 0.
 */
static const CRPCConvertParam vRPCConvertParams[] =
{
    { "setgenerate", 0, "generate" },
    { "setgenerate", 0, "genproclimit" },
    { "sendtoaddress", 1, "amount" },
    { "settxfee", 0, "amount" },
    { "getreceivedbyaddress", 1, "minconf" },
    { "getreceivedbyaccount", 1, "minconf" },
    { "listaddressbalances", 0, "minamount" },
    { "listreceivedbyaddress", 0, "minconf" },
    { "listreceivedbyaddress", 1, "addlockconf" },
    { "listreceivedbyaccount", 0, "minconf" },
    { "listreceivedbyaccount", 1, "addlockconf" },
    { "getbalance", 1, "minconf" },
    { "getchaintips", 0, "count" },
    { "getchaintips", 1, "branchlen" },
    { "getblockhash", 0, "height" },
    { "getblockbynumber", 0, "height" },
    { "getblockbynumber", 1, "txinfo" },
    { "getblockbyrange", 0, "from" },
    { "getblockbyrange", 1, "to" },
    { "getblockbyrange", 2, "txinfo" },
    { "getblockversionstats", 0, "version" },
    { "getblockversionstats", 1, "blocks_to_count" },
    { "invalidateblock", 0, "height" },
    { "getsuperblockbudget", 0, "index" },
    { "waitforblockheight", 0, "height" },
    { "waitforblockheight", 1, "timeout" },
    { "waitforblock", 1, "timeout" },
    { "waitfornewblock", 0, "timeout" },
    { "move", 2, "amount" },
    { "move", 3, "minconf" },
    { "sendfrom", 2, "amount" },
    { "sendfrom", 3, "minconf" },
    { "listtransactions", 1, "count" },
    { "listtransactions", 2, "skip" },
    { "listaccounts", 0, "minconf" },
    { "walletpassphrase", 1, "timeout" },
    { "walletpassphrase", 2, "mixingonly" },
    { "getblocktemplate", 0, "template_request" },
    { "listsinceblock", 1, "target_confirmations" },
    { "sendmany", 1, "amounts" },
    { "sendmany", 2, "minconf" },
    { "addmultisigaddress", 0, "nrequired" },
    { "addmultisigaddress", 1, "keys" },
    { "createmultisig", 0, "nrequired" },
    { "createmultisig", 1, "keys" },
    { "listunspent", 0, "minconf" },
    { "listunspent", 1, "maxconf" },
    { "listunspent", 2, "addresses" },
    { "getblock", 1, "verbose" },
    { "getblockheader", 1, "verbose" },
    { "getblockheaders", 1, "count" },
    { "getblockheaders", 2, "verbose" },
    { "gettransaction", 1, "include_watchonly" },
    { "getrawtransaction", 1, "verbose" },
    { "createrawtransaction", 0, "inputs" },
    { "createrawtransaction", 1, "outputs" },
    { "signrawtransaction", 1, "prevtxs" },
    { "signrawtransaction", 2, "privkeys" },
    { "sendrawtransaction", 1, "allowhighfees" },
    { "sendrawtransaction", 2, "instantsend" },
    { "sendrawtransaction", 3, "bypasslimits" },
    { "fundrawtransaction", 1, "options" },
    { "gettxout", 1, "n" },
    { "gettxout", 2, "include_mempool" },
    { "gettxoutproof", 0, "txids" },
    { "lockunspent", 0, "unlock" },
    { "lockunspent", 1, "transactions" },
    { "importprivkey", 2, "rescan" },
    { "importaddress", 2, "rescan" },
    { "importaddress", 3, "p2sh" },
    { "importpubkey", 2, "rescan" },
    { "importmulti", 0, "requests" },
    { "importmulti", 1, "options" },
    { "verifychain", 0, "checklevel" },
    { "verifychain", 1, "nblocks" },
    { "pruneblockchain", 0, "height" },
    { "keypoolrefill", 0, "newsize" },
    { "getrawmempool", 0, "verbose" },
    { "estimatefee", 0, "nblocks" },
    { "estimatepriority", 0, "nblocks" },
    { "estimatesmartfee", 0, "nblocks" },
    { "estimatesmartpriority", 0, "nblocks" },
    { "prioritisetransaction", 1, "priority_delta" },
    { "prioritisetransaction", 2, "fee_delta" },
    { "setban", 2, "bantime" },
    { "setban", 3, "absolute" },
    { "setnetworkactive", 0, "state" },
    { "getmempoolancestors", 1, "verbose" },
    { "getmempooldescendants", 1, "verbose" },
    { "spork", 1, "value" },
    { "voteraw", 1, "tx_index" },
    { "voteraw", 5, "time" },
    { "getblockhashes", 0, "high"},
    { "getblockhashes", 1, "low" },
    { "getspentinfo", 0, "json" },
    { "getaddresstxids", 0, "addresses" },
    { "getaddressbalance", 0, "addresses" },
    { "getaddressdeltas", 0, "addresses" },
    { "getaddressutxos", 0, "addresses" },
    { "getaddressmempool", 0, "addresses" },
    // Echo with conversion (For testing only)
    { "echojson", 0, "arg0" },
    { "echojson", 1, "arg1" },
    { "echojson", 2, "arg2" },
    { "echojson", 3, "arg3" },
    { "echojson", 4, "arg4" },
    { "echojson", 5, "arg5" },
    { "echojson", 6, "arg6" },
    { "echojson", 7, "arg7" },
    { "echojson", 8, "arg8" },
    { "echojson", 9, "arg9" },

    { "debug", 0, "enabled" },
    { "stop", 0, "detach" },
    { "reservebalance", 0, "reserve" },
    { "reservebalance", 1, "amount" },
    { "sendalert", 2, "" },
    { "sendalert", 3, "" },
    { "sendalert", 4, "" },
    { "sendalert", 5, "" },
    { "sendalert", 6, "" },
};

class CRPCConvertTable
{
private:
    std::set<std::pair<std::string, int>> members;
    std::set<std::pair<std::string, std::string>> membersByName;

public:
    CRPCConvertTable();

    bool convert(const std::string& method, int idx)
    {
        return (members.count(std::make_pair(method, idx)) > 0);
    }

    bool convert(const std::string& method, const std::string& name)
    {
        return (membersByName.count(std::make_pair(method, name)) > 0);
    }
};

CRPCConvertTable::CRPCConvertTable()
{
    const unsigned int n_elem = sizeof(vRPCConvertParams) / sizeof(vRPCConvertParams[0]);

    for (unsigned int i = 0; i < n_elem; i++)
    {
        members.insert(std::make_pair(vRPCConvertParams[i].methodName,
                                      vRPCConvertParams[i].paramIdx));

        membersByName.insert(std::make_pair(vRPCConvertParams[i].methodName,
                                            vRPCConvertParams[i].paramName));
    }
}

static CRPCConvertTable rpcCvtTable;

/** Non-RFC4627 JSON parser, accepts internal values (such as numbers, true, false, null)
 * as well as objects and arrays.
 */
UniValue ParseNonRFCJSONValue(const std::string& strVal)
{
    UniValue jVal;

    if (!jVal.read(std::string("[")+strVal+std::string("]")) || !jVal.isArray() || jVal.size()!=1)
        throw std::runtime_error(std::string("Error parsing JSON:")+strVal);

    return jVal[0];
}

UniValue RPCConvertValues(const std::string &strMethod, const std::vector<std::string> &strParams)
{
    UniValue params(UniValue::VARR);

    for (unsigned int idx = 0; idx < strParams.size(); idx++)
    {
        const std::string& strVal = strParams[idx];

        if (!rpcCvtTable.convert(strMethod, idx))
            params.push_back(strVal);
        else
            params.push_back(ParseNonRFCJSONValue(strVal));
    }

    return params;
}

UniValue RPCConvertNamedValues(const std::string &strMethod, const std::vector<std::string> &strParams)
{
    UniValue params(UniValue::VOBJ);

    for (const std::string &s: strParams)
    {
        size_t pos = s.find("=");

        if (pos == std::string::npos)
            throw(std::runtime_error("No '=' in named argument '"+s+"', this needs to be present for every argument (even if it is empty)"));

        std::string name = s.substr(0, pos);
        std::string value = s.substr(pos+1);

        if (!rpcCvtTable.convert(strMethod, name))
            params.pushKV(name, value);
        else
            params.pushKV(name, ParseNonRFCJSONValue(value));
    }

    return params;
}


int CommandLineRPC(int argc, char *argv[])
{
    std::string strPrint;
    int nRet = 0;

    try
    {
        // Skip switches
        while (argc > 1 && IsSwitchChar(argv[1][0]))
        {
            argc--;
            argv++;
        }

        if (argc < 2)
            throw runtime_error("too few parameters");

        string strMethod = argv[1];

        std::vector<std::string> strParams(&argv[2], &argv[argc]);
        UniValue params = RPCConvertValues(strMethod, strParams);
        const UniValue reply = CallRPC(strMethod, params);
        const UniValue& result = find_value(reply, "result");
        const UniValue& error  = find_value(reply, "error");

        if (!error.isNull())
        {
            strPrint = "error: " + error.write();
            int code = find_value(error.get_obj(), "code").get_int();
            nRet = abs(code);
        }
        else
        {
            if (result.isNull())
                strPrint = "";
            else if (result.isStr())
                strPrint = result.get_str();
            else
                strPrint = result.write(2);
        }
    }
    catch (std::exception& e)
    {
        strPrint = string("error: ") + e.what();
        nRet = 87;
    }
    catch (...)
    {
        PrintException(NULL, "CommandLineRPC()");
    }

    if (strPrint != "")
    {
        fprintf((nRet == 0 ? stdout : stderr), "%s\n", strPrint.c_str());
    }

    return nRet;
}

#ifdef TEST
int main(int argc, char *argv[])
{
#ifdef _MSC_VER
    // Turn off Microsoft heap dump noise
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, CreateFile("NUL", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0));
#endif
    setbuf(stdin, NULL);
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    try
    {
        if (argc >= 2 && string(argv[1]) == "-server")
        {
            LogPrintf("server ready\n");
            ThreadRPCServer(NULL);
        }
        else
        {
            return CommandLineRPC(argc, argv);
        }
    }
    catch (std::exception& e)
    {
        PrintException(&e, "main()");
    }
    catch (...)
    {
        PrintException(NULL, "main()");
    }

    return 0;
}
#endif

std::string HelpExampleCli(string methodname, string args)
{
    return "> neutrond " + methodname + " " + args + "\n";
}

std::string HelpExampleRpc(string methodname, string args)
{
    return "> curl --user myusername --data-binary '{\"jsonrpc\": \"1.0\", \"id\":\"curltest\", "
        "\"method\": \"" + methodname + "\", \"params\": [" + args + "] }' -H 'content-type: text/plain;' http://127.0.0.1:32001/\n";
}

CRPCTable tableRPC;
