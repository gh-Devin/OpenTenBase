/*-------------------------------------------------------------------------
 *
 * fmgr.c
 *      The Postgres function manager.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *      src/backend/utils/fmgr/fmgr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/tuptoaster.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "executor/functions.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgrtab.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#ifdef __OPENTENBASE__
#include "pgxc/poolmgr.h"
#endif
/*
 * Hooks for function calls
 */
PGDLLIMPORT needs_fmgr_hook_type needs_fmgr_hook = NULL;
PGDLLIMPORT fmgr_hook_type fmgr_hook = NULL;

/*
 * Hashtable for fast lookup of external C functions
 */
typedef struct
{
    /* fn_oid is the hash key and so must be first! */
    Oid            fn_oid;            /* OID of an external C function */
    TransactionId fn_xmin;        /* for checking up-to-dateness */
    ItemPointerData fn_tid;
    PGFunction    user_fn;        /* the function's address */
    const Pg_finfo_record *inforec; /* address of its info record */
} CFuncHashTabEntry;

static HTAB *CFuncHash = NULL;


static void fmgr_info_cxt_security(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt,
                       bool ignore_security);
static void fmgr_info_C_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple);
static void fmgr_info_other_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple);
static CFuncHashTabEntry *lookup_C_func(HeapTuple procedureTuple);
static void record_C_func(HeapTuple procedureTuple,
              PGFunction user_fn, const Pg_finfo_record *inforec);
static Datum fmgr_security_definer(PG_FUNCTION_ARGS);


/*
 * Lookup routines for builtin-function table.  We can search by either Oid
 * or name, but search by Oid is much faster.
 */

static const FmgrBuiltin *
fmgr_isbuiltin(Oid id)
{
    int            low = 0;
    int            high = fmgr_nbuiltins - 1;

    /*
     * Loop invariant: low is the first index that could contain target entry,
     * and high is the last index that could contain it.
     */
    while (low <= high)
    {
        int            i = (high + low) / 2;
        const FmgrBuiltin *ptr = &fmgr_builtins[i];

        if (id == ptr->foid)
            return ptr;
        else if (id > ptr->foid)
            low = i + 1;
        else
            high = i - 1;
    }
    return NULL;
}

/*
 * Lookup a builtin by name.  Note there can be more than one entry in
 * the array with the same name, but they should all point to the same
 * routine.
 */
static const FmgrBuiltin *
fmgr_lookupByName(const char *name)
{
    int            i;

    for (i = 0; i < fmgr_nbuiltins; i++)
    {
        if (strcmp(name, fmgr_builtins[i].funcName) == 0)
            return fmgr_builtins + i;
    }
    return NULL;
}

/*
 * This routine fills a FmgrInfo struct, given the OID
 * of the function to be called.
 *
 * The caller's CurrentMemoryContext is used as the fn_mcxt of the info
 * struct; this means that any subsidiary data attached to the info struct
 * (either by fmgr_info itself, or later on by a function call handler)
 * will be allocated in that context.  The caller must ensure that this
 * context is at least as long-lived as the info struct itself.  This is
 * not a problem in typical cases where the info struct is on the stack or
 * in freshly-palloc'd space.  However, if one intends to store an info
 * struct in a long-lived table, it's better to use fmgr_info_cxt.
 */
void
fmgr_info(Oid functionId, FmgrInfo *finfo)
{
    fmgr_info_cxt_security(functionId, finfo, CurrentMemoryContext, false);
}

/*
 * Fill a FmgrInfo struct, specifying a memory context in which its
 * subsidiary data should go.
 */
void
fmgr_info_cxt(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt)
{
    fmgr_info_cxt_security(functionId, finfo, mcxt, false);
}

/*
 * This one does the actual work.  ignore_security is ordinarily false
 * but is set to true when we need to avoid recursion.
 */
static void
fmgr_info_cxt_security(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt,
                       bool ignore_security)
{// #lizard forgives
    const FmgrBuiltin *fbp;
    HeapTuple    procedureTuple;
    Form_pg_proc procedureStruct;
    Datum        prosrcdatum;
    bool        isnull;
    char       *prosrc;

    /*
     * fn_oid *must* be filled in last.  Some code assumes that if fn_oid is
     * valid, the whole struct is valid.  Some FmgrInfo struct's do survive
     * elogs.
     */
    finfo->fn_oid = InvalidOid;
    finfo->fn_extra = NULL;
    finfo->fn_mcxt = mcxt;
    finfo->fn_expr = NULL;        /* caller may set this later */

    if ((fbp = fmgr_isbuiltin(functionId)) != NULL)
    {
        /*
         * Fast path for builtin functions: don't bother consulting pg_proc
         */
        finfo->fn_nargs = fbp->nargs;
        finfo->fn_strict = fbp->strict;
        finfo->fn_retset = fbp->retset;
        finfo->fn_stats = TRACK_FUNC_ALL;    /* ie, never track */
        finfo->fn_addr = fbp->func;
        finfo->fn_oid = functionId;
        return;
    }

    /* Otherwise we need the pg_proc entry */
    procedureTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(functionId));
    if (!HeapTupleIsValid(procedureTuple))
        elog(ERROR, "cache lookup failed for function %u", functionId);
    procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

    finfo->fn_nargs = procedureStruct->pronargs;
    finfo->fn_strict = procedureStruct->proisstrict;
    finfo->fn_retset = procedureStruct->proretset;

    /*
     * If it has prosecdef set, non-null proconfig, or if a plugin wants to
     * hook function entry/exit, use fmgr_security_definer call handler ---
     * unless we are being called again by fmgr_security_definer or
     * fmgr_info_other_lang.
     *
     * When using fmgr_security_definer, function stats tracking is always
     * disabled at the outer level, and instead we set the flag properly in
     * fmgr_security_definer's private flinfo and implement the tracking
     * inside fmgr_security_definer.  This loses the ability to charge the
     * overhead of fmgr_security_definer to the function, but gains the
     * ability to set the track_functions GUC as a local GUC parameter of an
     * interesting function and have the right things happen.
     */
    if (!ignore_security &&
        (procedureStruct->prosecdef ||
         !heap_attisnull(procedureTuple, Anum_pg_proc_proconfig, NULL) ||
         FmgrHookIsNeeded(functionId)))
    {
        finfo->fn_addr = fmgr_security_definer;
        finfo->fn_stats = TRACK_FUNC_ALL;    /* ie, never track */
        finfo->fn_oid = functionId;
        ReleaseSysCache(procedureTuple);
        return;
    }

    switch (procedureStruct->prolang)
    {
        case INTERNALlanguageId:

            /*
             * For an ordinary builtin function, we should never get here
             * because the isbuiltin() search above will have succeeded.
             * However, if the user has done a CREATE FUNCTION to create an
             * alias for a builtin function, we can end up here.  In that case
             * we have to look up the function by name.  The name of the
             * internal function is stored in prosrc (it doesn't have to be
             * the same as the name of the alias!)
             */
            prosrcdatum = SysCacheGetAttr(PROCOID, procedureTuple,
                                          Anum_pg_proc_prosrc, &isnull);
            if (isnull)
                elog(ERROR, "null prosrc");
            prosrc = TextDatumGetCString(prosrcdatum);
            fbp = fmgr_lookupByName(prosrc);
            if (fbp == NULL)
                ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_FUNCTION),
                         errmsg("internal function \"%s\" is not in internal lookup table",
                                prosrc)));
            pfree(prosrc);
            /* Should we check that nargs, strict, retset match the table? */
            finfo->fn_addr = fbp->func;
            /* note this policy is also assumed in fast path above */
            finfo->fn_stats = TRACK_FUNC_ALL;    /* ie, never track */
            break;

        case ClanguageId:
            fmgr_info_C_lang(functionId, finfo, procedureTuple);
            finfo->fn_stats = TRACK_FUNC_PL;    /* ie, track if ALL */
            break;

        case SQLlanguageId:
            finfo->fn_addr = fmgr_sql;
            finfo->fn_stats = TRACK_FUNC_PL;    /* ie, track if ALL */
            break;

        default:
            fmgr_info_other_lang(functionId, finfo, procedureTuple);
            finfo->fn_stats = TRACK_FUNC_OFF;    /* ie, track if not OFF */
            break;
    }

    finfo->fn_oid = functionId;
    ReleaseSysCache(procedureTuple);
}

