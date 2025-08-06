#ifndef UPROBE_ATTACH_INTERFACE_H
#define UPROBE_ATTACH_INTERFACE_H
#include "postgres.h"

/*
 * Interface for attaching urpobes.
 *
 * inFunc will be called when target symbol  is called.
 *      prototype for it is void inFunc(void* data, size_t arg1, size_t arg2);
 *      you can add up to 8 args and you must specify amount in field numArgs
 * retFunc will be called when target symbol returns.
 *      prototype for it is void retFunc(void* data, size_t retArg);
 *      retArg will be passed if filed needRetVal is true.
 * timedCallback could be called from time to time. Common usage is sending data on shared uprobes
 * cleanFunc will be called when this uprobe is deleted at this process.
*/

struct UprobeAttachInterface;

typedef void (*UprobeAttachCleanFunc) (struct UprobeAttachInterface *);

typedef void (*UprobeAttachTimedFunc) (struct UprobeAttachInterface *);

typedef struct UprobeAttachInterface
{
	void	   *inFunc;
	void	   *retFunc;
	UprobeAttachTimedFunc timedCallback;
	UprobeAttachCleanFunc cleanFunc;
	void	   *data;
	char	   *targetSymbol;
	int			numArgs;
	bool		needRetVal;
} UprobeAttachInterface;

#endif							/* UPROBE_ATTACH_INTERFACE_H */
