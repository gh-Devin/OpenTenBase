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
 * tlist.c
 *      Target list manipulation routines
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * This source code file contains modifications made by THL A29 Limited ("Tencent Modifications").
 * All Tencent Modifications are Copyright (C) 2023 THL A29 Limited.
 *
 * IDENTIFICATION
 *      src/backend/optimizer/util/tlist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/tlist.h"


/* Test if an expression node represents a SRF call.  Beware multiple eval! */
#define IS_SRF_CALL(node) \
    ((IsA(node, FuncExpr) && ((FuncExpr *) (node))->funcretset) || \
     (IsA(node, OpExpr) && ((OpExpr *) (node))->opretset))

/* Workspace for split_pathtarget_walker */
typedef struct
{
    List       *input_target_exprs; /* exprs available from input */
    List       *level_srfs;        /* list of lists of SRF exprs */
    List       *level_input_vars;    /* vars needed by SRFs of each level */
    List       *level_input_srfs;    /* SRFs needed by SRFs of each level */
    List       *current_input_vars; /* vars needed in current subexpr */
    List       *current_input_srfs; /* SRFs needed in current subexpr */
    int            current_depth;    /* max SRF depth in current subexpr */
} split_pathtarget_context;

static bool split_pathtarget_walker(Node *node,
                        split_pathtarget_context *context);


/*****************************************************************************
 *        Target list creation and searching utilities
 *****************************************************************************/

/*
 * tlist_member
 *      Finds the (first) member of the given tlist whose expression is
 *      equal() to the given expression.  Result is NULL if no such member.
 */
TargetEntry *
tlist_member(Expr *node, List *targetlist)
{
    ListCell   *temp;

    foreach(temp, targetlist)
    {
        TargetEntry *tlentry = (TargetEntry *) lfirst(temp);

        if (equal(node, tlentry->expr))
            return tlentry;
    }
    return NULL;
}

/*
 * tlist_member_ignore_relabel
 *      Same as above, except that we ignore top-level RelabelType nodes
 *      while checking for a match.  This is needed for some scenarios
 *      involving binary-compatible sort operations.
 */
TargetEntry *
tlist_member_ignore_relabel(Expr *node, List *targetlist)
{
    ListCell   *temp;

    while (node && IsA(node, RelabelType))
        node = ((RelabelType *) node)->arg;

    foreach(temp, targetlist)
    {
        TargetEntry *tlentry = (TargetEntry *) lfirst(temp);
        Expr       *tlexpr = tlentry->expr;

        while (tlexpr && IsA(tlexpr, RelabelType))
            tlexpr = ((RelabelType *) tlexpr)->arg;

        if (equal(node, tlexpr))
            return tlentry;
    }
    return NULL;
}

/*
 * tlist_member_match_var
 *      Same as above, except that we match the provided Var on the basis
 *      of varno/varattno/varlevelsup/vartype only, rather than full equal().
 *
 * This is needed in some cases where we can't be sure of an exact typmod
 * match.  For safety, though, we insist on vartype match.
 */
static TargetEntry *
tlist_member_match_var(Var *var, List *targetlist)
{
    ListCell   *temp;

    foreach(temp, targetlist)
    {
        TargetEntry *tlentry = (TargetEntry *) lfirst(temp);
        Var           *tlvar = (Var *) tlentry->expr;

        if (!tlvar || !IsA(tlvar, Var))
            continue;
        if (var->varno == tlvar->varno &&
            var->varattno == tlvar->varattno &&
            var->varlevelsup == tlvar->varlevelsup &&
            var->vartype == tlvar->vartype)
            return tlentry;
    }
    return NULL;
}

/*
 * add_to_flat_tlist
 *        Add more items to a flattened tlist (if they're not already in it)
 *
 * 'tlist' is the flattened tlist
 * 'exprs' is a list of expressions (usually, but not necessarily, Vars)
 *
 * Returns the extended tlist.
 */
List *
add_to_flat_tlist(List *tlist, List *exprs)
{
    int            next_resno = list_length(tlist) + 1;
    ListCell   *lc;

    foreach(lc, exprs)
    {
        Expr       *expr = (Expr *) lfirst(lc);

        if (!tlist_member(expr, tlist))
        {
            TargetEntry *tle;

            tle = makeTargetEntry(copyObject(expr), /* copy needed?? */
                                  next_resno++,
                                  NULL,
                                  false);
            tlist = lappend(tlist, tle);
        }
    }
    return tlist;
}