/*
 * Special fmgr_info processing for C-language functions.  Note that
 * finfo->fn_oid is not valid yet.
 */
static void
fmgr_info_C_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple)
{
    CFuncHashTabEntry *hashentry;
    PGFunction    user_fn;
    const Pg_finfo_record *inforec;
    bool        isnull;

    /*
     * See if we have the function address cached already
     */
    hashentry = lookup_C_func(procedureTuple);
    if (hashentry)
    {
        user_fn = hashentry->user_fn;
        inforec = hashentry->inforec;
    }
    else
    {
        Datum        prosrcattr,
                    probinattr;
        char       *prosrcstring,
                   *probinstring;
        void       *libraryhandle;

        /*
         * Get prosrc and probin strings (link symbol and library filename).
         * While in general these columns might be null, that's not allowed
         * for C-language functions.
         */
        prosrcattr = SysCacheGetAttr(PROCOID, procedureTuple,
                                     Anum_pg_proc_prosrc, &isnull);
        if (isnull)
            elog(ERROR, "null prosrc for C function %u", functionId);
        prosrcstring = TextDatumGetCString(prosrcattr);

        probinattr = SysCacheGetAttr(PROCOID, procedureTuple,
                                     Anum_pg_proc_probin, &isnull);
        if (isnull)
            elog(ERROR, "null probin for C function %u", functionId);
        probinstring = TextDatumGetCString(probinattr);

        /* Look up the function itself */
        user_fn = load_external_function(probinstring, prosrcstring, true,
                                         &libraryhandle);

        /* Get the function information record (real or default) */
        inforec = fetch_finfo_record(libraryhandle, prosrcstring);

        /* Cache the addresses for later calls */
        record_C_func(procedureTuple, user_fn, inforec);

        pfree(prosrcstring);
        pfree(probinstring);
    }

    switch (inforec->api_version)
    {
        case 1:
            /* New style: call directly */
            finfo->fn_addr = user_fn;
            break;
        default:
            /* Shouldn't get here if fetch_finfo_record did its job */
            elog(ERROR, "unrecognized function API version: %d",
                 inforec->api_version);
            break;
    }
}

/*
 * Special fmgr_info processing for other-language functions.  Note
 * that finfo->fn_oid is not valid yet.
 */
static void
fmgr_info_other_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple)
{
    Form_pg_proc procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);
    Oid            language = procedureStruct->prolang;
    HeapTuple    languageTuple;
    Form_pg_language languageStruct;
    FmgrInfo    plfinfo;

    languageTuple = SearchSysCache1(LANGOID, ObjectIdGetDatum(language));
    if (!HeapTupleIsValid(languageTuple))
        elog(ERROR, "cache lookup failed for language %u", language);
    languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);

    /*
     * Look up the language's call handler function, ignoring any attributes
     * that would normally cause insertion of fmgr_security_definer.  We need
     * to get back a bare pointer to the actual C-language function.
     */
    fmgr_info_cxt_security(languageStruct->lanplcallfoid, &plfinfo,
                           CurrentMemoryContext, true);
    finfo->fn_addr = plfinfo.fn_addr;

    ReleaseSysCache(languageTuple);
}

/*
 * Fetch and validate the information record for the given external function.
 * The function is specified by a handle for the containing library
 * (obtained from load_external_function) as well as the function name.
 *
 * If no info function exists for the given name an error is raised.
 *
 * This function is broken out of fmgr_info_C_lang so that fmgr_c_validator
 * can validate the information record for a function not yet entered into
 * pg_proc.
 */
const Pg_finfo_record *
fetch_finfo_record(void *filehandle, const char *funcname)
{
    char       *infofuncname;
    PGFInfoFunction infofunc;
    const Pg_finfo_record *inforec;

    infofuncname = psprintf("pg_finfo_%s", funcname);

    /* Try to look up the info function */
    infofunc = (PGFInfoFunction) lookup_external_function(filehandle,
                                                          infofuncname);
    if (infofunc == NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_FUNCTION),
                 errmsg("could not find function information for function \"%s\"",
                        funcname),
                 errhint("SQL-callable functions need an accompanying PG_FUNCTION_INFO_V1(funcname).")));
        return NULL;            /* silence compiler */
    }

    /* Found, so call it */
    inforec = (*infofunc) ();

    /* Validate result as best we can */
    if (inforec == NULL)
        elog(ERROR, "null result from info function \"%s\"", infofuncname);
    switch (inforec->api_version)
    {
        case 1:
            /* OK, no additional fields to validate */
            break;
        default:
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("unrecognized API version %d reported by info function \"%s\"",
                            inforec->api_version, infofuncname)));
            break;
    }

    pfree(infofuncname);
    return inforec;
}


