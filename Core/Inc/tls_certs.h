/**
 * @file    tls_certs.h
 * @brief   Embedded CA certificate for TLS server verification.
 *
 * Embedded CA certificate used to verify the remote TLS relay.
 */

#ifndef TLS_CERTS_H
#define TLS_CERTS_H

/** CA certificate in PEM format, including its null terminator. */
static const unsigned char tls_ca_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIID7zCCAtegAwIBAgIUOVQzCh/87UBb6OlbNKX3oqL0ItIwDQYJKoZIhvcNAQEL\n"
    "BQAwgYYxCzAJBgNVBAYTAlRSMQ8wDQYDVQQIDAZBbmthcmExDzANBgNVBAcMBkFu\n"
    "a2FyYTENMAsGA1UECgwER2F6aTEMMAoGA1UECwwDRUVNMQ0wCwYDVQQDDARKVEFH\n"
    "MSkwJwYJKoZIhvcNAQkBFhpzYWhpbi5kdXJhbi45Mjc1QGdtYWlsLmNvbTAeFw0y\n"
    "NjA2MTcxNjQ5NDlaFw0yNzA2MTcxNjQ5NDlaMIGGMQswCQYDVQQGEwJUUjEPMA0G\n"
    "A1UECAwGQW5rYXJhMQ8wDQYDVQQHDAZBbmthcmExDTALBgNVBAoMBEdhemkxDDAK\n"
    "BgNVBAsMA0VFTTENMAsGA1UEAwwESlRBRzEpMCcGCSqGSIb3DQEJARYac2FoaW4u\n"
    "ZHVyYW4uOTI3NUBnbWFpbC5jb20wggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEK\n"
    "AoIBAQCttetIzS8Z9raatUSiaGGiMJTq0jnqjviSWPR1v8w2tUuXX7Kj9YX2s3Vo\n"
    "2qPmpN1AyheX7F9aHSopPGoZitbL+b2+KQCue9azWgybufQhx58VvM1bPeMfzW7+\n"
    "kAHMWjjEnoMBNBPrlCKM1WC+A49cB7czgm/PDBnBG4cocEo1naPGjDa6MKwJiBwV\n"
    "5k7kRJ2eLY59C+QCLujRmmOhSTl6PdRYa1xSQYE6xgwpOUTGue1+WYrJLEGE7jht\n"
    "QmcehwSAfsKE/vAL0BYW4Yz559RLVatB8lR9Iam0RDDfaVuIL4sN/lBhsjKEr8+O\n"
    "OKzHel/EZgy9CHkSDuXWXvej8ujXAgMBAAGjUzBRMB0GA1UdDgQWBBT7BpyCa3fZ\n"
    "U3QgsxEQE1zEUKDufzAfBgNVHSMEGDAWgBT7BpyCa3fZU3QgsxEQE1zEUKDufzAP\n"
    "BgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQBphZMTmnj0+efylHY/\n"
    "XoEk7asTyXcCzeYHkXIC347Y+UFnsObZfK4fYUZSzSddkD5SKDJN0j8fgfPe54+f\n"
    "Qu2+gVM+LAuGFYCSL4XwxSV68/ul1jGhLbYEFKIZoAOPum22EFq9VxE6LfSGySbv\n"
    "MQo/ydKNPkeJ9iGCaSR1neljXJkotlkpW+YvKfHVPlg20c5rUmAUn/QPdkRHr5LQ\n"
    "CgGk6SHgJOuvD7/0knEGN1rekdH6QryAFrICl1N3LQBN9EMpJyPG/unWGJPovXbb\n"
    "09Ps8BNgwJzBPyVpPM14mbc95YE9Uvjyl6sSnQQeR9h8dr9O+65XoCArsNeTpNdS\n"
    "ORhf\n"
    "-----END CERTIFICATE-----\n";

static const unsigned int tls_ca_cert_pem_len = sizeof(tls_ca_cert_pem);

#endif /* TLS_CERTS_H */
