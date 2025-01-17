/*
 * Tencent is pleased to support the open source community by making OpenTenBase available.  
 * 
 * Copyright (C) 2019 THL A29 Limited, a Tencent company.  All rights reserved.
 * 
 * OpenTenBase is licensed under the BSD 3-Clause License, except for the third-party component listed below. 
 * 
 * A copy of the BSD 3-Clause License is included in this file.
 * 
 * Other dependencies and licenses:
 * 
 * Open Source Software Licensed Under the PostgreSQL License: 
 * --------------------------------------------------------------------
 * 1. Postgres-XL XL9_5_STABLE
 * Portions Copyright (c) 2015-2016, 2ndQuadrant Ltd
 * Portions Copyright (c) 2012-2015, TransLattice, Inc.
 * Portions Copyright (c) 2010-2017, Postgres-XC Development Group
 * Portions Copyright (c) 1996-2015, The PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, The Regents of the University of California
 * 
 * Terms of the PostgreSQL License: 
 * --------------------------------------------------------------------
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 * 
 * IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * 
 * THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 * 
 * 
 * Terms of the BSD 3-Clause License:
 * --------------------------------------------------------------------
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation 
 * and/or other materials provided with the distribution.
 * 
 * 3. Neither the name of THL A29 Limited nor the names of its contributors may be used to endorse or promote products derived from this software without 
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS 
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE 
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH 
 * DAMAGE.
 * 
 */
/*-------------------------------------------------------------------------
 *
 * ipc.c
 *      POSTGRES inter-process communication definitions.
 *
 * This file is misnamed, as it no longer has much of anything directly
 * to do with IPC.  The functionality here is concerned with managing
 * exit-time cleanup for either a postmaster or a backend.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *      src/backend/storage/ipc/ipc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include "miscadmin.h"
#ifdef PROFILE_PID_DIR
#include "postmaster/autovacuum.h"
#endif
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "tcop/tcopprot.h"

/*
 * This flag is set during proc_exit() to change ereport()'s behavior,
 * so that an ereport() from an on_proc_exit routine cannot get us out
 * of the exit procedure.  We do NOT want to go back to the idle loop...
 */
bool        proc_exit_inprogress = false;

/*
 * This flag tracks whether we've called atexit() in the current process
 * (or in the parent postmaster).
 */
static bool atexit_callback_setup = false;

/* local functions */
static void proc_exit_prepare(int code);


/* ----------------------------------------------------------------
 *                        exit() handling stuff
 *
 * These functions are in generally the same spirit as atexit(),
 * but provide some additional features we need --- in particular,
 * we want to register callbacks to invoke when we are disconnecting
 * from a broken shared-memory context but not exiting the postmaster.
 *
 * Callback functions can take zero, one, or two args: the first passed
 * arg is the integer exitcode, the second is the Datum supplied when
 * the callback was registered.
 * ----------------------------------------------------------------
 */

#define MAX_ON_EXITS 20

struct ONEXIT
{
    pg_on_exit_callback function;
    Datum        arg;
};

static struct ONEXIT on_proc_exit_list[MAX_ON_EXITS];
static struct ONEXIT on_shmem_exit_list[MAX_ON_EXITS];
static struct ONEXIT before_shmem_exit_list[MAX_ON_EXITS];

static int    on_proc_exit_index,
            on_shmem_exit_index,
            before_shmem_exit_index;


/* ----------------------------------------------------------------
 *        proc_exit
 *
 *        this function calls all the callbacks registered
 *        for it (to free resources) and then calls exit.
 *
 *        This should be the only function to call exit().
 *        -cim 2/6/90
 *
 *        Unfortunately, we can't really guarantee that add-on code
 *        obeys the rule of not calling exit() directly.  So, while
 *        this is the preferred way out of the system, we also register
 *        an atexit callback that will make sure cleanup happens.
 * ----------------------------------------------------------------
 */
