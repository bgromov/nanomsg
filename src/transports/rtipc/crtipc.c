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

#include "crtipc.h"
#include "srtipc.h"

#include "../../aio/fsm.h"
#include "../../aio/usock.h"

#include "../utils/backoff.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/fast.h"
#include "../../utils/attr.h"

#include <string.h>
#include <unistd.h>
#include <sys/un.h>

#define NN_CRTIPC_STATE_IDLE 1
#define NN_CRTIPC_STATE_CONNECTING 2
#define NN_CRTIPC_STATE_ACTIVE 3
#define NN_CRTIPC_STATE_STOPPING_SRTIPC 4
#define NN_CRTIPC_STATE_STOPPING_USOCK 5
#define NN_CRTIPC_STATE_WAITING 6
#define NN_CRTIPC_STATE_STOPPING_BACKOFF 7
#define NN_CRTIPC_STATE_STOPPING_SRTIPC_FINAL 8
#define NN_CRTIPC_STATE_STOPPING 9

#define NN_CRTIPC_SRC_USOCK 1
#define NN_CRTIPC_SRC_RECONNECT_TIMER 2
#define NN_CRTIPC_SRC_SRTIPC 3

struct nn_crtipc {

    /*  The state machine. */
    struct nn_fsm fsm;
    int state;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct nn_epbase epbase;

    /*  The underlying RTIPC socket. */
    struct nn_usock usock;

    /*  Used to wait before retrying to connect. */
    struct nn_backoff retry;

    /*  State machine that handles the active part of the connection
        lifetime. */
    struct nn_srtipc srtipc;
};

/*  nn_epbase virtual interface implementation. */
static void nn_crtipc_stop (struct nn_epbase *self);
static void nn_crtipc_destroy (struct nn_epbase *self);
const struct nn_epbase_vfptr nn_crtipc_epbase_vfptr = {
    nn_crtipc_stop,
    nn_crtipc_destroy
};

/*  Private functions. */
static void nn_crtipc_handler (struct nn_fsm *self, int src, int type,
    void *srcptr);
static void nn_crtipc_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr);
static void nn_crtipc_start_connecting (struct nn_crtipc *self);

int nn_crtipc_create (void *hint, struct nn_epbase **epbase)
{
    struct nn_crtipc *self;
    int reconnect_ivl;
    int reconnect_ivl_max;
    size_t sz;

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (struct nn_crtipc), "crtipc");
    alloc_assert (self);

    /*  Initialise the structure. */
    nn_epbase_init (&self->epbase, &nn_crtipc_epbase_vfptr, hint);
    nn_fsm_init_root (&self->fsm, nn_crtipc_handler, nn_crtipc_shutdown,
        nn_epbase_getctx (&self->epbase));
    self->state = NN_CRTIPC_STATE_IDLE;
    nn_usock_init (&self->usock, NN_CRTIPC_SRC_USOCK, &self->fsm);
    sz = sizeof (reconnect_ivl);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_RECONNECT_IVL,
        &reconnect_ivl, &sz);
    nn_assert (sz == sizeof (reconnect_ivl));
    sz = sizeof (reconnect_ivl_max);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_RECONNECT_IVL_MAX,
        &reconnect_ivl_max, &sz);
    nn_assert (sz == sizeof (reconnect_ivl_max));
    if (reconnect_ivl_max == 0)
        reconnect_ivl_max = reconnect_ivl;
    nn_backoff_init (&self->retry, NN_CRTIPC_SRC_RECONNECT_TIMER,
        reconnect_ivl, reconnect_ivl_max, &self->fsm);
    nn_srtipc_init (&self->srtipc, NN_CRTIPC_SRC_SRTIPC, &self->epbase, &self->fsm);

    /*  Start the state machine. */
    nn_fsm_start (&self->fsm);

    /*  Return the base class as an out parameter. */
    *epbase = &self->epbase;

    return 0;
}

static void nn_crtipc_stop (struct nn_epbase *self)
{
    struct nn_crtipc *crtipc;

    crtipc = nn_cont (self, struct nn_crtipc, epbase);

    nn_fsm_stop (&crtipc->fsm);
}

