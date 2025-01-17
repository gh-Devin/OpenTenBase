/*-------------------------------------------------------------------------
 *
 * nodeModifyTable.h
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeModifyTable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMODIFYTABLE_H
#define NODEMODIFYTABLE_H

#include "nodes/execnodes.h"

extern ModifyTableState *ExecInitModifyTable(ModifyTable *node, EState *estate, int eflags);
extern void ExecEndModifyTable(ModifyTableState *node);
extern void ExecReScanModifyTable(ModifyTableState *node);

#ifdef __OPENTENBASE__
extern TupleTableSlot *ExecRemoteUpdate(ModifyTableState *mtstate,
           ItemPointer tupleid,
           HeapTuple oldtuple,
           TupleTableSlot *slot,
           TupleTableSlot *planSlot,
           EPQState *epqstate,
           EState *estate,
           bool canSetTag);
#endif
#endif                            /* NODEMODIFYTABLE_H */
