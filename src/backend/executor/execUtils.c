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
 * execUtils.c
 *      miscellaneous executor utility routines
 *
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * This source code file contains modifications made by THL A29 Limited ("Tencent Modifications").
 * All Tencent Modifications are Copyright (C) 2023 THL A29 Limited.
 *
 * IDENTIFICATION
 *      src/backend/executor/execUtils.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *        CreateExecutorState        Create/delete executor working state
 *        FreeExecutorState
 *        CreateExprContext
 *        CreateStandaloneExprContext
 *        FreeExprContext
 *        ReScanExprContext
 *
 *        ExecAssignExprContext    Common code for plan node init routines.
 *        ExecAssignResultType
 *        etc
 *
 *        ExecOpenScanRelation    Common code for scan node init routines.
 *        ExecCloseScanRelation
 *
 *        executor_errposition    Report syntactic position of an error.
 *
 *        RegisterExprContextCallback    Register function shutdown callback
 *        UnregisterExprContextCallback  Deregister function shutdown callback
 *
 *        GetAttributeByName        Runtime extraction of columns from tuples.
 *        GetAttributeByNum
 *
 *     NOTES
 *        This file has traditionally been the place to stick misc.
 *        executor support stuff that doesn't really go anyplace else.
 */

#include "postgres.h"

#include "access/relscan.h"
#include "access/transam.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/typcache.h"
#ifdef __OPENTENBASE__
#include "utils/ruleutils.h"
#endif

#include "pgxc/execRemote.h"

static void ShutdownExprContext(ExprContext *econtext, bool isCommit);


/* ----------------------------------------------------------------
 *                 Executor state and memory management functions
 * ----------------------------------------------------------------
 */

/* ----------------
 *        CreateExecutorState
 *
 *        Create and initialize an EState node, which is the root of
 *        working storage for an entire Executor invocation.
 *
 * Principally, this creates the per-query memory context that will be
 * used to hold all working data that lives till the end of the query.
 * Note that the per-query context will become a child of the caller's
 * CurrentMemoryContext.
 * ----------------
 */
EState *
CreateExecutorState(void)
{
    EState       *estate;
    MemoryContext qcontext;
    MemoryContext oldcontext;

    /*
     * Create the per-query context for this Executor run.
     */
    qcontext = AllocSetContextCreate(CurrentMemoryContext,
                                     "ExecutorState",
                                     ALLOCSET_DEFAULT_SIZES);

    /*
     * Make the EState node within the per-query context.  This way, we don't
     * need a separate pfree() operation for it at shutdown.
     */
    oldcontext = MemoryContextSwitchTo(qcontext);

    estate = makeNode(EState);

    /*
     * Initialize all fields of the Executor State structure
     */
    estate->es_direction = ForwardScanDirection;
    estate->es_snapshot = InvalidSnapshot;    /* caller must initialize this */
    estate->es_crosscheck_snapshot = InvalidSnapshot;    /* no crosscheck */
    estate->es_range_table = NIL;
    estate->es_plannedstmt = NULL;

    estate->es_junkFilter = NULL;

    estate->es_output_cid = (CommandId) 0;

    estate->es_result_relations = NULL;
    estate->es_num_result_relations = 0;
    estate->es_result_relation_info = NULL;

    estate->es_trig_target_relations = NIL;
    estate->es_trig_tuple_slot = NULL;
    estate->es_trig_oldtup_slot = NULL;
    estate->es_trig_newtup_slot = NULL;

    estate->es_param_list_info = NULL;
    estate->es_param_exec_vals = NULL;

    estate->es_queryEnv = NULL;

    estate->es_query_cxt = qcontext;

    estate->es_tupleTable = NIL;

    estate->es_rowMarks = NIL;

    estate->es_processed = 0;
    estate->es_lastoid = InvalidOid;

    estate->es_top_eflags = 0;
    estate->es_instrument = 0;
    estate->es_finished = false;

    estate->es_exprcontexts = NIL;

    estate->es_subplanstates = NIL;

    estate->es_auxmodifytables = NIL;

    estate->es_per_tuple_exprcontext = NULL;

    estate->es_epqTuple = NULL;
    estate->es_epqTupleSet = NULL;
    estate->es_epqScanDone = NULL;
    estate->es_sourceText = NULL;

#ifdef __AUDIT__
    estate->es_remote_subplan_num = 0;
#endif

    /*
     * Return the executor state structure
     */
    MemoryContextSwitchTo(oldcontext);

    return estate;
}

