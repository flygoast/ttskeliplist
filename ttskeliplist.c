/*
 * Copyright (c) 2013, FengGu <flygoast@126.com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <tcadb.h>
#include <assert.h>
#include <pthread.h>
#include <arpa/inet.h>


#define IPLIST_INDEX_COUNT      65536
#define IPLIST_SIZE_PERPTR      sizeof(uintptr_t)
#define IPLIST_COUNT_PERPTR     (8 * IPLIST_SIZE_PERPTR)
#define IPLIST_COUNT_PERVOL     (65536 / IPLIST_COUNT_PERPTR)
#define IPLIST_HIGH_HALF(a)     (((a) >> 16) & 0xffff)
#define IPLIST_LOW_HALF(b)      ((b) & 0xffff)
#define IPLIST_INDEX_PTR(a)     ((a) / IPLIST_COUNT_PERPTR)
#define IPLIST_INDEX_BIT(a)     ((uintptr_t) 1 << ((a) % IPLIST_COUNT_PERPTR))
#define IPLIST_ALIGN_UP(s)      \
    (((s) + (sizeof(uintptr_t) * 8 - 1)) & ~(8 * sizeof(uintptr_t) - 1))
#define IPLIST_ALIGN_DOWN(s)    ((s) & (8 * sizeof(uintptr_t)))


typedef struct {
    uintptr_t         *index[IPLIST_INDEX_COUNT];
    int                count[IPLIST_INDEX_COUNT];
    pthread_rwlock_t   lock[IPLIST_INDEX_COUNT];
} iplist_t;


static bool iplist_parse_addr(char *p, int sz, in_addr_t *s, in_addr_t *e) {
    in_addr_t   addr;
    int         i, octet, n, m;
    uint32_t    mask;
    char        c;
    in_addr_t  *pa;

    addr = 0;
    octet = 0;
    n = 0;
    pa = s;
    *s = INADDR_NONE;
    *e = INADDR_NONE;

    for (i = 0; i < sz; i++, p++) {
        c = *p;

        if (c >= '0' && c <='9') {
            octet = octet * 10 + (c - '0');
            continue;
        }

        if (c == '.' && octet < 256) {
            addr = (addr << 8) + octet;
            octet = 0;
            n++;
            continue;
        }

        if (c == '-') {
            if (n == 3 && octet < 256) {
                *pa = (addr << 8) + octet;

                addr = 0;
                octet = 0;
                n = 0;
                pa = e;
                continue;
            }

            return false;
        }

        if (c == '/') {
            i++;
            p++;
            m = 0;
            for ( ; i < sz; i++, p++) {
                c = *p;
                if (c >= '0' && c <= '9') {
                    m = m * 10 + (c - '0');
                    continue;
                }

                return false;
            }

            if (m > 31) {
                return false;
            }

            mask = 0xffffffff & ~((1 << (32 - m)) - 1);

            if (n == 3 && octet < 256) {
                addr = ((addr << 8) + octet) & mask;
                *s = addr & mask;
                mask = (1 << (32 - m)) - 1;
                *e = addr | mask;
                return true;
            }

            return false;
        }

        return false;
    }

    if (n == 3 && octet < 256) {
        addr = (addr << 8) + octet;
        *pa = addr;
    }

    if (*e != INADDR_NONE && *e < *s) {
        return false;
    }

    return true;
}


static void iplist_put_range(iplist_t *ipl, int idx, int s, int e) {
    uintptr_t  *bm;
    uintptr_t   i, j, k, m, n, h, l;
    
    bm = ipl->index[idx];
    if (bm == NULL) {
        bm = tccalloc(IPLIST_COUNT_PERVOL, IPLIST_SIZE_PERPTR);
        ipl->index[idx] = bm;
    }

    if (e - s <= IPLIST_COUNT_PERPTR) {
        for (i = s; i <= e; i++) {
            m = IPLIST_INDEX_PTR(i);
            n = IPLIST_INDEX_BIT(i);
            if (bm[m] & n) {
                continue;
            }
            bm[m] |= n;
            ipl->count[idx]++;
        }

        return;
    }

    h = IPLIST_ALIGN_UP(s);

    n = ((uintptr_t) 1 << (h - s)) - 1;
    m = IPLIST_INDEX_PTR(s);

    if ((bm[m] & n) == 0) {
        bm[m] |= n;
        ipl->count[idx] += h - s;

    } else {
        for (i = s; i < h; i++) {
            m = IPLIST_INDEX_PTR(i);
            n = IPLIST_INDEX_BIT(i);
            if (bm[m] & n) {
                continue;
            }
            bm[m] |= n;
            ipl->count[idx]++;
        }
    }

    l = IPLIST_ALIGN_DOWN(e);
    if ((e - h + 1) % IPLIST_COUNT_PERPTR == 0) {
        for (i = h; i < l + 1; i += IPLIST_COUNT_PERPTR) {
            m = IPLIST_INDEX_PTR(i);
            if (bm[m] == 0) {
                bm[m] = (uintptr_t) -1;
                ipl->count[idx] += IPLIST_COUNT_PERPTR;
    
            } else {
                for (j = 0; j < IPLIST_COUNT_PERPTR; j++) {
                    k = (uintptr_t) 1 << j;
                    if (bm[m] & k) {
                        continue;
                    }
                    bm[m] |= k;
                    ipl->count[idx]++;
                }
            }
        }
        return;
    }

    for (i = h; i < l; i += IPLIST_COUNT_PERPTR) {
        m = IPLIST_INDEX_PTR(i);
        if (bm[m] == 0) {
            bm[m] = (uintptr_t) -1;
            ipl->count[idx] += IPLIST_COUNT_PERPTR;

        } else {
            for (j = 0; j < IPLIST_COUNT_PERPTR; j++) {
                k = (uintptr_t) 1 << j;
                if (bm[m] & k) {
                    continue;
                }
                bm[m] |= k;
                ipl->count[idx]++;
            }
        }
    }

    for (i = l; i <= e; i++) {
        m = IPLIST_INDEX_PTR(i);
        n = IPLIST_INDEX_BIT(i);
        if (bm[m] & n) {
            continue;
        }
        bm[m] |= n;
        ipl->count[idx]++;
    }
}


static void iplist_out_range(iplist_t *ipl, int idx, int s, int e) {
    uintptr_t  *bm;
    uintptr_t   i, j, k, m, n, h, l;
    
    bm = ipl->index[idx];
    if (bm == NULL) {
        return;
    }

    if (e - s <= IPLIST_COUNT_PERPTR) {
        for (i = s; i <= e; i++) {
            m = IPLIST_INDEX_PTR(i);
            n = IPLIST_INDEX_BIT(i);
            if (bm[m] & n) {
                bm[m] &= ~n;
                if (--ipl->count[idx] == 0) {
                    ipl->index[idx] = NULL;
                    tcfree(bm);
                    return;
                }
            }
        }

        return;
    }

    h = IPLIST_ALIGN_UP(s);

    n = ((uintptr_t) 1 << (h - s)) - 1;
    m = IPLIST_INDEX_PTR(s);

    if ((bm[m] & n) == n) {
        bm[m] &= ~n;
        ipl->count[idx] -= (h - s);
        assert(ipl->count[idx] >= 0);
        if (ipl->count[idx] == 0) {
            return;
        }

    } else {
        for (i = s; i < h; i++) {
            m = IPLIST_INDEX_PTR(i);
            n = IPLIST_INDEX_BIT(i);
            if (bm[m] & n) {
                bm[m] &= ~n;
                if (--ipl->count[idx] == 0) {
                    ipl->index[idx] = NULL;
                    tcfree(bm);
                    return;
                }
            }
        }
    }

    l = IPLIST_ALIGN_DOWN(e);
    if ((e - h + 1) % IPLIST_COUNT_PERPTR == 0) {
        for (i = h; i < l + 1; i += IPLIST_COUNT_PERPTR) {
            m = IPLIST_INDEX_PTR(i);
            if (bm[m] == (uintptr_t) -1) {
                bm[m] = 0;
                ipl->count[idx] -= IPLIST_COUNT_PERPTR;
                assert(ipl->count[idx] >= 0);
                if (ipl->count[idx] == 0) {
                    ipl->index[idx] = NULL;
                    tcfree(bm);
                    return;
                }
   
            } else {
                for (j = 0; j < IPLIST_COUNT_PERPTR; j++) {
                    k = (uintptr_t) 1 << j;
                    if (bm[m] & k) {
                        bm[m] &= ~k;
                        if (--ipl->count[idx] == 0) {
                            ipl->index[idx] = NULL;
                            tcfree(bm);
                            return;
                        }
                    }
                }
            }
        }
        return;
    }

    for (i = h; i < l; i += IPLIST_COUNT_PERPTR) {
        m = IPLIST_INDEX_PTR(i);
        if (bm[m] == (uintptr_t) -1) {
            bm[m] = 0;
            ipl->count[idx] -= IPLIST_COUNT_PERPTR;
            assert(ipl->count[idx] >= 0);
            if (ipl->count[idx] == 0) {
                ipl->index[idx] = NULL;
                tcfree(bm);
                return;
            }

        } else {
            for (j = 0; j < IPLIST_COUNT_PERPTR; j++) {
                k = (uintptr_t) 1 << j;
                if (bm[m] & k) {
                    bm[m] &= ~k;
                    if (--ipl->count[idx] == 0) {
                        ipl->index[idx] = NULL;
                        tcfree(bm);
                        return;
                    }
                }
            }
        }
    }

    for (i = l; i <= e; i++) {
        m = IPLIST_INDEX_PTR(i);
        n = IPLIST_INDEX_BIT(i);
        if (bm[m] & n) {
            bm[m] &= ~n;
            if (--ipl->count[idx] == 0) {
                ipl->index[idx] = NULL;
                tcfree(bm);
                return;
            }
        }
    }
}


static void iplist_del(void *opq) {
    int        i;
    iplist_t  *ipl = (iplist_t *) opq;

    for (i = 0; i < IPLIST_INDEX_COUNT; i++) {
        if (ipl->count[i] > 0) {
            tcfree(ipl->index[i]);
        }

        pthread_rwlock_destroy(&ipl->lock[i]);
    }

    tcfree(ipl);
}


static bool iplist_open(void *opq, const char *name) {
    return true;
}


static bool iplist_close(void *opq) {
    return true;
}


static void *iplist_get(void *opq, const void *kbuf, int ksiz, int *sp) {
    uintptr_t   *bm, m, n, v;
    in_addr_t    addr, dummy;
    iplist_t    *ipl;
    int          idx;

    assert(opq && kbuf && ksiz >= 0 && sp);

    if (!iplist_parse_addr((char *) kbuf, ksiz, &addr, &dummy)) {
        return NULL;
    }

    ipl = (iplist_t *) opq;
    idx = IPLIST_HIGH_HALF(addr);
    m = IPLIST_INDEX_PTR(IPLIST_LOW_HALF(addr));
    n = IPLIST_INDEX_BIT(IPLIST_LOW_HALF(addr));

    if (pthread_rwlock_rdlock(&ipl->lock[idx]) != 0) {
        return NULL;
    }

    *sp = 1;
    bm = ipl->index[idx];
    if (bm == NULL) {
        pthread_rwlock_unlock(&ipl->lock[idx]);
        return tcmemdup("0", 1);
    }

    v = bm[m];
    pthread_rwlock_unlock(&ipl->lock[idx]);

    if (v & n) {
        return tcmemdup("1", 1);
    }

    return tcmemdup("0", 1);
}


static bool iplist_put(void *opq, const void *kbuf, int ksiz, 
    const void *vbuf, int vsiz)
{
    uintptr_t   s, e;
    in_addr_t   start, end;
    int         i, idx;
    iplist_t   *ipl;

    assert(opq && kbuf && ksiz >= 0 && vbuf && vsiz >= 0);

    /* limit value only can be "1" */
    if (vsiz != 1 || *(char *) vbuf != '1') {
        return false;
    }

    if (!iplist_parse_addr((char *) kbuf, ksiz, &start, &end)) {
        return false;
    }

    ipl = (iplist_t *) opq;

    if (end == INADDR_NONE || end == start) {
        idx = IPLIST_HIGH_HALF(start);
        s = IPLIST_LOW_HALF(start);

        if (pthread_rwlock_wrlock(&ipl->lock[idx]) != 0) {
            return false;
        }

        iplist_put_range(ipl, idx, s, s);

        pthread_rwlock_unlock(&ipl->lock[idx]);
        return true;
    }

    if ((start & 0xffff0000) == (end & 0xffff0000)) {
        idx = IPLIST_HIGH_HALF(start);
        s = IPLIST_LOW_HALF(start);
        e = IPLIST_LOW_HALF(end);

        if (pthread_rwlock_wrlock(&ipl->lock[idx]) != 0) {
            return false;
        }

        iplist_put_range(ipl, idx, s, e);

        pthread_rwlock_unlock(&ipl->lock[idx]);
        return true;
    }

    idx = IPLIST_HIGH_HALF(start);
    s = IPLIST_LOW_HALF(start);
    e = 65535;

    if (pthread_rwlock_wrlock(&ipl->lock[idx]) != 0) {
        return false;
    }

    iplist_put_range(ipl, idx, s, e);

    pthread_rwlock_unlock(&ipl->lock[idx]);

    for (i = IPLIST_HIGH_HALF(start) + 1; i < IPLIST_HIGH_HALF(end); i++) {
        if (pthread_rwlock_wrlock(&ipl->lock[i]) != 0) {
            return false;
        }

        iplist_put_range(ipl, i, 0, 65535);

        pthread_rwlock_unlock(&ipl->lock[idx]);
    }

    e = IPLIST_LOW_HALF(end);

    if (pthread_rwlock_wrlock(&ipl->lock[i]) != 0) {
        return false;
    }

    iplist_put_range(ipl, i, 0, e);

    pthread_rwlock_unlock(&ipl->lock[idx]);
    return true;
}