/*
 * get_tlist_exprs
 *        Get just the expression subtrees of a tlist
 *
 * Resjunk columns are ignored unless includeJunk is true
 */
List *
get_tlist_exprs(List *tlist, bool includeJunk)
{
    List       *result = NIL;
    ListCell   *l;

    foreach(l, tlist)
    {
        TargetEntry *tle = (TargetEntry *) lfirst(l);

        if (tle->resjunk && !includeJunk)
            continue;

        result = lappend(result, tle->expr);
    }
    return result;
}


/*
 * count_nonjunk_tlist_entries
 *        What it says ...
 */
int
count_nonjunk_tlist_entries(List *tlist)
{
    int            len = 0;
    ListCell   *l;

    foreach(l, tlist)
    {
        TargetEntry *tle = (TargetEntry *) lfirst(l);

        if (!tle->resjunk)
            len++;
    }
    return len;
}


/*
 * tlist_same_exprs
 *        Check whether two target lists contain the same expressions
 *
 * Note: this function is used to decide whether it's safe to jam a new tlist
 * into a non-projection-capable plan node.  Obviously we can't do that unless
 * the node's tlist shows it already returns the column values we want.
 * However, we can ignore the TargetEntry attributes resname, ressortgroupref,
 * resorigtbl, resorigcol, and resjunk, because those are only labelings that
 * don't affect the row values computed by the node.  (Moreover, if we didn't
 * ignore them, we'd frequently fail to make the desired optimization, since
 * the planner tends to not bother to make resname etc. valid in intermediate
 * plan nodes.)  Note that on success, the caller must still jam the desired
 * tlist into the plan node, else it won't have the desired labeling fields.
 */
bool
tlist_same_exprs(List *tlist1, List *tlist2)
{
    ListCell   *lc1,
               *lc2;

    if (list_length(tlist1) != list_length(tlist2))
        return false;            /* not same length, so can't match */

    forboth(lc1, tlist1, lc2, tlist2)
    {
        TargetEntry *tle1 = (TargetEntry *) lfirst(lc1);
        TargetEntry *tle2 = (TargetEntry *) lfirst(lc2);

        if (!equal(tle1->expr, tle2->expr))
            return false;
    }

    return true;
}


/*
 * Does tlist have same output datatypes as listed in colTypes?
 *
 * Resjunk columns are ignored if junkOK is true; otherwise presence of
 * a resjunk column will always cause a 'false' result.
 *
 * Note: currently no callers care about comparing typmods.
 */
bool
tlist_same_datatypes(List *tlist, List *colTypes, bool junkOK)
{
    ListCell   *l;
    ListCell   *curColType = list_head(colTypes);

    foreach(l, tlist)
    {
        TargetEntry *tle = (TargetEntry *) lfirst(l);

        if (tle->resjunk)
        {
            if (!junkOK)
                return false;
        }
        else
        {
            if (curColType == NULL)
                return false;    /* tlist longer than colTypes */
            if (exprType((Node *) tle->expr) != lfirst_oid(curColType))
                return false;
            curColType = lnext(curColType);
        }
    }
    if (curColType != NULL)
        return false;            /* tlist shorter than colTypes */
    return true;
}

/*
 * Does tlist have same exposed collations as listed in colCollations?
 *
 * Identical logic to the above, but for collations.
 */
bool
tlist_same_collations(List *tlist, List *colCollations, bool junkOK)
{
    ListCell   *l;
    ListCell   *curColColl = list_head(colCollations);

    foreach(l, tlist)
    {
        TargetEntry *tle = (TargetEntry *) lfirst(l);

        if (tle->resjunk)
        {
            if (!junkOK)
                return false;
        }
        else
        {
            if (curColColl == NULL)
                return false;    /* tlist longer than colCollations */
            if (exprCollation((Node *) tle->expr) != lfirst_oid(curColColl))
                return false;
            curColColl = lnext(curColColl);
        }
    }
    if (curColColl != NULL)
        return false;            /* tlist shorter than colCollations */
    return true;
}

/*
 * apply_tlist_labeling
 *        Apply the TargetEntry labeling attributes of src_tlist to dest_tlist
 *
 * This is useful for reattaching column names etc to a plan's final output
 * targetlist.
 */
