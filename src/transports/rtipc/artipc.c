/*
    Copyright (c) 2012-2013 250bpm s.r.o.  All rights reserved.

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

#include "artipc.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/attr.h"

#define NN_ARTIPC_STATE_IDLE 1
#define NN_ARTIPC_STATE_ACCEPTING 2
#define NN_ARTIPC_STATE_ACTIVE 3
#define NN_ARTIPC_STATE_STOPPING_SRTIPC 4
#define NN_ARTIPC_STATE_STOPPING_USOCK 5
#define NN_ARTIPC_STATE_DONE 6
#define NN_ARTIPC_STATE_STOPPING_SRTIPC_FINAL 7
#define NN_ARTIPC_STATE_STOPPING 8

#define NN_ARTIPC_SRC_USOCK 1
#define NN_ARTIPC_SRC_SRTIPC 2
#define NN_ARTIPC_SRC_LISTENER 3

/*  Private functions. */
static void nn_artipc_handler (struct nn_fsm *self, int src, int type,
   void *srcptr);
static void nn_artipc_shutdown (struct nn_fsm *self, int src, int type,
   void *srcptr);

void nn_artipc_init (struct nn_artipc *self, int src,
    struct nn_epbase *epbase, struct nn_fsm *owner)
{
    nn_fsm_init (&self->fsm, nn_artipc_handler, nn_artipc_shutdown,
        src, self, owner);
    self->state = NN_ARTIPC_STATE_IDLE;
    self->epbase = epbase;
    nn_usock_init (&self->usock, NN_ARTIPC_SRC_USOCK, &self->fsm);
    self->listener = NULL;
    self->listener_owner.src = -1;
    self->listener_owner.fsm = NULL;
    nn_srtipc_init (&self->srtipc, NN_ARTIPC_SRC_SRTIPC, epbase, &self->fsm);
    nn_fsm_event_init (&self->accepted);
    nn_fsm_event_init (&self->done);
    nn_list_item_init (&self->item);
}

void nn_artipc_term (struct nn_artipc *self)
{
    nn_assert_state (self, NN_ARTIPC_STATE_IDLE);

    nn_list_item_term (&self->item);
    nn_fsm_event_term (&self->done);
    nn_fsm_event_term (&self->accepted);
    nn_srtipc_term (&self->srtipc);
    nn_usock_term (&self->usock);
    nn_fsm_term (&self->fsm);
}

int nn_artipc_isidle (struct nn_artipc *self)
{
    return nn_fsm_isidle (&self->fsm);
}

void nn_artipc_start (struct nn_artipc *self, struct nn_usock *listener)
{
    nn_assert_state (self, NN_ARTIPC_STATE_IDLE);

    /*  Take ownership of the listener socket. */
    self->listener = listener;
    self->listener_owner.src = NN_ARTIPC_SRC_LISTENER;
    self->listener_owner.fsm = &self->fsm;
    nn_usock_swap_owner (listener, &self->listener_owner);

    /*  Start the state machine. */
    nn_fsm_start (&self->fsm);
}

void nn_artipc_stop (struct nn_artipc *self)
{
    nn_fsm_stop (&self->fsm);
}

static void nn_artipc_shutdown (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    struct nn_artipc *artipc;

    artipc = nn_cont (self, struct nn_artipc, fsm);

    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {
        if (!nn_srtipc_isidle (&artipc->srtipc)) {
            nn_epbase_stat_increment (artipc->epbase,
                NN_STAT_DROPPED_CONNECTIONS, 1);
            nn_srtipc_stop (&artipc->srtipc);
        }
        artipc->state = NN_ARTIPC_STATE_STOPPING_SRTIPC_FINAL;
    }
    if (nn_slow (artipc->state == NN_ARTIPC_STATE_STOPPING_SRTIPC_FINAL)) {
        if (!nn_srtipc_isidle (&artipc->srtipc))
            return;
        nn_usock_stop (&artipc->usock);
        artipc->state = NN_ARTIPC_STATE_STOPPING;
    }
    if (nn_slow (artipc->state == NN_ARTIPC_STATE_STOPPING)) {
        if (!nn_usock_isidle (&artipc->usock))
            return;
       if (artipc->listener) {
            nn_assert (artipc->listener_owner.fsm);
            nn_usock_swap_owner (artipc->listener, &artipc->listener_owner);
            artipc->listener = NULL;
            artipc->listener_owner.src = -1;
            artipc->listener_owner.fsm = NULL;
        }
        artipc->state = NN_ARTIPC_STATE_IDLE;
        nn_fsm_stopped (&artipc->fsm, NN_ARTIPC_STOPPED);
        return;
    }

    nn_fsm_bad_state(artipc->state, src, type);
}

