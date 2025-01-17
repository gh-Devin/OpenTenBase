%{
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
 * repl_gram.y				- Parser for the replication commands
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/repl_gram.y
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlogdefs.h"
#include "nodes/makefuncs.h"
#include "nodes/replnodes.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"


/* Result of the parsing is returned here */
Node *replication_parse_result;

static SQLCmd *make_sqlcmd(void);


/*
 * Bison doesn't allocate anything that needs to live across parser calls,
 * so we can easily have it use palloc instead of malloc.  This prevents
 * memory leaks if we error out during parsing.  Note this only works with
 * bison >= 2.0.  However, in bison 1.875 the default is to use alloca()
 * if possible, so there's not really much problem anyhow, at least if
 * you're building with gcc.
 */
#define YYMALLOC palloc
#define YYFREE   pfree

%}

%expect 0
%name-prefix="replication_yy"

%union {
		char					*str;
		bool					boolval;
		uint32					uintval;

		XLogRecPtr				recptr;
		Node					*node;
		List					*list;
		DefElem					*defelt;
}

/* Non-keyword tokens */
%token <str> SCONST IDENT
%token <uintval> UCONST
%token <recptr> RECPTR
%token T_WORD

/* Keyword tokens. */
%token K_BASE_BACKUP
%token K_IDENTIFY_SYSTEM
%token K_SHOW
%token K_START_REPLICATION
%token K_CREATE_REPLICATION_SLOT
%token K_DROP_REPLICATION_SLOT
%token K_TIMELINE_HISTORY
%token K_LABEL
%token K_PROGRESS
%token K_FAST
%token K_NOWAIT
%token K_MAX_RATE
%token K_WAL
%token K_TABLESPACE_MAP
%token K_TIMELINE
%token K_PHYSICAL
%token K_LOGICAL
%token K_SLOT
%token K_RESERVE_WAL
%token K_TEMPORARY
%token K_EXPORT_SNAPSHOT
%token K_NOEXPORT_SNAPSHOT
%token K_USE_SNAPSHOT
%token K_SUBSCRIPTION_NAME
%token K_ID_SUBSCRIPTION
%token K_REL_NAMESPACE
%token K_NAME_REL

%type <node>	command
%type <node>	base_backup start_replication start_logical_replication
				create_replication_slot drop_replication_slot identify_system
				timeline_history show sql_cmd
%type <list>	base_backup_opt_list
%type <defelt>	base_backup_opt
%type <uintval>	opt_timeline
%type <list>	plugin_options plugin_opt_list
%type <defelt>	plugin_opt_elem
%type <node>	plugin_opt_arg
%type <str>		opt_slot var_name
%type <boolval>	opt_temporary
%type <list>	create_slot_opt_list
%type <defelt>	create_slot_opt

%%

firstcmd: command opt_semicolon
				{
					replication_parse_result = $1;
				}
			;

opt_semicolon:	';'
				| /* EMPTY */
				;

command:
			identify_system
			| base_backup
			| start_replication
			| start_logical_replication
			| create_replication_slot
			| drop_replication_slot
			| timeline_history
			| show
			| sql_cmd
			;

/*
 * IDENTIFY_SYSTEM
 */
identify_system:
			K_IDENTIFY_SYSTEM
				{
					$$ = (Node *) makeNode(IdentifySystemCmd);
				}
			;

/*
 * SHOW setting
 */
show:
			K_SHOW var_name
				{
					VariableShowStmt *n = makeNode(VariableShowStmt);
					n->name = $2;
					$$ = (Node *) n;
				}

var_name:	IDENT	{ $$ = $1; }
			| var_name '.' IDENT
				{ $$ = psprintf("%s.%s", $1, $3); }
		;

/*
 * BASE_BACKUP [LABEL '<label>'] [PROGRESS] [FAST] [WAL] [NOWAIT]
 * [MAX_RATE %d] [TABLESPACE_MAP]
 */
base_backup:
			K_BASE_BACKUP base_backup_opt_list
				{
					BaseBackupCmd *cmd = makeNode(BaseBackupCmd);
					cmd->options = $2;
					$$ = (Node *) cmd;
				}
			;

base_backup_opt_list:
			base_backup_opt_list base_backup_opt
				{ $$ = lappend($1, $2); }
			| /* EMPTY */
				{ $$ = NIL; }
			;

base_backup_opt:
			K_LABEL SCONST
				{
				  $$ = makeDefElem("label",
								   (Node *)makeString($2), -1);
				}
			| K_PROGRESS
				{
				  $$ = makeDefElem("progress",
								   (Node *)makeInteger(TRUE), -1);
				}
			| K_FAST
				{
				  $$ = makeDefElem("fast",
								   (Node *)makeInteger(TRUE), -1);
				}
			| K_WAL
				{
				  $$ = makeDefElem("wal",
								   (Node *)makeInteger(TRUE), -1);
				}
			| K_NOWAIT
				{
				  $$ = makeDefElem("nowait",
								   (Node *)makeInteger(TRUE), -1);
				}
			| K_MAX_RATE UCONST
				{
				  $$ = makeDefElem("max_rate",
								   (Node *)makeInteger($2), -1);
				}
			| K_TABLESPACE_MAP
				{
				  $$ = makeDefElem("tablespace_map",
								   (Node *)makeInteger(TRUE), -1);
				}
			;