/*-------------------------------------------------------------------------
 *        Routines for caching lookup information for external C functions.
 *
 * The routines in dfmgr.c are relatively slow, so we try to avoid running
 * them more than once per external function per session.  We use a hash table
 * with the function OID as the lookup key.
 *-------------------------------------------------------------------------
 */

/*
 * lookup_C_func: try to find a C function in the hash table
 *
 * If an entry exists and is up to date, return it; else return NULL
 */
static CFuncHashTabEntry *
lookup_C_func(HeapTuple procedureTuple)
{
    Oid            fn_oid = HeapTupleGetOid(procedureTuple);
    CFuncHashTabEntry *entry;

    if (CFuncHash == NULL)
        return NULL;            /* no table yet */
    entry = (CFuncHashTabEntry *)
        hash_search(CFuncHash,
                    &fn_oid,
                    HASH_FIND,
                    NULL);
    if (entry == NULL)
        return NULL;            /* no such entry */
    if (entry->fn_xmin == HeapTupleHeaderGetRawXmin(procedureTuple->t_data) &&
        ItemPointerEquals(&entry->fn_tid, &procedureTuple->t_self))
        return entry;            /* OK */
    return NULL;                /* entry is out of date */
}

/*
 * record_C_func: enter (or update) info about a C function in the hash table
 */
static void
record_C_func(HeapTuple procedureTuple,
              PGFunction user_fn, const Pg_finfo_record *inforec)
{
    Oid            fn_oid = HeapTupleGetOid(procedureTuple);
    CFuncHashTabEntry *entry;
    bool        found;

    /* Create the hash table if it doesn't exist yet */
    if (CFuncHash == NULL)
    {
        HASHCTL        hash_ctl;

        MemSet(&hash_ctl, 0, sizeof(hash_ctl));
        hash_ctl.keysize = sizeof(Oid);
        hash_ctl.entrysize = sizeof(CFuncHashTabEntry);
        CFuncHash = hash_create("CFuncHash",
                                100,
                                &hash_ctl,
                                HASH_ELEM | HASH_BLOBS);
    }

    entry = (CFuncHashTabEntry *)
        hash_search(CFuncHash,
                    &fn_oid,
                    HASH_ENTER,
                    &found);
    /* OID is already filled in */
    entry->fn_xmin = HeapTupleHeaderGetRawXmin(procedureTuple->t_data);
    entry->fn_tid = procedureTuple->t_self;
    entry->user_fn = user_fn;
    entry->inforec = inforec;
}

/*
 * clear_external_function_hash: remove entries for a library being closed
 *
 * Presently we just zap the entire hash table, but later it might be worth
 * the effort to remove only the entries associated with the given handle.
 */
void
clear_external_function_hash(void *filehandle)
{
    if (CFuncHash)
        hash_destroy(CFuncHash);
    CFuncHash = NULL;
}


/*
 * Copy an FmgrInfo struct
 *
 * This is inherently somewhat bogus since we can't reliably duplicate
 * language-dependent subsidiary info.  We cheat by zeroing fn_extra,
 * instead, meaning that subsidiary info will have to be recomputed.
 */
void
fmgr_info_copy(FmgrInfo *dstinfo, FmgrInfo *srcinfo,
               MemoryContext destcxt)
{
    memcpy(dstinfo, srcinfo, sizeof(FmgrInfo));
    dstinfo->fn_mcxt = destcxt;
    dstinfo->fn_extra = NULL;
}


/*
 * Specialized lookup routine for fmgr_internal_validator: given the alleged
 * name of an internal function, return the OID of the function.
 * If the name is not recognized, return InvalidOid.
 */
Oid
fmgr_internal_function(const char *proname)
{
    const FmgrBuiltin *fbp = fmgr_lookupByName(proname);

    if (fbp == NULL)
        return InvalidOid;
    return fbp->foid;
}


/*
 * Support for security-definer and proconfig-using functions.  We support
 * both of these features using the same call handler, because they are
 * often used together and it would be inefficient (as well as notationally
 * messy) to have two levels of call handler involved.
 */
struct fmgr_security_definer_cache
{
    FmgrInfo    flinfo;            /* lookup info for target function */
    Oid            userid;            /* userid to set, or InvalidOid */
    ArrayType  *proconfig;        /* GUC values to set, or NULL */
    Datum        arg;            /* passthrough argument for plugin modules */
};

/*
 * Function handler for security-definer/proconfig/plugin-hooked functions.
 * We extract the OID of the actual function and do a fmgr lookup again.
 * Then we fetch the pg_proc row and copy the owner ID and proconfig fields.
 * (All this info is cached for the duration of the current query.)
 * To execute a call, we temporarily replace the flinfo with the cached
 * and looked-up one, while keeping the outer fcinfo (which contains all
 * the actual arguments, etc.) intact.  This is not re-entrant, but then
 * the fcinfo itself can't be used reentrantly anyway.
 */
static Datum
fmgr_security_definer(PG_FUNCTION_ARGS)
{// #lizard forgives
    Datum        result;
    struct fmgr_security_definer_cache *volatile fcache;
    FmgrInfo   *save_flinfo;
    Oid            save_userid;
    int            save_sec_context;
    volatile int save_nestlevel;
    PgStat_FunctionCallUsage fcusage;

    if (!fcinfo->flinfo->fn_extra)
    {
        HeapTuple    tuple;
        Form_pg_proc procedureStruct;
        Datum        datum;
        bool        isnull;
        MemoryContext oldcxt;

        fcache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
                                        sizeof(*fcache));

        fmgr_info_cxt_security(fcinfo->flinfo->fn_oid, &fcache->flinfo,
                               fcinfo->flinfo->fn_mcxt, true);
        fcache->flinfo.fn_expr = fcinfo->flinfo->fn_expr;

        tuple = SearchSysCache1(PROCOID,
                                ObjectIdGetDatum(fcinfo->flinfo->fn_oid));
        if (!HeapTupleIsValid(tuple))
            elog(ERROR, "cache lookup failed for function %u",
                 fcinfo->flinfo->fn_oid);
        procedureStruct = (Form_pg_proc) GETSTRUCT(tuple);

        if (procedureStruct->prosecdef)
            fcache->userid = procedureStruct->proowner;

        datum = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proconfig,
                                &isnull);
        if (!isnull)
        {
            oldcxt = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
            fcache->proconfig = DatumGetArrayTypePCopy(datum);
            MemoryContextSwitchTo(oldcxt);
        }

        ReleaseSysCache(tuple);

        fcinfo->flinfo->fn_extra = fcache;
    }
    else
        fcache = fcinfo->flinfo->fn_extra;

    /* GetUserIdAndSecContext is cheap enough that no harm in a wasted call */
    GetUserIdAndSecContext(&save_userid, &save_sec_context);
    if (fcache->proconfig)        /* Need a new GUC nesting level */
        save_nestlevel = NewGUCNestLevel();
    else
        save_nestlevel = 0;        /* keep compiler quiet */

