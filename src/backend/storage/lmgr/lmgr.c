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
 * lmgr.c
 *      POSTGRES lock manager code
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *      src/backend/storage/lmgr/lmgr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/inval.h"


/*
 * Per-backend counter for generating speculative insertion tokens.
 *
 * This may wrap around, but that's OK as it's only used for the short
 * duration between inserting a tuple and checking that there are no (unique)
 * constraint violations.  It's theoretically possible that a backend sees a
 * tuple that was speculatively inserted by another backend, but before it has
 * started waiting on the token, the other backend completes its insertion,
 * and then performs 2^32 unrelated insertions.  And after all that, the
 * first backend finally calls SpeculativeInsertionLockAcquire(), with the
 * intention of waiting for the first insertion to complete, but ends up
 * waiting for the latest unrelated insertion instead.  Even then, nothing
 * particularly bad happens: in the worst case they deadlock, causing one of
 * the transactions to abort.
 */
static uint32 speculativeInsertionToken = 0;


/*
 * Struct to hold context info for transaction lock waits.
 *
 * 'oper' is the operation that needs to wait for the other transaction; 'rel'
 * and 'ctid' specify the address of the tuple being waited for.
 */
typedef struct XactLockTableWaitInfo
{
    XLTW_Oper    oper;
    Relation    rel;
    ItemPointer ctid;
} XactLockTableWaitInfo;

static void XactLockTableWaitErrorCb(void *arg);

/*
 * RelationInitLockInfo
 *        Initializes the lock information in a relation descriptor.
 *
 *        relcache.c must call this during creation of any reldesc.
 */
void
RelationInitLockInfo(Relation relation)
{
    Assert(RelationIsValid(relation));
    Assert(OidIsValid(RelationGetRelid(relation)));

    relation->rd_lockInfo.lockRelId.relId = RelationGetRelid(relation);

    if (relation->rd_rel->relisshared)
        relation->rd_lockInfo.lockRelId.dbId = InvalidOid;
    else
        relation->rd_lockInfo.lockRelId.dbId = MyDatabaseId;
}

/*
 * SetLocktagRelationOid
 *        Set up a locktag for a relation, given only relation OID
 */
static inline void
SetLocktagRelationOid(LOCKTAG *tag, Oid relid)
{
    Oid            dbid;

    if (IsSharedRelation(relid))
        dbid = InvalidOid;
    else
        dbid = MyDatabaseId;

    SET_LOCKTAG_RELATION(*tag, dbid, relid);
}

/*
 *        LockRelationOid
 *
 * Lock a relation given only its OID.  This should generally be used
 * before attempting to open the relation's relcache entry.
 */
void
LockRelationOid(Oid relid, LOCKMODE lockmode)
{
    LOCKTAG        tag;
    LockAcquireResult res;

    SetLocktagRelationOid(&tag, relid);

    res = LockAcquire(&tag, lockmode, false, false);

    /*
     * Now that we have the lock, check for invalidation messages, so that we
     * will update or flush any stale relcache entry before we try to use it.
     * RangeVarGetRelid() specifically relies on us for this.  We can skip
     * this in the not-uncommon case that we already had the same type of lock
     * being requested, since then no one else could have modified the
     * relcache entry in an undesirable way.  (In the case where our own xact
     * modifies the rel, the relcache update happens via
     * CommandCounterIncrement, not here.)
     *
     * In Postgres-XL, multiple backends may run concurrently to serve a
     * distributed transaction. In that case, a conflicting lock may be granted
     * to another backend running the same distributed transaction. But it's
     * important that such backends process invalidation messages to ensure
     * that relcache entry modified by the other cooperating backend is truly
     * reflected. For example, the other backend may TRUNCATE the table and
     * change the relfilenode. So we must see that change and work with the new
     * relfilenode.
     */
    if ((res != LOCKACQUIRE_ALREADY_HELD) ||
        (MyProc->coordPid && MyProc->coordId))
        AcceptInvalidationMessages();
}

/*
 *        ConditionalLockRelationOid
 *
 * As above, but only lock if we can get the lock without blocking.
 * Returns TRUE iff the lock was acquired.
 *
 * NOTE: we do not currently need conditional versions of all the
 * LockXXX routines in this file, but they could easily be added if needed.
 */
