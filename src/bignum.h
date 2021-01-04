// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2017-2019 The Swipp developers
// Copyright (c) 2021 The Neutron developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING.daemon or http://www.opensource.org/licenses/mit-license.php.

#ifndef __SWIPP_BIGNUM_H__
#define __SWIPP_BIGNUM_H__

#include <openssl/bn.h>
#include <openssl/opensslv.h>
#include <stdexcept>
#include <vector>
#include <stdint.h>

#include "serialize.h"
#include "uint256.h"
#include "version.h"

class bignum_error : public std::runtime_error
{
public:
    explicit bignum_error(const std::string& str) : std::runtime_error(str) { }
};

class CAutoBN_CTX
{
protected:
    BN_CTX* pctx;
    BN_CTX* operator=(BN_CTX* pnew) { return pctx = pnew; }

public:
    CAutoBN_CTX()
    {
        pctx = BN_CTX_new();

        if (pctx == NULL)
            throw bignum_error("CAutoBN_CTX : BN_CTX_new() returned NULL");
    }

    ~CAutoBN_CTX()
    {
        if (pctx != NULL)
            BN_CTX_free(pctx);
    }

    operator BN_CTX*()   { return pctx; }
    BN_CTX& operator*()  { return *pctx; }
    BN_CTX** operator&() { return &pctx; }
    bool operator!()     { return pctx == NULL; }
};

