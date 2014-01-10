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

#if !defined NN_HAVE_WINDOWS

#include "srtipc.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/wire.h"
#include "../../utils/int.h"
#include "../../utils/attr.h"

/*  Types of messages passed via RTIPC transport. */
#define NN_SRTIPC_MSG_NORMAL 1
#define NN_SRTIPC_MSG_SHMEM 2

/*  States of the object as a whole. */
#define NN_SRTIPC_STATE_IDLE 1
#define NN_SRTIPC_STATE_PROTOHDR 2
#define NN_SRTIPC_STATE_STOPPING_STREAMHDR 3
#define NN_SRTIPC_STATE_ACTIVE 4
#define NN_SRTIPC_STATE_SHUTTING_DOWN 5
#define NN_SRTIPC_STATE_DONE 6
#define NN_SRTIPC_STATE_STOPPING 7

/*  Subordinated srcptr objects. */
#define NN_SRTIPC_SRC_USOCK 1
#define NN_SRTIPC_SRC_STREAMHDR 2

/*  Possible states of the inbound part of the object. */
#define NN_SRTIPC_INSTATE_HDR 1
#define NN_SRTIPC_INSTATE_BODY 2
#define NN_SRTIPC_INSTATE_HASMSG 3

/*  Possible states of the outbound part of the object. */
#define NN_SRTIPC_OUTSTATE_IDLE 1
#define NN_SRTIPC_OUTSTATE_SENDING 2

/*  Stream is a special type of pipe. Implementation of the virtual pipe API. */
static int nn_srtipc_send (struct nn_pipebase *self, struct nn_msg *msg);
static int nn_srtipc_recv (struct nn_pipebase *self, struct nn_msg *msg);
const struct nn_pipebase_vfptr nn_srtipc_pipebase_vfptr = {
    nn_srtipc_send,
    nn_srtipc_recv
};

/*  Private functions. */
static void nn_srtipc_handler (struct nn_fsm *self, int src, int type,
    void *srcptr);
static void nn_srtipc_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr);

void nn_srtipc_init (struct nn_srtipc *self, int src,
    struct nn_epbase *epbase, struct nn_fsm *owner)
{
    nn_fsm_init (&self->fsm, nn_srtipc_handler, nn_srtipc_shutdown,
        src, self, owner);
    self->state = NN_SRTIPC_STATE_IDLE;
    nn_streamhdr_init (&self->streamhdr, NN_SRTIPC_SRC_STREAMHDR, &self->fsm);
    self->usock = NULL;
    self->usock_owner.src = -1;
    self->usock_owner.fsm = NULL;
    nn_pipebase_init (&self->pipebase, &nn_srtipc_pipebase_vfptr, epbase);
    self->instate = -1;
    nn_msg_init (&self->inmsg, 0);
    self->outstate = -1;
    nn_msg_init (&self->outmsg, 0);
    nn_fsm_event_init (&self->done);
}

void nn_srtipc_term (struct nn_srtipc *self)
{
    nn_assert_state (self, NN_SRTIPC_STATE_IDLE);

    nn_fsm_event_term (&self->done);
    nn_msg_term (&self->outmsg);
    nn_msg_term (&self->inmsg);
    nn_pipebase_term (&self->pipebase);
    nn_streamhdr_term (&self->streamhdr);
    nn_fsm_term (&self->fsm);
}

int nn_srtipc_isidle (struct nn_srtipc *self)
{
    return nn_fsm_isidle (&self->fsm);
}

void nn_srtipc_start (struct nn_srtipc *self, struct nn_usock *usock)
{
    /*  Take ownership of the underlying socket. */
    nn_assert (self->usock == NULL && self->usock_owner.fsm == NULL);
    self->usock_owner.src = NN_SRTIPC_SRC_USOCK;
    self->usock_owner.fsm = &self->fsm;
    nn_usock_swap_owner (usock, &self->usock_owner);
    self->usock = usock;

    /*  Launch the state machine. */
    nn_fsm_start (&self->fsm);
}

void nn_srtipc_stop (struct nn_srtipc *self)
{
    nn_fsm_stop (&self->fsm);
}