static void nn_crtipc_destroy (struct nn_epbase *self)
{
    struct nn_crtipc *crtipc;

    crtipc = nn_cont (self, struct nn_crtipc, epbase);

    nn_srtipc_term (&crtipc->srtipc);
    nn_backoff_term (&crtipc->retry);
    nn_usock_term (&crtipc->usock);
    nn_fsm_term (&crtipc->fsm);
    nn_epbase_term (&crtipc->epbase);

    nn_free (crtipc);
}

static void nn_crtipc_shutdown (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    struct nn_crtipc *crtipc;

    crtipc = nn_cont (self, struct nn_crtipc, fsm);

    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {
        if (!nn_srtipc_isidle (&crtipc->srtipc)) {
            nn_epbase_stat_increment (&crtipc->epbase,
                NN_STAT_DROPPED_CONNECTIONS, 1);
            nn_srtipc_stop (&crtipc->srtipc);
        }
        crtipc->state = NN_CRTIPC_STATE_STOPPING_SRTIPC_FINAL;
    }
    if (nn_slow (crtipc->state == NN_CRTIPC_STATE_STOPPING_SRTIPC_FINAL)) {
        if (!nn_srtipc_isidle (&crtipc->srtipc))
            return;
        nn_backoff_stop (&crtipc->retry);
        nn_usock_stop (&crtipc->usock);
        crtipc->state = NN_CRTIPC_STATE_STOPPING;
    }
    if (nn_slow (crtipc->state == NN_CRTIPC_STATE_STOPPING)) {
        if (!nn_backoff_isidle (&crtipc->retry) ||
              !nn_usock_isidle (&crtipc->usock))
            return;
        crtipc->state = NN_CRTIPC_STATE_IDLE;
        nn_fsm_stopped_noevent (&crtipc->fsm);
        nn_epbase_stopped (&crtipc->epbase);
        return;
    }

    nn_fsm_bad_state(crtipc->state, src, type);
}

