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
 * pg_cast.h
 *      definition of the system "type casts" relation (pg_cast)
 *      along with the relation's initial contents.
 *
 * As of Postgres 8.0, pg_cast describes not only type coercion functions
 * but also length coercion functions.
 *
 *
 * Copyright (c) 2002-2017, PostgreSQL Global Development Group
 *
 * src/include/catalog/pg_cast.h
 *
 * NOTES
 *      the genbki.pl script reads this file and generates .bki
 *      information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CAST_H
#define PG_CAST_H

#include "catalog/genbki.h"

/* ----------------
 *        pg_cast definition.  cpp turns this into
 *        typedef struct FormData_pg_cast
 * ----------------
 */
#define CastRelationId    2605

CATALOG(pg_cast,2605)
{
    Oid            castsource;        /* source datatype for cast */
    Oid            casttarget;        /* destination datatype for cast */
    Oid            castfunc;        /* cast function; 0 = binary coercible */
    char        castcontext;    /* contexts in which cast can be used */
    char        castmethod;        /* cast method */
} FormData_pg_cast;

typedef FormData_pg_cast *Form_pg_cast;

/*
 * The allowable values for pg_cast.castcontext are specified by this enum.
 * Since castcontext is stored as a "char", we use ASCII codes for human
 * convenience in reading the table.  Note that internally to the backend,
 * these values are converted to the CoercionContext enum (see primnodes.h),
 * which is defined to sort in a convenient order; the ASCII codes don't
 * have to sort in any special order.
 */

typedef enum CoercionCodes
{
    COERCION_CODE_IMPLICIT = 'i',    /* coercion in context of expression */
    COERCION_CODE_ASSIGNMENT = 'a', /* coercion in context of assignment */
    COERCION_CODE_EXPLICIT = 'e'    /* explicit cast operation */
} CoercionCodes;

/*
 * The allowable values for pg_cast.castmethod are specified by this enum.
 * Since castmethod is stored as a "char", we use ASCII codes for human
 * convenience in reading the table.
 */
typedef enum CoercionMethod
{
    COERCION_METHOD_FUNCTION = 'f', /* use a function */
    COERCION_METHOD_BINARY = 'b',    /* types are binary-compatible */
    COERCION_METHOD_INOUT = 'i' /* use input/output functions */
} CoercionMethod;


/* ----------------
 *        compiler constants for pg_cast
 * ----------------
 */
#define Natts_pg_cast                5
#define Anum_pg_cast_castsource        1
#define Anum_pg_cast_casttarget        2
#define Anum_pg_cast_castfunc        3
#define Anum_pg_cast_castcontext    4
#define Anum_pg_cast_castmethod        5

/* ----------------
 *        initial contents of pg_cast
 *
 * Note: this table has OIDs, but we don't bother to assign them manually,
 * since nothing needs to know the specific OID of any built-in cast.
 * ----------------
 */

/*
 * Numeric category: implicit casts are allowed in the direction
 * int2->int4->int8->numeric->float4->float8, while casts in the
 * reverse direction are assignment-only.
 */
