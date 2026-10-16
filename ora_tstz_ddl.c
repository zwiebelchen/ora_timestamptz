/*-------------------------------------------------------------------------
 *
 * ora_tstz_ddl.c
 *	  Optional DDL rewriting: columns declared as TIMESTAMP WITH TIME ZONE
 *	  (PostgreSQL timestamptz) become ora_timestamptz.
 *
 * PostgreSQL's grammar maps "TIMESTAMP [(p)] WITH TIME ZONE" to
 * pg_catalog.timestamptz and offers no hook to change that.  What an
 * extension can do is intercept utility statements (ProcessUtility_hook)
 * and replace the column types in CREATE TABLE, ALTER TABLE ... ADD COLUMN /
 * ALTER COLUMN TYPE, CREATE DOMAIN and CREATE TYPE ... AS (...) before they
 * are executed.  The result is a real ora_timestamptz column.
 *
 * Controlled by
 *	 ora_timestamptz.replace_timestamptz = off | with_time_zone | all
 *		off				nothing is rewritten (default)
 *		with_time_zone	only types written as TIMESTAMP [(p)] WITH TIME ZONE
 *						(Oracle spelling); "timestamptz" stays timestamptz
 *		all				also types written as timestamptz
 *	 ora_timestamptz.replace_schemas = 'schema1, schema2'
 *		restrict rewriting to objects created in these schemas
 *		(empty: all schemas except pg_catalog / information_schema)
 *
 * Statements run by CREATE EXTENSION scripts are never rewritten.  The
 * library must be loaded before the DDL runs, so for reliable operation
 * add it to shared_preload_libraries or session_preload_libraries.
 *
 * ora_timestamptz/ora_tstz_ddl.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parse_type.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/varlena.h"

#include "ora_timestamptz.h"

typedef enum
{
	OTZ_REPLACE_OFF,
	OTZ_REPLACE_WITH_TIME_ZONE,
	OTZ_REPLACE_ALL
} OtzReplaceMode;

static const struct config_enum_entry replace_options[] = {
	{"off", OTZ_REPLACE_OFF, false},
	{"with_time_zone", OTZ_REPLACE_WITH_TIME_ZONE, false},
	{"all", OTZ_REPLACE_ALL, false},
	{NULL, 0, false}
};

static int	otz_replace_mode = OTZ_REPLACE_OFF;
static char *otz_replace_schemas = NULL;

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

static bool
check_replace_schemas(char **newval, void **extra, GucSource source)
{
	char	   *raw = pstrdup(*newval);
	List	   *names;
	bool		ok = SplitIdentifierString(raw, ',', &names);

	list_free(names);
	pfree(raw);
	if (!ok)
	{
		GUC_check_errdetail("List syntax is invalid.");
		return false;
	}
	return true;
}

/* per-statement state */
typedef struct
{
	const char *query;			/* source text, for spelling check */
	size_t		querylen;
	char	   *ext_schema;		/* schema of the ora_timestamptz type */
	int			replaced;
} RewriteCtx;

/*
 * Was the type written as "TIMESTAMP ... WITH TIME ZONE"?  The grammar
 * sets the location of that TypeName to the TIMESTAMP keyword, while
 * "timestamptz" points at the identifier "timestamptz".
 */
static bool
written_as_with_time_zone(const TypeName *tn, const RewriteCtx *ctx)
{
	const char *p;

	if (ctx->query == NULL || tn->location < 0 ||
		(size_t) tn->location + 9 > ctx->querylen)
		return false;
	p = ctx->query + tn->location;
	if (pg_strncasecmp(p, "timestamp", 9) != 0)
		return false;
	return !(isalnum((unsigned char) p[9]) || p[9] == '_' || p[9] == '$');
}

static bool
is_timestamptz(const TypeName *tn)
{
	Oid			typid;

	if (tn == NULL || tn->setof || tn->pct_type || tn->names == NIL)
		return false;
	/* cheap pre-check on the last name component */
	if (strcmp(strVal(llast(tn->names)), "timestamptz") != 0)
		return false;
	/*
	 * Look up the base type without typmods and array bounds: typmods would
	 * be validated against timestamptz (which only allows precision 0..6,
	 * Oracle allows 0..9), and "timestamptz[]" must match as well.
	 */
	{
		TypeName   *base = copyObject((TypeName *) tn);

		base->typmods = NIL;
		base->arrayBounds = NIL;
		typid = LookupTypeNameOid(NULL, base, true);
	}
	return typid == TIMESTAMPTZOID;
}

static void
maybe_replace(TypeName *tn, RewriteCtx *ctx)
{
	if (!is_timestamptz(tn))
		return;
	if (otz_replace_mode == OTZ_REPLACE_WITH_TIME_ZONE &&
		!written_as_with_time_zone(tn, ctx))
		return;

	/* keep typmods (precision), array bounds and location */
	tn->names = list_make2(makeString(pstrdup(ctx->ext_schema)),
						   makeString(pstrdup("ora_timestamptz")));
	tn->typeOid = InvalidOid;
	ctx->replaced++;
}

static void
rewrite_coldefs(List *elts, RewriteCtx *ctx)
{
	ListCell   *lc;

	foreach(lc, elts)
	{
		Node	   *n = lfirst(lc);

		if (IsA(n, ColumnDef))
			maybe_replace(((ColumnDef *) n)->typeName, ctx);
	}
}

