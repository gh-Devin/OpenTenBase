/*-------------------------------------------------------------------------
 *
 * subtrans.c
 *        PostgreSQL subtransaction-log manager
 *
 * The pg_subtrans manager is a pg_xact-like manager that stores the parent
 * transaction Id for each transaction.  It is a fundamental part of the
 * nested transactions implementation.  A main transaction has a parent
 * of InvalidTransactionId, and each subtransaction has its immediate parent.
 * The tree can easily be walked from child to parent, but not in the
 * opposite direction.
 *
 * This code is based on xact.c, but the robustness requirements
 * are completely different from pg_xact, because we only need to remember
 * pg_subtrans information for currently-open transactions.  Thus, there is
 * no need to preserve data over a crash and restart.
 *
 * There are no XLOG interactions since we do not care about preserving
 * data across crashes.  During database startup, we simply force the
 * currently-active page of SUBTRANS to zeroes.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 *
 * This source code file contains modifications made by THL A29 Limited ("Tencent Modifications").
 * All Tencent Modifications are Copyright (C) 2023 THL A29 Limited.
 *
 * src/backend/access/transam/subtrans.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/slru.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "pg_trace.h"
#include "utils/snapmgr.h"

/*
 * Defines for SubTrans page sizes.  A page is the same BLCKSZ as is used
 * everywhere else in Postgres.
 *
 * Note: because TransactionIds are 32 bits and wrap around at 0xFFFFFFFF,
 * SubTrans page numbering also wraps around at
 * 0xFFFFFFFF/SUBTRANS_XACTS_PER_PAGE, and segment numbering at
 * 0xFFFFFFFF/SUBTRANS_XACTS_PER_PAGE/SLRU_PAGES_PER_SEGMENT.  We need take no
 * explicit notice of that fact in this module, except when comparing segment
 * and page numbers in TruncateSUBTRANS (see SubTransPagePrecedes) and zeroing
 * them in StartupSUBTRANS.
 */

/* We need four bytes per xact */
#define SUBTRANS_XACTS_PER_PAGE (BLCKSZ / sizeof(TransactionId))

#define TransactionIdToPage(xid) ((xid) / (TransactionId) SUBTRANS_XACTS_PER_PAGE)
#define TransactionIdToEntry(xid) ((xid) % (TransactionId) SUBTRANS_XACTS_PER_PAGE)


/*
 * Link to shared-memory data structures for SUBTRANS control
 */
static SlruCtlData SubTransCtlData;

#define SubTransCtl  (&SubTransCtlData)


static int    ZeroSUBTRANSPage(int pageno);
static bool SubTransPagePrecedes(int page1, int page2);


/*
 * Record the parent of a subtransaction in the subtrans log.
 */
void
SubTransSetParent(TransactionId xid, TransactionId parent)
{
    int            pageno = TransactionIdToPage(xid);
    int            entryno = TransactionIdToEntry(xid);
    int            slotno;
    TransactionId *ptr;

    Assert(TransactionIdIsValid(parent));
    Assert(TransactionIdFollows(xid, parent));

    LWLockAcquire(SubtransControlLock, LW_EXCLUSIVE);

    slotno = SimpleLruReadPage(SubTransCtl, pageno, true, xid);
    ptr = (TransactionId *) SubTransCtl->shared->page_buffer[slotno];
    ptr += entryno;

    /*
     * It's possible we'll try to set the parent xid multiple times but we
     * shouldn't ever be changing the xid from one valid xid to another valid
     * xid, which would corrupt the data structure.
     */
    if (*ptr != parent)
    {
        Assert(*ptr == InvalidTransactionId);
		SlruClogDisableMemoryProtection(SubTransCtl->shared->page_buffer[slotno]);
        *ptr = parent;
		SlruClogEnableMemoryProtection(SubTransCtl->shared->page_buffer[slotno]);
        SubTransCtl->shared->page_dirty[slotno] = true;
    }

    LWLockRelease(SubtransControlLock);
}

/*
 * Interrogate the parent of a transaction in the subtrans log.
 */
TransactionId
SubTransGetParent(TransactionId xid)
{
    int            pageno = TransactionIdToPage(xid);
    int            entryno = TransactionIdToEntry(xid);
    int            slotno;
    TransactionId *ptr;
    TransactionId parent;

    /* Can't ask about stuff that might not be around anymore */
    Assert(TransactionIdFollowsOrEquals(xid, TransactionXmin));

    /* Bootstrap and frozen XIDs have no parent */
    if (!TransactionIdIsNormal(xid))
        return InvalidTransactionId;

    /* lock is acquired by SimpleLruReadPage_ReadOnly */

    slotno = SimpleLruReadPage_ReadOnly(SubTransCtl, pageno, xid);
    ptr = (TransactionId *) SubTransCtl->shared->page_buffer[slotno];
    ptr += entryno;

    parent = *ptr;

    LWLockRelease(SubtransControlLock);

    return parent;
}

