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
 * catalog.c
 *        routines concerned with catalog naming conventions and other
 *        bits of hard-wired knowledge
 *
 *
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * This source code file contains modifications made by THL A29 Limited ("Tencent Modifications").
 * All Tencent Modifications are Copyright (C) 2023 THL A29 Limited.
 *
 * IDENTIFICATION
 *      src/backend/catalog/catalog.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/sysattr.h"
#include "access/transam.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_pltemplate.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_replication_origin.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_shdescription.h"
#include "catalog/pg_shseclabel.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "catalog/toasting.h"
#include "catalog/pgxc_node.h"
#include "catalog/pgxc_group.h"
#ifdef _MIGRATE_
#include "catalog/pgxc_shard_map.h"
#include "catalog/pgxc_key_values.h"
#endif
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/tqual.h"
#ifdef PGXC
#include "pgxc/pgxc.h"
#endif
#ifdef _MLS_
#include "catalog/mls/pg_transparent_crypt_policy_algorithm.h"
#endif

#ifdef __AUDIT__
#include "catalog/pg_audit.h"
#endif

/*
 * IsSystemRelation
 *        True iff the relation is either a system catalog or toast table.
 *        By a system catalog, we mean one that created in the pg_catalog schema
 *        during initdb.  User-created relations in pg_catalog don't count as
 *        system catalogs.
 *
 *        NB: TOAST relations are considered system relations by this test
 *        for compatibility with the old IsSystemRelationName function.
 *        This is appropriate in many places but not all.  Where it's not,
 *        also check IsToastRelation or use IsCatalogRelation().
 */
bool
IsSystemRelation(Relation relation)
{
    return IsSystemClass(RelationGetRelid(relation), relation->rd_rel);
}

bool
IsAuditClass(Oid relid)
{
    switch (relid)
    {
        case PgAuditStmtConfRelationId:
        case PgAuditUserConfRelationId:
        case PgAuditObjConfRelationId:
        case PgAuditObjDefOptsRelationId:
        case PgAuditFgaConfRelationId:
        {
            return true;
            break;
        }
        default:
        {
            return false;
            break;
        }
    }

    return false;
}

/*
 * IsSystemClass
 *        Like the above, but takes a Form_pg_class as argument.
 *        Used when we do not want to open the relation and have to
 *        search pg_class directly.
 */
bool
IsSystemClass(Oid relid, Form_pg_class reltuple)
{
    return IsToastClass(reltuple) || IsCatalogClass(relid, reltuple);
}

/*
 * IsCatalogRelation
 *        True iff the relation is a system catalog, or the toast table for
 *        a system catalog.  By a system catalog, we mean one that created
 *        in the pg_catalog schema during initdb.  As with IsSystemRelation(),
 *        user-created relations in pg_catalog don't count as system catalogs.
 *
 *        Note that IsSystemRelation() returns true for ALL toast relations,
 *        but this function returns true only for toast relations of system
 *        catalogs.
 */
bool
IsCatalogRelation(Relation relation)
{
    return IsCatalogClass(RelationGetRelid(relation), relation->rd_rel);
}

/*
 * IsCatalogClass
 *        True iff the relation is a system catalog relation.
 *
 * Check IsCatalogRelation() for details.
 */
bool
IsCatalogClass(Oid relid, Form_pg_class reltuple)
{
    Oid            relnamespace = reltuple->relnamespace;

    /*
     * Never consider relations outside pg_catalog/pg_toast to be catalog
     * relations.
     */
    if (!IsSystemNamespace(relnamespace) && !IsToastNamespace(relnamespace))
        return false;

    /* ----
     * Check whether the oid was assigned during initdb, when creating the
     * initial template database. Minus the relations in information_schema
     * excluded above, these are integral part of the system.
     * We could instead check whether the relation is pinned in pg_depend, but
     * this is noticeably cheaper and doesn't require catalog access.
     *
     * This test is safe since even an oid wraparound will preserve this
     * property (c.f. GetNewObjectId()) and it has the advantage that it works
     * correctly even if a user decides to create a relation in the pg_catalog
     * namespace.
     * ----
     */
    return relid < FirstNormalObjectId;
}

/*
 * IsToastRelation
 *        True iff relation is a TOAST support relation (or index).
 */
bool
IsToastRelation(Relation relation)
{
    return IsToastNamespace(RelationGetNamespace(relation));
}

