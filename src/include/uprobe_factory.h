#ifndef UPROBE_FACTORY_H
#define UPROBE_FACTORY_H

#include "uprobe_attach_interface.h"
#include "custom_uprobe_interface.h"

typedef enum
{
	INVALID_TYPE,
	TIME, HIST, MEM, LOCK_ACQUIRE, LOCK_RELEASE
} UprobeAttachType;


typedef struct UprobeAttach
{
	UprobeAttachType type;
	UprobeAttachInterface *impl;
} UprobeAttach;


typedef struct UprobeStorage *(*StorageInitFunc) (const char *symbol);

typedef struct UprobeAttachInterface *(*UprobeInterfaceInitFunc) (const char *symbol);


extern void CreateUprobeAttachForType(const char *type, const char *symbol, UprobeAttach *UprobeAttach);

extern const char *GetCharNameForUprobeAttachType(UprobeAttachType type);

extern UprobeStorage *GetUprobeStorageForType(UprobeAttachType type, const char *symbol);

extern UprobeAttachType GetTypeByCharName(const char *name);

#endif							/* UPROBE_FACTORY_H */