#ifdef __OPENTENBASE__
    /*
      * function with SECURITY DEFINER executed on coordinator,we need not only
      * set current user on coordinator, but also set remote current user.
         */
    if (OidIsValid(fcache->userid))
    {
        if (IS_PGXC_LOCAL_COORDINATOR)
        {
            #define LENGTH 512
            char cmd[LENGTH];
            char *username = GetUserNameFromId(fcache->userid, true);

            if (username)
            {
                A_Const *n = makeNode(A_Const);
                
                snprintf(cmd, LENGTH, "set SESSION AUTHORIZATION %s", username);

                n->val.type = T_String;
                n->val.val.str = username;
                n->location = -1;

                SetPGVariable("session_authorization", list_make1(n), false);

                //PoolManagerSetCommand(NULL, POOL_SET_COMMAND_ALL, POOL_CMD_GLOBAL_SET, cmd);
            }
        }
        
        SetUserIdAndSecContext(fcache->userid,
                               save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
    }
#else
    if (OidIsValid(fcache->userid))
        SetUserIdAndSecContext(fcache->userid,
                               save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
#endif

    if (fcache->proconfig)
    {
        ProcessGUCArray(fcache->proconfig,
                        (superuser() ? PGC_SUSET : PGC_USERSET),
                        PGC_S_SESSION,
                        GUC_ACTION_SAVE);
    }

    /* function manager hook */
    if (fmgr_hook)
        (*fmgr_hook) (FHET_START, &fcache->flinfo, &fcache->arg);

    /*
     * We don't need to restore GUC or userid settings on error, because the
     * ensuing xact or subxact abort will do that.  The PG_TRY block is only
     * needed to clean up the flinfo link.
     */
    save_flinfo = fcinfo->flinfo;

    PG_TRY();
    {
        fcinfo->flinfo = &fcache->flinfo;

        /* See notes in fmgr_info_cxt_security */
        pgstat_init_function_usage(fcinfo, &fcusage);

        result = FunctionCallInvoke(fcinfo);

        /*
         * We could be calling either a regular or a set-returning function,
         * so we have to test to see what finalize flag to use.
         */
        pgstat_end_function_usage(&fcusage,
                                  (fcinfo->resultinfo == NULL ||
                                   !IsA(fcinfo->resultinfo, ReturnSetInfo) ||
                                   ((ReturnSetInfo *) fcinfo->resultinfo)->isDone != ExprMultipleResult));
    }
    PG_CATCH();
    {
        fcinfo->flinfo = save_flinfo;
        if (fmgr_hook)
            (*fmgr_hook) (FHET_ABORT, &fcache->flinfo, &fcache->arg);

#ifdef __OPENTENBASE__
        /*
          * set current user back both local and remote
             */
        if (OidIsValid(fcache->userid))
        {
            SetUserIdAndSecContext(save_userid, save_sec_context);
            
            if (IS_PGXC_LOCAL_COORDINATOR)
            {
                #define LENGTH 512
                char cmd[LENGTH];
                char *username = GetUserNameFromId(save_userid, true);

                if (username)
                {
                    A_Const *n = makeNode(A_Const);
                
                    snprintf(cmd, LENGTH, "set SESSION AUTHORIZATION %s", username);

                    n->val.type = T_String;
                    n->val.val.str = username;
                    n->location = -1;

                    SetPGVariable("session_authorization", list_make1(n), false);

                    //PoolManagerSetCommand(NULL, POOL_SET_COMMAND_ALL, POOL_CMD_GLOBAL_SET, cmd);
                }
            }
        }
#endif

        PG_RE_THROW();
    }
    PG_END_TRY();

    fcinfo->flinfo = save_flinfo;

    if (fcache->proconfig)
        AtEOXact_GUC(true, save_nestlevel);
#ifdef __OPENTENBASE__
    /*
      * set current user back both local and remote
         */
    if (OidIsValid(fcache->userid))
    {
        SetUserIdAndSecContext(save_userid, save_sec_context);
        
        if (IS_PGXC_LOCAL_COORDINATOR)
        {
            #define LENGTH 512
            char cmd[LENGTH];
            char *username = GetUserNameFromId(save_userid, true);

            if (username)
            {
                A_Const *n = makeNode(A_Const);
            
                snprintf(cmd, LENGTH, "set SESSION AUTHORIZATION %s", username);

                n->val.type = T_String;
                n->val.val.str = username;
                n->location = -1;

                SetPGVariable("session_authorization", list_make1(n), false);

                //PoolManagerSetCommand(NULL, POOL_SET_COMMAND_ALL, POOL_CMD_GLOBAL_SET, cmd);
            }
        }
    }
#else
    if (OidIsValid(fcache->userid))
        SetUserIdAndSecContext(save_userid, save_sec_context);
#endif
    if (fmgr_hook)
        (*fmgr_hook) (FHET_END, &fcache->flinfo, &fcache->arg);

    return result;
}


/*-------------------------------------------------------------------------
 *        Support routines for callers of fmgr-compatible functions
 *-------------------------------------------------------------------------
 */

/*
 * These are for invocation of a specifically named function with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.  Also, the function cannot be one that needs to
 * look at FmgrInfo, since there won't be any.
 */
Datum
DirectFunctionCall1Coll(PGFunction func, Oid collation, Datum arg1)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, NULL, 1, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.argnull[0] = false;

    result = (*func) (&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %p returned NULL", (void *) func);

    return result;
}

Datum
DirectFunctionCall2Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, NULL, 2, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;

    result = (*func) (&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %p returned NULL", (void *) func);

    return result;
}

Datum
DirectFunctionCall3Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
                        Datum arg3)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, NULL, 3, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;

    result = (*func) (&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %p returned NULL", (void *) func);

    return result;
}

Datum
DirectFunctionCall4Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
                        Datum arg3, Datum arg4)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, NULL, 4, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;

    result = (*func) (&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %p returned NULL", (void *) func);

    return result;
}

Datum
DirectFunctionCall5Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
                        Datum arg3, Datum arg4, Datum arg5)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, NULL, 5, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;

    result = (*func) (&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %p returned NULL", (void *) func);

    return result;
}

