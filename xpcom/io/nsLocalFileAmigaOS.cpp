/* ***** BEGIN LICENSE BLOCK *****
 *
 * The contents of this file is copyrighted by Thomas and Hans-Joerg Frieden.
 * It's content is not open source and may not be redistributed, modified or adapted
 * without permission of the above-mentioned copyright holders.
 *
 * Since this code was originally developed under an AmigaOS related bounty, any derived
 * version of this file may only be used on an official AmigaOS system.
 *
 * Contributor(s):
 * 	Thomas Frieden <thomas@friedenhq.org>
 * 	Hans-Joerg Frieden <hans-joerg@friedenhq.org>
 *
 * ***** END LICENSE BLOCK ***** */


/**
 * Implementation of nsIFile for AmigaOS systems.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <utime.h>
#include <dirent.h>
#include <ctype.h>
#include <locale.h>


#include "nsDirectoryServiceDefs.h"
#include "nsCRT.h"
#include "nsCOMPtr.h"
#include "nsMemory.h"
#include "nsIFile.h"
#include "nsString.h"
#include "nsReadableUtils.h"
#include "nsLocalFile.h"
#include "nsIComponentManager.h"
#include "nsXPIDLString.h"
#include "prproces.h"
#include "nsIDirectoryEnumerator.h"
#include "nsISimpleEnumerator.h"
#include "nsITimelineService.h"


#include "nsNativeCharsetUtils.h"
#include "nsTraceRefcntImpl.h"

#include <proto/wb.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>


extern struct DOSIFace *IDOS;

//#define DEBUG_NSIFILE

// On some platforms file/directory name comparisons need to
// be case-blind.
#if defined(VMS)
    #define FILE_STRCMP strcasecmp
    #define FILE_STRNCMP strncasecmp
#else
    #define FILE_STRCMP strcmp
    #define FILE_STRNCMP strncmp
#endif

#define ENSURE_STAT_CACHE()                     \
    PR_BEGIN_MACRO                              \
        if (!FillStatCache())                   \
             return NSRESULT_FOR_ERRNO();       \
    PR_END_MACRO

#define CHECK_mPath()                           \
    PR_BEGIN_MACRO                              \
        if (mPath.IsEmpty())                    \
            return NS_ERROR_NOT_INITIALIZED;    \
    PR_END_MACRO

/* directory enumerator */
class NS_COM
nsDirEnumeratorAmigaOS : public nsISimpleEnumerator,
                      public nsIDirectoryEnumerator
{
    public:
    nsDirEnumeratorAmigaOS();

    // nsISupports interface
    NS_DECL_ISUPPORTS

    // nsISimpleEnumerator interface
    NS_DECL_NSISIMPLEENUMERATOR

    // nsIDirectoryEnumerator interface
    NS_DECL_NSIDIRECTORYENUMERATOR

    NS_IMETHOD Init(nsLocalFile *parent, PRBool ignored);

    private:
    ~nsDirEnumeratorAmigaOS();

    protected:
    NS_IMETHOD GetNextEntry();

    DIR           *mDir;
    struct dirent *mEntry;
    nsCString      mParentPath;
};

nsDirEnumeratorAmigaOS::nsDirEnumeratorAmigaOS() :
                         mDir(nsnull),
                         mEntry(nsnull)
{
}

nsDirEnumeratorAmigaOS::~nsDirEnumeratorAmigaOS()
{
    Close();
}

NS_IMPL_ISUPPORTS2(nsDirEnumeratorAmigaOS, nsISimpleEnumerator, nsIDirectoryEnumerator)

NS_IMETHODIMP
nsDirEnumeratorAmigaOS::Init(nsLocalFile *parent, PRBool resolveSymlinks /*ignored*/)
{
    nsCAutoString dirPath;
    if (NS_FAILED(parent->GetNativePath(dirPath)) ||
        dirPath.IsEmpty()) {
        return NS_ERROR_FILE_INVALID_PATH;
    }

    if (NS_FAILED(parent->GetNativePath(mParentPath)))
        return NS_ERROR_FAILURE;

    mDir = opendir(dirPath.get());
    if (!mDir)
        return NSRESULT_FOR_ERRNO();
    return GetNextEntry();
}

NS_IMETHODIMP
nsDirEnumeratorAmigaOS::HasMoreElements(PRBool *result)
{
    *result = mDir && mEntry;
    if (!*result)
        Close();
    return NS_OK;
}

NS_IMETHODIMP
nsDirEnumeratorAmigaOS::GetNext(nsISupports **_retval)
{
    nsCOMPtr<nsIFile> file;
    nsresult rv = GetNextFile(getter_AddRefs(file));
    if (NS_FAILED(rv))
        return rv;
    NS_IF_ADDREF(*_retval = file);
    return NS_OK;
}

NS_IMETHODIMP
nsDirEnumeratorAmigaOS::GetNextEntry()
{
    do {
        errno = 0;
        mEntry = readdir(mDir);

        // end of dir or error
        if (!mEntry)
            return NSRESULT_FOR_ERRNO();

        // keep going past "." and ".."
    } while (mEntry->d_name[0] == '.'     &&
            (mEntry->d_name[1] == '\0'    ||   // .\0
            (mEntry->d_name[1] == '.'     &&
            mEntry->d_name[2] == '\0')));      // ..\0
    return NS_OK;
}

NS_IMETHODIMP
nsDirEnumeratorAmigaOS::GetNextFile(nsIFile **_retval)
{
    nsresult rv;
    if (!mDir || !mEntry) {
        *_retval = nsnull;
        return NS_OK;
    }

    nsCOMPtr<nsILocalFile> file = new nsLocalFile();
    if (!file)
        return NS_ERROR_OUT_OF_MEMORY;

    if (NS_FAILED(rv = file->InitWithNativePath(mParentPath)) ||
        NS_FAILED(rv = file->AppendNative(nsDependentCString(mEntry->d_name))))
        return rv;

    *_retval = file;
    NS_ADDREF(*_retval);
    return GetNextEntry();
}

NS_IMETHODIMP
nsDirEnumeratorAmigaOS::Close()
{
    if (mDir) {
        closedir(mDir);
        mDir = nsnull;
    }
    return NS_OK;
}

nsLocalFile::nsLocalFile()
{
}

nsLocalFile::nsLocalFile(const nsLocalFile& other)
  : mPath(other.mPath)
{
}

NS_IMPL_THREADSAFE_ISUPPORTS3(nsLocalFile,
                              nsIFile,
                              nsILocalFile,
                              nsIHashable)

nsresult
nsLocalFile::nsLocalFileConstructor(nsISupports *outer,
                                    const nsIID &aIID,
                                    void **aInstancePtr)
{
    NS_ENSURE_ARG_POINTER(aInstancePtr);
    NS_ENSURE_NO_AGGREGATION(outer);

    *aInstancePtr = nsnull;

    nsCOMPtr<nsIFile> inst = new nsLocalFile();
    if (!inst)
        return NS_ERROR_OUT_OF_MEMORY;
    return inst->QueryInterface(aIID, aInstancePtr);
}

