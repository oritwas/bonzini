/*
 * Helpers for using (partial) iovecs.
 *
 * Copyright (C) 2010 Red Hat, Inc.
 *
 * Author(s):
 *  Amit Shah <amit.shah@redhat.com>
 *  Michael Tokarev <mjt@tls.msk.ru>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu-common.h"

/**
 * count and return data size, in bytes, of an iovec
 * starting at `iov' of `iov_cnt' number of elements.
 */
size_t iov_size(const struct iovec *iov, const unsigned int iov_cnt);

/**
 * Copy from single continuous buffer to scatter-gather vector of buffers
 * (iovec) and back like memcpy() between two continuous memory regions.
 * Data in single continuous buffer starting at address `buf' and
 * `bytes' bytes long will be copied to/from an iovec `iov' with
 * `iov_cnt' number of elements, starting at byte position `offset'
 * within the iovec.  If the iovec does not contain enough space,
 * only part of data will be copied, up to the end of the iovec.
 * Number of bytes actually copied will be returned, which is
 *  min(bytes, iov_size(iov)-offset)
 * `Offset' must point to the inside of iovec.
 * It is okay to use very large value for `bytes' since we're
 * limited by the size of the iovec anyway, provided that the
 * buffer pointed to by buf has enough space.  One possible
 * such "large" value is -1 (sinice size_t is unsigned),
 * so specifying `-1' as `bytes' means 'up to the end of iovec'.
 */
size_t iov_from_buf(const struct iovec *iov, unsigned int iov_cnt,
                    size_t offset, const void *buf, size_t bytes);
size_t iov_to_buf(const struct iovec *iov, const unsigned int iov_cnt,
                  size_t offset, void *buf, size_t bytes);

/**
 * Set data bytes pointed out by iovec `iov' of size `iov_cnt' elements,
 * starting at byte offset `start', to value `fillc', repeating it
 * `bytes' number of times.  `Offset' must point to the inside of iovec.
 * If `bytes' is large enough, only last bytes portion of iovec,
 * up to the end of it, will be filled with the specified value.
 * Function return actual number of bytes processed, which is
 * min(size, iov_size(iov) - offset).
 * Again, it is okay to use large value for `bytes' to mean "up to the end".
 */
size_t iov_memset(const struct iovec *iov, const unsigned int iov_cnt,
                  size_t offset, int fillc, size_t bytes);

/*
 * Send/recv data from/to iovec buffers directly
 *
 * `offset' bytes in the beginning of iovec buffer are skipped and
 * next `bytes' bytes are used, which must be within data of iovec.
 *
 *   r = iov_send_recv(sockfd, iov, iovcnt, offset, bytes, true);
 *
 * is logically equivalent to
 *
 *   char *buf = malloc(bytes);
 *   iov_to_buf(iov, iovcnt, offset, buf, bytes);
 *   r = send(sockfd, buf, bytes, 0);
 *   free(buf);
 *
 * For iov_send_recv() _whole_ area being sent or received
 * should be within the iovec, not only beginning of it.
 */
ssize_t iov_send_recv(int sockfd, struct iovec *iov, unsigned iov_cnt,
                      size_t offset, size_t bytes, bool do_send,
                      bool use_sendmsg);

#define iov_recv(sockfd, iov, iov_cnt, offset, bytes) \
    iov_send_recv(sockfd, iov, iov_cnt, offset, bytes, false, true)
#define iov_send(sockfd, iov, iov_cnt, offset, bytes) \
    iov_send_recv(sockfd, iov, iov_cnt, offset, bytes, true, true)
#define iov_send_no_sendmsg(sockfd, iov, iov_cnt, offset, bytes) \
    iov_send_recv(sockfd, iov, iov_cnt, offset, bytes, true, false)


/**
 * Produce a text hexdump of iovec `iov' with `iov_cnt' number of elements
 * in file `fp', prefixing each line with `prefix' and processing not more
 * than `limit' data bytes.
 */
void iov_hexdump(const struct iovec *iov, const unsigned int iov_cnt,
                 FILE *fp, const char *prefix, size_t limit);

/*
 * Partial copy of vector from iov to dst_iov (data is not copied).
 * dst_iov overlaps iov at a specified offset.
 * size of dst_iov is at most bytes. dst vector count is returned.
 */
unsigned iov_copy(struct iovec *dst_iov, unsigned int dst_iov_cnt,
                 const struct iovec *iov, unsigned int iov_cnt,
                 size_t offset, size_t bytes);
