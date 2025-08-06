#include "postgres.h"
#include "utils/wait_event.h"
#include "storage/shmem.h"
#include "storage/lwlock.h"
#include "storage/condition_variable.h"

#include "uprobe_message_buffer.h"

#define MESSAGE_BUFFER_EMPTY_SPACE_WAIT (ESTIMATE_MESSAGE_SIZE * 64)



typedef struct MessageBuffer
{
	LWLock	   *lock;
	ConditionVariable cond;
	pg_atomic_uint32 numMessages;
	pg_atomic_uint32 freeSpace;
	volatile bool deleted;
	Message		messages[FLEXIBLE_ARRAY_MEMBER];
} MessageBuffer;


MessageBuffer *messageBuffer;


static bool WaitUntillMessageBufferHasSpace(void);
static bool WaitUntilMessageBufferNotEmpty(void);
static void EndMessageBufferOperation(uint32 numMessages, uint32 freeSpace);

void
MessageBufferRequest(void)
{
	RequestAddinShmemSpace(MAXALIGN(sizeof(MessageBuffer) + MESSAGEBUFFER_SIZE));
	RequestNamedLWLockTranche("pg_uprobe collect lock", 1);
}


void
MessageBufferInit(void)
{
	bool		found;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	messageBuffer = ShmemInitStruct("pg_uprobe messageBuffer", sizeof(MessageBuffer) + MESSAGEBUFFER_SIZE,
									&found);
	LWLockRelease(AddinShmemInitLock);
	if (!found)
	{
		pg_atomic_write_u32(&messageBuffer->numMessages, 0);
		ConditionVariableInit(&messageBuffer->cond);
		messageBuffer->deleted = false;
		messageBuffer->lock = &(GetNamedLWLockTranche("pg_uprobe collect lock"))->lock;
		pg_atomic_write_u32(&messageBuffer->freeSpace, MESSAGEBUFFER_SIZE);
	}

}

static void
EndMessageBufferOperation(uint32 numMessages, uint32 freeSpace)
{
	pg_atomic_write_u32(&messageBuffer->numMessages, numMessages);
	pg_atomic_write_u32(&messageBuffer->freeSpace, freeSpace);

	LWLockRelease(messageBuffer->lock);
	ConditionVariableBroadcast(&messageBuffer->cond);
}

/*
 *If returns true messageBuffer->lock is acquired in LW_EXCLUSIVE
 *caller must call EndMessageBufferOperation to release it.
 *Else no lock is acquired and message buffer is deleted.
*/
static bool
WaitUntillMessageBufferHasSpace(void)
{
	uint32		freeSpaceupdated;
	bool		isLockTaken = false;

	ConditionVariablePrepareToSleep(&messageBuffer->cond);
	freeSpaceupdated = pg_atomic_read_u32(&messageBuffer->freeSpace);
	do
	{
		if (freeSpaceupdated < MESSAGE_BUFFER_EMPTY_SPACE_WAIT && !messageBuffer->deleted)
		{
			ConditionVariableSleep(&messageBuffer->cond, WAIT_EVENT_PG_SLEEP);
			freeSpaceupdated = pg_atomic_read_u32(&messageBuffer->freeSpace);
		}
		LWLockAcquire(messageBuffer->lock, LW_EXCLUSIVE);
		freeSpaceupdated = pg_atomic_read_u32(&messageBuffer->freeSpace);

		if (freeSpaceupdated > MESSAGE_BUFFER_EMPTY_SPACE_WAIT)
		{
			isLockTaken = true;
			break;
		}

		LWLockRelease(messageBuffer->lock);
	} while (!messageBuffer->deleted);
	ConditionVariableCancelSleep();
	return isLockTaken;
}

