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
 * dest.c
 *      support for communication destinations
 *
 *
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *      src/backend/tcop/dest.c
 *
 *-------------------------------------------------------------------------
 */
/*
 *     INTERFACE ROUTINES
 *        BeginCommand - initialize the destination at start of command
 *        CreateDestReceiver - create tuple receiver object for destination
 *        EndCommand - clean up the destination at end of command
 *        NullCommand - tell dest that an empty query string was recognized
 *        ReadyForQuery - tell dest that we are ready for a new query
 *
 *     NOTES
 *        These routines do the appropriate work before and after
 *        tuples are returned by a query to keep the backend and the
 *        "destination" portals synchronized.
 */

#include "postgres.h"

#include "access/printsimple.h"
#include "access/printtup.h"
#include "access/xact.h"
#include "commands/copy.h"
#include "commands/createas.h"
#include "commands/matview.h"
#include "executor/functions.h"
#ifdef XCP
#include "executor/producerReceiver.h"
#endif
#include "executor/tqueue.h"
#include "executor/tstoreReceiver.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "utils/portal.h"
#include "miscadmin.h"



/* ----------------
 *        dummy DestReceiver functions
 * ----------------
 */
static bool
donothingReceive(TupleTableSlot *slot, DestReceiver *self)
{
    return true;
}

static void
donothingStartup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
}

static void
donothingCleanup(DestReceiver *self)
{
    /* this is used for both shutdown and destroy methods */
}

/* ----------------
 *        static DestReceiver structs for dest types needing no local state
 * ----------------
 */
static DestReceiver donothingDR = {
    donothingReceive, donothingStartup, donothingCleanup, donothingCleanup,
    DestNone
};

static DestReceiver debugtupDR = {
    debugtup, debugStartup, donothingCleanup, donothingCleanup,
    DestDebug
};

static DestReceiver printsimpleDR = {
    printsimple, printsimple_startup, donothingCleanup, donothingCleanup,
    DestRemoteSimple
};

static DestReceiver spi_printtupDR = {
    spi_printtup, spi_dest_startup, donothingCleanup, donothingCleanup,
    DestSPI
};

/* Globally available receiver for DestNone */
DestReceiver *None_Receiver = &donothingDR;


/* ----------------
 *        BeginCommand - initialize the destination at start of command
 * ----------------
 */
void
BeginCommand(const char *commandTag, CommandDest dest)
{
    /* Nothing to do at present */
}

/* ----------------
 *        CreateDestReceiver - return appropriate receiver function set for dest
 * ----------------
 */
DestReceiver *
CreateDestReceiver(CommandDest dest)
{// #lizard forgives
    switch (dest)
    {
        case DestRemote:
        case DestRemoteExecute:
            return printtup_create_DR(dest);

        case DestRemoteSimple:
            return &printsimpleDR;

        case DestNone:
            return &donothingDR;

        case DestDebug:
            return &debugtupDR;

        case DestSPI:
            return &spi_printtupDR;

        case DestTuplestore:
            return CreateTuplestoreDestReceiver();

        case DestIntoRel:
            return CreateIntoRelDestReceiver(NULL);

        case DestCopyOut:
            return CreateCopyDestReceiver();

        case DestSQLFunction:
            return CreateSQLFunctionDestReceiver();

#ifdef XCP
        case DestProducer:
            return CreateProducerDestReceiver();
        case DestParallelSend:
            return NULL;
#endif

        case DestTransientRel:
            return CreateTransientRelDestReceiver(InvalidOid);

        case DestTupleQueue:
            return CreateTupleQueueDestReceiver(NULL);
    }

    /* should never get here */
    return &donothingDR;
}

/* ----------------
 *        EndCommand - clean up the destination at end of command
 * ----------------
 */
