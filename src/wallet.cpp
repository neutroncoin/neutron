// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2017-2020 The Swipp developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdb.h"
#include "wallet.h"
#include "walletdb.h"
#include "crypter.h"
#include "ui_interface.h"
#include "base58.h"
#include "kernel.h"
#include "coincontrol.h"
#include <boost/algorithm/string/replace.hpp>
#include "script.h"
#include "spork.h"
#include "darksend.h"
#include "masternode.h"
#include "validation.h"

using namespace std;

unsigned int nStakeSplitAge = 1 * 24 * 60 * 60;
int64_t nStakeCombineThreshold = 1000 * COIN;

struct CompareValueOnly
{
    bool operator()(const pair<int64_t, pair<const CWalletTx*, unsigned int> >& t1,
                    const pair<int64_t, pair<const CWalletTx*, unsigned int> >& t2) const
    {
        return t1.first < t2.first;
    }
};

std::string COutput::ToString() const
{
    return strprintf("COutput(%s, %d, %d) [%s]", tx->GetHash().ToString().substr(0,10).c_str(), i, nDepth, FormatMoney(tx->vout[i].nValue).c_str());
}

int COutput::Priority() const
{
    if (tx->vout[i].nValue == DARKSEND_FEE)
        return -20000;

    BOOST_FOREACH(int64_t d, darkSendDenominations)
    {
        if (tx->vout[i].nValue == d)
            return 10000;
    }

    if(tx->vout[i].nValue < 1*COIN)
        return 20000;

    // nondenom return largest first
    return -(tx->vout[i].nValue/COIN);
}

CPubKey CWallet::GenerateNewKey()
{
    bool fCompressed = CanSupportFeature(FEATURE_COMPRPUBKEY); // default to compressed public keys if we want 0.6.0 wallets

    RandAddSeedPerfmon();
    CKey key;
    key.MakeNewKey(fCompressed);

    // Compressed public keys were introduced in version 0.6.0
    if (fCompressed)
        SetMinVersion(FEATURE_COMPRPUBKEY);

    CPubKey pubkey = key.GetPubKey();

    // Create new metadata
    int64_t nCreationTime = GetTime();
    mapKeyMetadata[pubkey.GetID()] = CKeyMetadata(nCreationTime);

    if (!nTimeFirstKey || nCreationTime < nTimeFirstKey)
        nTimeFirstKey = nCreationTime;

    if (!AddKey(key))
        throw std::runtime_error("CWallet::GenerateNewKey() : AddKey failed");

    return key.GetPubKey();
}

bool CWallet::AddKey(const CKey& key)
{
    CPubKey pubkey = key.GetPubKey();

    if (!CCryptoKeyStore::AddKey(key))
        return false;

    if (!fFileBacked)
        return true;

    if (!IsCrypted())
        return CWalletDB(strWalletFile).WriteKey(pubkey, key.GetPrivKey(), mapKeyMetadata[pubkey.GetID()]);

    return true;
}

bool CWallet::AddCryptedKey(const CPubKey &vchPubKey, const vector<unsigned char> &vchCryptedSecret)
{
    if (!CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret))
        return false;

    if (!fFileBacked)
        return true;

    {
        LOCK(cs_wallet);

        if (pwalletdbEncryption)
            return pwalletdbEncryption->WriteCryptedKey(vchPubKey, vchCryptedSecret, mapKeyMetadata[vchPubKey.GetID()]);
        else
            return CWalletDB(strWalletFile).WriteCryptedKey(vchPubKey, vchCryptedSecret, mapKeyMetadata[vchPubKey.GetID()]);
    }

    return false;
}

bool CWallet::LoadKeyMetadata(const CPubKey &pubkey, const CKeyMetadata &meta)
{
    if (meta.nCreateTime && (!nTimeFirstKey || meta.nCreateTime < nTimeFirstKey))
        nTimeFirstKey = meta.nCreateTime;

    mapKeyMetadata[pubkey.GetID()] = meta;
    return true;
}

bool CWallet::LoadCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret)
{
    return CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret);
}

bool CWallet::AddCScript(const CScript& redeemScript)
{
    if (!CCryptoKeyStore::AddCScript(redeemScript))
        return false;

    if (!fFileBacked)
        return true;

    return CWalletDB(strWalletFile).WriteCScript(Hash160(redeemScript), redeemScript);
}

// optional setting to unlock wallet for staking only
// serves to disable the trivial sendmoney when OS account compromised
// provides no real security

bool fWalletUnlockStakingOnly = false;

bool CWallet::Unlock(const SecureString& strWalletPassphrase, bool anonymizeOnly)
{
    if (!IsLocked())
    {
	fWalletUnlockAnonymizeOnly = anonymizeOnly;
	return true;
    }

    CCrypter crypter;
    CKeyingMaterial vMasterKey;
    fWalletUnlockAnonymizeOnly = anonymizeOnly;

    {
        LOCK(cs_wallet);

        BOOST_FOREACH(const MasterKeyMap::value_type& pMasterKey, mapMasterKeys)
        {
            if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, pMasterKey.second.vchSalt,
                                             pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
            {
                return false;
            }

            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                return false;

            if (CCryptoKeyStore::Unlock(vMasterKey))
                return true;
        }
    }

    return false;
}

bool CWallet::ChangeWalletPassphrase(const SecureString& strOldWalletPassphrase, const SecureString& strNewWalletPassphrase)
{
    bool fWasLocked = IsLocked();

    {
        LOCK(cs_wallet);
        Lock();

        CCrypter crypter;
        CKeyingMaterial vMasterKey;

        BOOST_FOREACH(MasterKeyMap::value_type& pMasterKey, mapMasterKeys)
        {
            if(!crypter.SetKeyFromPassphrase(strOldWalletPassphrase, pMasterKey.second.vchSalt,
                                             pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
            {
                return false;
            }

            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                return false;

            if (CCryptoKeyStore::Unlock(vMasterKey))
            {
                int64_t nStartTime = GetTimeMillis();

                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt,
                                             pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);

                pMasterKey.second.nDeriveIterations = pMasterKey.second.nDeriveIterations *
                                                      (100 / ((double) (GetTimeMillis() - nStartTime)));
                nStartTime = GetTimeMillis();

                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt,
                                             pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);

                pMasterKey.second.nDeriveIterations = (pMasterKey.second.nDeriveIterations +
                                                       pMasterKey.second.nDeriveIterations * 100 /
                                                       ((double)(GetTimeMillis() - nStartTime))) / 2;

                if (pMasterKey.second.nDeriveIterations < 25000)
                    pMasterKey.second.nDeriveIterations = 25000;

                LogPrintf("%s: wallet passphrase changed to an nDeriveIterations of %i\n",
                          __func__, pMasterKey.second.nDeriveIterations);

                if (!crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt,
                                                  pMasterKey.second.nDeriveIterations,
                                                  pMasterKey.second.nDerivationMethod))
                {
                    return false;
                }

                if (!crypter.Encrypt(vMasterKey, pMasterKey.second.vchCryptedKey))
                    return false;

                CWalletDB(strWalletFile).WriteMasterKey(pMasterKey.first, pMasterKey.second);

                if (fWasLocked)
                    Lock();

                return true;
            }
        }
    }

    return false;
}

void CWallet::SetBestChain(const CBlockLocator& loc)
{
    CWalletDB walletdb(strWalletFile);
    walletdb.WriteBestBlock(loc);
}

bool CWallet::SetMinVersion(enum WalletFeature nVersion, CWalletDB* pwalletdbIn, bool fExplicit)
{
    if (nWalletVersion >= nVersion)
        return true;

    // when doing an explicit upgrade, if we pass the max version permitted, upgrade all the way
    if (fExplicit && nVersion > nWalletMaxVersion)
            nVersion = FEATURE_LATEST;

    nWalletVersion = nVersion;

    if (nVersion > nWalletMaxVersion)
        nWalletMaxVersion = nVersion;

    if (fFileBacked)
    {
        CWalletDB* pwalletdb = pwalletdbIn ? pwalletdbIn : new CWalletDB(strWalletFile);

        if (nWalletVersion > 40000)
            pwalletdb->WriteMinVersion(nWalletVersion);

        if (!pwalletdbIn)
            delete pwalletdb;
    }

    return true;
}

bool CWallet::SetMaxVersion(int nVersion)
{
    // cannot downgrade below current version
    if (nWalletVersion > nVersion)
        return false;

    nWalletMaxVersion = nVersion;

    return true;
}

bool CWallet::EncryptWallet(const SecureString& strWalletPassphrase)
{
    if (IsCrypted())
        return false;

    CKeyingMaterial vMasterKey;
    RandAddSeedPerfmon();

    vMasterKey.resize(WALLET_CRYPTO_KEY_SIZE);
    RAND_bytes(&vMasterKey[0], WALLET_CRYPTO_KEY_SIZE);

    CMasterKey kMasterKey(nDerivationMethodIndex);

    RandAddSeedPerfmon();
    kMasterKey.vchSalt.resize(WALLET_CRYPTO_SALT_SIZE);
    RAND_bytes(&kMasterKey.vchSalt[0], WALLET_CRYPTO_SALT_SIZE);

    CCrypter crypter;
    int64_t nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, 25000, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = 2500000 / ((double)(GetTimeMillis() - nStartTime));

    nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = (kMasterKey.nDeriveIterations + kMasterKey.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

    if (kMasterKey.nDeriveIterations < 25000)
        kMasterKey.nDeriveIterations = 25000;

    LogPrintf("Encrypting Wallet with an nDeriveIterations of %i\n", kMasterKey.nDeriveIterations);

    if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod))
        return false;
    if (!crypter.Encrypt(vMasterKey, kMasterKey.vchCryptedKey))
        return false;

    {
        LOCK(cs_wallet);
        mapMasterKeys[++nMasterKeyMaxID] = kMasterKey;
        if (fFileBacked)
        {
            pwalletdbEncryption = new CWalletDB(strWalletFile);
            if (!pwalletdbEncryption->TxnBegin())
                return false;
            pwalletdbEncryption->WriteMasterKey(nMasterKeyMaxID, kMasterKey);
        }

        if (!EncryptKeys(vMasterKey))
        {
            if (fFileBacked)
                pwalletdbEncryption->TxnAbort();
            exit(1); //We now probably have half of our keys encrypted in memory, and half not...die and let the user reload their unencrypted wallet.
        }

        // Encryption was introduced in version 0.4.0
        SetMinVersion(FEATURE_WALLETCRYPT, pwalletdbEncryption, true);

        if (fFileBacked)
        {
            if (!pwalletdbEncryption->TxnCommit())
                exit(1); //We now have keys encrypted in memory, but no on disk...die to avoid confusion and let the user reload their unencrypted wallet.

            delete pwalletdbEncryption;
            pwalletdbEncryption = NULL;
        }

        Lock();
        Unlock(strWalletPassphrase);
        NewKeyPool();
        Lock();

        // Need to completely rewrite the wallet file; if we don't, bdb might keep
        // bits of the unencrypted private key in slack space in the database file.
        CDB::Rewrite(strWalletFile);

    }
    NotifyStatusChanged(this);

    return true;
}

int64_t CWallet::IncOrderPosNext(CWalletDB *pwalletdb)
{
    int64_t nRet = nOrderPosNext++;

    if (pwalletdb)
        pwalletdb->WriteOrderPosNext(nOrderPosNext);
    else
        CWalletDB(strWalletFile).WriteOrderPosNext(nOrderPosNext);

    return nRet;
}

CWallet::TxItems CWallet::OrderedTxItems(std::list<CAccountingEntry>& acentries, std::string strAccount)
{
    CWalletDB walletdb(strWalletFile);

    // First: get all CWalletTx and CAccountingEntry into a sorted-by-order multimap.
    TxItems txOrdered;

    // Note: maintaining indices in the database of (account,time) --> txid and (account, time) --> acentry
    // would make this much faster for applications that do this a lot.
    for (auto it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        CWalletTx* wtx = &((*it).second);
        txOrdered.insert(make_pair(wtx->nOrderPos, TxPair(wtx, (CAccountingEntry*)0)));
    }

    acentries.clear();
    walletdb.ListAccountCreditDebit(strAccount, acentries);

    BOOST_FOREACH(CAccountingEntry& entry, acentries)
    {
        txOrdered.insert(make_pair(entry.nOrderPos, TxPair((CWalletTx*)0, &entry)));
    }

    return txOrdered;
}

void CWallet::WalletUpdateSpent(const CTransaction &tx, bool fBlock)
{
    // Anytime a signature is successfully verified, it's proof the outpoint is spent.
    // Update the wallet spent flag if it doesn't know due to wallet.dat being
    // restored from backup or the user making copies of wallet.dat.
    {
        LOCK(cs_wallet);

        BOOST_FOREACH(const CTxIn& txin, tx.vin)
        {
            auto mi = mapWallet.find(txin.prevout.hash);

            if (mi != mapWallet.end())
            {
                CWalletTx& wtx = (*mi).second;

                if (txin.prevout.n >= wtx.vout.size())
                {
                    LogPrintf("%s : bad wtx %s\n", __func__, wtx.GetHash().ToString().c_str());
                }
                else if (!wtx.IsSpent(txin.prevout.n) && IsMine(wtx.vout[txin.prevout.n]))
                {
                    LogPrintf("%s : found spent coin %s NTRN %s\n", __func__,
                              FormatMoney(wtx.GetCredit()).c_str(), wtx.GetHash().ToString().c_str());

                    wtx.MarkSpent(txin.prevout.n);
                    wtx.WriteToDisk();
                    NotifyTransactionChanged(this, txin.prevout.hash, CT_UPDATED);
                }
            }
        }

        if (fBlock)
        {
            uint256 hash = tx.GetHash();
            auto mi = mapWallet.find(hash);
            CWalletTx& wtx = (*mi).second;

            BOOST_FOREACH(const CTxOut& txout, tx.vout)
            {
                if (IsMine(txout))
                {
                    wtx.MarkUnspent(&txout - &tx.vout[0]);
                    wtx.WriteToDisk();
                    NotifyTransactionChanged(this, hash, CT_UPDATED);
                }
            }
        }

    }
}

void CWallet::MarkDirty()
{
    LOCK(cs_wallet);

    for (auto& item : mapWallet)
        item.second.MarkDirty();
}