void
apply_tlist_labeling(List *dest_tlist, List *src_tlist)
{
    ListCell   *ld,
               *ls;

    Assert(list_length(dest_tlist) == list_length(src_tlist));
    forboth(ld, dest_tlist, ls, src_tlist)
    {
        TargetEntry *dest_tle = (TargetEntry *) lfirst(ld);
        TargetEntry *src_tle = (TargetEntry *) lfirst(ls);

        Assert(dest_tle->resno == src_tle->resno);
        dest_tle->resname = src_tle->resname;
        dest_tle->ressortgroupref = src_tle->ressortgroupref;
        dest_tle->resorigtbl = src_tle->resorigtbl;
        dest_tle->resorigcol = src_tle->resorigcol;
        dest_tle->resjunk = src_tle->resjunk;
    }
}


/*
 * get_sortgroupref_tle
 *        Find the targetlist entry matching the given SortGroupRef index,
 *        and return it.
 */
TargetEntry *
get_sortgroupref_tle(Index sortref, List *targetList)
{
    ListCell   *l;

    foreach(l, targetList)
    {
        TargetEntry *tle = (TargetEntry *) lfirst(l);

        if (tle->ressortgroupref == sortref)
            return tle;
    }

    elog(ERROR, "ORDER/GROUP BY expression not found in targetlist");
    return NULL;                /* keep compiler quiet */
}

/*
 * get_sortgroupclause_tle
 *        Find the targetlist entry matching the given SortGroupClause
 *        by ressortgroupref, and return it.
 */
TargetEntry *
get_sortgroupclause_tle(SortGroupClause *sgClause,
                        List *targetList)
{
    return get_sortgroupref_tle(sgClause->tleSortGroupRef, targetList);
}

/*
 * get_sortgroupclause_expr
 *        Find the targetlist entry matching the given SortGroupClause
 *        by ressortgroupref, and return its expression.
 */
Node *
get_sortgroupclause_expr(SortGroupClause *sgClause, List *targetList)
{
    TargetEntry *tle = get_sortgroupclause_tle(sgClause, targetList);

    return (Node *) tle->expr;
}

/*
 * get_sortgrouplist_exprs
 *        Given a list of SortGroupClauses, build a list
 *        of the referenced targetlist expressions.
 */
List *
get_sortgrouplist_exprs(List *sgClauses, List *targetList)
{
    List       *result = NIL;
    ListCell   *l;

    foreach(l, sgClauses)
    {
        SortGroupClause *sortcl = (SortGroupClause *) lfirst(l);
        Node       *sortexpr;

        sortexpr = get_sortgroupclause_expr(sortcl, targetList);
        result = lappend(result, sortexpr);
    }
    return result;
}

/*
 * get_distinct_agg_sortgroupclause
 *		Given a pathtarget , acquire distinct clause
 *		for aggref with distinct.
 * Notice: only one distinct agg clause with one col
 * 		is allowed.
 */
Aggref *
get_optimize_distinct_agg(PathTarget *pathtarget)
{
	ListCell *lc = NULL;

	foreach (lc, pathtarget->exprs)
	{
		Aggref *aggref = (Aggref *)lfirst(lc);

		if (IsA(aggref, Aggref) && aggref->aggdistinct != NIL)
		{
			Assert(list_length(aggref->aggdistinct) == 1);
			return aggref;
		}
	}

	return NULL;
}

/*****************************************************************************
 *        Functions to extract data from a list of SortGroupClauses
 *
 * These don't really belong in tlist.c, but they are sort of related to the
 * functions just above, and they don't seem to deserve their own file.
 *****************************************************************************/

/*
 * get_sortgroupref_clause
 *        Find the SortGroupClause matching the given SortGroupRef index,
 *        and return it.
 */
SortGroupClause *
get_sortgroupref_clause(Index sortref, List *clauses)
{
    ListCell   *l;

    foreach(l, clauses)
    {
        SortGroupClause *cl = (SortGroupClause *) lfirst(l);

        if (cl->tleSortGroupRef == sortref)
            return cl;
    }

    elog(ERROR, "ORDER/GROUP BY expression not found in list");
    return NULL;                /* keep compiler quiet */
}

/*
 * get_sortgroupref_clause_noerr
 *        As above, but return NULL rather than throwing an error if not found.
 */
SortGroupClause *
get_sortgroupref_clause_noerr(Index sortref, List *clauses)
{
    ListCell   *l;

    foreach(l, clauses)
    {
        SortGroupClause *cl = (SortGroupClause *) lfirst(l);

        if (cl->tleSortGroupRef == sortref)
            return cl;
    }

    return NULL;
}

/*
 * extract_grouping_ops - make an array of the equality operator OIDs
 *        for a SortGroupClause list
 */
