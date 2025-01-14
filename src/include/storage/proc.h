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
 * proc.h
 *      per-process shared memory data structures
 *
 *
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * This source code file contains modifications made by THL A29 Limited ("Tencent Modifications").
 * All Tencent Modifications are Copyright (C) 2023 THL A29 Limited.
 *
 * src/include/storage/proc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PROC_H_
#define _PROC_H_

#include "access/xlogdefs.h"
#include "lib/ilist.h"
#include "storage/latch.h"
#include "storage/lock.h"
#include "storage/pg_sema.h"
#include "storage/proclist_types.h"
#include "port/atomics.h"
#include "storage/lwlock.h"


/*
 * Each backend advertises up to PGPROC_MAX_CACHED_SUBXIDS TransactionIds
 * for non-aborted subtransactions of its current top transaction.  These
 * have to be treated as running XIDs by other backends.
 *
 * We also keep track of whether the cache overflowed (ie, the transaction has
 * generated at least one subtransaction that didn't fit in the cache).
 * If none of the caches have overflowed, we can assume that an XID that's not
 * listed anywhere in the PGPROC array is not a running transaction.  Else we
 * have to look at pg_subtrans.
 */
#define PGPROC_MAX_CACHED_SUBXIDS 64    /* XXX guessed-at value */

struct XidCache
{
    TransactionId xids[PGPROC_MAX_CACHED_SUBXIDS];
};

/*
 * Flags for PGXACT->vacuumFlags
 *
 * Note: If you modify these flags, you need to modify PROCARRAY_XXX flags
 * in src/include/storage/procarray.h.
 *
 * PROC_RESERVED may later be assigned for use in vacuumFlags, but its value is
 * used for PROCARRAY_SLOTS_XMIN in procarray.h, so GetOldestXmin won't be able
 * to match and ignore processes with this flag set.
 */
#define        PROC_IS_AUTOVACUUM    0x01    /* is it an autovac worker? */
#define        PROC_IN_VACUUM        0x02    /* currently running lazy vacuum */
#define        PROC_IN_ANALYZE        0x04    /* currently running analyze */
#define        PROC_VACUUM_FOR_WRAPAROUND    0x08    /* set by autovac only */
#define        PROC_IN_LOGICAL_DECODING    0x10    /* currently doing logical
                                                 * decoding outside xact */
#define        PROC_RESERVED                0x20    /* reserved for procarray */

/* flags reset at EOXact */
#define        PROC_VACUUM_STATE_MASK \
    (PROC_IN_VACUUM | PROC_IN_ANALYZE | PROC_VACUUM_FOR_WRAPAROUND)

/*
 * We allow a small number of "weak" relation locks (AccesShareLock,
 * RowShareLock, RowExclusiveLock) to be recorded in the PGPROC structure
 * rather than the main lock table.  This eases contention on the lock
 * manager LWLocks.  See storage/lmgr/README for additional details.
 */
#define        FP_LOCK_SLOTS_PER_BACKEND 16

/*
 * An invalid pgprocno.  Must be larger than the maximum number of PGPROC
 * structures we could possibly have.  See comments for MAX_BACKENDS.
 */
#define INVALID_PGPROCNO        PG_INT32_MAX

/*
 * Each backend has a PGPROC struct in shared memory.  There is also a list of
 * currently-unused PGPROC structs that will be reallocated to new backends.
 *
 * links: list link for any list the PGPROC is in.  When waiting for a lock,
 * the PGPROC is linked into that lock's waitProcs queue.  A recycled PGPROC
 * is linked into ProcGlobal's freeProcs list.
 *
 * Note: twophase.c also sets up a dummy PGPROC struct for each currently
 * prepared transaction.  These PGPROCs appear in the ProcArray data structure
 * so that the prepared transactions appear to be still running and are
 * correctly shown as holding locks.  A prepared transaction PGPROC can be
 * distinguished from a real one at need by the fact that it has pid == 0.
 * The semaphore and lock-activity fields in a prepared-xact PGPROC are unused,
 * but its myProcLocks[] lists are valid.
 */
struct PGPROC
{
    /* proc->links MUST BE FIRST IN STRUCT (see ProcSleep,ProcWakeup,etc) */
    SHM_QUEUE    links;            /* list link if process is in a list */
    PGPROC      **procgloballist; /* procglobal list that owns this PGPROC */

    PGSemaphore sem;            /* ONE semaphore to sleep on */
    int            waitStatus;        /* STATUS_WAITING, STATUS_OK or STATUS_ERROR */

    Latch        procLatch;        /* generic latch for process */

    LocalTransactionId lxid;    /* local id of top-level transaction currently
                                 * being executed by this proc, if running;
                                 * else InvalidLocalTransactionId */
    int            pid;            /* Backend's process ID; 0 if prepared xact */


#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
    GlobalTimestamp      commitTs; /* Recently received global timestamp */
    bool        committed;

