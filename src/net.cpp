// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "net.h"
#include "addrman.h"
#include "clientversion.h"
#include "db.h"
#include "init.h"
#include "miner.h"
#include "netbase.h"
#include "strlcpy.h"
#include "wallet.h"
#include "ui_interface.h"
#include "utilstrencodings.h"
#include "darksend.h"

#ifdef WIN32
#include <string.h>
#else
#include <fcntl.h>
#endif

#ifdef USE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/miniwget.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

//#include <curl/curl.h>
//#include <regex>

#include <math.h>

// Dump addresses to peers.dat and banlist.dat every 15 minutes (900s)
#define DUMP_ADDRESSES_INTERVAL 900

void ThreadMessageHandler2(void* parg);
#ifdef USE_UPNP
void ThreadMapPort2(void* parg);
#endif

struct LocalServiceInfo
{
    int nScore;
    int nPort;
};

bool fClient = false;
bool fDiscover = true;
bool fListen = false;
bool fUseUPnP = false;
uint64_t nLocalServices = (fClient ? 0 : (uint64_t) NODE_NETWORK);
static CCriticalSection cs_mapLocalHost;
static map<CNetAddr, LocalServiceInfo> mapLocalHost;
static bool vfReachable[NET_MAX] = {};
static bool vfLimited[NET_MAX] = {};
static CNode* pnodeLocalHost = NULL;
CAddress addrSeenByPeer(CService("0.0.0.0", 0), nLocalServices);
uint64_t nLocalHostNonce = 0;
boost::array<int, THREAD_MAX> vnThreadsRunning;
static std::vector<SOCKET> vhListenSocket;
CAddrMan addrman;

vector<CNode*> vNodes;
CCriticalSection cs_vNodes;
map<CInv, CDataStream> mapRelay;
deque<pair<int64_t, CInv> > vRelayExpiration;
CCriticalSection cs_mapRelay;
map<CInv, int64_t> mapAlreadyAskedFor;

static deque<string> vOneShots;
CCriticalSection cs_vOneShots;
set<CNetAddr> setservAddNodeAddresses;
CCriticalSection cs_setservAddNodeAddresses;

NodeId nLastNodeId = 0;
CCriticalSection cs_nLastNodeId;

boost::condition_variable messageHandlerCondition;

void CConnman::AddOneShot(const std::string& strDest)
{
    LOCK(cs_vOneShots);
    vOneShots.push_back(strDest);
}

unsigned short GetListenPort()
{
    return (unsigned short)(GetArg("-port", GetDefaultPort()));
}

void CNode::PushGetBlocks(CBlockIndex* pindexBegin, uint256 hashEnd)
{
    // Filter out duplicate requests
    if (pindexBegin == pindexLastGetBlocksBegin && hashEnd == hashLastGetBlocksEnd)
        return;

    pindexLastGetBlocksBegin = pindexBegin;
    hashLastGetBlocksEnd = hashEnd;

    {
        LOCK(cs_vNodes);

        BOOST_FOREACH(CNode* pnode, vNodes)
        {
            if (!pnode->HasFulfilledRequest(NetMsgType::GETSPORKS))
            {
                pnode->PushMessage(NetMsgType::GETSPORKS);
                pnode->FulfilledRequest(NetMsgType::GETSPORKS);
            }
        }
    }

    if (fDebug)
    {
        LogPrintf("%s : getting blocks from index at [%d] with hash %s [isInitialBlockDownload = %d]\n",
                  __func__, pindexBegin->nHeight, pindexBegin->phashBlock->ToString().c_str(),
                  IsInitialBlockDownload());
    }

    PushMessage(NetMsgType::GETBLOCKS, CBlockLocator(pindexBegin), hashEnd);
}

// find 'best' local address for a particular peer
bool GetLocal(CService& addr, const CNetAddr *paddrPeer)
{
    if (!fListen)
        return false;

    int nBestScore = -1;
    int nBestReachability = -1;

    {
        LOCK(cs_mapLocalHost);

        for (map<CNetAddr, LocalServiceInfo>::iterator it = mapLocalHost.begin(); it != mapLocalHost.end(); it++)
        {
            int nScore = (*it).second.nScore;
            int nReachability = (*it).first.GetReachabilityFrom(paddrPeer);

            if (nReachability > nBestReachability || (nReachability == nBestReachability && nScore > nBestScore))
            {
                addr = CService((*it).first, (*it).second.nPort);
                nBestReachability = nReachability;
                nBestScore = nScore;
            }
        }
    }

    return nBestScore >= 0;
}

// get best local address for a particular peer as a CAddress
CAddress GetLocalAddress(const CNetAddr *paddrPeer)
{
    CAddress ret(CService("0.0.0.0", 0), 0);
    CService addr;

    if (GetLocal(addr, paddrPeer))
    {
        ret = CAddress(addr);
        ret.nServices = nLocalServices;
        ret.nTime = GetAdjustedTime();
    }

    return ret;
}

bool RecvLine(SOCKET hSocket, string& strLine)
{
    strLine = "";

    while (true)
    {
        char c;
        int nBytes = recv(hSocket, &c, 1, 0);

        if (nBytes > 0)
        {
            if (c == '\n')
                continue;

            if (c == '\r')
                return true;

            strLine += c;

            if (strLine.size() >= 9000)
                return true;
        }
        else if (nBytes <= 0)
        {
            if (fShutdown)
                return false;

            if (nBytes < 0)
            {
                int nErr = WSAGetLastError();

                if (nErr == WSAEMSGSIZE)
                    continue;

                if (nErr == WSAEWOULDBLOCK || nErr == WSAEINTR || nErr == WSAEINPROGRESS)
                {
                    MilliSleep(10);
                    continue;
                }
            }

            if (!strLine.empty())
                return true;

            if (nBytes == 0)
            {
                // socket closed
                LogPrintf("%s : socket closed\n", __func__);
                return false;
            }
            else
            {
                // socket error
                int nErr = WSAGetLastError();
                LogPrintf("%s : recv failed: %d\n", __func__, nErr);
                return false;
            }
        }
    }
}

// Used when scores of local addresses may have changed
// pushes better local address to peers
void static AdvertizeLocal()
{
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if (pnode->fSuccessfullyConnected)
        {
            CAddress addrLocal = GetLocalAddress(&pnode->addr);

            if (addrLocal.IsRoutable() && (CService)addrLocal != (CService)pnode->addrLocal)
            {
                pnode->PushAddress(addrLocal);
                pnode->addrLocal = addrLocal;
            }
        }
    }
}

void SetReachable(enum Network net, bool fFlag)
{
    LOCK(cs_mapLocalHost);
    vfReachable[net] = fFlag;

    if (net == NET_IPV6 && fFlag)
        vfReachable[NET_IPV4] = true;
}

// learn a new local address
bool AddLocal(const CService& addr, int nScore)
{
    if (!addr.IsRoutable())
        return false;

    if (!fDiscover && nScore < LOCAL_MANUAL)
        return false;

    if (IsLimited(addr))
        return false;

    LogPrintf("%s : adding new address %s, score:%i\n", __func__,
              addr.ToString().c_str(), nScore);

    {
        LOCK(cs_mapLocalHost);

        bool fAlready = mapLocalHost.count(addr) > 0;
        LocalServiceInfo &info = mapLocalHost[addr];

        if (!fAlready || nScore >= info.nScore)
        {
            info.nScore = nScore + (fAlready ? 1 : 0);
            info.nPort = addr.GetPort();
        }

        SetReachable(addr.GetNetwork());
    }

    AdvertizeLocal();
    return true;
}

bool AddLocal(const CNetAddr &addr, int nScore)
{
    return AddLocal(CService(addr, GetListenPort()), nScore);
}

// make a particular network entirely off-limits (no automatic connects to it)
void SetLimited(enum Network net, bool fLimited)
{
    if (net == NET_UNROUTABLE)
        return;

    LOCK(cs_mapLocalHost);
    vfLimited[net] = fLimited;
}

bool IsLimited(enum Network net)
{
    LOCK(cs_mapLocalHost);
    return vfLimited[net];
}

bool IsLimited(const CNetAddr &addr)
{
    return IsLimited(addr.GetNetwork());
}

// vote for a local address
bool SeenLocal(const CService& addr)
{
    {
        LOCK(cs_mapLocalHost);

        if (mapLocalHost.count(addr) == 0)
            return false;

        mapLocalHost[addr].nScore++;
    }

    AdvertizeLocal();
    return true;
}

bool IsLocal(const CService& addr)
{
    LOCK(cs_mapLocalHost);
    return mapLocalHost.count(addr) > 0;
}

bool IsReachable(const CNetAddr& addr)
{
    LOCK(cs_mapLocalHost);
    enum Network net = addr.GetNetwork();

    return vfReachable[net] && !vfLimited[net];
}

void AddressCurrentlyConnected(const CService& addr)
{
    addrman.Connected(addr);
}

CNode* FindNode(const CNetAddr& ip)
{
    {
        LOCK(cs_vNodes);

        BOOST_FOREACH(CNode* pnode, vNodes)
        {
            if ((CNetAddr)pnode->addr == ip)
                return (pnode);
        }
    }

    return NULL;
}

CNode* FindNode(std::string addrName)
{
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if (pnode->addrName == addrName)
            return (pnode);
    }

    return NULL;
}

CNode* FindNode(const CService& addr)
{
    {
        LOCK(cs_vNodes);

        BOOST_FOREACH(CNode* pnode, vNodes)
        {
            if ((CService)pnode->addr == addr)
                return (pnode);
        }
    }

    return NULL;
}