/*
 * SubTransGetTopmostTransaction
 *
 * Returns the topmost transaction of the given transaction id.
 *
 * Because we cannot look back further than TransactionXmin, it is possible
 * that this function will lie and return an intermediate subtransaction ID
 * instead of the true topmost parent ID.  This is OK, because in practice
 * we only care about detecting whether the topmost parent is still running
 * or is part of a current snapshot's list of still-running transactions.
 * Therefore, any XID before TransactionXmin is as good as any other.
 */
TransactionId
SubTransGetTopmostTransaction(TransactionId xid)
{
    TransactionId parentXid = xid,
                previousXid = xid;

    /* Can't ask about stuff that might not be around anymore */
    Assert(TransactionIdFollowsOrEquals(xid, TransactionXmin));

    while (TransactionIdIsValid(parentXid))
    {
        previousXid = parentXid;
        if (TransactionIdPrecedes(parentXid, TransactionXmin))
            break;
        parentXid = SubTransGetParent(parentXid);

        /*
         * By convention the parent xid gets allocated first, so should always
         * precede the child xid. Anything else points to a corrupted data
         * structure that could lead to an infinite loop, so exit.
         */
        if (!TransactionIdPrecedes(parentXid, previousXid))
            elog(ERROR, "pg_subtrans contains invalid entry: xid %u points to parent xid %u",
                 previousXid, parentXid);
    }

    Assert(TransactionIdIsValid(previousXid));

    return previousXid;
}


/*
 * Initialization of shared memory for SUBTRANS
 */
Size
SUBTRANSShmemSize(void)
{
    return SimpleLruShmemSize(NUM_SUBTRANS_BUFFERS, 0);
}

void
SUBTRANSShmemInit(void)
{
    SubTransCtl->PagePrecedes = SubTransPagePrecedes;
    SimpleLruInit(SubTransCtl, "subtrans", NUM_SUBTRANS_BUFFERS, 0,
                  SubtransControlLock, "pg_subtrans",
                  LWTRANCHE_SUBTRANS_BUFFERS);
    /* Override default assumption that writes should be fsync'd */
    SubTransCtl->do_fsync = false;
}

/*
 * This func must be called ONCE on system install.  It creates
 * the initial SUBTRANS segment.  (The SUBTRANS directory is assumed to
 * have been created by the initdb shell script, and SUBTRANSShmemInit
 * must have been called already.)
 *
 * Note: it's not really necessary to create the initial segment now,
 * since slru.c would create it on first write anyway.  But we may as well
 * do it to be sure the directory is set up correctly.
 */
void
BootStrapSUBTRANS(void)
{
    int            slotno;

    LWLockAcquire(SubtransControlLock, LW_EXCLUSIVE);

    /* Create and zero the first page of the subtrans log */
    slotno = ZeroSUBTRANSPage(0);

    /* Make sure it's written out */
    SimpleLruWritePage(SubTransCtl, slotno);
    Assert(!SubTransCtl->shared->page_dirty[slotno]);

    LWLockRelease(SubtransControlLock);
}

/*
 * Initialize (or reinitialize) a page of SUBTRANS to zeroes.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
ZeroSUBTRANSPage(int pageno)
{
    return SimpleLruZeroPage(SubTransCtl, pageno);
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup,
 * after StartupXLOG has initialized ShmemVariableCache->nextXid.
 *
 * oldestActiveXID is the oldest XID of any prepared transaction, or nextXid
 * if there are none.
 */
