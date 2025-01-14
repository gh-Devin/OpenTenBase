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
 * syscache.h
 *      System catalog cache definitions.
 *
 * See also lsyscache.h, which provides convenience routines for
 * common cache-lookup operations.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 *
 * src/include/utils/syscache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SYSCACHE_H
#define SYSCACHE_H

#include "access/attnum.h"
#include "access/htup.h"
/* we intentionally do not include utils/catcache.h here */

/*
 *        SysCache identifiers.
 *
 *        The order of these identifiers must match the order
 *        of the entries in the array cacheinfo[] in syscache.c.
 *        Keep them in alphabetical order (renumbering only costs a
 *        backend rebuild).
 */

enum SysCacheIdentifier
{
    AGGFNOID = 0,
    AMNAME,
    AMOID,
    AMOPOPID,
    AMOPSTRATEGY,
    AMPROCNUM,
    ATTNAME,
    ATTNUM,
#ifdef __AUDIT__
    AUDITSTMTCONF,
    AUDITSTMTCONFOID,
    AUDITUSERCONF,
    AUDITUSERCONFOID,
    AUDITOBJCONF,
    AUDITOBJCONFOID,
    AUDITOBJDEFAULT,
    AUDITOBJDEFAULTOID,
    AUDITFGAPOLICYCONF,
    AUDITFGAOBJPOLICYCONF,
#endif
    AUTHMEMMEMROLE,
    AUTHMEMROLEMEM,
    AUTHNAME,
    AUTHOID,
    CASTSOURCETARGET,
    CLAAMNAMENSP,
    CLAOID,
#ifdef _MLS_    
    CLSCOMOID,
    CLSGRPOID,
    CLSLABELOID,
    CLSLEVELOID,
    CLSPOLOID,
    CLSTBLOID,
    CLSUSEROID,
#endif    
    COLLNAMEENCNSP,
    COLLOID,
    CONDEFAULT,
    CONNAMENSP,
    CONSTROID,
    CONVOID,
    DATABASEOID,
#ifdef _MLS_    
    DATAMASKOID,
    DATAMASKUSEROID,
#endif    
    DEFACLROLENSPOBJ,
    ENUMOID,
    ENUMTYPOIDNAME,
    EVENTTRIGGERNAME,
    EVENTTRIGGEROID,
    FOREIGNDATAWRAPPERNAME,
    FOREIGNDATAWRAPPEROID,
    FOREIGNSERVERNAME,
    FOREIGNSERVEROID,
    FOREIGNTABLEREL,
    INDEXRELID,
    LANGNAME,
    LANGOID,
    NAMESPACENAME,
    NAMESPACEOID,
    OPERNAMENSP,
    OPEROID,
    OPFAMILYAMNAMENSP,
    OPFAMILYOID,
#ifdef PGXC
    PGXCCLASSRELID,
    PGXCGROUPNAME,
    PGXCGROUPOID,
    PGXCNODENAME,
    PGXCNODEOID,
    PGXCNODEIDENTIFIER,
#endif
#ifdef __OPENTENBASE__
    PGPARTITIONINTERVALREL,
#endif
    PARTRELID,
    PROCNAMEARGSNSP,
    PROCOID,
    PUBLICATIONNAME,
    PUBLICATIONOID,
    PUBLICATIONREL,
    PUBLICATIONRELMAP,
#ifdef __STORAGE_SCALABLE__
    PUBLICATIONSHARD,
    PUBLICATIONSHARDMAP,
#endif
    RANGETYPE,
    RELNAMENSP,
    RELOID,
    REPLORIGIDENT,
    REPLORIGNAME,
    RULERELNAME,
    SEQRELID,
#ifdef __COLD_HOT__
    SHARDKEYVALUE,
#endif
    STATEXTNAMENSP,
    STATEXTOID,
    STATRELATTINH,
    SUBSCRIPTIONNAME,
    SUBSCRIPTIONOID,
    SUBSCRIPTIONRELMAP,
#ifdef __STORAGE_SCALABLE__
    SUBSCRIPTIONSHARD,
    SUBSCRIPTIONSHARDMAP,
    SUBSCRIPTIONSHARDPUBNAME,
    SUBSCRIPTIONTABLE,
    SUBSCRIPTIONTABLEMAP,
    SUBSCRIPTIONTABLEOID,
#endif
    TABLESPACEOID,
#ifdef _MLS_    
    TCPALGORITHM,
    TCPOID,
    TCPSOID,
    TCPTOID,
#endif    
    TRFOID,
    TRFTYPELANG,
    TSCONFIGMAP,
    TSCONFIGNAMENSP,
    TSCONFIGOID,
    TSDICTNAMENSP,
    TSDICTOID,
    TSPARSERNAMENSP,
    TSPARSEROID,
    TSTEMPLATENAMENSP,
    TSTEMPLATEOID,
    TYPENAMENSP,
    TYPEOID,
    USERMAPPINGOID,
    USERMAPPINGUSERSERVER

#define SysCacheSize (USERMAPPINGUSERSERVER + 1)
};

extern void InitCatalogCache(void);
extern void InitCatalogCachePhase2(void);

extern HeapTuple SearchSysCache(int cacheId,
               Datum key1, Datum key2, Datum key3, Datum key4);
extern void ReleaseSysCache(HeapTuple tuple);

