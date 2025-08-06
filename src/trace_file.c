#include "postgres.h"
#include <sys/stat.h>
#include "utils/guc.h"
#include "miscadmin.h"

#include "trace_file.h"

static char *traceFileName = NULL;
char	   *dataDir = NULL;

/*limit in megabytes */
int			traceFileLimit = 16;
size_t		currentSize = 0;
FILE	   *traceFile = NULL;

static bool CheckDataDirValue(char **newval, void **extra, GucSource source);


void
TraceFileDeclareGucVariables(void)
{
	DefineCustomIntVariable("pg_uprobe.trace_file_limit",
							"trace session data limit in megabytes",
							NULL,
							&traceFileLimit,
							16,
							1,
							32 * 1024,
							PGC_SUSET,
							GUC_UNIT_MB,
							NULL,
							NULL,
							NULL);


	DefineCustomStringVariable("pg_uprobe.trace_file_name",
							   "file name for trace session data",
							   NULL,
							   &traceFileName,
							   "trace_file.txt",
							   PGC_SUSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("pg_uprobe.data_dir",
							   "dir for file pg_uprobe.trace_file_name and other pg_uprobe files",
							   NULL,
							   &dataDir,
							   "./pg_uprobe/",
							   PGC_SUSET,
							   0,
							   CheckDataDirValue, NULL, NULL);
}


bool
OpenTraceSessionFile(bool throwOnError)
{
	char		path[4096];
	int			error;

	sprintf(path, "%s/%s_%d", dataDir, traceFileName, MyProcPid);

	traceFile = fopen(path, "w");
	error = errno;

	if (traceFile == NULL && throwOnError)
		elog(ERROR, "can't open file %s for trace session data: %s", path, strerror(error));
	else if (traceFile == NULL)
		elog(LOG, "can't open file %s for trace session data: %s", path, strerror(error));

	currentSize = 0;
	return traceFile != NULL;
}


void
CloseTraceSessionFile(void)
{
	if (traceFile == NULL)
		return;

	fclose(traceFile);
	traceFile = NULL;
	currentSize = 0;
}


static bool
CheckDataDirValue(char **newval, void **extra, GucSource source)
{
	struct stat st;
	size_t		size;


	if (*newval == NULL || *newval[0] == '\0')
	{
		GUC_check_errdetail("pg_uprobe data can't be empty or null");
		return false;
	}

	size = strlen(*newval);

	if (size + 128 >= MAXPGPATH)
	{
		GUC_check_errdetail("pg_uprobe data dir too long.");
		return false;
	}

	if (stat(*newval, &st) != 0 || !S_ISDIR(st.st_mode))
	{
		GUC_check_errdetail("Specified pg_uprobe data dir does not exist.");
		return false;
	}

	return true;
}