CNode* CConnman::ConnectNode(CAddress addrConnect, const char *pszDest, bool darkSendMaster)
{
    if (pszDest == NULL)
    {
        if (IsLocal(addrConnect))
            return NULL;

        // Look for an existing connection
        CNode* pnode = FindNode((CService)addrConnect);

        if (pnode)
        {
            if(darkSendMaster)
                pnode->fDarkSendMaster = true;

            pnode->AddRef();
            return pnode;
        }
    }

    if (fDebug)
    {
        LogPrintf("%s : trying connection %s lastseen=%.1fhrs\n", __func__,
                  pszDest ? pszDest : addrConnect.ToString(),
                  pszDest ? 0.0 : (double)  (GetAdjustedTime() - addrConnect.nTime) / 3600.0);
    }

    // Connect
    SOCKET hSocket;
    bool proxyConnectionFailed = false;

    if (pszDest ? ConnectSocketByName(addrConnect, hSocket, pszDest, GetDefaultPort(), nConnectTimeout, &proxyConnectionFailed) :
                  ConnectSocket(addrConnect, hSocket, nConnectTimeout, &proxyConnectionFailed))
    {
        if (!IsSelectableSocket(hSocket))
        {
            LogPrintf("%s : cannot create connection, non-selectable socket created\n", __func__);
            CloseSocket(hSocket);
            return NULL;
        }

        addrman.Attempt(addrConnect);

        // Add node
        CNode* pnode = new CNode(hSocket, addrConnect, pszDest ? pszDest : "", false);

        pnode->AddRef();
        pnode->nTimeConnected = GetTime();

        LOCK(cs_vNodes);
        vNodes.push_back(pnode);

        return pnode;
    }
    else if (!proxyConnectionFailed)
    {
        // If connecting to the node failed, and failure is not caused by a problem connecting to
        // the proxy, mark this as an attempt.
        addrman.Attempt(addrConnect);
    }

    return NULL;
}

void CConnman::DumpBanlist()
{
    SweepBanned(); // clean unused entries (if bantime has expired)

    if (!BannedSetIsDirty())
        return;

    int64_t nStart = GetTimeMillis();

    CBanDB bandb;
    banmap_t banmap;
    SetBannedSetDirty(false);
    GetBanned(banmap);

    if (!bandb.Write(banmap))
        SetBannedSetDirty(true);

    LogPrintf("%s : flushed %d banned node ips/subnets to banlist.dat  %dms\n",
              __func__, banmap.size(), GetTimeMillis() - nStart);
}

void CNode::CloseSocketDisconnect()
{
    fDisconnect = true;

    if (hSocket != INVALID_SOCKET)
    {
        LogPrintf("%s : disconnecting node %s\n", __func__, addrName.c_str());
        CloseSocket(hSocket);
        hSocket = INVALID_SOCKET;

        // in case this fails, we'll empty the recv buffer when the CNode is deleted
        TRY_LOCK(cs_vRecvMsg, lockRecv);

        if (lockRecv)
            vRecvMsg.clear();
    }
}

bool CConnman::AddNode(const std::string& strNode)
{
    // check if node already added
    CNode* pnode = FindNode(strNode);

    if (pnode != NULL)
        return false;

    CService addr = CService(strNode);

    if (shared_connman->ConnectNode((CAddress)addr, NULL, true))
        LogPrintf("%s : successfully connected to node\n", __func__);
    else
        LogPrintf("%s : error connecting to node\n", __func__);

    return true;
}

bool CConnman::RemoveAddedNode(const std::string& strNode)
{
    CNode* pnode = FindNode(strNode);

    if (pnode != NULL)
    {
        pnode->CloseSocketDisconnect();
        return true;
    }

    return false;
}

void CNode::Cleanup()
{
}

void CNode::PushVersion()
{
    int64_t nTime = (fInbound ? GetAdjustedTime() : GetTime());
    CAddress addrYou = (addr.IsRoutable() && !IsProxy(addr) ? addr : CAddress(CService("0.0.0.0",0)));
    CAddress addrMe = GetLocalAddress(&addr);
    GetRandBytes((unsigned char*)&nLocalHostNonce, sizeof(nLocalHostNonce));

    if (fDebug)
    {
        if (fLogIPs)
        {
            LogPrintf("%s : send version message, version %d, blocks=%d, us=%s, them=%s, peer=%d\n",
                      __func__, PROTOCOL_VERSION, nBestHeight, addrMe.ToString(), addrYou.ToString(), id);
        }
        else
        {
            LogPrintf("%s : send version message, version %d, blocks=%d, us=%s, peer=%d\n",
                      __func__, PROTOCOL_VERSION, nBestHeight, addrMe.ToString(), id);
        }
    }

    PushMessage(NetMsgType::VERSION, PROTOCOL_VERSION, nLocalServices, nTime, addrYou, addrMe,
                nLocalHostNonce, FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, std::vector<string>()),
                nBestHeight, true);
}

void CConnman::ClearBanned()
{
    {
        LOCK(cs_setBanned);
        setBanned.clear();
        setBannedIsDirty = true;
    }

    DumpBanlist(); // store banlist to disk

    // if(clientInterface)
    //     clientInterface->BannedListChanged();
}

bool CConnman::IsBanned(CNetAddr ip)
{
    bool fResult = false;

    {
        LOCK(cs_setBanned);

        for (banmap_t::iterator it = setBanned.begin(); it != setBanned.end(); it++)
        {
            CSubNet subNet = (*it).first;
            CBanEntry banEntry = (*it).second;

            if(subNet.Match(ip) && GetTime() < banEntry.nBanUntil)
                fResult = true;
        }
    }

    return fResult;
}

bool CConnman::IsBanned(CSubNet subnet)
{
    bool fResult = false;

    {
        LOCK(cs_setBanned);
        banmap_t::iterator i = setBanned.find(subnet);

        if (i != setBanned.end())
        {
            CBanEntry banEntry = (*i).second;

            if (GetTime() < banEntry.nBanUntil)
                fResult = true;
        }
    }

    return fResult;
}

void CConnman::Ban(const CNetAddr& addr, const BanReason &banReason, int64_t bantimeoffset, bool sinceUnixEpoch)
{
    CSubNet subNet(addr);
    Ban(subNet, banReason, bantimeoffset, sinceUnixEpoch);
}

void CConnman::Ban(const CSubNet& subNet, const BanReason &banReason, int64_t bantimeoffset, bool sinceUnixEpoch)
{
    CBanEntry banEntry(GetTime());
    banEntry.banReason = banReason;

    if (bantimeoffset <= 0)
    {
        bantimeoffset = GetArg("-bantime", DEFAULT_MISBEHAVING_BANTIME);
        sinceUnixEpoch = false;
    }

    banEntry.nBanUntil = (sinceUnixEpoch ? 0 : GetTime() )+bantimeoffset;

    {
        LOCK(cs_setBanned);

        if (setBanned[subNet].nBanUntil < banEntry.nBanUntil)
        {
            setBanned[subNet] = banEntry;
            setBannedIsDirty = true;
        }
        else
            return;
    }

    // if(clientInterface)
    //     clientInterface->BannedListChanged();

    {
        LOCK(cs_vNodes);

        BOOST_FOREACH(CNode* pnode, vNodes)
        {
            if (subNet.Match((CNetAddr)pnode->addr))
                pnode->CloseSocketDisconnect();
        }
    }

    if(banReason == BanReasonManuallyAdded)
        DumpBanlist(); // store banlist to disk immediately if user requested ban
}

bool CConnman::Unban(const CNetAddr &addr)
{
    CSubNet subNet(addr);
    return Unban(subNet);
}

bool CConnman::Unban(const CSubNet &subNet)
{
    {
        LOCK(cs_setBanned);

        if (!setBanned.erase(subNet))
            return false;

        setBannedIsDirty = true;
    }

    // if(clientInterface)
    //     clientInterface->BannedListChanged();

    DumpBanlist(); // store banlist to disk immediately
    return true;
}

void CConnman::GetBanned(banmap_t &banMap)
{
    LOCK(cs_setBanned);
    banMap = setBanned; // create a thread safe copy
}

void CConnman::SetBanned(const banmap_t &banMap)
{
    LOCK(cs_setBanned);
    setBanned = banMap;
    setBannedIsDirty = true;
}

void CConnman::SweepBanned()
{
    int64_t now = GetTime();

    LOCK(cs_setBanned);
    banmap_t::iterator it = setBanned.begin();

    while (it != setBanned.end())
    {
        CSubNet subNet = (*it).first;
        CBanEntry banEntry = (*it).second;

        if (now > banEntry.nBanUntil)
        {
            setBanned.erase(it++);
            setBannedIsDirty = true;
            LogPrintf("%s: removed banned node ip/subnet from banlist.dat: %s\n", __func__, subNet.ToString());
        }
        else
            ++it;
    }
}

bool CConnman::BannedSetIsDirty()
{
    LOCK(cs_setBanned);
    return setBannedIsDirty;
}

void CConnman::SetBannedSetDirty(bool dirty)
{
    LOCK(cs_setBanned); //reuse setBanned lock for the isDirty flag
    setBannedIsDirty = dirty;
}

bool CNode::Misbehaving(std::string cause, int howmuch)
{
    if (addr.IsLocal())
    {
        LogPrintf("%s : [WARNING] Local node %s misbehaving (delta: %d, cause: %s)\n",
                  __func__, addrName.c_str(), howmuch, cause.c_str());
        return false;
    }

    nMisbehavior += howmuch;

    {
        LOCK(cs_misbehaviors);
        misbehaviors.push_back(Misbehavior(GetTime(), howmuch, cause));
    }

    if (nMisbehavior >= GetArg("-banscore", 100))
    {
        LogPrintf("%s : [WARNING] %s (%d -> %d, cause: %s) DISCONNECTING\n", __func__, addr.ToString().c_str(),
                  nMisbehavior - howmuch, nMisbehavior, cause.c_str());
        g_connman->Ban(addr, BanReasonNodeMisbehaving);
        return true;
    }
    else
    {
        LogPrintf("%s : [WARNING] %s (%d -> %d, cause: %s)\n", __func__, addr.ToString().c_str(),
                  nMisbehavior - howmuch, nMisbehavior, cause.c_str());
    }

    return false;
}

std::vector<CNode::Misbehavior> CNode::GetMisbehaviors()
{
    std::vector<CNode::Misbehavior> copy;

    {
        LOCK(cs_misbehaviors);
        copy = misbehaviors;
    }

    return copy;
}