Oid *
extract_grouping_ops(List *groupClause)
{
    int            numCols = list_length(groupClause);
    int            colno = 0;
    Oid           *groupOperators;
    ListCell   *glitem;

    groupOperators = (Oid *) palloc(sizeof(Oid) * numCols);

    foreach(glitem, groupClause)
    {
        SortGroupClause *groupcl = (SortGroupClause *) lfirst(glitem);

        groupOperators[colno] = groupcl->eqop;
        Assert(OidIsValid(groupOperators[colno]));
        colno++;
    }

    return groupOperators;
}

/*
 * extract_grouping_cols - make an array of the grouping column resnos
 *        for a SortGroupClause list
 */
AttrNumber *
extract_grouping_cols(List *groupClause, List *tlist)
{
    AttrNumber *grpColIdx;
    int            numCols = list_length(groupClause);
    int            colno = 0;
    ListCell   *glitem;

    grpColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);

    foreach(glitem, groupClause)
    {
        SortGroupClause *groupcl = (SortGroupClause *) lfirst(glitem);
        TargetEntry *tle = get_sortgroupclause_tle(groupcl, tlist);

        grpColIdx[colno++] = tle->resno;
    }

    return grpColIdx;
}

/*
 * grouping_is_sortable - is it possible to implement grouping list by sorting?
 *
 * This is easy since the parser will have included a sortop if one exists.
 */
bool
grouping_is_sortable(List *groupClause)
{
    ListCell   *glitem;

    foreach(glitem, groupClause)
    {
        SortGroupClause *groupcl = (SortGroupClause *) lfirst(glitem);

        if (!OidIsValid(groupcl->sortop))
            return false;
    }
    return true;
}

/*
 * grouping_is_hashable - is it possible to implement grouping list by hashing?
 *
 * We rely on the parser to have set the hashable flag correctly.
 */
bool
grouping_is_hashable(List *groupClause)
{
    ListCell   *glitem;

    foreach(glitem, groupClause)
    {
        SortGroupClause *groupcl = (SortGroupClause *) lfirst(glitem);

        if (!groupcl->hashable)
            return false;
    }
    return true;
}


/*****************************************************************************
 *        PathTarget manipulation functions
 *
 * PathTarget is a somewhat stripped-down version of a full targetlist; it
 * omits all the TargetEntry decoration except (optionally) sortgroupref data,
 * and it adds evaluation cost and output data width info.
 *****************************************************************************/

/*
 * make_pathtarget_from_tlist
 *      Construct a PathTarget equivalent to the given targetlist.
 *
 * This leaves the cost and width fields as zeroes.  Most callers will want
 * to use create_pathtarget(), so as to get those set.
 */
PathTarget *
make_pathtarget_from_tlist(List *tlist)
{
    PathTarget *target = makeNode(PathTarget);
    int            i;
    ListCell   *lc;

    target->sortgrouprefs = (Index *) palloc(list_length(tlist) * sizeof(Index));

    i = 0;
    foreach(lc, tlist)
    {
        TargetEntry *tle = (TargetEntry *) lfirst(lc);

        target->exprs = lappend(target->exprs, tle->expr);
        target->sortgrouprefs[i] = tle->ressortgroupref;
        i++;
    }

    return target;
}

/*
 * make_tlist_from_pathtarget
 *      Construct a targetlist from a PathTarget.
 */
List *
make_tlist_from_pathtarget(PathTarget *target)
{
    List       *tlist = NIL;
    int            i;
    ListCell   *lc;

    i = 0;
    foreach(lc, target->exprs)
    {
        Expr       *expr = (Expr *) lfirst(lc);
        TargetEntry *tle;

        tle = makeTargetEntry(expr,
                              i + 1,
                              NULL,
                              false);
        if (target->sortgrouprefs)
            tle->ressortgroupref = target->sortgrouprefs[i];
        tlist = lappend(tlist, tle);
        i++;
    }

    return tlist;
}

/*
 * copy_pathtarget
 *      Copy a PathTarget.
 *
 * The new PathTarget has its own List cells, but shares the underlying
 * target expression trees with the old one.  We duplicate the List cells
 * so that items can be added to one target without damaging the other.
 */
