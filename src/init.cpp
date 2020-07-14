// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdb.h"
#include "walletdb.h"
#include "bitcoinrpc.h"
#include "net.h"
#include "netbase.h"
#include "noui.h"
#include "init.h"
#include "rpc/register.h"
#include "script/standard.h"
#include "scheduler.h"
#include "util.h"
#include "utiltime.h"
#include "ui_interface.h"
#include "checkpoints.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <openssl/crypto.h>

#include "main.h"
#include "version.h"
#include "activemasternode.h"
#include "spork.h"
#include "darksend.h"
#include "masternodeconfig.h"
#include "txdb-leveldb.h"

#ifndef WIN32
#include <signal.h>
#endif

using namespace std;
using namespace boost;

#define PACKAGE_NAME "Neutron"

CWallet* pwalletMain;
bool fConfChange;
bool fEnforceCanonical;
bool fRestartRequested = false; // true: restart false: shutdown
unsigned int nNodeLifespan;
unsigned int nDerivationMethodIndex;
unsigned int nMinerSleep;
bool fUseFastIndex;
enum Checkpoints::CPMode CheckpointsMode;

std::unique_ptr<CConnman> g_connman;
CConnman* shared_connman;

CCriticalSection cs_Shutdown;

// Thread management and startup/shutdown:
//
// The network-processing threads are all part of a thread group
// created by AppInit() or the Qt main() function.
//
// A clean exit happens when StartShutdown() or the SIGTERM
// signal handler sets fRequestShutdown, which triggers
// the DetectShutdownThread(), which interrupts the main thread group.
// DetectShutdownThread() then exits, which causes AppInit() to
// continue (it .joins the shutdown thread).
// Shutdown() is then
// called to clean up database connections, and stop other
// threads that should only be stopped after the main network-processing
// threads have exited.
//
// Note that if running -daemon the parent process returns from AppInit2
// before adding any threads to the threadGroup, so .join_all() returns
// immediately and the parent exits from main().
//
// Shutdown for Qt is very similar, only it uses a QTimer to detect
// fRequestShutdown getting set, and then does the normal Qt
// shutdown thing.

std::atomic<bool> fRequestShutdown(false);

void WaitForShutdown(boost::thread_group* threadGroup)
{
    bool fShutdown = ShutdownRequested();
    // Tell the main threads to shutdown.
    while (!fShutdown)
    {
        MilliSleep(200);
        fShutdown = ShutdownRequested();
    }
    if (threadGroup)
    {
        Interrupt(*threadGroup);
        threadGroup->join_all();
    }
}

void ExitTimeout(void* parg)
{
#ifdef WIN32
    MilliSleep(5000);
    ExitProcess(0);
#endif
}

void StartShutdown()
{
    fRequestShutdown = true;

#ifdef QT_GUI
    // ensure we leave the Qt main loop for a clean GUI exit (Shutdown() is called in bitcoin.cpp afterwards)
    uiInterface.QueueShutdown();
#else
    // Without UI, Shutdown() can simply be started in a new thread
    boost::thread t(Shutdown);
#endif
}

bool ShutdownRequested()
{
    return fRequestShutdown || fRestartRequested;
}

void Interrupt(boost::thread_group& threadGroup)
{
    LogPrintf("%s: in progress...\n", __func__);

    if (g_connman)
        g_connman->Interrupt();
    threadGroup.interrupt_all();
}

/** Preparing steps before shutting down or restarting the wallet */
bool PrepareShutdown()
{
    if (fShutdown)
        return false;
    else
        fShutdown = true;

    LOCK(cs_Shutdown);
    LogPrintf("%s: in progress...\n", __func__);
    RenameThread("neutron-shutoff");

    nTransactionsUpdated++;
    CTxDB().Close();
    bitdb.Flush(false);
    LogPrintf("%s: call ConnMan::reset\n", __func__);
    g_connman.reset();
    LogPrintf("%s: call ConnMan::reset finished\n", __func__);
    bitdb.Flush(true);
    boost::filesystem::remove(GetPidFile());
    UnregisterWallet(pwalletMain);
    delete pwalletMain;
    NewThread(ExitTimeout, NULL);
    MilliSleep(50);

    LogPrintf("neutron exited\n\n");
    return true;
}

void Shutdown()
{
    LogPrintf("%s: in progress...\n", __func__);

    {
        LOCK(cs_Shutdown);

        if (PrepareShutdown())
        {
            MilliSleep(200);
            return;
        }
    }

#ifndef QT_GUI
    exit(0);
#endif

    LogPrintf("%s: done\n", __func__);
    DebugPrintShutdown();
}

/**
 * Signal handlers are very limited in what they are allowed to do, so:
 */
void HandleSIGTERM(int)
{
    fRequestShutdown = true;
}

void HandleSIGHUP(int)
{
    fReopenDebugLog = true;
}