PRBool
nsLocalFile::FillStatCache() {
#ifdef HAVE_STAT64
    if (stat64(mPath.get(), &mCachedStat) == -1) {
        // try lstat it may be a symlink
        if (lstat64(mPath.get(), &mCachedStat) == -1) {
            return PR_FALSE;
        }
    }
#else
    if (stat(mPath.get(), &mCachedStat) == -1) {
        // try lstat it may be a symlink
        if (lstat(mPath.get(), &mCachedStat) == -1) {
            return PR_FALSE;
        }
    }
#endif
    return PR_TRUE;
}

NS_IMETHODIMP
nsLocalFile::Clone(nsIFile **file)
{
    // Just copy-construct ourselves
    *file = new nsLocalFile(*this);
    if (!*file)
      return NS_ERROR_OUT_OF_MEMORY;

    NS_ADDREF(*file);

    return NS_OK;
}

void
nsLocalFile::MakeAmiga(void)
{
	nsCString newPath;
    char *buffer = mPath.BeginWriting();
    char *slashp = buffer;
    char *colonP = strchr(buffer, ':');
    bool absolute = (*buffer == '/');
    bool gotColon = false;

    if (colonP)
    {
    	/* Assume the path is already an Amiga style path
    	 * Need to check for :/ constructs, though
    	 */
    	if (colonP[1] == '/')
    	{
    		while (*buffer != 0)
    		{
    			newPath += *buffer;
    			if (buffer == colonP)
    				buffer++;
    			buffer++;
    		}

    		mPath = newPath;
    	}
    	return;
    }

    if (absolute)
    	buffer ++;

    while ((slashp = strchr(slashp + 1, '/')))
    {
    	// Terminate the string here
    	*slashp = 0;

    	if (absolute)
    	{
    		newPath += buffer;
    		newPath += ":";
    		absolute = false;
    		gotColon = true;
    	}
    	else
    	{
    		newPath += buffer;
    		newPath += "/";
    	}

#ifdef DEBUG_NSIFILE
    	fprintf(stderr, "nsIFile: MakeAmiga: %s\n", newPath.get());
#endif
    	*slashp = '/';
    }

    if (!gotColon)
    	newPath += ":";

    mPath = newPath;
}