void
EndCommand(const char *commandTag, CommandDest dest)
{// #lizard forgives
    switch (dest)
    {
        case DestRemote:
        case DestRemoteExecute:
        case DestRemoteSimple:

            /*
             * We assume the commandTag is plain ASCII and therefore requires
             * no encoding conversion.
             */
            elog(DEBUG1, "pid:%d send CommandComplete", MyProcPid);
            pq_putmessage('C', commandTag, strlen(commandTag) + 1);
            break;

        case DestNone:
        case DestDebug:
        case DestSPI:
        case DestTuplestore:
        case DestIntoRel:
        case DestCopyOut:
        case DestSQLFunction:
        case DestProducer:
        case DestParallelSend:
        case DestTransientRel:
        case DestTupleQueue:
            break;
    }
}

/* ----------------
 *        NullCommand - tell dest that an empty query string was recognized
 *
 *        In FE/BE protocol version 1.0, this hack is necessary to support
 *        libpq's crufty way of determining whether a multiple-command
 *        query string is done.  In protocol 2.0 it's probably not really
 *        necessary to distinguish empty queries anymore, but we still do it
 *        for backwards compatibility with 1.0.  In protocol 3.0 it has some
 *        use again, since it ensures that there will be a recognizable end
 *        to the response to an Execute message.
 * ----------------
 */
void
NullCommand(CommandDest dest)
{// #lizard forgives
    switch (dest)
    {
        case DestRemote:
        case DestRemoteExecute:
        case DestRemoteSimple:

            /*
             * tell the fe that we saw an empty query string.  In protocols
             * before 3.0 this has a useless empty-string message body.
             */
            if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
                pq_putemptymessage('I');
            else
                pq_putmessage('I', "", 1);
            break;

        case DestNone:
        case DestDebug:
        case DestSPI:
        case DestTuplestore:
        case DestIntoRel:
        case DestCopyOut:
        case DestSQLFunction:
        case DestProducer:
        case DestParallelSend:
        case DestTransientRel:
        case DestTupleQueue:
            break;
    }
}

/* ----------------
 *        ReadyForQuery - tell dest that we are ready for a new query
 *
 *        The ReadyForQuery message is sent so that the FE can tell when
 *        we are done processing a query string.
 *        In versions 3.0 and up, it also carries a transaction state indicator.
 *
 *        Note that by flushing the stdio buffer here, we can avoid doing it
 *        most other places and thus reduce the number of separate packets sent.
 * ----------------
 */
void
ReadyForQuery(CommandDest dest)
{// #lizard forgives
    switch (dest)
    {
        case DestRemote:
        case DestRemoteExecute:
        case DestRemoteSimple:
            if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
            {
                StringInfoData buf;

                pq_beginmessage(&buf, 'Z');
                pq_sendbyte(&buf, TransactionBlockStatusCode());
                pq_endmessage(&buf);
            }
            else
                pq_putemptymessage('Z');
            /* Flush output at end of cycle in any case. */
            pq_flush();
            
#ifdef    _PG_REGRESS_
            elog(LOG, "pid %d send ReadyForQuery", MyProcPid);
#endif
            break;

        case DestNone:
        case DestDebug:
        case DestSPI:
        case DestTuplestore:
        case DestIntoRel:
        case DestCopyOut:
        case DestSQLFunction:
        case DestProducer:
        case DestParallelSend:
        case DestTransientRel:
        case DestTupleQueue:
            break;
    }
}

#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__

void
ReadyForCommit(CommandDest dest)
{// #lizard forgives
    switch (dest)
    {
        case DestRemote:
        case DestRemoteExecute:
        case DestRemoteSimple:
            if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
            {
                StringInfoData buf;

                pq_beginmessage(&buf, 'Y');
                pq_sendbyte(&buf, TransactionBlockStatusCode());
                pq_endmessage(&buf);
            }
            else
                pq_putemptymessage('Y');
            /* Flush output at end of cycle in any case. */
            pq_flush();
            break;

        case DestNone:
        case DestDebug:
        case DestSPI:
        case DestTuplestore:
        case DestIntoRel:
        case DestCopyOut:
        case DestSQLFunction:
        case DestProducer:
        case DestParallelSend:
        case DestTransientRel:
        case DestTupleQueue:
            break;
    }

}

#endif