static bool LockDataDirectory(bool probeOnly)
{
    std::string strDataDir = GetDataDir().string();

    // Make sure only a single Neutron Core process is using the data directory.
    boost::filesystem::path pathLockFile = GetDataDir() / ".lock";
    FILE* file = fopen(pathLockFile.string().c_str(), "a"); // empty lock file; created if it doesn't exist.
    if (file) fclose(file);

    try {
        static boost::interprocess::file_lock lock(pathLockFile.string().c_str());
        if (!lock.try_lock()) {
            return InitError(strprintf(_("Cannot obtain a lock on data directory %s. %s is probably already running."), strDataDir, _(PACKAGE_NAME)));
        }
        if (probeOnly) {
            lock.unlock();
        }
    } catch(const boost::interprocess::interprocess_exception& e) {
        return InitError(strprintf(_("Cannot obtain a lock on data directory %s. %s is probably already running.") + " %s.", strDataDir, _(PACKAGE_NAME), e.what()));
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// Start
//

#if !defined(QT_GUI)
bool AppInit(int argc, char* argv[])
{
    boost::thread_group threadGroup;
    CScheduler scheduler;

    bool fRet = false;
    try
    {
        //
        // Parameters
        //
        // If Qt is used, parameters/bitcoin.conf are parsed in qt/bitcoin.cpp's main()
        ParseParameters(argc, argv);
        if (!boost::filesystem::is_directory(GetDataDir(false)))
        {
            fprintf(stderr, "Error: Specified directory does not exist\n");
            Shutdown();
        }
        ReadConfigFile(mapArgs, mapMultiArgs);

        if (mapArgs.count("-?") || mapArgs.count("--help"))
        {
            // First part of help message is specific to bitcoind / RPC client
            std::string strUsage = _("Neutron version") + " " + FormatFullVersion() + "\n\n" +
                _("Usage:") + "\n" +
                  "  neutrond [options]                     " + "\n" +
                  "  neutrond [options] <command> [params]  " + _("Send command to -server or neutrond") + "\n" +
                  "  neutrond [options] help                " + _("List commands") + "\n" +
                  "  neutrond [options] help <command>      " + _("Get help for a command") + "\n";

            strUsage += "\n" + HelpMessage();

            fprintf(stdout, "%s", strUsage.c_str());
            return false;
        }

        // Command-line RPC
        for (int i = 1; i < argc; i++)
            if (!IsSwitchChar(argv[i][0]) && !boost::algorithm::istarts_with(argv[i], "Neutron:"))
                fCommandLine = true;

        if (fCommandLine)
        {
            int ret = CommandLineRPC(argc, argv);
            exit(ret);
        }

        if (!LockDataDirectory(true)) {
            // Detailed error printed inside LockDataDirectory
            return false;
        }

        fRet = AppInit2(threadGroup, scheduler);
    }
    catch (std::exception& e) {
        PrintException(&e, "AppInit()");
    } catch (...) {
        PrintException(NULL, "AppInit()");
    }

    if (!fRet)
    {
        Interrupt(threadGroup);
        // threadGroup.join_all(); was left out intentionally here, because we didn't re-test all of
        // the startup-failure cases to make sure they don't result in a hang due to some
        // thread-blocking-waiting-for-another-thread-during-startup case
    } else {
        WaitForShutdown(&threadGroup);
    }

    Shutdown();
    return fRet;
}

int main(int argc, char* argv[])
{
    bool fRet = false;

    // Connect bitcoind signal handlers
    noui_connect();

    fRet = AppInit(argc, argv);

    if (fRet && fDaemon)
        return 0;

    return 1;
}
#endif

bool static Bind(const CService &addr, bool fError = true) {
    if (IsLimited(addr))
        return false;
    std::string strError;
    if (!BindListenPort(addr, strError)) {
        if (fError)
            return InitError(strError);
        return false;
    }
    return true;
}

// Core-specific options shared between UI and daemon
std::string HelpMessage()
{
    string strUsage = _("Options:") + "\n" +
        "  -?                     " + _("This help message") + "\n" +
        "  -conf=<file>           " + _("Specify configuration file (default: neutron.conf)") + "\n" +
        "  -pid=<file>            " + _("Specify pid file (default: neutrond.pid)") + "\n" +
        "  -datadir=<dir>         " + _("Specify data directory") + "\n" +
        "  -wallet=<dir>          " + _("Specify wallet file (within data directory)") + "\n" +
        "  -dbcache=<n>           " + _("Set database cache size in megabytes (default: 64)") + "\n" +
        "  -dblogsize=<n>         " + _("Set database disk log size in megabytes (default: 100)") + "\n" +
        "  -timeout=<n>           " + _("Specify connection timeout in milliseconds (default: 5000)") + "\n" +
        "  -proxy=<ip:port>       " + _("Connect through socks proxy") + "\n" +
        "  -dns                   " + _("Allow DNS lookups for -addnode, -seednode and -connect") + "\n" +
        "  -port=<port>           " + _("Listen for connections on <port> (default: 32001 or testnet: 25714)") + "\n" +
        "  -maxconnections=<n>    " + _("Maintain at most <n> connections to peers (default: 125)") + "\n" +
        "  -addnode=<ip>          " + _("Add a node to connect to and attempt to keep the connection open") + "\n" +
        "  -connect=<ip>          " + _("Connect only to the specified node(s)") + "\n" +
        "  -seednode=<ip>         " + _("Connect to a node to retrieve peer addresses, and disconnect") + "\n" +
        "  -externalip=<ip>       " + _("Specify your own public address") + "\n" +
        "  -onlynet=<net>         " + _("Only connect to nodes in network <net> (IPv4 & IPv6)") + "\n" +
        "  -discover              " + _("Discover own IP address (default: 1 when listening and no -externalip)") + "\n" +
        "  -listen                " + _("Accept connections from outside (default: 1 if no -proxy or -connect)") + "\n" +
        "  -bind=<addr>           " + _("Bind to given address. Use [host]:port notation for IPv6") + "\n" +
        "  -dnsseed               " + _("Find peers using DNS lookup (default: 1)") + "\n" +
        "  -staking               " + _("Stake your coins to support network and gain reward (default: 1)") + "\n" +
        "  -synctime              " + _("Sync time with other nodes. Disable if time on your system is precise e.g. syncing with NTP (default: 1)") + "\n" +
        "  -cppolicy              " + _("Sync checkpoints policy (default: strict)") + "\n" +
        "  -banscore=<n>          " + _("Threshold for disconnecting misbehaving peers (default: 100)") + "\n" +
        "  -bantime=<n>           " + strprintf(_("Number of seconds to keep misbehaving peers from reconnecting (default: %u)"), DEFAULT_MISBEHAVING_BANTIME) + "\n" +
        "  -maxreceivebuffer=<n>  " + _("Maximum per-connection receive buffer, <n>*1000 bytes (default: 5000)") + "\n" +
        "  -maxsendbuffer=<n>     " + _("Maximum per-connection send buffer, <n>*1000 bytes (default: 1000)") + "\n" +
#ifdef USE_UPNP
#if USE_UPNP
        "  -upnp                  " + _("Use UPnP to map the listening port (default: 1 when listening)") + "\n" +
#else
        "  -upnp                  " + _("Use UPnP to map the listening port (default: 0)") + "\n" +
#endif
#endif
        "  -detachdb              " + _("Detach block and address databases. Increases shutdown time (default: 0)") + "\n" +
        "  -paytxfee=<amt>        " + _("Fee per KB to add to transactions you send") + "\n" +
        "  -mininput=<amt>        " + _("When creating transactions, ignore inputs with value less than this (default: 0.01)") + "\n" +
        "  -maxtipage=<n>         " + strprintf(_("Maximum tip age in seconds to consider node in initial block download (default: %u)"), DEFAULT_MAX_TIP_AGE) + "\n" +
#ifdef QT_GUI
        "  -server                " + _("Accept command line and JSON-RPC commands") + "\n" +
#endif
#if !defined(WIN32) && !defined(QT_GUI)
        "  -daemon                " + _("Run in the background as a daemon and accept commands") + "\n" +
#endif
        "  -testnet               " + _("Use the test network") + "\n" +
        "  -debug                 " + _("Output extra debugging information. Implies all other -debug* options") + "\n" +
        "  -debugnet              " + _("Output extra network debugging information") + "\n" +
        "  -logtimestamps         " + _("Prepend debug output with timestamp") + "\n" +
        "  -shrinkdebugfile       " + _("Shrink debug.log file on client startup (default: 1 when no -debug)") + "\n" +
        "  -printtoconsole        " + _("Send trace/debug info to console instead of debug.log file") + "\n" +
#ifdef WIN32
        "  -printtodebugger       " + _("Send trace/debug info to debugger") + "\n" +
#endif
        "  -rpcuser=<user>        " + _("Username for JSON-RPC connections") + "\n" +
        "  -rpcpassword=<pw>      " + _("Password for JSON-RPC connections") + "\n" +
        "  -rpcport=<port>        " + _("Listen for JSON-RPC connections on <port> (default: 32000 or testnet: 25715)") + "\n" +
        "  -rpcallowip=<ip>       " + _("Allow JSON-RPC connections from specified IP address") + "\n" +
        "  -rpcconnect=<ip>       " + _("Send commands to node running on <ip> (default: 127.0.0.1)") + "\n" +
        "  -blocknotify=<cmd>     " + _("Execute command when the best block changes (%s in cmd is replaced by block hash)") + "\n" +
        "  -walletnotify=<cmd>    " + _("Execute command when a wallet transaction changes (%s in cmd is replaced by TxID)") + "\n" +
        "  -confchange            " + _("Require a confirmations for change (default: 0)") + "\n" +
        "  -enforcecanonical      " + _("Enforce transaction scripts to use canonical PUSH operators (default: 1)") + "\n" +
        "  -alertnotify=<cmd>     " + _("Execute command when a relevant alert is received (%s in cmd is replaced by message)") + "\n" +
        "  -upgradewallet         " + _("Upgrade wallet to latest format") + "\n" +
        "  -keypool=<n>           " + _("Set key pool size to <n> (default: 100)") + "\n" +
        "  -rescan                " + _("Rescan the block chain for missing wallet transactions") + "\n" +
        "  -salvagewallet         " + _("Attempt to recover private keys from a corrupt wallet.dat") + "\n" +
        "  -checkblocks=<n>       " + _("How many blocks to check at startup (default: 500, 0 = all)") + "\n" +
        "  -checklevel=<n>        " + _("How thorough the block verification is (0-6, default: 1)") + "\n" +
        "  -loadblock=<file>      " + _("Imports blocks from external blk000?.dat file") + "\n" +

        "\n" + _("Block creation options:") + "\n" +
        "  -blockminsize=<n>      "   + _("Set minimum block size in bytes (default: 0)") + "\n" +
        "  -blockmaxsize=<n>      "   + _("Set maximum block size in bytes (default: 250000)") + "\n" +
        "  -blockprioritysize=<n> "   + _("Set maximum size of high-priority/low-fee transactions in bytes (default: 27000)") + "\n" +

        "\n" + _("SSL options: (see the Bitcoin Wiki for SSL setup instructions)") + "\n" +
        "  -rpcssl                                  " + _("Use OpenSSL (https) for JSON-RPC connections") + "\n" +
        "  -rpcsslcertificatechainfile=<file.cert>  " + _("Server certificate file (default: server.cert)") + "\n" +
        "  -rpcsslprivatekeyfile=<file.pem>         " + _("Server private key (default: server.pem)") + "\n" +
        "  -rpcsslciphers=<ciphers>                 " + _("Acceptable ciphers (default: TLSv1+HIGH:!SSLv2:!aNULL:!eNULL:!AH:!3DES:@STRENGTH)") + "\n";

    strUsage += "\n" + _("Masternode options:") + "\n";
    strUsage += "  -masternode=<n>            " + _("Enable the client to act as a masternode (0-1, default: 0)") + "\n";
    strUsage += "  -mnconf=<file>             " + _("Specify masternode configuration file (default: masternode.conf)") + "\n";
    strUsage += "  -masternodeprivkey=<n>     " + _("Set the masternode private key") + "\n";
    strUsage += "  -masternodeaddr=<n>        " + _("Set external address:port to get to this masternode (example: address:port)") + "\n";

    strUsage += "\n" + _("Wallet options:") + "\n";
    strUsage += "  -zapwallettxes=<mode>      " + _("Delete all wallet transactions and only recover those parts of the blockchain through -rescan on startup") +
        " " + _("(1 = keep tx meta data e.g. account owner and payment request information, 2 = drop tx meta data)") + "\n";

    return strUsage;
}

namespace { // Variables internal to initialization process only

// ServiceFlags nRelevantServices = NODE_NETWORK;
int nMaxConnections;
int nUserMaxConnections;
int nFD;
// ServiceFlags nLocalServices = NODE_NETWORK;

}

/** Initialize bitcoin.
 *  @pre Parameters should be parsed and config file should be read.
 */
bool AppInit2(boost::thread_group& threadGroup, CScheduler& scheduler)
{
    // ********************************************************* Step 1: setup
#ifdef _MSC_VER
    // Turn off Microsoft heap dump noise
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, CreateFileA("NUL", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0));
#endif
#if _MSC_VER >= 1400
    // Disable confusing "helpful" text message on abort, Ctrl-C
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#ifdef WIN32
    // Enable Data Execution Prevention (DEP)
    // Minimum supported OS versions: WinXP SP3, WinVista >= SP1, Win Server 2008
    // A failure is non-critical and needs no further attention!
#ifndef PROCESS_DEP_ENABLE
// We define this here, because GCCs winbase.h limits this to _WIN32_WINNT >= 0x0601 (Windows 7),
// which is not correct. Can be removed, when GCCs winbase.h is fixed!
#define PROCESS_DEP_ENABLE 0x00000001
#endif
    typedef BOOL (WINAPI *PSETPROCDEPPOL)(DWORD);
    PSETPROCDEPPOL setProcDEPPol = (PSETPROCDEPPOL)GetProcAddress(GetModuleHandleA("Kernel32.dll"), "SetProcessDEPPolicy");
    if (setProcDEPPol != NULL) setProcDEPPol(PROCESS_DEP_ENABLE);
#endif
#ifndef WIN32
    umask(077);

    // Clean shutdown on SIGTERM
    struct sigaction sa;
    sa.sa_handler = HandleSIGTERM;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    // Reopen debug.log on SIGHUP
    struct sigaction sa_hup;
    sa_hup.sa_handler = HandleSIGHUP;
    sigemptyset(&sa_hup.sa_mask);
    sa_hup.sa_flags = 0;
    sigaction(SIGHUP, &sa_hup, NULL);
#endif

    // ********************************************************* Step 2: parameter interactions

    nNodeLifespan = GetArg("-addrlifespan", 7);
    fUseFastIndex = GetBoolArg("-fastindex", true);
    nMinerSleep = GetArg("-minersleep", 500);

    CheckpointsMode = Checkpoints::STRICT;
    std::string strCpMode = GetArg("-cppolicy", "strict");

    if(strCpMode == "strict")
        CheckpointsMode = Checkpoints::STRICT;

    if(strCpMode == "advisory")
        CheckpointsMode = Checkpoints::ADVISORY;

    if(strCpMode == "permissive")
        CheckpointsMode = Checkpoints::PERMISSIVE;

    nDerivationMethodIndex = 0;

    fTestNet = GetBoolArg("-testnet");

    if (mapArgs.count("-bind"))
    {
        // when specifying an explicit binding address, you want to listen on it
        // even when -connect or -proxy is specified
        SoftSetBoolArg("-listen", true);
    }

    if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0)
    {
        // when only connecting to trusted nodes, do not seed via DNS, or listen by default
        SoftSetBoolArg("-dnsseed", false);
        SoftSetBoolArg("-listen", false);
    }

    if (mapArgs.count("-proxy"))
    {
        // to protect privacy, do not listen by default if a proxy server is specified
        SoftSetBoolArg("-listen", false);
    }

    if (!GetBoolArg("-listen", true)) {
        // do not map ports or try to retrieve public IP when not listening (pointless)
        SoftSetBoolArg("-upnp", false);
        SoftSetBoolArg("-discover", false);
    }

    if (mapArgs.count("-externalip"))
    {
        // if an explicit public IP is specified, do not try to find others
        SoftSetBoolArg("-discover", false);
    }

    if (GetBoolArg("-salvagewallet"))
    {
        // Rewrite just private keys: rescan to find transactions
        SoftSetBoolArg("-rescan", true);
    }

    // -zapwallettx implies a rescan
    if (GetBoolArg("-zapwallettxes", false)) {
        if (SoftSetBoolArg("-rescan", true))
            LogPrintf("AppInit2 : parameter interaction: -zapwallettxes=<mode> -> setting -rescan=1\n");
    }


    // Make sure enough file descriptors are available
    int nBind = std::max((mapMultiArgs.count("-bind") ? mapMultiArgs.at("-bind").size() : 0) +
                         (mapMultiArgs.count("-whitebind") ? mapMultiArgs.at("-whitebind").size() : 0), size_t(1));

    nUserMaxConnections = GetArg("-maxconnections", DEFAULT_MAX_PEER_CONNECTIONS);
    nMaxConnections = std::max(nUserMaxConnections, 0);

    // Trim requested connection counts, to fit into system limitations
    nMaxConnections = std::max(std::min(nMaxConnections, (int)(FD_SETSIZE - nBind - MIN_CORE_FILEDESCRIPTORS - MAX_ADDNODE_CONNECTIONS)), 0);
    nFD = RaiseFileDescriptorLimit(nMaxConnections + MIN_CORE_FILEDESCRIPTORS + MAX_ADDNODE_CONNECTIONS);

    if (nFD < MIN_CORE_FILEDESCRIPTORS)
        return InitError(_("Not enough file descriptors available."));

    nMaxConnections = std::min(nFD - MIN_CORE_FILEDESCRIPTORS - MAX_ADDNODE_CONNECTIONS, nMaxConnections);

    if (nMaxConnections < nUserMaxConnections)
    {
        LogPrintf("[AppInit2] Reducing -maxconnections from %d to %d, because of system limitations.",
                  nUserMaxConnections, nMaxConnections);
    }

    // ********************************************************* Step 3: parameter-to-internal-flags

    fDebug = GetBoolArg("-debug");

    // -debug implies fDebug*
    if (fDebug)
        fDebugNet = true;
    else
        fDebugNet = GetBoolArg("-debugnet");

    bitdb.SetDetach(GetBoolArg("-detachdb", false));

#if !defined(WIN32) && !defined(QT_GUI)
    fDaemon = GetBoolArg("-daemon");
#else
    fDaemon = false;
#endif

    if (fDaemon)
        fServer = true;
    else
        fServer = GetBoolArg("-server");

    // Staking needs a CWallet instance, so make sure wallet is enabled
#ifdef ENABLE_WALLET
    bool fDisableWallet = GetBoolArg("-disablewallet", false);
    if (fDisableWallet) {
#endif
        if (SoftSetBoolArg("-staking", false))
            LogPrintf("AppInit2 : parameter interaction: wallet functionality not enabled -> setting -staking=0\n");
#ifdef ENABLE_WALLET
    }
#endif


    fPrintToConsole = GetBoolArg("-printtoconsole");
    fPrintToDebugger = GetBoolArg("-printtodebugger");
    fLogTimestamps = GetBoolArg("-logtimestamps");

    RegisterAllCoreRPCCommands(tableRPC);

    if (mapArgs.count("-timeout"))
    {
        int nNewTimeout = GetArg("-timeout", 5000);

        if (nNewTimeout > 0 && nNewTimeout < 600000)
            nConnectTimeout = nNewTimeout;
    }

    if (mapArgs.count("-paytxfee"))
    {
        if (!ParseMoney(mapArgs["-paytxfee"], nTransactionFee))
            return InitError(strprintf(_("Invalid amount for -paytxfee=<amount>: '%s'"), mapArgs["-paytxfee"].c_str()));

        if (nTransactionFee > 0.25 * COIN)
            InitWarning(_("Warning: -paytxfee is set very high! This is the transaction fee you will pay if you send a transaction."));
    }

    fConfChange = GetBoolArg("-confchange", false);
    fEnforceCanonical = GetBoolArg("-enforcecanonical", true);

    if (mapArgs.count("-mininput"))
    {
        if (!ParseMoney(mapArgs["-mininput"], nMinimumInputValue))
            return InitError(strprintf(_("Invalid amount for -mininput=<amount>: '%s'"), mapArgs["-mininput"].c_str()));
    }

    nMaxTipAge = GetArg("-maxtipage", DEFAULT_MAX_TIP_AGE);


    // ********************************************************* Step 4: application initialization: dir lock, daemonize, pidfile, debug log

    std::string strDataDir = GetDataDir().string();
    std::string strWalletFileName = GetArg("-wallet", "wallet.dat");

    // strWalletFileName must be a plain filename without a directory
    if (strWalletFileName != boost::filesystem::basename(strWalletFileName) +
        boost::filesystem::extension(strWalletFileName))
    {
        return InitError(strprintf(_("Wallet %s resides outside data directory %s."),
                                     strWalletFileName.c_str(), strDataDir.c_str()));
    }

    if (!LockDataDirectory(true))
    {
        // Detailed error printed inside LockDataDirectory
        return false;
    }

    if (fDaemon)
    {
#if !defined(WIN32) && !defined(QT_GUI)
        fprintf(stdout, "Neutron server starting\n");

        // Daemonize
        if (daemon(1, 0))
        {
            LogPrintf("[AppInit2] Error: daemon() failed: %s\n", strerror(errno));
            return false;
        }

        CreatePidFile(GetPidFile(), getpid());
#endif
    }

    if (!LockDataDirectory(false))
        return false;

    if (GetBoolArg("-shrinkdebugfile", !fDebug))
        ShrinkDebugFile();

    LogPrintf("[AppInit2] Neutron version %s (%s)\n", FormatFullVersion().c_str(), CLIENT_DATE.c_str());
    LogPrintf("[AppInit2] Using OpenSSL version %s\n", SSLeay_version(SSLEAY_VERSION));
    LogPrintf("[AppInit2] Using BerkeleyDB version %s\n", DbEnv::version(0, 0, 0));
    LogPrintf("[AppInit2] Using Boost version %s\n", BOOST_VERSION_NUM.c_str());

    if (!fLogTimestamps)
        LogPrintf("[AppInit2] Startup time: %s\n", DateTimeStrFormat("%x %H:%M:%S", GetTime()).c_str());

    LogPrintf("[AppInit2] Default data directory %s\n", GetDefaultDataDir().string().c_str());
    LogPrintf("[AppInit2] Used data directory %s\n", strDataDir.c_str());

    std::ostringstream strErrors;

    if (mapArgs.count("-sporkkey"))
    {
        if (!sporkManager.SetPrivKey(GetArg("-sporkkey", "")))
            return InitError(_("Unable to sign spork message, wrong key?"));
    }

    if (mapArgs.count("-masternodepaymentskey")) // masternode payments priv key
    {
        if (!masternodePayments.SetPrivKey(GetArg("-masternodepaymentskey", "")))
            return InitError(_("Unable to sign masternode payment winner, wrong key?"));

    }

    int64_t nStart;

    // ********************************************************* Step 5: verify database integrity

    uiInterface.InitMessage(_("Verifying database integrity..."));

    if (!bitdb.Open(GetDataDir()))
    {
        string msg = strprintf(_("Error initializing database environment %s!"
                                 " To recover, BACKUP THAT DIRECTORY, then remove"
                                 " everything from it except for wallet.dat."), strDataDir.c_str());
        return InitError(msg);
    }

    if (GetBoolArg("-salvagewallet"))
    {
        // Recover readable keypairs:
        if (!CWalletDB::Recover(bitdb, strWalletFileName, true))
            return false;
    }

    if (filesystem::exists(GetDataDir() / strWalletFileName))
    {
        CDBEnv::VerifyResult r = bitdb.Verify(strWalletFileName, CWalletDB::Recover);

        if (r == CDBEnv::RECOVER_OK)
        {
            string msg = strprintf(_("Warning: wallet.dat corrupt, data salvaged!"
                                     " Original wallet.dat saved as wallet.{timestamp}.bak in %s; if"
                                     " your balance or transactions are incorrect you should"
                                     " restore from a backup."), strDataDir.c_str());

            uiInterface.ThreadSafeMessageBox(msg, _("Neutron"), CClientUIInterface::OK |
                                             CClientUIInterface::ICON_EXCLAMATION |
                                             CClientUIInterface::MODAL);
        }

        if (r == CDBEnv::RECOVER_FAIL)
            return InitError(_("wallet.dat corrupt, salvage failed"));
    }

    // ********************************************************* Step 6: network initialization

    assert(!g_connman);
    g_connman = std::unique_ptr<CConnman>(new CConnman(GetRand(std::numeric_limits<uint64_t>::max()),
                                                       GetRand(std::numeric_limits<uint64_t>::max())));
    CConnman& connman = *g_connman;
    shared_connman = &connman;

    // Check for -socks - as this is a privacy risk to continue, exit here
    if (IsArgSet("-socks"))
    {
        return InitError(_("Unsupported argument -socks found. Setting SOCKS version isn't "
                           "possible anymore, only SOCKS5 proxies are supported."));
    }

    if (mapArgs.count("-onlynet"))
    {
        std::set<enum Network> nets;

        BOOST_FOREACH(std::string snet, mapMultiArgs["-onlynet"])
        {
            enum Network net = ParseNetwork(snet);

            if (net == NET_UNROUTABLE)
                return InitError(strprintf(_("Unknown network specified in -onlynet: '%s'"), snet.c_str()));

            nets.insert(net);
        }

        for (int n = 0; n < NET_MAX; n++)
        {
            enum Network net = (enum Network) n;

            if (!nets.count(net))
                SetLimited(net);
        }
    }

    CService addrProxy;
    bool fProxy = false;

    if (mapArgs.count("-proxy"))
    {
        addrProxy = CService(mapArgs["-proxy"], 9050);

        if (!addrProxy.IsValid())
            return InitError(strprintf(_("Invalid -proxy address: '%s'"), mapArgs["-proxy"].c_str()));

        SetProxy(NET_IPV4, addrProxy);
        SetProxy(NET_IPV6, addrProxy);
        SetNameProxy(addrProxy);
        fProxy = true;
    }

    // -tor can override normal proxy, -notor disables tor entirely
    if (!(mapArgs.count("-tor") && mapArgs["-tor"] == "0") && (fProxy || mapArgs.count("-tor")))
    {
        CService addrOnion;

        if (!mapArgs.count("-tor"))
            addrOnion = addrProxy;
        else
            addrOnion = CService(mapArgs["-tor"], 9050);

        if (!addrOnion.IsValid())
            return InitError(strprintf(_("Invalid -tor address: '%s'"), mapArgs["-tor"].c_str()));

        SetProxy(NET_TOR, addrOnion);
        SetReachable(NET_TOR);
    }

    // see Step 2: parameter interactions for more information about these
    fListen = GetBoolArg("-listen", DEFAULT_LISTEN);
    fDiscover = GetBoolArg("-discover", true);
    fNameLookup = GetBoolArg("-dns", true);

#ifdef USE_UPNP
    fUseUPnP = GetBoolArg("-upnp", USE_UPNP);
#endif

    bool fBound = false;

    if (fListen)
    {
        std::string strError;

        if (mapArgs.count("-bind"))
        {
            BOOST_FOREACH(std::string strBind, mapMultiArgs["-bind"])
            {
                CService addrBind;

                if (!Lookup(strBind.c_str(), addrBind, GetListenPort(), false))
                    return InitError(strprintf(_("Cannot resolve -bind address: '%s'"), strBind.c_str()));

                fBound |= Bind(addrBind);
            }
        }
        else
        {
            struct in_addr inaddr_any;
            inaddr_any.s_addr = INADDR_ANY;

            if (!IsLimited(NET_IPV6))
                fBound |= Bind(CService(in6addr_any, GetListenPort()), false);

            if (!IsLimited(NET_IPV4))
                fBound |= Bind(CService(inaddr_any, GetListenPort()), !fBound);
        }

        if (!fBound)
            return InitError(_("Failed to listen on any port. Use -listen=0 if you want this."));
    }

    if (mapArgs.count("-externalip"))
    {
        BOOST_FOREACH(string strAddr, mapMultiArgs["-externalip"])
        {
            CService addrLocal(strAddr, GetListenPort(), fNameLookup);

            if (!addrLocal.IsValid())
                return InitError(strprintf(_("Cannot resolve -externalip address: '%s'"), strAddr.c_str()));

            AddLocal(CService(strAddr, GetListenPort(), fNameLookup), LOCAL_MANUAL);
        }
    }

    if (mapArgs.count("-reservebalance")) // ppcoin: reserve balance amount
    {
        if (!ParseMoney(mapArgs["-reservebalance"], nReserveBalance))
        {
            InitError(_("Invalid amount for -reservebalance=<amount>"));
            return false;
        }
    }

    if (mapArgs.count("-checkpointkey")) // ppcoin: checkpoint master priv key
    {
        if (!Checkpoints::SetCheckpointPrivKey(GetArg("-checkpointkey", "")))
            InitError(_("Unable to sign checkpoint, wrong checkpointkey?\n"));
    }

    BOOST_FOREACH(const std::string& strDest, mapMultiArgs["-seednode"])
        connman.AddOneShot(strDest);

    // ********************************************************* Step 7: load blockchain

    if (!bitdb.Open(GetDataDir()))
    {
        string msg = strprintf(_("Error initializing database environment %s!"
                                 " To recover, BACKUP THAT DIRECTORY, then remove"
                                 " everything from it except for wallet.dat."), strDataDir.c_str());
        return InitError(msg);
    }

    if (GetBoolArg("-loadblockindextest"))
    {
        CTxDB txdb("r");
        txdb.LoadBlockIndex();
        PrintBlockTree();
        return false;
    }

    uiInterface.InitMessage(_("Loading block index..."));
    nStart = GetTimeMillis();

    if (!LoadBlockIndex())
        return InitError(_("Error loading blkindex.dat"));

    // As LoadBlockIndex can take several minutes, it's possible the user
    // requested to kill the GUI during the last operation. If so, exit.
    // As the program has not fully started yet, Shutdown() is possibly overkill.
    if (fRequestShutdown)
    {
        LogPrintf("[AppInit2] Shutdown requested. Exiting.\n");
        return false;
    }

    LogPrintf("[AppInit2]  block index %15dms\n", GetTimeMillis() - nStart);

    if (GetBoolArg("-printblockindex") || GetBoolArg("-printblocktree"))
    {
        PrintBlockTree();
        return false;
    }
    else
        PrintBlockInfo();

    if (mapArgs.count("-printblock"))
    {
        string strMatch = mapArgs["-printblock"];
        int nFound = 0;

        for (auto mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
        {
            uint256 hash = (*mi).first;

            if (strncmp(hash.ToString().c_str(), strMatch.c_str(), strMatch.size()) == 0)
            {
                CBlockIndex* pindex = (*mi).second;
                CBlock block;
                block.ReadFromDisk(pindex);
                block.BuildMerkleTree();
                block.print();
                LogPrintf("\n");
                nFound++;
            }
        }

        if (nFound == 0)
            LogPrintf("[AppInit2] No blocks matching %s were found\n", strMatch.c_str());

        return false;
    }

    // ********************************************************* Step 8: load wallet

    if (fDisableWallet) {
        pwalletMain = NULL;
        LogPrintf("Wallet disabled!\n");
    } else {
            // needed to restore wallet transaction meta data after -zapwallettxes
            std::vector<CWalletTx> vWtx;

            if (GetBoolArg("-zapwallettxes", false)) {
                uiInterface.InitMessage(_("Zapping all transactions from wallet..."));

                pwalletMain = new CWallet(strWalletFileName);
                DBErrors nZapWalletRet = pwalletMain->ZapWalletTx(vWtx);
                if (nZapWalletRet != DB_LOAD_OK) {
                    uiInterface.InitMessage(_("Error loading wallet.dat: Wallet corrupted"));
                    return false;
                }

                delete pwalletMain;
                pwalletMain = NULL;
            }

    uiInterface.InitMessage(_("Loading wallet..."));
    nStart = GetTimeMillis();
    bool fFirstRun = true;
    pwalletMain = new CWallet(strWalletFileName);
    DBErrors nLoadWalletRet = pwalletMain->LoadWallet(fFirstRun);

    if (nLoadWalletRet != DB_LOAD_OK)
    {
        if (nLoadWalletRet == DB_CORRUPT)
            strErrors << _("Error loading wallet.dat: Wallet corrupted") << "\n";
        else if (nLoadWalletRet == DB_NONCRITICAL_ERROR)
        {
            string msg(_("Warning: error reading wallet.dat! All keys read correctly, but transaction data"
                         " or address book entries might be missing or incorrect."));
            uiInterface.ThreadSafeMessageBox(msg, _("Neutron"), CClientUIInterface::OK | CClientUIInterface::ICON_EXCLAMATION | CClientUIInterface::MODAL);
        }
        else if (nLoadWalletRet == DB_TOO_NEW)
            strErrors << _("Error loading wallet.dat: Wallet requires newer version of Neutron") << "\n";
        else if (nLoadWalletRet == DB_NEED_REWRITE)
        {
            strErrors << _("Wallet needed to be rewritten: restart Neutron to complete") << "\n";
            LogPrintf("[AppInit2] %s", strErrors.str().c_str());
            return InitError(strErrors.str());
        }
        else
            strErrors << _("Error loading wallet.dat") << "\n";
    }

    if (GetBoolArg("-upgradewallet", fFirstRun))
    {
        int nMaxVersion = GetArg("-upgradewallet", 0);

        if (nMaxVersion == 0) // the -upgradewallet without argument case
        {
            LogPrintf("[AppInit2] Performing wallet upgrade to %i\n", FEATURE_LATEST);
            nMaxVersion = CLIENT_VERSION;
            pwalletMain->SetMinVersion(FEATURE_LATEST); // permanently upgrade the wallet immediately
        }
        else
            LogPrintf("[AppInit2] Allowing wallet upgrade up to %i\n", nMaxVersion);

        if (nMaxVersion < pwalletMain->GetVersion())
            strErrors << _("Cannot downgrade wallet") << "\n";

        pwalletMain->SetMaxVersion(nMaxVersion);
    }

    if (fFirstRun)
    {
        // Create new keyUser and set as default key
        RandAddSeedPerfmon();
        CPubKey newDefaultKey;

        if (pwalletMain->GetKeyFromPool(newDefaultKey, false))
        {
            pwalletMain->SetDefaultKey(newDefaultKey);

            if (!pwalletMain->SetAddressBookName(pwalletMain->vchDefaultKey.GetID(), ""))
                strErrors << _("Cannot write default address") << "\n";
        }
    }

    LogPrintf("[AppInit2] %s", strErrors.str().c_str());
    LogPrintf("[AppInit2]  wallet      %15dms\n", GetTimeMillis() - nStart);

    RegisterWallet(pwalletMain);
    CBlockIndex *pindexRescan = pindexBest;

    if (GetBoolArg("-rescan"))
        pindexRescan = pindexGenesisBlock;
    else
    {
        CWalletDB walletdb(strWalletFileName);
        CBlockLocator locator;

        if (walletdb.ReadBestBlock(locator))
            pindexRescan = locator.GetBlockIndex();
    }

    if (pindexBest != pindexRescan && pindexBest && pindexRescan &&
        pindexBest->nHeight > pindexRescan->nHeight)
    {
        uiInterface.InitMessage(_("Rescanning..."));

        LogPrintf("[AppInit2] Rescanning last %i blocks (from block %i)...\n",
                  pindexBest->nHeight - pindexRescan->nHeight, pindexRescan->nHeight);

        nStart = GetTimeMillis();
        pwalletMain->ScanForWalletTransactions(pindexRescan, true);
        LogPrintf("[AppInit2] rescan %15dms\n", GetTimeMillis() - nStart);

        // Restore wallet transaction metadata after -zapwallettxes=1
        if (GetBoolArg("-zapwallettxes", false) && GetArg("-zapwallettxes", "1") != "2") {
            BOOST_FOREACH (const CWalletTx& wtxOld, vWtx) {
                uint256 hash = wtxOld.GetHash();
                std::map<uint256, CWalletTx>::iterator mi = pwalletMain->mapWallet.find(hash);
                if (mi != pwalletMain->mapWallet.end()) {
                    const CWalletTx* copyFrom = &wtxOld;
                    CWalletTx* copyTo = &mi->second;
                    copyTo->mapValue = copyFrom->mapValue;
                    copyTo->vOrderForm = copyFrom->vOrderForm;
                    copyTo->nTimeReceived = copyFrom->nTimeReceived;
                    copyTo->nTimeSmart = copyFrom->nTimeSmart;
                    copyTo->fFromMe = copyFrom->fFromMe;
                    copyTo->strFromAccount = copyFrom->strFromAccount;
                    copyTo->nOrderPos = copyFrom->nOrderPos;
                    copyTo->WriteToDisk();
                }
            }
        }
    }
}
    // ********************************************************* Step 9: import blocks

    if (mapArgs.count("-loadblock"))
    {
        uiInterface.InitMessage(_("Importing blockchain data file."));

        BOOST_FOREACH(string strFile, mapMultiArgs["-loadblock"])
        {
            FILE *file = fopen(strFile.c_str(), "rb");

            if (file)
                LoadExternalBlockFile(file);
        }
        exit(0);
    }

    filesystem::path pathBootstrap = GetDataDir() / "bootstrap.dat";

    if (filesystem::exists(pathBootstrap))
    {
        uiInterface.InitMessage(_("Importing bootstrap blockchain data file."));
        FILE *file = fopen(pathBootstrap.string().c_str(), "rb");

        if (file)
        {
            filesystem::path pathBootstrapOld = GetDataDir() / "bootstrap.dat.old";
            LoadExternalBlockFile(file);
            RenameOver(pathBootstrap, pathBootstrapOld);
        }
    }

    // ********************************************************* Step 10: start node

    if (!CheckDiskSpace())
        return false;

    string mnConfErr = "";

    if (!masternodeConfig.read(mnConfErr))
        return InitError("Masternode Conf Error: " + mnConfErr);

    fMasterNode = GetBoolArg("-masternode", false);

    if (fMasterNode)
    {
        LogPrintf("[AppInit2] IS NEUTRON MASTER NODE\n");
        strMasterNodeAddr = GetArg("-masternodeaddr", "");

        LogPrintf("[AppInit2]  addr %s\n", strMasterNodeAddr.c_str());

        if (!strMasterNodeAddr.empty())
        {
            CService addrTest = CService(strMasterNodeAddr);

            if (!addrTest.IsValid())
                return InitError("Invalid -masternodeaddr address: " + strMasterNodeAddr);

            //if (addrTest.GetPort() != GetDefaultPort())
            //{
            //    std::string errorMessage = strprintf("Invalid -masternodeaddr port %u detected in neutron.conf "
            //                                         "(only %d is supported for mainnet)",
            //                                         addrTest.GetPort(), GetDefaultPort());
            //    return InitError(errorMessage);
            //}
        }

        strMasterNodePrivKey = GetArg("-masternodeprivkey", "");

        if (!strMasterNodePrivKey.empty())
        {
            std::string errorMessage;
            CKey key;
            CPubKey pubkey;

            if (!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, key, pubkey))
                return InitError(_("Invalid masternodeprivkey. Please see documenation."));

            activeMasternode.pubKeyMasternode = pubkey;

        }
        else
        {
            return InitError(_("You must specify a masternodeprivkey in the configuration. "
                               "Please see documentation for help."));
        }
    }

    fEnableDarksend = false;

    /* Denominations
       A note about convertability. Within Darksend pools, each denomination
       is convertable to another.
       For example:
       1Neutron+1000 == (.1Neutron+100)*10
       10Neutron+10000 == (1Neutron+1000)*10
    */
    darkSendDenominations.push_back( (100000      * COIN)+100000000 );
    darkSendDenominations.push_back( (10000       * COIN)+10000000 );
    darkSendDenominations.push_back( (1000        * COIN)+1000000 );
    darkSendDenominations.push_back( (100         * COIN)+100000 );
    darkSendDenominations.push_back( (10          * COIN)+10000 );
    darkSendDenominations.push_back( (1           * COIN)+1000 );
    darkSendDenominations.push_back( (.1          * COIN)+100 );

    /* Disabled till we need them
    darkSendDenominations.push_back( (.01      * COIN)+10 );
    darkSendDenominations.push_back( (.001     * COIN)+1 );
    */

    darkSendPool.InitCollateralAddress();
    threadGroup.create_thread(boost::bind(&ThreadCheckDarkSend, boost::ref(*g_connman)));
    RandAddSeedPerfmon();

    //// debug print
    LogPrintf("[AppInit2] mapBlockIndex.size() = %u\n",   mapBlockIndex.size());
    LogPrintf("[AppInit2] nBestHeight = %d\n",            nBestHeight);
    LogPrintf("[AppInit2] setKeyPool.size() = %u\n",      pwalletMain->setKeyPool.size());
    LogPrintf("[AppInit2] mapWallet.size() = %u\n",       pwalletMain->mapWallet.size());
    LogPrintf("[AppInit2] mapAddressBook.size() = %u\n",  pwalletMain->mapAddressBook.size());

    CConnman::Options connOptions;
    connOptions.nMaxConnections = nMaxConnections;
    connOptions.nMaxOutbound = std::min(MAX_OUTBOUND_CONNECTIONS, connOptions.nMaxConnections);
    connOptions.nMaxAddnode = MAX_ADDNODE_CONNECTIONS;
    connOptions.nMaxFeeler = 1;

    if (!connman.Start(scheduler, connOptions))
    {
        InitError(_("Error: could not start node"));
        return false;
    }

    if (fServer)
        NewThread(ThreadRPCServer, NULL);

    // ********************************************************* Step 12: finished

    uiInterface.InitMessage(_("Done loading"));

    if (!strErrors.str().empty())
        return InitError(strErrors.str());

     // Add wallet transactions that aren't already in a block to mapTransactions
    pwalletMain->ReacceptWalletTransactions();

#if !defined(QT_GUI)
    // // Loop until process is exit()ed from shutdown() function,
    // // called from ThreadRPCServer thread when a "stop" command is received.
    // while (1)
    //     MilliSleep(5000);
#endif

    return true;
}