void
proc_exit(int code)
{
    /* Clean up everything that must be cleaned up */
    proc_exit_prepare(code);

#ifdef PROFILE_PID_DIR
    {
        /*
         * If we are profiling ourself then gprof's mcleanup() is about to
         * write out a profile to ./gmon.out.  Since mcleanup() always uses a
         * fixed file name, each backend will overwrite earlier profiles. To
         * fix that, we create a separate subdirectory for each backend
         * (./gprof/pid) and 'cd' to that subdirectory before we exit() - that
         * forces mcleanup() to write each profile into its own directory.  We
         * end up with something like: $PGDATA/gprof/8829/gmon.out
         * $PGDATA/gprof/8845/gmon.out ...
         *
         * To avoid undesirable disk space bloat, autovacuum workers are
         * discriminated against: all their gmon.out files go into the same
         * subdirectory.  Without this, an installation that is "just sitting
         * there" nonetheless eats megabytes of disk space every few seconds.
         *
         * Note that we do this here instead of in an on_proc_exit() callback
         * because we want to ensure that this code executes last - we don't
         * want to interfere with any other on_proc_exit() callback.  For the
         * same reason, we do not include it in proc_exit_prepare ... so if
         * you are exiting in the "wrong way" you won't drop your profile in a
         * nice place.
         */
        char        gprofDirName[32];

        if (IsAutoVacuumWorkerProcess())
            snprintf(gprofDirName, 32, "gprof/avworker");
        else
            snprintf(gprofDirName, 32, "gprof/%d", (int) getpid());

        mkdir("gprof", S_IRWXU | S_IRWXG | S_IRWXO);
        mkdir(gprofDirName, S_IRWXU | S_IRWXG | S_IRWXO);
        chdir(gprofDirName);
    }
#endif

#ifdef    _PG_REGRESS_
    elog(LOG, "process pid:%d exit(%d) exit now", (int) getpid(), code);
#endif

    elog(DEBUG3, "exit(%d)", code);

    exit(code);
}

/*
 * Code shared between proc_exit and the atexit handler.  Note that in
 * normal exit through proc_exit, this will actually be called twice ...
 * but the second call will have nothing to do.
 */
static void
proc_exit_prepare(int code)
{
    /*
     * Once we set this flag, we are committed to exit.  Any ereport() will
     * NOT send control back to the main loop, but right back here.
     */
    proc_exit_inprogress = true;

    /*
     * Forget any pending cancel or die requests; we're doing our best to
     * close up shop already.  Note that the signal handlers will not set
     * these flags again, now that proc_exit_inprogress is set.
     */
    InterruptPending = false;
    ProcDiePending = false;
    QueryCancelPending = false;
    InterruptHoldoffCount = 1;
    CritSectionCount = 0;

    /*
     * Also clear the error context stack, to prevent error callbacks from
     * being invoked by any elog/ereport calls made during proc_exit. Whatever
     * context they might want to offer is probably not relevant, and in any
     * case they are likely to fail outright after we've done things like
     * aborting any open transaction.  (In normal exit scenarios the context
     * stack should be empty anyway, but it might not be in the case of
     * elog(FATAL) for example.)
     */
    error_context_stack = NULL;
    /* For the same reason, reset debug_query_string before it's clobbered */
    debug_query_string = NULL;

    /* do our shared memory exits first */
    shmem_exit(code);

    elog(DEBUG3, "proc_exit(%d): %d callbacks to make",
         code, on_proc_exit_index);

    /*
     * call all the registered callbacks.
     *
     * Note that since we decrement on_proc_exit_index each time, if a
     * callback calls ereport(ERROR) or ereport(FATAL) then it won't be
     * invoked again when control comes back here (nor will the
     * previously-completed callbacks).  So, an infinite loop should not be
     * possible.
     */
    while (--on_proc_exit_index >= 0)
        (*on_proc_exit_list[on_proc_exit_index].function) (code,
                                                           on_proc_exit_list[on_proc_exit_index].arg);

    on_proc_exit_index = 0;
}

/* ------------------
 * Run all of the on_shmem_exit routines --- but don't actually exit.
 * This is used by the postmaster to re-initialize shared memory and
 * semaphores after a backend dies horribly.  As with proc_exit(), we
 * remove each callback from the list before calling it, to avoid
 * infinite loop in case of error.
 * ------------------
 */
void
shmem_exit(int code)
{
    /*
     * Call before_shmem_exit callbacks.
     *
     * These should be things that need most of the system to still be up and
     * working, such as cleanup of temp relations, which requires catalog
     * access; or things that need to be completed because later cleanup steps
     * depend on them, such as releasing lwlocks.
     */
    elog(DEBUG3, "shmem_exit(%d): %d before_shmem_exit callbacks to make",
         code, before_shmem_exit_index);
    while (--before_shmem_exit_index >= 0)
        (*before_shmem_exit_list[before_shmem_exit_index].function) (code,
                                                                     before_shmem_exit_list[before_shmem_exit_index].arg);
    before_shmem_exit_index = 0;

    /*
     * Call dynamic shared memory callbacks.
     *
     * These serve the same purpose as late callbacks, but for dynamic shared
     * memory segments rather than the main shared memory segment.
     * dsm_backend_shutdown() has the same kind of progressive logic we use
     * for the main shared memory segment; namely, it unregisters each
     * callback before invoking it, so that we don't get stuck in an infinite
     * loop if one of those callbacks itself throws an ERROR or FATAL.
     *
     * Note that explicitly calling this function here is quite different from
     * registering it as an on_shmem_exit callback for precisely this reason:
     * if one dynamic shared memory callback errors out, the remaining
     * callbacks will still be invoked.  Thus, hard-coding this call puts it
     * equal footing with callbacks for the main shared memory segment.
     */
    dsm_backend_shutdown();

    /*
     * Call on_shmem_exit callbacks.
     *
     * These are generally releasing low-level shared memory resources.  In
     * some cases, this is a backstop against the possibility that the early
     * callbacks might themselves fail, leading to re-entry to this routine;
     * in other cases, it's cleanup that only happens at process exit.
     */
    elog(DEBUG3, "shmem_exit(%d): %d on_shmem_exit callbacks to make",
         code, on_shmem_exit_index);
    while (--on_shmem_exit_index >= 0)
        (*on_shmem_exit_list[on_shmem_exit_index].function) (code,
                                                             on_shmem_exit_list[on_shmem_exit_index].arg);
    on_shmem_exit_index = 0;
}

