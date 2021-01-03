// Copyright (c) 2017-2019 The Swipp developers
// Copyright (c) 2021 The Neutron developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING.daemon or http://www.opensource.org/licenses/mit-license.php.

#ifndef __SWIPP_OPENSSLCOMPAT_H__
#define __SWIPP_OPENSSLCOMPAT_H__

#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/opensslv.h>

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#include <openssl/ecdh.h>

#define DEF_BIGNUM BIGNUM
#define DEF_BN_init(a) BN_init(&a)
#define DEF_HMAC_CTX HMAC_CTX
#define DEF_HMAC_CTX_init(a) HMAC_CTX_init(&a)
#define DEF_HMAC_CTX_cleanup(a) HMAC_CTX_cleanup(&a)
#define DEF_EVP_CIPHER_CTX  EVP_CIPHER_CTX
#define DEF_EVP_CIPHER_CTX_init(a) EVP_CIPHER_CTX_init(&a)
#define DEF_ECDHKEY_set_method(a) ECDH_set_method(a, ECDH_OpenSSL())
#define SSL_ADDR(a) &a
#else
#include <openssl/ec.h>

#define DEF_BIGNUM BIGNUM *
#define DEF_BN_init(a) a = BN_new()
#define DEF_HMAC_CTX HMAC_CTX *
#define DEF_HMAC_CTX_init(a) a = HMAC_CTX_new()
#define DEF_HMAC_CTX_cleanup(a) HMAC_CTX_free(a)
#define DEF_EVP_CIPHER_CTX  EVP_CIPHER_CTX *
#define DEF_EVP_CIPHER_CTX_init(a) a = EVP_CIPHER_CTX_new()
#define DEF_ECDHKEY_set_method(a) EC_KEY_set_method(a, EC_KEY_OpenSSL());
#define SSL_ADDR(a) a
#endif

extern "C" BIGNUM *ECDSA_SIG_getr(const ECDSA_SIG *sig);
extern "C" BIGNUM *ECDSA_SIG_gets(const ECDSA_SIG *sig);

#endif