static int nn_srtipc_send (struct nn_pipebase *self, struct nn_msg *msg)
{
    struct nn_srtipc *srtipc;
    struct nn_iovec iov [3];

    srtipc = nn_cont (self, struct nn_srtipc, pipebase);

    nn_assert_state (srtipc, NN_SRTIPC_STATE_ACTIVE);
    nn_assert (srtipc->outstate == NN_SRTIPC_OUTSTATE_IDLE);

    /*  Move the message to the local storage. */
    nn_msg_term (&srtipc->outmsg);
    nn_msg_mv (&srtipc->outmsg, msg);

    /*  Serialise the message header. */
    srtipc->outhdr [0] = NN_SRTIPC_MSG_NORMAL;
    nn_putll (srtipc->outhdr + 1, nn_chunkref_size (&srtipc->outmsg.hdr) +
        nn_chunkref_size (&srtipc->outmsg.body));

    /*  Start async sending. */
    iov [0].iov_base = srtipc->outhdr;
    iov [0].iov_len = sizeof (srtipc->outhdr);
    iov [1].iov_base = nn_chunkref_data (&srtipc->outmsg.hdr);
    iov [1].iov_len = nn_chunkref_size (&srtipc->outmsg.hdr);
    iov [2].iov_base = nn_chunkref_data (&srtipc->outmsg.body);
    iov [2].iov_len = nn_chunkref_size (&srtipc->outmsg.body);
    nn_usock_send (srtipc->usock, iov, 3);

    srtipc->outstate = NN_SRTIPC_OUTSTATE_SENDING;

    return 0;
}

static int nn_srtipc_recv (struct nn_pipebase *self, struct nn_msg *msg)
{
    struct nn_srtipc *srtipc;

    srtipc = nn_cont (self, struct nn_srtipc, pipebase);

    nn_assert_state (srtipc, NN_SRTIPC_STATE_ACTIVE);
    nn_assert (srtipc->instate == NN_SRTIPC_INSTATE_HASMSG);

    /*  Move received message to the user. */
    nn_msg_mv (msg, &srtipc->inmsg);
    nn_msg_init (&srtipc->inmsg, 0);

    /*  Start receiving new message. */
    srtipc->instate = NN_SRTIPC_INSTATE_HDR;
    nn_usock_recv (srtipc->usock, srtipc->inhdr, sizeof (srtipc->inhdr));

    return 0;
}

static void nn_srtipc_shutdown (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    struct nn_srtipc *srtipc;

    srtipc = nn_cont (self, struct nn_srtipc, fsm);

    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {
        nn_pipebase_stop (&srtipc->pipebase);
        nn_streamhdr_stop (&srtipc->streamhdr);
        srtipc->state = NN_SRTIPC_STATE_STOPPING;
    }
    if (nn_slow (srtipc->state == NN_SRTIPC_STATE_STOPPING)) {
        if (nn_streamhdr_isidle (&srtipc->streamhdr)) {
            nn_usock_swap_owner (srtipc->usock, &srtipc->usock_owner);
            srtipc->usock = NULL;
            srtipc->usock_owner.src = -1;
            srtipc->usock_owner.fsm = NULL;
            srtipc->state = NN_SRTIPC_STATE_IDLE;
            nn_fsm_stopped (&srtipc->fsm, NN_SRTIPC_STOPPED);
            return;
        }
        return;
    }

    nn_fsm_bad_state(srtipc->state, src, type);
}