/* ----------------
 *        FreeExecutorState
 *
 *        Release an EState along with all remaining working storage.
 *
 * Note: this is not responsible for releasing non-memory resources,
 * such as open relations or buffer pins.  But it will shut down any
 * still-active ExprContexts within the EState.  That is sufficient
 * cleanup for situations where the EState has only been used for expression
 * evaluation, and not to run a complete Plan.
 *
 * This can be called in any memory context ... so long as it's not one
 * of the ones to be freed.
 * ----------------
 */
void
FreeExecutorState(EState *estate)
{
    /*
     * Shut down and free any remaining ExprContexts.  We do this explicitly
     * to ensure that any remaining shutdown callbacks get called (since they
     * might need to release resources that aren't simply memory within the
     * per-query memory context).
     */
    while (estate->es_exprcontexts)
    {
        /*
         * XXX: seems there ought to be a faster way to implement this than
         * repeated list_delete(), no?
         */
        FreeExprContext((ExprContext *) linitial(estate->es_exprcontexts),
                        true);
        /* FreeExprContext removed the list link for us */
    }

    /*
     * Free the per-query memory context, thereby releasing all working
     * memory, including the EState node itself.
     */
    MemoryContextDelete(estate->es_query_cxt);
}

/* ----------------
 *        CreateExprContext
 *
 *        Create a context for expression evaluation within an EState.
 *
 * An executor run may require multiple ExprContexts (we usually make one
 * for each Plan node, and a separate one for per-output-tuple processing
 * such as constraint checking).  Each ExprContext has its own "per-tuple"
 * memory context.
 *
 * Note we make no assumption about the caller's memory context.
 * ----------------
 */
ExprContext *
CreateExprContext(EState *estate)
{
    ExprContext *econtext;
    MemoryContext oldcontext;

    /* Create the ExprContext node within the per-query memory context */
    oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

    econtext = makeNode(ExprContext);

    /* Initialize fields of ExprContext */
    econtext->ecxt_scantuple = NULL;
    econtext->ecxt_innertuple = NULL;
    econtext->ecxt_outertuple = NULL;

    econtext->ecxt_per_query_memory = estate->es_query_cxt;

    /*
     * Create working memory for expression evaluation in this context.
     */
    econtext->ecxt_per_tuple_memory =
        AllocSetContextCreate(estate->es_query_cxt,
                              "ExprContext",
                              ALLOCSET_DEFAULT_SIZES);

    econtext->ecxt_param_exec_vals = estate->es_param_exec_vals;
    econtext->ecxt_param_list_info = estate->es_param_list_info;

    econtext->ecxt_aggvalues = NULL;
    econtext->ecxt_aggnulls = NULL;

    econtext->caseValue_datum = (Datum) 0;
    econtext->caseValue_isNull = true;

    econtext->domainValue_datum = (Datum) 0;
    econtext->domainValue_isNull = true;

    econtext->ecxt_estate = estate;

    econtext->ecxt_callbacks = NULL;

    /*
     * Link the ExprContext into the EState to ensure it is shut down when the
     * EState is freed.  Because we use lcons(), shutdowns will occur in
     * reverse order of creation, which may not be essential but can't hurt.
     */
    estate->es_exprcontexts = lcons(econtext, estate->es_exprcontexts);

    MemoryContextSwitchTo(oldcontext);

    return econtext;
}

