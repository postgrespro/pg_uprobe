#ifndef TRACE_WAIT_EVENTS_H
#define TRACE_WAIT_EVENTS_H

#include "postgres.h"
#include "lib/stringinfo.h"

#include "uprobe_attach_interface.h"


extern UprobeAttachInterface* TraceWaitEventsUprobesGet(size_t* resSize);

extern void TraceWaitEventsUprobesClean(void);

extern bool TraceWaitEventDumpStat(StringInfo out);

extern void TraceWaitEventsClearStat(void);

extern void SignalWaitEventStart(uint64 time);

extern void SignalWaitEventEnd(uint64 time);

#endif