static bool iplist_out(void *opq, const void *kbuf, int ksiz) {
    uintptr_t   s, e;
    in_addr_t   start, end;
    int         idx, i;
    iplist_t   *ipl;

    assert(opq && kbuf && ksiz >= 0);

    if (!iplist_parse_addr((char *) kbuf, ksiz, &start, &end)) {
        return false;
    }

    ipl = (iplist_t *) opq;

    if (end == INADDR_NONE || end == start) {
        idx = IPLIST_HIGH_HALF(start);
        s = IPLIST_LOW_HALF(start);

        if (pthread_rwlock_wrlock(&ipl->lock[idx]) != 0) {
            return false;
        }

        iplist_out_range(ipl, idx, s, s);

        pthread_rwlock_unlock(&ipl->lock[idx]);
        return true;
    }

    if ((start & 0xffff0000) == (end & 0xffff0000)) {
        idx = IPLIST_HIGH_HALF(start);
        s = IPLIST_LOW_HALF(start);
        e = IPLIST_LOW_HALF(end);

        if (pthread_rwlock_wrlock(&ipl->lock[idx]) != 0) {
            return false;
        }

        iplist_out_range(ipl, idx, s, e);

        pthread_rwlock_unlock(&ipl->lock[idx]);
        return true;
    }

    idx = IPLIST_HIGH_HALF(start);
    s = IPLIST_LOW_HALF(start);
    e = 65535;

    if (pthread_rwlock_wrlock(&ipl->lock[idx]) != 0) {
        return false;
    }

    iplist_put_range(ipl, idx, s, e);

    pthread_rwlock_unlock(&ipl->lock[idx]);

    for (i = IPLIST_HIGH_HALF(start) + 1; i < IPLIST_HIGH_HALF(end); i++) {
        if (pthread_rwlock_wrlock(&ipl->lock[i]) != 0) {
            return false;
        }

        iplist_out_range(ipl, i, 0, 65535);

        pthread_rwlock_unlock(&ipl->lock[i]);
    }

    e = IPLIST_LOW_HALF(end);

    if (pthread_rwlock_wrlock(&ipl->lock[i]) != 0) {
        return false;
    }

    iplist_out_range(ipl, i, 0, e);

    pthread_rwlock_unlock(&ipl->lock[i]);
    return true;
}


