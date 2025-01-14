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
 *    exec.c
 *
 *    execution functions
 *
 *    Copyright (c) 2010-2017, PostgreSQL Global Development Group
 *    src/bin/pg_upgrade/exec.c
 */

#include "postgres_fe.h"

#include <fcntl.h>

#include "pg_upgrade.h"

static void check_data_dir(ClusterInfo *cluster);
static void check_bin_dir(ClusterInfo *cluster);
static void get_bin_version(ClusterInfo *cluster);
static void validate_exec(const char *dir, const char *cmdName);

#ifdef WIN32
static int    win32_check_directory_write_permissions(void);
#endif


/*
 * get_bin_version
 *
 *    Fetch versions of binaries for cluster.
 */
static void
get_bin_version(ClusterInfo *cluster)
{
    char        cmd[MAXPGPATH],
                cmd_output[MAX_STRING];
    FILE       *output;
    int            pre_dot = 0,
                post_dot = 0;

    snprintf(cmd, sizeof(cmd), "\"%s/pg_ctl\" --version", cluster->bindir);

    if ((output = popen(cmd, "r")) == NULL ||
        fgets(cmd_output, sizeof(cmd_output), output) == NULL)
        pg_fatal("could not get pg_ctl version data using %s: %s\n",
                 cmd, strerror(errno));

    pclose(output);

    /* Remove trailing newline */
    if (strchr(cmd_output, '\n') != NULL)
        *strchr(cmd_output, '\n') = '\0';

    if (sscanf(cmd_output, "%*s %*s %d.%d", &pre_dot, &post_dot) < 1)
        pg_fatal("could not get version from %s\n", cmd);

    cluster->bin_version = (pre_dot * 100 + post_dot) * 100;
}


/*
 * exec_prog()
 *        Execute an external program with stdout/stderr redirected, and report
 *        errors
 *
 * Formats a command from the given argument list, logs it to the log file,
 * and attempts to execute that command.  If the command executes
 * successfully, exec_prog() returns true.
 *
 * If the command fails, an error message is saved to the specified log_file.
 * If throw_error is true, this raises a PG_FATAL error and pg_upgrade
 * terminates; otherwise it is just reported as PG_REPORT and exec_prog()
 * returns false.
 *
 * The code requires it be called first from the primary thread on Windows.
 */
bool
exec_prog(const char *log_file, const char *opt_log_file,
          bool throw_error, const char *fmt,...)
{// #lizard forgives
    int            result = 0;
    int            written;

#define MAXCMDLEN (2 * MAXPGPATH)
    char        cmd[MAXCMDLEN];
    FILE       *log;
    va_list        ap;

#ifdef WIN32
    static DWORD mainThreadId = 0;

    /* We assume we are called from the primary thread first */
    if (mainThreadId == 0)
        mainThreadId = GetCurrentThreadId();
#endif

    written = 0;
    va_start(ap, fmt);
    written += vsnprintf(cmd + written, MAXCMDLEN - written, fmt, ap);
    va_end(ap);
    if (written >= MAXCMDLEN)
        pg_fatal("command too long\n");
    written += snprintf(cmd + written, MAXCMDLEN - written,
                        " >> \"%s\" 2>&1", log_file);
    if (written >= MAXCMDLEN)
        pg_fatal("command too long\n");

    pg_log(PG_VERBOSE, "%s\n", cmd);

#ifdef WIN32

    /*
     * For some reason, Windows issues a file-in-use error if we write data to
     * the log file from a non-primary thread just before we create a
     * subprocess that also writes to the same log file.  One fix is to sleep
     * for 100ms.  A cleaner fix is to write to the log file _after_ the
     * subprocess has completed, so we do this only when writing from a
     * non-primary thread.  fflush(), running system() twice, and pre-creating
     * the file do not see to help.
     */
    if (mainThreadId != GetCurrentThreadId())
        result = system(cmd);
#endif

    log = fopen(log_file, "a");

#ifdef WIN32
    {
        /*
         * "pg_ctl -w stop" might have reported that the server has stopped
         * because the postmaster.pid file has been removed, but "pg_ctl -w
         * start" might still be in the process of closing and might still be
         * holding its stdout and -l log file descriptors open.  Therefore,
         * try to open the log file a few more times.
         */
        int            iter;

        for (iter = 0; iter < 4 && log == NULL; iter++)
        {
            pg_usleep(1000000); /* 1 sec */
            log = fopen(log_file, "a");
        }
    }
#endif

    if (log == NULL)
        pg_fatal("cannot write to log file %s\n", log_file);

#ifdef WIN32
    /* Are we printing "command:" before its output? */
    if (mainThreadId == GetCurrentThreadId())
        fprintf(log, "\n\n");
#endif
    fprintf(log, "command: %s\n", cmd);
#ifdef WIN32
    /* Are we printing "command:" after its output? */
    if (mainThreadId != GetCurrentThreadId())
        fprintf(log, "\n\n");
#endif

    /*
     * In Windows, we must close the log file at this point so the file is not
     * open while the command is running, or we get a share violation.
     */
    fclose(log);

#ifdef WIN32
    /* see comment above */
    if (mainThreadId == GetCurrentThreadId())
#endif
        result = system(cmd);

    if (result != 0)
    {
        /* we might be in on a progress status line, so go to the next line */
        report_status(PG_REPORT, "\n*failure*");
        fflush(stdout);

        pg_log(PG_VERBOSE, "There were problems executing \"%s\"\n", cmd);
        if (opt_log_file)
            pg_log(throw_error ? PG_FATAL : PG_REPORT,
                   "Consult the last few lines of \"%s\" or \"%s\" for\n"
                   "the probable cause of the failure.\n",
                   log_file, opt_log_file);
        else
            pg_log(throw_error ? PG_FATAL : PG_REPORT,
                   "Consult the last few lines of \"%s\" for\n"
                   "the probable cause of the failure.\n",
                   log_file);
    }

#ifndef WIN32

    /*
     * We can't do this on Windows because it will keep the "pg_ctl start"
     * output filename open until the server stops, so we do the \n\n above on
     * that platform.  We use a unique filename for "pg_ctl start" that is
     * never reused while the server is running, so it works fine.  We could
     * log these commands to a third file, but that just adds complexity.
     */
    if ((log = fopen(log_file, "a")) == NULL)
        pg_fatal("cannot write to log file %s\n", log_file);
    fprintf(log, "\n\n");
    fclose(log);
#endif

    return result == 0;
}