/*
 * IsToastClass
 *        Like the above, but takes a Form_pg_class as argument.
 *        Used when we do not want to open the relation and have to
 *        search pg_class directly.
 */
bool
IsToastClass(Form_pg_class reltuple)
{
    Oid            relnamespace = reltuple->relnamespace;

    return IsToastNamespace(relnamespace);
}

/*
 * IsSystemNamespace
 *        True iff namespace is pg_catalog.
 *
 * NOTE: the reason this isn't a macro is to avoid having to include
 * catalog/pg_namespace.h in a lot of places.
 */
bool
IsSystemNamespace(Oid namespaceId)
{
#ifdef _PG_ORCL_
    return (namespaceId == PG_CATALOG_NAMESPACE ||
            namespaceId == PG_ORACLE_NAMESPACE);
#else
    return namespaceId == PG_CATALOG_NAMESPACE;
#endif
}

/*
 * IsToastNamespace
 *        True iff namespace is pg_toast or my temporary-toast-table namespace.
 *
 * Note: this will return false for temporary-toast-table namespaces belonging
 * to other backends.  Those are treated the same as other backends' regular
 * temp table namespaces, and access is prevented where appropriate.
 */
bool
IsToastNamespace(Oid namespaceId)
{
    return (namespaceId == PG_TOAST_NAMESPACE) ||
        isTempToastNamespace(namespaceId);
}


/*
 * IsReservedName
 *        True iff name starts with the pg_ prefix.
 *
 *        For some classes of objects, the prefix pg_ is reserved for
 *        system objects only.  As of 8.0, this was only true for
 *        schema and tablespace names.  With 9.6, this is also true
 *        for roles.
 */
bool
IsReservedName(const char *name)
{
    /* ugly coding for speed */
    return (name[0] == 'p' &&
            name[1] == 'g' &&
            name[2] == '_');
}


/*
 * IsSharedRelation
 *        Given the OID of a relation, determine whether it's supposed to be
 *        shared across an entire database cluster.
 *
 * In older releases, this had to be hard-wired so that we could compute the
 * locktag for a relation and lock it before examining its catalog entry.
 * Since we now have MVCC catalog access, the race conditions that made that
 * a hard requirement are gone, so we could look at relaxing this restriction.
 * However, if we scanned the pg_class entry to find relisshared, and only
 * then locked the relation, pg_class could get updated in the meantime,
 * forcing us to scan the relation again, which would definitely be complex
 * and might have undesirable performance consequences.  Fortunately, the set
 * of shared relations is fairly static, so a hand-maintained list of their
 * OIDs isn't completely impractical.
 */
bool
IsSharedRelation(Oid relationId)
{// #lizard forgives
    /* These are the shared catalogs (look for BKI_SHARED_RELATION) */
    if (relationId == AuthIdRelationId ||
        relationId == AuthMemRelationId ||
        relationId == DatabaseRelationId ||
        relationId == PLTemplateRelationId ||
        relationId == SharedDescriptionRelationId ||
        relationId == SharedDependRelationId ||
        relationId == SharedSecLabelRelationId ||
        relationId == TableSpaceRelationId ||
#ifdef PGXC
        relationId == PgxcGroupRelationId ||
        relationId == PgxcNodeRelationId ||
#endif
        relationId == DbRoleSettingRelationId ||
        relationId == ReplicationOriginRelationId ||
#ifdef _MLS_
        relationId == TransparentCryptPolicyAlgorithmId ||
#endif
        relationId == SubscriptionRelationId)
        return true;
    /* These are their indexes (see indexing.h) */
    if (relationId == AuthIdRolnameIndexId ||
        relationId == AuthIdOidIndexId ||
        relationId == AuthMemRoleMemIndexId ||
        relationId == AuthMemMemRoleIndexId ||
        relationId == DatabaseNameIndexId ||
        relationId == DatabaseOidIndexId ||
        relationId == PLTemplateNameIndexId ||
        relationId == SharedDescriptionObjIndexId ||
        relationId == SharedDependDependerIndexId ||
        relationId == SharedDependReferenceIndexId ||
        relationId == SharedSecLabelObjectIndexId ||
        relationId == TablespaceOidIndexId ||
        relationId == TablespaceNameIndexId ||
#ifdef PGXC
        relationId == PgxcNodeNodeNameIndexId ||
        relationId == PgxcNodeNodeIdIndexId ||
        relationId == PgxcNodeOidIndexId ||
        relationId == PgxcGroupGroupNameIndexId ||
        relationId == PgxcGroupOidIndexId ||
#endif
#ifdef _MLS_
        relationId == PgTransparentCryptPolicyAlgorithmIndexId ||
#endif

#ifdef _MIGRATE_
        relationId == PgxcShardMapRelationId ||
        relationId == PgxcShardMapNodeIndexId ||
        relationId == PgxcShardMapShardIndexId ||
        relationId == PgxcShardMapGroupIndexId ||
        relationId == PgxcKeyValueRelationId ||
        relationId == PgxcShardKeyValuesIndexID ||
        relationId == PgxcShardKeyGroupIndexID ||
#endif
        relationId == DbRoleSettingDatidRolidIndexId ||
        relationId == ReplicationOriginIdentIndex ||
        relationId == ReplicationOriginNameIndex ||
        relationId == SubscriptionObjectIndexId ||
        relationId == SubscriptionNameIndexId)
        return true;
    /* These are their toast tables and toast indexes (see toasting.h) */
    if (relationId == PgShdescriptionToastTable ||
        relationId == PgShdescriptionToastIndex ||
        relationId == PgDbRoleSettingToastTable ||
        relationId == PgDbRoleSettingToastIndex ||
        relationId == PgShseclabelToastTable ||
        relationId == PgShseclabelToastIndex)
        return true;
    return false;
}


