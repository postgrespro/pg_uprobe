#include "postgres.h"

#include "check_bin_change.h"

#ifdef __linux__

#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

static bool
isBinDeleated(const char *name)
{
	struct stat sb;
	char		linkname[1024];
	char		fullname[1024];

	sprintf(fullname, "/proc/self/map_files/%s", name);
	if (lstat(fullname, &sb))
		return true;

	Assert(S_ISLNK(sb.st_mode));

	/* It's shared memeory and outher non library files*/
	if (((S_IRUSR | S_IWUSR) & sb.st_mode) == (S_IRUSR | S_IWUSR))
		return false;

	if (readlink(fullname, linkname, 1024) < 0)
		return true;

	/* If link's name ends with "(deleted)" then the original file is deleted*/
	return strstr(linkname, "(deleted)") != NULL;
}


bool 
binChange(void)
{
	DIR 		*dir = opendir("/proc/self/map_files/");
	struct dirent *item;

	if (dir == NULL)
		return true;

	while ((item = readdir(dir)))
	{
		if ((item->d_type == DT_LNK || item->d_type == DT_UNKNOWN) && 
			isBinDeleated(item->d_name))
		{
			closedir(dir);
			return true;
		}
	}
	closedir(dir);
	return false;
}


#elifdef __FreeBSD__
#include <unistd.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/user.h>
#include <libprocstat.h>
#include <sys/sysctl.h>
#include <sys/stat.h>


static bool
checkOneMapping(struct kinfo_vmentry *vm)
{
	struct stat	sb;
	int			res;

	if (vm->kve_type != KVME_TYPE_VNODE)
		return false;

	/* We can't stat the file, so it must be deleated */
	res = stat(vm->kve_path, &sb);
	if (res < 0)
		return true;

	if (sb.st_ino != vm->kve_vn_fileid)
		return true;
	
	return false;
}

bool
binChange(void)
{
	struct procstat *pstat = procstat_open_sysctl();
	unsigned int nproc;
	unsigned int nvms;
	struct kinfo_proc *mproc;
	struct kinfo_vmentry *vms;
	bool		result = false;

	if (pstat == NULL)
		return true;

	mproc = procstat_getprocs(pstat, KERN_PROC_PID, getpid(), &nproc);

	if (mproc == NULL)
	{
		procstat_close(pstat);
		return true;
	}

	/* Simply use first proc in case we have theaads */
	vms = procstat_getvmmap(pstat, mproc, &nvms);

	if (vms == NULL)
	{
		procstat_freeprocs(pstat, mproc);
		procstat_close(pstat);
		return true;
	}

	for (unsigned int i = 0; i < nvms; i++)
	{
		if (checkOneMapping(&vms[i]))
		{
			result = true;
			break;
		}
	}
	
	procstat_freevmmap(pstat, vms);
	procstat_freeprocs(pstat, mproc);
	procstat_close(pstat);
	return result;
}


#else
/* On outher platforms we can't determine binary change, at least for now*/
bool
binChange(void)
{
	return false;
}

#endif