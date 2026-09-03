/* vms_plan.h — bounded compiled plan for safe pushdown (R7).
 *
 * Safety rules (TZ mandatory):
 *   - projection: only consumed columns (colUsed), quoted identifiers
 *   - integer comparisons: only when both the column is INT-affine and the
 *     rhs value is an integer (float/text never pushed down)
 *   - IS NULL / IS NOT NULL: any column
 *   - IN: only bounded integer lists (sqlite3_vtab_in family)
 *   - ordering: only exact integer columns
 *   - LIMIT/OFFSET: only integer values; OFFSET requires LIMIT present
 * Text comparison/order stay local (no semantic proof).
 * Everything else degrades to a full scan + local filtering — correctness
 * first, no silent semantic drift.
 *
 * NOTE: include order matters — sqlite3ext.h must be processed before any
 * plain sqlite3.h so the api-thunk macros take effect. */
#ifndef VIRTUALMSSQL_VMS_PLAN_H
#define VIRTUALMSSQL_VMS_PLAN_H

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#endif
#include "vms_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VMS_PLAN_MAX_ARGS 32
#define VMS_PLAN_MAGIC 0x56504C37u /* "VPL7" */

typedef enum VmsPlanOp {
    VMS_OP_EQ = 0,
    VMS_OP_LT,
    VMS_OP_LE,
    VMS_OP_GT,
    VMS_OP_GE,
    VMS_OP_ISNULL,
    VMS_OP_ISNOTNULL,
    VMS_OP_IN
} VmsPlanOp;

typedef struct VmsPlanTerm {
    int col;             /* 0-based column index into the remote table */
    VmsPlanOp op;
    int arg_index;       /* index into xFilter argv; -1 when n/a */
} VmsPlanTerm;

typedef struct VmsPlan {
    unsigned magic;
    int used_mask;               /* projection: bits of columns we select */
    int omit_mask;               /* constraints SQLite lets us omit locally */
    VmsPlanTerm terms[VMS_PLAN_MAX_ARGS];
    int nterms;
    int nargs;                   /* total argv values consumed */
    /* ordering: sequence of (col, desc) pairs; only INT-affine columns */
    int order_cols[VMS_PLAN_MAX_ARGS];
    int order_desc[VMS_PLAN_MAX_ARGS];
    int norder;
    int has_limit;
    int has_offset;
    int limit_arg;               /* argv index of limit value, -1 n/a */
    int offset_arg;
} VmsPlan;

/* compile from xBestIndex info. Returns 1 when a plan was built (idxNum /
 * idxStr payload ready), 0 when nothing is pushed down (full scan). */
int vms_plan_compile(sqlite3_index_info* info, const VmsMetaColumn* cols,
                     int ncols, VmsPlan* plan);

/* serialize into idxStr-compatible buffer (memcpy-able, self-validating). */
int vms_plan_serialize(const VmsPlan* plan, char* buf, size_t cap);
/* inverse of serialize; validates magic and sizes; returns 1 on success. */
int vms_plan_deserialize(const char* buf, size_t len, VmsPlan* plan);

/* build the remote T-SQL:
 *   SELECT <projection> FROM [schema].[table]
 *   [WHERE <terms with ? placeholders>]
 *   [ORDER BY <int columns>]
 *   [OFFSET n ROWS FETCH NEXT m ROWS ONLY]
 * Placeholders map 1:1 to ODBC parameters; *nparams receives the count.
 * Returns 1 on success. */
int vms_plan_build_sql(const VmsPlan* plan, const char* schema, const char* table,
                       const VmsMetaColumn* cols, int ncols,
                       int spatial_wkt,
                       wchar_t* sql, size_t sql_chars, int* nparams);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_PLAN_H */