bool CWallet::AddToWallet(const CWalletTx& wtxIn)
{
    uint256 hash = wtxIn.GetHash();
    {
        LOCK(cs_wallet);

        // Inserts only if not already there, returns tx inserted or tx found
        auto ret = mapWallet.emplace(std::make_pair(hash, wtxIn));
        CWalletTx& wtx = (*ret.first).second;
        wtx.BindWallet(this);
        bool fInsertedNew = ret.second;

        if (fInsertedNew)
        {
            wtx.nTimeReceived = GetAdjustedTime();
            wtx.nOrderPos = IncOrderPosNext();
            wtx.nTimeSmart = wtx.nTimeReceived;

            if (wtxIn.hashBlock != 0)
            {
                if (mapBlockIndex.count(wtxIn.hashBlock))
                {
                    unsigned int latestNow = wtx.nTimeReceived;
                    unsigned int latestEntry = 0;
                    {
                        // Tolerate times up to the last timestamp in the wallet not more than 5 minutes into the future
                        int64_t latestTolerated = latestNow + 300;
                        std::list<CAccountingEntry> acentries;
                        TxItems txOrdered = OrderedTxItems(acentries);

                        for (TxItems::reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it)
                        {
                            CWalletTx *const pwtx = (*it).second.first;

                            if (pwtx == &wtx)
                                continue;

                            CAccountingEntry *const pacentry = (*it).second.second;
                            int64_t nSmartTime;

                            if (pwtx)
                            {
                                nSmartTime = pwtx->nTimeSmart;

                                if (!nSmartTime)
                                    nSmartTime = pwtx->nTimeReceived;
                            }
                            else
                                nSmartTime = pacentry->nTime;

                            if (nSmartTime <= latestTolerated)
                            {
                                latestEntry = nSmartTime;

                                if (nSmartTime > latestNow)
                                    latestNow = nSmartTime;

                                break;
                            }
                        }
                    }

                    unsigned int& blocktime = mapBlockIndex[wtxIn.hashBlock]->nTime;
                    wtx.nTimeSmart = std::max(latestEntry, std::min(blocktime, latestNow));
                }
                else
                    LogPrintf("%s : found %s in block %s not in index\n", __func__,
                              wtxIn.GetHash().ToString().substr(0,10).c_str(),
                              wtxIn.hashBlock.ToString().c_str());
            }
        }

        bool fUpdated = false;

        if (!fInsertedNew)
        {
            // Merge
            if (wtxIn.hashBlock != 0 && wtxIn.hashBlock != wtx.hashBlock)
            {
                wtx.hashBlock = wtxIn.hashBlock;
                fUpdated = true;
            }

            if (wtxIn.nIndex != -1 && (wtxIn.vMerkleBranch != wtx.vMerkleBranch || wtxIn.nIndex != wtx.nIndex))
            {
                wtx.vMerkleBranch = wtxIn.vMerkleBranch;
                wtx.nIndex = wtxIn.nIndex;
                fUpdated = true;
            }

            if (wtxIn.fFromMe && wtxIn.fFromMe != wtx.fFromMe)
            {
                wtx.fFromMe = wtxIn.fFromMe;
                fUpdated = true;
            }

            fUpdated |= wtx.UpdateSpent(wtxIn.vfSpent);
        }

        LogPrintf("AddToWallet %s  %s%s\n", wtxIn.GetHash().ToString().substr(0,10).c_str(),
                  fInsertedNew ? "new" : "", fUpdated ? "update" : "");

        if (fInsertedNew || fUpdated)
            if (!wtx.WriteToDisk())
                return false;
#ifndef QT_GUI
        // If default receiving address gets used, replace it with a new one
        if (vchDefaultKey.IsValid()) {
            CScript scriptDefaultKey;
            scriptDefaultKey.SetDestination(vchDefaultKey.GetID());
            BOOST_FOREACH(const CTxOut& txout, wtx.vout)
            {
                if (txout.scriptPubKey == scriptDefaultKey)
                {
                    CPubKey newDefaultKey;
                    if (GetKeyFromPool(newDefaultKey, false))
                    {
                        SetDefaultKey(newDefaultKey);
                        SetAddressBookName(vchDefaultKey.GetID(), "");
                    }
                }
            }
        }
#endif
        // since AddToWallet is called directly for self-originating transactions, check for consumption of own coins
        WalletUpdateSpent(wtx, (wtxIn.hashBlock != 0));

        // Notify UI of new or updated transaction
        NotifyTransactionChanged(this, hash, fInsertedNew ? CT_NEW : CT_UPDATED);

        // notify an external script when a wallet transaction comes in or is updated
        std::string strCmd = GetArg("-walletnotify", "");

        if ( !strCmd.empty())
        {
            boost::replace_all(strCmd, "%s", wtxIn.GetHash().GetHex());
            boost::thread t(runCommand, strCmd); // thread runs free
        }

    }
    return true;
}

// Add a transaction to the wallet, or update it.
// pblock is optional, but should be provided if the transaction is known to be in a block.
// If fUpdate is true, existing transactions will be updated.
bool CWallet::AddToWalletIfInvolvingMe(const CTransaction& tx, const CBlock* pblock, bool fUpdate, bool fFindBlock)
{
    uint256 hash = tx.GetHash();
    {
        LOCK(cs_wallet);
        bool fExisted = mapWallet.count(hash);

        if (fExisted && !fUpdate)
            return false;

        if (fExisted || IsMine(tx) || IsFromMe(tx))
        {
            CWalletTx wtx(this, tx);

            // Get merkle branch if transaction was found in a block
            if (pblock)
                wtx.SetMerkleBranch(pblock);

            return AddToWallet(wtx);
        }
        else
            WalletUpdateSpent(tx);
    }

    return false;
}

bool CWallet::EraseFromWallet(uint256 hash)
{
    if (!fFileBacked)
        return false;
    {
        LOCK(cs_wallet);

        if (mapWallet.erase(hash))
            CWalletDB(strWalletFile).EraseTx(hash);
    }

    return true;
}

bool CWallet::IsMine(const CTxIn &txin) const
{
    {
        LOCK(cs_wallet);
        auto mi = mapWallet.find(txin.prevout.hash);

        if (mi != mapWallet.end())
        {
            const CWalletTx& prev = (*mi).second;

            if (txin.prevout.n < prev.vout.size())
            {
                if (IsMine(prev.vout[txin.prevout.n]))
                    return true;
            }
        }
    }

    return false;
}

int64_t CWallet::GetDebit(const CTxIn &txin) const
{
    {
        LOCK(cs_wallet);
        auto mi = mapWallet.find(txin.prevout.hash);

        if (mi != mapWallet.end())
        {
            const CWalletTx& prev = (*mi).second;

            if (txin.prevout.n < prev.vout.size())
                if (IsMine(prev.vout[txin.prevout.n]))
                    return prev.vout[txin.prevout.n].nValue;
        }
    }

    return 0;
}

bool CWallet::IsDenominated(const CTxIn &txin) const
{
    {
        LOCK(cs_wallet);
        auto mi = mapWallet.find(txin.prevout.hash);

        if (mi != mapWallet.end())
        {
            const CWalletTx& prev = (*mi).second;

            if (txin.prevout.n < prev.vout.size())
                return CDarkSendPool::IsDenominatedAmount(prev.vout[txin.prevout.n].nValue);
        }
    }

    return false;
}

bool CWallet::IsDenominated(const CTransaction& tx) const
{
    /*
        Return false if ANY inputs are non-denom
    */
    bool ret = true;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        if(!IsDenominated(txin)) {
            ret = false;
        }
    }
    return ret;
}

bool CWallet::IsMine(const CTxOut& txout) const
{
    return ::IsMine(*this, txout.scriptPubKey);
}

int64_t CWallet::GetCredit(const CTxOut& txout) const
{
    if (!MoneyRange(txout.nValue))
        throw std::runtime_error("CWallet::GetCredit() : value out of range");
    return (IsMine(txout) ? txout.nValue : 0);
}

bool CWallet::IsChange(const CTxOut& txout) const
{
    CTxDestination address;

    // TODO: fix handling of 'change' outputs. The assumption is that any
    // payment to a TX_PUBKEYHASH that is mine but isn't in the address book
    // is change. That assumption is likely to break when we implement multisignature
    // wallets that return change back into a multi-signature-protected address;
    // a better way of identifying which outputs are 'the send' and which are
    // 'the change' will need to be implemented (maybe extend CWalletTx to remember
    // which output, if any, was change).
    if (ExtractDestination(txout.scriptPubKey, address) && ::IsMine(*this, address))
    {
        LOCK(cs_wallet);
        if (!mapAddressBook.count(address))
            return true;
    }
    return false;
}

int64_t CWallet::GetChange(const CTxOut& txout) const
{
    if (!MoneyRange(txout.nValue))
        throw std::runtime_error("CWallet::GetChange() : value out of range");
    return (IsChange(txout) ? txout.nValue : 0);
}

bool CWallet::IsMine(const CTransaction& tx) const
{
    BOOST_FOREACH(const CTxOut& txout, tx.vout)
    {
        if (IsMine(txout) && txout.nValue >= nMinimumInputValue)
            return true;
    }

    return false;
}

bool CWallet::IsFromMe(const CTransaction& tx) const
{
    return (GetDebit(tx) > 0);
}

int64_t CWallet::GetDebit(const CTransaction& tx) const
{
    int64_t nDebit = 0;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        nDebit += GetDebit(txin);
        if (!MoneyRange(nDebit))
            throw std::runtime_error("CWallet::GetDebit() : value out of range");
    }
    return nDebit;
}

int64_t CWallet::GetCredit(const CTransaction& tx) const
{
    int64_t nCredit = 0;
    BOOST_FOREACH(const CTxOut& txout, tx.vout)
    {
        nCredit += GetCredit(txout);
        if (!MoneyRange(nCredit))
            throw std::runtime_error("CWallet::GetCredit() : value out of range");
    }
    return nCredit;
}

int64_t CWallet::GetChange(const CTransaction& tx) const
{
    int64_t nChange = 0;
    BOOST_FOREACH(const CTxOut& txout, tx.vout)
    {
        nChange += GetChange(txout);
        if (!MoneyRange(nChange))
            throw std::runtime_error("CWallet::GetChange() : value out of range");
    }
    return nChange;
}

int64_t CWalletTx::GetTxTime() const
{
    int64_t n = nTimeSmart;
    return n ? n : nTimeReceived;
}

int CWalletTx::GetRequestCount() const
{
    // Returns -1 if it wasn't being tracked
    int nRequests = -1;
    {
        LOCK(pwallet->cs_wallet);
        if (IsCoinBase() || IsCoinStake())
        {
            // Generated block
            if (hashBlock != 0)
            {
                map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(hashBlock);
                if (mi != pwallet->mapRequestCount.end())
                    nRequests = (*mi).second;
            }
        }
        else
        {
            // Did anyone request this transaction?
            map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(GetHash());
            if (mi != pwallet->mapRequestCount.end())
            {
                nRequests = (*mi).second;

                // How about the block it's in?
                if (nRequests == 0 && hashBlock != 0)
                {
                    map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(hashBlock);
                    if (mi != pwallet->mapRequestCount.end())
                        nRequests = (*mi).second;
                    else
                        nRequests = 1; // If it's in someone else's block it must have got out
                }
            }
        }
    }
    return nRequests;
}

void CWalletTx::GetAmounts(std::list<COutputEntry>& listReceived,
                           std::list<COutputEntry>& listSent, int64_t& nFee, string& strSentAccount) const
{
    nFee = 0;
    listReceived.clear();
    listSent.clear();
    strSentAccount = strFromAccount;

    // Compute fee:
    int64_t nDebit = GetDebit();
    if (nDebit > 0) // debit>0 means we signed/sent this transaction
    {
        int64_t nValueOut = GetValueOut();
        nFee = nDebit - nValueOut;
    }

    // Sent/received.
    for (unsigned int i = 0; i < vout.size(); ++i)
    {
        const CTxOut& txout = vout[i];

        // Skip special stake out
        if (txout.scriptPubKey.empty())
            continue;

        bool fIsMine;
        // Only need to handle txouts if AT LEAST one of these is true:
        //   1) they debit from us (sent)
        //   2) the output is to us (received)
        if (nDebit > 0)
        {
            // Don't report 'change' txouts
            if (pwallet->IsChange(txout))
                continue;
            fIsMine = pwallet->IsMine(txout);
        }
        else if (!(fIsMine = pwallet->IsMine(txout)))
            continue;

        // In either case, we need to get the destination address
        CTxDestination address;
        if (!ExtractDestination(txout.scriptPubKey, address))
        {
            LogPrintf("CWalletTx::GetAmounts: Unknown transaction type found, txid %s\n",
                   this->GetHash().ToString().c_str());
            address = CNoDestination();
        }

        COutputEntry output = {address, txout.nValue, (int)i};

        // If we are debited by the transaction, add the output as a "sent" entry
        if (nDebit > 0)
            listSent.push_back(output);

        // If we are receiving the output, add it as a "received" entry
        if (fIsMine)
            listReceived.push_back(output);
    }

}

void CWalletTx::GetAccountAmounts(const string& strAccount, int64_t& nReceived,
                                  int64_t& nSent, int64_t& nFee) const
{
    nReceived = nSent = nFee = 0;

    int64_t allFee;
    string strSentAccount;
    list<COutputEntry> listReceived;
    list<COutputEntry> listSent;
    GetAmounts(listReceived, listSent, allFee, strSentAccount);

    if (strAccount == strSentAccount)
    {
        BOOST_FOREACH(const COutputEntry& s, listSent)
            nSent += s.amount;
        nFee = allFee;
    }
    {
        LOCK(pwallet->cs_wallet);
        BOOST_FOREACH(const COutputEntry& r, listReceived)
        {
            if (pwallet->mapAddressBook.count(r.destination))
            {
                map<CTxDestination, string>::const_iterator mi = pwallet->mapAddressBook.find(r.destination);
                if (mi != pwallet->mapAddressBook.end() && (*mi).second == strAccount)
                    nReceived += r.amount;
            }
            else if (strAccount.empty())
            {
                nReceived += r.amount;
            }
        }
    }
}

// Scan the block chain (starting in pindexStart) for transactions
// from or to us. If fUpdate is true, found transactions that already
// exist in the wallet will be updated.
int CWallet::ScanForWalletTransactions(CBlockIndex* pindexStart, bool fUpdate)
{
    int ret = 0;
    int64_t nNow = GetTime();

    CBlockIndex* pindex = pindexStart;
    {
        LOCK2(cs_main, cs_wallet);

        // no need to read and scan block, if block was created before
        // our wallet birthday (as adjusted for block time variability)
        while (pindex && nTimeFirstKey && (pindex->nTime < (nTimeFirstKey - 7200)))
            pindex = pindex->pnext;

        ShowProgress(_("Rescanning..."), 0); // show rescan progress in GUI as dialog or on splashscreen, if -rescan on startup
        double dProgressStart = GuessVerificationProgress(pindex);
        double dProgressTip = GuessVerificationProgress(pindexBest);

        LogPrintf("[Rescan] Start - pindexBest->nHeight=%d, pindex->nHeight=%d,  dProgressStart=%lf, dProgressTip=%lf\n",
                  pindexBest->nHeight, pindex->nHeight, dProgressStart, dProgressTip);

        while (pindex)
        {
            if (pindex->nHeight % 100 == 0 && dProgressTip - dProgressStart > 0.0)
            {
                ShowProgress(_("Rescanning..."), std::max(1, std::min(99, (int)((GuessVerificationProgress(pindex) -
                                                 dProgressStart) / (dProgressTip - dProgressStart) * 100))));
            }

            if (GetTime() >= nNow + 30)
            {
                nNow = GetTime();
                LogPrintf("[Rescan] Still rescanning. At block %d. Progress=%f\n", pindex->nHeight, GuessVerificationProgress(pindex));
            }

            CBlock block;
            block.ReadFromDisk(pindex, true);

            BOOST_FOREACH(CTransaction& tx, block.vtx)
            {
                if (AddToWalletIfInvolvingMe(tx, &block, fUpdate))
                    ret++;
            }

            pindex = pindex->pnext;
        }

        LogPrintf("[Rescan] Completed - %d transactions identified as part of this wallet.\n", ret);
        ShowProgress(_("Rescanning..."), 100); // hide progress dialog in GUI
    }

    return ret;
}

