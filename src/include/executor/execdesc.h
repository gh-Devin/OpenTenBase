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
 * execdesc.h
 *      plan and query descriptor accessor macros used by the executor
 *      and related modules.
 *
 *
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * This source code file contains modifications made by THL A29 Limited ("Tencent Modifications").
 * All Tencent Modifications are Copyright (C) 2023 THL A29 Limited.
 *
 * src/include/executor/execdesc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECDESC_H
#define EXECDESC_H

#include "nodes/execnodes.h"
#ifdef XCP
#include "pgxc/squeue.h"
#endif
#include "tcop/dest.h"

/* ----------------
 *        query descriptor:
 *
 *    a QueryDesc encapsulates everything that the executor
 *    needs to execute the query.
 *
 *    For the convenience of SQL-language functions, we also support QueryDescs
 *    containing utility statements; these must not be passed to the executor
 *    however.
 * ---------------------
 */
typedef struct QueryDesc
{
    /* These fields are provided by CreateQueryDesc */
    CmdType        operation;        /* CMD_SELECT, CMD_UPDATE, etc. */
    PlannedStmt *plannedstmt;    /* planner's output (could be utility, too) */
    const char *sourceText;        /* source text of the query */
    Snapshot    snapshot;        /* snapshot to use for query */
    Snapshot    crosscheck_snapshot;    /* crosscheck for RI update/delete */
    DestReceiver *dest;            /* the destination for tuple output */
    ParamListInfo params;        /* param values being passed in */
    QueryEnvironment *queryEnv; /* query environment passed in */
    int            instrument_options; /* OR of InstrumentOption flags */

    /* These fields are set by ExecutorStart */
    TupleDesc    tupDesc;        /* descriptor for result tuples */
    EState       *estate;            /* executor's query-wide state */
    PlanState  *planstate;        /* tree of per-plan-node state */

#ifdef XCP
    SharedQueue squeue;         /* the shared memory queue to sent data to other
                                 * nodes */
#ifdef __OPENTENBASE__
     DataPumpSender sender; /* used for locally data transfering */
    ParamExecData *es_param_exec_vals;    /* values of internal params */
	RemoteEPQContext *epqContext; /* information about EvalPlanQual from remote */
#endif
                                 
    int         myindex;        /* -1 if locally executed subplan is producing
                                 * data and distribute via squeue. Otherwise
                                 * get local data from squeue */
#endif
    /* This field is set by ExecutorRun */
    bool        already_executed;    /* true if previously executed */

    /* This is always set NULL by the core system, but plugins can change it */
    struct Instrumentation *totaltime;    /* total time spent in ExecutorRun */
} QueryDesc;

/* in pquery.c */
extern QueryDesc *CreateQueryDesc(PlannedStmt *plannedstmt,
                const char *sourceText,
                Snapshot snapshot,
                Snapshot crosscheck_snapshot,
                DestReceiver *dest,
                ParamListInfo params,
                QueryEnvironment *queryEnv,
                int instrument_options);

extern void FreeQueryDesc(QueryDesc *qdesc);

#endif                            /* EXECDESC_H  */