int
MessageBufferPut(const Message *mes, uint32 n, char *symbol)
{
	uint32		numToStore = 0;
	uint32		bytesToStore = 0;
	uint32		freeSpace;
	uint32		numInBuffer;
	const Message *currentMessage = mes;
	Message    *lastMessageInbuffer;
	size_t		symbolLen;

	if (messageBuffer->deleted || n == 0)
		return n;

	if (!WaitUntillMessageBufferHasSpace())
		return n;

	freeSpace = pg_atomic_read_u32(&messageBuffer->freeSpace);
	lastMessageInbuffer = (Message *) (((char *) messageBuffer->messages) + (MESSAGEBUFFER_SIZE - freeSpace));
	numInBuffer = pg_atomic_read_u32(&messageBuffer->numMessages);

	symbolLen = strlen(symbol);
	lastMessageInbuffer->type = MESSAGE_SYMBOL;
	lastMessageInbuffer->size = (uint16) symbolLen + sizeof(Message);
	/* + 1 is used to skip type and size fields in struct */
	memcpy(lastMessageInbuffer + 1, symbol, lastMessageInbuffer->size);

	numInBuffer++;
	freeSpace -= lastMessageInbuffer->size;

	lastMessageInbuffer = (Message *) (((char *) lastMessageInbuffer) + lastMessageInbuffer->size);

	while (numToStore < n)
	{
		uint32		mesSize = (uint32) currentMessage->size;

		if (bytesToStore + mesSize > freeSpace)
			break;
		bytesToStore += mesSize;
		currentMessage = (const Message *) (((char *) mes) + bytesToStore);
		numToStore++;
	}

	memcpy(lastMessageInbuffer, mes, bytesToStore);

	EndMessageBufferOperation(numToStore + numInBuffer, freeSpace - bytesToStore);

	return numToStore;
}


/*
 *If returns true messageBuffer->lock is acquired in LW_EXCLUSIVE
 *caller must call EndMessageBufferOperation to release it.
 *Else no lock is acquired and message buffer is deleted.
*/
static bool
WaitUntilMessageBufferNotEmpty(void)
{
	uint32		numMessagesUpdated;
	bool		isLockTaken = false;

	ConditionVariablePrepareToSleep(&messageBuffer->cond);
	numMessagesUpdated = pg_atomic_read_u32(&messageBuffer->numMessages);
	do
	{
		if (!numMessagesUpdated && !messageBuffer->deleted)
		{
			ConditionVariableSleep(&messageBuffer->cond, WAIT_EVENT_PG_SLEEP);
			numMessagesUpdated = pg_atomic_read_u32(&messageBuffer->numMessages);
		}
		LWLockAcquire(messageBuffer->lock, LW_EXCLUSIVE);
		numMessagesUpdated = pg_atomic_read_u32(&messageBuffer->numMessages);

		if (numMessagesUpdated)
		{
			isLockTaken = true;
			break;
		}

		LWLockRelease(messageBuffer->lock);
	} while (!messageBuffer->deleted);
	ConditionVariableCancelSleep();
	return isLockTaken;
}

/*
 *getting messages from messageBuffer with blocking if threre is no messages
*/
int
MessageBufferGet(Message *mes, uint32 bufferSize)
{
	uint32		numToGet = 0;
	uint32		numInTheBuffer;
	uint32		bytesToGet = 0;
	Message    *currentMessage = messageBuffer->messages;
	uint32		freeSpace;

	if (messageBuffer->deleted)
		return 0;

	if (!WaitUntilMessageBufferNotEmpty())
		return 0;

	numInTheBuffer = pg_atomic_read_u32(&messageBuffer->numMessages);
	freeSpace = pg_atomic_read_u32(&messageBuffer->freeSpace);
	while (numToGet < numInTheBuffer)
	{
		uint32		mesSize = (uint32) currentMessage->size;

		if (bytesToGet + mesSize > bufferSize)
			break;

		bytesToGet += mesSize;
		currentMessage = (Message *) (((char *) currentMessage) + mesSize);
		numToGet++;
	}

	memcpy(mes, messageBuffer->messages, bytesToGet);
	memmove(messageBuffer->messages,
			((char *) messageBuffer->messages) + bytesToGet,
			MESSAGEBUFFER_SIZE - freeSpace - bytesToGet);

	EndMessageBufferOperation(numInTheBuffer - numToGet, freeSpace + bytesToGet);
	return numToGet;
}

/* We don't actualy delete MessageBuffer. We make all operations finish as soon as they get in */
void
MessageBufferDelete()
{
	messageBuffer->deleted = true;
	ConditionVariableBroadcast(&messageBuffer->cond);
}
