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
/*
 * pgcrypto.c
 *        Various cryptographic stuff for PostgreSQL.
 *
 * Copyright (c) 2001 Marko Kreen
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * contrib/pgcrypto/pgcrypto.c
 */

#include "postgres.h"

#include <ctype.h>

#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/uuid.h"

#include "contrib/pgcrypto/px.h"
#include "contrib/pgcrypto/px-crypt.h"
#include "contrib/pgcrypto/pgcrypto.h"

PG_MODULE_MAGIC;

/* private stuff */

typedef int (*PFN) (const char *name, void **res);
static void *find_provider(text *name, PFN pf, char *desc, int silent);

/* SQL function: hash(bytea, text) returns bytea */
PG_FUNCTION_INFO_V1(pg_digest);

Datum
pg_digest(PG_FUNCTION_ARGS)
{
    bytea       *arg;
    text       *name;
    unsigned    len,
                hlen;
    PX_MD       *md;
    bytea       *res;

    name = PG_GETARG_TEXT_P(1);

    /* will give error if fails */
    md = find_provider(name, (PFN) px_find_digest, "Digest", 0);

    hlen = px_md_result_size(md);

    res = (text *) crypt_alloc(hlen + VARHDRSZ);
    SET_VARSIZE(res, hlen + VARHDRSZ);

    arg = PG_GETARG_BYTEA_P(0);
    len = VARSIZE(arg) - VARHDRSZ;

    px_md_update(md, (uint8 *) VARDATA(arg), len);
    px_md_finish(md, (uint8 *) VARDATA(res));
    px_md_free(md);

    PG_FREE_IF_COPY(arg, 0);
    PG_FREE_IF_COPY(name, 1);

    PG_RETURN_BYTEA_P(res);
}

/* SQL function: hmac(data:bytea, key:bytea, type:text) returns bytea */
PG_FUNCTION_INFO_V1(pg_hmac);

Datum
pg_hmac(PG_FUNCTION_ARGS)
{
    bytea       *arg;
    bytea       *key;
    text       *name;
    unsigned    len,
                hlen,
                klen;
    PX_HMAC    *h;
    bytea       *res;

    name = PG_GETARG_TEXT_P(2);

    /* will give error if fails */
    h = find_provider(name, (PFN) px_find_hmac, "HMAC", 0);

    hlen = px_hmac_result_size(h);

    res = (text *) crypt_alloc(hlen + VARHDRSZ);
    SET_VARSIZE(res, hlen + VARHDRSZ);

    arg = PG_GETARG_BYTEA_P(0);
    key = PG_GETARG_BYTEA_P(1);
    len = VARSIZE(arg) - VARHDRSZ;
    klen = VARSIZE(key) - VARHDRSZ;

    px_hmac_init(h, (uint8 *) VARDATA(key), klen);
    px_hmac_update(h, (uint8 *) VARDATA(arg), len);
    px_hmac_finish(h, (uint8 *) VARDATA(res));
    px_hmac_free(h);

    PG_FREE_IF_COPY(arg, 0);
    PG_FREE_IF_COPY(key, 1);
    PG_FREE_IF_COPY(name, 2);

    PG_RETURN_BYTEA_P(res);
}


/* SQL function: pg_gen_salt(text) returns text */
PG_FUNCTION_INFO_V1(pg_gen_salt);

Datum
pg_gen_salt(PG_FUNCTION_ARGS)
{
    text       *arg0 = PG_GETARG_TEXT_PP(0);
    int            len;
    char        buf[PX_MAX_SALT_LEN + 1];

    text_to_cstring_buffer(arg0, buf, sizeof(buf));
    len = px_gen_salt(buf, buf, 0);
    if (len < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("gen_salt: %s", px_strerror(len))));

    PG_FREE_IF_COPY(arg0, 0);

    PG_RETURN_TEXT_P(cstring_to_text_with_len(buf, len));
}

/* SQL function: pg_gen_salt(text, int4) returns text */
PG_FUNCTION_INFO_V1(pg_gen_salt_rounds);

Datum
pg_gen_salt_rounds(PG_FUNCTION_ARGS)
{
    text       *arg0 = PG_GETARG_TEXT_PP(0);
    int            rounds = PG_GETARG_INT32(1);
    int            len;
    char        buf[PX_MAX_SALT_LEN + 1];

    text_to_cstring_buffer(arg0, buf, sizeof(buf));
    len = px_gen_salt(buf, buf, rounds);
    if (len < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("gen_salt: %s", px_strerror(len))));

    PG_FREE_IF_COPY(arg0, 0);

    PG_RETURN_TEXT_P(cstring_to_text_with_len(buf, len));
}

/* SQL function: pg_crypt(psw:text, salt:text) returns text */
PG_FUNCTION_INFO_V1(pg_crypt);