/*
 * GetNewOid
 *        Generate a new OID that is unique within the given relation.
 *
 * Caller must have a suitable lock on the relation.
 *
 * Uniqueness is promised only if the relation has a unique index on OID.
 * This is true for all system catalogs that have OIDs, but might not be
 * true for user tables.  Note that we are effectively assuming that the
 * table has a relatively small number of entries (much less than 2^32)
 * and there aren't very long runs of consecutive existing OIDs.  Again,
 * this is reasonable for system catalogs but less so for user tables.
 *
 * Since the OID is not immediately inserted into the table, there is a
 * race condition here; but a problem could occur only if someone else
 * managed to cycle through 2^32 OIDs and generate the same OID before we
 * finish inserting our row.  This seems unlikely to be a problem.  Note
 * that if we had to *commit* the row to end the race condition, the risk
 * would be rather higher; therefore we use SnapshotAny in the test, so that
 * we will see uncommitted rows.	(We used to use SnapshotDirty, but that has
 * the disadvantage that it ignores recently-deleted rows, creating a risk
 * of transient conflicts for as long as our own MVCC snapshots think a
 * recently-deleted row is live.	The risk is far higher when selecting TOAST
 * OIDs, because SnapshotToast considers dead rows as active indefinitely.)

 */
Oid
GetNewOid(Relation relation)
{
    Oid            oidIndex;

    /* If relation doesn't have OIDs at all, caller is confused */
    Assert(relation->rd_rel->relhasoids);

    /* In bootstrap mode, we don't have any indexes to use */
    if (IsBootstrapProcessingMode())
        return GetNewObjectId();

    /* The relcache will cache the identity of the OID index for us */
    oidIndex = RelationGetOidIndex(relation);

    /* If no OID index, just hand back the next OID counter value */
    if (!OidIsValid(oidIndex))
    {
        /*
         * System catalogs that have OIDs should *always* have a unique OID
         * index; we should only take this path for user tables. Give a
         * warning if it looks like somebody forgot an index.
         */
        if (IsSystemRelation(relation))
            elog(WARNING, "generating possibly-non-unique OID for \"%s\"",
                 RelationGetRelationName(relation));

        return GetNewObjectId();
    }

    /* Otherwise, use the index to find a nonconflicting OID */
    return GetNewOidWithIndex(relation, oidIndex, ObjectIdAttributeNumber);
}

/*
 * GetNewOidWithIndex
 *        Guts of GetNewOid: use the supplied index
 *
 * This is exported separately because there are cases where we want to use
 * an index that will not be recognized by RelationGetOidIndex: TOAST tables
 * have indexes that are usable, but have multiple columns and are on
 * ordinary columns rather than a true OID column.  This code will work
 * anyway, so long as the OID is the index's first column.  The caller must
 * pass in the actual heap attnum of the OID column, however.
 *
 * Caller must have a suitable lock on the relation.
 */