Datum
DirectFunctionCall6Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
                        Datum arg3, Datum arg4, Datum arg5,
                        Datum arg6)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, NULL, 6, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.arg[5] = arg6;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;
    fcinfo.argnull[5] = false;

    result = (*func) (&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %p returned NULL", (void *) func);

    return result;
}

Datum
DirectFunctionCall7Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
                        Datum arg3, Datum arg4, Datum arg5,
                        Datum arg6, Datum arg7)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, NULL, 7, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.arg[5] = arg6;
    fcinfo.arg[6] = arg7;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;
    fcinfo.argnull[5] = false;
    fcinfo.argnull[6] = false;

    result = (*func) (&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %p returned NULL", (void *) func);

    return result;
}

Datum
DirectFunctionCall8Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
                        Datum arg3, Datum arg4, Datum arg5,
                        Datum arg6, Datum arg7, Datum arg8)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, NULL, 8, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.arg[5] = arg6;
    fcinfo.arg[6] = arg7;
    fcinfo.arg[7] = arg8;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;
    fcinfo.argnull[5] = false;
    fcinfo.argnull[6] = false;
    fcinfo.argnull[7] = false;

    result = (*func) (&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %p returned NULL", (void *) func);

    return result;
}

Datum
DirectFunctionCall9Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
                        Datum arg3, Datum arg4, Datum arg5,
                        Datum arg6, Datum arg7, Datum arg8,
                        Datum arg9)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, NULL, 9, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.arg[5] = arg6;
    fcinfo.arg[6] = arg7;
    fcinfo.arg[7] = arg8;
    fcinfo.arg[8] = arg9;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;
    fcinfo.argnull[5] = false;
    fcinfo.argnull[6] = false;
    fcinfo.argnull[7] = false;
    fcinfo.argnull[8] = false;

    result = (*func) (&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %p returned NULL", (void *) func);

    return result;
}

/*
 * These functions work like the DirectFunctionCall functions except that
 * they use the flinfo parameter to initialise the fcinfo for the call.
 * It's recommended that the callee only use the fn_extra and fn_mcxt
 * fields, as other fields will typically describe the calling function
 * not the callee.  Conversely, the calling function should not have
 * used fn_extra, unless its use is known to be compatible with the callee's.
 */

Datum
CallerFInfoFunctionCall1(PGFunction func, FmgrInfo *flinfo, Oid collation, Datum arg1)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, flinfo, 1, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.argnull[0] = false;

    result = (*func) (&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %p returned NULL", (void *) func);

    return result;
}

Datum
CallerFInfoFunctionCall2(PGFunction func, FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, flinfo, 2, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;

    result = (*func) (&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %p returned NULL", (void *) func);

    return result;
}

/*
 * These are for invocation of a previously-looked-up function with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.
 */
Datum
FunctionCall1Coll(FmgrInfo *flinfo, Oid collation, Datum arg1)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, flinfo, 1, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.argnull[0] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

    return result;
}

Datum
FunctionCall2Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, flinfo, 2, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

    return result;
}

Datum
FunctionCall3Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
                  Datum arg3)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, flinfo, 3, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

    return result;
}

Datum
FunctionCall4Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
                  Datum arg3, Datum arg4)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, flinfo, 4, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

    return result;
}

Datum
FunctionCall5Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
                  Datum arg3, Datum arg4, Datum arg5)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, flinfo, 5, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

    return result;
}

Datum
FunctionCall6Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
                  Datum arg3, Datum arg4, Datum arg5,
                  Datum arg6)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, flinfo, 6, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.arg[5] = arg6;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;
    fcinfo.argnull[5] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

    return result;
}

Datum
FunctionCall7Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
                  Datum arg3, Datum arg4, Datum arg5,
                  Datum arg6, Datum arg7)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, flinfo, 7, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.arg[5] = arg6;
    fcinfo.arg[6] = arg7;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;
    fcinfo.argnull[5] = false;
    fcinfo.argnull[6] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

    return result;
}

Datum
FunctionCall8Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
                  Datum arg3, Datum arg4, Datum arg5,
                  Datum arg6, Datum arg7, Datum arg8)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, flinfo, 8, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.arg[5] = arg6;
    fcinfo.arg[6] = arg7;
    fcinfo.arg[7] = arg8;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;
    fcinfo.argnull[5] = false;
    fcinfo.argnull[6] = false;
    fcinfo.argnull[7] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

    return result;
}

Datum
FunctionCall9Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
                  Datum arg3, Datum arg4, Datum arg5,
                  Datum arg6, Datum arg7, Datum arg8,
                  Datum arg9)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    InitFunctionCallInfoData(fcinfo, flinfo, 9, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.arg[5] = arg6;
    fcinfo.arg[6] = arg7;
    fcinfo.arg[7] = arg8;
    fcinfo.arg[8] = arg9;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;
    fcinfo.argnull[5] = false;
    fcinfo.argnull[6] = false;
    fcinfo.argnull[7] = false;
    fcinfo.argnull[8] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

    return result;
}


/*
 * These are for invocation of a function identified by OID with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.  These are essentially fmgr_info() followed
 * by FunctionCallN().  If the same function is to be invoked repeatedly,
 * do the fmgr_info() once and then use FunctionCallN().
 */
Datum
OidFunctionCall0Coll(Oid functionId, Oid collation)
{
    FmgrInfo    flinfo;
    FunctionCallInfoData fcinfo;
    Datum        result;

    fmgr_info(functionId, &flinfo);

    InitFunctionCallInfoData(fcinfo, &flinfo, 0, collation, NULL, NULL);

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

    return result;
}