bool
ConditionalLockRelationOid(Oid relid, LOCKMODE lockmode)
{
    LOCKTAG        tag;
    LockAcquireResult res;

    SetLocktagRelationOid(&tag, relid);

    res = LockAcquire(&tag, lockmode, false, true);

    if (res == LOCKACQUIRE_NOT_AVAIL)
        return false;

    /*
     * Now that we have the lock, check for invalidation messages; see notes
     * in LockRelationOid.
     */
    if (res != LOCKACQUIRE_ALREADY_HELD)
        AcceptInvalidationMessages();

    return true;
}

/*
 *        UnlockRelationId
 *
 * Unlock, given a LockRelId.  This is preferred over UnlockRelationOid
 * for speed reasons.
 */
void
UnlockRelationId(LockRelId *relid, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_RELATION(tag, relid->dbId, relid->relId);

    LockRelease(&tag, lockmode, false);
}

/*
 *        UnlockRelationOid
 *
 * Unlock, given only a relation Oid.  Use UnlockRelationId if you can.
 */
void
UnlockRelationOid(Oid relid, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SetLocktagRelationOid(&tag, relid);

    LockRelease(&tag, lockmode, false);
}

/*
 *        LockRelation
 *
 * This is a convenience routine for acquiring an additional lock on an
 * already-open relation.  Never try to do "relation_open(foo, NoLock)"
 * and then lock with this.
 */
void
LockRelation(Relation relation, LOCKMODE lockmode)
{
    LOCKTAG        tag;
    LockAcquireResult res;

    SET_LOCKTAG_RELATION(tag,
                         relation->rd_lockInfo.lockRelId.dbId,
                         relation->rd_lockInfo.lockRelId.relId);

    res = LockAcquire(&tag, lockmode, false, false);

    /*
     * Now that we have the lock, check for invalidation messages; see notes
     * in LockRelationOid.
     */
    if (res != LOCKACQUIRE_ALREADY_HELD)
        AcceptInvalidationMessages();
}

/*
 *        ConditionalLockRelation
 *
 * This is a convenience routine for acquiring an additional lock on an
 * already-open relation.  Never try to do "relation_open(foo, NoLock)"
 * and then lock with this.
 */
bool
ConditionalLockRelation(Relation relation, LOCKMODE lockmode)
{
    LOCKTAG        tag;
    LockAcquireResult res;

    SET_LOCKTAG_RELATION(tag,
                         relation->rd_lockInfo.lockRelId.dbId,
                         relation->rd_lockInfo.lockRelId.relId);

    res = LockAcquire(&tag, lockmode, false, true);

    if (res == LOCKACQUIRE_NOT_AVAIL)
        return false;

    /*
     * Now that we have the lock, check for invalidation messages; see notes
     * in LockRelationOid.
     */
    if (res != LOCKACQUIRE_ALREADY_HELD)
        AcceptInvalidationMessages();

    return true;
}

/*
 *        UnlockRelation
 *
 * This is a convenience routine for unlocking a relation without also
 * closing it.
 */
void
UnlockRelation(Relation relation, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_RELATION(tag,
                         relation->rd_lockInfo.lockRelId.dbId,
                         relation->rd_lockInfo.lockRelId.relId);

    LockRelease(&tag, lockmode, false);
}

/*
 *        LockHasWaitersRelation
 *
 * This is a function to check whether someone else is waiting for a
 * lock which we are currently holding.
 */
bool
LockHasWaitersRelation(Relation relation, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_RELATION(tag,
                         relation->rd_lockInfo.lockRelId.dbId,
                         relation->rd_lockInfo.lockRelId.relId);

    return LockHasWaiters(&tag, lockmode, false);
}

/*
 *        LockRelationIdForSession
 *
 * This routine grabs a session-level lock on the target relation.  The
 * session lock persists across transaction boundaries.  It will be removed
 * when UnlockRelationIdForSession() is called, or if an ereport(ERROR) occurs,
 * or if the backend exits.
 *
 * Note that one should also grab a transaction-level lock on the rel
 * in any transaction that actually uses the rel, to ensure that the
 * relcache entry is up to date.
 */
