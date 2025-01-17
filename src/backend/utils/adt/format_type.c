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
 * format_type.c
 *      Display type names "nicely".
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *      src/backend/utils/adt/format_type.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/syscache.h"
#include "mb/pg_wchar.h"

#define MAX_INT32_LEN 11

static char *format_type_internal(Oid type_oid, int32 typemod,
                     bool typemod_given, bool allow_invalid,
                     bool force_qualify);
static char *printTypmod(const char *typname, int32 typmod, Oid typmodout);


/*
 * SQL function: format_type(type_oid, typemod)
 *
 * `type_oid' is from pg_type.oid, `typemod' is from
 * pg_attribute.atttypmod. This function will get the type name and
 * format it and the modifier to canonical SQL format, if the type is
 * a standard type. Otherwise you just get pg_type.typname back,
 * double quoted if it contains funny characters or matches a keyword.
 *
 * If typemod is NULL then we are formatting a type name in a context where
 * no typemod is available, eg a function argument or result type.  This
 * yields a slightly different result from specifying typemod = -1 in some
 * cases.  Given typemod = -1 we feel compelled to produce an output that
 * the parser will interpret as having typemod -1, so that pg_dump will
 * produce CREATE TABLE commands that recreate the original state.  But
 * given NULL typemod, we assume that the parser's interpretation of
 * typemod doesn't matter, and so we are willing to output a slightly
 * "prettier" representation of the same type.  For example, type = bpchar
 * and typemod = NULL gets you "character", whereas typemod = -1 gets you
 * "bpchar" --- the former will be interpreted as character(1) by the
 * parser, which does not yield typemod -1.
 *
 * XXX encoding a meaning in typemod = NULL is ugly; it'd have been
 * cleaner to make two functions of one and two arguments respectively.
 * Not worth changing it now, however.
 */
