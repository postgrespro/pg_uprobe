#include "postgres.h"

#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "check_bin_change.h"


static bool
isBinDeleated(const char *name)
{
    struct stat sb;
    char        linkname[1024];
    char        fullname[1024];

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
    DIR *dir = opendir("/proc/self/map_files/");
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