void
LockRelationIdForSession(LockRelId *relid, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_RELATION(tag, relid->dbId, relid->relId);

    (void) LockAcquire(&tag, lockmode, true, false);
}

/*
 *        UnlockRelationIdForSession
 */
void
UnlockRelationIdForSession(LockRelId *relid, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_RELATION(tag, relid->dbId, relid->relId);

    LockRelease(&tag, lockmode, true);
}

/*
 *        LockRelationForExtension
 *
 * This lock tag is used to interlock addition of pages to relations.
 * We need such locking because bufmgr/smgr definition of P_NEW is not
 * race-condition-proof.
 *
 * We assume the caller is already holding some type of regular lock on
 * the relation, so no AcceptInvalidationMessages call is needed here.
 */
void
LockRelationForExtension(Relation relation, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_RELATION_EXTEND(tag,
                                relation->rd_lockInfo.lockRelId.dbId,
                                relation->rd_lockInfo.lockRelId.relId);

    (void) LockAcquire(&tag, lockmode, false, false);
}

/*
 *        ConditionalLockRelationForExtension
 *
 * As above, but only lock if we can get the lock without blocking.
 * Returns TRUE iff the lock was acquired.
 */
bool
ConditionalLockRelationForExtension(Relation relation, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_RELATION_EXTEND(tag,
                                relation->rd_lockInfo.lockRelId.dbId,
                                relation->rd_lockInfo.lockRelId.relId);

    return (LockAcquire(&tag, lockmode, false, true) != LOCKACQUIRE_NOT_AVAIL);
}

/*
 *        RelationExtensionLockWaiterCount
 *
 * Count the number of processes waiting for the given relation extension lock.
 */
int
RelationExtensionLockWaiterCount(Relation relation)
{
    LOCKTAG        tag;

    SET_LOCKTAG_RELATION_EXTEND(tag,
                                relation->rd_lockInfo.lockRelId.dbId,
                                relation->rd_lockInfo.lockRelId.relId);

    return LockWaiterCount(&tag);
}

/*
 *        UnlockRelationForExtension
 */
void
UnlockRelationForExtension(Relation relation, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_RELATION_EXTEND(tag,
                                relation->rd_lockInfo.lockRelId.dbId,
                                relation->rd_lockInfo.lockRelId.relId);

    LockRelease(&tag, lockmode, false);
}

/*
 *        LockPage
 *
 * Obtain a page-level lock.  This is currently used by some index access
 * methods to lock individual index pages.
 */
void
LockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_PAGE(tag,
                     relation->rd_lockInfo.lockRelId.dbId,
                     relation->rd_lockInfo.lockRelId.relId,
                     blkno);

    (void) LockAcquire(&tag, lockmode, false, false);
}

/*
 *        ConditionalLockPage
 *
 * As above, but only lock if we can get the lock without blocking.
 * Returns TRUE iff the lock was acquired.
 */
bool
ConditionalLockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_PAGE(tag,
                     relation->rd_lockInfo.lockRelId.dbId,
                     relation->rd_lockInfo.lockRelId.relId,
                     blkno);

    return (LockAcquire(&tag, lockmode, false, true) != LOCKACQUIRE_NOT_AVAIL);
}

/*
 *        UnlockPage
 */
void
UnlockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_PAGE(tag,
                     relation->rd_lockInfo.lockRelId.dbId,
                     relation->rd_lockInfo.lockRelId.relId,
                     blkno);

    LockRelease(&tag, lockmode, false);
}

/*
 *        LockTuple
 *
 * Obtain a tuple-level lock.  This is used in a less-than-intuitive fashion
 * because we can't afford to keep a separate lock in shared memory for every
 * tuple.  See heap_lock_tuple before using this!
 */
void
LockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_TUPLE(tag,
                      relation->rd_lockInfo.lockRelId.dbId,
                      relation->rd_lockInfo.lockRelId.relId,
                      ItemPointerGetBlockNumber(tid),
                      ItemPointerGetOffsetNumber(tid));

    (void) LockAcquire(&tag, lockmode, false, false);
}

