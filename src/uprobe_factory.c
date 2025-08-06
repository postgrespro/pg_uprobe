#include "postgres.h"

#include "count_uprobes.h"
#include "lockmanager_trace.h"

#include "uprobe_factory.h"

#define UNVALIDTYPESTR "INVALID_TYPE"


typedef struct UprobeFactoryEntry
{
	StorageInitFunc storageInit;
	UprobeInterfaceInitFunc interfaceInit;
	UprobeAttachType type;
	char	   *stringtype;
} UprobeFactoryEntry;


UprobeFactoryEntry staticEntries[] = {
	{
		.interfaceInit = UprobeAttachTimeInit,
		.storageInit = UprobeStorageTimeInit,
		.type = TIME,
		.stringtype = "TIME"
	},
	{
		.interfaceInit = UprobeAttachHistInit,
		.storageInit = UprobeStorageHistInit,
		.type = HIST,
		.stringtype = "HIST"
	},
	{
		.interfaceInit = UprobeAttachMemInit,
		.storageInit = UprobeStorageMemInit,
		.type = MEM,
		.stringtype = "MEM"
	},
	{
		.interfaceInit = LWLockAcquireInit,
		.storageInit = NullStorageInit,
		.type = LOCK_ACQUIRE,
		.stringtype = "LOCK_ACQUIRE"
	},
	{
		.interfaceInit = LWLockReleaseInit,
		.storageInit = LockManagerStorageInit,
		.type = LOCK_RELEASE,
		.stringtype = "LOCK_RELEASE"
	}
};

#define STATIC_ENTRIES_SIZE sizeof(staticEntries) / sizeof(staticEntries[0])


void
CreateUprobeAttachForType(const char *type, const char *symbol, UprobeAttach *uprobeAttach)
{

	for (size_t i = 0; i < STATIC_ENTRIES_SIZE; i++)
	{
		if (strcmp(type, staticEntries[i].stringtype) == 0)
		{
			uprobeAttach->type = staticEntries[i].type;
			uprobeAttach->impl = staticEntries[i].interfaceInit(symbol);
			return;
		}
	}

	uprobeAttach->type = INVALID_TYPE;
	uprobeAttach->impl = NULL;
}


const char *
GetCharNameForUprobeAttachType(UprobeAttachType type)
{
	for (size_t i = 0; i < STATIC_ENTRIES_SIZE; i++)
	{
		if (type == staticEntries[i].type)
			return staticEntries[i].stringtype;
	}

	return UNVALIDTYPESTR;
}


UprobeStorage *
GetUprobeStorageForType(UprobeAttachType type, const char *symbol)
{

	for (size_t i = 0; i < STATIC_ENTRIES_SIZE; i++)
	{
		if (type == staticEntries[i].type)
			return staticEntries[i].storageInit(symbol);
	}

	return NULL;
}


UprobeAttachType
GetTypeByCharName(const char *name)
{
	for (size_t i = 0; i < STATIC_ENTRIES_SIZE; i++)
	{
		if (strcmp(name, staticEntries[i].stringtype) == 0)
			return staticEntries[i].type;
	}

	return INVALID_TYPE;
}