Datum
OidFunctionCall1Coll(Oid functionId, Oid collation, Datum arg1)
{
    FmgrInfo    flinfo;
    FunctionCallInfoData fcinfo;
    Datum        result;

    fmgr_info(functionId, &flinfo);

    InitFunctionCallInfoData(fcinfo, &flinfo, 1, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.argnull[0] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

    return result;
}

Datum
OidFunctionCall2Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2)
{
    FmgrInfo    flinfo;
    FunctionCallInfoData fcinfo;
    Datum        result;

    fmgr_info(functionId, &flinfo);

    InitFunctionCallInfoData(fcinfo, &flinfo, 2, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

    return result;
}

Datum
OidFunctionCall3Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
                     Datum arg3)
{
    FmgrInfo    flinfo;
    FunctionCallInfoData fcinfo;
    Datum        result;

    fmgr_info(functionId, &flinfo);

    InitFunctionCallInfoData(fcinfo, &flinfo, 3, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

    return result;
}

Datum
OidFunctionCall4Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
                     Datum arg3, Datum arg4)
{
    FmgrInfo    flinfo;
    FunctionCallInfoData fcinfo;
    Datum        result;

    fmgr_info(functionId, &flinfo);

    InitFunctionCallInfoData(fcinfo, &flinfo, 4, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

    return result;
}

Datum
OidFunctionCall5Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
                     Datum arg3, Datum arg4, Datum arg5)
{
    FmgrInfo    flinfo;
    FunctionCallInfoData fcinfo;
    Datum        result;

    fmgr_info(functionId, &flinfo);

    InitFunctionCallInfoData(fcinfo, &flinfo, 5, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

    return result;
}

Datum
OidFunctionCall6Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
                     Datum arg3, Datum arg4, Datum arg5,
                     Datum arg6)
{
    FmgrInfo    flinfo;
    FunctionCallInfoData fcinfo;
    Datum        result;

    fmgr_info(functionId, &flinfo);

    InitFunctionCallInfoData(fcinfo, &flinfo, 6, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.arg[5] = arg6;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;
    fcinfo.argnull[5] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

    return result;
}

Datum
OidFunctionCall7Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
                     Datum arg3, Datum arg4, Datum arg5,
                     Datum arg6, Datum arg7)
{
    FmgrInfo    flinfo;
    FunctionCallInfoData fcinfo;
    Datum        result;

    fmgr_info(functionId, &flinfo);

    InitFunctionCallInfoData(fcinfo, &flinfo, 7, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.arg[5] = arg6;
    fcinfo.arg[6] = arg7;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;
    fcinfo.argnull[5] = false;
    fcinfo.argnull[6] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

    return result;
}

Datum
OidFunctionCall8Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
                     Datum arg3, Datum arg4, Datum arg5,
                     Datum arg6, Datum arg7, Datum arg8)
{
    FmgrInfo    flinfo;
    FunctionCallInfoData fcinfo;
    Datum        result;

    fmgr_info(functionId, &flinfo);

    InitFunctionCallInfoData(fcinfo, &flinfo, 8, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.arg[5] = arg6;
    fcinfo.arg[6] = arg7;
    fcinfo.arg[7] = arg8;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;
    fcinfo.argnull[5] = false;
    fcinfo.argnull[6] = false;
    fcinfo.argnull[7] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

    return result;
}

Datum
OidFunctionCall9Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
                     Datum arg3, Datum arg4, Datum arg5,
                     Datum arg6, Datum arg7, Datum arg8,
                     Datum arg9)
{
    FmgrInfo    flinfo;
    FunctionCallInfoData fcinfo;
    Datum        result;

    fmgr_info(functionId, &flinfo);

    InitFunctionCallInfoData(fcinfo, &flinfo, 9, collation, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.arg[2] = arg3;
    fcinfo.arg[3] = arg4;
    fcinfo.arg[4] = arg5;
    fcinfo.arg[5] = arg6;
    fcinfo.arg[6] = arg7;
    fcinfo.arg[7] = arg8;
    fcinfo.arg[8] = arg9;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;
    fcinfo.argnull[3] = false;
    fcinfo.argnull[4] = false;
    fcinfo.argnull[5] = false;
    fcinfo.argnull[6] = false;
    fcinfo.argnull[7] = false;
    fcinfo.argnull[8] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

    return result;
}


/*
 * Special cases for convenient invocation of datatype I/O functions.
 */

/*
 * Call a previously-looked-up datatype input function.
 *
 * "str" may be NULL to indicate we are reading a NULL.  In this case
 * the caller should assume the result is NULL, but we'll call the input
 * function anyway if it's not strict.  So this is almost but not quite
 * the same as FunctionCall3.
 */
Datum
InputFunctionCall(FmgrInfo *flinfo, char *str, Oid typioparam, int32 typmod)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    if (str == NULL && flinfo->fn_strict)
        return (Datum) 0;        /* just return null result */

    InitFunctionCallInfoData(fcinfo, flinfo, 3, InvalidOid, NULL, NULL);

    fcinfo.arg[0] = CStringGetDatum(str);
    fcinfo.arg[1] = ObjectIdGetDatum(typioparam);
    fcinfo.arg[2] = Int32GetDatum(typmod);
    fcinfo.argnull[0] = (str == NULL);
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Should get null result if and only if str is NULL */
    if (str == NULL)
    {
        if (!fcinfo.isnull)
            elog(ERROR, "input function %u returned non-NULL",
                 fcinfo.flinfo->fn_oid);
    }
    else
    {
        if (fcinfo.isnull)
            elog(ERROR, "input function %u returned NULL",
                 fcinfo.flinfo->fn_oid);
    }

    return result;
}

/*
 * Call a previously-looked-up datatype output function.
 *
 * Do not call this on NULL datums.
 *
 * This is currently little more than window dressing for FunctionCall1.
 */
char *
OutputFunctionCall(FmgrInfo *flinfo, Datum val)
{
    return DatumGetCString(FunctionCall1(flinfo, val));
}

/*
 * Call a previously-looked-up datatype binary-input function.
 *
 * "buf" may be NULL to indicate we are reading a NULL.  In this case
 * the caller should assume the result is NULL, but we'll call the receive
 * function anyway if it's not strict.  So this is almost but not quite
 * the same as FunctionCall3.
 */
Datum
ReceiveFunctionCall(FmgrInfo *flinfo, StringInfo buf,
                    Oid typioparam, int32 typmod)
{
    FunctionCallInfoData fcinfo;
    Datum        result;

    if (buf == NULL && flinfo->fn_strict)
        return (Datum) 0;        /* just return null result */

    InitFunctionCallInfoData(fcinfo, flinfo, 3, InvalidOid, NULL, NULL);

    fcinfo.arg[0] = PointerGetDatum(buf);
    fcinfo.arg[1] = ObjectIdGetDatum(typioparam);
    fcinfo.arg[2] = Int32GetDatum(typmod);
    fcinfo.argnull[0] = (buf == NULL);
    fcinfo.argnull[1] = false;
    fcinfo.argnull[2] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Should get null result if and only if buf is NULL */
    if (buf == NULL)
    {
        if (!fcinfo.isnull)
            elog(ERROR, "receive function %u returned non-NULL",
                 fcinfo.flinfo->fn_oid);
    }
    else
    {
        if (fcinfo.isnull)
            elog(ERROR, "receive function %u returned NULL",
                 fcinfo.flinfo->fn_oid);
    }

    return result;
}

