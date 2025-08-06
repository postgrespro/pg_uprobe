#ifndef LOCKMANAGER_TRACE_H
#define LOCKMANAGER_TRACE_H

#include "custom_uprobe_interface.h"


extern UprobeAttachInterface *LWLockAcquireInit(const char *symbol);

extern UprobeAttachInterface *LWLockReleaseInit(const char *symbol);

extern UprobeStorage *LockManagerStorageInit(const char *symbol);

extern UprobeStorage *NullStorageInit(const char *symbol);

#endif							/* LOCKMANAGER_TRACE_H */