PathTarget *
copy_pathtarget(PathTarget *src)
{
    PathTarget *dst = makeNode(PathTarget);

    /* Copy scalar fields */
    memcpy(dst, src, sizeof(PathTarget));
    /* Shallow-copy the expression list */
    dst->exprs = list_copy(src->exprs);
    /* Duplicate sortgrouprefs if any (if not, the memcpy handled this) */
    if (src->sortgrouprefs)
    {
        Size        nbytes = list_length(src->exprs) * sizeof(Index);

        dst->sortgrouprefs = (Index *) palloc(nbytes);
        memcpy(dst->sortgrouprefs, src->sortgrouprefs, nbytes);
    }
    return dst;
}

/*
 * create_empty_pathtarget
 *      Create an empty (zero columns, zero cost) PathTarget.
 */
PathTarget *
create_empty_pathtarget(void)
{
    /* This is easy, but we don't want callers to hard-wire this ... */
    return makeNode(PathTarget);
}

/*
 * add_column_to_pathtarget
 *        Append a target column to the PathTarget.
 *
 * As with make_pathtarget_from_tlist, we leave it to the caller to update
 * the cost and width fields.
 */
void
add_column_to_pathtarget(PathTarget *target, Expr *expr, Index sortgroupref)
{
    /* Updating the exprs list is easy ... */
    target->exprs = lappend(target->exprs, expr);
    /* ... the sortgroupref data, a bit less so */
    if (target->sortgrouprefs)
    {
        int            nexprs = list_length(target->exprs);

        /* This might look inefficient, but actually it's usually cheap */
        target->sortgrouprefs = (Index *)
            repalloc(target->sortgrouprefs, nexprs * sizeof(Index));
        target->sortgrouprefs[nexprs - 1] = sortgroupref;
    }
    else if (sortgroupref)
    {
        /* Adding sortgroupref labeling to a previously unlabeled target */
        int            nexprs = list_length(target->exprs);

        target->sortgrouprefs = (Index *) palloc0(nexprs * sizeof(Index));
        target->sortgrouprefs[nexprs - 1] = sortgroupref;
    }
}

/*
 * add_new_column_to_pathtarget
 *        Append a target column to the PathTarget, but only if it's not
 *        equal() to any pre-existing target expression.
 *
 * The caller cannot specify a sortgroupref, since it would be unclear how
 * to merge that with a pre-existing column.
 *
 * As with make_pathtarget_from_tlist, we leave it to the caller to update
 * the cost and width fields.
 */
void
add_new_column_to_pathtarget(PathTarget *target, Expr *expr)
{
    if (!list_member(target->exprs, expr))
        add_column_to_pathtarget(target, expr, 0);
}

/*
 * add_new_columns_to_pathtarget
 *        Apply add_new_column_to_pathtarget() for each element of the list.
 */
void
add_new_columns_to_pathtarget(PathTarget *target, List *exprs)
{
    ListCell   *lc;

    foreach(lc, exprs)
    {
        Expr       *expr = (Expr *) lfirst(lc);

        add_new_column_to_pathtarget(target, expr);
    }
}

/*
 * apply_pathtarget_labeling_to_tlist
 *        Apply any sortgrouprefs in the PathTarget to matching tlist entries
 *
 * Here, we do not assume that the tlist entries are one-for-one with the
 * PathTarget.  The intended use of this function is to deal with cases
 * where createplan.c has decided to use some other tlist and we have
 * to identify what matches exist.
 */
void
apply_pathtarget_labeling_to_tlist(List *tlist, PathTarget *target)
{// #lizard forgives
    int            i;
    ListCell   *lc;

    /* Nothing to do if PathTarget has no sortgrouprefs data */
    if (target->sortgrouprefs == NULL)
        return;

    i = 0;
    foreach(lc, target->exprs)
    {
        Expr       *expr = (Expr *) lfirst(lc);
        TargetEntry *tle;

        if (target->sortgrouprefs[i])
        {
            /*
             * For Vars, use tlist_member_match_var's weakened matching rule;
             * this allows us to deal with some cases where a set-returning
             * function has been inlined, so that we now have more knowledge
             * about what it returns than we did when the original Var was
             * created.  Otherwise, use regular equal() to find the matching
             * TLE.  (In current usage, only the Var case is actually needed;
             * but it seems best to have sane behavior here for non-Vars too.)
             */
            if (expr && IsA(expr, Var))
                tle = tlist_member_match_var((Var *) expr, tlist);
            else
                tle = tlist_member(expr, tlist);

            /*
             * Complain if noplace for the sortgrouprefs label, or if we'd
             * have to label a column twice.  (The case where it already has
             * the desired label probably can't happen, but we may as well
             * allow for it.)
             */
            if (!tle)
                elog(ERROR, "ORDER/GROUP BY expression not found in targetlist");
            if (tle->ressortgroupref != 0 &&
                tle->ressortgroupref != target->sortgrouprefs[i])
                elog(ERROR, "targetlist item has multiple sortgroupref labels");

            tle->ressortgroupref = target->sortgrouprefs[i];
        }
        i++;
    }
}

