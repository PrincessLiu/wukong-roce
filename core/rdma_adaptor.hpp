/*
 * Copyright (c) 2016 Shanghai Jiao Tong University.
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://ipads.se.sjtu.edu.cn/projects/wukong
 *
 */

#pragma once

#include <string>
#include <iostream>
#include <unistd.h>
#include <unordered_map>
#include <fstream>
#include <errno.h>
#include <sstream>

#include "config.hpp"
#include "rdma.hpp"

using namespace std;

#define WK_CLINE 64

// The communication over RDMA-based ring buffer
class RDMA_Adaptor {
private:
    Mem *mem;
    int sid;
    int num_servers;
    int num_threads;

    /// The ring-buffer space contains #threads logical-queues.
    /// Each logical-queue contains #servers physical queues (ring-buffer).
    /// The X physical-queue (ring-buffer) of thread(tid) is written by the responding threads
    /// (proxies and engine with the same "tid") on the X server.
    /// Access mode of physical queue is N writers (from the same server) and 1 reader.

    // track tail of ring buffer for writer
    struct rbf_rmeta_t {
        uint64_t tail;
        pthread_spinlock_t lock;
    } __attribute__ ((aligned (WK_CLINE)));

    // track head of ring buffer for reader
    struct rbf_lmeta_t {
        uint64_t head;
        pthread_spinlock_t lock;
    } __attribute__ ((aligned (WK_CLINE)));

    rbf_rmeta_t *rmetas = NULL;
    rbf_lmeta_t *lmetas = NULL;

    // each thread uses a round-robin strategy to check its physical-queues
    struct scheduler_t {
        uint64_t rr_cnt; // round-robin
    } __attribute__ ((aligned (WK_CLINE)));

    scheduler_t *schedulers;

    // Align given value down to given alignment
    uint64_t inline floor(uint64_t val, uint64_t alignment) {
        ASSERT(alignment != 0);
        return val - val % alignment;
    }

    // Align given value up to given alignment
    uint64_t inline ceil(uint64_t val, uint64_t alignment) {
        ASSERT(alignment != 0);
        if (val % alignment == 0)
            return val;
        return val - val % alignment + alignment;
    }

    // Check if there is data from threads in dst_sid to tid
    bool check(int tid, int dst_sid) {
        rbf_lmeta_t *lmeta = &lmetas[tid * num_servers + dst_sid];
        char *rbf = mem->ring(tid, dst_sid); // ring buffer for tid to recv data from threads in dst_sid
        uint64_t rbf_sz = mem->ring_size();
        volatile uint64_t data_sz = *(volatile uint64_t *)(rbf + lmeta->head % rbf_sz);  // header

        return (data_sz != 0);
    }

    // Fetch data from threads in dst_sid to tid
    std::string fetch(int tid, int dst_sid) {
        rbf_lmeta_t *lmeta = &lmetas[tid * num_servers + dst_sid];
        char * rbf = mem->ring(tid, dst_sid);
        uint64_t rbf_sz = mem->ring_size();
        // struct of data: [size | data | size]
        volatile uint64_t data_sz = *(volatile uint64_t *)(rbf + lmeta->head % rbf_sz);  // header
        *(uint64_t *)(rbf + lmeta->head % rbf_sz) = 0;  // clean header

        uint64_t to_footer = sizeof(uint64_t) + ceil(data_sz, sizeof(uint64_t));
        volatile uint64_t * footer = (volatile uint64_t *)(rbf + (lmeta->head + to_footer) % rbf_sz); // footer
        while (*footer != data_sz) { // spin-wait RDMA-WRITE done
            _mm_pause();
            /* If RDMA-WRITE is done, footer == header == size
             * Otherwise, footer == 0
             */
            ASSERT(*footer == 0 || *footer == data_sz);
        }
        *footer = 0;  // clean footer

        // actually read data
        std::string result;
        result.reserve(data_sz);
        uint64_t start = (lmeta->head + sizeof(uint64_t)) % rbf_sz; // start of data
        uint64_t end = (lmeta->head + sizeof(uint64_t) + data_sz) % rbf_sz;  // end of data
        if (start < end) {
            result.append(rbf + start, data_sz);
            memset(rbf + start, 0, ceil(data_sz, sizeof(uint64_t)));  // clean data
        } else { // overwrite from the start
            result.append(rbf + start, data_sz - end);
            result.append(rbf, end);
            memset(rbf + start, 0, data_sz - end);                    // clean data
            memset(rbf, 0, ceil(end, sizeof(uint64_t)));              // clean data
        }
        lmeta->head += 2 * sizeof(uint64_t) + ceil(data_sz, sizeof(uint64_t));

        // update heads of ring buffer to writer to help it detect overflow
        char *head = mem->local_ring_head(tid, dst_sid);
        const uint64_t threshold = rbf_sz / 8;
        if (lmeta->head - * (uint64_t *)head > threshold) {
            *(uint64_t *)head = lmeta->head;
            if (sid != dst_sid) {  // update to remote server
                RDMA &rdma = RDMA::get_rdma();
                uint64_t remote_head = mem->remote_ring_head_offset(tid, sid);
                rdma.dev->RdmaWrite(tid, dst_sid, head, mem->remote_ring_head_size(), remote_head);
            } else {
                *(uint64_t *)mem->remote_ring_head(tid, sid) = lmeta->head;
            }
        }

        return result;
    } // end of fetch

    /* Check whether overflow occurs if given msg is sent
     * @tid tid of writer
     * @dst_sid, @dst_tid sid and tid of reader
     * @msg_sz size of msg to send
    */
    inline bool rbf_full(int tid, int dst_sid, int dst_tid, uint64_t msg_sz) {
        uint64_t rbf_sz = mem->ring_size();
        uint64_t tail = rmetas[dst_sid * num_threads + dst_tid].tail;
        uint64_t head = *(uint64_t *)mem->remote_ring_head(dst_tid, dst_sid);

        return (rbf_sz < (tail - head + msg_sz));
    }

public:
    bool init = false;


