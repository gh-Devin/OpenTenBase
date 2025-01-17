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
#ifndef __PG_ORACLE__
#define __PG_ORACLE__

#include "postgres.h"
#include "catalog/catversion.h"
#include "nodes/pg_list.h"
#include <sys/time.h>
#include "utils/datetime.h"
#include "utils/datum.h"
/* 
*function macro 
*/
#define PG_GETARG_IF_EXISTS(n, type, defval) \
    ((PG_NARGS() > (n) && !PG_ARGISNULL(n)) ? PG_GETARG_##type(n) : (defval))

#define PG_GETARG_TEXT_P_IF_EXISTS(_n) \
    (PG_NARGS() > (_n) ? PG_GETARG_TEXT_P(_n) : NULL)
#define PG_GETARG_TEXT_P_IF_NULL(_n)\
    (PG_ARGISNULL(_n) ? NULL : PG_GETARG_TEXT_P_IF_EXISTS(_n))

#define PG_GETARG_TEXT_PP_IF_EXISTS(_n) \
    (PG_NARGS() > (_n) ? PG_GETARG_TEXT_PP(_n) : NULL)
#define PG_GETARG_TEXT_PP_IF_NULL(_n)\
    (PG_ARGISNULL(_n) ? NULL : PG_GETARG_TEXT_PP_IF_EXISTS(_n))

#define PG_GETARG_INT32_0_IF_EXISTS(_n) \
    (PG_NARGS() > (_n) ? PG_GETARG_INT32(_n) : 0)
#define PG_GETARG_INT32_0_IF_NULL(_n) \
    (PG_ARGISNULL(_n) ? 0 : PG_GETARG_INT32_0_IF_EXISTS(_n))

#define PG_GETARG_INT32_1_IF_EXISTS(_n) \
    (PG_NARGS() > (_n) ? PG_GETARG_INT32(_n) : 1)
#define PG_GETARG_INT32_1_IF_NULL(_n) \
    (PG_ARGISNULL(_n) ? 1 : PG_GETARG_INT32_1_IF_EXISTS(_n))

#define PG_RETURN_NULL_IF_EMPTY_TEXT(_in) \
    do { \
        if (_in != NULL) \
        { \
            char _instr[2] = {0, 0}; \
            text_to_cstring_buffer(_in, _instr, 2); \
            if (_instr[0] == '\0') \
                PG_RETURN_NULL(); \
        } \
    } while (0)

/* 
* related function declaration
*/
extern Datum orcl_lpad(PG_FUNCTION_ARGS);
extern Datum orcl_rpad(PG_FUNCTION_ARGS);

#endif  /* __PG_ORACLE__ */