    LWLock         globalxidLock;                /* Protect the following globalXid field */
    char        globalXid[NAMEDATALEN];  /* Global Xid passed from Coordinator */
    bool        hasGlobalXid;      /* Indicate whether it has global xid passed from Coordinator */
#endif

    int            pgprocno;

    /* These fields are zero while a backend is still starting up: */
    BackendId    backendId;        /* This backend's backend ID (if assigned) */
    Oid            databaseId;        /* OID of database this backend is using */
    Oid            roleId;            /* OID of role using this backend */
#ifdef XCP
    Oid            coordId;          /* Oid of originating coordinator */
    int            coordPid;        /* Pid of the originating session */
    BackendId    firstBackendId;    /* Backend ID of the first backend of
                                 * the distributed session */
#endif

    bool        isBackgroundWorker; /* true if background worker. */

    /*
     * While in hot standby mode, shows that a conflict signal has been sent
     * for the current transaction. Set/cleared while holding ProcArrayLock,
     * though not required. Accessed without lock, if needed.
     */
    bool        recoveryConflictPending;

#ifdef PGXC
    /* Postgres-XC flags */
    bool        isPooler;        /* true if process is Postgres-XC pooler */
#endif

    /* Info about LWLock the process is currently waiting for, if any. */
    bool        lwWaiting;        /* true if waiting for an LW lock */
    uint8        lwWaitMode;        /* lwlock mode being waited for */
    proclist_node lwWaitLink;    /* position in LW lock wait list */

    /* Support for condition variables. */
    proclist_node cvWaitLink;    /* position in CV wait list */

    /* Info about lock the process is currently waiting for, if any. */
    /* waitLock and waitProcLock are NULL if not currently waiting. */
    LOCK       *waitLock;        /* Lock object we're sleeping on ... */
    PROCLOCK   *waitProcLock;    /* Per-holder info for awaited lock */
    LOCKMODE    waitLockMode;    /* type of lock we're waiting for */
    LOCKMASK    heldLocks;        /* bitmask for lock types already held on this
                                 * lock object by this backend */

    /*
     * Info to allow us to wait for synchronous replication, if needed.
     * waitLSN is InvalidXLogRecPtr if not waiting; set only by user backend.
     * syncRepState must not be touched except by owning process or WALSender.
     * syncRepLinks used only while holding SyncRepLock.
     */
    XLogRecPtr    waitLSN;        /* waiting for this LSN or higher */
    int            syncRepState;    /* wait state for sync rep */
    SHM_QUEUE    syncRepLinks;    /* list link if process is in syncrep queue */

    /*
     * All PROCLOCK objects for locks held or awaited by this backend are
     * linked into one of these lists, according to the partition number of
     * their lock.
     */
    SHM_QUEUE    myProcLocks[NUM_LOCK_PARTITIONS];

    struct XidCache subxids;    /* cache for subtransaction XIDs */

    /* Support for group XID clearing. */
    /* true, if member of ProcArray group waiting for XID clear */
    bool        procArrayGroupMember;
    /* next ProcArray group member waiting for XID clear */
    pg_atomic_uint32 procArrayGroupNext;

    /*
     * latest transaction id among the transaction's main XID and
     * subtransactions
     */
    TransactionId procArrayGroupMemberXid;

    uint32        wait_event_info;    /* proc's wait information */

    /* Per-backend LWLock.  Protects fields below (but not group fields). */
    LWLock        backendLock;

    /* Lock manager data, recording fast-path locks taken by this backend. */
    uint64        fpLockBits;        /* lock modes held for each fast-path slot */
    Oid            fpRelId[FP_LOCK_SLOTS_PER_BACKEND]; /* slots for rel oids */
    bool        fpVXIDLock;        /* are we holding a fast-path VXID lock? */
    LocalTransactionId fpLocalTransactionId;    /* lxid for fast-path VXID
                                                 * lock */

    /*
     * Support for lock groups.  Use LockHashPartitionLockByProc on the group
     * leader to get the LWLock protecting these fields.
     */
    PGPROC       *lockGroupLeader;    /* lock group leader, if I'm a member */
    dlist_head    lockGroupMembers;    /* list of members, if I'm a leader */
    dlist_node    lockGroupLink;    /* my member link, if I'm a member */
};

/* NOTE: "typedef struct PGPROC PGPROC" appears in storage/lock.h. */


extern PGDLLIMPORT PGPROC *MyProc;
extern PGDLLIMPORT struct PGXACT *MyPgXact;

/*
 * Prior to PostgreSQL 9.2, the fields below were stored as part of the
 * PGPROC.  However, benchmarking revealed that packing these particular
 * members into a separate array as tightly as possible sped up GetSnapshotData
 * considerably on systems with many CPU cores, by reducing the number of
 * cache lines needing to be fetched.  Thus, think very carefully before adding
 * anything else here.
 */
