// -*- c++ -*-
// Copyright (c) Microsoft Corporation.
// Licensed under the GNU General Public License v3.0 or later.
// See License.txt in the project root for license information.

#ifndef INCLUDED_TCP_CLIENT_LEGACY_H
#define INCLUDED_TCP_CLIENT_LEGACY_H

#include <string>
#include <netinet/in.h>


namespace gr {
namespace difi {

class tcp_client_legacy
{
    public:

        tcp_client_legacy(std::string ip_addr, uint32_t port);
        ~tcp_client_legacy();

        bool connect();
        bool is_connected();
        int send(int8_t* buf, int lent);

    private:

        void create_socket();

        int d_socket;
        struct sockaddr_in d_servaddr;
};

} // namespace difi
} // namespace gr

#endif /* INCLUDED_TCP_CLIENT_LEGACY_H */