// requires LOCK(cs_vRecvMsg)
bool CNode::ReceiveMsgBytes(const char *pch, unsigned int nBytes)
{
    while (nBytes > 0)
    {
        // get current incomplete message, or create a new one
        if (vRecvMsg.empty() || vRecvMsg.back().complete())
            vRecvMsg.push_back(CNetMessage(SER_NETWORK, nRecvVersion));

        CNetMessage& msg = vRecvMsg.back();

        // absorb network data
        int handled;

        if (!msg.in_data)
            handled = msg.readHeader(pch, nBytes);
        else
            handled = msg.readData(pch, nBytes);

        if (handled < 0)
                return false;

        if (msg.in_data && msg.hdr.nMessageSize > MAX_PROTOCOL_MESSAGE_LENGTH)
        {
            LogPrint("net", "Oversized message from peer=%i, disconnecting\n", GetId());
            return false;
        }

        pch += handled;
        nBytes -= handled;

        if (msg.complete())
        {
            msg.nTime = GetTimeMicros();
            messageHandlerCondition.notify_one();
        }
    }

    return true;
}

int CNetMessage::readHeader(const char *pch, unsigned int nBytes)
{
    // copy data to temporary parsing buffer
    unsigned int nRemaining = 24 - nHdrPos;
    unsigned int nCopy = std::min(nRemaining, nBytes);

    memcpy(&hdrbuf[nHdrPos], pch, nCopy);
    nHdrPos += nCopy;

    // if header incomplete, exit
    if (nHdrPos < 24)
        return nCopy;

    // deserialize to CMessageHeader
    try
    {
        hdrbuf >> hdr;
    }
    catch (std::exception &e)
    {
        return -1;
    }

    // reject messages larger than MAX_SIZE
    if (hdr.nMessageSize > MAX_SIZE)
            return -1;

    // switch state to reading message data
    in_data = true;
    vRecv.resize(hdr.nMessageSize);

    return nCopy;
}

int CNetMessage::readData(const char *pch, unsigned int nBytes)
{
    unsigned int nRemaining = hdr.nMessageSize - nDataPos;
    unsigned int nCopy = std::min(nRemaining, nBytes);

    memcpy(&vRecv[nDataPos], pch, nCopy);
    nDataPos += nCopy;

    return nCopy;
}

// TODO: implement void CConnman::AcceptConnection

// requires LOCK(cs_vSend)
void SocketSendData(CNode *pnode)
{
    std::deque<CSerializeData>::iterator it = pnode->vSendMsg.begin();

    while (it != pnode->vSendMsg.end())
    {
        const CSerializeData &data = *it;
        assert(data.size() > pnode->nSendOffset);
        int nBytes = send(pnode->hSocket, &data[pnode->nSendOffset], data.size() - pnode->nSendOffset, MSG_NOSIGNAL | MSG_DONTWAIT);

        if (nBytes > 0)
        {
            pnode->nLastSend = GetTime();
            pnode->nSendOffset += nBytes;

            if (pnode->nSendOffset == data.size())
            {
                pnode->nSendOffset = 0;
                pnode->nSendSize -= data.size();
                it++;
            }
            else
            {
                // could not send full message; stop sending more
                break;
            }
        }
        else
        {
            if (nBytes < 0)
            {
                // error
                int nErr = WSAGetLastError();

                if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                {
                    LogPrintf("%s : socket send error %d\n", __func__, nErr);
                    pnode->CloseSocketDisconnect();
                }
            }

            // couldn't send anything at all
            break;
        }
    }

    if (it == pnode->vSendMsg.end())
    {
        assert(pnode->nSendOffset == 0);
        assert(pnode->nSendSize == 0);
    }

    pnode->vSendMsg.erase(pnode->vSendMsg.begin(), it);
}

void CConnman::ThreadSocketHandler()
{
    // Make this thread recognisable as the networking thread
    RenameThread("neutron-net");

    try
    {
        vnThreadsRunning[THREAD_SOCKETHANDLER]++;
        ThreadSocketHandler2();
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
    }
    catch (std::exception& e)
    {
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        PrintException(&e, __func__);
    }
    catch (...)
    {
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        throw; // support pthread_cancel()
    }

    LogPrintf("%s : exited\n", __func__);
}

void CConnman::ThreadSocketHandler2()
{
    LogPrintf("%s : started\n", __func__);
    list<CNode*> vNodesDisconnected;
    unsigned int nPrevNodeCount = 0;

    while (true)
    {
        // Disconnect nodes
        {
            LOCK(cs_vNodes);
            vector<CNode*> vNodesCopy = vNodes;

            BOOST_FOREACH(CNode* pnode, vNodesCopy)
            {
                if (pnode->fDisconnect ||
                    (pnode->GetRefCount() <= 0 && pnode->vRecvMsg.empty() && pnode->nSendSize == 0 && pnode->ssSend.empty()))
                {
                    // remove from vNodes
                    vNodes.erase(remove(vNodes.begin(), vNodes.end(), pnode), vNodes.end());

                    // release outbound grant (if any)
                    pnode->grantOutbound.Release();

                    // close socket and cleanup
                    pnode->CloseSocketDisconnect();
                    pnode->Cleanup();

                    // hold in disconnected pool until all refs are released
                    if (pnode->fNetworkNode || pnode->fInbound)
                        pnode->Release();

                    vNodesDisconnected.push_back(pnode);
                }
            }

            // Delete disconnected nodes
            list<CNode*> vNodesDisconnectedCopy = vNodesDisconnected;

            BOOST_FOREACH(CNode* pnode, vNodesDisconnectedCopy)
            {
                // wait until threads are done using it
                if (pnode->GetRefCount() <= 0)
                {
                    bool fDelete = false;
                    {
                        TRY_LOCK(pnode->cs_vSend, lockSend);

                        if (lockSend)
                        {
                            TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);

                            if (lockRecv)
                            {
                                TRY_LOCK(pnode->cs_mapRequests, lockReq);

                                if (lockReq)
                                {
                                    TRY_LOCK(pnode->cs_inventory, lockInv);

                                    if (lockInv)
                                        fDelete = true;
                                }
                            }
                        }
                    }

                    if (fDelete)
                    {
                        vNodesDisconnected.remove(pnode);
                        delete pnode;
                    }
                }
            }
        }

        {
            LOCK(cs_vNodes);

            if (vNodes.size() != nPrevNodeCount)
            {
                nPrevNodeCount = vNodes.size();
                uiInterface.NotifyNumConnectionsChanged(vNodes.size());
            }
        }


        // Make sure we periodically flush the banlist
        static unsigned int ticks = 0;

        if (++ticks % 100 == 0)
            SweepBanned();

        // Find which sockets have data to receive
        struct timeval timeout;
        timeout.tv_sec  = 0;
        timeout.tv_usec = 50000; // frequency to poll pnode->vSend

        fd_set fdsetRecv;
        fd_set fdsetSend;
        fd_set fdsetError;
        FD_ZERO(&fdsetRecv);
        FD_ZERO(&fdsetSend);
        FD_ZERO(&fdsetError);
        SOCKET hSocketMax = 0;
        bool have_fds = false;

        BOOST_FOREACH(SOCKET hListenSocket, vhListenSocket)
        {
            FD_SET(hListenSocket, &fdsetRecv);
            hSocketMax = max(hSocketMax, hListenSocket);
            have_fds = true;
        }

        {
            LOCK(cs_vNodes);

            BOOST_FOREACH(CNode* pnode, vNodes)
            {
                if (pnode->hSocket == INVALID_SOCKET)
                    continue;
                {
                    TRY_LOCK(pnode->cs_vSend, lockSend);

                    if (lockSend)
                    {
                        // do not read, if draining write queue
                        if (!pnode->vSendMsg.empty())
                            FD_SET(pnode->hSocket, &fdsetSend);
                        else
                            FD_SET(pnode->hSocket, &fdsetRecv);

                        FD_SET(pnode->hSocket, &fdsetError);
                        hSocketMax = max(hSocketMax, pnode->hSocket);
                        have_fds = true;
                    }
                }
            }
        }

        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        int nSelect = select(have_fds ? hSocketMax + 1 : 0,
                             &fdsetRecv, &fdsetSend, &fdsetError, &timeout);
        vnThreadsRunning[THREAD_SOCKETHANDLER]++;

        if (fShutdown)
            return;

        if (nSelect == SOCKET_ERROR)
        {
            if (have_fds)
            {
                int nErr = WSAGetLastError();
                LogPrintf("%s : socket select error %d\n", __func__, nErr);

                for (unsigned int i = 0; i <= hSocketMax; i++)
                    FD_SET(i, &fdsetRecv);
            }

            FD_ZERO(&fdsetSend);
            FD_ZERO(&fdsetError);
            MilliSleep(timeout.tv_usec / 1000);
        }

        // Accept new connections
        BOOST_FOREACH(SOCKET hListenSocket, vhListenSocket)
        if (hListenSocket != INVALID_SOCKET && FD_ISSET(hListenSocket, &fdsetRecv))
        {
            struct sockaddr_storage sockaddr;
            socklen_t len = sizeof(sockaddr);
            SOCKET hSocket = accept(hListenSocket, (struct sockaddr*)&sockaddr, &len);
            CAddress addr;
            int nInbound = 0;

            if (hSocket != INVALID_SOCKET)
                if (!addr.SetSockAddr((const struct sockaddr*)&sockaddr))
                    LogPrintf("%s : [WARNING] unknown socket family\n", __func__);

            {
                LOCK(cs_vNodes);

                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    if (pnode->fInbound)
                        nInbound++;
                }
            }

            if (hSocket == INVALID_SOCKET)
            {
                int nErr = WSAGetLastError();

                if (nErr != WSAEWOULDBLOCK)
                    LogPrintf("%s : socket error accept failed: %d\n", __func__, nErr);
            }
            else if (nInbound >= GetArg("-maxconnections", 125) - MAX_OUTBOUND_CONNECTIONS)
            {
                CloseSocket(hSocket);
            }
            else if (IsBanned(addr))
            {
                LogPrintf("%s : connection from %s dropped (banned)\n", __func__,
                          addr.ToString().c_str());
                CloseSocket(hSocket);
            }
            else
            {
                LogPrintf("%s : accepted connection %s\n", __func__, addr.ToString().c_str());
                CNode* pnode = new CNode(hSocket, addr, "", true);
                pnode->AddRef();

                {
                    LOCK(cs_vNodes);
                    vNodes.push_back(pnode);
                }
            }
        }

        vector<CNode*> vNodesCopy;

        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;

            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->AddRef();
        }

        BOOST_FOREACH(CNode* pnode, vNodesCopy)
        {
            if (fShutdown)
                return;

            // Receive
            if (pnode->hSocket == INVALID_SOCKET)
                continue;

            if (FD_ISSET(pnode->hSocket, &fdsetRecv) || FD_ISSET(pnode->hSocket, &fdsetError))
            {
                TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);

                if (lockRecv)
                {
                    if (pnode->GetTotalRecvSize() > ReceiveFloodSize())
                    {
                        if (!pnode->fDisconnect)
                            LogPrintf("%s : socket recv flood control disconnect (%u bytes)\n",
                                      __func__, pnode->GetTotalRecvSize());

                        pnode->CloseSocketDisconnect();
                    }
                    else
                    {
                        // typical socket buffer is 8K-64K
                        char pchBuf[0x10000];
                        int nBytes = recv(pnode->hSocket, pchBuf, sizeof(pchBuf), MSG_DONTWAIT);

                        if (nBytes > 0)
                        {
                            if (!pnode->ReceiveMsgBytes(pchBuf, nBytes))
                                pnode->CloseSocketDisconnect();

                            pnode->nLastRecv = GetTime();
                        }
                        else if (nBytes == 0)
                        {
                            // socket closed gracefully
                            if (!pnode->fDisconnect)
                                LogPrintf("%s : socket closed\n", __func__);

                            pnode->CloseSocketDisconnect();
                        }
                        else if (nBytes < 0)
                        {
                            // error
                            int nErr = WSAGetLastError();

                            if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                            {
                                if (!pnode->fDisconnect)
                                    LogPrintf("%s: socket recv error %d\n", __func__, nErr);

                                pnode->CloseSocketDisconnect();
                            }
                        }
                    }
                }
            }

            // Send
            if (pnode->hSocket == INVALID_SOCKET)
                continue;

            if (FD_ISSET(pnode->hSocket, &fdsetSend))
            {
                TRY_LOCK(pnode->cs_vSend, lockSend);

                if (lockSend)
                    SocketSendData(pnode);
            }

            // Inactivity checking

            if (pnode->vSendMsg.empty())
                pnode->nLastSendEmpty = GetTime();

            if (GetTime() - pnode->nTimeConnected > 60)
            {
                if (pnode->nLastRecv == 0 || pnode->nLastSend == 0)
                {
                    LogPrintf("%s : socket no message in first 60 seconds, %d %d\n", __func__,
                              pnode->nLastRecv != 0, pnode->nLastSend != 0);
                    pnode->fDisconnect = true;
                }
                else if (GetTime() - pnode->nLastSend > 90 * 60 && GetTime() - pnode->nLastSendEmpty > 90 * 60)
                {
                    LogPrintf("%s : socket not sending\n", __func__);
                    pnode->fDisconnect = true;
                }
                else if (GetTime() - pnode->nLastRecv > 90 * 60)
                {
                    LogPrintf("%s : socket inactivity timeout\n", __func__);
                    pnode->fDisconnect = true;
                }
            }
        }

        {
            LOCK(cs_vNodes);

            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->Release();
        }

        MilliSleep(10);
    }
}