void CWallet::ReacceptWalletTransactions()
{
    CTxDB txdb("r");
    bool fRepeat = true;
    std::vector<CDiskTxPos> vMissingTx;

    while (fRepeat)
    {
        {
            LOCK(cs_wallet);
            fRepeat = false;

            for (auto& item : mapWallet)
            {
                CWalletTx& wtx = item.second;

                if ((wtx.IsCoinBase() && wtx.IsSpent(0)) || (wtx.IsCoinStake() && wtx.IsSpent(1)))
                    continue;

                CTxIndex txindex;
                bool fUpdated = false;

                if (txdb.ReadTxIndex(wtx.GetHash(), txindex))
                {
                    // Update fSpent if a tx got spent somewhere else by a copy of wallet.dat
                    if (txindex.vSpent.size() != wtx.vout.size())
                    {
                        LogPrintf("%s : [ERROR] txindex.vSpent.size() %u != wtx.vout.size() %u\n", __func__,
                                  txindex.vSpent.size(), wtx.vout.size());
                        continue;
                    }

                    for (unsigned int i = 0; i < txindex.vSpent.size(); i++)
                    {
                        if (wtx.IsSpent(i))
                            continue;

                        if (!txindex.vSpent[i].IsNull() && IsMine(wtx.vout[i]))
                        {
                            wtx.MarkSpent(i);
                            fUpdated = true;
                            vMissingTx.push_back(txindex.vSpent[i]);
                        }
                    }

                    if (fUpdated)
                    {
                        LogPrintf("%s : Found spent coin %s NTRN %s\n", __func__,
                                  FormatMoney(wtx.GetCredit()).c_str(), wtx.GetHash().ToString().c_str());

                        wtx.MarkDirty();
                        wtx.WriteToDisk();
                    }
                }
                else
                {
                    // Re-accept any txes of ours that aren't already in a block
                    if (!(wtx.IsCoinBase() || wtx.IsCoinStake()))
                        wtx.AcceptWalletTransaction(txdb);
                }
	    }
        }

        if (!vMissingTx.empty())
        {
            // TODO: optimize this to scan just part of the block chain?
            if (ScanForWalletTransactions(pindexGenesisBlock))
                fRepeat = true;  // Found missing transactions: re-do re-accept.
        }
    }
}

void CWalletTx::RelayWalletTransaction(CTxDB& txdb)
{
    BOOST_FOREACH(const CMerkleTx& tx, vtxPrev)
    {
        if (!(tx.IsCoinBase() || tx.IsCoinStake()))
        {
            uint256 hash = tx.GetHash();
            if (!txdb.ContainsTx(hash))
                RelayTransaction((CTransaction)tx, hash);
        }
    }
    if (!(IsCoinBase() || IsCoinStake()))
    {
        uint256 hash = GetHash();
        if (!txdb.ContainsTx(hash))
        {
            LogPrintf("Relaying wtx %s\n", hash.ToString().substr(0,10).c_str());
            RelayTransaction((CTransaction)*this, hash);
        }
    }
}

void CWalletTx::RelayWalletTransaction()
{
   CTxDB txdb("r");
   RelayWalletTransaction(txdb);
}

void CWalletTx::AddSupportingTransactions(CTxDB& txdb)
{
    vtxPrev.clear();
    const int COPY_DEPTH = 3;

    if (SetMerkleBranch() < COPY_DEPTH)
    {
        vector<uint256> vWorkQueue;

        BOOST_FOREACH(const CTxIn& txin, vin)
            vWorkQueue.push_back(txin.prevout.hash);

        // This critsect is OK because txdb is already open
        {
            LOCK(pwallet->cs_wallet);
            map<uint256, const CMerkleTx*> mapWalletPrev;
            set<uint256> setAlreadyDone;

            for (unsigned int i = 0; i < vWorkQueue.size(); i++)
            {
                uint256 hash = vWorkQueue[i];

                if (setAlreadyDone.count(hash))
                    continue;

                setAlreadyDone.insert(hash);
                CMerkleTx tx;
                auto mi = pwallet->mapWallet.find(hash);

                if (mi != pwallet->mapWallet.end())
                {
                    tx = (*mi).second;

                    BOOST_FOREACH(const CMerkleTx& txWalletPrev, (*mi).second.vtxPrev)
                        mapWalletPrev[txWalletPrev.GetHash()] = &txWalletPrev;
                }
                else if (mapWalletPrev.count(hash))
                {
                    tx = *mapWalletPrev[hash];
                }
                else if (!fClient && txdb.ReadDiskTx(hash, tx))
                {
                    // TODO: Why is this empty?
                }
                else
                {
                    LogPrintf("ERROR: AddSupportingTransactions() : unsupported transaction\n");
                    continue;
                }

                int nDepth = tx.SetMerkleBranch();
                vtxPrev.push_back(tx);

                if (nDepth < COPY_DEPTH)
                {
                    BOOST_FOREACH(const CTxIn& txin, tx.vin)
                        vWorkQueue.push_back(txin.prevout.hash);
                }
            }
        }
    }

    reverse(vtxPrev.begin(), vtxPrev.end());
}

bool CWalletTx::WriteToDisk()
{
    return CWalletDB(pwallet->strWalletFile).WriteTx(GetHash(), *this);
}

int64_t CWalletTx::IsDenominated() const
{
    if (vin.empty())
        return 0;

    return pwallet->IsDenominated(*this);
}

int64_t CWalletTx::GetDebit() const
{
    if (vin.empty())
        return 0;

    if (fDebitCached)
        return nDebitCached;

    nDebitCached = pwallet->GetDebit(*this);
    fDebitCached = true;
    return nDebitCached;
}

int64_t CWalletTx::GetCredit(bool fUseCache) const
{
    // Must wait until coinbase is safely deep enough in the chain before valuing it
    if (IsCoinBase() && GetBlocksToMaturity() > 0)
        return 0;

    // GetBalance can assume transactions in mapWallet won't change
    if (fUseCache && fCreditCached)
        return nCreditCached;

    nCreditCached = pwallet->GetCredit(*this);
    fCreditCached = true;
    return nCreditCached;
}

int64_t CWalletTx::GetAvailableCredit(bool fUseCache) const
{
    // Must wait until coinbase is safely deep enough in the chain before valuing it
    if ((IsCoinBase() || IsCoinStake()) && GetBlocksToMaturity() > 0)
        return 0;

    if (fUseCache && fAvailableCreditCached)
        return nAvailableCreditCached;

    int64_t nCredit = 0;

    for (unsigned int i = 0; i < vout.size(); i++)
    {
        if (!IsSpent(i))
        {
            const CTxOut &txout = vout[i];
            nCredit += pwallet->GetCredit(txout);

            if (!MoneyRange(nCredit))
                throw std::runtime_error("CWalletTx::GetAvailableCredit() : value out of range");
        }
    }

    nAvailableCreditCached = nCredit;
    fAvailableCreditCached = true;
    return nCredit;
}

int64_t CWalletTx::GetChange() const
{
    if (fChangeCached)
        return nChangeCached;

    nChangeCached = pwallet->GetChange(*this);
    fChangeCached = true;

    return nChangeCached;
}

bool CWalletTx::IsTrusted() const
{
    // Quick answer in most cases
    if (!IsFinal())
        return false;

    int nDepth = GetDepthInMainChain();

    if (nDepth >= 1)
        return true;

    if (nDepth < 0)
        return false;

    if (fConfChange || !IsFromMe()) // using wtx's cached debit
        return false;

    // If no confirmations but it's from us, we can still
    // consider it confirmed if all dependencies are confirmed
    std::map<uint256, const CMerkleTx*> mapPrev;
    std::vector<const CMerkleTx*> vWorkQueue;

    vWorkQueue.reserve(vtxPrev.size()+1);
    vWorkQueue.push_back(this);

    for (unsigned int i = 0; i < vWorkQueue.size(); i++)
    {
        const CMerkleTx* ptx = vWorkQueue[i];

        if (!ptx->IsFinal())
            return false;

        int nPDepth = ptx->GetDepthInMainChain();

        if (nPDepth >= 1)
            continue;

        if (nPDepth < 0)
            return false;

        if (!pwallet->IsFromMe(*ptx))
            return false;

        if (mapPrev.empty())
        {
            BOOST_FOREACH(const CMerkleTx& tx, vtxPrev)
                mapPrev[tx.GetHash()] = &tx;
        }

        BOOST_FOREACH(const CTxIn& txin, ptx->vin)
        {
            if (!mapPrev.count(txin.prevout.hash))
                return false;

            vWorkQueue.push_back(mapPrev[txin.prevout.hash]);
        }
    }

    return true;
}

void CWallet::ResendWalletTransactions(bool fForce)
{
    if (!fForce)
    {
        // Do this infrequently and randomly to avoid giving away
        // that these are our transactions.
        static int64_t nNextTime;

        if (GetTime() < nNextTime)
            return;

        bool fFirst = (nNextTime == 0);
        nNextTime = GetTime() + GetRand(30 * 60);

        if (fFirst)
            return;

        // Only do it if there's been a new block since last time
        static int64_t nLastTime;

        if (nTimeBestReceived < nLastTime)
            return;

        nLastTime = GetTime();
    }

    // Rebroadcast any of our txes that aren't in a block yet
    LogPrintf("ResendWalletTransactions()\n");

    CTxDB txdb("r");
    {
        LOCK(cs_wallet);

        // Sort them in chronological order
        multimap<unsigned int, CWalletTx*> mapSorted;

        for (auto& item : mapWallet)
        {
            CWalletTx& wtx = item.second;

            // Don't rebroadcast until it's had plenty of time that
            // it should have gotten in already by now.
            if (fForce || nTimeBestReceived - (int64_t)wtx.nTimeReceived > 5 * 60)
                mapSorted.insert(make_pair(wtx.nTimeReceived, &wtx));
        }

        BOOST_FOREACH(PAIRTYPE(const unsigned int, CWalletTx*)& item, mapSorted)
        {
            CWalletTx& wtx = *item.second;

            if (wtx.CheckTransaction())
                wtx.RelayWalletTransaction(txdb);
            else
                LogPrintf("ResendWalletTransactions() : CheckTransaction failed for transaction %s\n", wtx.GetHash().ToString().c_str());
        }
    }
}

int64_t CWallet::GetBalance() const
{
    int64_t nTotal = 0;

    {
        LOCK(cs_wallet);

        for (auto it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;

            if (pcoin->IsTrusted())
                nTotal += pcoin->GetAvailableCredit();
        }
    }

    return nTotal;
}

//TODO: Remove me
CAmount CWallet::GetAnonymizedBalance() const
{
    return 0;
}

//TODO: Remove me
double CWallet::GetAverageAnonymizedRounds() const
{
    return 0;
}

//TODO: Remove me
CAmount CWallet::GetNormalizedAnonymizedBalance() const
{
    return 0;
}

CAmount CWallet::GetDenominatedBalance(bool onlyDenom, bool onlyUnconfirmed) const
{
    int64_t nTotal = 0;

    {
        LOCK(cs_wallet);

        for (auto it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            int nDepth = pcoin->GetDepthInMainChain();

            // skip conflicted
            if(nDepth < 0)
                continue;

            bool unconfirmed = (!pcoin->IsFinal() || (!pcoin->IsTrusted() && nDepth == 0));

            if(onlyUnconfirmed != unconfirmed)
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            {
		// isminetype mine = IsMine(pcoin->vout[i]);
		// bool mine = IsMine(pcoin->vout[i]);
                // COutput out = COutput(pcoin, i, nDepth, (mine & ISMINE_SPENDABLE) != ISMINE_NO);
		// COutput out = COutput(pcoin, i, nDepth, mine);
                // if(IsSpent(out.tx->GetHash(), i)) continue;

		if(pcoin->IsSpent(i))
                    continue;

                if(!IsMine(pcoin->vout[i]))
                    continue;

                if(onlyDenom != CDarkSendPool::IsDenominatedAmount(pcoin->vout[i].nValue))
                    continue;

                nTotal += pcoin->vout[i].nValue;
            }
        }
    }

    return nTotal;
}

int64_t CWallet::GetUnconfirmedBalance() const
{
    int64_t nTotal = 0;
    {
        LOCK(cs_wallet);

        for (auto it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;

            if (!pcoin->IsFinal() || (!pcoin->IsTrusted() && pcoin->GetDepthInMainChain() == 0))
                nTotal += pcoin->GetAvailableCredit();
        }
    }

    return nTotal;
}

int64_t CWallet::GetImmatureBalance() const
{
    int64_t nTotal = 0;
    {
        LOCK(cs_wallet);

        for (auto it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx& pcoin = (*it).second;

            if (pcoin.IsCoinBase() && pcoin.GetBlocksToMaturity() > 0 && pcoin.IsInMainChain())
                nTotal += GetCredit(pcoin);
        }
    }

    return nTotal;
}