/* ----------------
 *        CreateStandaloneExprContext
 *
 *        Create a context for standalone expression evaluation.
 *
 * An ExprContext made this way can be used for evaluation of expressions
 * that contain no Params, subplans, or Var references (it might work to
 * put tuple references into the scantuple field, but it seems unwise).
 *
 * The ExprContext struct is allocated in the caller's current memory
 * context, which also becomes its "per query" context.
 *
 * It is caller's responsibility to free the ExprContext when done,
 * or at least ensure that any shutdown callbacks have been called
 * (ReScanExprContext() is suitable).  Otherwise, non-memory resources
 * might be leaked.
 * ----------------
 */
ExprContext *
CreateStandaloneExprContext(void)
{
    ExprContext *econtext;

    /* Create the ExprContext node within the caller's memory context */
    econtext = makeNode(ExprContext);

    /* Initialize fields of ExprContext */
    econtext->ecxt_scantuple = NULL;
    econtext->ecxt_innertuple = NULL;
    econtext->ecxt_outertuple = NULL;

    econtext->ecxt_per_query_memory = CurrentMemoryContext;

    /*
     * Create working memory for expression evaluation in this context.
     */
    econtext->ecxt_per_tuple_memory =
        AllocSetContextCreate(CurrentMemoryContext,
                              "ExprContext",
                              ALLOCSET_DEFAULT_SIZES);

    econtext->ecxt_param_exec_vals = NULL;
    econtext->ecxt_param_list_info = NULL;

    econtext->ecxt_aggvalues = NULL;
    econtext->ecxt_aggnulls = NULL;

    econtext->caseValue_datum = (Datum) 0;
    econtext->caseValue_isNull = true;

    econtext->domainValue_datum = (Datum) 0;
    econtext->domainValue_isNull = true;

    econtext->ecxt_estate = NULL;

    econtext->ecxt_callbacks = NULL;

    return econtext;
}

/* ----------------
 *        FreeExprContext
 *
 *        Free an expression context, including calling any remaining
 *        shutdown callbacks.
 *
 * Since we free the temporary context used for expression evaluation,
 * any previously computed pass-by-reference expression result will go away!
 *
 * If isCommit is false, we are being called in error cleanup, and should
 * not call callbacks but only release memory.  (It might be better to call
 * the callbacks and pass the isCommit flag to them, but that would require
 * more invasive code changes than currently seems justified.)
 *
 * Note we make no assumption about the caller's memory context.
 * ----------------
 */
void
FreeExprContext(ExprContext *econtext, bool isCommit)
{
    EState       *estate;

    /* Call any registered callbacks */
    ShutdownExprContext(econtext, isCommit);
    /* And clean up the memory used */
    MemoryContextDelete(econtext->ecxt_per_tuple_memory);
    /* Unlink self from owning EState, if any */
    estate = econtext->ecxt_estate;
    if (estate)
        estate->es_exprcontexts = list_delete_ptr(estate->es_exprcontexts,
                                                  econtext);
    /* And delete the ExprContext node */
    pfree(econtext);
}

/*
 * ReScanExprContext
 *
 *        Reset an expression context in preparation for a rescan of its
 *        plan node.  This requires calling any registered shutdown callbacks,
 *        since any partially complete set-returning-functions must be canceled.
 *
 * Note we make no assumption about the caller's memory context.
 */
void
ReScanExprContext(ExprContext *econtext)
{
    /* Call any registered callbacks */
    ShutdownExprContext(econtext, true);
    /* And clean up the memory used */
    MemoryContextReset(econtext->ecxt_per_tuple_memory);
}

/*
 * Build a per-output-tuple ExprContext for an EState.
 *
 * This is normally invoked via GetPerTupleExprContext() macro,
 * not directly.
 */
ExprContext *
MakePerTupleExprContext(EState *estate)
{
    if (estate->es_per_tuple_exprcontext == NULL)
        estate->es_per_tuple_exprcontext = CreateExprContext(estate);

    return estate->es_per_tuple_exprcontext;
}


/* ----------------------------------------------------------------
 *                 miscellaneous node-init support functions
 *
 * Note: all of these are expected to be called with CurrentMemoryContext
 * equal to the per-query memory context.
 * ----------------------------------------------------------------
 */