/*
 *        ConditionalLockTuple
 *
 * As above, but only lock if we can get the lock without blocking.
 * Returns TRUE iff the lock was acquired.
 */
bool
ConditionalLockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_TUPLE(tag,
                      relation->rd_lockInfo.lockRelId.dbId,
                      relation->rd_lockInfo.lockRelId.relId,
                      ItemPointerGetBlockNumber(tid),
                      ItemPointerGetOffsetNumber(tid));

    return (LockAcquire(&tag, lockmode, false, true) != LOCKACQUIRE_NOT_AVAIL);
}

/*
 *        UnlockTuple
 */
void
UnlockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_TUPLE(tag,
                      relation->rd_lockInfo.lockRelId.dbId,
                      relation->rd_lockInfo.lockRelId.relId,
                      ItemPointerGetBlockNumber(tid),
                      ItemPointerGetOffsetNumber(tid));

    LockRelease(&tag, lockmode, false);
}

/*
 *        XactLockTableInsert
 *
 * Insert a lock showing that the given transaction ID is running ---
 * this is done when an XID is acquired by a transaction or subtransaction.
 * The lock can then be used to wait for the transaction to finish.
 */
void
XactLockTableInsert(TransactionId xid)
{
    LOCKTAG        tag;

    SET_LOCKTAG_TRANSACTION(tag, xid);

    (void) LockAcquire(&tag, ExclusiveLock, false, false);
}

/*
 *        XactLockTableDelete
 *
 * Delete the lock showing that the given transaction ID is running.
 * (This is never used for main transaction IDs; those locks are only
 * released implicitly at transaction end.  But we do use it for subtrans IDs.)
 */
void
XactLockTableDelete(TransactionId xid)
{
    LOCKTAG        tag;

    SET_LOCKTAG_TRANSACTION(tag, xid);

    LockRelease(&tag, ExclusiveLock, false);
}

/*
 *        XactLockTableWait
 *
 * Wait for the specified transaction to commit or abort.  If an operation
 * is specified, an error context callback is set up.  If 'oper' is passed as
 * None, no error context callback is set up.
 *
 * Note that this does the right thing for subtransactions: if we wait on a
 * subtransaction, we will exit as soon as it aborts or its top parent commits.
 * It takes some extra work to ensure this, because to save on shared memory
 * the XID lock of a subtransaction is released when it ends, whether
 * successfully or unsuccessfully.  So we have to check if it's "still running"
 * and if so wait for its parent.
 */
void
XactLockTableWait(TransactionId xid, Relation rel, ItemPointer ctid,
                  XLTW_Oper oper)
{
    LOCKTAG        tag;
    XactLockTableWaitInfo info;
    ErrorContextCallback callback;

    /*
     * If an operation is specified, set up our verbose error context
     * callback.
     */
    if (oper != XLTW_None)
    {
        Assert(RelationIsValid(rel));
        Assert(ItemPointerIsValid(ctid));

        info.rel = rel;
        info.ctid = ctid;
        info.oper = oper;

        callback.callback = XactLockTableWaitErrorCb;
        callback.arg = &info;
        callback.previous = error_context_stack;
        error_context_stack = &callback;
    }

#ifdef XCP
    TransactionRecordXidWait(xid);    
#endif

    for (;;)
    {
        Assert(TransactionIdIsValid(xid));
        Assert(!TransactionIdEquals(xid, GetTopTransactionIdIfAny()));

        SET_LOCKTAG_TRANSACTION(tag, xid);

        (void) LockAcquire(&tag, ShareLock, false, false);

        LockRelease(&tag, ShareLock, false);

        if (!TransactionIdIsInProgress(xid))
            break;
        xid = SubTransGetParent(xid);
    }

    if (oper != XLTW_None)
        error_context_stack = callback.previous;
}

/*
 *        ConditionalXactLockTableWait
 *
 * As above, but only lock if we can get the lock without blocking.
 * Returns TRUE if the lock was acquired.
 */
