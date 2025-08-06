#ifndef COUNT_UPROBES_H
#define COUNT_UPROBES_H

#include "uprobe_attach_interface.h"
#include "custom_uprobe_interface.h"

/*  works like hist() in bpftrace */
typedef struct
{
	size_t	   *histArray;
	size_t		histArraySize;
	size_t		totalCalls;
	double		start;
	double		stop;
	double		step;
} UprobeAttachHistStat;


typedef struct
{
	size_t		numCalls;
	size_t		avgTime;
} UprobeAttachTimeStat;


extern UprobeAttachInterface *UprobeAttachTimeInit(const char *symbol);
extern UprobeStorage *UprobeStorageTimeInit(const char *symbol);
extern UprobeAttachTimeStat *UprobeAttachTimeGetStat(const UprobeAttachInterface *uprobe);

extern UprobeAttachInterface *UprobeAttachHistInit(const char *symbol);
extern UprobeStorage *UprobeStorageHistInit(const char *symbol);
extern UprobeAttachHistStat *UprobeAttachHistGetStat(const UprobeAttachInterface *uprobe, double start, double stop, double step);
extern UprobeAttachHistStat *UprobeAttachHistGetStatSimple(const UprobeAttachInterface *uprobe);

extern UprobeAttachInterface *UprobeAttachMemInit(const char *symbol);
extern UprobeStorage *UprobeStorageMemInit(const char *symbol);

#endif							/* COUNT_UPROBES_H */