Datum
pg_crypt(PG_FUNCTION_ARGS)
{
    text       *arg0 = PG_GETARG_TEXT_PP(0);
    text       *arg1 = PG_GETARG_TEXT_PP(1);
    char       *buf0,
               *buf1,
               *cres,
               *resbuf;
    text       *res;

    buf0 = text_to_cstring(arg0);
    buf1 = text_to_cstring(arg1);

    resbuf = crypt_alloc(PX_MAX_CRYPT);
    memset(resbuf, 0, PX_MAX_CRYPT);

    cres = px_crypt(buf0, buf1, resbuf, PX_MAX_CRYPT);

    crypt_free(buf0);
    crypt_free(buf1);

    if (cres == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
                 errmsg("crypt(3) returned NULL")));

    res = cstring_to_text(cres);

    crypt_free(resbuf);

    PG_FREE_IF_COPY(arg0, 0);
    PG_FREE_IF_COPY(arg1, 1);

    PG_RETURN_TEXT_P(res);
}

/* SQL function: pg_encrypt(bytea, bytea, text) returns bytea */
PG_FUNCTION_INFO_V1(pg_encrypt);

Datum
pg_encrypt(PG_FUNCTION_ARGS)
{
    int            err;
    bytea       *data,
               *key,
               *res;
    text       *type;
    PX_Combo   *c;
    unsigned    dlen,
                klen,
                rlen;

    type = PG_GETARG_TEXT_P(2);
    c = find_provider(type, (PFN) px_find_combo, "Cipher", 0);

    data = PG_GETARG_BYTEA_P(0);
    key = PG_GETARG_BYTEA_P(1);
    dlen = VARSIZE(data) - VARHDRSZ;
    klen = VARSIZE(key) - VARHDRSZ;

    rlen = px_combo_encrypt_len(c, dlen);
    res = crypt_alloc(VARHDRSZ + rlen);

    err = px_combo_init(c, (uint8 *) VARDATA(key), klen, NULL, 0);
    if (!err)
        err = px_combo_encrypt(c, (uint8 *) VARDATA(data), dlen,
                               (uint8 *) VARDATA(res), &rlen);
    px_combo_free(c);

    PG_FREE_IF_COPY(data, 0);
    PG_FREE_IF_COPY(key, 1);
    PG_FREE_IF_COPY(type, 2);

    if (err)
    {
        crypt_free(res);
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
                 errmsg("encrypt error: %s", px_strerror(err))));
    }

    SET_VARSIZE(res, VARHDRSZ + rlen);
    PG_RETURN_BYTEA_P(res);
}

/* SQL function: pg_decrypt(bytea, bytea, text) returns bytea */
PG_FUNCTION_INFO_V1(pg_decrypt);

Datum
pg_decrypt(PG_FUNCTION_ARGS)
{
    int            err;
    bytea       *data,
               *key,
               *res;
    text       *type;
    PX_Combo   *c;
    unsigned    dlen,
                klen,
                rlen;

    type = PG_GETARG_TEXT_P(2);
    c = find_provider(type, (PFN) px_find_combo, "Cipher", 0);

    data = PG_GETARG_BYTEA_P(0);
    key = PG_GETARG_BYTEA_P(1);
    dlen = VARSIZE(data) - VARHDRSZ;
    klen = VARSIZE(key) - VARHDRSZ;

    rlen = px_combo_decrypt_len(c, dlen);
    res = crypt_alloc(VARHDRSZ + rlen);

    err = px_combo_init(c, (uint8 *) VARDATA(key), klen, NULL, 0);
    if (!err)
        err = px_combo_decrypt(c, (uint8 *) VARDATA(data), dlen,
                               (uint8 *) VARDATA(res), &rlen);

    px_combo_free(c);

    if (err)
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
                 errmsg("decrypt error: %s", px_strerror(err))));

    SET_VARSIZE(res, VARHDRSZ + rlen);

    PG_FREE_IF_COPY(data, 0);
    PG_FREE_IF_COPY(key, 1);
    PG_FREE_IF_COPY(type, 2);

    PG_RETURN_BYTEA_P(res);
}

/* SQL function: pg_encrypt_iv(bytea, bytea, bytea, text) returns bytea */
PG_FUNCTION_INFO_V1(pg_encrypt_iv);

Datum
pg_encrypt_iv(PG_FUNCTION_ARGS)
{
    int            err;
    bytea       *data,
               *key,
               *iv,
               *res;
    text       *type;
    PX_Combo   *c;
    unsigned    dlen,
                klen,
                ivlen,
                rlen;

    type = PG_GETARG_TEXT_P(3);
    c = find_provider(type, (PFN) px_find_combo, "Cipher", 0);

    data = PG_GETARG_BYTEA_P(0);
    key = PG_GETARG_BYTEA_P(1);
    iv = PG_GETARG_BYTEA_P(2);
    dlen = VARSIZE(data) - VARHDRSZ;
    klen = VARSIZE(key) - VARHDRSZ;
    ivlen = VARSIZE(iv) - VARHDRSZ;

    rlen = px_combo_encrypt_len(c, dlen);
    res = crypt_alloc(VARHDRSZ + rlen);

    err = px_combo_init(c, (uint8 *) VARDATA(key), klen,
                        (uint8 *) VARDATA(iv), ivlen);
    if (!err)
        err = px_combo_encrypt(c, (uint8 *) VARDATA(data), dlen,
                               (uint8 *) VARDATA(res), &rlen);

    px_combo_free(c);

    if (err)
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
                 errmsg("encrypt_iv error: %s", px_strerror(err))));

    SET_VARSIZE(res, VARHDRSZ + rlen);

    PG_FREE_IF_COPY(data, 0);
    PG_FREE_IF_COPY(key, 1);
    PG_FREE_IF_COPY(iv, 2);
    PG_FREE_IF_COPY(type, 3);

    PG_RETURN_BYTEA_P(res);
}