/*
 * pid_lock_file_exists()
 *
 * Checks whether the postmaster.pid file exists.
 */
bool
pid_lock_file_exists(const char *datadir)
{
    char        path[MAXPGPATH];
    int            fd;

    snprintf(path, sizeof(path), "%s/postmaster.pid", datadir);

    if ((fd = open(path, O_RDONLY, 0)) < 0)
    {
        /* ENOTDIR means we will throw a more useful error later */
        if (errno != ENOENT && errno != ENOTDIR)
            pg_fatal("could not open file \"%s\" for reading: %s\n",
                     path, strerror(errno));

        return false;
    }

    close(fd);
    return true;
}


/*
 * verify_directories()
 *
 * does all the hectic work of verifying directories and executables
 * of old and new server.
 *
 * NOTE: May update the values of all parameters
 */
void
verify_directories(void)
{
#ifndef WIN32
    if (access(".", R_OK | W_OK | X_OK) != 0)
#else
    if (win32_check_directory_write_permissions() != 0)
#endif
        pg_fatal("You must have read and write access in the current directory.\n");

    check_bin_dir(&old_cluster);
    check_data_dir(&old_cluster);
    check_bin_dir(&new_cluster);
    check_data_dir(&new_cluster);
}


#ifdef WIN32
/*
 * win32_check_directory_write_permissions()
 *
 *    access() on WIN32 can't check directory permissions, so we have to
 *    optionally create, then delete a file to check.
 *        http://msdn.microsoft.com/en-us/library/1w06ktdy%28v=vs.80%29.aspx
 */
static int
win32_check_directory_write_permissions(void)
{
    int            fd;

    /*
     * We open a file we would normally create anyway.  We do this even in
     * 'check' mode, which isn't ideal, but this is the best we can do.
     */
    if ((fd = open(GLOBALS_DUMP_FILE, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR)) < 0)
        return -1;
    close(fd);

    return unlink(GLOBALS_DUMP_FILE);
}
#endif


/*
 * check_single_dir()
 *
 *    Check for the presence of a single directory in PGDATA, and fail if
 * is it missing or not accessible.
 */
static void
check_single_dir(const char *pg_data, const char *subdir)
{
    struct stat statBuf;
    char        subDirName[MAXPGPATH];

    snprintf(subDirName, sizeof(subDirName), "%s%s%s", pg_data,
    /* Win32 can't stat() a directory with a trailing slash. */
             *subdir ? "/" : "",
             subdir);

    if (stat(subDirName, &statBuf) != 0)
        report_status(PG_FATAL, "check for \"%s\" failed: %s\n",
                      subDirName, strerror(errno));
    else if (!S_ISDIR(statBuf.st_mode))
        report_status(PG_FATAL, "%s is not a directory\n",
                      subDirName);
}


