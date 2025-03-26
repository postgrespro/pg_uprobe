#ifndef TRACE_FILE_H
#define TRACE_FILE_H
#include "postgres.h"

extern FILE* traceFile;

extern char *dataDir;

extern int traceFileLimit;

extern size_t currentSize;

extern void TraceFileDeclareGucVariables(void);

extern bool OpenTraceSessionFile(bool throwOnError);

extern void CloseTraceSessionFile(void);

#define TRACE_FILE_LIMIT_BT 1024 * 1024 * (size_t) traceFileLimit

pg_attribute_printf(1, 2) static inline int TracePrintf(char* fmt, ...)
{
    if (TRACE_FILE_LIMIT_BT > currentSize)
    {
        va_list		args;

	    va_start(args, fmt);
        /*
         * We estimate that sizes of written data is not that big,
         * so we won't need to worry alot about going beyond TRACE_FILE_LIMIT_BT.
	    */
        currentSize += pg_vfprintf(traceFile, fmt, args);
	    va_end(args);
    }
    return 0;
}


#endif