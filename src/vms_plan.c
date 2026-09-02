/* vms_plan.c — bounded compiled plan for safe pushdown (R7). */
#include "vms_plan.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

/* present in SQLite sources since 3.38 but missing from this amalgamation's
 * documented #define block; the numeric value is fixed by the core */
#ifndef SQLITE_INDEX_CONSTRAINT_IN
#define SQLITE_INDEX_CONSTRAINT_IN 31
#endif

/* LIMIT/OFFSET arrive as regular constraints since 3.38 (73/74) */
#ifndef SQLITE_INDEX_CONSTRAINT_LIMIT
#define SQLITE_INDEX_CONSTRAINT_LIMIT 73
#endif
#ifndef SQLITE_INDEX_CONSTRAINT_OFFSET
#define SQLITE_INDEX_CONSTRAINT_OFFSET 74
#endif

/* column is integer-affine for pushdown: the R5 registry mapped it to INT64 */
static int col_is_int(const VmsMetaColumn* c)
{
    return c && c->vtype == VMS_CT_INT64;
}

int vms_plan_compile(sqlite3_index_info* info, const VmsMetaColumn* cols,
                     int ncols, VmsPlan* plan)
{
    int i;
    int nargs = 0;
    int order_ok;

    memset(plan, 0, sizeof(*plan));
    plan->magic = VMS_PLAN_MAGIC;
    plan->limit_arg = -1;
    plan->offset_arg = -1;

    /* projection: mark consumed columns; colUsed bit 63 covers columns >= 62
     * and cannot be trusted per docs — if set, fall back to all columns. */
    if (info->colUsed & ((sqlite3_uint64)1 << 63)) {
        plan->used_mask = -1; /* all */
    } else {
        for (i = 0; i < ncols && i < 62; i++) {
            if (info->colUsed & ((sqlite3_uint64)1 << i))
                plan->used_mask |= (1 << i);
        }
        /* always project the columns referenced by constraints/order too */
        for (i = 0; i < info->nConstraint; i++) {
            int c = info->aConstraint[i].iColumn;
            if (c >= 0 && c < 62) plan->used_mask |= (1 << c);
        }
        for (i = 0; i < info->nOrderBy; i++) {
            int c = info->aOrderBy[i].iColumn;
            if (c >= 0 && c < 62) plan->used_mask |= (1 << c);
        }
    }

    /* constraints: consume only provably safe ones */
    for (i = 0; i < info->nConstraint && plan->nterms < VMS_PLAN_MAX_ARGS; i++) {
        struct sqlite3_index_constraint* c = &info->aConstraint[i];
        VmsPlanTerm* t;
        int col = c->iColumn;
        int op = -1;

        if (!c->usable) continue;
        if (col < 0 || col >= ncols) continue; /* rowid: stay local */

        switch (c->op) {
        case SQLITE_INDEX_CONSTRAINT_EQ:    op = VMS_OP_EQ; break;
        case SQLITE_INDEX_CONSTRAINT_LT:    op = VMS_OP_LT; break;
        case SQLITE_INDEX_CONSTRAINT_LE:    op = VMS_OP_LE; break;
        case SQLITE_INDEX_CONSTRAINT_GT:    op = VMS_OP_GT; break;
        case SQLITE_INDEX_CONSTRAINT_GE:    op = VMS_OP_GE; break;
        case SQLITE_INDEX_CONSTRAINT_ISNULL: op = VMS_OP_ISNULL; break;
        case SQLITE_INDEX_CONSTRAINT_ISNOTNULL: op = VMS_OP_ISNOTNULL; break;
        default: continue;
        }

        t = &plan->terms[plan->nterms];
        memset(t, 0, sizeof(*t));
        t->col = col;

        if (op == VMS_OP_ISNULL || op == VMS_OP_ISNOTNULL) {
            t->op = (VmsPlanOp)op;
            t->arg_index = -1;
        } else if (col_is_int(&cols[col])) {
            t->op = (VmsPlanOp)op;
            t->arg_index = nargs++;
        } else {
            continue; /* text/float comparison stays local */
        }

        info->aConstraintUsage[i].argvIndex = t->arg_index + 1;
        info->aConstraintUsage[i].omit = 1;
        plan->omit_mask |= (1 << i);
        plan->nterms++;
    }

    /* IN constraints: bounded integer lists via sqlite3_vtab_in */
    for (i = 0; i < info->nConstraint && plan->nterms < VMS_PLAN_MAX_ARGS; i++) {
        struct sqlite3_index_constraint* c = &info->aConstraint[i];
        VmsPlanTerm* t;
        int col = c->iColumn;

        if (!c->usable) continue;
        if (c->op != SQLITE_INDEX_CONSTRAINT_IN) continue;
        if (col < 0 || col >= ncols) continue;
        if (!col_is_int(&cols[col])) continue;
        if (!sqlite3_vtab_in(info, i, 0)) continue; /* not this cycle */

        if (!sqlite3_vtab_in(info, i, 1)) continue; /* opt-in failed */
        t = &plan->terms[plan->nterms];
        memset(t, 0, sizeof(*t));
        t->col = col;
        t->op = VMS_OP_IN;
        t->arg_index = nargs++;
        info->aConstraintUsage[i].argvIndex = t->arg_index + 1;
        info->aConstraintUsage[i].omit = 1;
        plan->omit_mask |= (1 << i);
        plan->nterms++;
    }

    /* ordering: only exact integer columns, contiguous from the start */
    order_ok = 1;
    for (i = 0; i < info->nOrderBy && order_ok; i++) {
        struct sqlite3_index_orderby* o = &info->aOrderBy[i];
        if (o->iColumn < 0 || o->iColumn >= ncols ||
            !col_is_int(&cols[o->iColumn])) {
            order_ok = 0;
            break;
        }
        if (plan->norder >= VMS_PLAN_MAX_ARGS) { order_ok = 0; break; }
        plan->order_cols[plan->norder] = o->iColumn;
        plan->order_desc[plan->norder] = o->desc ? 1 : 0;
        plan->norder++;
    }
    if (order_ok && plan->norder > 0 && plan->norder == info->nOrderBy) {
        info->orderByConsumed = 1;
    } else {
        plan->norder = 0; /* partial ORDER BY is never consumed */
    }

    /* LIMIT/OFFSET: they arrive as regular constraints (3.38+). Consume
     * them as the tail argv slots; OFFSET requires LIMIT. */
    for (i = 0; i < info->nConstraint; i++) {
        struct sqlite3_index_constraint* c = &info->aConstraint[i];
        if (!c->usable) continue;
        if (c->iColumn != -1) continue; /* LIMIT/OFFSET have no column */
        if (c->op == SQLITE_INDEX_CONSTRAINT_LIMIT && plan->limit_arg < 0) {
            plan->has_limit = 1;
            plan->limit_arg = nargs++;
            info->aConstraintUsage[i].argvIndex = plan->limit_arg + 1;
            info->aConstraintUsage[i].omit = 1;
        } else if (c->op == SQLITE_INDEX_CONSTRAINT_OFFSET && plan->has_limit) {
            plan->has_offset = 1;
            plan->offset_arg = nargs++;
            info->aConstraintUsage[i].argvIndex = plan->offset_arg + 1;
            info->aConstraintUsage[i].omit = 1;
        }
    }

    plan->nargs = nargs;

    /* cost: cheaper when we push something down */
    if (plan->nterms > 0) info->estimatedCost = 100.0;
    if (plan->norder > 0) info->estimatedCost /= 10.0;
    return 1;
}