#ifdef USE_UPNP
void ThreadMapPort(void* parg)
{
    // Make this thread recognisable as the UPnP thread
    RenameThread("neutron-upnp");

    try
    {
        vnThreadsRunning[THREAD_UPNP]++;
        ThreadMapPort2(parg);
        vnThreadsRunning[THREAD_UPNP]--;
    }
    catch (std::exception& e)
    {
        vnThreadsRunning[THREAD_UPNP]--;
        PrintException(&e, __func__);
    }
    catch (...)
    {
        vnThreadsRunning[THREAD_UPNP]--;
        PrintException(NULL, __func__);
    }

    LogPrintf("%s : exited\n", __func__);
}

void ThreadMapPort2(void* parg)
{
    LogPrintf("%s : started\n", __func__);

    std::string port = strprintf("%u", GetListenPort());
    const char * multicastif = 0;
    const char * minissdpdpath = 0;
    struct UPNPDev * devlist = 0;
    char lanaddr[64];

#ifndef UPNPDISCOVER_SUCCESS
    /* miniupnpc 1.5 */
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0);
#elif MINIUPNPC_API_VERSION < 14
    /* miniupnpc 1.6 */
    int error = 0;
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, &error);
#else
    /* miniupnpc 1.9.20150730 */
    int error = 0;
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, 2, &error);
#endif

    struct UPNPUrls urls;
    struct IGDdatas data;
    int r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr));

    if (r == 1)
    {
        if (fDiscover) {
            char externalIPAddress[40];
            r = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, externalIPAddress);

            if(r != UPNPCOMMAND_SUCCESS)
                LogPrintf("%s : GetExternalIPAddress() returned %d\n", __func__, r);
            else
            {
                if(externalIPAddress[0])
                {
                    LogPrintf("%s : externalIPAddress = %s\n", __func__, externalIPAddress);
                    AddLocal(CNetAddr(externalIPAddress), LOCAL_UPNP);
                }
                else
                    LogPrintf("%s : GetExternalIPAddress() failed\n", __func__);
            }
        }

        string strDesc = "Neutron " + FormatFullVersion();

#ifndef UPNPDISCOVER_SUCCESS
        /* miniupnpc 1.5 */
        r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0);
#else
        /* miniupnpc 1.6 */
        r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0, "0");