Datum
format_type(PG_FUNCTION_ARGS)
{
    Oid            type_oid;
    int32        typemod;
    char       *result;

    /* Since this function is not strict, we must test for null args */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    type_oid = PG_GETARG_OID(0);

    if (PG_ARGISNULL(1))
        result = format_type_internal(type_oid, -1, false, true, false);
    else
    {
        typemod = PG_GETARG_INT32(1);
        result = format_type_internal(type_oid, typemod, true, true, false);
    }

    PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * This version is for use within the backend in error messages, etc.
 * One difference is that it will fail for an invalid type.
 *
 * The result is always a palloc'd string.
 */
char *
format_type_be(Oid type_oid)
{
    return format_type_internal(type_oid, -1, false, false, false);
}

/*
 * This version returns a name that is always qualified (unless it's one
 * of the SQL-keyword type names, such as TIMESTAMP WITH TIME ZONE).
 */
char *
format_type_be_qualified(Oid type_oid)
{
    return format_type_internal(type_oid, -1, false, false, true);
}

/*
 * This version allows a nondefault typemod to be specified.
 */
char *
format_type_with_typemod(Oid type_oid, int32 typemod)
{
    return format_type_internal(type_oid, typemod, true, false, false);
}

/*
 * This version allows a nondefault typemod to be specified, and forces
 * qualification of normal type names.
 */
char *
format_type_with_typemod_qualified(Oid type_oid, int32 typemod)
{
    return format_type_internal(type_oid, typemod, true, false, true);
}

/*
 * Common workhorse.
 */
static char *
format_type_internal(Oid type_oid, int32 typemod,
                     bool typemod_given, bool allow_invalid,
                     bool force_qualify)
{// #lizard forgives
    bool        with_typemod = typemod_given && (typemod >= 0);
    HeapTuple    tuple;
    Form_pg_type typeform;
    Oid            array_base_type;
    bool        is_array;
    char       *buf;

    if (type_oid == InvalidOid && allow_invalid)
        return pstrdup("-");

    tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
    if (!HeapTupleIsValid(tuple))
    {
        if (allow_invalid)
            return pstrdup("???");
        else
            elog(ERROR, "cache lookup failed for type %u", type_oid);
    }
    typeform = (Form_pg_type) GETSTRUCT(tuple);

    /*
     * Check if it's a regular (variable length) array type.  Fixed-length
     * array types such as "name" shouldn't get deconstructed.  As of Postgres
     * 8.1, rather than checking typlen we check the toast property, and don't
     * deconstruct "plain storage" array types --- this is because we don't
     * want to show oidvector as oid[].
     */
    array_base_type = typeform->typelem;

    if (array_base_type != InvalidOid &&
        typeform->typstorage != 'p')
    {
        /* Switch our attention to the array element type */
        ReleaseSysCache(tuple);
        tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(array_base_type));
        if (!HeapTupleIsValid(tuple))
        {
            if (allow_invalid)
                return pstrdup("???[]");
            else
                elog(ERROR, "cache lookup failed for type %u", type_oid);
        }
        typeform = (Form_pg_type) GETSTRUCT(tuple);
        type_oid = array_base_type;
        is_array = true;
    }
    else
        is_array = false;

    /*
     * See if we want to special-case the output for certain built-in types.
     * Note that these special cases should all correspond to special
     * productions in gram.y, to ensure that the type name will be taken as a
     * system type, not a user type of the same name.
     *
     * If we do not provide a special-case output here, the type name will be
     * handled the same way as a user type name --- in particular, it will be
     * double-quoted if it matches any lexer keyword.  This behavior is
     * essential for some cases, such as types "bit" and "char".
     */
    buf = NULL;                    /* flag for no special case */

    switch (type_oid)
    {
        case BITOID:
            if (with_typemod)
                buf = printTypmod("bit", typemod, typeform->typmodout);
            else if (typemod_given)
            {
                /*
                 * bit with typmod -1 is not the same as BIT, which means
                 * BIT(1) per SQL spec.  Report it as the quoted typename so
                 * that parser will not assign a bogus typmod.
                 */
            }
            else
                buf = pstrdup("bit");
            break;

        case BOOLOID:
            buf = pstrdup("boolean");
            break;

        case BPCHAROID:
            if (with_typemod)
                buf = printTypmod("character", typemod, typeform->typmodout);
            else if (typemod_given)
            {
                /*
                 * bpchar with typmod -1 is not the same as CHARACTER, which
                 * means CHARACTER(1) per SQL spec.  Report it as bpchar so
                 * that parser will not assign a bogus typmod.
                 */
            }
            else
                buf = pstrdup("character");
            break;

        case FLOAT4OID:
            buf = pstrdup("real");
            break;

        case FLOAT8OID:
            buf = pstrdup("double precision");
            break;

        case INT2OID:
            buf = pstrdup("smallint");
            break;

        case INT4OID:
            buf = pstrdup("integer");
            break;

        case INT8OID:
            buf = pstrdup("bigint");
            break;

        case NUMERICOID:
            if (with_typemod)
                buf = printTypmod("numeric", typemod, typeform->typmodout);
            else
                buf = pstrdup("numeric");
            break;

        case INTERVALOID:
            if (with_typemod)
                buf = printTypmod("interval", typemod, typeform->typmodout);
            else
                buf = pstrdup("interval");
            break;

        case TIMEOID:
            if (with_typemod)
                buf = printTypmod("time", typemod, typeform->typmodout);
            else
                buf = pstrdup("time without time zone");
            break;

        case TIMETZOID:
            if (with_typemod)
                buf = printTypmod("time", typemod, typeform->typmodout);
            else
                buf = pstrdup("time with time zone");
            break;

        case TIMESTAMPOID:
            if (with_typemod)
                buf = printTypmod("timestamp", typemod, typeform->typmodout);
            else
                buf = pstrdup("timestamp without time zone");
            break;

        case TIMESTAMPTZOID:
            if (with_typemod)
                buf = printTypmod("timestamp", typemod, typeform->typmodout);
            else
                buf = pstrdup("timestamp with time zone");
            break;

        case VARBITOID:
            if (with_typemod)
                buf = printTypmod("bit varying", typemod, typeform->typmodout);
            else
                buf = pstrdup("bit varying");
            break;

        case VARCHAROID:
            if (with_typemod)
                buf = printTypmod("character varying", typemod, typeform->typmodout);
            else
                buf = pstrdup("character varying");
            break;
#ifdef _PG_ORCL_
        case VARCHAR2OID:
            if (with_typemod)
                buf = printTypmod("varchar2", typemod, typeform->typmodout);
            else
                buf = pstrdup("varchar2");
            break;
        case NVARCHAR2OID:
            if (with_typemod)
                buf = printTypmod("nvarchar2", typemod, typeform->typmodout);
            else
                buf = pstrdup("nvarchar2");
            break;
#endif
    }

    if (buf == NULL)
    {
        /*
         * Default handling: report the name as it appears in the catalog.
         * Here, we must qualify the name if it is not visible in the search
         * path, and we must double-quote it if it's not a standard identifier
         * or if it matches any keyword.
         */
        char       *nspname;
        char       *typname;

        if (!force_qualify && TypeIsVisible(type_oid))
            nspname = NULL;
        else
            nspname = get_namespace_name_or_temp(typeform->typnamespace);

        typname = NameStr(typeform->typname);

        buf = quote_qualified_identifier(nspname, typname);

        if (with_typemod)
            buf = printTypmod(buf, typemod, typeform->typmodout);
    }

    if (is_array)
        buf = psprintf("%s[]", buf);

    ReleaseSysCache(tuple);

    return buf;
}