#if OPENSSL_VERSION_NUMBER < 0x10100000L
class CBigNum : public BIGNUM
#else
class CBigNum
#endif
{
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
private:
    BIGNUM *bn;
#endif

public:
    CBigNum()
    {
        #if OPENSSL_VERSION_NUMBER < 0x10100000L
        BN_init(this);
        #else
        bn = BN_new();
        #endif
    }

    CBigNum(const CBigNum& b) : CBigNum()
    {
        if (!BN_copy(getBN(), b.getBNConst()))
        {
            BN_clear_free(getBN());
            throw bignum_error("CBigNum::CBigNum(const CBigNum&) : BN_copy failed");
        }
    }

    CBigNum& operator=(const CBigNum& b)
    {
        if (!BN_copy(getBN(), b.getBNConst()))
            throw bignum_error("CBigNum::operator= : BN_copy failed");

        return *this;
    }

    ~CBigNum()
    {
        BN_clear_free(getBN());
    }

    CBigNum(signed char n) : CBigNum()        { if (n >= 0) setulong(n); else setint64(n); }
    CBigNum(short n) : CBigNum()              { if (n >= 0) setulong(n); else setint64(n); }
    CBigNum(int n) : CBigNum()                { if (n >= 0) setulong(n); else setint64(n); }
    CBigNum(long n) : CBigNum()               { if (n >= 0) setulong(n); else setint64(n); }
    CBigNum(long long n) : CBigNum()          { setint64(n); }
    CBigNum(unsigned char n) : CBigNum()      { setulong(n); }
    CBigNum(unsigned short n) : CBigNum()     { setulong(n); }
    CBigNum(unsigned int n) : CBigNum()       { setulong(n); }
    CBigNum(unsigned long n) : CBigNum()      { setulong(n); }
    CBigNum(unsigned long long n) : CBigNum() { setuint64(n); }

    explicit CBigNum(uint256 n) : CBigNum()
    {
        setuint256(n);
    }

    explicit CBigNum(const std::vector<unsigned char>& vch) : CBigNum()
    {
        setvch(vch);
    }

    #if OPENSSL_VERSION_NUMBER < 0x10100000L
    BIGNUM *getBN() { return this; }
    const BIGNUM *getBNConst() const { return this; }
    #else
    BIGNUM *getBN() { return bn; }
    const BIGNUM *getBNConst() const { return bn; }
    #endif

    // Generates a cryptographically secure random number between zero and range exclusive
    static CBigNum randBignum(const CBigNum& range)
    {
        CBigNum ret;

        if (!BN_rand_range(ret.getBN(), range.getBNConst()))
            throw bignum_error("CBigNum:rand element : BN_rand_range failed");

        return ret;
    }

    // Generates a cryptographically secure random k-bit number
    static CBigNum RandKBitBigum(const uint32_t k)
    {
        CBigNum ret;

        if (!BN_rand(ret.getBN(), k, -1, 0))
            throw bignum_error("CBigNum:rand element : BN_rand failed");

        return ret;
    }

    int bitSize() const
    {
        return  BN_num_bits(getBNConst());
    }

    void setulong(unsigned long n)
    {
        if (!BN_set_word(getBN(), n))
            throw bignum_error("CBigNum conversion from unsigned long : BN_set_word failed");
    }

    unsigned long getulong() const
    {
        return BN_get_word(getBNConst());
    }

    unsigned int getuint() const
    {
        return BN_get_word(getBNConst());
    }

    int getint() const
    {
        unsigned long n = BN_get_word(getBNConst());

        if (!BN_is_negative(getBNConst())) {
            return (n > (unsigned long) std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : n);
        } else {
            return (n > (unsigned long) std::numeric_limits<int>::max() ? std::numeric_limits<int>::min() : -(int) n);
        }
    }

    void setint64(int64_t sn)
    {
        unsigned char pch[sizeof(sn) + 6];
        unsigned char* p = pch + 4;
        bool fNegative;
        uint64_t n;

        if (sn < (int64_t) 0)
        {
            n = -(sn + 1);
            ++n;
            fNegative = true;
        }
        else
        {
            n = sn;
            fNegative = false;
        }

        bool fLeadingZeroes = true;

        for (int i = 0; i < 8; i++)
        {
            unsigned char c = (n >> 56) & 0xff;
            n <<= 8;

            if (fLeadingZeroes)
            {
                if (c == 0)
                    continue;

                if (c & 0x80)
                    *p++ = (fNegative ? 0x80 : 0);
                else if (fNegative)
                    c |= 0x80;

                fLeadingZeroes = false;
            }

            *p++ = c;
        }

        unsigned int nSize = p - (pch + 4);

        pch[0] = (nSize >> 24) & 0xff;
        pch[1] = (nSize >> 16) & 0xff;
        pch[2] = (nSize >> 8) & 0xff;
        pch[3] = (nSize) & 0xff;
        BN_mpi2bn(pch, p - pch, getBN());
    }

    uint64_t getuint64()
    {
        unsigned int nSize = BN_bn2mpi(getBN(), NULL);

        if (nSize < 4)
            return 0;

        std::vector<unsigned char> vch(nSize);
        BN_bn2mpi(getBN(), &vch[0]);

        if (vch.size() > 4)
            vch[4] &= 0x7f;

        uint64_t n = 0;

        for (unsigned int i = 0, j = vch.size()-1; i < sizeof(n) && j >= 4; i++, j--)
            ((unsigned char*) &n)[i] = vch[j];

        return n;
    }

    void setuint64(uint64_t n)
    {
        unsigned char pch[sizeof(n) + 6];
        unsigned char* p = pch + 4;
        bool fLeadingZeroes = true;

        for (int i = 0; i < 8; i++)
        {
            unsigned char c = (n >> 56) & 0xff;
            n <<= 8;

            if (fLeadingZeroes)
            {
                if (c == 0)
                    continue;

                if (c & 0x80)
                    *p++ = 0;

                fLeadingZeroes = false;
            }

            *p++ = c;
        }

        unsigned int nSize = p - (pch + 4);

        pch[0] = (nSize >> 24) & 0xff;
        pch[1] = (nSize >> 16) & 0xff;
        pch[2] = (nSize >> 8) & 0xff;
        pch[3] = (nSize) & 0xff;
        BN_mpi2bn(pch, p - pch, getBN());
    }

    void setuint256(uint256 n)
    {
        unsigned char pch[sizeof(n) + 6];
        unsigned char* p = pch + 4;
        bool fLeadingZeroes = true;
        unsigned char* pbegin = (unsigned char*) &n;
        unsigned char* psrc = pbegin + sizeof(n);

        while (psrc != pbegin)
        {
            unsigned char c = *(--psrc);

            if (fLeadingZeroes)
            {
                if (c == 0)
                    continue;

                if (c & 0x80)
                    *p++ = 0;

                fLeadingZeroes = false;
            }

            *p++ = c;
        }

        unsigned int nSize = p - (pch + 4);

        pch[0] = (nSize >> 24) & 0xff;
        pch[1] = (nSize >> 16) & 0xff;
        pch[2] = (nSize >> 8) & 0xff;
        pch[3] = (nSize >> 0) & 0xff;
        BN_mpi2bn(pch, p - pch, getBN());
    }

    uint256 getuint256() const
    {
        unsigned int nSize = BN_bn2mpi(getBNConst(), NULL);

        if (nSize < 4)
            return 0;

        std::vector<unsigned char> vch(nSize);
        BN_bn2mpi(getBNConst(), &vch[0]);

        if (vch.size() > 4)
            vch[4] &= 0x7f;

        uint256 n = 0;

        for (unsigned int i = 0, j = vch.size()-1; i < sizeof(n) && j >= 4; i++, j--) {
            ((unsigned char*) &n) [i] = vch[j];
        }

        return n;
    }

    void setvch(const std::vector<unsigned char>& vch)
    {
        std::vector<unsigned char> vch2(vch.size() + 4);
        unsigned int nSize = vch.size();

        vch2[0] = (nSize >> 24) & 0xff;
        vch2[1] = (nSize >> 16) & 0xff;
        vch2[2] = (nSize >> 8) & 0xff;
        vch2[3] = (nSize >> 0) & 0xff;

        // Swap data to big endian
        reverse_copy(vch.begin(), vch.end(), vch2.begin() + 4);
        BN_mpi2bn(&vch2[0], vch2.size(), getBN());
    }

    std::vector<unsigned char> getvch() const
    {
        unsigned int nSize = BN_bn2mpi(getBNConst(), NULL);

        if (nSize <= 4)
            return std::vector<unsigned char>();

        std::vector<unsigned char> vch(nSize);
        BN_bn2mpi(getBNConst(), &vch[0]);

        vch.erase(vch.begin(), vch.begin() + 4);
        reverse(vch.begin(), vch.end());

        return vch;
    }

    CBigNum& SetCompact(unsigned int nCompact)
    {
        unsigned int nSize = nCompact >> 24;
        std::vector<unsigned char> vch(4 + nSize);
        vch[3] = nSize;

        if (nSize >= 1)
            vch[4] = (nCompact >> 16) & 0xff;
        if (nSize >= 2)
            vch[5] = (nCompact >> 8) & 0xff;
        if (nSize >= 3)
            vch[6] = (nCompact >> 0) & 0xff;

        BN_mpi2bn(&vch[0], vch.size(), getBN());
        return *this;
    }

    unsigned int GetCompact() const
    {
        unsigned int nSize = BN_bn2mpi(getBNConst(), NULL);
        std::vector<unsigned char> vch(nSize);

        nSize -= 4;
        BN_bn2mpi(getBNConst(), &vch[0]);
        unsigned int nCompact = nSize << 24;

        if (nSize >= 1)
            nCompact |= (vch[4] << 16);
        if (nSize >= 2)
            nCompact |= (vch[5] << 8);
        if (nSize >= 3)
            nCompact |= (vch[6] << 0);

        return nCompact;
    }

    void SetHex(const std::string& str)
    {
        const char* psz = str.c_str();
        bool fNegative = false;

        while (isspace(*psz)) {
            psz++;
        }

        if (*psz == '-')
        {
            fNegative = true;
            psz++;
        }

        if (psz[0] == '0' && tolower(psz[1]) == 'x')
            psz += 2;

        while (isspace(*psz))
            psz++;

        // Hex string to bignum
        static const signed char phexdigit[256] =
        {
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,             0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,             0,1,2,3,4,5,6,7,8,9,0,0,0,0,0,0,
            0,0xa,0xb,0xc,0xd,0xe,0xf,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            0,0xa,0xb,0xc,0xd,0xe,0xf,0,0,0,0,0,0,0,0,0
        };

        *this = 0;

        while (isxdigit(*psz))
        {
            *this <<= 4;
            int n = phexdigit[(unsigned char) *psz++];
            *this += n;
        }

        if (fNegative)
            *this = 0 - *this;
    }

    std::string ToString(int nBase = 10) const
    {
        CAutoBN_CTX pctx;
        CBigNum bnBase = nBase;
        CBigNum bn0 = 0;
        std::string str;
        CBigNum bn = *this;
        BN_set_negative(bn.getBN(), false);
        CBigNum dv;
        CBigNum rem;

        if (BN_cmp(bn.getBN(), bn0.getBNConst()) == 0)
            return "0";

        while (BN_cmp(bn.getBN(), bn0.getBNConst()) > 0)
        {
            if (!BN_div(dv.getBN(), rem.getBN(), bn.getBNConst(), bnBase.getBNConst(), pctx))
                throw bignum_error("CBigNum::ToString() : BN_div failed");

            bn = dv;
            unsigned int c = rem.getulong();
            str += "0123456789abcdef"[c];
        }

        if (BN_is_negative(getBNConst()))
            str += "-";

        reverse(str.begin(), str.end());
        return str;
    }

    std::string GetHex() const
    {
        return ToString(16);
    }

    unsigned int GetSerializeSize(int nType=0, int nVersion=PROTOCOL_VERSION) const
    {
        return ::GetSerializeSize(getvch(), nType, nVersion);
    }

    template<typename Stream>
    void Serialize(Stream& s, int nType=0, int nVersion=PROTOCOL_VERSION) const
    {
        ::Serialize(s, getvch(), nType, nVersion);
    }

    template<typename Stream>
    void Unserialize(Stream& s, int nType=0, int nVersion=PROTOCOL_VERSION)
    {
        std::vector<unsigned char> vch;
        ::Unserialize(s, vch, nType, nVersion);
        setvch(vch);
    }

    CBigNum pow(const int e) const
    {
        return this->pow(CBigNum(e));
    }

    CBigNum pow(const CBigNum& e) const
    {
        CAutoBN_CTX pctx;
        CBigNum ret;

        if (!BN_exp(ret.getBN(), getBNConst(), e.getBNConst(), pctx)) {
            throw bignum_error("CBigNum::pow : BN_exp failed");
        }

        return ret;
    }

     // Modular multiplication: (BIGNUM_REF * b) mod m
    CBigNum mul_mod(const CBigNum& b, const CBigNum& m) const
    {
        CAutoBN_CTX pctx;
        CBigNum ret;

        if (!BN_mod_mul(ret.getBN(), getBNConst(), b.getBNConst(), m.getBNConst(), pctx)) {
            throw bignum_error("CBigNum::mul_mod : BN_mod_mul failed");
        }

        return ret;
    }

    // Modular exponentiation: BIGNUM_REF^e mod n
    CBigNum pow_mod(const CBigNum& e, const CBigNum& m) const
    {
        CAutoBN_CTX pctx;
        CBigNum ret;

        if( e < 0) {
            // g^-x = (g^-1)^x
            CBigNum inv = this->inverse(m);
            CBigNum posE = e * -1;

            if (!BN_mod_exp(ret.getBN(), inv.getBNConst(), posE.getBNConst(), m.getBNConst(), pctx)) {
                throw bignum_error("CBigNum::pow_mod: BN_mod_exp failed on negative exponent");
            }
        } else if (!BN_mod_exp(ret.getBN(), getBNConst(), e.getBNConst(), m.getBNConst(), pctx)) {
            throw bignum_error("CBigNum::pow_mod : BN_mod_exp failed");
        }

        return ret;
    }

    CBigNum inverse(const CBigNum& m) const
    {
        CAutoBN_CTX pctx;
        CBigNum ret;

        if (!BN_mod_inverse(ret.getBN(), getBNConst(), m.getBNConst(), pctx)) {
            throw bignum_error("CBigNum::inverse*= :BN_mod_inverse");
        }

        return ret;
    }

    // Generates a random (safe) prime of numBits bits
    static CBigNum generatePrime(const unsigned int numBits, bool safe = false)
    {
        CBigNum ret;

        if(!BN_generate_prime_ex(ret.getBN(), numBits, (safe == true), NULL, NULL, NULL))
            throw bignum_error("CBigNum::generatePrime*= :BN_generate_prime_ex");

        return ret;
    }

     // Calculates the greatest common divisor (GCD) of two numbers.
    CBigNum gcd( const CBigNum& b) const
    {
        CAutoBN_CTX pctx;
        CBigNum ret;

        if (!BN_gcd(ret.getBN(), getBNConst(), b.getBNConst(), pctx))
            throw bignum_error("CBigNum::gcd*= :BN_gcd");

        return ret;
    }

    // Miller-Rabin primality test on BIGNUM_REF element
    bool isPrime(const int checks = BN_prime_checks) const
    {
        CAutoBN_CTX pctx;

        #if OPENSSL_VERSION_NUMBER < 0x10100000L
        int ret = BN_is_prime(getBNConst(), checks, NULL, pctx, NULL);
        #else
        #if OPENSSL_VERSION_NUMBER > 0x20000000L
        int ret = BN_check_prime(getBNConst(), pctx, NULL);
        #else
        int ret = BN_is_prime_ex(getBNConst(), checks, pctx, NULL);
        #endif
        #endif

        if(ret < 0)
            throw bignum_error("CBigNum::isPrime :BN_is_prime");

        return ret;
    }

    bool isOne() const
    {
        return BN_is_one(getBNConst());
    }

    bool operator!() const
    {
        return BN_is_zero(getBNConst());
    }

    CBigNum& operator+=(const CBigNum& b)
    {
        if (!BN_add(getBN(), getBNConst(), b.getBNConst()))
            throw bignum_error("CBigNum::operator+= : BN_add failed");

        return *this;
    }

    CBigNum& operator-=(const CBigNum& b)
    {
        if (!BN_sub(getBN(), getBNConst(), b.getBNConst()))
            throw bignum_error("CBigNum::operator+= : BN_add failed");

        return *this;
    }

    CBigNum& operator*=(const CBigNum& b)
    {
        CAutoBN_CTX pctx;

        if (!BN_mul(getBN(), getBNConst(), b.getBNConst(), pctx))
            throw bignum_error("CBigNum::operator*= : BN_mul failed");

        return *this;
    }

    CBigNum& operator/=(const CBigNum& b)
    {
        *this = *this / b;
        return *this;
    }

    CBigNum& operator%=(const CBigNum& b)
    {
        *this = *this % b;
        return *this;
    }

    CBigNum& operator<<=(unsigned int shift)
    {
        if (!BN_lshift(getBN(), getBNConst(), shift))
            throw bignum_error("CBigNum:operator<<= : BN_lshift failed");

        return *this;
    }

    CBigNum& operator>>=(unsigned int shift)
    {
        // NOTE: BN_rshift segfaults on 64-bit if 2^shift is greater than the number
        // if built on Ubuntu 9.04 or 9.10, probably depends on version of OpenSSL
        CBigNum a = 1;
        a <<= shift;

        if (BN_cmp(a.getBNConst(), getBNConst()) > 0)
        {
            *this = 0;
            return *this;
        }

        if (!BN_rshift(getBN(), getBNConst(), shift))
            throw bignum_error("CBigNum:operator>>= : BN_rshift failed");

        return *this;
    }


    CBigNum& operator++()
    {
        if (!BN_add(getBN(), getBNConst(), BN_value_one()))
            throw bignum_error("CBigNum::operator++ : BN_add failed");

        return *this;
    }

    const CBigNum operator++(int)
    {
        const CBigNum ret = *this;
        ++(*this);
        return ret;
    }

    CBigNum& operator--()
    {
        CBigNum r;

        if (!BN_sub(r.getBN(), getBNConst(), BN_value_one()))
            throw bignum_error("CBigNum::operator-- : BN_sub failed");

        *this = r;
        return *this;
    }

    const CBigNum operator--(int)
    {
        const CBigNum ret = *this;
        --(*this);
        return ret;
    }

    friend inline const CBigNum operator-(const CBigNum& a, const CBigNum& b);
    friend inline const CBigNum operator/(const CBigNum& a, const CBigNum& b);
    friend inline const CBigNum operator%(const CBigNum& a, const CBigNum& b);
    friend inline const CBigNum operator*(const CBigNum& a, const CBigNum& b);
    friend inline bool operator<(const CBigNum& a, const CBigNum& b);
};

