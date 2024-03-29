/*
 * Helpers for getting linearized buffers from iov / filling buffers into iovs
 *
 * Copyright IBM, Corp. 2007, 2008
 * Copyright (C) 2010 Red Hat, Inc.
 *
 * Author(s):
 *  Anthony Liguori <aliguori@us.ibm.com>
 *  Amit Shah <amit.shah@redhat.com>
 *  Michael Tokarev <mjt@tls.msk.ru>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "iov.h"

#ifdef _WIN32
# include <windows.h>
# include <winsock2.h>
#else
# include <sys/types.h>
# include <sys/socket.h>
#endif

size_t iov_from_buf(const struct iovec *iov, unsigned int iov_cnt,
                    size_t offset, const void *buf, size_t bytes)
{
    size_t done;
    unsigned int i;
    for (i = 0, done = 0; (offset || done < bytes) && i < iov_cnt; i++) {
        if (offset < iov[i].iov_len) {
            size_t len = MIN(iov[i].iov_len - offset, bytes - done);
            memcpy(iov[i].iov_base + offset, buf + done, len);
            done += len;
            offset = 0;
        } else {
            offset -= iov[i].iov_len;
        }
    }
    assert(offset == 0);
    return done;
}

size_t iov_to_buf(const struct iovec *iov, const unsigned int iov_cnt,
                  size_t offset, void *buf, size_t bytes)
{
    size_t done;
    unsigned int i;
    for (i = 0, done = 0; (offset || done < bytes) && i < iov_cnt; i++) {
        if (offset < iov[i].iov_len) {
            size_t len = MIN(iov[i].iov_len - offset, bytes - done);
            memcpy(buf + done, iov[i].iov_base + offset, len);
            done += len;
            offset = 0;
        } else {
            offset -= iov[i].iov_len;
        }
    }
    assert(offset == 0);
    return done;
}

size_t iov_memset(const struct iovec *iov, const unsigned int iov_cnt,
                  size_t offset, int fillc, size_t bytes)
{
    size_t done;
    unsigned int i;
    for (i = 0, done = 0; (offset || done < bytes) && i < iov_cnt; i++) {
        if (offset < iov[i].iov_len) {
            size_t len = MIN(iov[i].iov_len - offset, bytes - done);
            memset(iov[i].iov_base + offset, fillc, len);
            done += len;
            offset = 0;
        } else {
            offset -= iov[i].iov_len;
        }
    }
    assert(offset == 0);
    return done;
}

size_t iov_size(const struct iovec *iov, const unsigned int iov_cnt)
{
    size_t len;
    unsigned int i;

    len = 0;
    for (i = 0; i < iov_cnt; i++) {
        len += iov[i].iov_len;
    }
    return len;
}

#if defined CONFIG_IOVEC && defined CONFIG_POSIX
#define HAS_SEND_MSG (true)
#else
#define HAS_SEND_MSG (false)
#endif

/* helper function for iov_send_recv() */
static ssize_t
do_send_recv(int sockfd, struct iovec *iov, unsigned iov_cnt, bool do_send,
             bool use_send_msg)
{
    if (HAS_SEND_MSG && use_send_msg) {
        ssize_t ret;
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = iov;
        msg.msg_iovlen = iov_cnt;
        do {
            ret = do_send
               ? sendmsg(sockfd, &msg, 0)
               : recvmsg(sockfd, &msg, 0);
        } while (ret < 0 && errno == EINTR);
        return ret;
    } else {
        /* else send piece-by-piece */
        /*XXX Note: windows has WSASend() and WSARecv() */
        unsigned i = 0;
        ssize_t ret = 0;
        while (i < iov_cnt) {
            ssize_t r = do_send
                ? send(sockfd, iov[i].iov_base, iov[i].iov_len, 0)
                : recv(sockfd, iov[i].iov_base, iov[i].iov_len, 0);
            if (r > 0) {
               ret += r;
            } else if (!r) {
                break;
            } else if (errno == EINTR) {
                continue;
            } else {
                /* else it is some "other" error,
                 * only return if there was no data processed. */
                if (ret == 0) {
                    ret = -1;
                }
                break;
            }
            i++;
        }
        return ret;
    }
}

ssize_t iov_send_recv(int sockfd, struct iovec *iov, unsigned iov_cnt,
                      size_t offset, size_t bytes,
                      bool do_send, bool use_send_msg)
{
    ssize_t ret;
    unsigned si, ei;            /* start and end indexes */
    if (bytes == 0) {
        /* Catch the do-nothing case early, as otherwise we will pass an
         * empty iovec to sendmsg/recvmsg(), and not all implementations
         * accept this.
         */
        return 0;
    }

    /* Find the start position, skipping `offset' bytes:
     * first, skip all full-sized vector elements, */
    for (si = 0; si < iov_cnt && offset >= iov[si].iov_len; ++si) {
        offset -= iov[si].iov_len;
    }
    if (offset) {
        assert(si < iov_cnt);
        /* second, skip `offset' bytes from the (now) first element,
         * undo it on exit */
        iov[si].iov_base += offset;
        iov[si].iov_len -= offset;
    }
    /* Find the end position skipping `bytes' bytes: */
    /* first, skip all full-sized elements */
    for (ei = si; ei < iov_cnt && iov[ei].iov_len <= bytes; ++ei) {
        bytes -= iov[ei].iov_len;
    }
    if (bytes) {
        /* second, fixup the last element, and remember
         * the length we've cut from the end of it in `bytes' */
        size_t tail;
        assert(ei < iov_cnt);
        assert(iov[ei].iov_len > bytes);
        tail = iov[ei].iov_len - bytes;
        iov[ei].iov_len = bytes;
        bytes = tail;  /* bytes is now equal to the tail size */
        ++ei;
    }

    ret = do_send_recv(sockfd, iov + si, ei - si, do_send, use_send_msg);

    /* Undo the changes above */
    if (offset) {
        iov[si].iov_base -= offset;
        iov[si].iov_len += offset;
    }
    if (bytes) {
        iov[ei-1].iov_len += bytes;
    }

    return ret;
}