/* ----------------
 *        ExecAssignExprContext
 *
 *        This initializes the ps_ExprContext field.  It is only necessary
 *        to do this for nodes which use ExecQual or ExecProject
 *        because those routines require an econtext. Other nodes that
 *        don't have to evaluate expressions don't need to do this.
 * ----------------
 */
void
ExecAssignExprContext(EState *estate, PlanState *planstate)
{
    planstate->ps_ExprContext = CreateExprContext(estate);
}

/* ----------------
 *        ExecAssignResultType
 * ----------------
 */
void
ExecAssignResultType(PlanState *planstate, TupleDesc tupDesc)
{
    TupleTableSlot *slot = planstate->ps_ResultTupleSlot;

    ExecSetSlotDescriptor(slot, tupDesc);
}

/* ----------------
 *        ExecAssignResultTypeFromTL
 * ----------------
 */
void
ExecAssignResultTypeFromTL(PlanState *planstate)
{
    bool        hasoid;
    TupleDesc    tupDesc;
	List		*targetList = NIL;

    if (ExecContextForcesOids(planstate, &hasoid))
    {
        /* context forces OID choice; hasoid is now set correctly */
    }
    else
    {
        /* given free choice, don't leave space for OIDs in result tuples */
        hasoid = false;
    }

    /*
	 * If the command with returning syntax, the tupDesc's info should
	 * be maked up of returningList
	 */
	if (IsA(planstate, RemoteQueryState) &&
		(((((RemoteQueryState *)planstate)->eflags) & EXEC_FLAG_RETURNING) != 0))
	{
		if (planstate->state && planstate->state->es_plannedstmt &&
			planstate->state->es_plannedstmt->parseTree &&
			planstate->state->es_plannedstmt->parseTree->returningList)
			targetList = planstate->state->es_plannedstmt->parseTree->returningList;
	}
	if (targetList == NIL)
		targetList = planstate->plan->targetlist;

	/*
     * ExecTypeFromTL needs the parse-time representation of the tlist, not a
     * list of ExprStates.  This is good because some plan nodes don't bother
     * to set up planstate->targetlist ...
     */
	tupDesc = ExecTypeFromTL(targetList, hasoid);
    ExecAssignResultType(planstate, tupDesc);
}

/* ----------------
 *        ExecGetResultType
 * ----------------
 */
TupleDesc
ExecGetResultType(PlanState *planstate)
{
    TupleTableSlot *slot = planstate->ps_ResultTupleSlot;

    return slot->tts_tupleDescriptor;
}


/* ----------------
 *        ExecAssignProjectionInfo
 *
 * forms the projection information from the node's targetlist
 *
 * Notes for inputDesc are same as for ExecBuildProjectionInfo: supply it
 * for a relation-scan node, can pass NULL for upper-level nodes
 * ----------------
 */
void
ExecAssignProjectionInfo(PlanState *planstate,
                         TupleDesc inputDesc)
{
    planstate->ps_ProjInfo =
        ExecBuildProjectionInfo(planstate->plan->targetlist,
                                planstate->ps_ExprContext,
                                planstate->ps_ResultTupleSlot,
                                planstate,
                                inputDesc);
}


/* ----------------
 *        ExecFreeExprContext
 *
 * A plan node's ExprContext should be freed explicitly during executor
 * shutdown because there may be shutdown callbacks to call.  (Other resources
 * made by the above routines, such as projection info, don't need to be freed
 * explicitly because they're just memory in the per-query memory context.)
 *
 * However ... there is no particular need to do it during ExecEndNode,
 * because FreeExecutorState will free any remaining ExprContexts within
 * the EState.  Letting FreeExecutorState do it allows the ExprContexts to
 * be freed in reverse order of creation, rather than order of creation as
 * will happen if we delete them here, which saves O(N^2) work in the list
 * cleanup inside FreeExprContext.
 * ----------------
 */
void
ExecFreeExprContext(PlanState *planstate)
{
    /*
     * Per above discussion, don't actually delete the ExprContext. We do
     * unlink it from the plan node, though.
     */
    planstate->ps_ExprContext = NULL;
}