inline const CBigNum operator+(const CBigNum& a, const CBigNum& b)
{
    CBigNum r;

    if (!BN_add(r.getBN(), a.getBNConst(), b.getBNConst()))
        throw bignum_error("CBigNum::operator+ : BN_add failed");

    return r;
}

inline const CBigNum operator-(const CBigNum& a, const CBigNum& b)
{
    CBigNum r;

    if (!BN_sub(r.getBN(), a.getBNConst(), b.getBNConst()))
        throw bignum_error("CBigNum::operator- : BN_sub failed");

    return r;
}

inline const CBigNum operator-(const CBigNum& a)
{
    CBigNum r(a);
    BN_set_negative(r.getBN(), !BN_is_negative(r.getBNConst()));
    return r;
}

inline const CBigNum operator*(const CBigNum& a, const CBigNum& b)
{
    CAutoBN_CTX pctx;
    CBigNum r;

    if (!BN_mul(r.getBN(), a.getBNConst(), b.getBNConst(), pctx))
        throw bignum_error("CBigNum::operator* : BN_mul failed");

    return r;
}

inline const CBigNum operator/(const CBigNum& a, const CBigNum& b)
{
    CAutoBN_CTX pctx;
    CBigNum r;

    if (!BN_div(r.getBN(), NULL, a.getBNConst(), b.getBNConst(), pctx))
        throw bignum_error("CBigNum::operator/ : BN_div failed");

    return r;
}