static uint64_t iplist_rnum(void *opq) {
    iplist_t   *ipl;
    int         i;
    uint64_t    rnum = 0, cnt;

    assert(opq);

    ipl = (iplist_t *) opq;

    for (i = 0; i < IPLIST_INDEX_COUNT; i++) {

        if (pthread_rwlock_rdlock(&ipl->lock[i]) != 0) {
            return (uint64_t) -1;
        }

        cnt = ipl->count[i];

        pthread_rwlock_unlock(&ipl->lock[i]);

        rnum += cnt;
    }

    return rnum;
}


static uint64_t iplist_size(void *opq) {
    int         i;
    iplist_t   *ipl;
    uint64_t    size, cnt;

    assert(opq);

    size = sizeof(iplist_t);
    cnt = 0;

    ipl = (iplist_t *) opq;

    for (i = 0; i < IPLIST_INDEX_COUNT; i++) {
        if (pthread_rwlock_rdlock(&ipl->lock[i]) != 0) {
            return (uint64_t) -1;
        }

        if (ipl->count[i] > 0) {
            cnt++;
        }

        pthread_rwlock_unlock(&ipl->lock[i]);
    }

    size += cnt * IPLIST_COUNT_PERVOL * IPLIST_SIZE_PERPTR;
    return size;
}


