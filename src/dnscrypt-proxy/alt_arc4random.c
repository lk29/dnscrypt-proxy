/*
 * Copyright (c) 1996, David Mazieres <dm@uun.org>
 * Copyright (c) 2008, Damien Miller <djm@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Arc4 random number generator for OpenBSD.
 *
 * This code is derived from section 17.1 of Applied Cryptography,
 * second edition, which describes a stream cipher allegedly
 * compatible with RSA Labs "RC4" cipher (the actual description of
 * which is a trade secret).  The same algorithm is used as a stream
 * cipher called "arcfour" in Tatu Ylonen's ssh package.
 *
 * RC4 is a registered trademark of RSA Laboratories.
 */

/*
 * OpenBSD can fetch random data through a sysctl call, but other operating
 * systems require reading a device.
 * This modified version of the arc4random*() functions keeps an open file
 * descriptor, so that we can still reseed the PRNG after a chroot() call.
 */

#ifndef __OpenBSD__

#include <config.h>
#include <sys/types.h>

#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "alt_arc4random.h"
#include "safe_rw.h"

struct alt_arc4_stream {
    uint8_t i;
    uint8_t j;
    uint8_t s[256];
};

static int rs_initialized;
static struct alt_arc4_stream rs;
static pid_t alt_arc4_stir_pid;
static int alt_arc4_count;
static int random_data_source_fd = -1;
static uint8_t alt_arc4_getbyte(void);

/* Warning: no thread safety. But we don't need any */
#define _alt_arc4_LOCK()   do { } while(0)
#define _alt_arc4_UNLOCK() do { } while(0)

static int
alt_arc4_random_dev_open(void)
{
    static const char * const devices[] = {
        "/dev/arandom", "/dev/urandom", "/dev/random", NULL
    };
    const char * const *device = devices;

    do {
        if (access(*device, F_OK | R_OK) == 0) {
            return open(*device, O_RDONLY);
        }
        device++;
    } while (*device != NULL);

    return -1;
}

static void
alt_arc4_init(void)
{
    int     n;

    for (n = 0; n < 256; n++) {
        rs.s[n] = n;
    }
    rs.i = 0;
    rs.j = 0;

    if (random_data_source_fd != -1) {
        return;
    }
    if ((random_data_source_fd = alt_arc4_random_dev_open()) == -1) {
        abort();
    }
}

static void
alt_arc4_addrandom(unsigned char *dat, int datlen)
{
    int     n;
    uint8_t si;

    rs.i--;
    for (n = 0; n < 256; n++) {
        rs.i = (rs.i + 1);
        si = rs.s[rs.i];
        rs.j = (rs.j + si + dat[n % datlen]);
        rs.s[rs.i] = rs.s[rs.j];
        rs.s[rs.j] = si;
    }
    rs.j = rs.i;
}

static void
alt_arc4_stir(void)
{
    int     i;
    unsigned char rnd[128];

    if (!rs_initialized) {
        alt_arc4_init();
        rs_initialized = 1;
    }

    safe_read(random_data_source_fd, rnd, sizeof(rnd));

    alt_arc4_addrandom(rnd, sizeof(rnd));

    /*
     * Discard early keystream, as per recommendations in:
     * http://www.wisdom.weizmann.ac.il/~itsik/RC4/Papers/Rc4_ksa.ps
     */
    for (i = 0; i < 256; i++) {
        (void)alt_arc4_getbyte();
    }
    alt_arc4_count = 1600000;
}

static void
alt_arc4_stir_if_needed(void)
{
    pid_t pid = getpid();

    if (alt_arc4_count <= 0 || !rs_initialized || alt_arc4_stir_pid != pid) {
        alt_arc4_stir_pid = pid;
        alt_arc4_stir();
    }
}

static uint8_t
alt_arc4_getbyte(void)
{
    uint8_t si, sj;

    rs.i = (rs.i + 1);
    si = rs.s[rs.i];
    rs.j = (rs.j + si);
    sj = rs.s[rs.j];
    rs.s[rs.i] = sj;
    rs.s[rs.j] = si;

    return (rs.s[(si + sj) & 0xff]);
}

static uint32_t
alt_arc4_getword(void)
{
    uint32_t val;
    val =  ((uint32_t) alt_arc4_getbyte()) << 24;
    val |= ((uint32_t) alt_arc4_getbyte()) << 16;
    val |= ((uint32_t) alt_arc4_getbyte()) << 8;
    val |= ((uint32_t) alt_arc4_getbyte());

    return val;
}

void
alt_arc4random_stir(void)
{
    _alt_arc4_LOCK();
    alt_arc4_stir();
    _alt_arc4_UNLOCK();
}

int
alt_arc4random_close(void)
{
    int ret = -1;

    _alt_arc4_LOCK();
    if (random_data_source_fd != -1 && close(random_data_source_fd) == 0) {
        random_data_source_fd = -1;
        ret = 0;
    }
    _alt_arc4_UNLOCK();

    return ret;
}

void
alt_arc4random_addrandom(unsigned char *dat, int datlen)
{
    _alt_arc4_LOCK();
    if (!rs_initialized) {
        alt_arc4_stir();
    }
    alt_arc4_addrandom(dat, datlen);
    _alt_arc4_UNLOCK();
}

uint32_t
alt_arc4random(void)
{
    uint32_t val;
    _alt_arc4_LOCK();
    alt_arc4_count -= 4;
    alt_arc4_stir_if_needed();
    val = alt_arc4_getword();
    _alt_arc4_UNLOCK();

    return val;
}

void
alt_arc4random_buf(void *_buf, size_t n)
{
    unsigned char *buf = (unsigned char *)_buf;
    _alt_arc4_LOCK();
    alt_arc4_stir_if_needed();
    while (n--) {
        if (--alt_arc4_count <= 0) {
            alt_arc4_stir();
        }
        buf[n] = alt_arc4_getbyte();
    }
    _alt_arc4_UNLOCK();
}

/*
 * Calculate a uniformly distributed random number less than upper_bound
 * avoiding "modulo bias".
 *
 * Uniformity is achieved by generating new random numbers until the one
 * returned is outside the range [0, 2**32 % upper_bound).  This
 * guarantees the selected random number will be inside
 * [2**32 % upper_bound, 2**32) which maps back to [0, upper_bound)
 * after reduction modulo upper_bound.
 */
uint32_t
alt_arc4random_uniform(uint32_t upper_bound)
{
    uint32_t r, min;

    if (upper_bound < 2) {
        return 0;
    }

#if (ULONG_MAX > 0xffffffffUL)
    min = (uint32_t) (0x100000000UL % upper_bound);
#else
    /* Calculate (2**32 % upper_bound) avoiding 64-bit math */
    if (upper_bound > 0x80000000)
        min = 1 + ~upper_bound;         /* 2**32 - upper_bound */
    else {
        /* (2**32 - (x * 2)) % x == 2**32 % x when x <= 2**31 */
        min = ((0xffffffff - (upper_bound * 2)) + 1) % upper_bound;
    }
#endif

    /*
     * This could theoretically loop forever but each retry has
     * p > 0.5 (worst case, usually far better) of selecting a
     * number inside the range we need, so it should rarely need
     * to re-roll.
     */
    for (;;) {
        r = alt_arc4random();
        if (r >= min) {
            break;
        }
    }

    return r % upper_bound;
}

#else

int
alt_arc4random_close(void)
{
    return 0;
}

#endif