/* ----------------------------------------------------------------
 *        the following scan type support functions are for
 *        those nodes which are stubborn and return tuples in
 *        their Scan tuple slot instead of their Result tuple
 *        slot..  luck fur us, these nodes do not do projections
 *        so we don't have to worry about getting the ProjectionInfo
 *        right for them...  -cim 6/3/91
 * ----------------------------------------------------------------
 */

/* ----------------
 *        ExecAssignScanType
 * ----------------
 */
void
ExecAssignScanType(ScanState *scanstate, TupleDesc tupDesc)
{
    TupleTableSlot *slot = scanstate->ss_ScanTupleSlot;

    ExecSetSlotDescriptor(slot, tupDesc);
}

/* ----------------
 *        ExecAssignScanTypeFromOuterPlan
 * ----------------
 */
void
ExecAssignScanTypeFromOuterPlan(ScanState *scanstate)
{
    PlanState  *outerPlan;
    TupleDesc    tupDesc;

    outerPlan = outerPlanState(scanstate);
    tupDesc = ExecGetResultType(outerPlan);

    ExecAssignScanType(scanstate, tupDesc);
}


/* ----------------------------------------------------------------
 *                  Scan node support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *        ExecRelationIsTargetRelation
 *
 *        Detect whether a relation (identified by rangetable index)
 *        is one of the target relations of the query.
 * ----------------------------------------------------------------
 */
bool
ExecRelationIsTargetRelation(EState *estate, Index scanrelid)
{
    ResultRelInfo *resultRelInfos;
    int            i;

    resultRelInfos = estate->es_result_relations;
    for (i = 0; i < estate->es_num_result_relations; i++)
    {
        if (resultRelInfos[i].ri_RangeTableIndex == scanrelid)
            return true;
    }
    return false;
}

/* ----------------------------------------------------------------
 *        ExecOpenScanRelation
 *
 *        Open the heap relation to be scanned by a base-level scan plan node.
 *        This should be called during the node's ExecInit routine.
 *
 * By default, this acquires AccessShareLock on the relation.  However,
 * if the relation was already locked by InitPlan, we don't need to acquire
 * any additional lock.  This saves trips to the shared lock manager.
 * ----------------------------------------------------------------
 */
Relation
ExecOpenScanRelation(EState *estate, Index scanrelid, int eflags)
{
    Relation    rel;
    Oid            reloid;
    LOCKMODE    lockmode;

    /*
     * Determine the lock type we need.  First, scan to see if target relation
     * is a result relation.  If not, check if it's a FOR UPDATE/FOR SHARE
     * relation.  In either of those cases, we got the lock already.
     */
    lockmode = AccessShareLock;
    if (ExecRelationIsTargetRelation(estate, scanrelid))
        lockmode = NoLock;
    else
    {
        /* Keep this check in sync with InitPlan! */
        ExecRowMark *erm = ExecFindRowMark(estate, scanrelid, true);

        if (erm != NULL && erm->relation != NULL)
            lockmode = NoLock;
    }

    /* Open the relation and acquire lock as needed */
    reloid = getrelid(scanrelid, estate->es_range_table);
    rel = heap_open(reloid, lockmode);

    /*
     * Complain if we're attempting a scan of an unscannable relation, except
     * when the query won't actually be run.  This is a slightly klugy place
     * to do this, perhaps, but there is no better place.
     */
    if ((eflags & (EXEC_FLAG_EXPLAIN_ONLY | EXEC_FLAG_WITH_NO_DATA)) == 0 &&
        !RelationIsScannable(rel))
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("materialized view \"%s\" has not been populated",
                        RelationGetRelationName(rel)),
                 errhint("Use the REFRESH MATERIALIZED VIEW command.")));

    return rel;
}