void CWallet::AvailableCoins(vector<COutput>& vCoins, bool fOnlyConfirmed, const CCoinControl *coinControl,
                             AvailableCoinsType coin_type, bool useIX) const
{
    vCoins.clear();

    {
        LOCK(cs_wallet);

        for (auto it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;

            if (!pcoin->IsFinal())
                continue;

            if (fOnlyConfirmed && !pcoin->IsTrusted())
                continue;

            if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            if(pcoin->IsCoinStake() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain();

            if (nDepth < 0)
                continue;

            // for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            //     if (!(pcoin->IsSpent(i)) && IsMine(pcoin->vout[i]) && pcoin->vout[i].nValue >= nMinimumInputValue &&
            //         (!coinControl || !coinControl->HasSelected() || coinControl->IsSelected((*it).first, i)))
            //         vCoins.push_back(COutput(pcoin, i, nDepth));

            if (useIX && nDepth < 6)
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            {
                bool found = false;

                if(coin_type == ONLY_DENOMINATED)
                {
                    //should make this a vector
                    found = CDarkSendPool::IsDenominatedAmount(pcoin->vout[i].nValue);
                }
                else if(coin_type == ONLY_NONDENOMINATED || coin_type == ONLY_NONDENOMINATED_NOTMN)
                {
                    found = true;

                    if (IsCollateralAmount(pcoin->vout[i].nValue))
                        continue; // do not use collateral amounts

                    found = !CDarkSendPool::IsDenominatedAmount(pcoin->vout[i].nValue);

                    if(found && coin_type == ONLY_NONDENOMINATED_NOTMN)
                        found = (pcoin->vout[i].nValue != 500 * COIN); // do not use MN funds
                }
                else
                    found = true;

                if(!found)
                    continue;

	        bool mine = IsMine(pcoin->vout[i]);

                // if (!(IsSpent(wtxid, i)) && mine != ISMINE_NO &&
                //     !IsLockedCoin((*it).first, i) && pcoin->vout[i].nValue > 0 &&
                //     (!coinControl || !coinControl->HasSelected() || coinControl->IsSelected((*it).first, i)))
                //     vCoins.push_back(COutput(pcoin, i, nDepth, (mine & ISMINE_SPENDABLE) != ISMINE_NO));
		// if (!(IsSpent(wtxid, i)) && mine &&

	        if (!(pcoin->IsSpent(i)) && mine &&
                    !IsLockedCoin((*it).first, i) && pcoin->vout[i].nValue > 0 &&
                    (!coinControl || !coinControl->HasSelected() || coinControl->IsSelected((*it).first, i)))
                {
                        vCoins.push_back(COutput(pcoin, i, nDepth, mine));
                }
            }

        }
    }
}

void CWallet::AvailableCoinsMN(vector<COutput>& vCoins, bool fOnlyConfirmed, const CCoinControl *coinControl,
                               AvailableCoinsType coin_type, bool useIX) const
{
    vCoins.clear();

    {
        LOCK2(cs_main, cs_wallet);

        for (auto it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;

            if (!pcoin->IsFinal())
                continue;

            if (fOnlyConfirmed && !pcoin->IsTrusted())
                continue;

            if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            if(pcoin->IsCoinStake() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth <= 0) // NTRNNOTE: coincontrol fix / ignore 0 confirm
                continue;

            /* for (unsigned int i = 0; i < pcoin->vout.size(); i++)
                if (!(pcoin->IsSpent(i)) && IsMine(pcoin->vout[i]) && pcoin->vout[i].nValue >= nMinimumInputValue &&
                (!coinControl || !coinControl->HasSelected() || coinControl->IsSelected((*it).first, i)))
                    vCoins.push_back(COutput(pcoin, i, nDepth));*/

            // do not use IX for inputs that have less then 6 blockchain confirmations
            if (useIX && nDepth < 6)
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            {
                bool found = false;

                if(coin_type == ONLY_DENOMINATED)
                {
                    // TODO: Should make this a vector
                    found = CDarkSendPool::IsDenominatedAmount(pcoin->vout[i].nValue);
                }
                else if(coin_type == ONLY_NONDENOMINATED || coin_type == ONLY_NONDENOMINATED_NOTMN)
                {
                    found = true;
                    if (IsCollateralAmount(pcoin->vout[i].nValue)) continue; // do not use collateral amounts
                    found = !CDarkSendPool::IsDenominatedAmount(pcoin->vout[i].nValue);
                    if(found && coin_type == ONLY_NONDENOMINATED_NOTMN) found = (pcoin->vout[i].nValue != 25000*COIN); // do not use MN funds
                }
                else
                    found = true;

                if(!found)
                    continue;

                //isminetype mine = IsMine(pcoin->vout[i]);
	        bool mine = IsMine(pcoin->vout[i]);

                //if (!(IsSpent(wtxid, i)) && mine != ISMINE_NO &&
                //    !IsLockedCoin((*it).first, i) && pcoin->vout[i].nValue > 0 &&
                //    (!coinControl || !coinControl->HasSelected() || coinControl->IsSelected((*it).first, i)))
                //        vCoins.push_back(COutput(pcoin, i, nDepth, (mine & ISMINE_SPENDABLE) != ISMINE_NO));
		        //if (!(IsSpent(wtxid, i)) && mine &&
	        if (!(pcoin->IsSpent(i)) &&
                    !IsLockedCoin((*it).first, i) && pcoin->vout[i].nValue > 0 &&
                    (!coinControl || !coinControl->HasSelected() || coinControl->IsSelected((*it).first, i)))
                        vCoins.push_back(COutput(pcoin, i, nDepth, mine));
            }
        }
    }
}

void CWallet::AvailableCoinsMinConf(vector<COutput>& vCoins, int nConf) const
{
    vCoins.clear();

    {
        LOCK(cs_wallet);

        for (auto it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;

            if (!pcoin->IsFinal())
                continue;

            if(pcoin->GetDepthInMainChain() < nConf)
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++)
                if (!(pcoin->IsSpent(i)) && IsMine(pcoin->vout[i]) && pcoin->vout[i].nValue >= nMinimumInputValue)
                    vCoins.push_back(COutput(pcoin, i, pcoin->GetDepthInMainChain(), IsMine(pcoin->vout[i])));
        }
    }
}

static void ApproximateBestSubset(vector<pair<int64_t, pair<const CWalletTx*,unsigned int> > >vValue, int64_t nTotalLower, int64_t nTargetValue,
                                  vector<char>& vfBest, int64_t& nBest, int iterations = 1000)
{
    vector<char> vfIncluded;

    vfBest.assign(vValue.size(), true);
    nBest = nTotalLower;

    for (int nRep = 0; nRep < iterations && nBest != nTargetValue; nRep++)
    {
        vfIncluded.assign(vValue.size(), false);
        int64_t nTotal = 0;
        bool fReachedTarget = false;
        for (int nPass = 0; nPass < 2 && !fReachedTarget; nPass++)
        {
            for (unsigned int i = 0; i < vValue.size(); i++)
            {
                if (nPass == 0 ? rand() % 2 : !vfIncluded[i])
                {
                    nTotal += vValue[i].first;
                    vfIncluded[i] = true;
                    if (nTotal >= nTargetValue)
                    {
                        fReachedTarget = true;
                        if (nTotal < nBest)
                        {
                            nBest = nTotal;
                            vfBest = vfIncluded;
                        }
                        nTotal -= vValue[i].first;
                        vfIncluded[i] = false;
                    }
                }
            }
        }
    }
}

// TODO: find appropriate place for this sort function
// move denoms down
bool less_then_denom(const COutput& out1, const COutput& out2)
{
    const CWalletTx *pcoin1 = out1.tx;
    const CWalletTx *pcoin2 = out2.tx;
    bool found1 = false;
    bool found2 = false;

    BOOST_FOREACH(int64_t d, darkSendDenominations) // loop through predefined denoms
    {
        if(pcoin1->vout[out1.i].nValue == d) found1 = true;
        if(pcoin2->vout[out2.i].nValue == d) found2 = true;
    }

    return (!found1 && found2);
}

int64_t CWallet::GetStake() const
{
    int64_t nTotal = 0;
    LOCK(cs_wallet);

    for (auto it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        const CWalletTx* pcoin = &(*it).second;

        if (pcoin->IsCoinStake() && pcoin->GetBlocksToMaturity() > 0 && pcoin->GetDepthInMainChain() > 0)
            nTotal += CWallet::GetCredit(*pcoin);
    }

    return nTotal;
}

int64_t CWallet::GetNewMint() const
{
    int64_t nTotal = 0;
    LOCK(cs_wallet);

    for (auto it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        const CWalletTx* pcoin = &(*it).second;

        if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0 && pcoin->GetDepthInMainChain() > 0)
            nTotal += CWallet::GetCredit(*pcoin);
    }

    return nTotal;
}

int64_t CWallet::GetTotal() const
{
    return this->GetBalance() + this->GetNewMint() + this->GetStake();
}

bool CWallet::MintableCoins()
{
    CAmount nBalance = GetBalance();

    if (mapArgs.count("-reservebalance") && !ParseMoney(mapArgs["-reservebalance"], nReserveBalance))
        return error("MintableCoins() : invalid reserve balance amount");

    if (nBalance <= nReserveBalance)
        return false;

    vector<COutput> vCoins;
    AvailableCoins(vCoins, true);

    BOOST_FOREACH (const COutput& out, vCoins)
    {
        if (GetTime() - out.tx->GetTxTime() > nStakeMinAge)
            return true;
    }

    return false;
}

//TODO: Fix broken  indentation in this function
bool CWallet::SelectCoinsMinConf(int64_t nTargetValue, unsigned int nSpendTime, int nConfMine, int nConfTheirs,
                                 vector<COutput> vCoins, set<pair<const CWalletTx*,unsigned int> >& setCoinsRet,
                                 int64_t& nValueRet) const
{
    setCoinsRet.clear();
    nValueRet = 0;

    // List of values less than target
    pair<int64_t, pair<const CWalletTx*,unsigned int> > coinLowestLarger;
    coinLowestLarger.first = std::numeric_limits<int64_t>::max();
    coinLowestLarger.second.first = NULL;
    vector<pair<int64_t, pair<const CWalletTx*,unsigned int> > > vValue;
    int64_t nTotalLower = 0;

    random_shuffle(vCoins.begin(), vCoins.end(), GetRandInt);

    // move denoms down on the list
    sort(vCoins.begin(), vCoins.end(), less_then_denom);

    // try to find nondenom first to prevent unneeded spending of mixed coins
    for (unsigned int tryDenom = 0; tryDenom < 2; tryDenom++)
    {
        if (fDebug) LogPrintf("selectcoins", "tryDenom: %d\n", tryDenom);
        vValue.clear();
        nTotalLower = 0;

    BOOST_FOREACH(COutput output, vCoins)
    {
        const CWalletTx *pcoin = output.tx;

        if (output.nDepth < (pcoin->IsFromMe() ? nConfMine : nConfTheirs))
            continue;

        int i = output.i;

        // Follow the timestamp rules
        if (pcoin->nTime > nSpendTime)
            continue;

        int64_t n = pcoin->vout[i].nValue;

    if (tryDenom == 0 && CDarkSendPool::IsDenominatedAmount(n)) continue; // we don't want denom values on first run


        pair<int64_t,pair<const CWalletTx*,unsigned int> > coin = make_pair(n,make_pair(pcoin, i));

        if (n == nTargetValue)
        {
            setCoinsRet.insert(coin.second);
            nValueRet += coin.first;
            return true;
        }
        else if (n < nTargetValue + CENT)
        {
            vValue.push_back(coin);
            nTotalLower += n;
        }
        else if (n < coinLowestLarger.first)
        {
            coinLowestLarger = coin;
        }
    }

    if (nTotalLower == nTargetValue)
    {
        for (unsigned int i = 0; i < vValue.size(); ++i)
        {
            setCoinsRet.insert(vValue[i].second);
            nValueRet += vValue[i].first;
        }
        return true;
    }

    if (nTotalLower < nTargetValue)
    {
        if (coinLowestLarger.second.first == NULL)
            return false;
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
        return true;
    }

    // Solve subset sum by stochastic approximation
    sort(vValue.rbegin(), vValue.rend(), CompareValueOnly());
    vector<char> vfBest;
    int64_t nBest;

    ApproximateBestSubset(vValue, nTotalLower, nTargetValue, vfBest, nBest, 1000);
    if (nBest != nTargetValue && nTotalLower >= nTargetValue + CENT)
        ApproximateBestSubset(vValue, nTotalLower, nTargetValue + CENT, vfBest, nBest, 1000);

    // If we have a bigger coin and (either the stochastic approximation didn't find a good solution,
    //                                   or the next bigger coin is closer), return the bigger coin
    if (coinLowestLarger.second.first &&
        ((nBest != nTargetValue && nBest < nTargetValue + CENT) || coinLowestLarger.first <= nBest))
    {
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
    }
    else {
        for (unsigned int i = 0; i < vValue.size(); i++)
            if (vfBest[i])
            {
                setCoinsRet.insert(vValue[i].second);
                nValueRet += vValue[i].first;
            }

        if (fDebug && GetBoolArg("-printpriority"))
        {
            //// debug print
            LogPrintf("SelectCoins() best subset: ");
            for (unsigned int i = 0; i < vValue.size(); i++)
                if (vfBest[i])
                    LogPrintf("%s ", FormatMoney(vValue[i].first).c_str());
            LogPrintf("total %s\n", FormatMoney(nBest).c_str());
        }
    }

    return true;
  }
    return false;
}

bool CWallet::SelectCoins(int64_t nTargetValue, unsigned int nSpendTime, set<pair<const CWalletTx*,unsigned int> >& setCoinsRet,
                          int64_t& nValueRet, const CCoinControl* coinControl, AvailableCoinsType coin_type, bool useIX) const
{
    vector<COutput> vCoins;
    AvailableCoins(vCoins, true, coinControl);

    //if we're doing only denominated, we need to round up to the nearest .1 NTRN
    if(coin_type == ONLY_DENOMINATED)
    {
        // Make outputs by looping through denominations, from large to small
        BOOST_FOREACH(int64_t v, darkSendDenominations)
        {
            int added = 0;

            BOOST_FOREACH(const COutput& out, vCoins)
            {
                if(out.tx->vout[out.i].nValue == v                                                // make sure it's the denom we're looking for
                    && nValueRet + out.tx->vout[out.i].nValue < nTargetValue + (0.1 * COIN) + 100 // round the amount up to .1 NTRN over
                    && added <= 100)                                                              // don't add more than 100 of one denom type
                {
                        CTxIn vin = CTxIn(out.tx->GetHash(),out.i);
                        nValueRet += out.tx->vout[out.i].nValue;
                        setCoinsRet.insert(make_pair(out.tx, out.i));
                        added++;
                }
            }
        }

        return (nValueRet >= nTargetValue);
    }

    // coin control -> return all selected outputs (we want all selected to go into the transaction for sure)
    if (coinControl && coinControl->HasSelected())
    {
        BOOST_FOREACH(const COutput& out, vCoins)
        {
	    if(!out.fSpendable)
                continue;

            nValueRet += out.tx->vout[out.i].nValue;
            setCoinsRet.insert(make_pair(out.tx, out.i));
        }

        return (nValueRet >= nTargetValue);
    }

    return (SelectCoinsMinConf(nTargetValue, nSpendTime, 1, 10, vCoins, setCoinsRet, nValueRet) ||
            SelectCoinsMinConf(nTargetValue, nSpendTime, 1, 1, vCoins, setCoinsRet, nValueRet) ||
            SelectCoinsMinConf(nTargetValue, nSpendTime, 0, 1, vCoins, setCoinsRet, nValueRet));
}

// Select some coins without random shuffle or best subset approximation
bool CWallet::SelectCoinsSimple(int64_t nTargetValue, unsigned int nSpendTime, int nMinConf,
                                set<pair<const CWalletTx*,unsigned int> >& setCoinsRet, int64_t& nValueRet) const
{
    vector<COutput> vCoins;
    AvailableCoinsMinConf(vCoins, nMinConf);

    setCoinsRet.clear();
    nValueRet = 0;

    BOOST_FOREACH(COutput output, vCoins)
    {
        const CWalletTx *pcoin = output.tx;
        int i = output.i;

        // Stop if we've chosen enough inputs
        if (nValueRet >= nTargetValue)
            break;

        // Follow the timestamp rules
        if (pcoin->nTime > nSpendTime)
            continue;

        int64_t n = pcoin->vout[i].nValue;

        pair<int64_t,pair<const CWalletTx*,unsigned int> > coin = make_pair(n,make_pair(pcoin, i));

        if (n >= nTargetValue)
        {
            // If input value is greater or equal to target then simply insert
            //    it into the current subset and exit
            setCoinsRet.insert(coin.second);
            nValueRet += coin.first;
            break;
        }
        else if (n < nTargetValue + CENT)
        {
            setCoinsRet.insert(coin.second);
            nValueRet += coin.first;
        }
    }

    return true;
}