/*
  * set corresponding sort/group refnos to each target acoording to source
  */
static void
set_sortgrouprefs_to_pathtarget(PathTarget *dest, PathTarget *source)
{
    int i = 0;
    int j = 0;
    ListCell *s = NULL;
    ListCell *d = NULL;

    if (dest == NULL || source == NULL)
        return;
    
    if (!source->sortgrouprefs)
        return;

    if(dest == source)
        return;

    if (!dest->sortgrouprefs)
    {
        dest->sortgrouprefs = (Index *)
            palloc0(list_length(dest->exprs) * sizeof(Index));
    }

    /* 
      * check each target to see if it also appears in source
      * if so, set sortgrouprefs according to source
      */
    foreach(d, dest->exprs)
    {
        Expr *d_expr = (Expr *) lfirst(d);
        j = 0;
        
        foreach(s, source->exprs)
        {
            Expr *s_expr = (Expr *) lfirst(s);

            if (equal(s_expr, d_expr))
            {
                dest->sortgrouprefs[i] = source->sortgrouprefs[j];
                break;
            }
            
            j++;
        }
        
        i++;
    }
}

/*
 * split_pathtarget_at_srfs
 *        Split given PathTarget into multiple levels to position SRFs safely
 *
 * The executor can only handle set-returning functions that appear at the
 * top level of the targetlist of a ProjectSet plan node.  If we have any SRFs
 * that are not at top level, we need to split up the evaluation into multiple
 * plan levels in which each level satisfies this constraint.  This function
 * creates appropriate PathTarget(s) for each level.
 *
 * As an example, consider the tlist expression
 *        x + srf1(srf2(y + z))
 * This expression should appear as-is in the top PathTarget, but below that
 * we must have a PathTarget containing
 *        x, srf1(srf2(y + z))
 * and below that, another PathTarget containing
 *        x, srf2(y + z)
 * and below that, another PathTarget containing
 *        x, y, z
 * When these tlists are processed by setrefs.c, subexpressions that match
 * output expressions of the next lower tlist will be replaced by Vars,
 * so that what the executor gets are tlists looking like
 *        Var1 + Var2
 *        Var1, srf1(Var2)
 *        Var1, srf2(Var2 + Var3)
 *        x, y, z
 * which satisfy the desired property.
 *
 * Another example is
 *        srf1(x), srf2(srf3(y))
 * That must appear as-is in the top PathTarget, but below that we need
 *        srf1(x), srf3(y)
 * That is, each SRF must be computed at a level corresponding to the nesting
 * depth of SRFs within its arguments.
 *
 * In some cases, a SRF has already been evaluated in some previous plan level
 * and we shouldn't expand it again (that is, what we see in the target is
 * already meant as a reference to a lower subexpression).  So, don't expand
 * any tlist expressions that appear in input_target, if that's not NULL.
 *
 * The outputs of this function are two parallel lists, one a list of
 * PathTargets and the other an integer list of bool flags indicating
 * whether the corresponding PathTarget contains any evaluatable SRFs.
 * The lists are given in the order they'd need to be evaluated in, with
 * the "lowest" PathTarget first.  So the last list entry is always the
 * originally given PathTarget, and any entries before it indicate evaluation
 * levels that must be inserted below it.  The first list entry must not
 * contain any SRFs (other than ones duplicating input_target entries), since
 * it will typically be attached to a plan node that cannot evaluate SRFs.
 *
 * Note: using a list for the flags may seem like overkill, since there
 * are only a few possible patterns for which levels contain SRFs.
 * But this representation decouples callers from that knowledge.
 */