create_replication_slot:
			/* CREATE_REPLICATION_SLOT slot TEMPORARY PHYSICAL RESERVE_WAL */
			K_CREATE_REPLICATION_SLOT IDENT opt_temporary K_PHYSICAL create_slot_opt_list
				{
					CreateReplicationSlotCmd *cmd;
					cmd = makeNode(CreateReplicationSlotCmd);
					cmd->kind = REPLICATION_KIND_PHYSICAL;
					cmd->slotname = $2;
					cmd->temporary = $3;
					cmd->options = $5;
					$$ = (Node *) cmd;
				}
			/* CREATE_REPLICATION_SLOT slot TEMPORARY LOGICAL plugin */
			| K_CREATE_REPLICATION_SLOT IDENT opt_temporary K_LOGICAL IDENT create_slot_opt_list
				{
					CreateReplicationSlotCmd *cmd;
					cmd = makeNode(CreateReplicationSlotCmd);
					cmd->kind = REPLICATION_KIND_LOGICAL;
					cmd->slotname = $2;
					cmd->temporary = $3;
					cmd->plugin = $5;
					cmd->options = $6;
					$$ = (Node *) cmd;
				}
			;

create_slot_opt_list:
			create_slot_opt_list create_slot_opt
				{ $$ = lappend($1, $2); }
			| /* EMPTY */
				{ $$ = NIL; }
			;

create_slot_opt:
			K_EXPORT_SNAPSHOT
				{
				  $$ = makeDefElem("export_snapshot",
								   (Node *)makeInteger(TRUE), -1);
				}
			| K_NOEXPORT_SNAPSHOT
				{
				  $$ = makeDefElem("export_snapshot",
								   (Node *)makeInteger(FALSE), -1);
				}
			| K_USE_SNAPSHOT
				{
				  $$ = makeDefElem("use_snapshot",
								   (Node *)makeInteger(TRUE), -1);
				}
			| K_RESERVE_WAL
				{
				  $$ = makeDefElem("reserve_wal",
								   (Node *)makeInteger(TRUE), -1);
				}
			| K_SUBSCRIPTION_NAME IDENT
				{
				  $$ = makeDefElem("subscription_name",
								   (Node *)makeString($2), -1);
				}
            | K_ID_SUBSCRIPTION UCONST
				{
				  $$ = makeDefElem("subscription_id",
								   (Node *)makeInteger($2), -1);
				}
			| K_REL_NAMESPACE IDENT
				{
				  $$ = makeDefElem("rel_namespace",
								   (Node *)makeString($2), -1);
				}
			| K_NAME_REL IDENT
				{
				  $$ = makeDefElem("rel_name",
								   (Node *)makeString($2), -1);
				}
			;

/* DROP_REPLICATION_SLOT slot */
drop_replication_slot:
			K_DROP_REPLICATION_SLOT IDENT
				{
					DropReplicationSlotCmd *cmd;
					cmd = makeNode(DropReplicationSlotCmd);
					cmd->slotname = $2;
					$$ = (Node *) cmd;
				}
			;

/*
 * START_REPLICATION [SLOT slot] [PHYSICAL] %X/%X [TIMELINE %d]
 */
start_replication:
			K_START_REPLICATION opt_slot opt_physical RECPTR opt_timeline
				{
					StartReplicationCmd *cmd;

					cmd = makeNode(StartReplicationCmd);
					cmd->kind = REPLICATION_KIND_PHYSICAL;
					cmd->slotname = $2;
					cmd->startpoint = $4;
					cmd->timeline = $5;
					$$ = (Node *) cmd;
				}
			;

/* START_REPLICATION SLOT slot LOGICAL %X/%X options */
start_logical_replication:
			K_START_REPLICATION K_SLOT IDENT K_LOGICAL RECPTR plugin_options
				{
					StartReplicationCmd *cmd;
					cmd = makeNode(StartReplicationCmd);
					cmd->kind = REPLICATION_KIND_LOGICAL;
					cmd->slotname = $3;
					cmd->startpoint = $5;
					cmd->options = $6;
					$$ = (Node *) cmd;
				}
			;
/*
 * TIMELINE_HISTORY %d
 */
timeline_history:
			K_TIMELINE_HISTORY UCONST
				{
					TimeLineHistoryCmd *cmd;

					if ($2 <= 0)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 (errmsg("invalid timeline %u", $2))));

					cmd = makeNode(TimeLineHistoryCmd);
					cmd->timeline = $2;

					$$ = (Node *) cmd;
				}
			;

opt_physical:
			K_PHYSICAL
			| /* EMPTY */
			;

opt_temporary:
			K_TEMPORARY						{ $$ = true; }
			| /* EMPTY */					{ $$ = false; }
			;

opt_slot:
			K_SLOT IDENT
				{ $$ = $2; }
			| /* EMPTY */
				{ $$ = NULL; }
			;

opt_timeline:
			K_TIMELINE UCONST
				{
					if ($2 <= 0)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 (errmsg("invalid timeline %u", $2))));
					$$ = $2;
				}
				| /* EMPTY */			{ $$ = 0; }
			;


plugin_options:
			'(' plugin_opt_list ')'			{ $$ = $2; }
			| /* EMPTY */					{ $$ = NIL; }
		;

plugin_opt_list:
			plugin_opt_elem
				{
					$$ = list_make1($1);
				}
			| plugin_opt_list ',' plugin_opt_elem
				{
					$$ = lappend($1, $3);
				}
		;

plugin_opt_elem:
			IDENT plugin_opt_arg
				{
					$$ = makeDefElem($1, $2, -1);
				}
		;

plugin_opt_arg:
			SCONST							{ $$ = (Node *) makeString($1); }
			| /* EMPTY */					{ $$ = NULL; }
		;

sql_cmd:
			IDENT							{ $$ = (Node *) make_sqlcmd(); }
		;
%%

static SQLCmd *
make_sqlcmd(void)
{
	SQLCmd *cmd = makeNode(SQLCmd);
	int tok;

	/* Just move lexer to the end of command. */
	for (;;)
	{
		tok = yylex();
		if (tok == ';' || tok == 0)
			break;
	}
	return cmd;
}

#include "repl_scanner.c"