static void nn_crtipc_handler (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    struct nn_crtipc *crtipc;

    crtipc = nn_cont (self, struct nn_crtipc, fsm);

    switch (crtipc->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  The state machine wasn't yet started.                                     */
/******************************************************************************/
    case NN_CRTIPC_STATE_IDLE:
        switch (src) {

        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:
                nn_crtipc_start_connecting (crtipc);
                return;
            default:
                nn_fsm_bad_action (crtipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (crtipc->state, src, type);
        }

/******************************************************************************/
/*  CONNECTING state.                                                         */
/*  Non-blocking connect is under way.                                        */
/******************************************************************************/
    case NN_CRTIPC_STATE_CONNECTING:
        switch (src) {

        case NN_CRTIPC_SRC_USOCK:
            switch (type) {
            case NN_USOCK_CONNECTED:
                nn_srtipc_start (&crtipc->srtipc, &crtipc->usock);
                crtipc->state = NN_CRTIPC_STATE_ACTIVE;
                nn_epbase_stat_increment (&crtipc->epbase,
                    NN_STAT_INPROGRESS_CONNECTIONS, -1);
                nn_epbase_stat_increment (&crtipc->epbase,
                    NN_STAT_ESTABLISHED_CONNECTIONS, 1);
                nn_epbase_clear_error (&crtipc->epbase);
                return;
            case NN_USOCK_ERROR:
                nn_epbase_set_error (&crtipc->epbase,
                    nn_usock_geterrno (&crtipc->usock));
                nn_usock_stop (&crtipc->usock);
                crtipc->state = NN_CRTIPC_STATE_STOPPING_USOCK;
                nn_epbase_stat_increment (&crtipc->epbase,
                    NN_STAT_INPROGRESS_CONNECTIONS, -1);
                nn_epbase_stat_increment (&crtipc->epbase,
                    NN_STAT_CONNECT_ERRORS, 1);
                return;
            default:
                nn_fsm_bad_action (crtipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (crtipc->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  Connection is established and handled by the srtipc state machine.          */
/******************************************************************************/
    case NN_CRTIPC_STATE_ACTIVE:
        switch (src) {

        case NN_CRTIPC_SRC_SRTIPC:
            switch (type) {
            case NN_SRTIPC_ERROR:
                nn_srtipc_stop (&crtipc->srtipc);
                crtipc->state = NN_CRTIPC_STATE_STOPPING_SRTIPC;
                nn_epbase_stat_increment (&crtipc->epbase,
                    NN_STAT_BROKEN_CONNECTIONS, 1);
                return;
            default:
               nn_fsm_bad_action (crtipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (crtipc->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_SRTIPC state.                                                      */
/*  srtipc object was asked to stop but it haven't stopped yet.                 */
/******************************************************************************/
    case NN_CRTIPC_STATE_STOPPING_SRTIPC:
        switch (src) {

        case NN_CRTIPC_SRC_SRTIPC:
            switch (type) {
            case NN_USOCK_SHUTDOWN:
                return;
            case NN_SRTIPC_STOPPED:
                nn_usock_stop (&crtipc->usock);
                crtipc->state = NN_CRTIPC_STATE_STOPPING_USOCK;
                return;
            default:
                nn_fsm_bad_action (crtipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (crtipc->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_USOCK state.                                                     */
/*  usock object was asked to stop but it haven't stopped yet.                */
/******************************************************************************/
    case NN_CRTIPC_STATE_STOPPING_USOCK:
        switch (src) {

        case NN_CRTIPC_SRC_USOCK:
            switch (type) {
            case NN_USOCK_SHUTDOWN:
                return;
            case NN_USOCK_STOPPED:
                nn_backoff_start (&crtipc->retry);
                crtipc->state = NN_CRTIPC_STATE_WAITING;
                return;
            default:
                nn_fsm_bad_action (crtipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (crtipc->state, src, type);
        }

/******************************************************************************/
/*  WAITING state.                                                            */
/*  Waiting before re-connection is attempted. This way we won't overload     */
/*  the system by continuous re-connection attemps.                           */
/******************************************************************************/
    case NN_CRTIPC_STATE_WAITING:
        switch (src) {

        case NN_CRTIPC_SRC_RECONNECT_TIMER:
            switch (type) {
            case NN_BACKOFF_TIMEOUT:
                nn_backoff_stop (&crtipc->retry);
                crtipc->state = NN_CRTIPC_STATE_STOPPING_BACKOFF;
                return;
            default:
                nn_fsm_bad_action (crtipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (crtipc->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_BACKOFF state.                                                   */
/*  backoff object was asked to stop, but it haven't stopped yet.             */
/******************************************************************************/
    case NN_CRTIPC_STATE_STOPPING_BACKOFF:
        switch (src) {

        case NN_CRTIPC_SRC_RECONNECT_TIMER:
            switch (type) {
            case NN_BACKOFF_STOPPED:
                nn_crtipc_start_connecting (crtipc);
                return;
            default:
                nn_fsm_bad_action (crtipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (crtipc->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_fsm_bad_state (crtipc->state, src, type);
    }
}

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

static void nn_crtipc_start_connecting (struct nn_crtipc *self)
{
    int rc;
    struct sockaddr_storage ss;
    struct sockaddr_un *un;
    const char *addr;
    int val;
    size_t sz;

    /*  Try to start the underlying socket. */
    rc = nn_usock_start (&self->usock, AF_UNIX, SOCK_STREAM, 0);
    if (nn_slow (rc < 0)) {
        nn_backoff_start (&self->retry);
        self->state = NN_CRTIPC_STATE_WAITING;
        return;
    }

    /*  Set the relevant socket options. */
    sz = sizeof (val);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_SNDBUF, &val, &sz);
    nn_assert (sz == sizeof (val));
    nn_usock_setsockopt (&self->usock, SOL_SOCKET, SO_SNDBUF,
        &val, sizeof (val));
    sz = sizeof (val);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_RCVBUF, &val, &sz);
    nn_assert (sz == sizeof (val));
    nn_usock_setsockopt (&self->usock, SOL_SOCKET, SO_RCVBUF,
        &val, sizeof (val));

    /*  Create the RTIPC address from the address string. */
    addr = nn_epbase_getaddr (&self->epbase);
    memset (&ss, 0, sizeof (ss));
    un = (struct sockaddr_un*) &ss;
    nn_assert (strlen (addr) < sizeof (un->sun_path));
    ss.ss_family = AF_UNIX;
    strncpy (un->sun_path, addr, sizeof (un->sun_path));

    /*  Start connecting. */
    nn_usock_connect (&self->usock, (struct sockaddr*) &ss,
        sizeof (struct sockaddr_un));
    self->state  = NN_CRTIPC_STATE_CONNECTING;

    nn_epbase_stat_increment (&self->epbase,
        NN_STAT_INPROGRESS_CONNECTIONS, 1);
}

#endif