/*
 * check_data_dir()
 *
 *    This function validates the given cluster directory - we search for a
 *    small set of subdirectories that we expect to find in a valid $PGDATA
 *    directory.  If any of the subdirectories are missing (or secured against
 *    us) we display an error message and exit()
 *
 */
static void
check_data_dir(ClusterInfo *cluster)
{
    const char *pg_data = cluster->pgdata;

    /* get old and new cluster versions */
    old_cluster.major_version = get_major_server_version(&old_cluster);
    new_cluster.major_version = get_major_server_version(&new_cluster);

    check_single_dir(pg_data, "");
    check_single_dir(pg_data, "base");
    check_single_dir(pg_data, "global");
    check_single_dir(pg_data, "pg_multixact");
    check_single_dir(pg_data, "pg_subtrans");
    check_single_dir(pg_data, "pg_tblspc");
    check_single_dir(pg_data, "pg_twophase");
#ifdef __TWO_PHASE_TRANS__
    check_single_dir(pg_data, "pg_2pc");
#endif

    /* pg_xlog has been renamed to pg_wal in v10 */
    if (GET_MAJOR_VERSION(cluster->major_version) < 1000)
        check_single_dir(pg_data, "pg_xlog");
    else
        check_single_dir(pg_data, "pg_wal");

    /* pg_clog has been renamed to pg_xact in v10 */
    if (GET_MAJOR_VERSION(cluster->major_version) < 1000)
        check_single_dir(pg_data, "pg_clog");
    else
        check_single_dir(pg_data, "pg_xact");
}


/*
 * check_bin_dir()
 *
 *    This function searches for the executables that we expect to find
 *    in the binaries directory.  If we find that a required executable
 *    is missing (or secured against us), we display an error message and
 *    exit().
 */
static void
check_bin_dir(ClusterInfo *cluster)
{
    struct stat statBuf;

    /* check bindir */
    if (stat(cluster->bindir, &statBuf) != 0)
        report_status(PG_FATAL, "check for \"%s\" failed: %s\n",
                      cluster->bindir, strerror(errno));
    else if (!S_ISDIR(statBuf.st_mode))
        report_status(PG_FATAL, "%s is not a directory\n",
                      cluster->bindir);

    validate_exec(cluster->bindir, "postgres");
    validate_exec(cluster->bindir, "pg_ctl");

    /*
     * Fetch the binary versions after checking for the existence of pg_ctl,
     * this gives a correct error if the binary used itself for the version
     * fetching is broken.
     */
    get_bin_version(&old_cluster);
    get_bin_version(&new_cluster);

    /* pg_resetxlog has been renamed to pg_resetwal in version 10 */
    if (GET_MAJOR_VERSION(cluster->bin_version) < 1000)
        validate_exec(cluster->bindir, "pg_resetxlog");
    else
        validate_exec(cluster->bindir, "pg_resetwal");
    if (cluster == &new_cluster)
    {
        /* these are only needed in the new cluster */
        validate_exec(cluster->bindir, "psql");
        validate_exec(cluster->bindir, "pg_dump");
        validate_exec(cluster->bindir, "pg_dumpall");
    }
}


/*
 * validate_exec()
 *
 * validate "path" as an executable file
 */
static void
validate_exec(const char *dir, const char *cmdName)
{// #lizard forgives
    char        path[MAXPGPATH];
    struct stat buf;

    snprintf(path, sizeof(path), "%s/%s", dir, cmdName);

#ifdef WIN32
    /* Windows requires a .exe suffix for stat() */
    if (strlen(path) <= strlen(EXE_EXT) ||
        pg_strcasecmp(path + strlen(path) - strlen(EXE_EXT), EXE_EXT) != 0)
        strlcat(path, EXE_EXT, sizeof(path));
#endif

    /*
     * Ensure that the file exists and is a regular file.
     */
    if (stat(path, &buf) < 0)
        pg_fatal("check for \"%s\" failed: %s\n",
                 path, strerror(errno));
    else if (!S_ISREG(buf.st_mode))
        pg_fatal("check for \"%s\" failed: not an executable file\n",
                 path);

    /*
     * Ensure that the file is both executable and readable (required for
     * dynamic loading).
     */
#ifndef WIN32
    if (access(path, R_OK) != 0)
#else
    if ((buf.st_mode & S_IRUSR) == 0)
#endif
        pg_fatal("check for \"%s\" failed: cannot read file (permission denied)\n",
                 path);

#ifndef WIN32
    if (access(path, X_OK) != 0)
#else
    if ((buf.st_mode & S_IXUSR) == 0)
#endif
        pg_fatal("check for \"%s\" failed: cannot execute (permission denied)\n",
                 path);
}