DATA(insert (    20     21  714 a f ));
DATA(insert (    20     23  480 a f ));
DATA(insert (    20    700  652 i f ));
DATA(insert (    20    701  482 i f ));
DATA(insert (    20 1700 1781 i f ));
DATA(insert (    21     20  754 i f ));
DATA(insert (    21     23  313 i f ));
DATA(insert (    21    700  236 i f ));
DATA(insert (    21    701  235 i f ));
DATA(insert (    21 1700 1782 i f ));
DATA(insert (    23     20  481 i f ));
DATA(insert (    23     21  314 a f ));
DATA(insert (    23    700  318 i f ));
DATA(insert (    23    701  316 i f ));
DATA(insert (    23 1700 1740 i f ));
DATA(insert (  700     20  653 a f ));
DATA(insert (  700     21  238 a f ));
DATA(insert (  700     23  319 a f ));
DATA(insert (  700    701  311 i f ));
DATA(insert (  700 1700 1742 a f ));
DATA(insert (  701     20  483 a f ));
DATA(insert (  701     21  237 a f ));
DATA(insert (  701     23  317 a f ));
DATA(insert (  701    700  312 a f ));
DATA(insert (  701 1700 1743 a f ));
DATA(insert ( 1700     20 1779 a f ));
DATA(insert ( 1700     21 1783 a f ));
DATA(insert ( 1700     23 1744 a f ));
DATA(insert ( 1700    700 1745 i f ));
DATA(insert ( 1700    701 1746 i f ));
DATA(insert (  790 1700 3823 a f ));
DATA(insert ( 1700    790 3824 a f ));
DATA(insert ( 23    790 3811 a f ));
DATA(insert ( 20    790 3812 a f ));

/* Allow explicit coercions between int4 and bool */
DATA(insert (    23    16    2557 e f ));
DATA(insert (    16    23    2558 e f ));

/*
 * OID category: allow implicit conversion from any integral type (including
 * int8, to support OID literals > 2G) to OID, as well as assignment coercion
 * from OID to int4 or int8.  Similarly for each OID-alias type.  Also allow
 * implicit coercions between OID and each OID-alias type, as well as
 * regproc<->regprocedure and regoper<->regoperator.  (Other coercions
 * between alias types must pass through OID.)    Lastly, there are implicit
 * casts from text and varchar to regclass, which exist mainly to support
 * legacy forms of nextval() and related functions.
 */
DATA(insert (    20     26 1287 i f ));
DATA(insert (    21     26  313 i f ));
DATA(insert (    23     26    0 i b ));
DATA(insert (    26     20 1288 a f ));
DATA(insert (    26     23    0 a b ));
DATA(insert (    26     24    0 i b ));
DATA(insert (    24     26    0 i b ));
DATA(insert (    20     24 1287 i f ));
DATA(insert (    21     24  313 i f ));
DATA(insert (    23     24    0 i b ));
DATA(insert (    24     20 1288 a f ));
DATA(insert (    24     23    0 a b ));
DATA(insert (    24 2202    0 i b ));
DATA(insert ( 2202     24    0 i b ));
DATA(insert (    26 2202    0 i b ));
DATA(insert ( 2202     26    0 i b ));
DATA(insert (    20 2202 1287 i f ));
DATA(insert (    21 2202  313 i f ));
DATA(insert (    23 2202    0 i b ));
DATA(insert ( 2202     20 1288 a f ));
DATA(insert ( 2202     23    0 a b ));
DATA(insert (    26 2203    0 i b ));
DATA(insert ( 2203     26    0 i b ));
DATA(insert (    20 2203 1287 i f ));
DATA(insert (    21 2203  313 i f ));
DATA(insert (    23 2203    0 i b ));
DATA(insert ( 2203     20 1288 a f ));
DATA(insert ( 2203     23    0 a b ));
DATA(insert ( 2203 2204    0 i b ));
DATA(insert ( 2204 2203    0 i b ));
DATA(insert (    26 2204    0 i b ));
DATA(insert ( 2204     26    0 i b ));
DATA(insert (    20 2204 1287 i f ));
DATA(insert (    21 2204  313 i f ));
DATA(insert (    23 2204    0 i b ));
DATA(insert ( 2204     20 1288 a f ));
DATA(insert ( 2204     23    0 a b ));
DATA(insert (    26 2205    0 i b ));
DATA(insert ( 2205     26    0 i b ));
DATA(insert (    20 2205 1287 i f ));
DATA(insert (    21 2205  313 i f ));
DATA(insert (    23 2205    0 i b ));
DATA(insert ( 2205     20 1288 a f ));
DATA(insert ( 2205     23    0 a b ));
DATA(insert (    26 2206    0 i b ));
DATA(insert ( 2206     26    0 i b ));
DATA(insert (    20 2206 1287 i f ));
DATA(insert (    21 2206  313 i f ));
DATA(insert (    23 2206    0 i b ));
DATA(insert ( 2206     20 1288 a f ));
DATA(insert ( 2206     23    0 a b ));
DATA(insert (    26 3734    0 i b ));
DATA(insert ( 3734     26    0 i b ));
DATA(insert (    20 3734 1287 i f ));
DATA(insert (    21 3734  313 i f ));
DATA(insert (    23 3734    0 i b ));
DATA(insert ( 3734     20 1288 a f ));
DATA(insert ( 3734     23    0 a b ));
DATA(insert (    26 3769    0 i b ));
DATA(insert ( 3769     26    0 i b ));
DATA(insert (    20 3769 1287 i f ));
DATA(insert (    21 3769  313 i f ));
DATA(insert (    23 3769    0 i b ));
DATA(insert ( 3769     20 1288 a f ));
DATA(insert ( 3769     23    0 a b ));
DATA(insert (    25 2205 1079 i f ));
DATA(insert ( 1043 2205 1079 i f ));
DATA(insert (    26 4096    0 i b ));
DATA(insert ( 4096     26    0 i b ));
DATA(insert (    20 4096 1287 i f ));
DATA(insert (    21 4096  313 i f ));
DATA(insert (    23 4096    0 i b ));
DATA(insert ( 4096     20 1288 a f ));
DATA(insert ( 4096     23    0 a b ));
DATA(insert (    26 4089    0 i b ));
DATA(insert ( 4089     26    0 i b ));
DATA(insert (    20 4089 1287 i f ));
DATA(insert (    21 4089  313 i f ));
DATA(insert (    23 4089    0 i b ));
DATA(insert ( 4089     20 1288 a f ));
DATA(insert ( 4089     23    0 a b ));

