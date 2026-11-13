/*
 * fileenum.h - Recursive Unicode file-system enumeration.
 */
#ifndef HSDEFRAG_FILEENUM_H
#define HSDEFRAG_FILEENUM_H

#include "compat.h"

/*
 * Callback invoked for each entry found. 'isDir' is non-zero for directories.
 * Return 0 to abort enumeration, non-zero to continue.
 */
typedef int (*file_cb)(const wchar_t *fullPath, const WIN32_FIND_DATAW *fd,
                       int isDir, void *ctx);

/*
 * Recursively enumerate 'root' (e.g. "C:\\"). Directories are reported via the
 * callback and then descended into, except reparse points, which are reported
 * but not followed. Uses extended drive paths and a heap-backed walk rather
 * than recursive C calls. Errors mean the scan does not cover the whole tree.
 */
typedef struct {
    unsigned long long entries;
    unsigned long long errors;
    DWORD firstError;
    int aborted;
    int interrupted;  /* stopped by Ctrl+C/Ctrl+Break (aborted is set too) */
} ENUM_RESULT;

ENUM_RESULT enum_tree(const wchar_t *root, file_cb cb, void *ctx);

/* Install a console handler that turns Ctrl+C and Ctrl+Break into a request
 * to stop: enum_tree() then ends before the next entry. NT only; the 9x
 * engine has its own handler. Returns 0 if the handler was not installed. */
int enum_catch_ctrl_c(void);

/* Non-zero once Ctrl+C or Ctrl+Break was pressed after enum_catch_ctrl_c(). */
int enum_interrupted(void);

#endif /* HSDEFRAG_FILEENUM_H */