static void nn_artipc_handler (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    struct nn_artipc *artipc;
    int val;
    size_t sz;

    artipc = nn_cont (self, struct nn_artipc, fsm);

    switch (artipc->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  The state machine wasn't yet started.                                     */
/******************************************************************************/
    case NN_ARTIPC_STATE_IDLE:
        switch (src) {

        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:
                nn_usock_accept (&artipc->usock, artipc->listener);
                artipc->state = NN_ARTIPC_STATE_ACCEPTING;
                return;
            default:
                nn_fsm_bad_action (artipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (artipc->state, src, type);
        }

/******************************************************************************/
/*  ACCEPTING state.                                                          */
/*  Waiting for incoming connection.                                          */
/******************************************************************************/
    case NN_ARTIPC_STATE_ACCEPTING:
        switch (src) {

        case NN_ARTIPC_SRC_USOCK:
            switch (type) {
            case NN_USOCK_ACCEPTED:
                nn_epbase_clear_error (artipc->epbase);

                /*  Set the relevant socket options. */
                sz = sizeof (val);
                nn_epbase_getopt (artipc->epbase, NN_SOL_SOCKET, NN_SNDBUF,
                    &val, &sz);
                nn_assert (sz == sizeof (val));
                nn_usock_setsockopt (&artipc->usock, SOL_SOCKET, SO_SNDBUF,
                    &val, sizeof (val));
                sz = sizeof (val);
                nn_epbase_getopt (artipc->epbase, NN_SOL_SOCKET, NN_RCVBUF,
                    &val, &sz);
                nn_assert (sz == sizeof (val));
                nn_usock_setsockopt (&artipc->usock, SOL_SOCKET, SO_RCVBUF,
                    &val, sizeof (val));

                /*  Return ownership of the listening socket to the parent. */
                nn_usock_swap_owner (artipc->listener, &artipc->listener_owner);
                artipc->listener = NULL;
                artipc->listener_owner.src = -1;
                artipc->listener_owner.fsm = NULL;
                nn_fsm_raise (&artipc->fsm, &artipc->accepted, NN_ARTIPC_ACCEPTED);

                /*  Start the srtipc state machine. */
                nn_usock_activate (&artipc->usock);
                nn_srtipc_start (&artipc->srtipc, &artipc->usock);
                artipc->state = NN_ARTIPC_STATE_ACTIVE;

                nn_epbase_stat_increment (artipc->epbase,
                    NN_STAT_ACCEPTED_CONNECTIONS, 1);

                return;

            default:
                nn_fsm_bad_action (artipc->state, src, type);
            }

        case NN_ARTIPC_SRC_LISTENER:
            switch (type) {
            case NN_USOCK_ACCEPT_ERROR:
                nn_epbase_set_error (artipc->epbase,
                    nn_usock_geterrno (artipc->listener));
                nn_epbase_stat_increment (artipc->epbase,
                    NN_STAT_ACCEPT_ERRORS, 1);
                nn_usock_accept (&artipc->usock, artipc->listener);

                return;

            default:
                nn_fsm_bad_action (artipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (artipc->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case NN_ARTIPC_STATE_ACTIVE:
        switch (src) {

        case NN_ARTIPC_SRC_SRTIPC:
            switch (type) {
            case NN_SRTIPC_ERROR:
                nn_srtipc_stop (&artipc->srtipc);
                artipc->state = NN_ARTIPC_STATE_STOPPING_SRTIPC;
                nn_epbase_stat_increment (artipc->epbase,
                    NN_STAT_BROKEN_CONNECTIONS, 1);
                return;
            default:
                nn_fsm_bad_action (artipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (artipc->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_SRTIPC state.                                                      */
/******************************************************************************/
    case NN_ARTIPC_STATE_STOPPING_SRTIPC:
        switch (src) {

        case NN_ARTIPC_SRC_SRTIPC:
            switch (type) {
            case NN_USOCK_SHUTDOWN:
                return;
            case NN_SRTIPC_STOPPED:
                nn_usock_stop (&artipc->usock);
                artipc->state = NN_ARTIPC_STATE_STOPPING_USOCK;
                return;
            default:
                nn_fsm_bad_action (artipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (artipc->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_USOCK state.                                                      */
/******************************************************************************/
    case NN_ARTIPC_STATE_STOPPING_USOCK:
        switch (src) {

        case NN_ARTIPC_SRC_USOCK:
            switch (type) {
            case NN_USOCK_SHUTDOWN:
                return;
            case NN_USOCK_STOPPED:
                nn_fsm_raise (&artipc->fsm, &artipc->done, NN_ARTIPC_ERROR);
                artipc->state = NN_ARTIPC_STATE_DONE;
                return;
            default:
                nn_fsm_bad_action (artipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (artipc->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_fsm_bad_state (artipc->state, src, type);
    }
}

#endif