/*
 * String category
 */
DATA(insert (    25 1042    0 i b ));
DATA(insert (    25 1043    0 i b ));
DATA(insert ( 1042     25  401 i f ));
DATA(insert ( 1042 1043  401 i f ));
DATA(insert ( 1043     25    0 i b ));
DATA(insert ( 1043 1042    0 i b ));
DATA(insert (    18     25  946 i f ));
DATA(insert (    18 1042  860 a f ));
DATA(insert (    18 1043  946 a f ));
DATA(insert (    19     25  406 i f ));
DATA(insert (    19 1042  408 a f ));
DATA(insert (    19 1043 1401 a f ));
DATA(insert (    25     18  944 a f ));
DATA(insert ( 1042     18  944 a f ));
DATA(insert ( 1043     18  944 a f ));
DATA(insert (    25     19  407 i f ));
DATA(insert ( 1042     19  409 i f ));
DATA(insert ( 1043     19 1400 i f ));

/* Allow explicit coercions between int4 and "char" */
DATA(insert (    18     23   77 e f ));
DATA(insert (    23     18   78 e f ));

/* pg_node_tree can be coerced to, but not from, text */
DATA(insert (  194     25    0 i b ));

/* pg_ndistinct can be coerced to, but not from, bytea and text */
DATA(insert (  3361  17    0 i b ));
DATA(insert (  3361  25    0 i i ));

/* pg_dependencies can be coerced to, but not from, bytea and text */
DATA(insert (  3402  17    0 i b ));
DATA(insert (  3402  25    0 i i ));

/*
 * Datetime category
 */