bool
ConditionalXactLockTableWait(TransactionId xid)
{
    LOCKTAG        tag;

    for (;;)
    {
        Assert(TransactionIdIsValid(xid));
        Assert(!TransactionIdEquals(xid, GetTopTransactionIdIfAny()));

        SET_LOCKTAG_TRANSACTION(tag, xid);

        if (LockAcquire(&tag, ShareLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
            return false;

        LockRelease(&tag, ShareLock, false);

        if (!TransactionIdIsInProgress(xid))
            break;
        xid = SubTransGetParent(xid);
    }

    return true;
}

/*
 *        SpeculativeInsertionLockAcquire
 *
 * Insert a lock showing that the given transaction ID is inserting a tuple,
 * but hasn't yet decided whether it's going to keep it.  The lock can then be
 * used to wait for the decision to go ahead with the insertion, or aborting
 * it.
 *
 * The token is used to distinguish multiple insertions by the same
 * transaction.  It is returned to caller.
 */
uint32
SpeculativeInsertionLockAcquire(TransactionId xid)
{
    LOCKTAG        tag;

    speculativeInsertionToken++;

    /*
     * Check for wrap-around. Zero means no token is held, so don't use that.
     */
    if (speculativeInsertionToken == 0)
        speculativeInsertionToken = 1;

    SET_LOCKTAG_SPECULATIVE_INSERTION(tag, xid, speculativeInsertionToken);

    (void) LockAcquire(&tag, ExclusiveLock, false, false);

    return speculativeInsertionToken;
}

/*
 *        SpeculativeInsertionLockRelease
 *
 * Delete the lock showing that the given transaction is speculatively
 * inserting a tuple.
 */
void
SpeculativeInsertionLockRelease(TransactionId xid)
{
    LOCKTAG        tag;

    SET_LOCKTAG_SPECULATIVE_INSERTION(tag, xid, speculativeInsertionToken);

    LockRelease(&tag, ExclusiveLock, false);
}

/*
 *        SpeculativeInsertionWait
 *
 * Wait for the specified transaction to finish or abort the insertion of a
 * tuple.
 */
void
SpeculativeInsertionWait(TransactionId xid, uint32 token)
{
    LOCKTAG        tag;

    SET_LOCKTAG_SPECULATIVE_INSERTION(tag, xid, token);

    Assert(TransactionIdIsValid(xid));
    Assert(token != 0);

    (void) LockAcquire(&tag, ShareLock, false, false);
    LockRelease(&tag, ShareLock, false);
}

/*
 * XactLockTableWaitErrorContextCb
 *        Error context callback for transaction lock waits.
 */
static void
XactLockTableWaitErrorCb(void *arg)
{// #lizard forgives
    XactLockTableWaitInfo *info = (XactLockTableWaitInfo *) arg;

    /*
     * We would like to print schema name too, but that would require a
     * syscache lookup.
     */
    if (info->oper != XLTW_None &&
        ItemPointerIsValid(info->ctid) && RelationIsValid(info->rel))
    {
        const char *cxt;

        switch (info->oper)
        {
            case XLTW_Update:
                cxt = gettext_noop("while updating tuple (%u,%u) in relation \"%s\"");
                break;
            case XLTW_Delete:
                cxt = gettext_noop("while deleting tuple (%u,%u) in relation \"%s\"");
                break;
            case XLTW_Lock:
                cxt = gettext_noop("while locking tuple (%u,%u) in relation \"%s\"");
                break;
            case XLTW_LockUpdated:
                cxt = gettext_noop("while locking updated version (%u,%u) of tuple in relation \"%s\"");
                break;
            case XLTW_InsertIndex:
                cxt = gettext_noop("while inserting index tuple (%u,%u) in relation \"%s\"");
                break;
            case XLTW_InsertIndexUnique:
                cxt = gettext_noop("while checking uniqueness of tuple (%u,%u) in relation \"%s\"");
                break;
            case XLTW_FetchUpdated:
                cxt = gettext_noop("while rechecking updated tuple (%u,%u) in relation \"%s\"");
                break;
            case XLTW_RecheckExclusionConstr:
                cxt = gettext_noop("while checking exclusion constraint on tuple (%u,%u) in relation \"%s\"");
                break;

            default:
                return;
        }

        errcontext(cxt,
                   ItemPointerGetBlockNumber(info->ctid),
                   ItemPointerGetOffsetNumber(info->ctid),
                   RelationGetRelationName(info->rel));
    }
}

/*
 * WaitForLockersMultiple
 *        Wait until no transaction holds locks that conflict with the given
 *        locktags at the given lockmode.
 *
 * To do this, obtain the current list of lockers, and wait on their VXIDs
 * until they are finished.
 *
 * Note we don't try to acquire the locks on the given locktags, only the VXIDs
 * of its lock holders; if somebody grabs a conflicting lock on the objects
 * after we obtained our initial list of lockers, we will not wait for them.
 */
void
WaitForLockersMultiple(List *locktags, LOCKMODE lockmode)
{
    List       *holders = NIL;
    ListCell   *lc;

    /* Done if no locks to wait for */
    if (list_length(locktags) == 0)
        return;

    /* Collect the transactions we need to wait on */
    foreach(lc, locktags)
    {
        LOCKTAG    *locktag = lfirst(lc);

        holders = lappend(holders, GetLockConflicts(locktag, lockmode));
    }

    /*
     * Note: GetLockConflicts() never reports our own xid, hence we need not
     * check for that.  Also, prepared xacts are not reported, which is fine
     * since they certainly aren't going to do anything anymore.
     */

    /* Finally wait for each such transaction to complete */
    foreach(lc, holders)
    {
        VirtualTransactionId *lockholders = lfirst(lc);

        while (VirtualTransactionIdIsValid(*lockholders))
        {
            VirtualXactLock(*lockholders, true);
            lockholders++;
        }
    }

    list_free_deep(holders);
}

/*
 * WaitForLockers
 *
 * Same as WaitForLockersMultiple, for a single lock tag.
 */
void
WaitForLockers(LOCKTAG heaplocktag, LOCKMODE lockmode)
{
    List       *l;

    l = list_make1(&heaplocktag);
    WaitForLockersMultiple(l, lockmode);
    list_free(l);
}


/*
 *        LockDatabaseObject
 *
 * Obtain a lock on a general object of the current database.  Don't use
 * this for shared objects (such as tablespaces).  It's unwise to apply it
 * to relations, also, since a lock taken this way will NOT conflict with
 * locks taken via LockRelation and friends.
 */
void
LockDatabaseObject(Oid classid, Oid objid, uint16 objsubid,
                   LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_OBJECT(tag,
                       MyDatabaseId,
                       classid,
                       objid,
                       objsubid);

    (void) LockAcquire(&tag, lockmode, false, false);

    /* Make sure syscaches are up-to-date with any changes we waited for */
    AcceptInvalidationMessages();
}

/*
 *        UnlockDatabaseObject
 */
void
UnlockDatabaseObject(Oid classid, Oid objid, uint16 objsubid,
                     LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_OBJECT(tag,
                       MyDatabaseId,
                       classid,
                       objid,
                       objsubid);

    LockRelease(&tag, lockmode, false);
}

/*
 *        LockSharedObject
 *
 * Obtain a lock on a shared-across-databases object.
 */
void
LockSharedObject(Oid classid, Oid objid, uint16 objsubid,
                 LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_OBJECT(tag,
                       InvalidOid,
                       classid,
                       objid,
                       objsubid);

    (void) LockAcquire(&tag, lockmode, false, false);

    /* Make sure syscaches are up-to-date with any changes we waited for */
    AcceptInvalidationMessages();
}

/*
 *        UnlockSharedObject
 */
void
UnlockSharedObject(Oid classid, Oid objid, uint16 objsubid,
                   LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_OBJECT(tag,
                       InvalidOid,
                       classid,
                       objid,
                       objsubid);

    LockRelease(&tag, lockmode, false);
}

/*
 *        LockSharedObjectForSession
 *
 * Obtain a session-level lock on a shared-across-databases object.
 * See LockRelationIdForSession for notes about session-level locks.
 */
void
LockSharedObjectForSession(Oid classid, Oid objid, uint16 objsubid,
                           LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_OBJECT(tag,
                       InvalidOid,
                       classid,
                       objid,
                       objsubid);

    (void) LockAcquire(&tag, lockmode, true, false);
}

/*
 *        UnlockSharedObjectForSession
 */
void
UnlockSharedObjectForSession(Oid classid, Oid objid, uint16 objsubid,
                             LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_OBJECT(tag,
                       InvalidOid,
                       classid,
                       objid,
                       objsubid);

    LockRelease(&tag, lockmode, true);
}

#ifdef _SHARDING_
void
LockShard(Relation relation, uint32 shardid, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_SHARD(tag,
                    relation->rd_lockInfo.lockRelId.dbId,
                    relation->rd_lockInfo.lockRelId.relId,
                    shardid);

    (void) LockAcquire(&tag, lockmode, false, false);

    AcceptInvalidationMessages();
}

void
UnlockShard(Relation relation, uint32 shardid, LOCKMODE lockmode)
{
    LOCKTAG        tag;

    SET_LOCKTAG_SHARD(tag,
                        relation->rd_lockInfo.lockRelId.dbId,
                        relation->rd_lockInfo.lockRelId.relId,
                        shardid);
    
    LockRelease(&tag, lockmode, false);
}
#endif

/*
 * Append a description of a lockable object to buf.
 *
 * Ideally we would print names for the numeric values, but that requires
 * getting locks on system tables, which might cause problems since this is
 * typically used to report deadlock situations.
 */
void
DescribeLockTag(StringInfo buf, const LOCKTAG *tag)
{// #lizard forgives
    switch ((LockTagType) tag->locktag_type)
    {
        case LOCKTAG_RELATION:
            appendStringInfo(buf,
                             _("relation %u of database %u"),
                             tag->locktag_field2,
                             tag->locktag_field1);
            break;
        case LOCKTAG_RELATION_EXTEND:
            appendStringInfo(buf,
                             _("extension of relation %u of database %u"),
                             tag->locktag_field2,
                             tag->locktag_field1);
            break;
        case LOCKTAG_PAGE:
            appendStringInfo(buf,
                             _("page %u of relation %u of database %u"),
                             tag->locktag_field3,
                             tag->locktag_field2,
                             tag->locktag_field1);
            break;
        case LOCKTAG_TUPLE:
            appendStringInfo(buf,
                             _("tuple (%u,%u) of relation %u of database %u"),
                             tag->locktag_field3,
                             tag->locktag_field4,
                             tag->locktag_field2,
                             tag->locktag_field1);
            break;
        case LOCKTAG_TRANSACTION:
            appendStringInfo(buf,
                             _("transaction %u"),
                             tag->locktag_field1);
            break;
        case LOCKTAG_VIRTUALTRANSACTION:
            appendStringInfo(buf,
                             _("virtual transaction %d/%u"),
                             tag->locktag_field1,
                             tag->locktag_field2);
            break;
        case LOCKTAG_SPECULATIVE_TOKEN:
            appendStringInfo(buf,
                             _("speculative token %u of transaction %u"),
                             tag->locktag_field2,
                             tag->locktag_field1);
            break;
        case LOCKTAG_OBJECT:
            appendStringInfo(buf,
                             _("object %u of class %u of database %u"),
                             tag->locktag_field3,
                             tag->locktag_field2,
                             tag->locktag_field1);
            break;
        case LOCKTAG_USERLOCK:
            /* reserved for old contrib code, now on pgfoundry */
            appendStringInfo(buf,
                             _("user lock [%u,%u,%u]"),
                             tag->locktag_field1,
                             tag->locktag_field2,
                             tag->locktag_field3);
            break;
        case LOCKTAG_ADVISORY:
            appendStringInfo(buf,
                             _("advisory lock [%u,%u,%u,%u]"),
                             tag->locktag_field1,
                             tag->locktag_field2,
                             tag->locktag_field3,
                             tag->locktag_field4);
            break;
        default:
            appendStringInfo(buf,
                             _("unrecognized locktag type %d"),
                             (int) tag->locktag_type);
            break;
    }
}

/*
 * GetLockNameFromTagType
 *
 *    Given locktag type, return the corresponding lock name.
 */
const char *
GetLockNameFromTagType(uint16 locktag_type)
{
    if (locktag_type > LOCKTAG_LAST_TYPE)
        return "???";
    return LockTagTypeNames[locktag_type];
}