#endif

        if(r!=UPNPCOMMAND_SUCCESS)
        {
            LogPrintf("%s : AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                      __func__, port.c_str(), port.c_str(), lanaddr, r, strupnperror(r));
        }
        else
            LogPrintf("%s : UPnP port mapping successful.\n", __func__);

        int i = 1;

        while (true)
        {
            if (fShutdown || !fUseUPnP)
            {
                r = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port.c_str(), "TCP", 0);
                LogPrintf("%s : UPNP_DeletePortMapping() returned : %d\n", __func__, r);
                freeUPNPDevlist(devlist); devlist = 0;
                FreeUPNPUrls(&urls);
                return;
            }

            if (i % 600 == 0) // Refresh every 20 minutes
            {
#ifndef UPNPDISCOVER_SUCCESS
                /* miniupnpc 1.5 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                        port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0);
#else
                /* miniupnpc 1.6 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                        port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0, "0");
#endif

                if(r!=UPNPCOMMAND_SUCCESS)
                    LogPrintf("%s : AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                              __func__, port.c_str(), port.c_str(), lanaddr, r, strupnperror(r));
                else
                    LogPrintf("%s : UPnP port mapping successful.\n", __func__);;
            }

            MilliSleep(2000);
            i++;
        }
    }
    else
    {
        LogPrintf("%s : no valid UPnP IGDs found\n", __func__);
        freeUPNPDevlist(devlist); devlist = 0;

        if (r != 0)
            FreeUPNPUrls(&urls);

        while (true)
        {
            if (fShutdown || !fUseUPnP)
                return;

            MilliSleep(2000);
        }
    }
}

void MapPort()
{
    if (fUseUPnP && vnThreadsRunning[THREAD_UPNP] < 1)
    {
        if (!NewThread(ThreadMapPort, NULL))
            LogPrintf("%s : [ERROR] ThreadMapPort(ThreadMapPort) failed\n", __func__);
    }
}
#else
void MapPort()
{
    // Intentionally left blank.
}
#endif

// DNS seeds
// Each pair gives a source name and a seed name.
// The first name is used as information source for addrman.
// The second name should resolve to a list of seed addresses.

struct CDNSSeedData
{
    std::string name, host;
    CDNSSeedData(const std::string &strName, const std::string &strHost) : name(strName), host(strHost) {}
};

std::vector<CDNSSeedData> vSeeds;

const std::vector<CDNSSeedData>& DNSSeeds()
{
    vSeeds.push_back(CDNSSeedData("seed", "seed.neutroncoin.com"));
    vSeeds.push_back(CDNSSeedData("seed1", "seed1.neutroncoin.com"));
    vSeeds.push_back(CDNSSeedData("seed2", "seed2.neutroncoin.com"));
    vSeeds.push_back(CDNSSeedData("seed3", "seed3.neutroncoin.com"));
    return vSeeds;
}

void CConnman::ThreadDNSAddressSeed()
{
    LogPrintf("%s : started\n", __func__);

    // Make this thread recognisable as the DNS seeding thread
    RenameThread("neutron-dnsseed");

    try
    {
        vnThreadsRunning[THREAD_DNSSEED]++;
        const vector<CDNSSeedData> &vSeeds = DNSSeeds();
        int found = 0;

        if (!fTestNet)
        {
            LogPrintf("%s : loading addresses from DNS seeds (could take a while)\n", __func__);

            BOOST_FOREACH (const CDNSSeedData& seed, vSeeds)
            {
                if (HaveNameProxy())
                {
                    LogPrintf("%s : trying %s using proxy\n", __func__, seed.host);
                    AddOneShot(seed.host);
                }
                else
                {
                    LogPrintf("%s : trying %s (%s)\n", __func__, seed.host, seed.name);

                    vector<CNetAddr> vaddr;
                    vector<CAddress> vAdd;

                    if (LookupHost(seed.host.c_str(), vaddr, 0, true))
                    {
                        LogPrintf("%s : found %d addresses from %s\n", __func__, vaddr.size(), seed.host);

                        BOOST_FOREACH(CNetAddr& ip, vaddr)
                        {
                            int nOneDay = 24 * 3600;
                            CAddress addr = CAddress(CService(ip, GetDefaultPort()));
                            addr.nTime = GetTime() - 3 * nOneDay - GetRand(4 * nOneDay); // use a random age between 3 and 7 days old
                            vAdd.push_back(addr);
                            found++;
                        }
                    }

                    addrman.Add(vAdd, CNetAddr(seed.name, true));
                }
            }
        }

        vnThreadsRunning[THREAD_DNSSEED]--;
    }
    catch (std::exception& e)
    {
        vnThreadsRunning[THREAD_DNSSEED]--;
        PrintException(&e, __func__);
    }
    catch (...)
    {
        vnThreadsRunning[THREAD_DNSSEED]--;
        throw; // support pthread_cancel()
    }

    LogPrintf("%s : exited\n", __func__);
}

unsigned int pnSeed[] = { };

void CConnman::DumpAddresses()
{
    int64_t nStart = GetTimeMillis();

    CAddrDB adb;
    adb.Write(addrman);

    LogPrintf("%s : flushed %d addresses to peers.dat  %dms\n",
              __func__, addrman.size(), GetTimeMillis() - nStart);
}

void CConnman::DumpData()
{
    DumpAddresses();
    DumpBanlist();
}

void CConnman::ThreadOpenConnections()
{
    // Make this thread recognisable as the connection opening thread
    RenameThread("neutron-opencon");

    try
    {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
        ThreadOpenConnections2();
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
    }
    catch (std::exception& e)
    {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        PrintException(&e, __func__);
    }
    catch (...)
    {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        PrintException(NULL, __func__);
    }

    LogPrintf("%s : exited\n", __func__);
}

void CConnman::ProcessOneShot()
{
    string strDest;
    {
        LOCK(cs_vOneShots);

        if (vOneShots.empty())
            return;

        strDest = vOneShots.front();
        vOneShots.pop_front();
    }

    CAddress addr;
    CSemaphoreGrant grant(*semOutbound, true);

    if (grant) {
        if (!OpenNetworkConnection(addr, &grant, strDest.c_str(), true))
            AddOneShot(strDest);
    }
}

void CConnman::ThreadStakeMiner(CWallet *pwallet)
{
    LogPrintf("%s : started\n", __func__);

    try
    {
        vnThreadsRunning[THREAD_STAKE_MINER]++;
        StakeMiner(pwallet, true);
        vnThreadsRunning[THREAD_STAKE_MINER]--;
    }
    catch (std::exception& e)
    {
        vnThreadsRunning[THREAD_STAKE_MINER]--;
        PrintException(&e, __func__);
    }
    catch (...)
    {
        vnThreadsRunning[THREAD_STAKE_MINER]--;
        PrintException(NULL, __func__);
    }

    LogPrintf("%s : exiting, %d threads remaining\n", __func__, vnThreadsRunning[THREAD_STAKE_MINER]);
}

void CConnman::ThreadOpenConnections2()
{
    LogPrintf("%s : started\n", __func__);

    // Connect to specific addresses
    if (mapMultiArgs.count("-connect") && mapMultiArgs.at("-connect").size() > 0)
    {
        for (int64_t nLoop = 0;; nLoop++)
        {
            ProcessOneShot();
            BOOST_FOREACH(const std::string& strAddr, mapMultiArgs.at("-connect"))
            {
                CAddress addr;
                OpenNetworkConnection(addr, NULL, strAddr.c_str());
                for (int i = 0; i < 10 && i < nLoop; i++)
                {
                    if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
                        return;
                }
            }
            if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
                return;
        }
    }

    // Initiate network connections
    int64_t nStart = GetTime();

    // Minimum time before next feeler connection (in microseconds).
    int64_t nNextFeeler = PoissonNextSend(nStart*1000 * 1000, FEELER_INTERVAL);
    while (!interruptNet)
    {
        ProcessOneShot();

        if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
            return;

        CSemaphoreGrant grant(*semOutbound);
        if (interruptNet)
            return;

        // Add seed nodes
        if (addrman.size()==0 && (GetTime() - nStart > 60) && !fTestNet)
        {
            std::vector<CAddress> vAdd;
            for (unsigned int i = 0; i < ARRAYLEN(pnSeed); i++)
            {
                // It'll only connect to one or two seed nodes because once it connects,
                // it'll get a pile of addresses with newer timestamps.
                // Seed nodes are given a random 'last seen time' of between one and two
                // weeks ago.
                const int64_t nOneWeek = 7 * 24 * 60 * 60;
                struct in_addr ip;

                memcpy(&ip, &pnSeed[i], sizeof(ip));
                CAddress addr(CService(ip, GetDefaultPort()));
                addr.nTime = GetTime()-GetRand(nOneWeek)-nOneWeek;
                vAdd.push_back(addr);
            }

            addrman.Add(vAdd, CNetAddr("127.0.0.1"));
        }

        // Choose an address to connect to based on most recently seen
        CAddress addrConnect;

        // Only connect out to one peer per network group (/16 for IPv4).
        // Do this here so we don't have to critsect vNodes inside mapAddresses critsect.
        // This is only done for mainnet and testnet
        int nOutbound = 0;
        std::set<std::vector<unsigned char> > setConnected;

        {
            LOCK(cs_vNodes);

            BOOST_FOREACH(CNode* pnode, vNodes)
            {
                if (!pnode->fInbound && !pnode->fMasternode)
                {
                    setConnected.insert(pnode->addr.GetGroup());
                    nOutbound++;
                }
            }
        }

        // Feeler Connections
        //
        // Design goals:
        //  * Increase the number of connectable addresses in the tried table.
        //
        // Method:
        //  * Choose a random address from new and attempt to connect to it if we can connect
        //    successfully it is added to tried.
        //  * Start attempting feeler connections only after node finishes making outbound
        //    connections.
        //  * Only make a feeler connection once every few minutes.
        //
        if (nOutbound >= nMaxOutbound)
        {
            int64_t nTime = GetTimeMicros(); // The current time right now (in microseconds)

            if (nTime > nNextFeeler)
                nNextFeeler = PoissonNextSend(nTime, FEELER_INTERVAL);
            else
                continue;
        }

        int64_t nANow = GetAdjustedTime();
        int nTries = 0;

        while (!interruptNet)
        {
            // use an nUnkBias between 10 (no outgoing connections) and 90 (8 outgoing connections)
            CAddress addr = addrman.Select(10 + min(nOutbound, 8) * 10);

            // if we selected an invalid address, restart
            if (!addr.IsValid() || setConnected.count(addr.GetGroup()) || IsLocal(addr))
                break;

            // If we didn't find an appropriate destination after trying 100 addresses fetched from addrman,
            // stop this loop, and let the outer loop run again (which sleeps, adds seed nodes, recalculates
            // already-connected network ranges, ...) before trying new addrman addresses.
            nTries++;

            if (nTries > 100)
                break;

            if (IsLimited(addr))
                continue;

            // only consider very recently tried nodes after 30 failed attempts
            if (nANow - addr.nLastTry < 600 && nTries < 30)
                continue;

            // do not allow non-default ports, unless after 50 invalid addresses selected already
            //if (addr.GetPort() != GetDefaultPort() && nTries < 50)
            //    continue;

            addrConnect = addr;
            break;
        }

        if (addrConnect.IsValid())
            OpenNetworkConnection(addrConnect, &grant);
    }
}

void CConnman::ThreadOpenAddedConnections()
{
    // Make this thread recognisable as the connection opening thread
    RenameThread("neutron-opencon");

    try
    {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]++;
        ThreadOpenAddedConnections2();
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
    }
    catch (std::exception& e)
    {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        PrintException(&e, __func__);
    }
    catch (...)
    {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        PrintException(NULL, __func__);
    }

    LogPrintf("%s : exited\n", __func__);
}

void CConnman::ThreadOpenAddedConnections2()
{
    LogPrintf("%s : started\n", __func__);

    if (mapArgs.count("-addnode") == 0)
        return;

    if (HaveNameProxy())
    {
        while(!fShutdown)
        {
            BOOST_FOREACH(const std::string& strAddNode, mapMultiArgs["-addnode"])
            {
                CAddress addr;
                CSemaphoreGrant grant(*semOutbound);

                OpenNetworkConnection(addr, &grant, strAddNode.c_str());
                MilliSleep(500);
            }

            vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
            MilliSleep(120000); // retry every 2 minutes
            vnThreadsRunning[THREAD_ADDEDCONNECTIONS]++;
        }

        return;
    }

    vector<vector<CService> > vservAddressesToAdd(0);

    BOOST_FOREACH(string& strAddNode, mapMultiArgs["-addnode"])
    {
        vector<CService> vservNode(0);

        if(Lookup(strAddNode.c_str(), vservNode, GetDefaultPort(), fNameLookup, 0))
        {
            vservAddressesToAdd.push_back(vservNode);
            {
                LOCK(cs_setservAddNodeAddresses);

                BOOST_FOREACH(CService& serv, vservNode)
                    setservAddNodeAddresses.insert(serv);
            }
        }
    }

    while (true)
    {
        vector<vector<CService> > vservConnectAddresses = vservAddressesToAdd;

        // Attempt to connect to each IP for each addnode entry until at least one is successful per addnode entry
        // (keeping in mind that addnode entries can have many IPs if fNameLookup)
        {
            LOCK(cs_vNodes);

            BOOST_FOREACH(CNode* pnode, vNodes)
            {
                for (vector<vector<CService> >::iterator it = vservConnectAddresses.begin(); it != vservConnectAddresses.end(); it++)
                {
                    BOOST_FOREACH(CService& addrNode, *(it))
                    {
                        if (pnode->addr == addrNode)
                        {
                            it = vservConnectAddresses.erase(it);
                            it--;

                            break;
                        }
                    }
                }
            }
        }

        BOOST_FOREACH(vector<CService>& vserv, vservConnectAddresses)
        {
            CSemaphoreGrant grant(*semOutbound);
            OpenNetworkConnection(CAddress(*(vserv.begin())), &grant);
            MilliSleep(500);

            if (fShutdown)
                return;
        }

        if (fShutdown)
            return;

        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        MilliSleep(120000); // retry every 2 minutes
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]++;

        if (fShutdown)
            return;
    }
}

// NTRN TODO: create CConnman class and move this method there eventually
// if successful, this moves the passed grant to the constructed node
bool CConnman::OpenNetworkConnection(const CAddress& addrConnect, CSemaphoreGrant *grantOutbound,
                                     const char *strDest, bool fOneShot)
{
    // Initiate outbound network connection
    if (fShutdown)
        return false;

    if (!strDest)
    {
        if (IsLocal(addrConnect) ||
            FindNode((CNetAddr)addrConnect) || IsBanned(addrConnect) ||
            FindNode(addrConnect.ToStringIPPort().c_str()))
            return false;
    }

    if (strDest && FindNode(strDest))
        return false;

    vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
    CNode* pnode = ConnectNode(addrConnect, strDest);
    vnThreadsRunning[THREAD_OPENCONNECTIONS]++;

    if (fShutdown)
        return false;

    if (!pnode)
        return false;

    if (grantOutbound)
        grantOutbound->MoveTo(pnode->grantOutbound);

    pnode->fNetworkNode = true;

    if (fOneShot)
        pnode->fOneShot = true;

    return true;
}

void ThreadMessageHandler(void* parg)
{
    // Make this thread recognisable as the message handling thread
    RenameThread("neutron-msghand");

    try
    {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]++;
        ThreadMessageHandler2(parg);
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
    }
    catch (std::exception& e)
    {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
        PrintException(&e, __func__);
    }
    catch (...)
    {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
        PrintException(NULL, __func__);
    }

    LogPrintf("%s : exited\n", __func__);
}

void ThreadMessageHandler2(void* parg)
{
    boost::mutex condition_mutex;
    boost::unique_lock<boost::mutex> lock(condition_mutex);
    SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL);

    while (!fShutdown)
    {
        vector<CNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;

            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->AddRef();
        }

        // Poll the connected nodes for messages
        CNode* pnodeTrickle = NULL;

        if (!vNodesCopy.empty())
            pnodeTrickle = vNodesCopy[GetRand(vNodesCopy.size())];

        bool fSleep = true;

        BOOST_FOREACH(CNode* pnode, vNodesCopy)
        {
            if (pnode->fDisconnect)
                continue;

            // Receive messages
            if (!fShutdown)
            {
                TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                TRY_LOCK(cs_Shutdown, lockShutdown);

                if (lockRecv && lockShutdown)
                {
                    if (!ProcessMessages(pnode))
                        pnode->CloseSocketDisconnect();

                    if (pnode->nSendSize < SendBufferSize())
                    {
                        if (!pnode->vRecvGetData.empty() || (!pnode->vRecvMsg.empty() &&
                            pnode->vRecvMsg[0].complete()))
                        {
                            fSleep = false;
                        }
                    }
                }
            }
            else
                return;

            boost::this_thread::interruption_point();

            // Send messages
            if (!fShutdown)
            {
                TRY_LOCK(pnode->cs_vSend, lockSend);
                TRY_LOCK(cs_Shutdown, lockShutdown);

                if (lockSend && lockShutdown)
                    SendMessages(pnode, pnode == pnodeTrickle);
            }
            else
                return;

            boost::this_thread::interruption_point();
        }

        //TODO: Why is the copy locking the main array?
        {
            LOCK(cs_vNodes);

            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->Release();
        }

        if (fSleep)
        {
            messageHandlerCondition.timed_wait(lock, boost::posix_time::microsec_clock::universal_time() +
                                               boost::posix_time::milliseconds(250));
        }
    }
}

bool BindListenPort(const CService &addrBind, string& strError)
{
    strError = "";
    int nOne = 1;

#ifdef WIN32
    // Initialize Windows Sockets
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2,2), &wsadata);
    if (ret != NO_ERROR)
    {
        strError = strprintf("%s : [ERROR] TCP/IP socket library failed to start "
                             "(WSAStartup returned error %d)", __func__, ret);
        LogPrintf("%s\n", strError.c_str());
        return false;
    }
#endif

    // Create socket for listening for incoming connections
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    if (!addrBind.GetSockAddr((struct sockaddr*)&sockaddr, &len))
    {
        strError = strprintf("%s : [ERROR] bind address family for %s not supported",
                             __func__, addrBind.ToString().c_str());
        LogPrintf("%s\n", strError.c_str());
        return false;
    }

    SOCKET hListenSocket = socket(((struct sockaddr*)&sockaddr)->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (hListenSocket == INVALID_SOCKET)
    {
        strError = strprintf("%s : [ERROR] couldn't open socket for incoming connections "
                             "(socket returned error %d)", __func__, WSAGetLastError());
        LogPrintf("%s\n", strError.c_str());
        return false;
    }

#ifdef SO_NOSIGPIPE
    // Different way of disabling SIGPIPE on BSD
    setsockopt(hListenSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&nOne, sizeof(int));
#endif

#ifndef WIN32
    // Allow binding if the port is still in TIME_WAIT state after
    // the program was closed and restarted.  Not an issue on windows.
    setsockopt(hListenSocket, SOL_SOCKET, SO_REUSEADDR, (void*)&nOne, sizeof(int));
#endif

#ifdef WIN32
    // Set to non-blocking, incoming connections will also inherit this
    if (ioctlsocket(hListenSocket, FIONBIO, (u_long*)&nOne) == SOCKET_ERROR)
#else
    if (fcntl(hListenSocket, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
#endif
    {
        strError = strprintf("%s : [ERROR] couldn't set properties on socket for incoming "
                             "connections (error %d)", __func__, WSAGetLastError());
        LogPrintf("%s\n", strError.c_str());
        return false;
    }

    // some systems don't have IPV6_V6ONLY but are always v6only; others do have the option
    // and enable it by default or not. Try to enable it, if possible.
    if (addrBind.IsIPv6())
    {
#ifdef IPV6_V6ONLY
#ifdef WIN32
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&nOne, sizeof(int));
#else
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&nOne, sizeof(int));
#endif
#endif
#ifdef WIN32
        int nProtLevel = 10 /* PROTECTION_LEVEL_UNRESTRICTED */;
        int nParameterId = 23 /* IPV6_PROTECTION_LEVEl */;
        // this call is allowed to fail
        setsockopt(hListenSocket, IPPROTO_IPV6, nParameterId, (const char*)&nProtLevel, sizeof(int));
#endif
    }

    if (::bind(hListenSocket, (struct sockaddr*)&sockaddr, len) == SOCKET_ERROR)
    {
        int nErr = WSAGetLastError();

        if (nErr == WSAEADDRINUSE)
            strError = strprintf(_("%s : unable to bind to %s on this computer. Neutron is probably "
                                   "already running."), __func__, addrBind.ToString().c_str());
        else
            strError = strprintf(_("%s : unable to bind to %s on this computer (bind returned error "
                                   "%d, %s)"), __func__, addrBind.ToString().c_str(), nErr, strerror(nErr));
        LogPrintf("%s\n", strError.c_str());
        return false;
    }
    LogPrintf("Bound to %s\n", addrBind.ToString().c_str());

    // Listen for incoming connections
    if (listen(hListenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        strError = strprintf("%s : [ERROR] listening for incoming connections failed (listen "
                             "returned error %d)", __func__, WSAGetLastError());
        LogPrintf("%s\n", strError.c_str());
        return false;
    }

    vhListenSocket.push_back(hListenSocket);

    if (addrBind.IsRoutable() && fDiscover)
        AddLocal(addrBind, LOCAL_BIND);

    return true;
}

void Discover()
{
    if (!fDiscover)
        return;

#ifdef WIN32
    // Get local host IP
    char pszHostName[256] = "";

    if (gethostname(pszHostName, sizeof(pszHostName)) != SOCKET_ERROR)
    {
        std::vector<CNetAddr> vaddr;
        if (LookupHost(pszHostName, vaddr, 0, true))
        {
            for (const CNetAddr &addr : vaddr)
            {
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: %s - %s\n", __func__, pszHostName, addr.ToString());
            }
        }
    }
#else
    // Get local host ip
    struct ifaddrs* myaddrs;

    if (getifaddrs(&myaddrs) == 0)
    {
        for (struct ifaddrs* ifa = myaddrs; ifa != nullptr; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == nullptr) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0) continue;
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;
            if (strcmp(ifa->ifa_name, "lo0") == 0) continue;

            if (ifa->ifa_addr->sa_family == AF_INET)
            {
                struct sockaddr_in* s4 = (struct sockaddr_in*)(ifa->ifa_addr);
                CNetAddr addr(s4->sin_addr);
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: IPv4 %s: %s\n", __func__, ifa->ifa_name, addr.ToString());
            }
            else if (ifa->ifa_addr->sa_family == AF_INET6)
            {
                struct sockaddr_in6* s6 = (struct sockaddr_in6*)(ifa->ifa_addr);
                CNetAddr addr(s6->sin6_addr);
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: IPv6 %s: %s\n", __func__, ifa->ifa_name, addr.ToString());
            }
        }

        freeifaddrs(myaddrs);
    }