void
split_pathtarget_at_srfs(PlannerInfo *root,
                         PathTarget *target, PathTarget *input_target,
                         List **targets, List **targets_contain_srfs)
{// #lizard forgives
    split_pathtarget_context context;
    int            max_depth;
    bool        need_extra_projection;
    List       *prev_level_tlist;
    ListCell   *lc,
               *lc1,
               *lc2,
               *lc3;

    /*
     * It's not unusual for planner.c to pass us two physically identical
     * targets, in which case we can conclude without further ado that all
     * expressions are available from the input.  (The logic below would
     * arrive at the same conclusion, but much more tediously.)
     */
    if (target == input_target)
    {
        *targets = list_make1(target);
        *targets_contain_srfs = list_make1_int(false);
        return;
    }

    /* Pass any input_target exprs down to split_pathtarget_walker() */
    context.input_target_exprs = input_target ? input_target->exprs : NIL;

    /*
     * Initialize with empty level-zero lists, and no levels after that.
     * (Note: we could dispense with representing level zero explicitly, since
     * it will never receive any SRFs, but then we'd have to special-case that
     * level when we get to building result PathTargets.  Level zero describes
     * the SRF-free PathTarget that will be given to the input plan node.)
     */
    context.level_srfs = list_make1(NIL);
    context.level_input_vars = list_make1(NIL);
    context.level_input_srfs = list_make1(NIL);

    /* Initialize data we'll accumulate across all the target expressions */
    context.current_input_vars = NIL;
    context.current_input_srfs = NIL;
    max_depth = 0;
    need_extra_projection = false;

    /* Scan each expression in the PathTarget looking for SRFs */
    foreach(lc, target->exprs)
    {
        Node       *node = (Node *) lfirst(lc);

        /*
         * Find all SRFs and Vars (and Var-like nodes) in this expression, and
         * enter them into appropriate lists within the context struct.
         */
        context.current_depth = 0;
        split_pathtarget_walker(node, &context);

        /* An expression containing no SRFs is of no further interest */
        if (context.current_depth == 0)
            continue;

        /*
         * Track max SRF nesting depth over the whole PathTarget.  Also, if
         * this expression establishes a new max depth, we no longer care
         * whether previous expressions contained nested SRFs; we can handle
         * any required projection for them in the final ProjectSet node.
         */
        if (max_depth < context.current_depth)
        {
            max_depth = context.current_depth;
            need_extra_projection = false;
        }

        /*
         * If any maximum-depth SRF is not at the top level of its expression,
         * we'll need an extra Result node to compute the top-level scalar
         * expression.
         */
        if (max_depth == context.current_depth && !IS_SRF_CALL(node))
            need_extra_projection = true;
    }

    /*
     * If we found no SRFs needing evaluation (maybe they were all present in
     * input_target, or maybe they were all removed by const-simplification),
     * then no ProjectSet is needed; fall out.
     */
    if (max_depth == 0)
    {
        *targets = list_make1(target);
        *targets_contain_srfs = list_make1_int(false);
        return;
    }

    /*
     * The Vars and SRF outputs needed at top level can be added to the last
     * level_input lists if we don't need an extra projection step.  If we do
     * need one, add a SRF-free level to the lists.
     */
    if (need_extra_projection)
    {
        context.level_srfs = lappend(context.level_srfs, NIL);
        context.level_input_vars = lappend(context.level_input_vars,
                                           context.current_input_vars);
        context.level_input_srfs = lappend(context.level_input_srfs,
                                           context.current_input_srfs);
    }
    else
    {
        lc = list_nth_cell(context.level_input_vars, max_depth);
        lfirst(lc) = list_concat(lfirst(lc), context.current_input_vars);
        lc = list_nth_cell(context.level_input_srfs, max_depth);
        lfirst(lc) = list_concat(lfirst(lc), context.current_input_srfs);
    }

    /*
     * Now construct the output PathTargets.  The original target can be used
     * as-is for the last one, but we need to construct a new SRF-free target
     * representing what the preceding plan node has to emit, as well as a
     * target for each intermediate ProjectSet node.
     */
    *targets = *targets_contain_srfs = NIL;
    prev_level_tlist = NIL;

    forthree(lc1, context.level_srfs,
             lc2, context.level_input_vars,
             lc3, context.level_input_srfs)
    {
        List       *level_srfs = (List *) lfirst(lc1);
        PathTarget *ntarget;

        if (lnext(lc1) == NULL)
        {
            ntarget = target;
        }
        else
        {
            ntarget = create_empty_pathtarget();

            /*
             * This target should actually evaluate any SRFs of the current
             * level, and it needs to propagate forward any Vars needed by
             * later levels, as well as SRFs computed earlier and needed by
             * later levels.  We rely on add_new_columns_to_pathtarget() to
             * remove duplicate items.  Also, for safety, make a separate copy
             * of each item for each PathTarget.
             */
            add_new_columns_to_pathtarget(ntarget, copyObject(level_srfs));
            for_each_cell(lc, lnext(lc2))
            {
                List       *input_vars = (List *) lfirst(lc);

                add_new_columns_to_pathtarget(ntarget, copyObject(input_vars));
            }
            for_each_cell(lc, lnext(lc3))
            {
                List       *input_srfs = (List *) lfirst(lc);
                ListCell   *lcx;

                foreach(lcx, input_srfs)
                {
                    Expr       *srf = (Expr *) lfirst(lcx);

                    if (list_member(prev_level_tlist, srf))
                        add_new_column_to_pathtarget(ntarget, copyObject(srf));
                }
            }
            set_pathtarget_cost_width(root, ntarget);
        }

        /* set sortgrouprefs if needed */
        set_sortgrouprefs_to_pathtarget(ntarget, target);

        /*
         * Add current target and does-it-compute-SRFs flag to output lists.
         */
        *targets = lappend(*targets, ntarget);
        *targets_contain_srfs = lappend_int(*targets_contain_srfs,
                                            (level_srfs != NIL));

        /* Remember this level's output for next pass */
        prev_level_tlist = ntarget->exprs;
    }
}

