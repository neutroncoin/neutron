// Copyright (c) 2017-2019 The Swipp developers
// Copyright (c) 2021 The Neutron developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING.daemon or http://www.opensource.org/licenses/mit-license.php.

#include <openssl/ecdsa.h>
#include <openssl/opensslv.h>

#if OPENSSL_VERSION_NUMBER < 0x10100000L
BIGNUM *ECDSA_SIG_getr(const ECDSA_SIG *sig)
{
    return sig->r;
}

BIGNUM *ECDSA_SIG_gets(const ECDSA_SIG *sig)
{
    return sig->s;
}
#else
BIGNUM *ECDSA_SIG_getr(const ECDSA_SIG *sig)
{
    const BIGNUM *r;
    ECDSA_SIG_get0(sig, &r, NULL);
    return (BIGNUM *) r;
}

BIGNUM *ECDSA_SIG_gets(const ECDSA_SIG *sig)
{
    const BIGNUM *s;
    ECDSA_SIG_get0(sig, NULL, &s);
    return (BIGNUM *) s;
}
#endif

