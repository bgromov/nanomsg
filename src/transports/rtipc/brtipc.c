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

#include "brtipc.h"
#include "artipc.h"

#include "../../aio/fsm.h"
#include "../../aio/usock.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/fast.h"

#include <string.h>
#include <unistd.h>
#include <sys/un.h>

#define NN_BRTIPC_BACKLOG 10

#define NN_BRTIPC_STATE_IDLE 1
#define NN_BRTIPC_STATE_ACTIVE 2
#define NN_BRTIPC_STATE_STOPPING_ARTIPC 3
#define NN_BRTIPC_STATE_STOPPING_USOCK 4
#define NN_BRTIPC_STATE_STOPPING_ARTIPCS 5

#define NN_BRTIPC_SRC_USOCK 1
#define NN_BRTIPC_SRC_ARTIPC 2

struct nn_brtipc {

    /*  The state machine. */
    struct nn_fsm fsm;
    int state;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct nn_epbase epbase;

    /*  The underlying listening RTIPC socket. */
    struct nn_usock usock;

    /*  The connection being accepted at the moment. */
    struct nn_artipc *artipc;

    /*  List of accepted connections. */
    struct nn_list artipcs;
};

/*  nn_epbase virtual interface implementation. */
static void nn_brtipc_stop (struct nn_epbase *self);
static void nn_brtipc_destroy (struct nn_epbase *self);
const struct nn_epbase_vfptr nn_brtipc_epbase_vfptr = {
    nn_brtipc_stop,
    nn_brtipc_destroy
};

/*  Private functions. */
static void nn_brtipc_handler (struct nn_fsm *self, int src, int type,
    void *srcptr);
static void nn_brtipc_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr);
static void nn_brtipc_start_listening (struct nn_brtipc *self);
static void nn_brtipc_start_accepting (struct nn_brtipc *self);

int nn_brtipc_create (void *hint, struct nn_epbase **epbase)
{
    struct nn_brtipc *self;

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (struct nn_brtipc), "brtipc");
    alloc_assert (self);

    /*  Initialise the structure. */
    nn_epbase_init (&self->epbase, &nn_brtipc_epbase_vfptr, hint);
    nn_fsm_init_root (&self->fsm, nn_brtipc_handler, nn_brtipc_shutdown,
        nn_epbase_getctx (&self->epbase));
    self->state = NN_BRTIPC_STATE_IDLE;
    nn_usock_init (&self->usock, NN_BRTIPC_SRC_USOCK, &self->fsm);
    self->artipc = NULL;
    nn_list_init (&self->artipcs);

    /*  Start the state machine. */
    nn_fsm_start (&self->fsm);

    /*  Return the base class as an out parameter. */
    *epbase = &self->epbase;

    return 0;
}

static void nn_brtipc_stop (struct nn_epbase *self)
{
    struct nn_brtipc *brtipc;

    brtipc = nn_cont (self, struct nn_brtipc, epbase);

    nn_fsm_stop (&brtipc->fsm);
}

static void nn_brtipc_destroy (struct nn_epbase *self)
{
    struct nn_brtipc *brtipc;

    brtipc = nn_cont (self, struct nn_brtipc, epbase);

    nn_assert_state (brtipc, NN_BRTIPC_STATE_IDLE);
    nn_list_term (&brtipc->artipcs);
    nn_assert (brtipc->artipc == NULL);
    nn_usock_term (&brtipc->usock);
    nn_epbase_term (&brtipc->epbase);
    nn_fsm_term (&brtipc->fsm);

    nn_free (brtipc);
}

static void nn_brtipc_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    struct nn_brtipc *brtipc;
    struct nn_list_item *it;
    struct nn_artipc *artipc;

    brtipc = nn_cont (self, struct nn_brtipc, fsm);

    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {
        nn_artipc_stop (brtipc->artipc);
        brtipc->state = NN_BRTIPC_STATE_STOPPING_ARTIPC;
    }
    if (nn_slow (brtipc->state == NN_BRTIPC_STATE_STOPPING_ARTIPC)) {
        if (!nn_artipc_isidle (brtipc->artipc))
            return;
        nn_artipc_term (brtipc->artipc);
        nn_free (brtipc->artipc);
        brtipc->artipc = NULL;
        nn_usock_stop (&brtipc->usock);
        brtipc->state = NN_BRTIPC_STATE_STOPPING_USOCK;
    }
    if (nn_slow (brtipc->state == NN_BRTIPC_STATE_STOPPING_USOCK)) {
       if (!nn_usock_isidle (&brtipc->usock))
            return;
        for (it = nn_list_begin (&brtipc->artipcs);
              it != nn_list_end (&brtipc->artipcs);
              it = nn_list_next (&brtipc->artipcs, it)) {
            artipc = nn_cont (it, struct nn_artipc, item);
            nn_artipc_stop (artipc);
        }
        brtipc->state = NN_BRTIPC_STATE_STOPPING_ARTIPCS;
        goto artipcs_stopping;
    }
    if (nn_slow (brtipc->state == NN_BRTIPC_STATE_STOPPING_ARTIPCS)) {
        nn_assert (src == NN_BRTIPC_SRC_ARTIPC && type == NN_ARTIPC_STOPPED);
        artipc = (struct nn_artipc *) srcptr;
        nn_list_erase (&brtipc->artipcs, &artipc->item);
        nn_artipc_term (artipc);
        nn_free (artipc);

        /*  If there are no more artipc state machines, we can stop the whole
            brtipc object. */
artipcs_stopping:
        if (nn_list_empty (&brtipc->artipcs)) {
            brtipc->state = NN_BRTIPC_STATE_IDLE;
            nn_fsm_stopped_noevent (&brtipc->fsm);
            nn_epbase_stopped (&brtipc->epbase);
            return;
        }

        return;
    }

    nn_fsm_bad_state(brtipc->state, src, type);
}