/*
 * Add typmod decoration to the basic type name
 */
static char *
printTypmod(const char *typname, int32 typmod, Oid typmodout)
{
    char       *res;

    /* Shouldn't be called if typmod is -1 */
    Assert(typmod >= 0);

    if (typmodout == InvalidOid)
    {
        /* Default behavior: just print the integer typmod with parens */
        res = psprintf("%s(%d)", typname, (int) typmod);
    }
    else
    {
        /* Use the type-specific typmodout procedure */
        char       *tmstr;

        tmstr = DatumGetCString(OidFunctionCall1(typmodout,
                                                 Int32GetDatum(typmod)));
        res = psprintf("%s%s", typname, tmstr);
    }

    return res;
}


/*
 * type_maximum_size --- determine maximum width of a variable-width column
 *
 * If the max width is indeterminate, return -1.  In particular, we return
 * -1 for any type not known to this routine.  We assume the caller has
 * already determined that the type is a variable-width type, so it's not
 * necessary to look up the type's pg_type tuple here.
 *
 * This may appear unrelated to format_type(), but in fact the two routines
 * share knowledge of the encoding of typmod for different types, so it's
 * convenient to keep them together.  (XXX now that most of this knowledge
 * has been pushed out of format_type into the typmodout functions, it's
 * interesting to wonder if it's worth trying to factor this code too...)
 */
int32
type_maximum_size(Oid type_oid, int32 typemod)
{// #lizard forgives
    if (typemod < 0)
        return -1;

    switch (type_oid)
    {
        case BPCHAROID:
        case VARCHAROID:
#ifdef _PG_ORCL_
        case VARCHAR2OID:
        case NVARCHAR2OID:
#endif
            /* typemod includes varlena header */

            /* typemod is in characters not bytes */
            return (typemod - VARHDRSZ) *
                pg_encoding_max_length(GetDatabaseEncoding())
                + VARHDRSZ;

        case NUMERICOID:
            return numeric_maximum_size(typemod);

        case VARBITOID:
        case BITOID:
            /* typemod is the (max) number of bits */
            return (typemod + (BITS_PER_BYTE - 1)) / BITS_PER_BYTE
                + 2 * sizeof(int32);
    }

    /* Unknown type, or unlimited-width type such as 'text' */
    return -1;
}


/*
 * oidvectortypes            - converts a vector of type OIDs to "typname" list
 */
Datum
oidvectortypes(PG_FUNCTION_ARGS)
{
    oidvector  *oidArray = (oidvector *) PG_GETARG_POINTER(0);
    char       *result;
    int            numargs = oidArray->dim1;
    int            num;
    size_t        total;
    size_t        left;

    total = 20 * numargs + 1;
    result = palloc(total);
    result[0] = '\0';
    left = total - 1;

    for (num = 0; num < numargs; num++)
    {
        char       *typename = format_type_internal(oidArray->values[num], -1,
                                                    false, true, false);
        size_t        slen = strlen(typename);

        if (left < (slen + 2))
        {
            total += slen + 2;
            result = repalloc(result, total);
            left += slen + 2;
        }

        if (num > 0)
        {
            strcat(result, ", ");
            left -= 2;
        }
        strcat(result, typename);
        left -= slen;
    }

    PG_RETURN_TEXT_P(cstring_to_text(result));
}
