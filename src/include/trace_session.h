#ifndef TRACE_SESSION_H
#define TRACE_SESSION_H
#include "postgres.h"
#include "executor/executor.h"

typedef enum TraceDataWriteMode
{
    TEXT_WRITE_MODE,
    JSON_WRITE_MODE
} TraceDataWriteMode;

extern int writeMode;
extern bool isExecuteTime;

extern void SessionTraceStart(void);

extern void SessionTraceStop(void);

extern void SessionTraceInit(void);

#endif