/*
 * Call a previously-looked-up datatype binary-output function.
 *
 * Do not call this on NULL datums.
 *
 * This is little more than window dressing for FunctionCall1, but it does
 * guarantee a non-toasted result, which strictly speaking the underlying
 * function doesn't.
 */
bytea *
SendFunctionCall(FmgrInfo *flinfo, Datum val)
{
    return DatumGetByteaP(FunctionCall1(flinfo, val));
}

/*
 * As above, for I/O functions identified by OID.  These are only to be used
 * in seldom-executed code paths.  They are not only slow but leak memory.
 */
Datum
OidInputFunctionCall(Oid functionId, char *str, Oid typioparam, int32 typmod)
{
    FmgrInfo    flinfo;

    fmgr_info(functionId, &flinfo);
    return InputFunctionCall(&flinfo, str, typioparam, typmod);
}

char *
OidOutputFunctionCall(Oid functionId, Datum val)
{
    FmgrInfo    flinfo;

    fmgr_info(functionId, &flinfo);
    return OutputFunctionCall(&flinfo, val);
}

Datum
OidReceiveFunctionCall(Oid functionId, StringInfo buf,
                       Oid typioparam, int32 typmod)
{
    FmgrInfo    flinfo;

    fmgr_info(functionId, &flinfo);
    return ReceiveFunctionCall(&flinfo, buf, typioparam, typmod);
}

bytea *
OidSendFunctionCall(Oid functionId, Datum val)
{
    FmgrInfo    flinfo;

    fmgr_info(functionId, &flinfo);
    return SendFunctionCall(&flinfo, val);
}


/*-------------------------------------------------------------------------
 *        Support routines for standard maybe-pass-by-reference datatypes
 *
 * int8, float4, and float8 can be passed by value if Datum is wide enough.
 * (For backwards-compatibility reasons, we allow pass-by-ref to be chosen
 * at compile time even if pass-by-val is possible.)
 *
 * Note: there is only one switch controlling the pass-by-value option for
 * both int8 and float8; this is to avoid making things unduly complicated
 * for the timestamp types, which might have either representation.
 *-------------------------------------------------------------------------
 */

#ifndef USE_FLOAT8_BYVAL        /* controls int8 too */

Datum
Int64GetDatum(int64 X)
{
    int64       *retval = (int64 *) palloc(sizeof(int64));

    *retval = X;
    return PointerGetDatum(retval);
}
#endif                            /* USE_FLOAT8_BYVAL */

#ifndef USE_FLOAT4_BYVAL

Datum
Float4GetDatum(float4 X)
{
    float4       *retval = (float4 *) palloc(sizeof(float4));

    *retval = X;
    return PointerGetDatum(retval);
}
#endif

#ifndef USE_FLOAT8_BYVAL

Datum
Float8GetDatum(float8 X)
{
    float8       *retval = (float8 *) palloc(sizeof(float8));

    *retval = X;
    return PointerGetDatum(retval);
}
#endif


/*-------------------------------------------------------------------------
 *        Support routines for toastable datatypes
 *-------------------------------------------------------------------------
 */

struct varlena *
pg_detoast_datum(struct varlena *datum)
{
    if (VARATT_IS_EXTENDED(datum))
        return heap_tuple_untoast_attr(datum);
    else
        return datum;
}

struct varlena *
pg_detoast_datum_copy(struct varlena *datum)
{
    if (VARATT_IS_EXTENDED(datum))
        return heap_tuple_untoast_attr(datum);
    else
    {
        /* Make a modifiable copy of the varlena object */
        Size        len = VARSIZE(datum);
        struct varlena *result = (struct varlena *) palloc(len);

        memcpy(result, datum, len);
        return result;
    }
}

struct varlena *
pg_detoast_datum_slice(struct varlena *datum, int32 first, int32 count)
{
    /* Only get the specified portion from the toast rel */
    return heap_tuple_untoast_attr_slice(datum, first, count);
}

struct varlena *
pg_detoast_datum_packed(struct varlena *datum)
{
    if (VARATT_IS_COMPRESSED(datum) || VARATT_IS_EXTERNAL(datum))
        return heap_tuple_untoast_attr(datum);
    else
        return datum;
}

/*-------------------------------------------------------------------------
 *        Support routines for extracting info from fn_expr parse tree
 *
 * These are needed by polymorphic functions, which accept multiple possible
 * input types and need help from the parser to know what they've got.
 * Also, some functions might be interested in whether a parameter is constant.
 * Functions taking VARIADIC ANY also need to know about the VARIADIC keyword.
 *-------------------------------------------------------------------------
 */

/*
 * Get the actual type OID of the function return type
 *
 * Returns InvalidOid if information is not available
 */
Oid
get_fn_expr_rettype(FmgrInfo *flinfo)
{
    Node       *expr;

    /*
     * can't return anything useful if we have no FmgrInfo or if its fn_expr
     * node has not been initialized
     */
    if (!flinfo || !flinfo->fn_expr)
        return InvalidOid;

    expr = flinfo->fn_expr;

    return exprType(expr);
}

/*
 * Get the actual type OID of a specific function argument (counting from 0)
 *
 * Returns InvalidOid if information is not available
 */
Oid
get_fn_expr_argtype(FmgrInfo *flinfo, int argnum)
{
    /*
     * can't return anything useful if we have no FmgrInfo or if its fn_expr
     * node has not been initialized
     */
    if (!flinfo || !flinfo->fn_expr)
        return InvalidOid;

    return get_call_expr_argtype(flinfo->fn_expr, argnum);
}

/*
 * Get the actual type OID of a specific function argument (counting from 0),
 * but working from the calling expression tree instead of FmgrInfo
 *
 * Returns InvalidOid if information is not available
 */
Oid
get_call_expr_argtype(Node *expr, int argnum)
{// #lizard forgives
    List       *args;
    Oid            argtype;

    if (expr == NULL)
        return InvalidOid;

    if (IsA(expr, FuncExpr))
        args = ((FuncExpr *) expr)->args;
    else if (IsA(expr, OpExpr))
        args = ((OpExpr *) expr)->args;
    else if (IsA(expr, DistinctExpr))
        args = ((DistinctExpr *) expr)->args;
    else if (IsA(expr, ScalarArrayOpExpr))
        args = ((ScalarArrayOpExpr *) expr)->args;
    else if (IsA(expr, ArrayCoerceExpr))
        args = list_make1(((ArrayCoerceExpr *) expr)->arg);
    else if (IsA(expr, NullIfExpr))
        args = ((NullIfExpr *) expr)->args;
    else if (IsA(expr, WindowFunc))
        args = ((WindowFunc *) expr)->args;
    else
        return InvalidOid;

    if (argnum < 0 || argnum >= list_length(args))
        return InvalidOid;

    argtype = exprType((Node *) list_nth(args, argnum));

    /*
     * special hack for ScalarArrayOpExpr and ArrayCoerceExpr: what the
     * underlying function will actually get passed is the element type of the
     * array.
     */
    if (IsA(expr, ScalarArrayOpExpr) &&
        argnum == 1)
        argtype = get_base_element_type(argtype);
    else if (IsA(expr, ArrayCoerceExpr) &&
             argnum == 0)
        argtype = get_base_element_type(argtype);

    return argtype;
}

