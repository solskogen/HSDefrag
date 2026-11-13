/*
 * bootjob.c - Text handling for the Windows 95/98 boot-time pass (see
 * bootjob.h). Freestanding: shared by the Win32 and the MS-DOS program.
 */
#include "bootjob.h"
#include "nostd.h"

static const char *const job_head =
    "; House Soft Defrag: defragment these drives before Windows starts.\r\n"
    "; Delete this file (or run hsdefrag X: --boot-cancel) to cancel.\r\n";

static size_t slen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

static int up(int c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == 0x1A;
}

static int hexval(int c)
{
    c = up(c);
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int equal_ci(const char *s, size_t len, const char *word)
{
    size_t i;

    if (len != slen(word))
        return 0;
    for (i = 0; i < len; i++)
        if (up((unsigned char)s[i]) != word[i])
            return 0;
    return 1;
}

/* Length of the line at s (up to and including '\n'). */
static size_t line_len(const char *s, size_t len)
{
    size_t i = 0;

    while (i < len && s[i] != '\n')
        i++;
    return i < len ? i + 1 : i;
}

/* Length without the line end. */
static size_t body_len(const char *s, size_t n)
{
    if (n && s[n - 1] == '\n')
        n--;
    if (n && s[n - 1] == '\r')
        n--;
    return n;
}

int bj_boot_serial(const unsigned char *bs, unsigned long *serial)
{
    /* BPB_FATSz16 is zero only on FAT32, whose extended BPB starts at 40h. */
    int ext = (bs[0x16] == 0 && bs[0x17] == 0) ? 0x42 : 0x26;

    if (bs[ext] != 0x29)
        return 0;
    *serial = (unsigned long)bs[ext + 1] | ((unsigned long)bs[ext + 2] << 8) |
              ((unsigned long)bs[ext + 3] << 16) |
              ((unsigned long)bs[ext + 4] << 24);
    return 1;
}

int bj_parse(const char *text, size_t len, bj_entry *e, int cap)
{
    size_t pos = 0;
    int n = 0;

    while (pos < len && text[pos] != 0x1A) {
        size_t ll = line_len(text + pos, len - pos), i = 0;
        size_t bl = body_len(text + pos, ll);
        const char *s = text + pos;
        bj_entry x;
        int k;

        pos += ll;
        while (i < bl && is_space((unsigned char)s[i]))
            i++;
        if (i == bl || s[i] == ';' || s[i] == 0x1A)
            continue;
        x.drive = (char)up((unsigned char)s[i]);
        if (x.drive < 'A' || x.drive > 'Z' || i + 1 >= bl || s[i + 1] != ':')
            return -1;
        i += 2;
        x.hasSerial = 0;
        x.serial = 0;
        while (i < bl && is_space((unsigned char)s[i]))
            i++;
        if (i < bl) {
            if (bl - i < 9 || s[i + 4] != '-')
                return -1;
            for (k = 0; k < 9; k++) {
                int h = hexval((unsigned char)s[i + k]);

                if (k == 4)
                    continue;
                if (h < 0)
                    return -1;
                x.serial = (x.serial << 4) | (unsigned long)h;
            }
            x.hasSerial = 1;
            i += 9;
            while (i < bl && is_space((unsigned char)s[i]))
                i++;
            if (i < bl)
                return -1;
        }
        if (bj_find(e, n, x.drive) < 0) {
            if (n >= cap)
                return -1;
            e[n++] = x;
        }
    }
    return n;
}

size_t bj_format(const bj_entry *e, int n, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t h = slen(job_head), pos = h;
    int i, k;

    if (cap < h + (size_t)n * 14 + 1)
        return BJ_ERR;
    memcpy(out, job_head, h);
    for (i = 0; i < n; i++) {
        out[pos++] = e[i].drive;
        out[pos++] = ':';
        if (e[i].hasSerial) {
            out[pos++] = ' ';
            for (k = 7; k >= 0; k--) {
                out[pos++] = hex[(e[i].serial >> (k * 4)) & 15];
                if (k == 4)
                    out[pos++] = '-';
            }
        }
        out[pos++] = '\r';
        out[pos++] = '\n';
    }
    out[pos] = 0;
    return pos;
}

int bj_find(const bj_entry *e, int n, char drive)
{
    int i;

    drive = (char)up((unsigned char)drive);
    for (i = 0; i < n; i++)
        if (e[i].drive == drive)
            return i;
    return -1;
}

int bj_set(bj_entry *e, int n, const bj_entry *x)
{
    int i = bj_find(e, n, x->drive);

    if (i < 0) {
        if (n >= BJ_MAX_DRIVES)
            return n;
        i = n++;
    }
    e[i] = *x;
    e[i].drive = (char)up((unsigned char)x->drive);
    return n;
}

int bj_remove(bj_entry *e, int n, char drive)
{
    int i = bj_find(e, n, drive);

    if (i < 0)
        return n;
    for (; i + 1 < n; i++)
        e[i] = e[i + 1];
    return n - 1;
}

size_t bj_line(const char *exePath, const char *jobPath, char *out, size_t cap)
{
    static const char *const part[] = {"@IF EXIST ", NULL, " ", NULL,
                                       " --boot ", NULL};
    size_t pos = 0;
    int i;

    for (i = 0; i < 6; i++) {
        const char *s = part[i] ? part[i] : i == 3 ? exePath : jobPath;
        size_t n = slen(s);

        if (pos + n >= cap || pos + n > BJ_MAX_LINE)
            return BJ_ERR;
        memcpy(out + pos, s, n);
        pos += n;
    }
    out[pos] = 0;
    return pos;
}

int bj_is_ours(const char *line, size_t len)
{
    const char *token[6], *base;
    size_t size[6], pos = 0, k, baseLen;
    int n;

    while (pos < len && is_space((unsigned char)line[pos]))
        pos++;
    if (pos < len && line[pos] == '@')
        pos++;
    for (n = 0; n < 6; n++) {
        while (pos < len && is_space((unsigned char)line[pos]))
            pos++;
        token[n] = line + pos;
        for (k = pos; pos < len && !is_space((unsigned char)line[pos]); pos++)
            ;
        size[n] = pos - k;
        if (!size[n])
            return 0;
    }
    while (pos < len && is_space((unsigned char)line[pos]))
        pos++;
    if (pos != len || !equal_ci(token[0], size[0], "IF") ||
        !equal_ci(token[1], size[1], "EXIST") ||
        !equal_ci(token[4], size[4], "--BOOT"))
        return 0;
    base = token[3];
    baseLen = size[3];
    for (k = 0; k < size[3]; k++)
        if (token[3][k] == '\\' || token[3][k] == '/' || token[3][k] == ':') {
            base = token[3] + k + 1;
            baseLen = size[3] - k - 1;
        }
    /* Only our invocation, not comments, ECHO text or similarly named tools. */
    return equal_ci(base, baseLen, "HSDEFRAG.EXE") ||
           equal_ci(base, baseLen, "HSDEFRAG");
}

int bj_autoexec_count(const char *text, size_t len)
{
    size_t pos = 0;
    int n = 0;

    while (pos < len) {
        size_t ll = line_len(text + pos, len - pos);

        n += bj_is_ours(text + pos, body_len(text + pos, ll));
        pos += ll;
    }
    return n;
}

/* "@ECHO OFF" with any case and surrounding blanks. */
static int is_echo_off(const char *s, size_t n)
{
    static const char want[] = "@ECHO OFF";
    size_t i = 0, k = 0;

    while (i < n && is_space((unsigned char)s[i]))
        i++;
    for (; k < sizeof(want) - 1; k++, i++) {
        if (want[k] == ' ') {
            if (i >= n || (s[i] != ' ' && s[i] != '\t'))
                return 0;
            while (i + 1 < n && (s[i + 1] == ' ' || s[i + 1] == '\t'))
                i++;
            continue;
        }
        if (i >= n || up((unsigned char)s[i]) != want[k])
            return 0;
    }
    while (i < n && is_space((unsigned char)s[i]))
        i++;
    return i == n;
}

static int put(char *out, size_t cap, size_t *pos, const char *s, size_t n)
{
    if (*pos + n + 1 > cap)
        return 0;
    memcpy(out + *pos, s, n);
    *pos += n;
    return 1;
}

size_t bj_autoexec_edit(const char *text, size_t len, const char *line,
                        char *out, size_t cap)
{
    size_t pos = 0, in = 0, first = line_len(text, len), at = 0;
    int done = line == NULL;

    if (cap == 0)
        return BJ_ERR;
    if (len && is_echo_off(text, body_len(text, first)))
        at = first;
    for (;;) {
        size_t ll;

        if (!done && in >= at) {
            if (pos && out[pos - 1] != '\n' && !put(out, cap, &pos, "\r\n", 2))
                return BJ_ERR;
            if (!put(out, cap, &pos, line, slen(line)) ||
                !put(out, cap, &pos, "\r\n", 2))
                return BJ_ERR;
            done = 1;
        }
        if (in >= len)
            break;
        ll = line_len(text + in, len - in);
        if (!bj_is_ours(text + in, body_len(text + in, ll)) &&
            !put(out, cap, &pos, text + in, ll))
            return BJ_ERR;
        in += ll;
    }
    out[pos] = 0;
    return pos;
}
