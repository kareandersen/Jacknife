// "Design" "doc":
// - adding files
// - deleting files
// - creating new disk images
// - extract deleted files -because tIn insisted-
// - monitoring directory and if it changes sync the differences with the image
// - "Make me a 800KB floppy with the following files on"
// - Supply disk geometry for new images, or use the default expending strategy

#ifdef _MSC_VER
#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include "Shlwapi.h"
#endif

#include <stdlib.h>
#include <stdio.h>

#if defined(_WIN32) && !defined(__MINGW32__)
#define DIR_SEPARATOR_STRING "\\"
#define FOPEN_S(a,b,c) fopen_s(&a,b,c)
#else
#include <unistd.h>
#define DIR_SEPARATOR_STRING "/"
#define __stdcall
#define sprintf_s(a,b,...) sprintf(a,__VA_ARGS__)
#define strcpy_s(a,b,c) strcpy(a,c)
#define FOPEN_S(a,b,c) a=fopen(b,c)
#define _strcmpi strcasecmp
typedef char *LPCSTR;
#define _stat stat
#define _ftelli64 ftello
#include <signal.h>
#define DebugBreak() raise(SIGTRAP);
#include <ctype.h>
#define TRUE true
#define FALSE false
#define BOOL bool
#include <stdbool.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#endif

#include "wcxhead.h"
#include "dosfs-1.03/dosfs.h"

extern int Pack(char *PackedFile, char *SubPath, char *SrcPath, char *AddList, int Flags);
extern int Delete(char *PackedFile, char *DeleteList);
extern int install_bootsector(char *image_file, char *bootsector_filename);
extern int add_volume_label(char *image_file, char *volume_name);

typedef enum
{
    ST_NONE,
    ST_CREATE,
    ST_ADD,
    ST_DELETE,
    ST_EXTRACT,
    ST_UNDELETE,
    ST_MONITOR
} ST_MODES;

BOOL check_if_pathname_exists(char *pathname)
{
#if (defined(_WIN32) || defined(_WIN64)) && !defined(__MINGW32__)
    BOOL test = PathFileExists(pathname);
#else
    struct stat path_info;
    int test = stat(pathname, &path_info);
    test = !((test == -1) && (errno == ENOENT));
#endif
    return (BOOL)test;
}

BOOL check_if_directory(char *pathname)
{
#if (defined(WIN32) || defined(WIN64)) && !defined(__MINGW32__)
    BOOL test = GetFileAttributesA(pathname) & FILE_ATTRIBUTE_DIRECTORY;
#else
    struct stat path_info;
    int test = stat(pathname, &path_info);
    if (test >= 0) test = ((path_info.st_mode & S_IFMT) == S_IFDIR);
#endif
    return (BOOL)test;
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        printf("more arguments - TODO: better error message\n");
        return -1;
    }

    // Eat the program filename argument
    argv++;
    argc--;
    
    int bootsector_install = 0;
    int volume_label = 0;
    int volume_label_index = 0;
    int bootsector_name_index = 0;
    int filenames_start_index = 2;
    
    ST_MODES mode = ST_NONE;
    if (*argv[0] == 'c')
    {
        mode = ST_CREATE;
    }
    else if (*argv[0] == 'd')
    {
        mode = ST_DELETE;
    }
    else if (*argv[0] == 'a')
    {
        mode = ST_ADD;
    }
    else if (*argv[0] == 'b')
    {
        if (argc != 2)
        {
            printf("exactly 2 arguments for b - TODO: better error message\n");
            return -1;
        }
        bootsector_install = 1;
    }
    if (*argv[2] == 'b')
    {
        bootsector_install = 1;
        bootsector_name_index = 3;
        filenames_start_index += 2;  // Skip parameter 'b' and bootsector filename
    }
    if (*argv[2] == 'l' || *argv[4]=='l')
    {
        volume_label = 1;
        volume_label_index = 3;
        if (*argv[4] == 'l')
        {
            volume_label_index = 5;
        }
        filenames_start_index += 2;  // Skip parameter 'l' and disk label name
    }

    char tc_file_listing[4096] = { 0 }; // TODO either make this a resizable array, or make 2 passes scanning filenames (the first to count characters)
    switch (mode)
    {
    case ST_CREATE:
    {
        if (check_if_pathname_exists(argv[1]))
        {
            printf("image exists - TODO: better error message\n");
            return -1;
        }
        // And we fallthrough to the next state
    }
    case ST_ADD:
    {
        // Populate file listing
        int i;
        char *current_file = tc_file_listing;
        for (i = filenames_start_index; i < argc; i++)
        {
            // TODO: disallow absolute filenames on windows (i.e. strip off "C:")
            if (!check_if_pathname_exists(argv[i]))
            {
                printf("file %s doesn't exist - TODO: better error message\n",argv[i]);
                return -1;
            }

            // Check if it's a file
            if (!check_if_directory(argv[i]))
            {
                // It's a file, add it to the list
                // If it's a complex pathname, i.e. "a\b\c\file" then add entries like
                // "a\", "a\b\", "a\b\c\", "a\b\c\file", so that the subfolders can be created
                int j;
                for (j = 0; j < strlen(argv[i]); j++)
                {
                    if (j != 0 && argv[i][j] == DIR_SEPARATOR)
                    {
                        memcpy(current_file, argv[i], j + 1);
                        current_file += j + 1;
                        *current_file++ = 0;
                    }
                }
                strcat(current_file, argv[i]);
                current_file += strlen(current_file) + 1; // Move past the filename and the 0 terminator
            }
            else
            {
                // It's a directory, we have to scan all the things and then add them to the list
                printf("add directory recursively to the list - TODO: better error message\n");
            }
        }
        *current_file = 0; // Add a second 0 terminator to indicate end of list
        
        int ret = Pack(argv[1], "", "", tc_file_listing, PK_PACK_SAVE_PATHS);
        if (ret != 0)
        {
            printf("Pack fail %d - TODO: better error message\n",ret);
        }
        
        ret = add_volume_label(argv[1], argv[volume_label_index]);
        
        break;
    }
    case ST_DELETE:
    {
        // Populate file listing
        int i;
        char *current_file = tc_file_listing;
        for (i = filenames_start_index; i < argc; i++)
        {
            strcat(current_file, argv[i]);
            current_file += strlen(current_file) + 1; // Move past the filename and the 0 terminator
        }
        *current_file = 0; // Add a second 0 terminator to indicate end of list
        if (Delete(argv[1], tc_file_listing) != 0)
        {
            printf("Delete fail - TODO: better error message\n");
        }

        break;
    }
    default:
        printf("unknown mode - TODO: better error message\n");
        return -1;
    }
    
    if (bootsector_install)
    {
        install_bootsector(argv[1], argv[bootsector_name_index]);
    }
    
    return 0;
}