#ifdef __OPENTENBASE__
Relation
ExecOpenScanRelationPartition(EState *estate, Index scanrelid, int eflags, int partidx)
{    
    Relation    rel;
    Oid            reloid;
    Relation     partrel;
    Oid            partoid;
    LOCKMODE    lockmode;

    /*
     * Determine the lock type we need.  First, scan to see if target relation
     * is a result relation.  If not, check if it's a FOR UPDATE/FOR SHARE
     * relation.  In either of those cases, we got the lock already.
     */
    lockmode = AccessShareLock;
    if (ExecRelationIsTargetRelation(estate, scanrelid))
        lockmode = NoLock;
    else
    {
        ListCell   *l;

        foreach(l, estate->es_rowMarks)
        {
            ExecRowMark *erm = lfirst(l);

            if (erm->rti == scanrelid)
            {
                lockmode = NoLock;
                break;
            }
        }
    }

    /* Open the relation and acquire lock as needed */
    reloid = getrelid(scanrelid, estate->es_range_table);
    rel = heap_open(reloid, NoLock);

    partoid = RelationGetPartition(rel, partidx, false);

	if (partoid)
	{
	    partrel = heap_open(partoid, lockmode);
	}
	else
	{
		partrel = NULL;
	}

    heap_close(rel, NoLock);
    /*
     * Complain if we're attempting a scan of an unscannable relation, except
     * when the query won't actually be run.  This is a slightly klugy place
     * to do this, perhaps, but there is no better place.
     */
    if ((eflags & (EXEC_FLAG_EXPLAIN_ONLY | EXEC_FLAG_WITH_NO_DATA)) == 0 &&
        !RelationIsScannable(rel))
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("materialized view \"%s\" has not been populated",
                        RelationGetRelationName(rel)),
                 errhint("Use the REFRESH MATERIALIZED VIEW command.")));

    return partrel;
}

#endif

/* ----------------------------------------------------------------
 *        ExecCloseScanRelation
 *
 *        Close the heap relation scanned by a base-level scan plan node.
 *        This should be called during the node's ExecEnd routine.
 *
 * Currently, we do not release the lock acquired by ExecOpenScanRelation.
 * This lock should be held till end of transaction.  (There is a faction
 * that considers this too much locking, however.)
 *
 * If we did want to release the lock, we'd have to repeat the logic in
 * ExecOpenScanRelation in order to figure out what to release.
 * ----------------------------------------------------------------
 */
void
ExecCloseScanRelation(Relation scanrel)
{
    heap_close(scanrel, NoLock);
}

/*
 * UpdateChangedParamSet
 *        Add changed parameters to a plan node's chgParam set
 */
void
UpdateChangedParamSet(PlanState *node, Bitmapset *newchg)
{
    Bitmapset  *parmset;

    /*
     * The plan node only depends on params listed in its allParam set. Don't
     * include anything else into its chgParam set.
     */
    parmset = bms_intersect(node->plan->allParam, newchg);

    /*
     * Keep node->chgParam == NULL if there's not actually any members; this
     * allows the simplest possible tests in executor node files.
     */
    if (!bms_is_empty(parmset))
        node->chgParam = bms_join(node->chgParam, parmset);
    else
        bms_free(parmset);
}

/*
 * executor_errposition
 *        Report an execution-time cursor position, if possible.
 *
 * This is expected to be used within an ereport() call.  The return value
 * is a dummy (always 0, in fact).
 *
 * The locations stored in parsetrees are byte offsets into the source string.
 * We have to convert them to 1-based character indexes for reporting to
 * clients.  (We do things this way to avoid unnecessary overhead in the
 * normal non-error case: computing character indexes would be much more
 * expensive than storing token offsets.)
 */
int
executor_errposition(EState *estate, int location)
{
    int            pos;

    /* No-op if location was not provided */
    if (location < 0)
        return 0;
    /* Can't do anything if source text is not available */
    if (estate == NULL || estate->es_sourceText == NULL)
        return 0;
    /* Convert offset to character number */
    pos = pg_mbstrlen_with_len(estate->es_sourceText, location) + 1;
    /* And pass it to the ereport mechanism */
    return errposition(pos);
}

/*
 * Register a shutdown callback in an ExprContext.
 *
 * Shutdown callbacks will be called (in reverse order of registration)
 * when the ExprContext is deleted or rescanned.  This provides a hook
 * for functions called in the context to do any cleanup needed --- it's
 * particularly useful for functions returning sets.  Note that the
 * callback will *not* be called in the event that execution is aborted
 * by an error.
 */