#endif
}

CConnman::CConnman(uint64_t nSeed0In, uint64_t nSeed1In) :
        nSeed0(nSeed0In), nSeed1(nSeed1In)
{
    // fNetworkActive = true;
    setBannedIsDirty = false;
    fAddressesInitialized = false;
    // nLastNodeId = 0;
    // nSendBufferMaxSize = 0;
    // nReceiveFloodSize = 0;
    semOutbound = NULL;
    semAddnode = NULL;
    // semMasternodeOutbound = NULL;
    nMaxConnections = 0;
    nMaxOutbound = 0;
    nMaxAddnode = 0;
    // nBestHeight = 0;
    // clientInterface = NULL;
    flagInterruptMsgProc = false;
}

CConnman::~CConnman()
{
    Interrupt();
    Stop();
}

bool CConnman::Start(CScheduler& scheduler, Options connOptions)
{
    // Make this thread recognisable as the startup thread
    RenameThread("neutron-start");

    nMaxConnections = connOptions.nMaxConnections;
    nMaxOutbound = std::min((connOptions.nMaxOutbound), nMaxConnections);
    nMaxAddnode = connOptions.nMaxAddnode;
    nMaxFeeler = connOptions.nMaxFeeler;

    clientInterface = &uiInterface;

    if (clientInterface)
        uiInterface.InitMessage(_("Loading addresses..."));

    // Load addresses for peers.dat
    int64_t nStart = GetTimeMillis();
    {
        CAddrDB adb;
        if (adb.Read(addrman))
            LogPrintf("%s : loaded %i addresses from peers.dat  %dms\n", __func__, addrman.size(), GetTimeMillis() - nStart);
        else
        {
            // addrman.Clear(); // Addrman can be in an inconsistent state after failure, reset it
            LogPrintf("%s : invalid or missing peers.dat; recreating\n", __func__);
            DumpAddresses();
        }
    }

    if (clientInterface)
        clientInterface->InitMessage(_("Loading banlist..."));

    // Load addresses from banlist.dat
    nStart = GetTimeMillis();
    CBanDB bandb;
    banmap_t banmap;

    if (bandb.Read(banmap))
    {
        SetBanned(banmap); // thread save setter
        SetBannedSetDirty(false); // no need to write down, just read data
        SweepBanned(); // sweep out unused entries

        LogPrintf("%s : loaded %d banned node ips/subnets from banlist.dat %dms\n",
                  __func__, banmap.size(), GetTimeMillis() - nStart);
    }
    else
    {
        LogPrintf("%s : invalid or missing banlist.dat; recreating\n", __func__);
        SetBannedSetDirty(true); // force write
        DumpBanlist();
    }

    uiInterface.InitMessage(_("Starting network threads..."));
    fAddressesInitialized = true;

    if (semOutbound == NULL)
        semOutbound = new CSemaphore(std::min((nMaxOutbound + nMaxFeeler), nMaxConnections));

    if (semAddnode == NULL)
        semAddnode = new CSemaphore(nMaxAddnode);

    if (pnodeLocalHost == NULL)
        pnodeLocalHost = new CNode(INVALID_SOCKET, CAddress(CService("127.0.0.1", 0), nLocalServices));

    Discover();

    // Start threads
    InterruptSocks5(false);
    interruptNet.reset();
    flagInterruptMsgProc = false;

    // Send and receive from sockets, accept connections
    // threadSocketHandler = std::thread(&TraceThread<std::function<void()> >, "net",
    //                       std::function<void()>(std::bind(&CConnman::ThreadSocketHandler, this)));

    if (!GetBoolArg("-dnsseed", true))
        LogPrintf("%s : DNS seeding disabled\n", __func__);
    else
    {
        threadDNSAddressSeed = std::thread(&TraceThread<std::function<void()> >, "dnsseed",
                               std::function<void()>(std::bind(&CConnman::ThreadDNSAddressSeed, this)));
    }

    // Map ports with UPnP
    if (fUseUPnP)
        MapPort();

    // Send and receive from sockets, accept connections
    threadSocketHandler = std::thread(&TraceThread<std::function<void()> >, "net",
                          std::function<void()>(std::bind(&CConnman::ThreadSocketHandler, this)));

    // Initiate outbound connections from -addnode
    threadOpenAddedConnections = std::thread(&TraceThread<std::function<void()> >, "addcon",
                                 std::function<void()>(std::bind(&CConnman::ThreadOpenAddedConnections, this)));

    // Initiate outbound connections unless connect=0
    if (!mapMultiArgs.count("-connect") || mapMultiArgs.at("-connect").size() != 1 ||
        mapMultiArgs.at("-connect")[0] != "0")
    {
        threadOpenConnections = std::thread(&TraceThread<std::function<void()> >, "opencon",
                                std::function<void()>(std::bind(&CConnman::ThreadOpenConnections, this)));
    }

    // NTRN TODO: convert this to use std::thread
    // Process messages
    if (!NewThread(ThreadMessageHandler, NULL))
        LogPrintf("%s : [ERROR] NewThread(ThreadMessageHandler) failed\n", __func__);

    // Mine proof-of-stake blocks in the background
    if (!GetBoolArg("-staking", true))
        LogPrintf("%s : staking disabled\n", __func__);
    else
    {
        threadStakeMiner = std::thread(&TraceThread<std::function<void()> >, "stakeminer",
                           std::function<void()>(std::bind(&CConnman::ThreadStakeMiner, this, pwalletMain)));
    }

    // Dump network addresses
    scheduler.scheduleEvery(boost::bind(&CConnman::DumpData, this), DUMP_ADDRESSES_INTERVAL);
    return true;
}