DATA(insert (  702 1082 1179 a f ));
DATA(insert (  702 1083 1364 a f ));
DATA(insert (  702 1114 2023 i f ));
DATA(insert (  702 1184 1173 i f ));
DATA(insert (  703 1186 1177 i f ));
DATA(insert ( 1082 1114 2024 i f ));
DATA(insert ( 1082 1184 1174 i f ));
DATA(insert ( 1083 1186 1370 i f ));
DATA(insert ( 1083 1266 2047 i f ));
DATA(insert ( 1114    702 2030 a f ));
DATA(insert ( 1114 1082 2029 a f ));
DATA(insert ( 1114 1083 1316 a f ));
DATA(insert ( 1114 1184 2028 i f ));
DATA(insert ( 1184    702 1180 a f ));
DATA(insert ( 1184 1082 1178 a f ));
DATA(insert ( 1184 1083 2019 a f ));
DATA(insert ( 1184 1114 2027 a f ));
DATA(insert ( 1184 1266 1388 a f ));
DATA(insert ( 1186    703 1194 a f ));
DATA(insert ( 1186 1083 1419 a f ));
DATA(insert ( 1266 1083 2046 a f ));
/* Cross-category casts between int4 and abstime, reltime */
DATA(insert (    23    702    0 e b ));
DATA(insert (  702     23    0 e b ));
DATA(insert (    23    703    0 e b ));
DATA(insert (  703     23    0 e b ));

/*
 * Geometric category
 */
DATA(insert (  600    603 4091 a f ));
DATA(insert (  601    600 1532 e f ));
DATA(insert (  602    600 1533 e f ));
DATA(insert (  602    604 1449 a f ));
DATA(insert (  603    600 1534 e f ));
DATA(insert (  603    601 1541 e f ));
DATA(insert (  603    604 1448 a f ));
DATA(insert (  603    718 1479 e f ));
DATA(insert (  604    600 1540 e f ));
DATA(insert (  604    602 1447 a f ));
DATA(insert (  604    603 1446 e f ));
DATA(insert (  604    718 1474 e f ));
DATA(insert (  718    600 1416 e f ));
DATA(insert (  718    603 1480 e f ));
DATA(insert (  718    604 1544 e f ));

/*
 * MAC address category
 */
DATA(insert (  829    774    4123 i f ));
DATA(insert (  774    829    4124 i f ));

/*
 * INET category
 */
DATA(insert (  650    869    0 i b ));
DATA(insert (  869    650 1715 a f ));

/*
 * BitString category
 */
DATA(insert ( 1560 1562    0 i b ));
DATA(insert ( 1562 1560    0 i b ));
/* Cross-category casts between bit and int4, int8 */
DATA(insert (    20 1560 2075 e f ));
DATA(insert (    23 1560 1683 e f ));
DATA(insert ( 1560     20 2076 e f ));
DATA(insert ( 1560     23 1684 e f ));

/*
 * Cross-category casts to and from TEXT
 *
 * We need entries here only for a few specialized cases where the behavior
 * of the cast function differs from the datatype's I/O functions.  Otherwise,
 * parse_coerce.c will generate CoerceViaIO operations without any prompting.
 *
 * Note that the castcontext values specified here should be no stronger than
 * parse_coerce.c's automatic casts ('a' to text, 'e' from text) else odd
 * behavior will ensue when the automatic cast is applied instead of the
 * pg_cast entry!
 */
DATA(insert (  650     25  730 a f ));
DATA(insert (  869     25  730 a f ));
DATA(insert (    16     25 2971 a f ));
DATA(insert (  142     25    0 a b ));
DATA(insert (    25    142 2896 e f ));

/*
 * Cross-category casts to and from VARCHAR
 *
 * We support all the same casts as for TEXT.
 */
DATA(insert (  650 1043  730 a f ));
DATA(insert (  869 1043  730 a f ));
DATA(insert (    16 1043 2971 a f ));
DATA(insert (  142 1043    0 a b ));
DATA(insert ( 1043    142 2896 e f ));

/*
 * Cross-category casts to and from BPCHAR
 *
 * We support all the same casts as for TEXT.
 */
DATA(insert (  650 1042  730 a f ));
DATA(insert (  869 1042  730 a f ));
DATA(insert (    16 1042 2971 a f ));
DATA(insert (  142 1042    0 a b ));
DATA(insert ( 1042    142 2896 e f ));

