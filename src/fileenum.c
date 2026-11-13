/*
 * fileenum.c - Recursive Unicode enumeration of a volume's files.
 *
 * The walk keeps its own stack of directory frames on the heap instead of
 * recursing in C: the native stack is fixed at link time and a deep tree
 * would overflow it, while a heap frame per level only costs memory that is
 * checked for. Paths use the \\?\ prefix so names longer than MAX_PATH
 * are reached (NT 4.0's wide APIs accept it). Reparse points are reported
 * but never followed, so a junction cannot make the walk visit a directory
 * twice or leave the volume. Every failure is counted, never ignored: the
 * callers report a scan with errors as incomplete (exit 4).
 *
 * Ctrl+C on NT only sets a flag here. The walk checks it between entries,
 * so the file being moved (one FSCTL_MOVE_FILE call, which the filesystem
 * completes or refuses as a whole) is finished before the program stops.
 */
#include "fileenum.h"
#include "util.h"

static volatile LONG g_interrupt;

/* Runs on a separate thread created by the console: only set the flag. */
static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_interrupt = 1;
        return TRUE;
    }
    return FALSE;
}

int enum_catch_ctrl_c(void)
{
    return SetConsoleCtrlHandler(on_ctrl, TRUE) != 0;
}

int enum_interrupted(void)
{
    return g_interrupt != 0;
}

typedef struct walk_frame {
    struct walk_frame *parent;
    wchar_t *path;
    HANDLE find;
    int started;
} walk_frame;

static int is_dot(const wchar_t *name)
{
    return name[0] == L'.' &&
           (!name[1] || (name[1] == L'.' && !name[2]));
}

static wchar_t *path_join(const wchar_t *dir, const wchar_t *name)
{
    size_t ld = wcslen(dir), ln = wcslen(name);
    int sep = ld && dir[ld - 1] != L'\\';
    wchar_t *p;
    if (ld + ln + sep + 1 > 32767) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return NULL;
    }
    p = malloc((ld + ln + sep + 1) * sizeof(*p));
    if (!p) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    wcscpy(p, dir);
    if (sep)
        wcscat(p, L"\\");
    wcscat(p, name);
    return p;
}

static void walk_error(ENUM_RESULT *r, const wchar_t *path, DWORD error)
{
    wchar_t message[200];
    if (!r->errors++)
        r->firstError = error;
    uerror(L"incomplete scan of %ls: %ls\n", path,
           uwin32_str(error, message, 200));
    if (error == ERROR_NOT_ENOUGH_MEMORY)
        r->aborted = 1;
}

ENUM_RESULT enum_tree(const wchar_t *root, file_cb cb, void *ctx)
{
    ENUM_RESULT r;
    walk_frame *top;
    wchar_t *path;
    memset(&r, 0, sizeof(r));
    if (root[0] && root[1] == L':')
        path = path_join(L"\\\\?\\", root);
    else
        path = path_join(root, L"");
    if (!path) {
        walk_error(&r, root, GetLastError());
        return r;
    }
    top = malloc(sizeof(*top));
    if (!top) {
        free(path);
        walk_error(&r, root, ERROR_NOT_ENOUGH_MEMORY);
        return r;
    }
    top->parent = NULL;
    top->path = path;
    top->find = INVALID_HANDLE_VALUE;
    top->started = 0;
    while (top && !r.aborted) {
        WIN32_FIND_DATAW fd;
        int got;
        if (!uoutput_ok()) {
            r.aborted = 1;
            break;
        }
        if (g_interrupt) {
            r.aborted = r.interrupted = 1;
            break;
        }
        if (!top->started) {
            wchar_t *pattern = path_join(top->path, L"*");
            top->started = 1;
            if (!pattern) {
                walk_error(&r, top->path, GetLastError());
                got = 0;
            } else {
                top->find = FindFirstFileW(pattern, &fd);
                free(pattern);
                got = top->find != INVALID_HANDLE_VALUE;
                if (!got) {
                    DWORD error = GetLastError();
                    DWORD attr = GetFileAttributesW(top->path);
                    if ((error != ERROR_FILE_NOT_FOUND && error != ERROR_NO_MORE_FILES) ||
                        attr == INVALID_FILE_ATTRIBUTES ||
                        !(attr & FILE_ATTRIBUTE_DIRECTORY))
                        walk_error(&r, top->path, error);
                }
            }
        } else {
            got = FindNextFileW(top->find, &fd) != 0;
            if (!got && GetLastError() != ERROR_NO_MORE_FILES)
                walk_error(&r, top->path, GetLastError());
        }
        if (!got) {
            walk_frame *old = top;
            top = top->parent;
            if (old->find != INVALID_HANDLE_VALUE && !FindClose(old->find))
                walk_error(&r, old->path, GetLastError());
            free(old->path);
            free(old);
            continue;
        }
        if (is_dot(fd.cFileName))
            continue;
        path = path_join(top->path, fd.cFileName);
        if (!path) {
            walk_error(&r, top->path, GetLastError());
            continue;
        }
        r.entries++;
        if (!cb(path, &fd, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                ctx)) {
            r.aborted = 1;
        } else if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                   !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            walk_frame *child = malloc(sizeof(*child));
            if (!child) {
                walk_error(&r, path, ERROR_NOT_ENOUGH_MEMORY);
            } else {
                child->parent = top;
                child->path = path;
                child->find = INVALID_HANDLE_VALUE;
                child->started = 0;
                top = child;
                path = NULL;
            }
        }
        free(path);
    }
    while (top) {
        walk_frame *old = top;
        top = top->parent;
        if (old->find != INVALID_HANDLE_VALUE && !FindClose(old->find))
            walk_error(&r, old->path, GetLastError());
        free(old->path);
        free(old);
    }
    return r;
}