Oid
GetNewOidWithIndex(Relation relation, Oid indexId, AttrNumber oidcolumn)
{
    Oid            newOid;
    SysScanDesc scan;
    ScanKeyData key;
    bool        collides;

    /*
     * We should never be asked to generate a new pg_type OID during
     * pg_upgrade; doing so would risk collisions with the OIDs it wants to
     * assign.  Hitting this assert means there's some path where we failed to
     * ensure that a type OID is determined by commands in the dump script.
     */
    Assert(!IsBinaryUpgrade || RelationGetRelid(relation) != TypeRelationId);


    /* Generate new OIDs until we find one not in the table */
    do
    {
        CHECK_FOR_INTERRUPTS();

        newOid = GetNewObjectId();

        ScanKeyInit(&key,
                    oidcolumn,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(newOid));

		/* see notes above about using SnapshotAny */
        scan = systable_beginscan(relation, indexId, true,
								  SnapshotAny, 1, &key);

        collides = HeapTupleIsValid(systable_getnext(scan));

        systable_endscan(scan);
    } while (collides);

    return newOid;
}

/*
 * GetNewRelFileNode
 *        Generate a new relfilenode number that is unique within the
 *        database of the given tablespace.
 *
 * If the relfilenode will also be used as the relation's OID, pass the
 * opened pg_class catalog, and this routine will guarantee that the result
 * is also an unused OID within pg_class.  If the result is to be used only
 * as a relfilenode for an existing relation, pass NULL for pg_class.
 *
 * As with GetNewOid, there is some theoretical risk of a race condition,
 * but it doesn't seem worth worrying about.
 *
 * Note: we don't support using this in bootstrap mode.  All relations
 * created by bootstrap have preassigned OIDs, so there's no need.
 */
Oid
GetNewRelFileNode(Oid reltablespace, Relation pg_class, char relpersistence)
{// #lizard forgives
    RelFileNodeBackend rnode;
    char       *rpath;
    int            fd;
    bool        collides;
    BackendId    backend;

    /*
     * If we ever get here during pg_upgrade, there's something wrong; all
     * relfilenode assignments during a binary-upgrade run should be
     * determined by commands in the dump script.
     */
    Assert(!IsBinaryUpgrade);

    switch (relpersistence)
    {
        case RELPERSISTENCE_TEMP:
#ifdef XCP
            if (OidIsValid(MyCoordId))
                backend = MyFirstBackendId;
            else
#endif
            backend = BackendIdForTempRelations();
            break;
        case RELPERSISTENCE_UNLOGGED:
        case RELPERSISTENCE_PERMANENT:
            backend = InvalidBackendId;
            break;
        default:
            elog(ERROR, "invalid relpersistence: %c", relpersistence);
            return InvalidOid;    /* placate compiler */
    }

    /* This logic should match RelationInitPhysicalAddr */
    rnode.node.spcNode = reltablespace ? reltablespace : MyDatabaseTableSpace;
    rnode.node.dbNode = (rnode.node.spcNode == GLOBALTABLESPACE_OID) ? InvalidOid : MyDatabaseId;

    /*
     * The relpath will vary based on the backend ID, so we must initialize
     * that properly here to make sure that any collisions based on filename
     * are properly detected.
     */
    rnode.backend = backend;

    do
    {
        CHECK_FOR_INTERRUPTS();

        /* Generate the OID */
        if (pg_class)
            rnode.node.relNode = GetNewOid(pg_class);
        else
            rnode.node.relNode = GetNewObjectId();

        /* Check for existing file of same name */
        rpath = relpath(rnode, MAIN_FORKNUM);
        fd = BasicOpenFile(rpath, O_RDONLY | PG_BINARY, 0);

        if (fd >= 0)
        {
            /* definite collision */
            close(fd);
            collides = true;
        }
        else
        {
            /*
             * Here we have a little bit of a dilemma: if errno is something
             * other than ENOENT, should we declare a collision and loop? In
             * particular one might think this advisable for, say, EPERM.
             * However there really shouldn't be any unreadable files in a
             * tablespace directory, and if the EPERM is actually complaining
             * that we can't read the directory itself, we'd be in an infinite
             * loop.  In practice it seems best to go ahead regardless of the
             * errno.  If there is a colliding file we will get an smgr
             * failure when we attempt to create the new relation file.
             */
            collides = false;
        }

        pfree(rpath);
    } while (collides);

    return rnode.node.relNode;
}
