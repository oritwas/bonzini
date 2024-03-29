/*
 * Coroutine-aware I/O functions
 *
 * Copyright (C) 2009-2010 Nippon Telegraph and Telephone Corporation.
 * Copyright (c) 2011, Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu-common.h"
#include "qemu_socket.h"
#include "qemu-coroutine.h"
#include "iov.h"

ssize_t coroutine_fn
qemu_co_sendv_recvv(int sockfd, struct iovec *iov, unsigned iov_cnt,
                    size_t offset, size_t bytes, bool do_send)
{
    size_t done = 0;
    ssize_t ret;
    while (done < bytes) {
        ret = iov_send_recv(sockfd, iov, iov_cnt,
                            offset + done, bytes - done, do_send, true);
        if (ret > 0) {
            done += ret;
        } else if (ret < 0) {
            if (errno == EAGAIN) {
                qemu_coroutine_yield();
            } else if (done == 0) {
                return -1;
            } else {
                break;
            }
        } else if (ret == 0 && !do_send) {
            /* write (send) should never return 0.
             * read (recv) returns 0 for end-of-file (-data).
             * In both cases there's little point retrying,
             * but we do for write anyway, just in case */
            break;
        }
    }
    return done;
}

ssize_t coroutine_fn
qemu_co_send_recv(int sockfd, void *buf, size_t bytes, bool do_send)
{
    struct iovec iov = { .iov_base = buf, .iov_len = bytes };
    return qemu_co_sendv_recvv(sockfd, &iov, 1, 0, bytes, do_send);
}