/*
 * Find out whether a specific function argument is constant for the
 * duration of a query
 *
 * Returns false if information is not available
 */
bool
get_fn_expr_arg_stable(FmgrInfo *flinfo, int argnum)
{
    /*
     * can't return anything useful if we have no FmgrInfo or if its fn_expr
     * node has not been initialized
     */
    if (!flinfo || !flinfo->fn_expr)
        return false;

    return get_call_expr_arg_stable(flinfo->fn_expr, argnum);
}

/*
 * Find out whether a specific function argument is constant for the
 * duration of a query, but working from the calling expression tree
 *
 * Returns false if information is not available
 */
bool
get_call_expr_arg_stable(Node *expr, int argnum)
{// #lizard forgives
    List       *args;
    Node       *arg;

    if (expr == NULL)
        return false;

    if (IsA(expr, FuncExpr))
        args = ((FuncExpr *) expr)->args;
    else if (IsA(expr, OpExpr))
        args = ((OpExpr *) expr)->args;
    else if (IsA(expr, DistinctExpr))
        args = ((DistinctExpr *) expr)->args;
    else if (IsA(expr, ScalarArrayOpExpr))
        args = ((ScalarArrayOpExpr *) expr)->args;
    else if (IsA(expr, ArrayCoerceExpr))
        args = list_make1(((ArrayCoerceExpr *) expr)->arg);
    else if (IsA(expr, NullIfExpr))
        args = ((NullIfExpr *) expr)->args;
    else if (IsA(expr, WindowFunc))
        args = ((WindowFunc *) expr)->args;
    else
        return false;

    if (argnum < 0 || argnum >= list_length(args))
        return false;

    arg = (Node *) list_nth(args, argnum);

    /*
     * Either a true Const or an external Param will have a value that doesn't
     * change during the execution of the query.  In future we might want to
     * consider other cases too, e.g. now().
     */
    if (IsA(arg, Const))
        return true;
    if (IsA(arg, Param) &&
        ((Param *) arg)->paramkind == PARAM_EXTERN)
        return true;

    return false;
}

/*
 * Get the VARIADIC flag from the function invocation
 *
 * Returns false (the default assumption) if information is not available
 *
 * Note this is generally only of interest to VARIADIC ANY functions
 */
bool
get_fn_expr_variadic(FmgrInfo *flinfo)
{
    Node       *expr;

    /*
     * can't return anything useful if we have no FmgrInfo or if its fn_expr
     * node has not been initialized
     */
    if (!flinfo || !flinfo->fn_expr)
        return false;

    expr = flinfo->fn_expr;

    if (IsA(expr, FuncExpr))
        return ((FuncExpr *) expr)->funcvariadic;
    else
        return false;
}

/*-------------------------------------------------------------------------
 *        Support routines for procedural language implementations
 *-------------------------------------------------------------------------
 */

/*
 * Verify that a validator is actually associated with the language of a
 * particular function and that the user has access to both the language and
 * the function.  All validators should call this before doing anything
 * substantial.  Doing so ensures a user cannot achieve anything with explicit
 * calls to validators that he could not achieve with CREATE FUNCTION or by
 * simply calling an existing function.
 *
 * When this function returns false, callers should skip all validation work
 * and call PG_RETURN_VOID().  This never happens at present; it is reserved
 * for future expansion.
 *
 * In particular, checking that the validator corresponds to the function's
 * language allows untrusted language validators to assume they process only
 * superuser-chosen source code.  (Untrusted language call handlers, by
 * definition, do assume that.)  A user lacking the USAGE language privilege
 * would be unable to reach the validator through CREATE FUNCTION, so we check
 * that to block explicit calls as well.  Checking the EXECUTE privilege on
 * the function is often superfluous, because most users can clone the
 * function to get an executable copy.  It is meaningful against users with no
 * database TEMP right and no permanent schema CREATE right, thereby unable to
 * create any function.  Also, if the function tracks persistent state by
 * function OID or name, validating the original function might permit more
 * mischief than creating and validating a clone thereof.
 */
bool
CheckFunctionValidatorAccess(Oid validatorOid, Oid functionOid)
{
    HeapTuple    procTup;
    HeapTuple    langTup;
    Form_pg_proc procStruct;
    Form_pg_language langStruct;
    AclResult    aclresult;

    /*
     * Get the function's pg_proc entry.  Throw a user-facing error for bad
     * OID, because validators can be called with user-specified OIDs.
     */
    procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(functionOid));
    if (!HeapTupleIsValid(procTup))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_FUNCTION),
                 errmsg("function with OID %u does not exist", functionOid)));
    procStruct = (Form_pg_proc) GETSTRUCT(procTup);

    /*
     * Fetch pg_language entry to know if this is the correct validation
     * function for that pg_proc entry.
     */
    langTup = SearchSysCache1(LANGOID, ObjectIdGetDatum(procStruct->prolang));
    if (!HeapTupleIsValid(langTup))
        elog(ERROR, "cache lookup failed for language %u", procStruct->prolang);
    langStruct = (Form_pg_language) GETSTRUCT(langTup);

    if (langStruct->lanvalidator != validatorOid)
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("language validation function %u called for language %u instead of %u",
                        validatorOid, procStruct->prolang,
                        langStruct->lanvalidator)));

    /* first validate that we have permissions to use the language */
    aclresult = pg_language_aclcheck(procStruct->prolang, GetUserId(),
                                     ACL_USAGE);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error(aclresult, ACL_KIND_LANGUAGE,
                       NameStr(langStruct->lanname));

    /*
     * Check whether we are allowed to execute the function itself. If we can
     * execute it, there should be no possible side-effect of
     * compiling/validation that execution can't have.
     */
    aclresult = pg_proc_aclcheck(functionOid, GetUserId(), ACL_EXECUTE);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error(aclresult, ACL_KIND_PROC, NameStr(procStruct->proname));

    ReleaseSysCache(procTup);
    ReleaseSysCache(langTup);

    return true;
}
