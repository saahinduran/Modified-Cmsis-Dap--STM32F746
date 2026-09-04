/**
 * @file    mbedtls_rng.c
 * @brief   Hardware RNG entropy source for mbedTLS on STM32F746.
 *
 * Implements mbedtls_hardware_poll() using the STM32's built-in
 * True Random Number Generator (RNG) peripheral.
 *
 * This file is only compiled when DAP_SERVER_MODE == DAP_SERVER_MODE_TLS.
 */

#include "dap_server_config.h"
#if (DAP_SERVER_MODE == DAP_SERVER_MODE_TLS)

#include "main.h"            /* HAL includes, RCC, RNG register defs */
#include <string.h>
#include <stdint.h>

/* mbedTLS entropy callback prototype */
#include "mbedtls/entropy.h"

/**
 * Enable the RNG peripheral clock (idempotent).
 */
static void rng_hw_init(void)
{
    static uint8_t initialised = 0;
    if (initialised) return;

    __HAL_RCC_RNG_CLK_ENABLE();
    RNG->CR |= RNG_CR_RNGEN;       /* Enable the RNG */
    initialised = 1;
}

/**
 * mbedtls_hardware_poll - entropy source callback for mbedTLS.
 *
 * @param data      Opaque context (unused).
 * @param output    Buffer to fill with random bytes.
 * @param len       Requested number of bytes.
 * @param olen      [out] Number of bytes actually written.
 * @return          0 on success, -1 on timeout / error.
 */
int mbedtls_hardware_poll(void *data,
                          unsigned char *output,
                          size_t len,
                          size_t *olen)
{
    (void)data;
    rng_hw_init();

    size_t n = 0;
    while (n < len) {
        /* Wait for a new random number to be ready (DRDY flag) */
        uint32_t timeout = 0xFFFF;
        while (!(RNG->SR & RNG_SR_DRDY)) {
            if (--timeout == 0) {
                *olen = n;
                return -1;   /* Timed out */
            }
        }

        /* Check for errors */
        if (RNG->SR & (RNG_SR_CECS | RNG_SR_SECS)) {
            /* Clear error flags and retry */
            RNG->SR &= ~(RNG_SR_CECS | RNG_SR_SECS);
            continue;
        }

        uint32_t rnd = RNG->DR;
        size_t copy_len = (len - n >= 4U) ? 4U : (len - n);
        memcpy(output + n, &rnd, copy_len);
        n += copy_len;
    }

    *olen = n;
    return 0;
}

#endif /* DAP_SERVER_MODE_TLS */