struct CompareByPriority
{
    bool operator()(const COutput& t1,
                    const COutput& t2) const
    {
        return t1.Priority() > t2.Priority();
    }
};

bool CWallet::SelectCoinsByDenominations(int nDenom, int64_t nValueMin, int64_t nValueMax, std::vector<CTxIn>& setCoinsRet, vector<COutput>& setCoinsRet2, int64_t& nValueRet, int nDarksendRoundsMin, int nDarksendRoundsMax)
{
    setCoinsRet.clear();
    nValueRet = 0;

    setCoinsRet2.clear();
    vector<COutput> vCoins;
    AvailableCoins(vCoins);

    //order the array so fees are first, then denominated money, then the rest.
    std::random_shuffle(vCoins.rbegin(), vCoins.rend());

    //keep track of each denomination that we have
    bool fFound100000 = false;
    bool fFound10000 = false;
    bool fFound1000 = false;
    bool fFound100 = false;
    bool fFound10 = false;
    bool fFound1 = false;
    bool fFoundDot1 = false;

    //Check to see if any of the denomination are off, in that case mark them as fulfilled



    if(!(nDenom & (1 << 0))) fFound100000 = true;
    if(!(nDenom & (1 << 1))) fFound10000 = true;
    if(!(nDenom & (1 << 2))) fFound1000 = true;
    if(!(nDenom & (1 << 3))) fFound100 = true;
    if(!(nDenom & (1 << 4))) fFound10 = true;
    if(!(nDenom & (1 << 5))) fFound1 = true;
    if(!(nDenom & (1 << 6))) fFoundDot1 = true;

    BOOST_FOREACH(const COutput& out, vCoins)
    {
        //there's no reason to allow inputs less than 1 COIN into DS (other than denominations smaller than that amount)
        if(out.tx->vout[out.i].nValue < 1  *COIN && out.tx->vout[out.i].nValue != (.1 * COIN) + 100)
            continue;

        if(fMasterNode && out.tx->vout[out.i].nValue == 25000 * COIN)
            continue; //masternode input

        if(nValueRet + out.tx->vout[out.i].nValue <= nValueMax)
        {
            bool fAccepted = false;

            // Function returns as follows:
            //
            // bit 0 - 100 NTRN+1 ( bit on if present )
            // bit 1 - 10 NTRN+1
            // bit 2 - 1 NTRN+1
            // bit 3 - .1 NTRN+1

            CTxIn vin = CTxIn(out.tx->GetHash(),out.i);

            if(fFound100000 && fFound10000 && fFound1000 && fFound100 && fFound10 && fFound1 && fFoundDot1)
            {
                //we can return this for submission
                if(nValueRet >= nValueMin)
                {
                    //random reduce the max amount we'll submit for anonymity
                    nValueMax -= (rand() % (nValueMax/5));
                    //on average use 50% of the inputs or less
                    int r = (rand() % (int)vCoins.size());
                    if((int)setCoinsRet.size() > r) return true;
                }
                //Denomination criterion has been met, we can take any matching denominations
                if((nDenom & (1 << 0)) && out.tx->vout[out.i].nValue ==      ((100000*COIN)    +100000000)) {fAccepted = true;}
                else if((nDenom & (1 << 1)) && out.tx->vout[out.i].nValue == ((10000*COIN)     +10000000)) {fAccepted = true;}
                else if((nDenom & (1 << 2)) && out.tx->vout[out.i].nValue == ((1000*COIN)      +1000000)) {fAccepted = true;}
                else if((nDenom & (1 << 3)) && out.tx->vout[out.i].nValue == ((100*COIN)       +100000)) {fAccepted = true;}
                else if((nDenom & (1 << 4)) && out.tx->vout[out.i].nValue == ((10*COIN)        +10000)) {fAccepted = true;}
                else if((nDenom & (1 << 5)) && out.tx->vout[out.i].nValue == ((1*COIN)         +1000)) {fAccepted = true;}
                else if((nDenom & (1 << 6)) && out.tx->vout[out.i].nValue == ((.1*COIN)        +100)) {fAccepted = true;}
            } else {
                //Criterion has not been satisfied, we will only take 1 of each until it is.
                if((nDenom & (1 << 0)) && out.tx->vout[out.i].nValue ==      ((100000*COIN)    +100000000)) {fAccepted = true; fFound100000 = true;}
                else if((nDenom & (1 << 1)) && out.tx->vout[out.i].nValue == ((10000*COIN)     +10000000)) {fAccepted = true; fFound10000 = true;}
                else if((nDenom & (1 << 1)) && out.tx->vout[out.i].nValue == ((1000*COIN)      +1000000)) {fAccepted = true; fFound1000 = true;}
                else if((nDenom & (1 << 1)) && out.tx->vout[out.i].nValue == ((100*COIN)       +100000)) {fAccepted = true; fFound100 = true;}
                else if((nDenom & (1 << 1)) && out.tx->vout[out.i].nValue == ((10*COIN)        +10000)) {fAccepted = true; fFound10 = true;}
                else if((nDenom & (1 << 2)) && out.tx->vout[out.i].nValue == ((1*COIN)         +1000)) {fAccepted = true; fFound1 = true;}
                else if((nDenom & (1 << 3)) && out.tx->vout[out.i].nValue == ((.1*COIN)        +100)) {fAccepted = true; fFoundDot1 = true;}
            }
            if(!fAccepted) continue;

            vin.prevPubKey = out.tx->vout[out.i].scriptPubKey; // the inputs PubKey
            nValueRet += out.tx->vout[out.i].nValue;
            setCoinsRet.push_back(vin);
            setCoinsRet2.push_back(out);
        }
    }

    return (nValueRet >= nValueMin && fFound100000 && fFound10000 && fFound1000 && fFound100 && fFound10 && fFound1 && fFoundDot1);
}

//TODO: Remove me
bool CWallet::SelectCoinsDark(int64_t nValueMin, int64_t nValueMax, std::vector<CTxIn>& setCoinsRet,
                              int64_t& nValueRet, int nDarksendRoundsMin, int nDarksendRoundsMax) const
{
    return false;
}

bool CWallet::SelectCoinsCollateral(std::vector<CTxIn>& setCoinsRet, int64_t& nValueRet) const
{
    vector<COutput> vCoins;

    //LogPrintf(" selecting coins for collateral\n");
    AvailableCoins(vCoins);

    //LogPrintf("found coins %d\n", (int)vCoins.size());

    set<pair<const CWalletTx*,unsigned int> > setCoinsRet2;

    BOOST_FOREACH(const COutput& out, vCoins)
    {
        // collateral inputs will always be a multiple of DARSEND_COLLATERAL, up to five
        if(IsCollateralAmount(out.tx->vout[out.i].nValue))
        {
            CTxIn vin = CTxIn(out.tx->GetHash(),out.i);

            vin.prevPubKey = out.tx->vout[out.i].scriptPubKey; // the inputs PubKey
            nValueRet += out.tx->vout[out.i].nValue;
            setCoinsRet.push_back(vin);
            setCoinsRet2.insert(make_pair(out.tx, out.i));

            return true;
        }
    }

    return false;
}

int CWallet::CountInputsWithAmount(int64_t nInputAmount)
{
    int64_t nTotal = 0;
    {
        LOCK(cs_wallet);

        for (auto it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;

            if (pcoin->IsTrusted())
            {
                int nDepth = pcoin->GetDepthInMainChain();

                for (unsigned int i = 0; i < pcoin->vout.size(); i++)
                {
                    //isminetype mine = IsMine(pcoin->vout[i]);
		    bool mine = IsMine(pcoin->vout[i]);
                    //COutput out = COutput(pcoin, i, nDepth, (mine & ISMINE_SPENDABLE) != ISMINE_NO);
		    COutput out = COutput(pcoin, i, nDepth, mine);
                    CTxIn vin = CTxIn(out.tx->GetHash(), out.i);

                    if(out.tx->vout[out.i].nValue != nInputAmount) continue;
                    if(!CDarkSendPool::IsDenominatedAmount(pcoin->vout[i].nValue)) continue;
                    //if(IsSpent(out.tx->GetHash(), i) || !IsMine(pcoin->vout[i]) || !IsDenominated(vin)) continue;
		    if(pcoin->IsSpent(i) || !IsMine(pcoin->vout[i]) || !IsDenominated(vin)) continue;

                    nTotal++;
                }
            }
        }
    }

    return nTotal;
}

bool CWallet::HasCollateralInputs() const
{
    vector<COutput> vCoins;
    AvailableCoins(vCoins);
    int nFound = 0;

    BOOST_FOREACH(const COutput& out, vCoins)
        if(IsCollateralAmount(out.tx->vout[out.i].nValue)) nFound++;

    return nFound > 1; // should have more than one just in case
}

bool CWallet::IsCollateralAmount(int64_t nInputAmount) const
{
    return  nInputAmount == (DARKSEND_COLLATERAL * 5)+DARKSEND_FEE ||
            nInputAmount == (DARKSEND_COLLATERAL * 4)+DARKSEND_FEE ||
            nInputAmount == (DARKSEND_COLLATERAL * 3)+DARKSEND_FEE ||
            nInputAmount == (DARKSEND_COLLATERAL * 2)+DARKSEND_FEE ||
            nInputAmount == (DARKSEND_COLLATERAL * 1)+DARKSEND_FEE;
}

bool CWallet::SelectCoinsWithoutDenomination(int64_t nTargetValue, set<pair<const CWalletTx*,unsigned int> >& setCoinsRet, int64_t& nValueRet) const
{
    CCoinControl *coinControl=NULL;

    vector<COutput> vCoins;
    AvailableCoins(vCoins, true, coinControl, ONLY_NONDENOMINATED);

    BOOST_FOREACH(const COutput& out, vCoins)
    {
        nValueRet += out.tx->vout[out.i].nValue;
        setCoinsRet.insert(make_pair(out.tx, out.i));
    }
    return (nValueRet >= nTargetValue);
}