void CConnman::Interrupt()
{
    LogPrintf("%s : started\n", __func__);

    {
        std::lock_guard<std::mutex> lock(mutexMsgProc);
        flagInterruptMsgProc = true;
    }

    messageHandlerCondition.notify_all();
    interruptNet();
    InterruptSocks5(true);

    if (semOutbound)
    {
        for (int i=0; i<(nMaxOutbound + nMaxFeeler); i++)
        {
            semOutbound->post();
        }
   }

    if (semAddnode)
    {
        for (int i=0; i<nMaxAddnode; i++)
        {
            semAddnode->post();
        }
    }

    LogPrintf("%s : finished\n", __func__);
}

void CConnman::Stop()
{
    LogPrintf("%s : started\n", __func__);

    // LogPrintf("%s : joining threadMessageHandler\n", __func__);
    // if (threadMessageHandler.joinable())
    //     threadMessageHandler.join();

    LogPrintf("%s : joining threadOpenConnections\n", __func__);

    if (threadOpenConnections.joinable())
        threadOpenConnections.join();

    LogPrintf("%s : joining threadOpenAddedConnections\n", __func__);

    if (threadOpenAddedConnections.joinable())
        threadOpenAddedConnections.join();

    LogPrintf("%s : joining threadDNSAddressSeed\n", __func__);

    if (threadDNSAddressSeed.joinable())
        threadDNSAddressSeed.join();

    LogPrintf("%s : joining threadSocketHandler\n", __func__);

    if (threadSocketHandler.joinable())
        threadSocketHandler.join();

    LogPrintf("%s : joining threadStakeMiner\n", __func__);

    if (threadStakeMiner.joinable())
        threadStakeMiner.join();

    LogPrintf("%s : dumping data\n", __func__);

    if (fAddressesInitialized)
    {
        DumpData();
        fAddressesInitialized = false;
    }

    LogPrintf("%s : closing sockets\n", __func__);

    // Close sockets
    {
        LOCK(cs_vNodes);

        for (CNode* pnode : vNodes)
            pnode->CloseSocketDisconnect();
    }

    for (SOCKET hListenSocket : vhListenSocket)
    {
        if (hListenSocket != INVALID_SOCKET)
        {
            if (!CloseSocket(hListenSocket))
                LogPrintf("%s : closing sockets failed with error %d\n", __func__, WSAGetLastError());
        }
    }

    // clean up some globals (to help leak detection)
    // for (CNode *pnode : vNodes) {
    //     DeleteNode(pnode);
    // }
    // for (CNode *pnode : vNodesDisconnected) {
    //     DeleteNode(pnode);
    // }

    {
        LOCK(cs_vNodes);
        vNodes.clear();
    }

    // vNodesDisconnected.clear();
    // vhListenSocket.clear();

    delete semOutbound;
    semOutbound = NULL;
    delete semAddnode;
    semAddnode = NULL;

    // int64_t nStart = GetTime();
    // do
    // {
    //     int nThreadsRunning = 0;
    //     for (int n = 0; n < THREAD_MAX; n++)
    //         nThreadsRunning += vnThreadsRunning[n];
    //     if (nThreadsRunning == 0)
    //         break;
    //     if (GetTime() - nStart > 20)
    //         break;
    //     MilliSleep(20);
    // } while(true);

    if (vnThreadsRunning[THREAD_SOCKETHANDLER] > 0)
        LogPrintf("%s : ThreadSocketHandler still running\n", __func__);

    if (vnThreadsRunning[THREAD_OPENCONNECTIONS] > 0)
        LogPrintf("%s : ThreadOpenConnections still running\n", __func__);

    if (vnThreadsRunning[THREAD_MESSAGEHANDLER] > 0)
        LogPrintf("%s : ThreadMessageHandler still running\n", __func__);

    if (vnThreadsRunning[THREAD_RPCLISTENER] > 0)
        LogPrintf("%s : ThreadRPCListener still running\n", __func__);

    if (vnThreadsRunning[THREAD_RPCHANDLER] > 0)
        LogPrintf("%s : ThreadsRPCServer still running\n", __func__);

#ifdef USE_UPNP
    if (vnThreadsRunning[THREAD_UPNP] > 0)
        LogPrintf("%s : ThreadMapPort still running\n", __func__);
#endif

    if (vnThreadsRunning[THREAD_DNSSEED] > 0)
        LogPrintf("%s : ThreadDNSAddressSeed still running\n", __func__);

    if (vnThreadsRunning[THREAD_ADDEDCONNECTIONS] > 0)
        LogPrintf("%s : ThreadOpenAddedConnections still running\n", __func__);

    if (vnThreadsRunning[THREAD_STAKE_MINER] > 0)
        LogPrintf("%s : ThreadStakeMiner still running\n", __func__);

    while (vnThreadsRunning[THREAD_MESSAGEHANDLER] > 0 || vnThreadsRunning[THREAD_RPCHANDLER] > 0)
        MilliSleep(20);

    MilliSleep(50);
    LogPrintf("%s : finished\n", __func__);
}

class CNetCleanup
{
public:
    CNetCleanup() { }
    ~CNetCleanup()
    {
#ifdef WIN32
        // Shutdown Windows Sockets
        WSACleanup();
#endif
    }
}

instance_of_cnetcleanup;

int64_t PoissonNextSend(int64_t nNow, int average_interval_seconds)
{
    return nNow + (int64_t)(log1p(GetRand(1ULL << 48) * -0.0000000000000035527136788 /* -1/2^48 */) *
           average_interval_seconds * -1000000.0 + 0.5);
}