NS_IMETHODIMP
nsLocalFile::InitWithNativePath(const nsACString &filePath)
{
    if (Substring(filePath, 0, 2).EqualsLiteral("~/")) {
        nsCOMPtr<nsIFile> homeDir;
        nsCAutoString homePath;
        if (NS_FAILED(NS_GetSpecialDirectory(NS_OS_HOME_DIR,
                                             getter_AddRefs(homeDir))) ||
            NS_FAILED(homeDir->GetNativePath(homePath))) {
            return NS_ERROR_FAILURE;
        }

        mPath = homePath + Substring(filePath, 1, filePath.Length() - 1);
    } else {
        if (filePath.IsEmpty())
            return NS_ERROR_FILE_UNRECOGNIZED_PATH;
        mPath = filePath;
    }

#ifdef DEBUG_NSIFILE
    	fprintf(stderr, "InitWithNativePath: %s\n", mPath.get());
//    	if (strcmp(mPath.get(), "work:/Applications/") == 0) __asm volatile ("trap");
#endif


    // trim off trailing slashes
    ssize_t len = mPath.Length();
    while ((len > 1) && (mPath[len - 1] == '/'))
        --len;
    mPath.SetLength(len);

#ifdef DEBUG_NSIFILE
    	fprintf(stderr,"InitWithNativePath: %s\n", mPath.get());
#endif
    MakeAmiga();

#ifdef DEBUG_NSIFILE
    	fprintf(stderr,"InitWithNativePath: %s\nDone\n", mPath.get());
#endif
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::CreateAllAncestors(PRUint32 permissions)
{
#warning Amiga-style path handling needed
    // <jband> I promise to play nice
    char *buffer = mPath.BeginWriting(),
         *slashp = buffer;

#ifdef DEBUG_NSIFILE
    fprintf(stderr, "nsIFile: before: %s\n", buffer);
#endif

    while ((slashp = strchr(slashp + 1, '/'))) {
        /*
         * Sequences of '/' are equivalent to a single '/'.
         */
        if (slashp[1] == '/')
            continue;

        /*
         * If the path has a trailing slash, don't make the last component,
         * because we'll get EEXIST in Create when we try to build the final
         * component again, and it's easier to condition the logic here than
         * there.
         */
        if (slashp[1] == '\0')
            break;

        /* Temporarily NUL-terminate here */
        *slashp = '\0';
#ifdef DEBUG_NSIFILE
        fprintf(stderr, "nsIFile: mkdir(\"%s\")\n", buffer);
#endif
        int mkdir_result = mkdir(buffer, permissions);
        int mkdir_errno  = errno;
        if (mkdir_result == -1) {
            /*
             * Always set |errno| to EEXIST if the dir already exists
             * (we have to do this here since the errno value is not consistent
             * in all cases - various reasons like different platform,
             * automounter-controlled dir, etc. can affect it (see bug 125489
             * for details)).
             */
            if (access(buffer, F_OK) == 0) {
                mkdir_errno = EEXIST;
            }
        }

        /* Put the / back before we (maybe) return */
        *slashp = '/';

        /*
         * We could get EEXIST for an existing file -- not directory --
         * with the name of one of our ancestors, but that's OK: we'll get
         * ENOTDIR when we try to make the next component in the path,
         * either here on back in Create, and error out appropriately.
         */
        if (mkdir_result == -1 && mkdir_errno != EEXIST)
            return nsresultForErrno(mkdir_errno);
    }

#ifdef DEBUG_NSIFILE
    fprintf(stderr, "nsIFile: after: %s\n", buffer);
#endif

    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::OpenNSPRFileDesc(PRInt32 flags, PRInt32 mode, PRFileDesc **_retval)
{
#ifdef DEBUG_NSIFILE
    fprintf(stderr, "OpenNSPRFileDesc: %s\n", mPath.get());
#endif

    *_retval = PR_Open(mPath.get(), flags, mode);
    if (! *_retval)
        return NS_ErrorAccordingToNSPR();

    if (flags & DELETE_ON_CLOSE) {
        PR_Delete(mPath.get());
    }

    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::OpenANSIFileDesc(const char *mode, FILE **_retval)
{
#ifdef DEBUG_NSIFILE
    	fprintf(stderr, "OpenANSIFileDesc: %s\n", mPath.get());
#endif
    *_retval = fopen(mPath.get(), mode);
    if (! *_retval)
        return NS_ERROR_FAILURE;

    return NS_OK;
}

static int
do_create(const char *path, PRIntn flags, mode_t mode, PRFileDesc **_retval)
{
#ifdef DEBUG_NSIFILE
  	fprintf(stderr, "do_create: %s\n", path);
#endif
    *_retval = PR_Open(path, flags, mode);
    return *_retval ? 0 : -1;
}

static int
do_mkdir(const char *path, PRIntn flags, mode_t mode, PRFileDesc **_retval)
{
    *_retval = nsnull;
    return mkdir(path, mode);
}

nsresult
nsLocalFile::CreateAndKeepOpen(PRUint32 type, PRIntn flags,
                               PRUint32 permissions, PRFileDesc **_retval)
{
#ifdef DEBUG_NSIFILE
    fprintf(stderr, "CreateAndKeepOpen: %s\n", mPath.get());
#endif

    if (type != NORMAL_FILE_TYPE && type != DIRECTORY_TYPE)
        return NS_ERROR_FILE_UNKNOWN_TYPE;

    int result;
    int (*createFunc)(const char *, PRIntn, mode_t, PRFileDesc **) =
        (type == NORMAL_FILE_TYPE) ? do_create : do_mkdir;

    result = createFunc(mPath.get(), flags, permissions, _retval);
    if (result == -1 && errno == ENOENT) {
        /*
         * If we failed because of missing ancestor components, try to create
         * them and then retry the original creation.
         *
         * Ancestor directories get the same permissions as the file we're
         * creating, with the X bit set for each of (user,group,other) with
         * an R bit in the original permissions.    If you want to do anything
         * fancy like setgid or sticky bits, do it by hand.
         */
        int dirperm = permissions;
        if (permissions & S_IRUSR)
            dirperm |= S_IXUSR;
        if (permissions & S_IRGRP)
            dirperm |= S_IXGRP;
        if (permissions & S_IROTH)
            dirperm |= S_IXOTH;

#ifdef DEBUG_NSIFILE
        fprintf(stderr, "nsIFile: perm = %o, dirperm = %o\n", permissions,
                dirperm);
#endif

        if (NS_FAILED(CreateAllAncestors(dirperm)))
            return NS_ERROR_FAILURE;

#ifdef DEBUG_NSIFILE
        fprintf(stderr, "nsIFile: Create(\"%s\") again\n", mPath.get());
#endif
        result = createFunc(mPath.get(), flags, permissions, _retval);
    }
    return NSRESULT_FOR_RETURN(result);
}

NS_IMETHODIMP
nsLocalFile::Create(PRUint32 type, PRUint32 permissions)
{
    PRFileDesc *junk = nsnull;
    nsresult rv = CreateAndKeepOpen(type,
                                    PR_WRONLY | PR_CREATE_FILE | PR_TRUNCATE |
                                    PR_EXCL,
                                    permissions,
                                    &junk);
    if (junk)
        PR_Close(junk);
    return rv;
}

NS_IMETHODIMP
nsLocalFile::AppendNative(const nsACString &fragment)
{
    if (fragment.IsEmpty())
        return NS_OK;

#ifdef DEBUG_NSIFILE
    fprintf(stderr, "AppendNative: %s -> %s\n", mPath.get(), fragment.BeginReading());
#endif

    // only one component of path can be appended
    nsACString::const_iterator begin, end;
    if (FindCharInReadable('/', fragment.BeginReading(begin),
                                fragment.EndReading(end)))
        return NS_ERROR_FILE_UNRECOGNIZED_PATH;

    // check for colons, too
    if (FindCharInReadable(':', fragment.BeginReading(begin),
                                fragment.EndReading(end)))
        return NS_ERROR_FILE_UNRECOGNIZED_PATH;

    return AppendRelativeNativePath(fragment);
}

NS_IMETHODIMP
nsLocalFile::AppendRelativeNativePath(const nsACString &fragment)
{
#ifdef DEBUG_NSIFILE
    fprintf(stderr, "pre AppendRelativeNativePath: %s\n", mPath.get());
#endif
    if (fragment.IsEmpty())
        return NS_OK;

    // No leading '/'
    if (fragment.First() == '/')
        return NS_ERROR_FILE_UNRECOGNIZED_PATH;

    if (mPath.EqualsLiteral("/") || mPath.Last() == ':')
        mPath.Append(fragment);
    else
        mPath.Append(NS_LITERAL_CSTRING("/") + fragment);

#ifdef DEBUG_NSIFILE
    fprintf(stderr, "post AppendRelativeNativePath: %s\n", mPath.get());
#endif
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::Normalize()
{
    char    resolved_path[PATH_MAX] = "";
    char *resolved_path_ptr = nsnull;

    resolved_path_ptr = realpath(mPath.get(), resolved_path);

    // if there is an error, the return is null.
    if (!resolved_path_ptr)
        return NSRESULT_FOR_ERRNO();

    mPath = resolved_path;

#ifdef DEBUG_NSIFILE
    fprintf(stderr, "Normalize: %s\n", mPath.get());
#endif
    return NS_OK;
}

void
nsLocalFile::LocateNativeLeafName(nsACString::const_iterator &begin,
                                  nsACString::const_iterator &end)
{
    // XXX perhaps we should cache this??

    mPath.BeginReading(begin);
    mPath.EndReading(end);

    nsACString::const_iterator it = end;
    nsACString::const_iterator stop = begin;
    --stop;
    while (--it != stop) {
        if (*it == '/') {
            begin = ++it;
            return;
        }
        if (*it == ':') {
        	begin = ++it;
        	return;
        }
    }
    // else, the entire path is the leaf name (which means this
    // isn't an absolute path... unexpected??)
}

NS_IMETHODIMP
nsLocalFile::GetNativeLeafName(nsACString &aLeafName)
{
    nsACString::const_iterator begin, end;
    LocateNativeLeafName(begin, end);
    aLeafName = Substring(begin, end);
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::SetNativeLeafName(const nsACString &aLeafName)
{
    nsACString::const_iterator begin, end;
    LocateNativeLeafName(begin, end);
    mPath.Replace(begin.get() - mPath.get(), Distance(begin, end), aLeafName);

    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetNativePath(nsACString &_retval)
{
    _retval = mPath;
    return NS_OK;
}

nsresult
nsLocalFile::GetNativeTargetPathName(nsIFile *newParent,
                                     const nsACString &newName,
                                     nsACString &_retval)
{
#warning Amiga-style path handling needed
    nsresult rv;
    nsCOMPtr<nsIFile> oldParent;

    if (!newParent) {
        if (NS_FAILED(rv = GetParent(getter_AddRefs(oldParent))))
            return rv;
        newParent = oldParent.get();
    } else {
        // check to see if our target directory exists
        PRBool targetExists;
        if (NS_FAILED(rv = newParent->Exists(&targetExists)))
            return rv;

        if (!targetExists) {
            // XXX create the new directory with some permissions
            rv = newParent->Create(DIRECTORY_TYPE, 0755);
            if (NS_FAILED(rv))
                return rv;
        } else {
            // make sure that the target is actually a directory
            PRBool targetIsDirectory;
            if (NS_FAILED(rv = newParent->IsDirectory(&targetIsDirectory)))
                return rv;
            if (!targetIsDirectory)
                return NS_ERROR_FILE_DESTINATION_NOT_DIR;
        }
    }

    nsACString::const_iterator nameBegin, nameEnd;
    if (!newName.IsEmpty()) {
        newName.BeginReading(nameBegin);
        newName.EndReading(nameEnd);
    }
    else
        LocateNativeLeafName(nameBegin, nameEnd);

    nsCAutoString dirName;
    if (NS_FAILED(rv = newParent->GetNativePath(dirName)))
        return rv;

    if (dirName.Last() == ':')
    {
    	_retval = dirName
        		+ Substring(nameBegin, nameEnd);
    }
    else
    {
    	_retval = dirName
    			+ NS_LITERAL_CSTRING("/")
    			+ Substring(nameBegin, nameEnd);
    }
    return NS_OK;
}

nsresult
nsLocalFile::CopyDirectoryTo(nsIFile *newParent)
{
    nsresult rv;
    /*
     * dirCheck is used for various boolean test results such as from Equals,
     * Exists, isDir, etc.
     */
    PRBool dirCheck, isSymlink;
    PRUint32 oldPerms;

    if (NS_FAILED(rv = IsDirectory(&dirCheck)))
        return rv;
    if (!dirCheck)
        return CopyToNative(newParent, EmptyCString());

    if (NS_FAILED(rv = Equals(newParent, &dirCheck)))
        return rv;
    if (dirCheck) {
        // can't copy dir to itself
        return NS_ERROR_INVALID_ARG;
    }

    if (NS_FAILED(rv = newParent->Exists(&dirCheck)))
        return rv;
    // get the dirs old permissions
    if (NS_FAILED(rv = GetPermissions(&oldPerms)))
        return rv;
    if (!dirCheck) {
        if (NS_FAILED(rv = newParent->Create(DIRECTORY_TYPE, oldPerms)))
            return rv;
    } else {    // dir exists lets try to use leaf
        nsCAutoString leafName;
        if (NS_FAILED(rv = GetNativeLeafName(leafName)))
            return rv;
        if (NS_FAILED(rv = newParent->AppendNative(leafName)))
            return rv;
        if (NS_FAILED(rv = newParent->Exists(&dirCheck)))
            return rv;
        if (dirCheck)
            return NS_ERROR_FILE_ALREADY_EXISTS; // dest exists
        if (NS_FAILED(rv = newParent->Create(DIRECTORY_TYPE, oldPerms)))
            return rv;
    }

    nsCOMPtr<nsISimpleEnumerator> dirIterator;
    if (NS_FAILED(rv = GetDirectoryEntries(getter_AddRefs(dirIterator))))
        return rv;

    PRBool hasMore = PR_FALSE;
    while (dirIterator->HasMoreElements(&hasMore), hasMore) {
        nsCOMPtr<nsIFile> entry;
        rv = dirIterator->GetNext((nsISupports**)getter_AddRefs(entry));
        if (NS_FAILED(rv))
            continue;
        if (NS_FAILED(rv = entry->IsSymlink(&isSymlink)))
            return rv;
        if (NS_FAILED(rv = entry->IsDirectory(&dirCheck)))
            return rv;
        if (dirCheck && !isSymlink) {
            nsCOMPtr<nsIFile> destClone;
            rv = newParent->Clone(getter_AddRefs(destClone));
            if (NS_SUCCEEDED(rv)) {
                nsCOMPtr<nsILocalFile> newDir(do_QueryInterface(destClone));
                if (NS_FAILED(rv = entry->CopyToNative(newDir, EmptyCString()))) {
#ifdef DEBUG
                    nsresult rv2;
                    nsCAutoString pathName;
                    if (NS_FAILED(rv2 = entry->GetNativePath(pathName)))
                        return rv2;
                    printf("Operation not supported: %s\n", pathName.get());
#endif
                    if (rv == NS_ERROR_OUT_OF_MEMORY)
                        return rv;
                    continue;
                }
            }
        } else {
            if (NS_FAILED(rv = entry->CopyToNative(newParent, EmptyCString()))) {
#ifdef DEBUG
                nsresult rv2;
                nsCAutoString pathName;
                if (NS_FAILED(rv2 = entry->GetNativePath(pathName)))
                    return rv2;
                printf("Operation not supported: %s\n", pathName.get());
#endif
                if (rv == NS_ERROR_OUT_OF_MEMORY)
                    return rv;
                continue;
            }
        }
    }
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::CopyToNative(nsIFile *newParent, const nsACString &newName)
{
    nsresult rv;
    // check to make sure that this has been initialized properly
    CHECK_mPath();

    // we copy the parent here so 'newParent' remains immutable
    nsCOMPtr <nsIFile> workParent;
    if (newParent) {
        if (NS_FAILED(rv = newParent->Clone(getter_AddRefs(workParent))))
            return rv;
    } else {
        if (NS_FAILED(rv = GetParent(getter_AddRefs(workParent))))
            return rv;
    }

    // check to see if we are a directory or if we are a file
    PRBool isDirectory;
    if (NS_FAILED(rv = IsDirectory(&isDirectory)))
        return rv;

    nsCAutoString newPathName;
    if (isDirectory) {
        if (!newName.IsEmpty()) {
            if (NS_FAILED(rv = workParent->AppendNative(newName)))
                return rv;
        } else {
            if (NS_FAILED(rv = GetNativeLeafName(newPathName)))
                return rv;
            if (NS_FAILED(rv = workParent->AppendNative(newPathName)))
                return rv;
        }
        if (NS_FAILED(rv = CopyDirectoryTo(workParent)))
            return rv;
    } else {
        rv = GetNativeTargetPathName(workParent, newName, newPathName);
        if (NS_FAILED(rv))
            return rv;

#ifdef DEBUG_blizzard
        printf("nsLocalFile::CopyTo() %s -> %s\n", mPath.get(), newPathName.get());
#endif

        // actually create the file.
        nsLocalFile *newFile = new nsLocalFile();
        if (!newFile)
            return NS_ERROR_OUT_OF_MEMORY;

        nsCOMPtr<nsILocalFile> fileRef(newFile); // release on exit

        rv = newFile->InitWithNativePath(newPathName);
        if (NS_FAILED(rv))
            return rv;

        // get the old permissions
        PRUint32 myPerms;
        GetPermissions(&myPerms);

        PRFileDesc *newFD;
        rv = newFile->CreateAndKeepOpen(NORMAL_FILE_TYPE,
                                        PR_WRONLY|PR_CREATE_FILE|PR_TRUNCATE,
                                        myPerms,
                                        &newFD);
        if (NS_FAILED(rv))
            return rv;

        // open the old file, too
        PRBool specialFile;
        if (NS_FAILED(rv = IsSpecial(&specialFile))) {
            PR_Close(newFD);
            return rv;
        }
        if (specialFile) {
#ifdef DEBUG
            printf("Operation not supported: %s\n", mPath.get());
#endif
            // make sure to clean up properly
            PR_Close(newFD);
            return NS_OK;
        }

        PRFileDesc *oldFD;
        rv = OpenNSPRFileDesc(PR_RDONLY, myPerms, &oldFD);
        if (NS_FAILED(rv)) {
            // make sure to clean up properly
            PR_Close(newFD);
            return rv;
        }

#ifdef DEBUG_blizzard
        PRInt32 totalRead = 0;
        PRInt32 totalWritten = 0;
#endif
        char buf[BUFSIZ];
        PRInt32 bytesRead;

        while ((bytesRead = PR_Read(oldFD, buf, BUFSIZ)) > 0) {
#ifdef DEBUG_blizzard
            totalRead += bytesRead;
#endif

            // PR_Write promises never to do a short write
            PRInt32 bytesWritten = PR_Write(newFD, buf, bytesRead);
            if (bytesWritten < 0) {
                bytesRead = -1;
                break;
            }
            NS_ASSERTION(bytesWritten == bytesRead, "short PR_Write?");

#ifdef DEBUG_blizzard
            totalWritten += bytesWritten;
#endif
        }

#ifdef DEBUG_blizzard
        printf("read %d bytes, wrote %d bytes\n",
                 totalRead, totalWritten);
#endif

        // close the files
        PR_Close(newFD);
        PR_Close(oldFD);

        // check for read (or write) error after cleaning up
        if (bytesRead < 0)
            return NS_ERROR_OUT_OF_MEMORY;
    }
    return rv;
}

NS_IMETHODIMP
nsLocalFile::CopyToFollowingLinksNative(nsIFile *newParent, const nsACString &newName)
{
    return CopyToNative(newParent, newName);
}

NS_IMETHODIMP
nsLocalFile::MoveToNative(nsIFile *newParent, const nsACString &newName)
{
    nsresult rv;

    // check to make sure that this has been initialized properly
    CHECK_mPath();

    // check to make sure that we have a new parent
    nsCAutoString newPathName;
    rv = GetNativeTargetPathName(newParent, newName, newPathName);
    if (NS_FAILED(rv))
        return rv;

    // if the original file exists, remove it first
    if (access(newPathName.get(), F_OK) == 0) {
    	unlink(newPathName.get());
    }

    // try for atomic rename, falling back to copy/delete
    if (rename(mPath.get(), newPathName.get()) < 0) {
        if (errno == EXDEV) {
            rv = CopyToNative(newParent, newName);
            if (NS_SUCCEEDED(rv))
                rv = Remove(PR_TRUE);
        } else {
            rv = NSRESULT_FOR_ERRNO();
        }
    }
    return rv;
}

NS_IMETHODIMP
nsLocalFile::Remove(PRBool recursive)
{
    CHECK_mPath();
    ENSURE_STAT_CACHE();

    PRBool isSymLink;

    nsresult rv = IsSymlink(&isSymLink);
    if (NS_FAILED(rv))
        return rv;

    if (!recursive && isSymLink)
        return NSRESULT_FOR_RETURN(unlink(mPath.get()));

    if (S_ISDIR(mCachedStat.st_mode)) {
        if (recursive) {
            nsDirEnumeratorAmigaOS *dir = new nsDirEnumeratorAmigaOS();
            if (!dir)
                return NS_ERROR_OUT_OF_MEMORY;

            nsCOMPtr<nsISimpleEnumerator> dirRef(dir); // release on exit

            rv = dir->Init(this, PR_FALSE);
            if (NS_FAILED(rv))
                return rv;

            PRBool more;
            while (dir->HasMoreElements(&more), more) {
                nsCOMPtr<nsISupports> item;
                rv = dir->GetNext(getter_AddRefs(item));
                if (NS_FAILED(rv))
                    return NS_ERROR_FAILURE;

                nsCOMPtr<nsIFile> file = do_QueryInterface(item, &rv);
                if (NS_FAILED(rv))
                    return NS_ERROR_FAILURE;
                if (NS_FAILED(rv = file->Remove(recursive)))
                    return rv;
            }
        }

        if (rmdir(mPath.get()) == -1)
            return NSRESULT_FOR_ERRNO();
    } else {
        if (unlink(mPath.get()) == -1)
            return NSRESULT_FOR_ERRNO();
    }

    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetLastModifiedTime(PRInt64 *aLastModTime)
{
    CHECK_mPath();
    NS_ENSURE_ARG(aLastModTime);

    PRFileInfo64 info;
    if (PR_GetFileInfo64(mPath.get(), &info) != PR_SUCCESS)
        return NSRESULT_FOR_ERRNO();

    // PRTime is a 64 bit value
    // microseconds -> milliseconds
    PRInt64 usecPerMsec;
    LL_I2L(usecPerMsec, PR_USEC_PER_MSEC);
    LL_DIV(*aLastModTime, info.modifyTime, usecPerMsec);
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::SetLastModifiedTime(PRInt64 aLastModTime)
{
    CHECK_mPath();

    int result;
    if (! LL_IS_ZERO(aLastModTime)) {
        ENSURE_STAT_CACHE();
        struct utimbuf ut;
        ut.actime = mCachedStat.st_atime;

        // convert milliseconds to seconds since the unix epoch
        double dTime;
        LL_L2D(dTime, aLastModTime);
        ut.modtime = (time_t) (dTime / PR_MSEC_PER_SEC);
        result = utime(mPath.get(), &ut);
    } else {
        result = utime(mPath.get(), nsnull);
    }
    return NSRESULT_FOR_RETURN(result);
}

NS_IMETHODIMP
nsLocalFile::GetLastModifiedTimeOfLink(PRInt64 *aLastModTimeOfLink)
{
    CHECK_mPath();
    NS_ENSURE_ARG(aLastModTimeOfLink);

    struct stat sbuf;
    if (lstat(mPath.get(), &sbuf) == -1)
        return NSRESULT_FOR_ERRNO();
    LL_I2L(*aLastModTimeOfLink, (PRInt32)sbuf.st_mtime);

    // lstat returns st_mtime in seconds
    PRInt64 msecPerSec;
    LL_I2L(msecPerSec, PR_MSEC_PER_SEC);
    LL_MUL(*aLastModTimeOfLink, *aLastModTimeOfLink, msecPerSec);

    return NS_OK;
}

/*
 * utime(2) may or may not dereference symlinks, joy.
 */
NS_IMETHODIMP
nsLocalFile::SetLastModifiedTimeOfLink(PRInt64 aLastModTimeOfLink)
{
    return SetLastModifiedTime(aLastModTimeOfLink);
}

/*
 * Only send back permissions bits: maybe we want to send back the whole
 * mode_t to permit checks against other file types?
 */

#define NORMALIZE_PERMS(mode)    ((mode)& (S_IRWXU | S_IRWXG | S_IRWXO))

NS_IMETHODIMP
nsLocalFile::GetPermissions(PRUint32 *aPermissions)
{
    NS_ENSURE_ARG(aPermissions);
    ENSURE_STAT_CACHE();
    *aPermissions = NORMALIZE_PERMS(mCachedStat.st_mode);
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetPermissionsOfLink(PRUint32 *aPermissionsOfLink)
{
    CHECK_mPath();
    NS_ENSURE_ARG(aPermissionsOfLink);

    struct stat sbuf;
    if (lstat(mPath.get(), &sbuf) == -1)
        return NSRESULT_FOR_ERRNO();
    *aPermissionsOfLink = NORMALIZE_PERMS(sbuf.st_mode);
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::SetPermissions(PRUint32 aPermissions)
{
    CHECK_mPath();

    /*
     * Race condition here: we should use fchmod instead, there's no way to
     * guarantee the name still refers to the same file.
     */
    if (chmod(mPath.get(), aPermissions) < 0)
        return NSRESULT_FOR_ERRNO();
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::SetPermissionsOfLink(PRUint32 aPermissions)
{
    // There isn't a consistent mechanism for doing this on UNIX platforms. We
    // might want to carefully implement this in the future though.
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
nsLocalFile::GetFileSize(PRInt64 *aFileSize)
{
    NS_ENSURE_ARG_POINTER(aFileSize);
    *aFileSize = LL_ZERO;
    ENSURE_STAT_CACHE();

#if defined(VMS)
    /* Only two record formats can report correct file content size */
    if ((mCachedStat.st_fab_rfm != FAB$C_STMLF) &&
        (mCachedStat.st_fab_rfm != FAB$C_STMCR)) {
        return NS_ERROR_FAILURE;
    }
#endif

    if (!S_ISDIR(mCachedStat.st_mode)) {
#ifdef HAVE_STAT64
        *aFileSize = mCachedStat.st_size;
#else
        LL_UI2L(*aFileSize, (PRUint32)mCachedStat.st_size);
#endif
    }
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::SetFileSize(PRInt64 aFileSize)
{
    CHECK_mPath();

    PRInt32 size;
    LL_L2I(size, aFileSize);
    /* XXX truncate64? */
    if (truncate(mPath.get(), (off_t)size) == -1)
        return NSRESULT_FOR_ERRNO();
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetFileSizeOfLink(PRInt64 *aFileSize)
{
    CHECK_mPath();
    NS_ENSURE_ARG(aFileSize);

#ifdef HAVE_LSTAT64
    struct stat64 sbuf;
    if (lstat64(mPath.get(), &sbuf) == -1)
        return NSRESULT_FOR_ERRNO();
    *aFileSize = sbuf.st_size;
#else
    struct stat sbuf;
    if (lstat(mPath.get(), &sbuf) == -1)
        return NSRESULT_FOR_ERRNO();
    LL_UI2L(*aFileSize, (PRUint32)sbuf.st_size);
#endif
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetDiskSpaceAvailable(PRInt64 *aDiskSpaceAvailable)
{
    NS_ENSURE_ARG_POINTER(aDiskSpaceAvailable);

    struct InfoData *id = (struct InfoData *)IDOS->AllocDosObject(DOS_INFODATA,0);

    int32 result = IDOS->GetDiskInfoTags(
    		GDI_StringNameInput,		"sys:",
    		GDI_InfoData,				id,
    	TAG_DONE);

    PRInt64 avail = (PRInt64)(id->id_NumBlocks - id->id_NumBlocksUsed) * (PRInt64)id->id_BytesPerBlock;

    IDOS->FreeDosObject(DOS_INFODATA, (void *)id);

    *aDiskSpaceAvailable = avail;

    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetParent(nsIFile **aParent)
{
#ifdef DEBUG_NSIFILE
    fprintf(stderr, "GetParent: %s\n", mPath.get());
#endif

    CHECK_mPath();
    NS_ENSURE_ARG_POINTER(aParent);
    *aParent       = nsnull;

    // if '/' we are at the top of the volume, return null
    if (mPath.Equals("/"))
        return  NS_OK;

    // <brendan, after jband> I promise to play nice
    char *buffer   = mPath.BeginWriting(),
         *slashp   = buffer;

    // If the last character is a colon, it's go no
//    if (mPath.Last() == ':')
//    	return NS_OK;

    // find the last significant slash in buffer
    slashp = strrchr(buffer, '/');
    if (!slashp)
    	slashp = strrchr(buffer, ':');

    NS_ASSERTION(slashp, "non-canonical mPath?");
    if (!slashp)
        return NS_ERROR_FILE_INVALID_PATH;

    // for the case where we are at '/'
    if (slashp == buffer)
        slashp++;

    // Check for ':', need to leave that in
    if (*slashp == ':')
    	slashp++;

    // temporarily terminate buffer at the last significant slash
    char c = *slashp;
    *slashp = '\0';

#ifdef DEBUG_NSIFILE
    fprintf(stderr, "Creating new parent %s\n", buffer);
#endif
    nsCOMPtr<nsILocalFile> localFile;
    nsresult rv = NS_NewNativeLocalFile(nsDependentCString(buffer), PR_TRUE,
                                        getter_AddRefs(localFile));

    // make buffer whole again
    *slashp = c;

    if (NS_SUCCEEDED(rv) && localFile)
        rv = CallQueryInterface(localFile, aParent);
    return rv;
}

/*
 * The results of Exists, isWritable and isReadable are not cached.
 */


NS_IMETHODIMP
nsLocalFile::Exists(PRBool *_retval)
{
    CHECK_mPath();
    NS_ENSURE_ARG_POINTER(_retval);

    *_retval = (access(mPath.get(), F_OK) == 0);
    return NS_OK;
}


NS_IMETHODIMP
nsLocalFile::IsWritable(PRBool *_retval)
{
    CHECK_mPath();
    NS_ENSURE_ARG_POINTER(_retval);

    *_retval = (access(mPath.get(), W_OK) == 0);
    if (*_retval || errno == EACCES)
        return NS_OK;
    return NSRESULT_FOR_ERRNO();
}

NS_IMETHODIMP
nsLocalFile::IsReadable(PRBool *_retval)
{
    CHECK_mPath();
    NS_ENSURE_ARG_POINTER(_retval);

    *_retval = (access(mPath.get(), R_OK) == 0);
    if (*_retval || errno == EACCES)
        return NS_OK;
    return NSRESULT_FOR_ERRNO();
}

NS_IMETHODIMP
nsLocalFile::IsExecutable(PRBool *_retval)
{
    CHECK_mPath();
    NS_ENSURE_ARG_POINTER(_retval);

    *_retval = (access(mPath.get(), X_OK) == 0);
    if (*_retval || errno == EACCES)
        return NS_OK;
    return NSRESULT_FOR_ERRNO();
}

NS_IMETHODIMP
nsLocalFile::IsDirectory(PRBool *_retval)
{
    NS_ENSURE_ARG_POINTER(_retval);
    *_retval = PR_FALSE;
    ENSURE_STAT_CACHE();
    *_retval = S_ISDIR(mCachedStat.st_mode);
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::IsFile(PRBool *_retval)
{
    NS_ENSURE_ARG_POINTER(_retval);
    *_retval = PR_FALSE;
    ENSURE_STAT_CACHE();
    *_retval = S_ISREG(mCachedStat.st_mode);
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::IsHidden(PRBool *_retval)
{
    NS_ENSURE_ARG_POINTER(_retval);
    nsACString::const_iterator begin, end;
    LocateNativeLeafName(begin, end);
    *_retval = (*begin == '.');
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::IsSymlink(PRBool *_retval)
{
    NS_ENSURE_ARG_POINTER(_retval);
    CHECK_mPath();

    struct stat symStat;
    lstat(mPath.get(), &symStat);
    *_retval=S_ISLNK(symStat.st_mode);
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::IsSpecial(PRBool *_retval)
{
    NS_ENSURE_ARG_POINTER(_retval);
    ENSURE_STAT_CACHE();
    *_retval = S_ISCHR(mCachedStat.st_mode)      ||
                 S_ISBLK(mCachedStat.st_mode)    ||
#ifdef S_ISSOCK
                 S_ISSOCK(mCachedStat.st_mode)   ||
#endif
                 S_ISFIFO(mCachedStat.st_mode);

    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::Equals(nsIFile *inFile, PRBool *_retval)
{
    NS_ENSURE_ARG(inFile);
    NS_ENSURE_ARG_POINTER(_retval);
    *_retval = PR_FALSE;

    nsresult rv;
    nsCAutoString inPath;

    if (NS_FAILED(rv = inFile->GetNativePath(inPath)))
        return rv;

    *_retval = !FILE_STRCMP(inPath.get(), mPath.get());
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::Contains(nsIFile *inFile, PRBool recur, PRBool *_retval)
{
    CHECK_mPath();
    NS_ENSURE_ARG(inFile);
    NS_ENSURE_ARG_POINTER(_retval);

    nsCAutoString inPath;
    nsresult rv;

    if (NS_FAILED(rv = inFile->GetNativePath(inPath)))
        return rv;

    *_retval = PR_FALSE;

    ssize_t len = mPath.Length();
    if (FILE_STRNCMP(mPath.get(), inPath.get(), len) == 0) {
        // Now make sure that the |inFile|'s path has a separator at len,
        // which implies that it has more components after len.
        if (inPath[len] == '/')
            *_retval = PR_TRUE;
    }

    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetNativeTarget(nsACString &_retval)
{
    CHECK_mPath();
    _retval.Truncate();

    struct stat symStat;
    lstat(mPath.get(), &symStat);
    if (!S_ISLNK(symStat.st_mode))
        return NS_ERROR_FILE_INVALID_PATH;

    PRInt64 targetSize64;
    if (NS_FAILED(GetFileSizeOfLink(&targetSize64)))
        return NS_ERROR_FAILURE;

    PRInt32 size;
    LL_L2I(size, targetSize64);
    char *target = (char *)nsMemory::Alloc(size + 1);
    if (!target)
        return NS_ERROR_OUT_OF_MEMORY;

    if (readlink(mPath.get(), target, (size_t)size) < 0) {
        nsMemory::Free(target);
        return NSRESULT_FOR_ERRNO();
    }
    target[size] = '\0';

    nsresult rv;
    PRBool isSymlink;
    nsCOMPtr<nsIFile> self(this);
    nsCOMPtr<nsIFile> parent;
    while (NS_SUCCEEDED(rv = self->GetParent(getter_AddRefs(parent)))) {
        NS_ASSERTION(parent != nsnull, "no parent?!");

        if (target[0] != '/') {
            nsCOMPtr<nsILocalFile> localFile(do_QueryInterface(parent, &rv));
            if (NS_FAILED(rv))
                break;
            if (NS_FAILED(rv = localFile->AppendRelativeNativePath(nsDependentCString(target))))
                break;
            if (NS_FAILED(rv = localFile->GetNativePath(_retval)))
                break;
            if (NS_FAILED(rv = parent->IsSymlink(&isSymlink)))
                break;
            self = parent;
        } else {
            nsCOMPtr<nsILocalFile> localFile;
            rv = NS_NewNativeLocalFile(nsDependentCString(target), PR_TRUE,
                                       getter_AddRefs(localFile));
            if (NS_FAILED(rv))
                break;
            if (NS_FAILED(rv = localFile->IsSymlink(&isSymlink)))
                break;
            _retval = target; // XXX can we avoid this buffer copy?
            self = do_QueryInterface(localFile);
        }
        if (NS_FAILED(rv) || !isSymlink)
            break;

        const nsPromiseFlatCString &flatRetval = PromiseFlatCString(_retval);

        // strip off any and all trailing '/'
        PRInt32 len = strlen(target);
        while (target[len-1] == '/' && len > 1)
            target[--len] = '\0';
        if (lstat(flatRetval.get(), &symStat) < 0) {
            rv = NSRESULT_FOR_ERRNO();
            break;
        }
        if (!S_ISLNK(symStat.st_mode)) {
            rv = NS_ERROR_FILE_INVALID_PATH;
            break;
        }
        size = symStat.st_size;
        if (readlink(flatRetval.get(), target, size) < 0) {
            rv = NSRESULT_FOR_ERRNO();
            break;
        }
        target[size] = '\0';

        _retval.Truncate();
    }

    nsMemory::Free(target);

    if (NS_FAILED(rv))
        _retval.Truncate();
    return rv;
}

/* attribute PRBool followLinks; */
NS_IMETHODIMP
nsLocalFile::GetFollowLinks(PRBool *aFollowLinks)
{
    *aFollowLinks = PR_TRUE;
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::SetFollowLinks(PRBool aFollowLinks)
{
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetDirectoryEntries(nsISimpleEnumerator **entries)
{
    nsDirEnumeratorAmigaOS *dir = new nsDirEnumeratorAmigaOS();
    if (!dir)
        return NS_ERROR_OUT_OF_MEMORY;

    NS_ADDREF(dir);
    nsresult rv = dir->Init(this, PR_FALSE);
    if (NS_FAILED(rv)) {
        *entries = nsnull;
        NS_RELEASE(dir);
    } else {
        *entries = dir; // transfer reference
    }

    return rv;
}

NS_IMETHODIMP
nsLocalFile::Load(PRLibrary **_retval)
{
    CHECK_mPath();
    NS_ENSURE_ARG_POINTER(_retval);

    NS_TIMELINE_START_TIMER("PR_LoadLibrary");

#ifdef NS_BUILD_REFCNT_LOGGING
    nsTraceRefcntImpl::SetActivityIsLegal(PR_FALSE);
#endif

    *_retval = PR_LoadLibrary(mPath.get());

#ifdef NS_BUILD_REFCNT_LOGGING
    nsTraceRefcntImpl::SetActivityIsLegal(PR_TRUE);
#endif

    NS_TIMELINE_STOP_TIMER("PR_LoadLibrary");
    NS_TIMELINE_MARK_TIMER1("PR_LoadLibrary", mPath.get());

    if (!*_retval)
        return NS_ERROR_FAILURE;
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetPersistentDescriptor(nsACString &aPersistentDescriptor)
{
    return GetNativePath(aPersistentDescriptor);
}

NS_IMETHODIMP
nsLocalFile::SetPersistentDescriptor(const nsACString &aPersistentDescriptor)
{
    return InitWithNativePath(aPersistentDescriptor);
}

static BOOL
__selectobject(struct Hook *hook, APTR reserved, struct IconSelectMsg *ism)
{
	if (strcasecmp(ism->ism_Name, (const char *)hook->h_Data) == 0)
		return TRUE;

	return FALSE;
}

NS_IMETHODIMP
nsLocalFile::Reveal()
{
	char path[512];

	path[511] = '\0';
	strncpy(path, mPath.get(), 510);

	char *pathPart = IDOS->PathPart(path);
	if (pathPart != path && pathPart)
		*pathPart = '\0';

	struct Library *wbBase = IExec->OpenLibrary("workbench.library", 0);
	if (wbBase)
	{
		struct WorkbenchIFace *iwb = (struct WorkbenchIFace *)IExec->GetInterface(wbBase, "main", 1, NULL);
		if (iwb)
		{
			BOOL success = iwb->OpenWorkbenchObjectA(path, NULL);
			if (success)
			{
				/* If the folder opened, we want to select the icon so the user can find it */
				iwb->MakeWorkbenchObjectVisibleA(path, NULL);

				/* Make it active */
				struct Hook cwshook;
				cwshook.h_Entry = (HOOKFUNC) __selectobject;
				cwshook.h_Data = (void *)IDOS->FilePart(mPath.get());

				iwb->ChangeWorkbenchSelectionA(path, &cwshook, NULL);
			}

			IExec->DropInterface((struct Interface *)iwb);
		}

		IExec->CloseLibrary(wbBase);
	}

	return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::Launch()
{
	struct Library *wbBase = IExec->OpenLibrary("workbench.library", 0);
	if (wbBase)
	{
		struct WorkbenchIFace *iwb = (struct WorkbenchIFace *)IExec->GetInterface(wbBase, "main", 1, NULL);
		if (iwb)
		{
			BOOL success = iwb->OpenWorkbenchObjectA(mPath.get(), NULL);

			IExec->DropInterface((struct Interface *)iwb);
		}

		IExec->CloseLibrary(wbBase);
	}
	return NS_OK;
}


nsresult
NS_NewNativeLocalFile(const nsACString &path, PRBool followSymlinks, nsILocalFile **result)
{
    nsLocalFile *file = new nsLocalFile();
    if (!file)
        return NS_ERROR_OUT_OF_MEMORY;
    NS_ADDREF(file);

#ifdef DEBUG_NSIFILE
    fprintf(stderr, "NS_NewNativeLocalFile(\"%s\", ...)\n", path.BeginReading());
#endif

    if (!path.IsEmpty()) {
        nsresult rv = file->InitWithNativePath(path);
        if (NS_FAILED(rv)) {
            NS_RELEASE(file);
            return rv;
        }
    }

    *result = file;
    return NS_OK;
}

//-----------------------------------------------------------------------------
// unicode support
//-----------------------------------------------------------------------------

#define SET_UCS(func, ucsArg) \
    { \
        nsCAutoString buf; \
        nsresult rv = NS_CopyUnicodeToNative(ucsArg, buf); \
        if (NS_FAILED(rv)) \
            return rv; \
        return (func)(buf); \
    }

#define GET_UCS(func, ucsArg) \
    { \
        nsCAutoString buf; \
        nsresult rv = (func)(buf); \
        if (NS_FAILED(rv)) return rv; \
        return NS_CopyNativeToUnicode(buf, ucsArg); \
    }

#define SET_UCS_2ARGS_2(func, opaqueArg, ucsArg) \
    { \
        nsCAutoString buf; \
        nsresult rv = NS_CopyUnicodeToNative(ucsArg, buf); \
        if (NS_FAILED(rv)) \
            return rv; \
        return (func)(opaqueArg, buf); \
    }

// Unicode interface Wrapper
nsresult
nsLocalFile::InitWithPath(const nsAString &filePath)
{
    SET_UCS(InitWithNativePath, filePath);
}
nsresult
nsLocalFile::Append(const nsAString &node)
{
    SET_UCS(AppendNative, node);
}
nsresult
nsLocalFile::AppendRelativePath(const nsAString &node)
{
    SET_UCS(AppendRelativeNativePath, node);
}
nsresult
nsLocalFile::GetLeafName(nsAString &aLeafName)
{
    GET_UCS(GetNativeLeafName, aLeafName);
}
nsresult
nsLocalFile::SetLeafName(const nsAString &aLeafName)
{
    SET_UCS(SetNativeLeafName, aLeafName);
}
nsresult
nsLocalFile::GetPath(nsAString &_retval)
{
    return NS_CopyNativeToUnicode(mPath, _retval);
}
nsresult
nsLocalFile::CopyTo(nsIFile *newParentDir, const nsAString &newName)
{
    SET_UCS_2ARGS_2(CopyToNative , newParentDir, newName);
}
nsresult
nsLocalFile::CopyToFollowingLinks(nsIFile *newParentDir, const nsAString &newName)
{
    SET_UCS_2ARGS_2(CopyToFollowingLinksNative , newParentDir, newName);
}
nsresult
nsLocalFile::MoveTo(nsIFile *newParentDir, const nsAString &newName)
{
    SET_UCS_2ARGS_2(MoveToNative, newParentDir, newName);
}
nsresult
nsLocalFile::GetTarget(nsAString &_retval)
{
    GET_UCS(GetNativeTarget, _retval);
}

// nsIHashable

NS_IMETHODIMP
nsLocalFile::Equals(nsIHashable* aOther, PRBool *aResult)
{
    nsCOMPtr<nsIFile> otherFile(do_QueryInterface(aOther));
    if (!otherFile) {
        *aResult = PR_FALSE;
        return NS_OK;
    }

    return Equals(otherFile, aResult);
}

NS_IMETHODIMP
nsLocalFile::GetHashCode(PRUint32 *aResult)
{
    *aResult = nsCRT::HashCode(mPath.get());
    return NS_OK;
}

nsresult
NS_NewLocalFile(const nsAString &path, PRBool followLinks, nsILocalFile* *result)
{
    nsCAutoString buf;
    nsresult rv = NS_CopyUnicodeToNative(path, buf);
    if (NS_FAILED(rv))
        return rv;
    return NS_NewNativeLocalFile(buf, followLinks, result);
}

//-----------------------------------------------------------------------------
// global init/shutdown
//-----------------------------------------------------------------------------

void
nsLocalFile::GlobalInit()
{
}

void
nsLocalFile::GlobalShutdown()
{
}