    RDMA_Adaptor(int sid, Mem *mem, int num_servers, int num_threads)
        : sid(sid), mem(mem), num_servers(num_servers), num_threads(num_threads) {

        // no RDMA device
        if (!RDMA::get_rdma().has_rdma()) return;

        // init the metadata of remote and local ring-buffers
        int nrbfs = num_servers * num_threads;

        rmetas = (rbf_rmeta_t *)malloc(sizeof(rbf_rmeta_t) * nrbfs);
        memset(rmetas, 0, sizeof(rbf_rmeta_t) * nrbfs);
        for (int i = 0; i < nrbfs; i++) {
            rmetas[i].tail = 0;
            pthread_spin_init(&rmetas[i].lock, 0);
        }

        lmetas = (rbf_lmeta_t *)malloc(sizeof(rbf_lmeta_t) * nrbfs);
        memset(lmetas, 0, sizeof(rbf_lmeta_t) * nrbfs);
        for (int i = 0; i < nrbfs; i++) {
            lmetas[i].head = 0;
            pthread_spin_init(&lmetas[i].lock, 0);
        }

        schedulers = (scheduler_t *)malloc(sizeof(scheduler_t) * num_threads);
        memset(schedulers, 0, sizeof(scheduler_t) * num_threads);

        init = true;
    }

    ~RDMA_Adaptor() { }  //TODO

    // Send given string to (dst_sid, dst_tid) by thread(tid)
    // Return false if failed . Otherwise, return true.
    bool send(int tid, int dst_sid, int dst_tid, string str) {
        ASSERT(init);

        rbf_rmeta_t *rmeta = &rmetas[dst_sid * num_threads + dst_tid];
        uint64_t rbf_sz = mem->ring_size();

        const char *data = str.c_str();
        uint64_t data_sz = str.length();

        // struct of data: [size | data | size] (use size of data as header and footer)
        uint64_t msg_sz = sizeof(uint64_t) + ceil(data_sz, sizeof(uint64_t)) + sizeof(uint64_t);

        ASSERT(msg_sz < rbf_sz);

        pthread_spin_lock(&rmeta->lock);
        if (rbf_full(tid, dst_sid, dst_tid, msg_sz)) { // detect overflow
            pthread_spin_unlock(&rmeta->lock);
            return false;
        }

        if (sid == dst_sid) { // local physical-queue
            uint64_t off = rmeta->tail;
            rmeta->tail += msg_sz;
            pthread_spin_unlock(&rmeta->lock);

            // write msg to the local physical-queue
            char *ptr = mem->ring(dst_tid, sid);
            *((uint64_t *)(ptr + off % rbf_sz)) = data_sz;       // header
            off += sizeof(uint64_t);
            if (off / rbf_sz == (off + data_sz - 1) / rbf_sz ) { // data
                memcpy(ptr + (off % rbf_sz), data, data_sz);
            } else {
                uint64_t _sz = rbf_sz - (off % rbf_sz);
                memcpy(ptr + (off % rbf_sz), data, _sz);
                memcpy(ptr, data + _sz, data_sz - _sz);
            }
            off += ceil(data_sz, sizeof(uint64_t));
            *((uint64_t *)(ptr + off % rbf_sz)) = data_sz;       // footer
        } else { // remote physical-queue
            uint64_t off = rmeta->tail;
            rmeta->tail += msg_sz;
            pthread_spin_unlock(&rmeta->lock);

            // prepare RDMA buffer for RDMA-WRITE
            char *rdma_buf = mem->buffer(tid);
            uint64_t buf_sz = mem->buffer_size();
            ASSERT(msg_sz < buf_sz); // enough space to buffer the msg

            *((uint64_t *)rdma_buf) = data_sz;  // header
            rdma_buf += sizeof(uint64_t);
            memcpy(rdma_buf, data, data_sz);    // data
            rdma_buf += ceil(data_sz, sizeof(uint64_t));
            *((uint64_t*)rdma_buf) = data_sz;   // footer

            // write msg to the remote physical-queue
            RDMA &rdma = RDMA::get_rdma();
            uint64_t rdma_off = mem->ring_offset(dst_tid, sid);
            if (off / rbf_sz == (off + msg_sz - 1) / rbf_sz ) {
                rdma.dev->RdmaWrite(tid, dst_sid, mem->buffer(tid), msg_sz, rdma_off + (off % rbf_sz));
            } else {
                uint64_t _sz = rbf_sz - (off % rbf_sz);
                rdma.dev->RdmaWrite(tid, dst_sid, mem->buffer(tid), _sz, rdma_off + (off % rbf_sz));
                rdma.dev->RdmaWrite(tid, dst_sid, mem->buffer(tid) + _sz, msg_sz - _sz, rdma_off);
            }
        }

        return true;
    } // end of send

    std::string recv(int tid) {
        ASSERT(init);

        while (true) {
            // each thread has a logical-queue (#servers physical-queues)
            int dst_sid = (schedulers[tid].rr_cnt++) % num_servers; // round-robin
            if (check(tid, dst_sid))
                return fetch(tid, dst_sid);
        }
    }

    // Try to recv data of given thread
    bool tryrecv(int tid, std::string &str) {
        ASSERT(init);

        // check all physical-queues of tid once
        for (int dst_sid = 0; dst_sid < num_servers; dst_sid++) {
            if (check(tid, dst_sid)) {
                str = fetch(tid, dst_sid);
                return true;
            }
        }
        return false;
    }
};