void iov_hexdump(const struct iovec *iov, const unsigned int iov_cnt,
                 FILE *fp, const char *prefix, size_t limit)
{
    unsigned int i, v, b;
    uint8_t *c;

    c = iov[0].iov_base;
    for (i = 0, v = 0, b = 0; b < limit; i++, b++) {
        if (i == iov[v].iov_len) {
            i = 0; v++;
            if (v == iov_cnt) {
                break;
            }
            c = iov[v].iov_base;
        }
        if ((b % 16) == 0) {
            fprintf(fp, "%s: %04x:", prefix, b);
        }
        if ((b % 4) == 0) {
            fprintf(fp, " ");
        }
        fprintf(fp, " %02x", c[i]);
        if ((b % 16) == 15) {
            fprintf(fp, "\n");
        }
    }
    if ((b % 16) != 0) {
        fprintf(fp, "\n");
    }
}

unsigned iov_copy(struct iovec *dst_iov, unsigned int dst_iov_cnt,
                 const struct iovec *iov, unsigned int iov_cnt,
                 size_t offset, size_t bytes)
{
    size_t len;
    unsigned int i, j;
    for (i = 0, j = 0; i < iov_cnt && j < dst_iov_cnt && bytes; i++) {
        if (offset >= iov[i].iov_len) {
            offset -= iov[i].iov_len;
            continue;
        }
        len = MIN(bytes, iov[i].iov_len - offset);

        dst_iov[j].iov_base = iov[i].iov_base + offset;
        dst_iov[j].iov_len = len;
        j++;
        bytes -= len;
        offset = 0;
    }
    assert(offset == 0);
    return j;
}

/* io vectors */

void qemu_iovec_init(QEMUIOVector *qiov, int alloc_hint)
{
    qiov->iov = g_malloc(alloc_hint * sizeof(struct iovec));
    qiov->niov = 0;
    qiov->nalloc = alloc_hint;
    qiov->size = 0;
}

void qemu_iovec_init_external(QEMUIOVector *qiov, struct iovec *iov, int niov)
{
    int i;

    qiov->iov = iov;
    qiov->niov = niov;
    qiov->nalloc = -1;
    qiov->size = 0;
    for (i = 0; i < niov; i++)
        qiov->size += iov[i].iov_len;
}

void qemu_iovec_add(QEMUIOVector *qiov, void *base, size_t len)
{
    assert(qiov->nalloc != -1);

    if (qiov->niov == qiov->nalloc) {
        qiov->nalloc = 2 * qiov->nalloc + 1;
        qiov->iov = g_realloc(qiov->iov, qiov->nalloc * sizeof(struct iovec));
    }
    qiov->iov[qiov->niov].iov_base = base;
    qiov->iov[qiov->niov].iov_len = len;
    qiov->size += len;
    ++qiov->niov;
}

/*
 * Concatenates (partial) iovecs from src to the end of dst.
 * It starts copying after skipping `soffset' bytes at the
 * beginning of src and adds individual vectors from src to
 * dst copies up to `sbytes' bytes total, or up to the end
 * of src if it comes first.  This way, it is okay to specify
 * very large value for `sbytes' to indicate "up to the end
 * of src".
 * Only vector pointers are processed, not the actual data buffers.
 */
void qemu_iovec_concat(QEMUIOVector *dst,
                       QEMUIOVector *src, size_t soffset, size_t sbytes)
{
    int i;
    size_t done;
    struct iovec *siov = src->iov;
    assert(dst->nalloc != -1);
    assert(src->size >= soffset);
    for (i = 0, done = 0; done < sbytes && i < src->niov; i++) {
        if (soffset < siov[i].iov_len) {
            size_t len = MIN(siov[i].iov_len - soffset, sbytes - done);
            qemu_iovec_add(dst, siov[i].iov_base + soffset, len);
            done += len;
            soffset = 0;
        } else {
            soffset -= siov[i].iov_len;
        }
    }
    /* return done; */
}

void qemu_iovec_destroy(QEMUIOVector *qiov)
{
    assert(qiov->nalloc != -1);

    qemu_iovec_reset(qiov);
    g_free(qiov->iov);
    qiov->nalloc = 0;
    qiov->iov = NULL;
}

void qemu_iovec_reset(QEMUIOVector *qiov)
{
    assert(qiov->nalloc != -1);

    qiov->niov = 0;
    qiov->size = 0;
}

size_t qemu_iovec_to_buf(QEMUIOVector *qiov, size_t offset,
                         void *buf, size_t bytes)
{
    return iov_to_buf(qiov->iov, qiov->niov, offset, buf, bytes);
}

size_t qemu_iovec_from_buf(QEMUIOVector *qiov, size_t offset,
                           const void *buf, size_t bytes)
{
    return iov_from_buf(qiov->iov, qiov->niov, offset, buf, bytes);
}

size_t qemu_iovec_memset(QEMUIOVector *qiov, size_t offset,
                         int fillc, size_t bytes)
{
    return iov_memset(qiov->iov, qiov->niov, offset, fillc, bytes);
}
