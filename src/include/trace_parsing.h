#ifndef TRACE_PARSING_H
#define TRACE_PARSING_H

#include "uprobe_attach_interface.h"

extern UprobeAttachInterface *ParsingUprobeGet(void);

extern void ParsingWriteData(void);

extern void ParsingClearData(void);

#endif							/* TRACE_PARSING_H */