/* Is the target schema subject to rewriting? */
static bool
schema_selected(Oid nspid)
{
	char	   *nspname;
	char	   *raw;
	List	   *names;
	ListCell   *lc;
	bool		found = false;

	if (!OidIsValid(nspid) || IsCatalogNamespace(nspid) ||
		IsToastNamespace(nspid))
		return false;
	nspname = get_namespace_name(nspid);
	if (nspname == NULL || strcmp(nspname, "information_schema") == 0)
		return false;
	if (otz_replace_schemas == NULL || otz_replace_schemas[0] == '\0')
		return true;

	raw = pstrdup(otz_replace_schemas);
	if (!SplitIdentifierString(raw, ',', &names))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax in parameter \"ora_timestamptz.replace_schemas\"")));
	foreach(lc, names)
	{
		if (strcmp((char *) lfirst(lc), nspname) == 0)
		{
			found = true;
			break;
		}
	}
	return found;
}

static Oid
relation_namespace(RangeVar *rv)
{
	Oid			relid = RangeVarGetRelid(rv, NoLock, true);

	return OidIsValid(relid) ? get_rel_namespace(relid) : InvalidOid;
}

/* Does this statement possibly contain column types we rewrite? */
static bool
is_candidate(Node *stmt)
{
	switch (nodeTag(stmt))
	{
		case T_CreateStmt:
		case T_CreateForeignTableStmt:
		case T_AlterTableStmt:
		case T_CreateDomainStmt:
		case T_CompositeTypeStmt:
			return true;
		default:
			return false;
	}
}

static void
rewrite_stmt(Node *stmt, RewriteCtx *ctx)
{
	switch (nodeTag(stmt))
	{
		case T_CreateStmt:
		case T_CreateForeignTableStmt:
			{
				/* CreateForeignTableStmt begins with a CreateStmt */
				CreateStmt *cs = (CreateStmt *) stmt;

				if (schema_selected(RangeVarGetCreationNamespace(cs->relation)))
					rewrite_coldefs(cs->tableElts, ctx);
			}
			break;
		case T_AlterTableStmt:
			{
				AlterTableStmt *as = (AlterTableStmt *) stmt;
				ListCell   *lc;

				if (as->objtype != OBJECT_TABLE &&
					as->objtype != OBJECT_FOREIGN_TABLE)
					break;
				if (!schema_selected(relation_namespace(as->relation)))
					break;
				foreach(lc, as->cmds)
				{
					AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

					if ((cmd->subtype == AT_AddColumn ||
						 cmd->subtype == AT_AlterColumnType) &&
						cmd->def && IsA(cmd->def, ColumnDef))
						maybe_replace(((ColumnDef *) cmd->def)->typeName, ctx);
				}
			}
			break;
		case T_CreateDomainStmt:
			{
				CreateDomainStmt *ds = (CreateDomainStmt *) stmt;
				char	   *objname;

				if (schema_selected(QualifiedNameGetCreationNamespace(ds->domainname, &objname)))
					maybe_replace(ds->typeName, ctx);
			}
			break;
		case T_CompositeTypeStmt:
			{
				CompositeTypeStmt *ct = (CompositeTypeStmt *) stmt;

				if (schema_selected(RangeVarGetCreationNamespace(ct->typevar)))
					rewrite_coldefs(ct->coldeflist, ctx);
			}
			break;
		default:
			break;
	}
}

static void
otz_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
				   bool readOnlyTree, ProcessUtilityContext context,
				   ParamListInfo params, QueryEnvironment *queryEnv,
				   DestReceiver *dest, QueryCompletion *qc)
{
	if (otz_replace_mode != OTZ_REPLACE_OFF && !creating_extension &&
		is_candidate(pstmt->utilityStmt))
	{
		Oid			extoid = get_extension_oid("ora_timestamptz", true);

		if (OidIsValid(extoid))
		{
			RewriteCtx	ctx;

			ctx.query = queryString;
			ctx.querylen = queryString ? strlen(queryString) : 0;
			ctx.ext_schema = get_namespace_name(get_extension_schema(extoid));
			ctx.replaced = 0;

			if (ctx.ext_schema != NULL)
			{
				if (readOnlyTree)
				{
					pstmt = copyObject(pstmt);
					readOnlyTree = false;
				}
				rewrite_stmt(pstmt->utilityStmt, &ctx);
				if (ctx.replaced > 0)
					ereport(DEBUG1,
							(errmsg_internal("ora_timestamptz: replaced %d timestamptz type reference(s) by ora_timestamptz",
											 ctx.replaced)));
			}
		}
	}

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

void
otz_ddl_init(void)
{
	DefineCustomEnumVariable("ora_timestamptz.replace_timestamptz",
							 "Rewrite timestamptz column types in DDL to ora_timestamptz.",
							 "off: never; with_time_zone: only types written as "
							 "TIMESTAMP [(p)] WITH TIME ZONE; all: also timestamptz. "
							 "Requires the library to be preloaded "
							 "(shared_preload_libraries or session_preload_libraries).",
							 &otz_replace_mode,
							 OTZ_REPLACE_OFF,
							 replace_options,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("ora_timestamptz.replace_schemas",
							   "Schemas in which DDL rewriting applies.",
							   "Comma-separated list; empty means all schemas "
							   "except pg_catalog and information_schema.",
							   &otz_replace_schemas,
							   "",
							   PGC_USERSET, GUC_LIST_INPUT,
							   check_replace_schemas, NULL, NULL);

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = otz_ProcessUtility;
}
