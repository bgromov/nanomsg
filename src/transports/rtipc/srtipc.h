/*
    Copyright (c) 2013 250bpm s.r.o.  All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#ifndef NN_SRTIPC_INCLUDED
#define NN_SRTIPC_INCLUDED

#if !defined NN_HAVE_WINDOWS

#include "../../transport.h"

#include "../../aio/fsm.h"
#include "../../aio/usock.h"

#include "../utils/streamhdr.h"

#include "../../utils/msg.h"

/*  This state machine handles RTIPC connection from the point where it is
    established to the point when it is broken. */

#define NN_SRTIPC_ERROR 1
#define NN_SRTIPC_STOPPED 2

struct nn_srtipc {

    /*  The state machine. */
    struct nn_fsm fsm;
    int state;

    /*  The undelrying socket. */
    struct nn_usock *usock;

    /*  Child state machine to do protocol header exchange. */
    struct nn_streamhdr streamhdr;

    /*  The original owner of the underlying socket. */
    struct nn_fsm_owner usock_owner;

    /*  Pipe connecting this RTIPC connection to the nanomsg core. */
    struct nn_pipebase pipebase;

    /*  State of inbound state machine. */
    int instate;

    /*  Buffer used to store the header of incoming message. */
    uint8_t inhdr [9];

    /*  Message being received at the moment. */
    struct nn_msg inmsg;

    /*  State of the outbound state machine. */
    int outstate;

    /*  Buffer used to store the header of outgoing message. */
    uint8_t outhdr [9];

    /*  Message being sent at the moment. */
    struct nn_msg outmsg;

    /*  Event raised when the state machine ends. */
    struct nn_fsm_event done;
};

void nn_srtipc_init (struct nn_srtipc *self, int src,
    struct nn_epbase *epbase, struct nn_fsm *owner);
void nn_srtipc_term (struct nn_srtipc *self);

int nn_srtipc_isidle (struct nn_srtipc *self);
void nn_srtipc_start (struct nn_srtipc *self, struct nn_usock *usock);
void nn_srtipc_stop (struct nn_srtipc *self);

#endif

#endif