static void nn_srtipc_handler (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    int rc;
    struct nn_srtipc *srtipc;
    uint64_t size;

    srtipc = nn_cont (self, struct nn_srtipc, fsm);


    switch (srtipc->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_SRTIPC_STATE_IDLE:
        switch (src) {

        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:
                nn_streamhdr_start (&srtipc->streamhdr, srtipc->usock,
                    &srtipc->pipebase);
                srtipc->state = NN_SRTIPC_STATE_PROTOHDR;
                return;
            default:
                nn_fsm_bad_action (srtipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (srtipc->state, src, type);
        }

/******************************************************************************/
/*  PROTOHDR state.                                                           */
/******************************************************************************/
    case NN_SRTIPC_STATE_PROTOHDR:
        switch (src) {

        case NN_SRTIPC_SRC_STREAMHDR:
            switch (type) {
            case NN_STREAMHDR_OK:

                /*  Before moving to the active state stop the streamhdr
                    state machine. */
                nn_streamhdr_stop (&srtipc->streamhdr);
                srtipc->state = NN_SRTIPC_STATE_STOPPING_STREAMHDR;
                return;

            case NN_STREAMHDR_ERROR:

                /* Raise the error and move directly to the DONE state.
                   streamhdr object will be stopped later on. */
                srtipc->state = NN_SRTIPC_STATE_DONE;
                nn_fsm_raise (&srtipc->fsm, &srtipc->done, NN_SRTIPC_ERROR);
                return;

            default:
                nn_fsm_bad_action (srtipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (srtipc->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_STREAMHDR state.                                                 */
/******************************************************************************/
    case NN_SRTIPC_STATE_STOPPING_STREAMHDR:
        switch (src) {

        case NN_SRTIPC_SRC_STREAMHDR:
            switch (type) {
            case NN_STREAMHDR_STOPPED:

                 /*  Start the pipe. */
                 rc = nn_pipebase_start (&srtipc->pipebase);
                 if (nn_slow (rc < 0)) {
                    srtipc->state = NN_SRTIPC_STATE_DONE;
                    nn_fsm_raise (&srtipc->fsm, &srtipc->done, NN_SRTIPC_ERROR);
                    return;
                 }

                 /*  Start receiving a message in asynchronous manner. */
                 srtipc->instate = NN_SRTIPC_INSTATE_HDR;
                 nn_usock_recv (srtipc->usock, &srtipc->inhdr,
                     sizeof (srtipc->inhdr));

                 /*  Mark the pipe as available for sending. */
                 srtipc->outstate = NN_SRTIPC_OUTSTATE_IDLE;

                 srtipc->state = NN_SRTIPC_STATE_ACTIVE;
                 return;

            default:
                nn_fsm_bad_action (srtipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (srtipc->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case NN_SRTIPC_STATE_ACTIVE:
        switch (src) {

        case NN_SRTIPC_SRC_USOCK:
            switch (type) {
            case NN_USOCK_SENT:

                /*  The message is now fully sent. */
                nn_assert (srtipc->outstate == NN_SRTIPC_OUTSTATE_SENDING);
                srtipc->outstate = NN_SRTIPC_OUTSTATE_IDLE;
                nn_msg_term (&srtipc->outmsg);
                nn_msg_init (&srtipc->outmsg, 0);
                nn_pipebase_sent (&srtipc->pipebase);
                return;

            case NN_USOCK_RECEIVED:

                switch (srtipc->instate) {
                case NN_SRTIPC_INSTATE_HDR:

                    /*  Message header was received. Allocate memory for the
                        message. */
                    nn_assert (srtipc->inhdr [0] == NN_SRTIPC_MSG_NORMAL);
                    size = nn_getll (srtipc->inhdr + 1);
                    nn_msg_term (&srtipc->inmsg);
                    nn_msg_init (&srtipc->inmsg, (size_t) size);

                    /*  Special case when size of the message body is 0. */
                    if (!size) {
                        srtipc->instate = NN_SRTIPC_INSTATE_HASMSG;
                        nn_pipebase_received (&srtipc->pipebase);
                        return;
                    }

                    /*  Start receiving the message body. */
                    srtipc->instate = NN_SRTIPC_INSTATE_BODY;
                    nn_usock_recv (srtipc->usock,
                        nn_chunkref_data (&srtipc->inmsg.body), (size_t) size);

                    return;

                case NN_SRTIPC_INSTATE_BODY:

                    /*  Message body was received. Notify the owner that it
                        can receive it. */
                    srtipc->instate = NN_SRTIPC_INSTATE_HASMSG;
                    nn_pipebase_received (&srtipc->pipebase);

                    return;

                default:
                    nn_assert (0);
                }

            case NN_USOCK_SHUTDOWN:
                nn_pipebase_stop (&srtipc->pipebase);
                srtipc->state = NN_SRTIPC_STATE_SHUTTING_DOWN;
                return;

            case NN_USOCK_ERROR:
                nn_pipebase_stop (&srtipc->pipebase);
                srtipc->state = NN_SRTIPC_STATE_DONE;
                nn_fsm_raise (&srtipc->fsm, &srtipc->done, NN_SRTIPC_ERROR);
                return;


            default:
                nn_fsm_bad_action (srtipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (srtipc->state, src, type);
        }

/******************************************************************************/
/*  SHUTTING_DOWN state.                                                      */
/*  The underlying connection is closed. We are just waiting that underlying  */
/*  usock being closed                                                        */
/******************************************************************************/
    case NN_SRTIPC_STATE_SHUTTING_DOWN:
        switch (src) {

        case NN_SRTIPC_SRC_USOCK:
            switch (type) {
            case NN_USOCK_ERROR:
                srtipc->state = NN_SRTIPC_STATE_DONE;
                nn_fsm_raise (&srtipc->fsm, &srtipc->done, NN_SRTIPC_ERROR);
                return;
            default:
                nn_fsm_bad_action (srtipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (srtipc->state, src, type);
        }

/******************************************************************************/
/*  DONE state.                                                               */
/*  The underlying connection is closed. There's nothing that can be done in  */
/*  this state except stopping the object.                                    */
/******************************************************************************/
    case NN_SRTIPC_STATE_DONE:
        nn_fsm_bad_source (srtipc->state, src, type);


/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_fsm_bad_state (srtipc->state, src, type);
    }
}

#endif