/*
 * Length-coercion functions
 */
DATA(insert ( 1042 1042  668 i f ));
DATA(insert ( 1043 1043  669 i f ));
DATA(insert ( 1083 1083 1968 i f ));
DATA(insert ( 1114 1114 1961 i f ));
DATA(insert ( 1184 1184 1967 i f ));
DATA(insert ( 1186 1186 1200 i f ));
DATA(insert ( 1266 1266 1969 i f ));
DATA(insert ( 1560 1560 1685 i f ));
DATA(insert ( 1562 1562 1687 i f ));
DATA(insert ( 1700 1700 1703 i f ));

/* json to/from jsonb */
DATA(insert (  114 3802    0 a i ));
DATA(insert ( 3802    114    0 a i ));

#ifdef _PG_ORCL_

/* cast nvarchar2 */
DATA(insert (  782   25    0 i b ));
DATA(insert (   25  782    0 i b ));
DATA(insert (  782 1042    0 i b ));
DATA(insert ( 1042  782    0 i b ));
DATA(insert (  782 1043    0 i b ));
DATA(insert ( 1043  782    0 i b ));
DATA(insert (  782  700    0 i i ));
DATA(insert (  700  782    0 i i ));
DATA(insert (  782  701    0 i i ));
DATA(insert (  701  782    0 i i ));
DATA(insert (  782   23    0 i i ));
DATA(insert (   23  782    0 i i ));
DATA(insert (  782   21    0 i i ));
DATA(insert (   21  782    0 i i ));
DATA(insert (  782   20    0 i i ));
DATA(insert (   20  782    0 i i ));
DATA(insert (  782 1700    0 i i ));
DATA(insert ( 1700  782    0 i i ));
DATA(insert (  782 1082    0 i i ));
DATA(insert ( 1082  782    0 i i ));
DATA(insert (  782 1114    0 i i ));
DATA(insert ( 1114  782    0 i i ));
DATA(insert (  782 1184    0 i i ));
DATA(insert ( 1184  782    0 i i ));
DATA(insert (  782 1186    0 i i ));
DATA(insert ( 1186  782    0 i i ));
DATA(insert (  782  782 3425 i f ));

/* cast varchar2 */
DATA(insert (  781   25    0 i b ));
DATA(insert (   25  781    0 i b ));
DATA(insert (  781 1042    0 i b ));
DATA(insert ( 1042  781    0 i b ));
DATA(insert (  781 1043    0 i b ));
DATA(insert ( 1043  781    0 i b ));
DATA(insert (  781  700    0 i i ));
DATA(insert (  700  781    0 i i ));
DATA(insert (  781  701    0 i i ));
DATA(insert (  701  781    0 i i ));
DATA(insert (  781   23    0 i i ));
DATA(insert (   23  781    0 i i ));
DATA(insert (  781   21    0 i i ));
DATA(insert (   21  781    0 i i ));
DATA(insert (  781   20    0 i i ));
DATA(insert (   20  781    0 i i ));
DATA(insert (  781 1700    0 i i ));
DATA(insert ( 1700  781    0 i i ));
DATA(insert (  781 1082    0 i i ));
DATA(insert ( 1082  781    0 i i ));
DATA(insert (  781 1114    0 i i ));
DATA(insert ( 1114  781    0 i i ));
DATA(insert (  781 1184    0 i i ));
DATA(insert ( 1184  781    0 i i ));
DATA(insert (  781 1186    0 i i ));
DATA(insert ( 1186  781    0 i i ));
DATA(insert (  781  781 4564 i f ));

/* cast to text */
DATA(insert ( 1186   25 4159 i f ));
DATA(insert ( 1114   25 4160 i f ));
DATA(insert ( 1184   25 4161 i f ));
DATA(insert ( 1082   25    0 i i ));

#endif

#endif                            /* PG_CAST_H */