void
RegisterExprContextCallback(ExprContext *econtext,
                            ExprContextCallbackFunction function,
                            Datum arg)
{
    ExprContext_CB *ecxt_callback;

    /* Save the info in appropriate memory context */
    ecxt_callback = (ExprContext_CB *)
        MemoryContextAlloc(econtext->ecxt_per_query_memory,
                           sizeof(ExprContext_CB));

    ecxt_callback->function = function;
    ecxt_callback->arg = arg;

    /* link to front of list for appropriate execution order */
    ecxt_callback->next = econtext->ecxt_callbacks;
    econtext->ecxt_callbacks = ecxt_callback;
}

/*
 * Deregister a shutdown callback in an ExprContext.
 *
 * Any list entries matching the function and arg will be removed.
 * This can be used if it's no longer necessary to call the callback.
 */
void
UnregisterExprContextCallback(ExprContext *econtext,
                              ExprContextCallbackFunction function,
                              Datum arg)
{
    ExprContext_CB **prev_callback;
    ExprContext_CB *ecxt_callback;

    prev_callback = &econtext->ecxt_callbacks;

    while ((ecxt_callback = *prev_callback) != NULL)
    {
        if (ecxt_callback->function == function && ecxt_callback->arg == arg)
        {
            *prev_callback = ecxt_callback->next;
            pfree(ecxt_callback);
        }
        else
            prev_callback = &ecxt_callback->next;
    }
}

/*
 * Call all the shutdown callbacks registered in an ExprContext.
 *
 * The callback list is emptied (important in case this is only a rescan
 * reset, and not deletion of the ExprContext).
 *
 * If isCommit is false, just clean the callback list but don't call 'em.
 * (See comment for FreeExprContext.)
 */
static void
ShutdownExprContext(ExprContext *econtext, bool isCommit)
{
    ExprContext_CB *ecxt_callback;
    MemoryContext oldcontext;

    /* Fast path in normal case where there's nothing to do. */
    if (econtext->ecxt_callbacks == NULL)
        return;

    /*
     * Call the callbacks in econtext's per-tuple context.  This ensures that
     * any memory they might leak will get cleaned up.
     */
    oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

    /*
     * Call each callback function in reverse registration order.
     */
    while ((ecxt_callback = econtext->ecxt_callbacks) != NULL)
    {
        econtext->ecxt_callbacks = ecxt_callback->next;
        if (isCommit)
            (*ecxt_callback->function) (ecxt_callback->arg);
        pfree(ecxt_callback);
    }

    MemoryContextSwitchTo(oldcontext);
}

/*
 * ExecLockNonLeafAppendTables
 *
 * Locks, if necessary, the tables indicated by the RT indexes contained in
 * the partitioned_rels list.  These are the non-leaf tables in the partition
 * tree controlled by a given Append or MergeAppend node.
 */
void
ExecLockNonLeafAppendTables(List *partitioned_rels, EState *estate)
{
    PlannedStmt *stmt = estate->es_plannedstmt;
    ListCell   *lc;

    foreach(lc, partitioned_rels)
    {
        ListCell   *l;
        Index        rti = lfirst_int(lc);
        bool        is_result_rel = false;
        Oid            relid = getrelid(rti, estate->es_range_table);

        /* If this is a result relation, already locked in InitPlan */
        foreach(l, stmt->nonleafResultRelations)
        {
            if (rti == lfirst_int(l))
            {
                is_result_rel = true;
                break;
            }
        }

        /*
         * Not a result relation; check if there is a RowMark that requires
         * taking a RowShareLock on this rel.
         */
        if (!is_result_rel)
        {
            PlanRowMark *rc = NULL;

            foreach(l, stmt->rowMarks)
            {
                if (((PlanRowMark *) lfirst(l))->rti == rti)
                {
                    rc = lfirst(l);
                    break;
                }
            }

            if (rc && RowMarkRequiresRowShareLock(rc->markType))
                LockRelationOid(relid, RowShareLock);
            else
                LockRelationOid(relid, AccessShareLock);
        }
    }
}

