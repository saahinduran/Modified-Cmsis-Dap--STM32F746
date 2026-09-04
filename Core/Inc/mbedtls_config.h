/**
 * @file    mbedtls_config.h
 * @brief   Minimal mbedTLS configuration for STM32F746 TLS client.
 *
 * This configuration enables only what is needed for a TLS 1.2 client
 * connection with server certificate verification.  It is optimised for
 * the STM32F746's 320 KB RAM / 1 MB Flash constraints.
 *
 * Cipher suite: TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256
 *          and: TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256
 *
 * Define MBEDTLS_CONFIG_FILE="mbedtls_config.h" in your compiler
 * preprocessor settings (or pass -DMBEDTLS_CONFIG_FILE=...).
 */

#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#include "dap_server_config.h"

#if (DAP_SERVER_MODE == DAP_SERVER_MODE_TLS)

/* ---------- System / platform ------------------------------------------- */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_NO_PLATFORM_ENTROPY          /* We supply HW entropy below   */
#define MBEDTLS_ENTROPY_HARDWARE_ALT         /* Use mbedtls_hardware_poll()  */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY              /* Use mbedtls_platform_set_calloc_free() or default calloc/free */

/* ---------- TLS protocol ------------------------------------------------ */
#define MBEDTLS_SSL_CLI_C                    /* TLS client mode only         */
#define MBEDTLS_SSL_TLS_C                    /* Core TLS engine              */
#define MBEDTLS_SSL_PROTO_TLS1_2             /* TLS 1.2 only                 */

/* ---------- Cipher / AEAD ----------------------------------------------- */
#define MBEDTLS_AES_C                        /* AES block cipher             */
#define MBEDTLS_GCM_C                        /* AES-GCM AEAD mode            */
#define MBEDTLS_CIPHER_C                     /* Cipher abstraction layer     */
#define MBEDTLS_CIPHER_MODE_CBC              /* Needed by some internal paths*/

/* ---------- Hashing ----------------------------------------------------- */
#define MBEDTLS_SHA256_C                     /* SHA-256                      */
#define MBEDTLS_MD_C                         /* Message digest abstraction   */

/* ---------- Public key / key exchange ----------------------------------- */
#define MBEDTLS_RSA_C                        /* RSA (for server certs)       */
#define MBEDTLS_ECP_C                        /* Elliptic Curve primitives    */
#define MBEDTLS_ECDH_C                       /* ECDHE key exchange           */
#define MBEDTLS_ECDSA_C                      /* ECDSA (for server certs)     */
#define MBEDTLS_BIGNUM_C                     /* Big number math              */
#define MBEDTLS_OID_C                        /* OID database                 */
#define MBEDTLS_PKCS1_V15                    /* RSA PKCS#1 v1.5 signatures   */
#define MBEDTLS_PK_C                         /* Public key abstraction       */
#define MBEDTLS_PK_PARSE_C                   /* Parse public keys            */

/* Supported elliptic curves (minimal set for common server certs) */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED     /* NIST P-256                   */
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED     /* NIST P-384                   */

/* Key exchange methods */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/* ---------- X.509 certificate ------------------------------------------- */
#define MBEDTLS_X509_CRT_PARSE_C            /* Parse X.509 certificates     */
#define MBEDTLS_X509_USE_C                   /* X.509 core                   */
#define MBEDTLS_ASN1_PARSE_C                 /* ASN.1 parsing                */
#define MBEDTLS_ASN1_WRITE_C                 /* ASN.1 writing (needed by TLS)*/
#define MBEDTLS_BASE64_C                     /* Base64 for PEM decoding      */
#define MBEDTLS_PEM_PARSE_C                  /* PEM certificate parsing      */

/* ---------- RNG / entropy ----------------------------------------------- */
#define MBEDTLS_CTR_DRBG_C                   /* CTR-DRBG PRNG               */
#define MBEDTLS_ENTROPY_C                    /* Entropy collector           */

/* ---------- TLS buffer sizes (tuned for constrained RAM) ---------------- */
#define MBEDTLS_SSL_MAX_CONTENT_LEN   4096   /* Max TLS record payload      */
#define MBEDTLS_SSL_IN_CONTENT_LEN    4096   /* Inbound TLS record buffer   */
#define MBEDTLS_SSL_OUT_CONTENT_LEN   4096   /* Outbound TLS record buffer  */

/* ---------- Miscellaneous optimisations --------------------------------- */
#define MBEDTLS_AES_ROM_TABLES               /* Put AES tables in Flash     */
#define MBEDTLS_ECP_NIST_OPTIM               /* Optimised NIST curve math   */
#define MBEDTLS_SHA256_SMALLER               /* Smaller SHA-256 (slower)    */

/* ---------- Not needed (keep disabled) ---------------------------------- */
/* #define MBEDTLS_SSL_SRV_C */              /* No TLS server               */
/* #define MBEDTLS_SSL_PROTO_DTLS */         /* No DTLS                     */
/* #define MBEDTLS_SSL_PROTO_TLS1 */         /* No TLS 1.0                  */
/* #define MBEDTLS_SSL_PROTO_TLS1_1 */       /* No TLS 1.1                  */
/* #define MBEDTLS_KEY_EXCHANGE_PSK_ENABLED */
/* #define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED */
/* #define MBEDTLS_DHM_C */                  /* No classic DHE              */
/* #define MBEDTLS_DES_C */                  /* No DES/3DES                 */
/* #define MBEDTLS_SHA512_C */               /* No SHA-384/512              */

/* Validate this configuration */
#include "mbedtls/check_config.h"

#endif /* DAP_SERVER_MODE == DAP_SERVER_MODE_TLS */

#endif /* MBEDTLS_CONFIG_H */