/* ---- serialization (raw struct copy with validation) ---- */

int vms_plan_serialize(const VmsPlan* plan, char* buf, size_t cap)
{
    if (cap < sizeof(VmsPlan)) return 0;
    memcpy(buf, plan, sizeof(VmsPlan));
    return (int)sizeof(VmsPlan);
}

int vms_plan_deserialize(const char* buf, size_t len, VmsPlan* plan)
{
    const unsigned magic = VMS_PLAN_MAGIC;
    if (len < sizeof(VmsPlan)) return 0;
    memcpy(plan, buf, sizeof(VmsPlan));
    if (plan->magic != magic) return 0;
    if (plan->nterms < 0 || plan->nterms > VMS_PLAN_MAX_ARGS) return 0;
    if (plan->nargs < 0 || plan->nargs > VMS_PLAN_MAX_ARGS) return 0;
    if (plan->norder < 0 || plan->norder > VMS_PLAN_MAX_ARGS) return 0;
    return 1;
}

/* ---- T-SQL builder ---- */

static int append_w(wchar_t* sql, size_t cap, size_t* len, const wchar_t* s)
{
    size_t n = wcslen(s);
    if (*len + n + 1 > cap) return 0;
    memcpy(sql + *len, s, n * sizeof(wchar_t));
    *len += n;
    sql[*len] = 0;
    return 1;
}

static int append_col(wchar_t* sql, size_t cap, size_t* len, const char* name)
{
    /* bracket-quote a validated identifier */
    wchar_t w[300];
    int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, w + 1, 298);
    if (n <= 0) return 0;
    w[0] = L'[';
    w[n] = L']';
    w[n + 1] = 0;
    return append_w(sql, cap, len, w);
}