typedef struct PGXACT
{
    TransactionId xid;            /* id of top-level transaction currently being
                                 * executed by this proc, if running and XID
                                 * is assigned; else InvalidTransactionId */

    TransactionId xmin;            /* minimal running XID as it was when we were
                                 * starting our xact, excluding LAZY VACUUM:
                                 * vacuum must not remove tuples deleted by
                                 * xid >= xmin ! */

    uint8        vacuumFlags;    /* vacuum-related flags, see above */
    bool        overflowed;
    bool        delayChkpt;        /* true if this proc delays checkpoint start;
                                 * previously called InCommit */

    uint8        nxids;
    
#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
    pg_atomic_uint64         prepare_timestamp; /* Global prepare timestmap for two-phase transaction */
    pg_atomic_uint64         tmin;     /* Recently Received Global Timestamp */
#endif
} PGXACT;

/*
 * There is one ProcGlobal struct for the whole database cluster.
 */
typedef struct PROC_HDR
{
    /* Array of PGPROC structures (not including dummies for prepared txns) */
    PGPROC       *allProcs;
    /* Array of PGXACT structures (not including dummies for prepared txns) */
    PGXACT       *allPgXact;
    /* Length of allProcs array */
    uint32        allProcCount;
    /* Head of list of free PGPROC structures */
    PGPROC       *freeProcs;
    /* Head of list of autovacuum's free PGPROC structures */
    PGPROC       *autovacFreeProcs;
    /* Head of list of bgworker free PGPROC structures */
    PGPROC       *bgworkerFreeProcs;
	/* Head of list of clean 2pc process free PGPROC structures */
	PGPROC	   *clean2pcFreeProcs;
    /* First pgproc waiting for group XID clear */
    pg_atomic_uint32 procArrayGroupFirst;
    /* WALWriter process's latch */
    Latch       *walwriterLatch;
    /* Checkpointer process's latch */
    Latch       *checkpointerLatch;
    /* Current shared estimate of appropriate spins_per_delay value */
    int            spins_per_delay;
    /* The proc of the Startup process, since not in ProcArray */
    PGPROC       *startupProc;
    int            startupProcPid;
    /* Buffer id of the buffer that Startup process waits for pin on, or -1 */
    int            startupBufferPinWaitBufId;
} PROC_HDR;

extern PROC_HDR *ProcGlobal;

extern PGPROC *PreparedXactProcs;

/* Accessor for PGPROC given a pgprocno. */
#define GetPGProcByNumber(n) (&ProcGlobal->allProcs[(n)])

/*
 * We set aside some extra PGPROC structures for auxiliary processes,
 * ie things that aren't full-fledged backends but need shmem access.
 *
 * Background writer, checkpointer and WAL writer run during normal operation.
 * Startup process and WAL receiver also consume 2 slots, but WAL writer is
 * launched only after startup has exited, so we only need 4 slots.
 *
 * PGXC needs another slot for the pool manager process
 */
#ifdef PGXC
#define NUM_AUXILIARY_PROCS        5
#else
#define NUM_AUXILIARY_PROCS        4
#endif

/* configurable options */
extern int    DeadlockTimeout;
extern int    StatementTimeout;
extern int    LockTimeout;
extern int    IdleInTransactionSessionTimeout;
extern bool log_lock_waits;


/*
 * Function Prototypes
 */
extern int    ProcGlobalSemas(void);
extern Size ProcGlobalShmemSize(void);
extern void InitProcGlobal(void);
extern void InitProcess(void);
extern void InitProcessPhase2(void);
extern void InitAuxiliaryProcess(void);

extern void PublishStartupProcessInformation(void);
extern void SetStartupBufferPinWaitBufId(int bufid);
extern int    GetStartupBufferPinWaitBufId(void);

extern bool HaveNFreeProcs(int n);
extern void ProcReleaseLocks(bool isCommit);

extern void ProcQueueInit(PROC_QUEUE *queue);
extern int    ProcSleep(LOCALLOCK *locallock, LockMethod lockMethodTable);
extern PGPROC *ProcWakeup(PGPROC *proc, int waitStatus);
extern void ProcLockWakeup(LockMethod lockMethodTable, LOCK *lock);
extern void CheckDeadLockAlert(void);
extern bool IsWaitingForLock(void);
extern void LockErrorCleanup(void);

extern void ProcWaitForSignal(uint32 wait_event_info);
extern void ProcSendSignal(int pid);

extern PGPROC *AuxiliaryPidGetProc(int pid);

extern void BecomeLockGroupLeader(void);
extern bool BecomeLockGroupMember(PGPROC *leader, int pid);

#endif                            /* PROC_H */
