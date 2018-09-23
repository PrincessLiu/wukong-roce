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

#include "config.hpp"
#include "query.hpp"
#include "tcp_adaptor.hpp"
#include "rdma_adaptor.hpp"

/// TODO: define adaptor as a C++ interface and make tcp and rdma implement it
class Adaptor {
public:
    int tid; // thread id

    TCP_Adaptor *tcp;   // communicaiton by TCP/IP
    RDMA_Adaptor *rdma; // communicaiton by RDMA

    Adaptor(int tid, TCP_Adaptor *tcp, RDMA_Adaptor *rdma)
        : tid(tid), tcp(tcp), rdma(rdma) { }

    ~Adaptor() { }

    bool send(int dst_sid, int dst_tid, Bundle &bundle) {
        if (global_use_rdma && rdma->init)
            return rdma->send(tid, dst_sid, dst_tid, bundle.get_type() + bundle.data);
        else
            return tcp->send(dst_sid, dst_tid, bundle.get_type() + bundle.data);
    }

    Bundle recv() {
        std::string str;
        if (global_use_rdma && rdma->init)
            str = rdma->recv(tid);
        else
            str = tcp->recv(tid);

        Bundle bundle(str);
        return bundle;
    }

    bool tryrecv(Bundle &bundle) {
        std::string str;
        if (global_use_rdma && rdma->init) {
            if (!rdma->tryrecv(tid, str)) return false;
        } else {
            if (!tcp->tryrecv(tid, str)) return false;
        }

        bundle.set_type(str.at(0));
        bundle.data = str.substr(1);
        return true;
    }
};
