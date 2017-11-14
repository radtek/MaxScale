/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2020-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "maxscale/mock/dcb.hh"

namespace
{

void initialize_dcb(DCB* pDcb)
{
    memset(pDcb, 0, sizeof(DCB));

    pDcb->dcb_chk_top = CHK_NUM_DCB;
    pDcb->fd = DCBFD_CLOSED;
    pDcb->state = DCB_STATE_ALLOC;
    pDcb->ssl_state = SSL_HANDSHAKE_UNKNOWN;
    pDcb->dcb_chk_tail = CHK_NUM_DCB;
}

}

namespace maxscale
{

namespace mock
{

Dcb::Dcb(MXS_SESSION* pSession,
         const char* zUser,
         const char* zHost)
    : m_user(zUser)
    , m_host(zHost)
{
    DCB* pDcb = this;
    initialize_dcb(this);

    pDcb->session = pSession;
    pDcb->remote = const_cast<char*>(zHost);
    pDcb->user = const_cast<char*>(zUser);
}

Dcb::~Dcb()
{
}

}

}