bool CWallet::CreateCollateralTransaction(CTransaction& txCollateral, std::string strReason)
{
    /*
        To doublespend a collateral transaction, it will require a fee higher than this. So there's
        still a significant cost.
    */
    int64_t nFeeRet = 0.001  *COIN;

    txCollateral.vin.clear();
    txCollateral.vout.clear();

    CReserveKey reservekey(this);
    int64_t nValueIn2 = 0;
    std::vector<CTxIn> vCoinsCollateral;

    if (!SelectCoinsCollateral(vCoinsCollateral, nValueIn2))
    {
        strReason = "Error: Darksend requires a collateral transaction and could not locate an acceptable input!";
        return false;
    }

    // make our change address
    CScript scriptChange;
    CPubKey vchPubKey;
    assert(reservekey.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptChange =GetScriptForDestination(vchPubKey.GetID());
    reservekey.KeepKey();

    BOOST_FOREACH(CTxIn v, vCoinsCollateral)
        txCollateral.vin.push_back(v);

    if(nValueIn2 - DARKSEND_COLLATERAL - nFeeRet > 0)
    {
        //pay collateral charge in fees
        CTxOut vout3 = CTxOut(nValueIn2 - DARKSEND_COLLATERAL, scriptChange);
        txCollateral.vout.push_back(vout3);
    }

    int vinNumber = 0;

    BOOST_FOREACH(CTxIn v, txCollateral.vin)
    {
        if(!SignSignature(*this, v.prevPubKey, txCollateral, vinNumber, int(SIGHASH_ALL|SIGHASH_ANYONECANPAY)))
        {
            BOOST_FOREACH(CTxIn v, vCoinsCollateral)
                UnlockCoin(v.prevout);

            strReason = "CDarkSendPool::Sign - Unable to sign collateral transaction! \n";
            return false;
        }

        vinNumber++;
    }

    return true;
}

bool CWallet::ConvertList(std::vector<CTxIn> vCoins, std::vector<int64_t>& vecAmounts)
{
    BOOST_FOREACH(CTxIn i, vCoins)
    {
        if (mapWallet.count(i.prevout.hash))
        {
            CWalletTx& wtx = mapWallet[i.prevout.hash];

            if(i.prevout.n < wtx.vout.size())
                vecAmounts.push_back(wtx.vout[i.prevout.n].nValue);
        }
        else
            LogPrintf("ConvertList -- Couldn't find transaction\n");
    }

    return true;
}


bool CWallet::CreateTransaction(const vector<pair<CScript, int64_t> >& vecSend, CWalletTx& wtxNew, CReserveKey& reservekey, int64_t& nFeeRet, std::string& strFailReason, const CCoinControl* coinControl, AvailableCoinsType coin_type, bool useIX)
{
    int64_t nValue = 0;

    BOOST_FOREACH (const PAIRTYPE(CScript, int64_t)& s, vecSend)
    {
        if (nValue < 0)
            return false;

        nValue += s.second;
    }

    if (vecSend.empty() || nValue < 0)
        return false;

    wtxNew.BindWallet(this);
    CTransaction txNew;

    {
        LOCK2(cs_main, cs_wallet);

        // txdb must be opened before the mapWallet lock
        CTxDB txdb("r");
        {
            nFeeRet = nTransactionFee;

            while (true)
            {
                wtxNew.vin.clear();
                wtxNew.vout.clear();
                wtxNew.fFromMe = true;

                int64_t nTotalValue = nValue + nFeeRet;
                double dPriority = 0;

                // vouts to the payees
                BOOST_FOREACH (const PAIRTYPE(CScript, int64_t)& s, vecSend)
                    wtxNew.vout.push_back(CTxOut(s.second, s.first));

                // Choose coins to use
                set<pair<const CWalletTx*,unsigned int> > setCoins;
                int64_t nValueIn = 0;

                if (!SelectCoins(nTotalValue, wtxNew.nTime, setCoins, nValueIn, coinControl))
                {
                    if(coin_type == ALL_COINS) {
                        strFailReason = _("Insufficient funds.");
                    } else if (coin_type == ONLY_NONDENOMINATED) {
                        strFailReason = _("Unable to locate enough Darksend non-denominated funds for this transaction.");
                    } else if (coin_type == ONLY_NONDENOMINATED_NOTMN) {
                        strFailReason = _("Unable to locate enough Darksend non-denominated funds for this transaction that are not equal 1000 DRK.");
                    } else {
                        strFailReason = _("Unable to locate enough Darksend denominated funds for this transaction.");
                        strFailReason += _("Darksend uses exact denominated amounts to send funds, you might simply need to anonymize some more coins.");
                    }

                    return false;
                }

                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
                {
                    int64_t nCredit = pcoin.first->vout[pcoin.second].nValue;
                    dPriority += (double)nCredit * pcoin.first->GetDepthInMainChain();
                }

                int64_t nChange = nValueIn - nValue - nFeeRet;

                // if sub-cent change is required, the fee must be raised to at least MIN_TX_FEE
                // or until nChange becomes zero
                // NOTE: this depends on the exact behaviour of GetMinFee
                if (nFeeRet < MIN_TX_FEE && nChange > 0 && nChange < CENT)
                {
                    int64_t nMoveToFee = min(nChange, MIN_TX_FEE - nFeeRet);
                    nChange -= nMoveToFee;
                    nFeeRet += nMoveToFee;
                }

                if (nChange > 0)
                {
                    // Fill a vout to ourself
                    // TODO: pass in scriptChange instead of reservekey so
                    // change transaction isn't always pay-to-bitcoin-address
                    CScript scriptChange;

                    // coin control: send change to custom address
                    if (coinControl && !boost::get<CNoDestination>(&coinControl->destChange))
                        scriptChange.SetDestination(coinControl->destChange);

                    // no coin control: send change to newly generated address
                    else
                    {
                        // Note: We use a new key here to keep it from being obvious which side is the change.
                        //  The drawback is that by not reusing a previous key, the change may be lost if a
                        //  backup is restored, if the backup doesn't have the new private key for the change.
                        //  If we reused the old key, it would be possible to add code to look for and
                        //  rediscover unknown transactions that were written with keys of ours to recover
                        //  post-backup change.

                        // Reserve a new key pair from key pool
                        CPubKey vchPubKey;
                        assert(reservekey.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked

                        scriptChange.SetDestination(vchPubKey.GetID());
                    }

                    // Insert change txn at random position:
                    vector<CTxOut>::iterator position = wtxNew.vout.begin()+GetRandInt(wtxNew.vout.size());
                    wtxNew.vout.insert(position, CTxOut(nChange, scriptChange));
                }
                else
                    reservekey.ReturnKey();

                // Fill vin
                BOOST_FOREACH(const PAIRTYPE(const CWalletTx*,unsigned int)& coin, setCoins)
                    wtxNew.vin.push_back(CTxIn(coin.first->GetHash(),coin.second));

                // Sign
                int nIn = 0;
                BOOST_FOREACH(const PAIRTYPE(const CWalletTx*,unsigned int)& coin, setCoins)
                    if (!SignSignature(*this, *coin.first, wtxNew, nIn++))
                        return false;

                // Limit size
                unsigned int nBytes = ::GetSerializeSize(*(CTransaction*)&wtxNew, SER_NETWORK, PROTOCOL_VERSION);
                if (nBytes >= MAX_BLOCK_SIZE_GEN/5)
                    return false;
                dPriority /= nBytes;

                // Check that enough fee is included
                int64_t nPayFee = nTransactionFee * (1 + (int64_t)nBytes / 1000);
                int64_t nMinFee = wtxNew.GetMinFee(1, GMF_SEND, nBytes);

                if (nFeeRet < max(nPayFee, nMinFee))
                {
                    nFeeRet = max(nPayFee, nMinFee);
                    continue;
                }

                // Fill vtxPrev by copying from previous transactions vtxPrev
                wtxNew.AddSupportingTransactions(txdb);
                wtxNew.fTimeReceivedIsTxTime = true;

                break;
            }
        }
    }

    return true;
}

bool CWallet::CreateTransaction(CScript scriptPubKey, int64_t nValue, CWalletTx& wtxNew, CReserveKey& reservekey,
                                int64_t& nFeeRet, const CCoinControl* coinControl)
{
    vector< pair<CScript, int64_t> > vecSend;
    vecSend.push_back(make_pair(scriptPubKey, nValue));
    std::string strFailReason;
    return CreateTransaction(vecSend, wtxNew, reservekey, nFeeRet, strFailReason, coinControl);
}

uint64_t CWallet::GetStakeWeight() const
{
    // Choose coins to use
    int64_t nBalance = GetBalance();

    if (nBalance <= nReserveBalance)
        return 0;

    vector<const CWalletTx*> vwtxPrev;
    set<pair<const CWalletTx*,unsigned int> > setCoins;
    int64_t nValueIn = 0;

    if (!SelectCoinsSimple(nBalance - nReserveBalance, GetTime(), nCoinbaseMaturity + 10, setCoins, nValueIn))
        return 0;

    if (setCoins.empty())
        return 0;

    uint64_t nWeight = 0;
    int64_t nCurrentTime = GetTime();
    CTxDB txdb("r");

    LOCK2(cs_main, cs_wallet);

    BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
    {
        CTxIndex txindex;

        if (!txdb.ReadTxIndex(pcoin.first->GetHash(), txindex))
            continue;

        if (nCurrentTime - pcoin.first->nTime > nStakeMinAge)
            nWeight += pcoin.first->vout[pcoin.second].nValue;
    }

    return nWeight;
}

bool CWallet::CreateCoinStake(const CKeyStore& keystore, unsigned int nBits, int64_t nSearchInterval, int64_t nFees, CTransaction& txNew, CKey& key)
{
    CBlockIndex* pindexPrev = pindexBest;
    CBigNum bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);

    txNew.vin.clear();
    txNew.vout.clear();

    // Mark coin stake transaction
    CScript scriptEmpty;
    scriptEmpty.clear();
    txNew.vout.push_back(CTxOut(0, scriptEmpty));

    // Choose coins to use
    int64_t nBalance = GetBalance();

    if (nBalance <= nReserveBalance)
        return false;

    vector<const CWalletTx*> vwtxPrev;

    set<pair<const CWalletTx*,unsigned int> > setCoins;
    int64_t nValueIn = 0;

    // Select coins with suitable depth
    if (!SelectCoinsSimple(nBalance - nReserveBalance, txNew.nTime, nCoinbaseMaturity + 10, setCoins, nValueIn))
        return false;

    if (setCoins.empty())
        return false;

    int64_t nCredit = 0;
    CScript scriptPubKeyKernel;
    CTxDB txdb("r");

    BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
    {
        CTxIndex txindex;
        {
            LOCK2(cs_main, cs_wallet);
            if (!txdb.ReadTxIndex(pcoin.first->GetHash(), txindex))
                continue;
        }

        // Read block header
        CBlock block;
        {
            LOCK2(cs_main, cs_wallet);
            if (!block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
                continue;
        }

        static int nMaxStakeSearchInterval = 60;

        if (block.GetBlockTime() + nStakeMinAge > txNew.nTime - nMaxStakeSearchInterval)
            continue; // only count coins meeting min age requirement

        bool fKernelFound = false;

        for (unsigned int n = 0; n < min(nSearchInterval,(int64_t) nMaxStakeSearchInterval) &&
             !fKernelFound && !fShutdown && pindexPrev == pindexBest; n++)
        {
            // Search backward in time from the given txNew timestamp
            // Search nSearchInterval seconds back up to nMaxStakeSearchInterval
            uint256 hashProofOfStake = 0, targetProofOfStake = 0;
            COutPoint prevoutStake = COutPoint(pcoin.first->GetHash(), pcoin.second);

            if (CheckStakeKernelHash(pindexPrev, nBits, block, txindex.pos.nTxPos - txindex.pos.nBlockPos, *pcoin.first,
                                     prevoutStake, txNew.nTime - n, hashProofOfStake, targetProofOfStake))
            {
                // Found a kernel
                if (fDebug && GetBoolArg("-printcoinstake"))
                    LogPrintf("%s : kernel found\n", __func__);

                vector<valtype> vSolutions;
                txnouttype whichType;
                CScript scriptPubKeyOut;
                scriptPubKeyKernel = pcoin.first->vout[pcoin.second].scriptPubKey;

                if (!Solver(scriptPubKeyKernel, whichType, vSolutions))
                {
                    if (fDebug && GetBoolArg("-printcoinstake"))
                        LogPrintf("%s : failed to parse kernel\n", __func__);

                    break;
                }

                if (fDebug && GetBoolArg("-printcoinstake"))
                    LogPrintf("%s : parsed kernel type=%d\n", __func__, whichType);

                if (whichType != TX_PUBKEY && whichType != TX_PUBKEYHASH)
                {
                    if (fDebug && GetBoolArg("-printcoinstake"))
                        LogPrintf("CreateCoinStake : no support for kernel type=%d\n", whichType);

                    break;  // only support pay to public key and pay to address
                }

                if (whichType == TX_PUBKEYHASH) // pay to address type
                {
                    // convert to pay to public key type
                    if (!keystore.GetKey(uint160(vSolutions[0]), key))
                    {
                        if (fDebug && GetBoolArg("-printcoinstake"))
                            LogPrintf("%s : failed to get key for kernel type=%d\n", __func__, whichType);

                        break;  // unable to find corresponding public key
                    }

                    scriptPubKeyOut << key.GetPubKey() << OP_CHECKSIG;
                }

                if (whichType == TX_PUBKEY)
                {
                    valtype& vchPubKey = vSolutions[0];

                    if (!keystore.GetKey(Hash160(vchPubKey), key))
                    {
                        if (fDebug && GetBoolArg("-printcoinstake"))
                            LogPrintf("%s : failed to get key for kernel type=%d\n", __func__, whichType);

                        break;  // unable to find corresponding public key
                    }

                    if (key.GetPubKey() != vchPubKey)
                    {
                        if (fDebug && GetBoolArg("-printcoinstake"))
                            LogPrintf("%s : invalid key for kernel type=%d\n", __func__, whichType);

                        break; // keys mismatch
                    }

                    scriptPubKeyOut = scriptPubKeyKernel;
                }

                txNew.nTime -= n;
                txNew.vin.push_back(CTxIn(pcoin.first->GetHash(), pcoin.second));
                nCredit += pcoin.first->vout[pcoin.second].nValue;
                vwtxPrev.push_back(pcoin.first);
                txNew.vout.push_back(CTxOut(0, scriptPubKeyOut));

                if (GetWeight(block.GetBlockTime(), (int64_t)txNew.nTime) < nStakeSplitAge)
                    txNew.vout.push_back(CTxOut(0, scriptPubKeyOut)); //split stake

                if (fDebug && GetBoolArg("-printcoinstake"))
                    LogPrintf("%s : added kernel type=%d\n", __func__, whichType);

                fKernelFound = true;
                break;
            }
        }

        if (fKernelFound || fShutdown)
            break; // if kernel is found stop searching
    }

    if (nCredit == 0 || nCredit > nBalance - nReserveBalance)
        return false;

    BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
    {
        // Attempt to add more inputs
        // Only add coins of the same key/address as kernel
        if (txNew.vout.size() == 2 && ((pcoin.first->vout[pcoin.second].scriptPubKey == scriptPubKeyKernel || pcoin.first->vout[pcoin.second].scriptPubKey == txNew.vout[1].scriptPubKey))
            && pcoin.first->GetHash() != txNew.vin[0].prevout.hash)
        {
            int64_t nTimeWeight = GetWeight((int64_t)pcoin.first->nTime, (int64_t)txNew.nTime);

            // Stop adding more inputs if already too many inputs
            if (txNew.vin.size() >= 100)
                break;
            // Stop adding more inputs if value is already pretty significant
            if (nCredit >= nStakeCombineThreshold)
                break;
            // Stop adding inputs if reached reserve limit
            if (nCredit + pcoin.first->vout[pcoin.second].nValue > nBalance - nReserveBalance)
                break;
            // Do not add additional significant input
            if (pcoin.first->vout[pcoin.second].nValue >= nStakeCombineThreshold)
                continue;
            // Do not add input that is still too young
            if (nTimeWeight < nStakeMinAge)
                continue;

            txNew.vin.push_back(CTxIn(pcoin.first->GetHash(), pcoin.second));
            nCredit += pcoin.first->vout[pcoin.second].nValue;
            vwtxPrev.push_back(pcoin.first);
        }
    }

    // Calculate coin age reward
    int64_t nReward;
    {
        uint64_t nCoinAge;
        CTxDB txdb("r");
        if (!txNew.GetCoinAge(txdb, nCoinAge))
            return error("CreateCoinStake : failed to calculate coin age");

        nReward = GetProofOfStakeReward(nCoinAge, nFees, pindexPrev->nHeight + 1);
        if (nReward <= 0)
            return false;

        nCredit += nReward;
    }

    // Masternode Payments
    int payments = 1;
    // start masternode payments
    bool bMasterNodePayment = true; // note was false, set true to test

    if ( fTestNet ){
        bMasterNodePayment = pindexPrev->nHeight + 1 >= START_MASTERNODE_PAYMENTS_TESTNET;
    }else{
        if (GetTime() > START_MASTERNODE_PAYMENTS){
            bMasterNodePayment = true;
        }
    }

    CScript payee;
    bool hasPayment = true;
    if(bMasterNodePayment) {
        //spork
        if(!masternodePayments.GetBlockPayee(pindexPrev->nHeight+1, payee)){
            hasPayment = false;
            LogPrintf("CreateCoinStake: Failed to detect masternode to pay for block %d\n", pindexPrev->nHeight+1);
        }
    }

    unsigned int nPosMnPmt = ~1;
    if(hasPayment){
        payments = txNew.vout.size() + 1;
        txNew.vout.resize(payments);
        nPosMnPmt = payments - 1;
        txNew.vout[nPosMnPmt].scriptPubKey = payee;
        txNew.vout[nPosMnPmt].nValue = 0;

        CTxDestination address1;
        ExtractDestination(payee, address1);
        CBitcoinAddress address2(address1);

        LogPrintf("Masternode payment to %s\n", address2.ToString().c_str());
    }

    //Add payment to developer address
    int nOut = txNew.vout.size() + 1;
    txNew.vout.resize(nOut);
    unsigned int nDevPmtPos = nOut - 1;
    txNew.vout[nDevPmtPos].scriptPubKey = GetDeveloperScript();
    int64_t nDevPmt = GetDeveloperPayment(nReward);
    txNew.vout[nDevPmtPos].nValue = nDevPmt;

    int64_t blockValue = nCredit - nDevPmt;
    int64_t masternodePayment = GetMasternodePayment(pindexPrev->nHeight+1, nReward);


    // Set output amount
    if (!hasPayment && txNew.vout.size() == 4) // 2 stake outputs, stake was split, no masternode payment, plus dev pmt
    {
        txNew.vout[1].nValue = (blockValue / 2 / CENT) * CENT;
        txNew.vout[2].nValue = blockValue - txNew.vout[1].nValue;
    }
    else if(hasPayment && txNew.vout.size() == 5) // 2 stake outputs, stake was split, plus a masternode payment, plus dev pmt
    {
        txNew.vout[nPosMnPmt].nValue = masternodePayment;
        blockValue -= masternodePayment;
        txNew.vout[1].nValue = (blockValue / 2 / CENT) * CENT;
        txNew.vout[2].nValue = blockValue - txNew.vout[1].nValue;
    }
    else if(!hasPayment && txNew.vout.size() == 3) // only 1 stake output, was not split, no masternode payment, plus dev pmt
        txNew.vout[1].nValue = blockValue;
    else if(hasPayment && txNew.vout.size() == 4) // only 1 stake output, was not split, plus a masternode payment, plus dev pmt
    {
        txNew.vout[nPosMnPmt].nValue = masternodePayment;
        blockValue -= masternodePayment;
        txNew.vout[1].nValue = blockValue;
    }


    // Sign
    int nIn = 0;
    BOOST_FOREACH(const CWalletTx* pcoin, vwtxPrev)
    {
        if (!SignSignature(*this, *pcoin, txNew, nIn++))
            return error("CreateCoinStake : failed to sign coinstake");
    }

    // Limit size
    unsigned int nBytes = ::GetSerializeSize(txNew, SER_NETWORK, PROTOCOL_VERSION);
    if (nBytes >= MAX_BLOCK_SIZE_GEN/5)
        return error("CreateCoinStake : exceeded coinstake size limit");

    // Successfully generated coinstake
    return true;
}


// Call after CreateTransaction unless you want to abort
bool CWallet::CommitTransaction(CWalletTx& wtxNew, CReserveKey& reservekey)
{
    {
        LOCK2(cs_main, cs_wallet);
        LogPrintf("CommitTransaction:\n%s", wtxNew.ToString().c_str());
        {
            // This is only to keep the database open to defeat the auto-flush for the
            // duration of this scope.  This is the only place where this optimization
            // maybe makes sense; please don't do it anywhere else.
            CWalletDB* pwalletdb = fFileBacked ? new CWalletDB(strWalletFile,"r") : NULL;

            // Take key pair from key pool so it won't be used again
            reservekey.KeepKey();

            // Add tx to wallet, because if it has change it's also ours,
            // otherwise just for transaction history.
            AddToWallet(wtxNew);

            // Mark old coins as spent
            set<CWalletTx*> setCoins;
            BOOST_FOREACH(const CTxIn& txin, wtxNew.vin)
            {
                CWalletTx &coin = mapWallet[txin.prevout.hash];
                coin.BindWallet(this);
                coin.MarkSpent(txin.prevout.n);
                coin.WriteToDisk();
                NotifyTransactionChanged(this, coin.GetHash(), CT_UPDATED);
            }

            if (fFileBacked)
                delete pwalletdb;
        }

        // Track how many getdata requests our transaction gets
        mapRequestCount[wtxNew.GetHash()] = 0;

        // Broadcast
        if (!wtxNew.AcceptToMemoryPool())
        {
            // This must not fail. The transaction has already been signed and recorded.
            LogPrintf("CommitTransaction() : Error: Transaction not valid\n");
            return false;
        }
        wtxNew.RelayWalletTransaction();
    }
    return true;
}




string CWallet::SendMoney(CScript scriptPubKey, int64_t nValue, CWalletTx& wtxNew, bool fAskFee)
{
    CReserveKey reservekey(this);
    int64_t nFeeRequired;

    if (IsLocked())
    {
        string strError = _("Error: Wallet locked, unable to create transaction  ");
        LogPrintf("SendMoney() : %s", strError.c_str());
        return strError;
    }
    if (fWalletUnlockStakingOnly)
    {
        string strError = _("Error: Wallet unlocked for staking only, unable to create transaction.");
        LogPrintf("SendMoney() : %s", strError.c_str());
        return strError;
    }
    if (!CreateTransaction(scriptPubKey, nValue, wtxNew, reservekey, nFeeRequired))
    {
        string strError;
        if (nValue + nFeeRequired > GetBalance())
            strError = strprintf(_("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds  "), FormatMoney(nFeeRequired).c_str());
        else
            strError = _("Error: Transaction creation failed  ");
        LogPrintf("SendMoney() : %s", strError.c_str());
        return strError;
    }

    if (fAskFee && !uiInterface.ThreadSafeAskFee(nFeeRequired, _("Sending...")))
        return "ABORTED";

    if (!CommitTransaction(wtxNew, reservekey))
        return _("Error: The transaction was rejected.  This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");

    return "";
}



string CWallet::SendMoneyToDestination(const CTxDestination& address, int64_t nValue, CWalletTx& wtxNew, bool fAskFee)
{
    // Check amount
    if (nValue <= 0)
        return _("Invalid amount");
    if (nValue + nTransactionFee > GetBalance())
        return _("Insufficient funds");

    // Parse Bitcoin address
    CScript scriptPubKey;
    scriptPubKey.SetDestination(address);

    return SendMoney(scriptPubKey, nValue, wtxNew, fAskFee);
}

// TODO: Remove me
string CWallet::PrepareDarksendDenominate(int minRounds, int maxRounds)
{
    return "";
}

int64_t CWallet::GetTotalValue(std::vector<CTxIn> vCoins)
{
    int64_t nTotalValue = 0;
    CWalletTx wtx;

    BOOST_FOREACH(CTxIn i, vCoins)
    {
        if (mapWallet.count(i.prevout.hash))
        {
            CWalletTx& wtx = mapWallet[i.prevout.hash];

            if(i.prevout.n < wtx.vout.size())
                nTotalValue += wtx.vout[i.prevout.n].nValue;
        }
        else
            LogPrintf("GetTotalValue -- Couldn't find transaction\n");
    }

    return nTotalValue;
}




DBErrors CWallet::LoadWallet(bool& fFirstRunRet)
{
    if (!fFileBacked)
        return DB_LOAD_OK;

    fFirstRunRet = false;
    DBErrors nLoadWalletRet = CWalletDB(strWalletFile,"cr+").LoadWallet(this);

    if (nLoadWalletRet == DB_NEED_REWRITE)
    {
        if (CDB::Rewrite(strWalletFile, "\x04pool"))
        {
            setKeyPool.clear();
            // Note: can't top-up keypool here, because wallet is locked.
            // User will be prompted to unlock wallet the next operation
            // the requires a new key.
        }
    }

    if (nLoadWalletRet != DB_LOAD_OK)
        return nLoadWalletRet;

    fFirstRunRet = !vchDefaultKey.IsValid();
    NewThread(ThreadFlushWalletDB, &strWalletFile);
    return DB_LOAD_OK;
}

DBErrors CWallet::ZapWalletTx(std::vector<CWalletTx>& vWtx)
{
    if (!fFileBacked)
        return DB_LOAD_OK;

    DBErrors nZapWalletTxRet = CWalletDB(strWalletFile, "cr+").ZapWalletTx(this, vWtx);

    if (nZapWalletTxRet == DB_NEED_REWRITE)
    {
        if (CDB::Rewrite(strWalletFile, "\x04pool"))
        {
            LOCK(cs_wallet);
            setKeyPool.clear();
            // Note: can't top-up keypool here, because wallet is locked.
            // User will be prompted to unlock wallet the next operation
            // that requires a new key.
        }
    }

    if (nZapWalletTxRet != DB_LOAD_OK)
        return nZapWalletTxRet;

    return DB_LOAD_OK;
}

bool CWallet::SetAddressBookName(const CTxDestination& address, const string& strName)
{
    std::map<CTxDestination, std::string>::iterator mi = mapAddressBook.find(address);
    mapAddressBook[address] = strName;
    NotifyAddressBookChanged(this, address, strName, ::IsMine(*this, address), (mi == mapAddressBook.end()) ? CT_NEW : CT_UPDATED);

    if (!fFileBacked)
        return false;

    return CWalletDB(strWalletFile).WriteName(CBitcoinAddress(address).ToString(), strName);
}

bool CWallet::DelAddressBookName(const CTxDestination& address)
{
    mapAddressBook.erase(address);
    NotifyAddressBookChanged(this, address, "", ::IsMine(*this, address), CT_DELETED);

    if (!fFileBacked)
        return false;

    return CWalletDB(strWalletFile).EraseName(CBitcoinAddress(address).ToString());
}


void CWallet::PrintWallet(const CBlock& block)
{
    {
        LOCK(cs_wallet);

        if (block.IsProofOfWork() && mapWallet.count(block.vtx[0].GetHash()))
        {
            CWalletTx& wtx = mapWallet[block.vtx[0].GetHash()];
            LogPrintf("    mine:  %d  %d  %d", wtx.GetDepthInMainChain(), wtx.GetBlocksToMaturity(), wtx.GetCredit());
        }

        if (block.IsProofOfStake() && mapWallet.count(block.vtx[1].GetHash()))
        {
            CWalletTx& wtx = mapWallet[block.vtx[1].GetHash()];
            LogPrintf("    stake: %d  %d  %d", wtx.GetDepthInMainChain(), wtx.GetBlocksToMaturity(), wtx.GetCredit());
         }

    }

    LogPrintf("\n");
}

bool CWallet::GetTransaction(const uint256 &hashTx, CWalletTx& wtx)
{
    {
        LOCK(cs_wallet);
        auto mi = mapWallet.find(hashTx);

        if (mi != mapWallet.end())
        {
            wtx = (*mi).second;
            return true;
        }
    }

    return false;
}

bool CWallet::SetDefaultKey(const CPubKey &vchPubKey)
{
    if (fFileBacked)
    {
        if (!CWalletDB(strWalletFile).WriteDefaultKey(vchPubKey))
            return false;
    }

    vchDefaultKey = vchPubKey;
    return true;
}

bool GetWalletFile(CWallet* pwallet, string &strWalletFileOut)
{
    if (!pwallet->fFileBacked)
        return false;

    strWalletFileOut = pwallet->strWalletFile;
    return true;
}

// Mark old keypool keys as used,
// and generate all new keys

bool CWallet::NewKeyPool()
{
    {
        LOCK(cs_wallet);
        CWalletDB walletdb(strWalletFile);

        BOOST_FOREACH(int64_t nIndex, setKeyPool)
            walletdb.ErasePool(nIndex);

        setKeyPool.clear();

        if (IsLocked())
            return false;

        int64_t nKeys = max(GetArg("-keypool", 100), (int64_t) 0);

        for (int i = 0; i < nKeys; i++)
        {
            int64_t nIndex = i+1;
            walletdb.WritePool(nIndex, CKeyPool(GenerateNewKey()));
            setKeyPool.insert(nIndex);
        }

        LogPrintf("CWallet::NewKeyPool wrote %d new keys\n", nKeys);
    }

    return true;
}

bool CWallet::TopUpKeyPool(unsigned int nSize)
{
    {
        LOCK(cs_wallet);

        if (IsLocked())
            return false;

        CWalletDB walletdb(strWalletFile);

        // Top up key pool
        unsigned int nTargetSize;

        if (nSize > 0)
            nTargetSize = nSize;
        else
            nTargetSize = max(GetArg("-keypool", 100), (int64_t)0);

        while (setKeyPool.size() < (nTargetSize + 1))
        {
            int64_t nEnd = 1;

            if (!setKeyPool.empty())
                nEnd = *(--setKeyPool.end()) + 1;

            if (!walletdb.WritePool(nEnd, CKeyPool(GenerateNewKey())))
                throw runtime_error("TopUpKeyPool() : writing generated key failed");

            setKeyPool.insert(nEnd);
            LogPrintf("keypool added key %d, size=%u\n", nEnd, setKeyPool.size());
        }
    }

    return true;
}

void CWallet::ReserveKeyFromKeyPool(int64_t& nIndex, CKeyPool& keypool)
{
    nIndex = -1;
    keypool.vchPubKey = CPubKey();

    {
        LOCK(cs_wallet);

        if (!IsLocked())
            TopUpKeyPool();

        // Get the oldest key
        if(setKeyPool.empty())
            return;

        CWalletDB walletdb(strWalletFile);
        nIndex = *(setKeyPool.begin());
        setKeyPool.erase(setKeyPool.begin());

        if (!walletdb.ReadPool(nIndex, keypool))
            throw runtime_error("ReserveKeyFromKeyPool() : read failed");

        if (!HaveKey(keypool.vchPubKey.GetID()))
            throw runtime_error("ReserveKeyFromKeyPool() : unknown key in key pool");

        assert(keypool.vchPubKey.IsValid());

        if (fDebug && GetBoolArg("-printkeypool"))
            LogPrintf("keypool reserve %d\n", nIndex);
    }
}

int64_t CWallet::AddReserveKey(const CKeyPool& keypool)
{
    {
        LOCK2(cs_main, cs_wallet);
        CWalletDB walletdb(strWalletFile);
        int64_t nIndex = 1 + *(--setKeyPool.end());

        if (!walletdb.WritePool(nIndex, keypool))
            throw runtime_error("AddReserveKey() : writing added key failed");

        setKeyPool.insert(nIndex);
        return nIndex;
    }

    return -1;
}

void CWallet::KeepKey(int64_t nIndex)
{
    // Remove from key pool
    if (fFileBacked)
    {
        CWalletDB walletdb(strWalletFile);
        walletdb.ErasePool(nIndex);
    }

    if(fDebug)
        LogPrintf("keypool keep %d\n", nIndex);
}

void CWallet::ReturnKey(int64_t nIndex)
{
    // Return to key pool
    {
        LOCK(cs_wallet);
        setKeyPool.insert(nIndex);
    }

    if(fDebug)
        LogPrintf("keypool return %d\n", nIndex);
}

bool CWallet::GetKeyFromPool(CPubKey& result, bool fAllowReuse)
{
    int64_t nIndex = 0;
    CKeyPool keypool;

    {
        LOCK(cs_wallet);
        ReserveKeyFromKeyPool(nIndex, keypool);

        if (nIndex == -1)
        {
            if (fAllowReuse && vchDefaultKey.IsValid())
            {
                result = vchDefaultKey;
                return true;
            }

            if (IsLocked())
                return false;

            result = GenerateNewKey();
            return true;
        }

        KeepKey(nIndex);
        result = keypool.vchPubKey;
    }

    return true;
}

int64_t CWallet::GetOldestKeyPoolTime()
{
    int64_t nIndex = 0;
    CKeyPool keypool;
    ReserveKeyFromKeyPool(nIndex, keypool);

    if (nIndex == -1)
        return GetTime();

    ReturnKey(nIndex);
    return keypool.nTime;
}

std::map<CTxDestination, int64_t> CWallet::GetAddressBalances()
{
    map<CTxDestination, int64_t> balances;

    {
        LOCK(cs_wallet);

        for (auto walletEntry : mapWallet)
        {
            CWalletTx *pcoin = &walletEntry.second;

            if (!pcoin->IsFinal() || !pcoin->IsTrusted())
                continue;

            if ((pcoin->IsCoinBase() || pcoin->IsCoinStake()) && pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain();

            if (nDepth < (pcoin->IsFromMe() ? 0 : 1))
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            {
                CTxDestination addr;

                if (!IsMine(pcoin->vout[i]))
                    continue;

                if(!ExtractDestination(pcoin->vout[i].scriptPubKey, addr))
                    continue;

                int64_t n = pcoin->IsSpent(i) ? 0 : pcoin->vout[i].nValue;

                if (!balances.count(addr))
                    balances[addr] = 0;

                balances[addr] += n;
            }
        }
    }

    return balances;
}

set< set<CTxDestination> > CWallet::GetAddressGroupings()
{
    set< set<CTxDestination> > groupings;
    set<CTxDestination> grouping;

    for (auto walletEntry : mapWallet)
    {
        CWalletTx *pcoin = &walletEntry.second;

        if (pcoin->vin.size() > 0 && IsMine(pcoin->vin[0]))
        {
            // group all input addresses with each other
            BOOST_FOREACH(CTxIn txin, pcoin->vin)
            {
                CTxDestination address;

                if(!ExtractDestination(mapWallet[txin.prevout.hash].vout[txin.prevout.n].scriptPubKey, address))
                    continue;

                grouping.insert(address);
            }

            // group change with input addresses
            BOOST_FOREACH(CTxOut txout, pcoin->vout)
            {
                if (IsChange(txout))
                {
                    CWalletTx tx = mapWallet[pcoin->vin[0].prevout.hash];
                    CTxDestination txoutAddr;

                    if(!ExtractDestination(txout.scriptPubKey, txoutAddr))
                        continue;

                    grouping.insert(txoutAddr);
                }
            }

            groupings.insert(grouping);
            grouping.clear();
        }

        // group lone addrs by themselves
        for (unsigned int i = 0; i < pcoin->vout.size(); i++)
        {
            if (IsMine(pcoin->vout[i]))
            {
                CTxDestination address;

                if(!ExtractDestination(pcoin->vout[i].scriptPubKey, address))
                    continue;

                grouping.insert(address);
                groupings.insert(grouping);
                grouping.clear();
            }
        }
    }

    set< set<CTxDestination>* > uniqueGroupings; // a set of pointers to groups of addresses
    map< CTxDestination, set<CTxDestination>* > setmap;  // map addresses to the unique group containing it

    BOOST_FOREACH(set<CTxDestination> grouping, groupings)
    {
        // make a set of all the groups hit by this new group
        set< set<CTxDestination>* > hits;
        map< CTxDestination, set<CTxDestination>* >::iterator it;

        BOOST_FOREACH(CTxDestination address, grouping)
            if ((it = setmap.find(address)) != setmap.end())
                hits.insert((*it).second);

        // merge all hit groups into a new single group and delete old groups
        set<CTxDestination>* merged = new set<CTxDestination>(grouping);

        BOOST_FOREACH(set<CTxDestination>* hit, hits)
        {
            merged->insert(hit->begin(), hit->end());
            uniqueGroupings.erase(hit);
            delete hit;
        }

        uniqueGroupings.insert(merged);

        // update setmap
        BOOST_FOREACH(CTxDestination element, *merged)
            setmap[element] = merged;
    }

    set< set<CTxDestination> > ret;

    BOOST_FOREACH(set<CTxDestination>* uniqueGrouping, uniqueGroupings)
    {
        ret.insert(*uniqueGrouping);
        delete uniqueGrouping;
    }

    return ret;
}

// ppcoin: check 'spent' consistency between wallet and txindex
// ppcoin: fix wallet spent state according to txindex
void CWallet::FixSpentCoins(int& nMismatchFound, int64_t& nBalanceInQuestion, bool fCheckOnly)
{
    nMismatchFound = 0;
    nBalanceInQuestion = 0;

    LOCK(cs_wallet);
    vector<CWalletTx*> vCoins;
    vCoins.reserve(mapWallet.size());

    for (auto it = mapWallet.begin(); it != mapWallet.end(); ++it)
        vCoins.push_back(&(*it).second);

    CTxDB txdb("r");

    BOOST_FOREACH(CWalletTx* pcoin, vCoins)
    {
        // Find the corresponding transaction index
        CTxIndex txindex;

        if (!txdb.ReadTxIndex(pcoin->GetHash(), txindex))
            continue;

        for (unsigned int n=0; n < pcoin->vout.size(); n++)
        {
            if (IsMine(pcoin->vout[n]) && pcoin->IsSpent(n) && (txindex.vSpent.size() <= n || txindex.vSpent[n].IsNull()))
            {
                LogPrintf("FixSpentCoins found lost coin %s NTRN %s[%d], %s\n",
                    FormatMoney(pcoin->vout[n].nValue).c_str(), pcoin->GetHash().ToString().c_str(), n, fCheckOnly? "repair not attempted" : "repairing");
                nMismatchFound++;
                nBalanceInQuestion += pcoin->vout[n].nValue;

                if (!fCheckOnly)
                {
                    pcoin->MarkUnspent(n);
                    pcoin->WriteToDisk();
                }
            }
            else if (IsMine(pcoin->vout[n]) && !pcoin->IsSpent(n) && (txindex.vSpent.size() > n && !txindex.vSpent[n].IsNull()))
            {
                LogPrintf("FixSpentCoins found spent coin %s NTRN %s[%d], %s\n",
                    FormatMoney(pcoin->vout[n].nValue).c_str(), pcoin->GetHash().ToString().c_str(), n, fCheckOnly? "repair not attempted" : "repairing");
                nMismatchFound++;
                nBalanceInQuestion += pcoin->vout[n].nValue;

                if (!fCheckOnly)
                {
                    pcoin->MarkSpent(n);
                    pcoin->WriteToDisk();
                }
            }
        }
    }
}

// ppcoin: disable transaction (only for coinstake)
void CWallet::DisableTransaction(const CTransaction &tx)
{
    if (!tx.IsCoinStake() || !IsFromMe(tx))
        return; // only disconnecting coinstake requires marking input unspent

    LOCK(cs_wallet);
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        auto mi = mapWallet.find(txin.prevout.hash);

        if (mi != mapWallet.end())
        {
            CWalletTx& prev = (*mi).second;

            if (txin.prevout.n < prev.vout.size() && IsMine(prev.vout[txin.prevout.n]))
            {
                prev.MarkUnspent(txin.prevout.n);
                prev.WriteToDisk();
            }
        }
    }
}

bool CReserveKey::GetReservedKey(CPubKey& pubkey)
{
    if (nIndex == -1)
    {
        CKeyPool keypool;
        pwallet->ReserveKeyFromKeyPool(nIndex, keypool);

        if (nIndex != -1)
            vchPubKey = keypool.vchPubKey;
        else
        {
            if (pwallet->vchDefaultKey.IsValid())
            {
                LogPrintf("CReserveKey::GetReservedKey(): Warning: Using default key instead of a new key, top up your keypool!");
                vchPubKey = pwallet->vchDefaultKey;
            }
            else
                return false;
        }
    }

    assert(vchPubKey.IsValid());
    pubkey = vchPubKey;
    return true;
}

void CReserveKey::KeepKey()
{
    if (nIndex != -1)
        pwallet->KeepKey(nIndex);

    nIndex = -1;
    vchPubKey = CPubKey();
}

void CReserveKey::ReturnKey()
{
    if (nIndex != -1)
        pwallet->ReturnKey(nIndex);

    nIndex = -1;
    vchPubKey = CPubKey();
}

void CWallet::GetAllReserveKeys(set<CKeyID>& setAddress) const
{
    setAddress.clear();
    CWalletDB walletdb(strWalletFile);
    LOCK2(cs_main, cs_wallet);

    BOOST_FOREACH(const int64_t& id, setKeyPool)
    {
        CKeyPool keypool;

        if (!walletdb.ReadPool(id, keypool))
            throw runtime_error("GetAllReserveKeyHashes() : read failed");

        assert(keypool.vchPubKey.IsValid());
        CKeyID keyID = keypool.vchPubKey.GetID();

        if (!HaveKey(keyID))
            throw runtime_error("GetAllReserveKeyHashes() : unknown key in key pool");

        setAddress.insert(keyID);
    }
}

void CWallet::UpdatedTransaction(const uint256 &hashTx)
{
    {
        LOCK(cs_wallet);

        // Only notify UI if this transaction is in this wallet
        auto mi = mapWallet.find(hashTx);

        if (mi != mapWallet.end())
            NotifyTransactionChanged(this, hashTx, CT_UPDATED);
    }
}

void CWallet::LockCoin(COutPoint& output)
{
    LOCK(cs_wallet); // setLockedCoins
    setLockedCoins.insert(output);
}

void CWallet::UnlockCoin(COutPoint& output)
{
    LOCK(cs_wallet); // setLockedCoins
    setLockedCoins.erase(output);
}

void CWallet::UnlockAllCoins()
{
    LOCK(cs_wallet); // setLockedCoins
    setLockedCoins.clear();
}

bool CWallet::IsLockedCoin(uint256 hash, unsigned int n) const
{
    LOCK(cs_wallet); // setLockedCoins
    COutPoint outpt(hash, n);

    return (setLockedCoins.count(outpt) > 0);
}

void CWallet::ListLockedCoins(std::vector<COutPoint>& vOutpts)
{
    LOCK(cs_wallet); // setLockedCoins

    for (std::set<COutPoint>::iterator it = setLockedCoins.begin(); it != setLockedCoins.end(); it++)
    {
        COutPoint outpt = (*it);
        vOutpts.push_back(outpt);
    }
}

void CWallet::GetKeyBirthTimes(std::map<CKeyID, int64_t> &mapKeyBirth) const
{
    mapKeyBirth.clear();

    // get birth times for keys with metadata
    for (std::map<CKeyID, CKeyMetadata>::const_iterator it = mapKeyMetadata.begin(); it != mapKeyMetadata.end(); it++)
        if (it->second.nCreateTime)
            mapKeyBirth[it->first] = it->second.nCreateTime;

    // map in which we'll infer heights of other keys
    CBlockIndex *pindexMax = FindBlockByHeight(std::max(0, nBestHeight - 144)); // the tip can be reorganised; use a 144-block safety margin
    std::map<CKeyID, CBlockIndex*> mapKeyFirstBlock;
    std::set<CKeyID> setKeys;
    GetKeys(setKeys);

    BOOST_FOREACH(const CKeyID &keyid, setKeys) {
        if (mapKeyBirth.count(keyid) == 0)
            mapKeyFirstBlock[keyid] = pindexMax;
    }
    setKeys.clear();

    // if there are no such keys, we're done
    if (mapKeyFirstBlock.empty())
        return;

    // find first block that affects those keys, if there are any left
    std::vector<CKeyID> vAffected;

    for (auto it = mapWallet.begin(); it != mapWallet.end(); it++)
    {
        // iterate over all wallet transactions...
        const CWalletTx &wtx = (*it).second;
        auto blit = mapBlockIndex.find(wtx.hashBlock);

        if (blit != mapBlockIndex.end() && blit->second->IsInMainChain()) {
            // ... which are already in a block
            int nHeight = blit->second->nHeight;

            BOOST_FOREACH(const CTxOut &txout, wtx.vout)
            {
                // iterate over all their outputs
                ::ExtractAffectedKeys(*this, txout.scriptPubKey, vAffected);

                BOOST_FOREACH(const CKeyID &keyid, vAffected)
                {
                    // ... and all their affected keys
                    auto rit = mapKeyFirstBlock.find(keyid);

                    if (rit != mapKeyFirstBlock.end() && nHeight < rit->second->nHeight)
                        rit->second = blit->second;
                }

                vAffected.clear();
            }
        }
    }

    // Extract block timestamps for those keys
    for (auto it = mapKeyFirstBlock.begin(); it != mapKeyFirstBlock.end(); it++)
        mapKeyBirth[it->first] = it->second->nTime - 7200; // block times can be 2h off
}

bool CWallet::AddAdrenalineNodeConfig(CAdrenalineNodeConfig nodeConfig)
{
    bool rv = CWalletDB(strWalletFile).WriteAdrenalineNodeConfig(nodeConfig.sAlias, nodeConfig);

    if(rv)
	uiInterface.NotifyAdrenalineNodeChanged(nodeConfig);

    return rv;
}



int CMerkleTx::SetMerkleBranch(const CBlock* pblock)
{
    if (fClient)
    {
        if (hashBlock == 0)
            return 0;
    }
    else
    {
        CBlock blockTmp;

        if (pblock == NULL)
        {
            // Load the block this tx is in
            CTxIndex txindex;

            if (!CTxDB("r").ReadTxIndex(GetHash(), txindex))
                return 0;

            if (!blockTmp.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos))
                return 0;

            pblock = &blockTmp;
        }

        // Update the tx's hashBlock
        hashBlock = pblock->GetHash();

        // Locate the transaction
        for (nIndex = 0; nIndex < (int)pblock->vtx.size(); nIndex++)
        {
            if (pblock->vtx[nIndex] == *(CTransaction*)this)
                break;
        }

        if (nIndex == (int)pblock->vtx.size())
        {
            vMerkleBranch.clear();
            nIndex = -1;
            LogPrintf("ERROR: SetMerkleBranch() : couldn't find tx in block\n");
            return 0;
        }

        // Fill in merkle branch
        vMerkleBranch = pblock->GetMerkleBranch(nIndex);
    }

    // Is the tx in a block that's in the main chain
    auto mi = mapBlockIndex.find(hashBlock);

    if (mi == mapBlockIndex.end())
        return 0;

    CBlockIndex* pindex = (*mi).second;

    if (!pindex || !pindex->IsInMainChain())
        return 0;

    return pindexBest->nHeight - pindex->nHeight + 1;
}


int CMerkleTx::GetDepthInMainChainINTERNAL(CBlockIndex* &pindexRet) const
{
    if (hashBlock == 0 || nIndex == -1)
        return 0;

    auto mi = mapBlockIndex.find(hashBlock);

    if (mi == mapBlockIndex.end())
        return 0;

    CBlockIndex* pindex = (*mi).second;

    if (!pindex || !pindex->IsInMainChain())
        return 0;

    // Make sure the merkle branch connects to this block
    if (!fMerkleVerified)
    {
        if (CBlock::CheckMerkleBranch(GetHash(), vMerkleBranch, nIndex) != pindex->hashMerkleRoot)
            return 0;

        fMerkleVerified = true;
    }

    pindexRet = pindex;
    return pindexBest->nHeight - pindex->nHeight + 1;
}

int CMerkleTx::GetDepthInMainChain(CBlockIndex* &pindexRet) const
{
    int nResult = GetDepthInMainChainINTERNAL(pindexRet);

    if (nResult == 0 && !mempool.exists(GetHash()))
        return -1; // Not in chain, not in mempool

    return nResult;
}

int CMerkleTx::GetBlocksToMaturity() const
{
    if (!(IsCoinBase() || IsCoinStake()))
        return 0;

    return max(0, (nCoinbaseMaturity+10) - GetDepthInMainChain());
}


bool CMerkleTx::AcceptToMemoryPool(CTxDB& txdb, bool fCheckInputs)
{
    if (fClient)
    {
        if (!IsInMainChain() && !ClientConnectInputs())
            return false;

        return CTransaction::AcceptToMemoryPool(txdb, fCheckInputs);
    }
    else
    {
        return CTransaction::AcceptToMemoryPool(txdb, fCheckInputs);
    }
}

bool CMerkleTx::AcceptToMemoryPool()
{
    CTxDB txdb("r");
    return AcceptToMemoryPool(txdb);
}