/*
 *        GetAttributeByName
 *        GetAttributeByNum
 *
 *        These functions return the value of the requested attribute
 *        out of the given tuple Datum.
 *        C functions which take a tuple as an argument are expected
 *        to use these.  Ex: overpaid(EMP) might call GetAttributeByNum().
 *        Note: these are actually rather slow because they do a typcache
 *        lookup on each call.
 */
Datum
GetAttributeByName(HeapTupleHeader tuple, const char *attname, bool *isNull)
{
    AttrNumber    attrno;
    Datum        result;
    Oid            tupType;
    int32        tupTypmod;
    TupleDesc    tupDesc;
    HeapTupleData tmptup;
    int            i;

    if (attname == NULL)
        elog(ERROR, "invalid attribute name");

    if (isNull == NULL)
        elog(ERROR, "a NULL isNull pointer was passed");

    if (tuple == NULL)
    {
        /* Kinda bogus but compatible with old behavior... */
        *isNull = true;
        return (Datum) 0;
    }

    tupType = HeapTupleHeaderGetTypeId(tuple);
    tupTypmod = HeapTupleHeaderGetTypMod(tuple);
    tupDesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

    attrno = InvalidAttrNumber;
    for (i = 0; i < tupDesc->natts; i++)
    {
        if (namestrcmp(&(tupDesc->attrs[i]->attname), attname) == 0)
        {
            attrno = tupDesc->attrs[i]->attnum;
            break;
        }
    }

    if (attrno == InvalidAttrNumber)
        elog(ERROR, "attribute \"%s\" does not exist", attname);

    /*
     * heap_getattr needs a HeapTuple not a bare HeapTupleHeader.  We set all
     * the fields in the struct just in case user tries to inspect system
     * columns.
     */
    tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
    ItemPointerSetInvalid(&(tmptup.t_self));
    tmptup.t_tableOid = InvalidOid;
	tmptup.t_xc_node_id = InvalidOid;
    tmptup.t_data = tuple;

    result = heap_getattr(&tmptup,
                          attrno,
                          tupDesc,
                          isNull);

    ReleaseTupleDesc(tupDesc);

    return result;
}

Datum
GetAttributeByNum(HeapTupleHeader tuple,
                  AttrNumber attrno,
                  bool *isNull)
{
    Datum        result;
    Oid            tupType;
    int32        tupTypmod;
    TupleDesc    tupDesc;
    HeapTupleData tmptup;

    if (!AttributeNumberIsValid(attrno))
        elog(ERROR, "invalid attribute number %d", attrno);

    if (isNull == NULL)
        elog(ERROR, "a NULL isNull pointer was passed");

    if (tuple == NULL)
    {
        /* Kinda bogus but compatible with old behavior... */
        *isNull = true;
        return (Datum) 0;
    }

    tupType = HeapTupleHeaderGetTypeId(tuple);
    tupTypmod = HeapTupleHeaderGetTypMod(tuple);
    tupDesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

    /*
     * heap_getattr needs a HeapTuple not a bare HeapTupleHeader.  We set all
     * the fields in the struct just in case user tries to inspect system
     * columns.
     */
    tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
    ItemPointerSetInvalid(&(tmptup.t_self));
    tmptup.t_tableOid = InvalidOid;
	tmptup.t_xc_node_id = InvalidOid;
    tmptup.t_data = tuple;

    result = heap_getattr(&tmptup,
                          attrno,
                          tupDesc,
                          isNull);

    ReleaseTupleDesc(tupDesc);

    return result;
}

/*
 * Number of items in a tlist (including any resjunk items!)
 */
int
ExecTargetListLength(List *targetlist)
{
    /* This used to be more complex, but fjoins are dead */
    return list_length(targetlist);
}

/*
 * Number of items in a tlist, not including any resjunk items
 */
int
ExecCleanTargetListLength(List *targetlist)
{
    int            len = 0;
    ListCell   *tl;

    foreach(tl, targetlist)
    {
        TargetEntry *curTle = lfirst_node(TargetEntry, tl);

        if (!curTle->resjunk)
            len++;
    }
    return len;
}