/* convenience routines */
extern HeapTuple SearchSysCacheCopy(int cacheId,
                   Datum key1, Datum key2, Datum key3, Datum key4);
extern bool SearchSysCacheExists(int cacheId,
                     Datum key1, Datum key2, Datum key3, Datum key4);
extern Oid GetSysCacheOid(int cacheId,
               Datum key1, Datum key2, Datum key3, Datum key4);

extern HeapTuple SearchSysCacheAttName(Oid relid, const char *attname);
extern HeapTuple SearchSysCacheCopyAttName(Oid relid, const char *attname);
extern bool SearchSysCacheExistsAttName(Oid relid, const char *attname);

extern Datum SysCacheGetAttr(int cacheId, HeapTuple tup,
                AttrNumber attributeNumber, bool *isNull);

extern uint32 GetSysCacheHashValue(int cacheId,
                     Datum key1, Datum key2, Datum key3, Datum key4);

/* list-search interface.  Users of this must import catcache.h too */
struct catclist;
extern struct catclist *SearchSysCacheList(int cacheId, int nkeys,
                   Datum key1, Datum key2, Datum key3, Datum key4);

extern void SysCacheInvalidate(int cacheId, uint32 hashValue);

extern bool RelationInvalidatesSnapshotsOnly(Oid relid);
extern bool RelationHasSysCache(Oid relid);
extern bool RelationSupportsSysCache(Oid relid);

#ifdef __AUDIT__
extern void GetSysCacheInfo(int32 cacheid,
                            Oid * reloid,
                            Oid * indoid,
                            int * nkeys);
#endif

/*
 * The use of the macros below rather than direct calls to the corresponding
 * functions is encouraged, as it insulates the caller from changes in the
 * maximum number of keys.
 */
#define SearchSysCache1(cacheId, key1) \
    SearchSysCache(cacheId, key1, 0, 0, 0)
#define SearchSysCache2(cacheId, key1, key2) \
    SearchSysCache(cacheId, key1, key2, 0, 0)
#define SearchSysCache3(cacheId, key1, key2, key3) \
    SearchSysCache(cacheId, key1, key2, key3, 0)
#define SearchSysCache4(cacheId, key1, key2, key3, key4) \
    SearchSysCache(cacheId, key1, key2, key3, key4)

#define SearchSysCacheCopy1(cacheId, key1) \
    SearchSysCacheCopy(cacheId, key1, 0, 0, 0)
#define SearchSysCacheCopy2(cacheId, key1, key2) \
    SearchSysCacheCopy(cacheId, key1, key2, 0, 0)
#define SearchSysCacheCopy3(cacheId, key1, key2, key3) \
    SearchSysCacheCopy(cacheId, key1, key2, key3, 0)
#define SearchSysCacheCopy4(cacheId, key1, key2, key3, key4) \
    SearchSysCacheCopy(cacheId, key1, key2, key3, key4)

#define SearchSysCacheExists1(cacheId, key1) \
    SearchSysCacheExists(cacheId, key1, 0, 0, 0)
#define SearchSysCacheExists2(cacheId, key1, key2) \
    SearchSysCacheExists(cacheId, key1, key2, 0, 0)
#define SearchSysCacheExists3(cacheId, key1, key2, key3) \
    SearchSysCacheExists(cacheId, key1, key2, key3, 0)
#define SearchSysCacheExists4(cacheId, key1, key2, key3, key4) \
    SearchSysCacheExists(cacheId, key1, key2, key3, key4)

#define GetSysCacheOid1(cacheId, key1) \
    GetSysCacheOid(cacheId, key1, 0, 0, 0)
#define GetSysCacheOid2(cacheId, key1, key2) \
    GetSysCacheOid(cacheId, key1, key2, 0, 0)
#define GetSysCacheOid3(cacheId, key1, key2, key3) \
    GetSysCacheOid(cacheId, key1, key2, key3, 0)
#define GetSysCacheOid4(cacheId, key1, key2, key3, key4) \
    GetSysCacheOid(cacheId, key1, key2, key3, key4)

#define GetSysCacheHashValue1(cacheId, key1) \
    GetSysCacheHashValue(cacheId, key1, 0, 0, 0)
#define GetSysCacheHashValue2(cacheId, key1, key2) \
    GetSysCacheHashValue(cacheId, key1, key2, 0, 0)
#define GetSysCacheHashValue3(cacheId, key1, key2, key3) \
    GetSysCacheHashValue(cacheId, key1, key2, key3, 0)
#define GetSysCacheHashValue4(cacheId, key1, key2, key3, key4) \
    GetSysCacheHashValue(cacheId, key1, key2, key3, key4)

#define SearchSysCacheList1(cacheId, key1) \
    SearchSysCacheList(cacheId, 1, key1, 0, 0, 0)
#define SearchSysCacheList2(cacheId, key1, key2) \
    SearchSysCacheList(cacheId, 2, key1, key2, 0, 0)
#define SearchSysCacheList3(cacheId, key1, key2, key3) \
    SearchSysCacheList(cacheId, 3, key1, key2, key3, 0)
#define SearchSysCacheList4(cacheId, key1, key2, key3, key4) \
    SearchSysCacheList(cacheId, 4, key1, key2, key3, key4)

#define ReleaseSysCacheList(x)    ReleaseCatCacheList(x)

#endif                            /* SYSCACHE_H */