static void nn_brtipc_handler (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    struct nn_brtipc *brtipc;
    struct nn_artipc *artipc;

    brtipc = nn_cont (self, struct nn_brtipc, fsm);

    switch (brtipc->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_BRTIPC_STATE_IDLE:
        switch (src) {

        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:
                nn_brtipc_start_listening (brtipc);
                nn_brtipc_start_accepting (brtipc);
                brtipc->state = NN_BRTIPC_STATE_ACTIVE;
                return;
            default:
                nn_fsm_bad_action (brtipc->state, src, type);
            }

        default:
            nn_fsm_bad_source (brtipc->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  The execution is yielded to the artipc state machine in this state.       */
/******************************************************************************/
    case NN_BRTIPC_STATE_ACTIVE:
        if (srcptr == brtipc->artipc) {
            switch (type) {
            case NN_ARTIPC_ACCEPTED:

                /*  Move the newly created connection to the list of existing
                    connections. */
                nn_list_insert (&brtipc->artipcs, &brtipc->artipc->item,
                    nn_list_end (&brtipc->artipcs));
                brtipc->artipc = NULL;

                /*  Start waiting for a new incoming connection. */
                nn_brtipc_start_accepting (brtipc);

                return;

            default:
                nn_fsm_bad_action (brtipc->state, src, type);
            }
        }

        /*  For all remaining events we'll assume they are coming from one
            of remaining child artipc objects. */
        nn_assert (src == NN_BRTIPC_SRC_ARTIPC);
        artipc = (struct nn_artipc*) srcptr;
        switch (type) {
        case NN_ARTIPC_ERROR:
            nn_artipc_stop (artipc);
            return;
        case NN_ARTIPC_STOPPED:
            nn_list_erase (&brtipc->artipcs, &artipc->item);
            nn_artipc_term (artipc);
            nn_free (artipc);
            return;
        default:
            nn_fsm_bad_action (brtipc->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_fsm_bad_state (brtipc->state, src, type);
    }
}

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

static void nn_brtipc_start_listening (struct nn_brtipc *self)
{
    int rc;
    struct sockaddr_storage ss;
    struct sockaddr_un *un;
    const char *addr;

    /*  First, create the AF_UNIX address. */
    addr = nn_epbase_getaddr (&self->epbase);
    memset (&ss, 0, sizeof (ss));
    un = (struct sockaddr_un*) &ss;
    nn_assert (strlen (addr) < sizeof (un->sun_path));
    ss.ss_family = AF_UNIX;
    strncpy (un->sun_path, addr, sizeof (un->sun_path));

    /*  Delete the RTIPC file left over by eventual previous runs of
        the application. */
    rc = unlink (addr);
    errno_assert (rc == 0 || errno == ENOENT);

    /*  Start listening for incoming connections. */
    rc = nn_usock_start (&self->usock, AF_UNIX, SOCK_STREAM, 0);
    /*  TODO: EMFILE error can happen here. We can wait a bit and re-try. */
    errnum_assert (rc == 0, -rc);
    rc = nn_usock_bind (&self->usock,
        (struct sockaddr*) &ss, sizeof (struct sockaddr_un));
    errnum_assert (rc == 0, -rc);
    rc = nn_usock_listen (&self->usock, NN_BRTIPC_BACKLOG);
    errnum_assert (rc == 0, -rc);
}

static void nn_brtipc_start_accepting (struct nn_brtipc *self)
{
    nn_assert (self->artipc == NULL);

    /*  Allocate new artipc state machine. */
    self->artipc = nn_alloc (sizeof (struct nn_artipc), "artipc");
    alloc_assert (self->artipc);
    nn_artipc_init (self->artipc, NN_BRTIPC_SRC_ARTIPC, &self->epbase, &self->fsm);

    /*  Start waiting for a new incoming connection. */
    nn_artipc_start (self->artipc, &self->usock);
}

#endif