void RelayInv(CInv& inv)
{
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if (pnode->nVersion >= ActiveProtocol())
            pnode->PushInventory(inv);
    }
}

void RelayTransaction(const CTransaction& tx, const uint256& hash)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(10000);
    ss << tx;
    RelayTransaction(tx, hash, ss);
}

void RelayTransaction(const CTransaction& tx, const uint256& hash, const CDataStream& ss)
{
    CInv inv(MSG_TX, hash);
    {
        LOCK(cs_mapRelay);

        // Expire old relay messages
        while (!vRelayExpiration.empty() && vRelayExpiration.front().first < GetTime())
        {
            mapRelay.erase(vRelayExpiration.front().second);
            vRelayExpiration.pop_front();
        }

        // Save original serialized message so newer versions are preserved
        mapRelay.insert(std::make_pair(inv, ss));
        vRelayExpiration.push_back(std::make_pair(GetTime() + 15 * 60, inv));
    }

    RelayInv(inv);
}

void RelayDarkSendFinalTransaction(const int sessionID, const CTransaction& txNew)
{
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
        pnode->PushMessage("dsf", sessionID, txNew);
}

void RelayDarkSendIn(const std::vector<CTxIn>& in, const int64_t& nAmount,
                     const CTransaction& txCollateral, const std::vector<CTxOut>& out)
{
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if ((CNetAddr) darkSendPool.submittedToMasternode != (CNetAddr) pnode->addr)
            continue;

        LogPrintf("%s : found master, relaying message - %s \n", __func__, pnode->addr.ToString().c_str());
        pnode->PushMessage("dsi", in, nAmount, txCollateral, out);
    }
}

void RelayDarkSendStatus(const int sessionID, const int newState, const int newEntriesCount,
                         const int newAccepted, const std::string error)
{
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
        pnode->PushMessage("dssu", sessionID, newState, newEntriesCount, newAccepted, error);
}

void RelayDarkSendElectionEntry(const CTxIn vin, const CService addr, const std::vector<unsigned char> vchSig,
                                const int64_t nNow, const CPubKey pubkey, const CPubKey pubkey2, const int count,
                                const int current, const int64_t lastUpdated, const int protocolVersion)
{
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if (!pnode->fRelayTxes)
            continue;

        pnode->PushMessage(NetMsgType::DSEE, vin, addr, vchSig, nNow, pubkey, pubkey2,
                           count, current, lastUpdated, protocolVersion);
    }
}

void SendDarkSendElectionEntry(const CTxIn vin, const CService addr, const std::vector<unsigned char> vchSig,
                               const int64_t nNow, const CPubKey pubkey, const CPubKey pubkey2, const int count,
                               const int current, const int64_t lastUpdated, const int protocolVersion)
{
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        pnode->PushMessage(NetMsgType::DSEE, vin, addr, vchSig, nNow, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);
    }
}

void RelayDarkSendElectionEntryPing(const CTxIn vin, const std::vector<unsigned char> vchSig,
                                    const int64_t nNow, const bool stop)
{
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if (!pnode->fRelayTxes)
            continue;

        pnode->PushMessage(NetMsgType::DSEEP, vin, vchSig, nNow, stop);
    }
}

void SendDarkSendElectionEntryPing(const CTxIn vin, const std::vector<unsigned char> vchSig, const int64_t nNow, const bool stop)
{
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
        pnode->PushMessage(NetMsgType::DSEEP, vin, vchSig, nNow, stop);
}

void RelayDarkSendCompletedTransaction(const int sessionID, const bool error, const std::string errorMessage)
{
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
        pnode->PushMessage("dsc", sessionID, error, errorMessage);
}


CNode::CNode(SOCKET hSocketIn, CAddress addrIn, std::string addrNameIn, bool fInboundIn) :
    ssSend(SER_NETWORK, INIT_PROTO_VERSION), setAddrKnown(5000)
{
    nServices = 0;
    hSocket = hSocketIn;
    nRecvVersion = INIT_PROTO_VERSION;
    nLastSend = 0;
    nLastRecv = 0;
    nLastSendEmpty = GetTime();
    nTimeConnected = GetTime();
    addr = addrIn;
    addrName = addrNameIn == "" ? addr.ToStringIPPort() : addrNameIn;
    nVersion = 0;
    strSubVer = "";
    fOneShot = false;
    fClient = false; // set by version message
    fInbound = fInboundIn;
    fNetworkNode = false;
    fSuccessfullyConnected = false;
    fDisconnect = false;
    nRefCount = 0;
    nSendSize = 0;
    nSendOffset = 0;
    hashContinue = 0;
    pindexLastGetBlocksBegin = 0;
    hashLastGetBlocksEnd = 0;
    nStartingHeight = -1;
    fGetAddr = false;
    fRelayTxes = false;
    nMisbehavior = 0;
    hashCheckpointKnown = 0;
    setInventoryKnown.max_size(SendBufferSize() / 1000);

    {
        LOCK(cs_nLastNodeId);
        id = nLastNodeId++;
    }

    if (fDebug)
    {
      if (fLogIPs)
          LogPrintf("%s : added connection to %s peer=%d\n", __func__, addrName, id);
      else
          LogPrintf("%s : added connection peer=%d\n", __func__, id);
    }

    // Be shy and don't send version until we hear
    if (hSocket != INVALID_SOCKET && !fInbound)
        PushVersion();
}

CNode::~CNode()
{
    if (hSocket != INVALID_SOCKET)
    {
        CloseSocket(hSocket);
        hSocket = INVALID_SOCKET;
    }
}

void CNode::AskFor(const CInv& inv)
{
    // We're using mapAskFor as a priority queue,
    // the key is the earliest time the request can be sent
    int64_t& nRequestTime = mapAlreadyAskedFor[inv];

    if (fDebugNet)
    {
        LogPrintf("%s : [blockHash = %s] %d (%s)\n", __func__, inv.hash.ToString().c_str(),
                  nRequestTime, DateTimeStrFormat("%H:%M:%S", nRequestTime/1000000).c_str());
    }

    // Make sure not to reuse time indexes to keep things in the same order
    int64_t nNow = (GetTime() - 1) * 1000000;
    static int64_t nLastTime;
    ++nLastTime;
    nNow = std::max(nNow, nLastTime);
    nLastTime = nNow;

    // Each retry is 2 minutes after the last
    nRequestTime = std::max(nRequestTime + 2 * 60 * 1000000, nNow);
    mapAskFor.insert(std::make_pair(nRequestTime, inv));
}

void CNode::BeginMessage(const char* pszCommand) EXCLUSIVE_LOCK_FUNCTION(cs_vSend)
{
    ENTER_CRITICAL_SECTION(cs_vSend);

    assert(ssSend.size() == 0);
    ssSend << CMessageHeader(pszCommand, 0);

    if (fDebug)
        LogPrintf("%s : sending, %s ", __func__, SanitizeString(pszCommand));
}

void CNode::AbortMessage() UNLOCK_FUNCTION(cs_vSend)
{
    ssSend.clear();

    LEAVE_CRITICAL_SECTION(cs_vSend);

    LogPrintf("%s : aborted\n", __func__);
}

void CNode::EndMessage() UNLOCK_FUNCTION(cs_vSend)
{
    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
    {
        LogPrintf("%s : dropping send message\n", __func__);
        AbortMessage();
        return;
    }

    if (ssSend.size() == 0)
        return;

    // Set the size
    unsigned int nSize = ssSend.size() - CMessageHeader::HEADER_SIZE;
    memcpy((char*)&ssSend[CMessageHeader::MESSAGE_SIZE_OFFSET], &nSize, sizeof(nSize));

    // Set the checksum
    uint256 hash = Hash(ssSend.begin() + CMessageHeader::HEADER_SIZE, ssSend.end());
    unsigned int nChecksum = 0;
    memcpy(&nChecksum, &hash, sizeof(nChecksum));
    assert(ssSend.size () >= CMessageHeader::CHECKSUM_OFFSET + sizeof(nChecksum));
    memcpy((char*)&ssSend[CMessageHeader::CHECKSUM_OFFSET], &nChecksum, sizeof(nChecksum));

    if (fDebug) {
        LogPrintf("(%d bytes)\n", nSize);
    }

    std::deque<CSerializeData>::iterator it = vSendMsg.insert(vSendMsg.end(), CSerializeData());
    ssSend.GetAndClear(*it);
    nSendSize += (*it).size();

    // If write queue empty, attempt "optimistic write"
    if (it == vSendMsg.begin())
        SocketSendData(this);

    LEAVE_CRITICAL_SECTION(cs_vSend);
}

//static size_t handle_chunk(void *downloaded, size_t size, size_t nmemb, void *destination)
//{
//    ((std::string *) destination)->append((char *) downloaded);
//    return size * nmemb;
//}
//
//static std::list<ComparableVersion> parse_releases(std::string result)
//{
//    std::regex version_regex("<title>[-A-Za-z ]*([0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?)[-A-Za-z)( ]*</title>");
//    std::sregex_iterator iter(result.begin(), result.end(), version_regex);
//    std::sregex_iterator end;
//    std::list<ComparableVersion> versions;
//
//    while(iter != end)
//    {
//        versions.push_back(ComparableVersion((*iter)[1]));
//        ++iter;
//    }
//
//    versions.sort();
//    return versions;
//}
//
//std::string GetLatestRelease()
//{
//    CURL *curl;
//    CURLcode res;
//    curl = curl_easy_init();
//    std::string result;
//
//    if(curl)
//    {
//        curl_easy_setopt(curl, CURLOPT_URL, NTRNCORE_RELEASES_ATOM_LOCATION);
//        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
//        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, handle_chunk);
//        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
//        res = curl_easy_perform(curl);
//
//        if(res != CURLE_OK)
//        {
//            LogPrintf("Download of Neutron core release list failed: %s\n", curl_easy_strerror(res));
//        }
//
//        curl_easy_cleanup(curl);
//    }
//
//    curl_global_cleanup();
//    std::list<ComparableVersion> versions = parse_releases(result);
//    return versions.empty() ? "unknown" : "v" + versions.back().ToString();
//}