inline const CBigNum operator%(const CBigNum& a, const CBigNum& b)
{
    CAutoBN_CTX pctx;
    CBigNum r;

    if (!BN_nnmod(r.getBN(), a.getBNConst(), b.getBNConst(), pctx))
        throw bignum_error("CBigNum::operator% : BN_div failed");

    return r;
}

inline const CBigNum operator<<(const CBigNum& a, unsigned int shift)
{
    CBigNum r;

    if (!BN_lshift(r.getBN(), a.getBNConst(), shift))
        throw bignum_error("CBigNum:operator<< : BN_lshift failed");

    return r;
}

inline const CBigNum operator>>(const CBigNum& a, unsigned int shift)
{
    CBigNum r = a;
    r >>= shift;
    return r;
}

inline bool operator==(const CBigNum& a, const CBigNum& b) { return (BN_cmp(a.getBNConst(), b.getBNConst()) == 0); }
inline bool operator!=(const CBigNum& a, const CBigNum& b) { return (BN_cmp(a.getBNConst(), b.getBNConst()) != 0); }
inline bool operator<=(const CBigNum& a, const CBigNum& b) { return (BN_cmp(a.getBNConst(), b.getBNConst()) <= 0); }
inline bool operator>=(const CBigNum& a, const CBigNum& b) { return (BN_cmp(a.getBNConst(), b.getBNConst()) >= 0); }
inline bool operator<(const CBigNum& a, const CBigNum& b)  { return (BN_cmp(a.getBNConst(), b.getBNConst()) < 0); }
inline bool operator>(const CBigNum& a, const CBigNum& b)  { return (BN_cmp(a.getBNConst(), b.getBNConst()) > 0); }
inline std::ostream& operator<<(std::ostream &strm, const CBigNum &b) { return strm << b.ToString(10); }

typedef  CBigNum Bignum;

#endif
