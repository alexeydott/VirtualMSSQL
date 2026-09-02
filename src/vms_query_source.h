/* vms_query_source.h — source=query vtab support (R8). */
#ifndef VIRTUALMSSQL_VMS_QUERY_SOURCE_H
#define VIRTUALMSSQL_VMS_QUERY_SOURCE_H

#include "vms_client.h"
#include "vms_meta.h"
#include "vms_lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VmsQuerySource {
    wchar_t* query;               /* validated remote query (UTF-16) */
    VmsMetaColumn cols[512];      /* described first result set shape */
    int ncols;
} VmsQuerySource;

/* prepare the source for scanning: the validated query is used as-is
 * (no outer wrapper: T-SQL forbids WITH inside derived tables, and the
 * validator already guarantees a single read-only row-producing SELECT).
 * The shape from sp/dm_exec describe is authoritative for xConnect. */
int vms_query_source_prepare(VmsConnection* cn, const char* query_utf8,
                             VmsQuerySource* src, VmsError* err);

/* copy the raw (validated) query for cursor execution */
int vms_query_source_get_sql(const VmsQuerySource* src,
                             wchar_t* out, size_t out_wchars);

void vms_query_source_free(VmsQuerySource* src);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_QUERY_SOURCE_H */