static bool iplist_vanish(void *opq) {
    int         i;
    iplist_t   *ipl;

    assert(opq);

    ipl = (iplist_t *) opq;

    for (i = 0; i < IPLIST_INDEX_COUNT; i++) {
        if (pthread_rwlock_rdlock(&ipl->lock[i]) != 0) {
            return (uint64_t) -1;
        }

        if (ipl->count[i] > 0) {
            tcfree(ipl->index[i]);
            ipl->index[i] = NULL;
            ipl->count[i] = 0;
        }

        pthread_rwlock_unlock(&ipl->lock[i]);
    }

    return true;
}


bool initialize(ADBSKEL *skel) {
    int        i;
    iplist_t  *ipl;

    ipl = tccalloc(1, sizeof(iplist_t));
    skel->opq = ipl;
    skel->del = iplist_del;
    skel->open = iplist_open;
    skel->close = iplist_close;
    skel->get = iplist_get;
    skel->put = iplist_put;
    skel->out = iplist_out;
    skel->rnum = iplist_rnum;
    skel->size = iplist_size;
    skel->vanish = iplist_vanish;

    for (i = 0; i < IPLIST_INDEX_COUNT; i++) {
        pthread_rwlock_init(&ipl->lock[i], NULL);
    }

    return true;
}


/* Add interp section just for geek.
 * This partion of code can be remove to Makefile 
 * to determine RTLD(runtime loader). */
#if __WORDSIZE == 64
#define RUNTIME_LINKER  "/lib64/ld-linux-x86-64.so.2"
#else
#define RUNTIME_LINKER  "/lib/ld-linux.so.2"
#endif

#ifndef __SO_INTERP__
#define __SO_INTERP__
const char __invoke_dynamic_linker__[] __attribute__ ((section (".interp")))
    = RUNTIME_LINKER;
#endif /* __SO_INTERP__ */


void __ttskeliplist_main(void) {
    printf("** TokyoTyrant Skel *IPLIST* Plugin **\n");
    printf("Copyright (c) flygoast, flygoast@126.com\n");
    printf("This plugin implements a bitmap of IPV4 address.\n"
           "It mainly would be used as an Access Control List storage.\n");
    exit(0);
}