/*
 * Recursively examine expressions for split_pathtarget_at_srfs.
 *
 * Note we make no effort here to prevent duplicate entries in the output
 * lists.  Duplicates will be gotten rid of later.
 */
static bool
split_pathtarget_walker(Node *node, split_pathtarget_context *context)
{// #lizard forgives
    if (node == NULL)
        return false;

    /*
     * A subexpression that matches an expression already computed in
     * input_target can be treated like a Var (which indeed it will be after
     * setrefs.c gets done with it), even if it's actually a SRF.  Record it
     * as being needed for the current expression, and ignore any
     * substructure.
     */
    if (list_member(context->input_target_exprs, node))
    {
        context->current_input_vars = lappend(context->current_input_vars,
                                              node);
        return false;
    }

    /*
     * Vars and Var-like constructs are expected to be gotten from the input,
     * too.  We assume that these constructs cannot contain any SRFs (if one
     * does, there will be an executor failure from a misplaced SRF).
     */
    if (IsA(node, Var) ||
        IsA(node, PlaceHolderVar) ||
        IsA(node, Aggref) ||
        IsA(node, GroupingFunc) ||
        IsA(node, WindowFunc))
    {
        context->current_input_vars = lappend(context->current_input_vars,
                                              node);
        return false;
    }

    /*
     * If it's a SRF, recursively examine its inputs, determine its level, and
     * make appropriate entries in the output lists.
     */
    if (IS_SRF_CALL(node))
    {
        List       *save_input_vars = context->current_input_vars;
        List       *save_input_srfs = context->current_input_srfs;
        int            save_current_depth = context->current_depth;
        int            srf_depth;
        ListCell   *lc;

        context->current_input_vars = NIL;
        context->current_input_srfs = NIL;
        context->current_depth = 0;

        (void) expression_tree_walker(node, split_pathtarget_walker,
                                      (void *) context);

        /* Depth is one more than any SRF below it */
        srf_depth = context->current_depth + 1;

        /* If new record depth, initialize another level of output lists */
        if (srf_depth >= list_length(context->level_srfs))
        {
            context->level_srfs = lappend(context->level_srfs, NIL);
            context->level_input_vars = lappend(context->level_input_vars, NIL);
            context->level_input_srfs = lappend(context->level_input_srfs, NIL);
        }

        /* Record this SRF as needing to be evaluated at appropriate level */
        lc = list_nth_cell(context->level_srfs, srf_depth);
        lfirst(lc) = lappend(lfirst(lc), node);

        /* Record its inputs as being needed at the same level */
        lc = list_nth_cell(context->level_input_vars, srf_depth);
        lfirst(lc) = list_concat(lfirst(lc), context->current_input_vars);
        lc = list_nth_cell(context->level_input_srfs, srf_depth);
        lfirst(lc) = list_concat(lfirst(lc), context->current_input_srfs);

        /*
         * Restore caller-level state and update it for presence of this SRF.
         * Notice we report the SRF itself as being needed for evaluation of
         * surrounding expression.
         */
        context->current_input_vars = save_input_vars;
        context->current_input_srfs = lappend(save_input_srfs, node);
        context->current_depth = Max(save_current_depth, srf_depth);

        /* We're done here */
        return false;
    }

    /*
     * Otherwise, the node is a scalar (non-set) expression, so recurse to
     * examine its inputs.
     */
    return expression_tree_walker(node, split_pathtarget_walker,
                                  (void *) context);
}