int vms_plan_build_sql(const VmsPlan* plan, const char* schema, const char* table,
                       const VmsMetaColumn* cols, int ncols,
                       wchar_t* sql, size_t sql_wchars, int* nparams)
{
    size_t len = 0;
    int i;
    int first = 1;

    if (!plan || plan->magic != VMS_PLAN_MAGIC || !schema || !table || !cols) return 0;
    if (!vms_meta_ident_valid(schema, 128) || !vms_meta_ident_valid(table, 128)) return 0;

    if (!append_w(sql, sql_wchars, &len, L"SELECT ")) return 0;
    if (plan->used_mask == 0) {
        if (!append_w(sql, sql_wchars, &len, L"*")) return 0;
    } else if (plan->used_mask == -1) {
        for (i = 0; i < ncols; i++) {
            if (i && !append_w(sql, sql_wchars, &len, L", ")) return 0;
            if (!append_col(sql, sql_wchars, &len, cols[i].name)) return 0;
        }
    } else {
        for (i = 0; i < ncols && i < 62; i++) {
            if (!(plan->used_mask & (1 << i))) continue;
            if (!first && !append_w(sql, sql_wchars, &len, L", ")) return 0;
            if (!append_col(sql, sql_wchars, &len, cols[i].name)) return 0;
            first = 0;
        }
        if (first && !append_w(sql, sql_wchars, &len, L"*")) return 0;
    }

    if (!append_w(sql, sql_wchars, &len, L" FROM [")) return 0;
    {
        wchar_t w[300];
        int n = MultiByteToWideChar(CP_UTF8, 0, schema, -1, w, 300);
        if (n <= 0) return 0;
        if (!append_w(sql, sql_wchars, &len, w)) return 0;
    }
    if (!append_w(sql, sql_wchars, &len, L"].[")) return 0;
    {
        wchar_t w[300];
        int n = MultiByteToWideChar(CP_UTF8, 0, table, -1, w, 300);
        if (n <= 0) return 0;
        if (!append_w(sql, sql_wchars, &len, w)) return 0;
    }
    if (!append_w(sql, sql_wchars, &len, L"]")) return 0;

    if (plan->nterms > 0 && !append_w(sql, sql_wchars, &len, L" WHERE ")) return 0;
    for (i = 0; i < plan->nterms; i++) {
        const VmsPlanTerm* t = &plan->terms[i];
        if (i && !append_w(sql, sql_wchars, &len, L" AND ")) return 0;
        /* contradiction markers (multi-value IN or empty IN): 1 = 0 */
        if (t->col < 0) {
            if (!append_w(sql, sql_wchars, &len, L"1 = 0")) return 0;
            continue;
        }
        if (!append_col(sql, sql_wchars, &len, cols[t->col].name)) return 0;
        switch (t->op) {
        case VMS_OP_EQ: if (!append_w(sql, sql_wchars, &len, L" = ?")) return 0; break;
        case VMS_OP_LT: if (!append_w(sql, sql_wchars, &len, L" < ?")) return 0; break;
        case VMS_OP_LE: if (!append_w(sql, sql_wchars, &len, L" <= ?")) return 0; break;
        case VMS_OP_GT: if (!append_w(sql, sql_wchars, &len, L" > ?")) return 0; break;
        case VMS_OP_GE: if (!append_w(sql, sql_wchars, &len, L" >= ?")) return 0; break;
        case VMS_OP_ISNULL: if (!append_w(sql, sql_wchars, &len, L" IS NULL")) return 0; break;
        case VMS_OP_ISNOTNULL: if (!append_w(sql, sql_wchars, &len, L" IS NOT NULL")) return 0; break;
        case VMS_OP_IN: if (!append_w(sql, sql_wchars, &len, L" = ?")) return 0; break;
        default: return 0;
        }
    }

    if (plan->norder > 0 && !append_w(sql, sql_wchars, &len, L" ORDER BY ")) return 0;
    for (i = 0; i < plan->norder; i++) {
        if (i && !append_w(sql, sql_wchars, &len, L", ")) return 0;
        if (!append_col(sql, sql_wchars, &len, cols[plan->order_cols[i]].name)) return 0;
        if (plan->order_desc[i] && !append_w(sql, sql_wchars, &len, L" DESC")) return 0;
    }

    if (plan->has_offset && plan->norder > 0) {
        /* OFFSET ... FETCH NEXT requires ORDER BY in T-SQL */
        if (!append_w(sql, sql_wchars, &len, L" OFFSET ? ROWS FETCH NEXT ? ROWS ONLY"))
            return 0;
    } else if (plan->has_limit) {
        if (plan->norder > 0) {
            if (!append_w(sql, sql_wchars, &len,
                          L" OFFSET 0 ROWS FETCH NEXT ? ROWS ONLY"))
                return 0;
        } else {
            if (!append_w(sql, sql_wchars, &len, L" TOP (?)")) return 0;
        }
    }

    *nparams = plan->nargs;
    return len < sql_wchars;
}