void
StartupSUBTRANS(TransactionId oldestActiveXID)
{
    int            startPage;
    int            endPage;

    /*
     * Since we don't expect pg_subtrans to be valid across crashes, we
     * initialize the currently-active page(s) to zeroes during startup.
     * Whenever we advance into a new page, ExtendSUBTRANS will likewise zero
     * the new page without regard to whatever was previously on disk.
     */
    LWLockAcquire(SubtransControlLock, LW_EXCLUSIVE);

    startPage = TransactionIdToPage(oldestActiveXID);
    endPage = TransactionIdToPage(ShmemVariableCache->nextXid);

    while (startPage != endPage)
    {
        (void) ZeroSUBTRANSPage(startPage);
        startPage++;
        /* must account for wraparound */
        if (startPage > TransactionIdToPage(MaxTransactionId))
            startPage = 0;
    }
    (void) ZeroSUBTRANSPage(startPage);

    LWLockRelease(SubtransControlLock);
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
ShutdownSUBTRANS(void)
{
    /*
     * Flush dirty SUBTRANS pages to disk
     *
     * This is not actually necessary from a correctness point of view. We do
     * it merely as a debugging aid.
     */
    TRACE_POSTGRESQL_SUBTRANS_CHECKPOINT_START(false);
    SimpleLruFlush(SubTransCtl, false);
    TRACE_POSTGRESQL_SUBTRANS_CHECKPOINT_DONE(false);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointSUBTRANS(void)
{
    /*
     * Flush dirty SUBTRANS pages to disk
     *
     * This is not actually necessary from a correctness point of view. We do
     * it merely to improve the odds that writing of dirty pages is done by
     * the checkpoint process and not by backends.
     */
    TRACE_POSTGRESQL_SUBTRANS_CHECKPOINT_START(true);
    SimpleLruFlush(SubTransCtl, true);
    TRACE_POSTGRESQL_SUBTRANS_CHECKPOINT_DONE(true);
}

/*
 * Make sure that SUBTRANS has room for a newly-allocated XID.
 *
 * NB: this is called while holding XidGenLock.  We want it to be very fast
 * most of the time; even when it's not so fast, no actual I/O need happen
 * unless we're forced to write out a dirty subtrans page to make room
 * in shared memory.
 */
void
ExtendSUBTRANS(TransactionId newestXact)
{// #lizard forgives
    int            pageno;
    TransactionId latestSubXid;

    /*
     * No work except at first XID of a page.  But beware: just after
     * wraparound, the first XID of page zero is FirstNormalTransactionId.
     */
#ifdef PGXC  /* PGXC_COORD || PGXC_DATANODE */
    /* 
     * In PGXC, it may be that a node is not involved in a transaction,
     * and therefore will be skipped, so we need to detect this by using
     * the latest_page_number instead of the pg index.
     *
     * latest_page_number always points to the last page of SubtransLog. We
     * don't need to do anything for an XID that maps to a page that precedes
     * or equals the latest_page_number. To handle wrap-around correctly, we
     * just compute the last XID mapped to latest_page_number and compare that
     * against the passed in XID.
     */
    pageno = TransactionIdToPage(newestXact);

    /* 
     * The first condition makes sure we did not wrap around 
     * The second checks if we are still using the same page.
     * Note that this value can change and we are not holding a lock, 
     * so we repeat the check below. We do it this way instead of 
     * grabbing the lock to avoid lock contention.
     */
    latestSubXid = (SubTransCtl->shared->latest_page_number *
            SUBTRANS_XACTS_PER_PAGE) + SUBTRANS_XACTS_PER_PAGE - 1;
    if (TransactionIdPrecedesOrEquals(newestXact, latestSubXid))
        return;
#else
    if (TransactionIdToEntry(newestXact) != 0 &&
        !TransactionIdEquals(newestXact, FirstNormalTransactionId))
        return;

    pageno = TransactionIdToPage(newestXact);
#endif

    LWLockAcquire(SubtransControlLock, LW_EXCLUSIVE);

#ifdef PGXC
    /*
     * We repeat the check.  Another process may have written 
     * out the page already and advanced the latest_page_number
     * while we were waiting for the lock.
     */
    latestSubXid = (SubTransCtl->shared->latest_page_number *
            SUBTRANS_XACTS_PER_PAGE) + SUBTRANS_XACTS_PER_PAGE - 1;
    if (TransactionIdPrecedesOrEquals(newestXact, latestSubXid))
    {
        LWLockRelease(SubtransControlLock);
        return;
    }

    /*
     * We must initialise all pages between latest_page_number and pageno,
     * taking into consideration XID wraparound
     */
    for (;;)
    {
        /* Zero the page and make an XLOG entry about it */
        int target_pageno = SubTransCtl->shared->latest_page_number + 1;
        if (target_pageno > TransactionIdToPage(MaxTransactionId))
            target_pageno = 0;
        ZeroSUBTRANSPage(target_pageno);
        if (target_pageno == pageno)
            break;
    }
#else
    /* Zero the page */
    ZeroSUBTRANSPage(pageno);
#endif

    LWLockRelease(SubtransControlLock);
}


/*
 * Remove all SUBTRANS segments before the one holding the passed transaction ID
 *
 * This is normally called during checkpoint, with oldestXact being the
 * oldest TransactionXmin of any running transaction.
 */
void
TruncateSUBTRANS(TransactionId oldestXact)
{
    int            cutoffPage;

    /*
     * The cutoff point is the start of the segment containing oldestXact. We
     * pass the *page* containing oldestXact to SimpleLruTruncate.  We step
     * back one transaction to avoid passing a cutoff page that hasn't been
     * created yet in the rare case that oldestXact would be the first item on
     * a page and oldestXact == next XID.  In that case, if we didn't subtract
     * one, we'd trigger SimpleLruTruncate's wraparound detection.
     */
    TransactionIdRetreat(oldestXact);
    cutoffPage = TransactionIdToPage(oldestXact);

    SimpleLruTruncate(SubTransCtl, cutoffPage);
}


/*
 * Decide which of two SUBTRANS page numbers is "older" for truncation purposes.
 *
 * We need to use comparison of TransactionIds here in order to do the right
 * thing with wraparound XID arithmetic.  However, if we are asked about
 * page number zero, we don't want to hand InvalidTransactionId to
 * TransactionIdPrecedes: it'll get weird about permanent xact IDs.  So,
 * offset both xids by FirstNormalTransactionId to avoid that.
 */
static bool
SubTransPagePrecedes(int page1, int page2)
{
    TransactionId xid1;
    TransactionId xid2;

    xid1 = ((TransactionId) page1) * SUBTRANS_XACTS_PER_PAGE;
    xid1 += FirstNormalTransactionId;
    xid2 = ((TransactionId) page2) * SUBTRANS_XACTS_PER_PAGE;
    xid2 += FirstNormalTransactionId;

    return TransactionIdPrecedes(xid1, xid2);
}