/* SQL function: pg_decrypt_iv(bytea, bytea, bytea, text) returns bytea */
PG_FUNCTION_INFO_V1(pg_decrypt_iv);

Datum
pg_decrypt_iv(PG_FUNCTION_ARGS)
{
    int            err;
    bytea       *data,
               *key,
               *iv,
               *res;
    text       *type;
    PX_Combo   *c;
    unsigned    dlen,
                klen,
                rlen,
                ivlen;

    type = PG_GETARG_TEXT_P(3);
    c = find_provider(type, (PFN) px_find_combo, "Cipher", 0);

    data = PG_GETARG_BYTEA_P(0);
    key = PG_GETARG_BYTEA_P(1);
    iv = PG_GETARG_BYTEA_P(2);
    dlen = VARSIZE(data) - VARHDRSZ;
    klen = VARSIZE(key) - VARHDRSZ;
    ivlen = VARSIZE(iv) - VARHDRSZ;

    rlen = px_combo_decrypt_len(c, dlen);
    res = crypt_alloc(VARHDRSZ + rlen);

    err = px_combo_init(c, (uint8 *) VARDATA(key), klen,
                        (uint8 *) VARDATA(iv), ivlen);
    if (!err)
        err = px_combo_decrypt(c, (uint8 *) VARDATA(data), dlen,
                               (uint8 *) VARDATA(res), &rlen);

    px_combo_free(c);

    if (err)
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
                 errmsg("decrypt_iv error: %s", px_strerror(err))));

    SET_VARSIZE(res, VARHDRSZ + rlen);

    PG_FREE_IF_COPY(data, 0);
    PG_FREE_IF_COPY(key, 1);
    PG_FREE_IF_COPY(iv, 2);
    PG_FREE_IF_COPY(type, 3);

    PG_RETURN_BYTEA_P(res);
}

/* SQL function: pg_random_bytes(int4) returns bytea */
PG_FUNCTION_INFO_V1(pg_random_bytes);

Datum
pg_random_bytes(PG_FUNCTION_ARGS)
{
    int            err;
    int            len = PG_GETARG_INT32(0);
    bytea       *res;

    if (len < 1 || len > 1024)
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
                 errmsg("Length not in range")));

    res = crypt_alloc(VARHDRSZ + len);
    SET_VARSIZE(res, VARHDRSZ + len);

    /* generate result */
    err = px_get_random_bytes((uint8 *) VARDATA(res), len);
    if (err < 0)
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
                 errmsg("Random generator error: %s", px_strerror(err))));

    PG_RETURN_BYTEA_P(res);
}

/* SQL function: gen_random_uuid() returns uuid */
PG_FUNCTION_INFO_V1(pg_random_uuid);

Datum
pg_random_uuid(PG_FUNCTION_ARGS)
{
    uint8       *buf = (uint8 *) crypt_alloc(UUID_LEN);
    int            err;

    /* generate random bits */
    err = px_get_random_bytes(buf, UUID_LEN);
    if (err < 0)
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
                 errmsg("Random generator error: %s", px_strerror(err))));

    /*
     * Set magic numbers for a "version 4" (pseudorandom) UUID, see
     * http://tools.ietf.org/html/rfc4122#section-4.4
     */
    buf[6] = (buf[6] & 0x0f) | 0x40;    /* "version" field */
    buf[8] = (buf[8] & 0x3f) | 0x80;    /* "variant" field */

    PG_RETURN_UUID_P((pg_uuid_t *) buf);
}

static void *
find_provider(text *name,
              PFN provider_lookup,
              char *desc, int silent)
{
    void       *res;
    char       *buf;
    int            err;

    buf = downcase_truncate_identifier(VARDATA(name),
                                       VARSIZE(name) - VARHDRSZ,
                                       false);

    err = provider_lookup(buf, &res);

    if (err && !silent)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("Cannot use \"%s\": %s", buf, px_strerror(err))));

    crypt_free(buf);

    return err ? NULL : res;
}


void crypt_free(void * ptr)
{
    free(ptr);
    return;
}

void * crypt_alloc(int size)
{
    return malloc(size);
}

void * crypt_realloc(void * ptr, int size)
{
    return realloc(ptr, size);
}
void * crypt_memset(void *s, int c, size_t n)
{
    return memset(s,c,n);
}