/* ----------------------------------------------------------------
 *        atexit_callback
 *
 *        Backstop to ensure that direct calls of exit() don't mess us up.
 *
 * Somebody who was being really uncooperative could call _exit(),
 * but for that case we have a "dead man switch" that will make the
 * postmaster treat it as a crash --- see pmsignal.c.
 * ----------------------------------------------------------------
 */
static void
atexit_callback(void)
{
    /* Clean up everything that must be cleaned up */
    /* ... too bad we don't know the real exit code ... */
    proc_exit_prepare(-1);
}

/* ----------------------------------------------------------------
 *        on_proc_exit
 *
 *        this function adds a callback function to the list of
 *        functions invoked by proc_exit().   -cim 2/6/90
 * ----------------------------------------------------------------
 */
void
on_proc_exit(pg_on_exit_callback function, Datum arg)
{
    if (on_proc_exit_index >= MAX_ON_EXITS)
        ereport(FATAL,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg_internal("out of on_proc_exit slots")));

    on_proc_exit_list[on_proc_exit_index].function = function;
    on_proc_exit_list[on_proc_exit_index].arg = arg;

    ++on_proc_exit_index;

    if (!atexit_callback_setup)
    {
        atexit(atexit_callback);
        atexit_callback_setup = true;
    }
}

/* ----------------------------------------------------------------
 *        before_shmem_exit
 *
 *        Register early callback to perform user-level cleanup,
 *        e.g. transaction abort, before we begin shutting down
 *        low-level subsystems.
 * ----------------------------------------------------------------
 */
void
before_shmem_exit(pg_on_exit_callback function, Datum arg)
{
    if (before_shmem_exit_index >= MAX_ON_EXITS)
        ereport(FATAL,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg_internal("out of before_shmem_exit slots")));

    before_shmem_exit_list[before_shmem_exit_index].function = function;
    before_shmem_exit_list[before_shmem_exit_index].arg = arg;

    ++before_shmem_exit_index;

    if (!atexit_callback_setup)
    {
        atexit(atexit_callback);
        atexit_callback_setup = true;
    }
}

/* ----------------------------------------------------------------
 *        on_shmem_exit
 *
 *        Register ordinary callback to perform low-level shutdown
 *        (e.g. releasing our PGPROC); run after before_shmem_exit
 *        callbacks and before on_proc_exit callbacks.
 * ----------------------------------------------------------------
 */
void
on_shmem_exit(pg_on_exit_callback function, Datum arg)
{
    if (on_shmem_exit_index >= MAX_ON_EXITS)
        ereport(FATAL,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg_internal("out of on_shmem_exit slots")));

    on_shmem_exit_list[on_shmem_exit_index].function = function;
    on_shmem_exit_list[on_shmem_exit_index].arg = arg;

    ++on_shmem_exit_index;

    if (!atexit_callback_setup)
    {
        atexit(atexit_callback);
        atexit_callback_setup = true;
    }
}

/* ----------------------------------------------------------------
 *        cancel_before_shmem_exit
 *
 *        this function removes a previously-registed before_shmem_exit
 *        callback.  For simplicity, only the latest entry can be
 *        removed.  (We could work harder but there is no need for
 *        current uses.)
 * ----------------------------------------------------------------
 */
void
cancel_before_shmem_exit(pg_on_exit_callback function, Datum arg)
{
    if (before_shmem_exit_index > 0 &&
        before_shmem_exit_list[before_shmem_exit_index - 1].function
        == function &&
        before_shmem_exit_list[before_shmem_exit_index - 1].arg == arg)
        --before_shmem_exit_index;
}

/* ----------------------------------------------------------------
 *        on_exit_reset
 *
 *        this function clears all on_proc_exit() and on_shmem_exit()
 *        registered functions.  This is used just after forking a backend,
 *        so that the backend doesn't believe it should call the postmaster's
 *        on-exit routines when it exits...
 * ----------------------------------------------------------------
 */
void
on_exit_reset(void)
{
    before_shmem_exit_index = 0;
    on_shmem_exit_index = 0;
    on_proc_exit_index = 0;
    reset_on_dsm_detach();
}
