/*
 * mkdual.c - Build hsdefrag.exe as one program for Windows and MS-DOS.
 *
 * Every Windows program begins with an MS-DOS header: MS-DOS runs the program
 * that header describes, Windows follows e_lfanew to the PE header. Here the
 * MS-DOS side is the DJGPP build of hsdefrag: the CWSDSTUB loader (CWSDPMI
 * built in) and the COFF image it loads. The stub finds the COFF image at the
 * end of the load module named by the MS-DOS header, but computes that offset
 * in 16 bits, so the COFF image must start in the first 64 KB. The result:
 *
 *   0               MS-DOS header (fields of the stub; e_lfanew kept)
 *   e_lfanew        PE headers and section table, as linked
 *   SizeOfHeaders   relocation table of the stub
 *                   stub load module (paragraph aligned)
 *                   COFF image
 *   FileAlignment   the PE sections and symbols, moved up as one block
 *
 * usage: mkdual <linked PE> <cwsdstub.exe> <COFF image> <output>
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_out;

static void die(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "mkdual: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (g_out)
        remove(g_out);
    exit(1);
}

static unsigned char *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f)
        die("cannot open %s", path);
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0)
        die("cannot read %s", path);
    buf = (unsigned char *)malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n)
        die("cannot read %s", path);
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static unsigned rd16(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16;
}

static void wr16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void wr32(unsigned char *p, uint32_t v)
{
    wr16(p, v & 0xFFFFu);
    wr16(p + 2, v >> 16);
}

static size_t align(size_t v, size_t a)
{
    return (v + a - 1) / a * a;
}

/* The PE checksum (as CheckSumMappedFile), with the field itself zero. */
static uint32_t pe_checksum(const unsigned char *p, size_t len)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += rd16(p + i);
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    if (len & 1) {
        sum += p[len - 1];
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    sum = (sum & 0xFFFFu) + (sum >> 16);
    return sum + (uint32_t)len;
}

int main(int argc, char **argv)
{
    static const unsigned keep[] = {0x0A, 0x0C, 0x0E, 0x10, 0x14, 0x16};
    unsigned char *pe, *stub, *coff, *out;
    size_t peLen, stubLen, coffLen, outLen, first, relOff, modOff, coffOff;
    size_t sectOff, delta, modLen, stubHdr, stubEnd, k;
    uint32_t lfanew, opt, sectab, fileAlign, sizeHdr, nDirs, symtab;
    unsigned nsec, optSize, crlc, lfarlc, cp, cblp, i;
    FILE *f;

    if (argc != 5) {
        fprintf(stderr, "usage: mkdual <linked PE> <cwsdstub.exe> "
                        "<COFF image> <output>\n");
        return 2;
    }
    pe = load(argv[1], &peLen);
    stub = load(argv[2], &stubLen);
    coff = load(argv[3], &coffLen);

    /* The PE file as the linker wrote it. */
    if (peLen < 0x40 || pe[0] != 'M' || pe[1] != 'Z')
        die("%s: not an MZ file", argv[1]);
    lfanew = rd32(pe + 0x3C);
    if (lfanew < 0x40 || lfanew > peLen - 24 ||
        memcmp(pe + lfanew, "PE\0\0", 4) != 0)
        die("%s: not a PE file", argv[1]);
    nsec = rd16(pe + lfanew + 6);
    symtab = rd32(pe + lfanew + 12);
    optSize = rd16(pe + lfanew + 20);
    opt = lfanew + 24;
    if (optSize < 96 || opt + optSize > peLen || rd16(pe + opt) != 0x10B)
        die("%s: not a 32-bit PE image", argv[1]);
    fileAlign = rd32(pe + opt + 36);
    sizeHdr = rd32(pe + opt + 60);
    nDirs = rd32(pe + opt + 92);
    sectab = opt + optSize;
    if (fileAlign < 512 || (fileAlign & (fileAlign - 1)) ||
        sizeHdr % fileAlign || sizeHdr > peLen ||
        sectab + 40u * nsec > sizeHdr)
        die("%s: unexpected PE header layout", argv[1]);
    /* The certificate and debug directories hold file offsets. */
    for (i = 4; i <= 6; i += 2)
        if (nDirs > i && 96 + 8 * i + 8 <= optSize &&
            rd32(pe + opt + 96 + 8 * i + 4) != 0)
            die("%s: has a %s directory", argv[1],
                i == 4 ? "certificate" : "debug");
    first = peLen;
    for (i = 0; i < nsec; i++) {
        const unsigned char *s = pe + sectab + 40 * i;
        uint32_t size = rd32(s + 16), raw = rd32(s + 20);

        if (rd32(s + 24) || rd32(s + 28))
            die("%s: section with COFF relocations", argv[1]);
        if (!size || !raw)
            continue;
        if (raw < sizeHdr || raw % fileAlign || raw > peLen ||
            size > peLen - raw)
            die("%s: unexpected section layout", argv[1]);
        if (raw < first)
            first = raw;
    }
    if (first == peLen)
        die("%s: no section data", argv[1]);
    if (symtab && (symtab < first || symtab >= peLen))
        die("%s: unexpected symbol table", argv[1]);
    for (k = sizeHdr; k < first; k++)
        if (pe[k])
            die("%s: data between the headers and the sections", argv[1]);

    /* The stub: header, relocations, load module. */
    if (stubLen < 0x1C || stub[0] != 'M' || stub[1] != 'Z')
        die("%s: not an MZ file", argv[2]);
    cblp = rd16(stub + 2);
    cp = rd16(stub + 4);
    crlc = rd16(stub + 6);
    stubHdr = (size_t)rd16(stub + 8) * 16;
    lfarlc = rd16(stub + 0x18);
    stubEnd = (size_t)cp * 512 - (cblp ? 512 - cblp : 0);
    if (!cp || cblp >= 512 || stubEnd > stubLen || stubHdr >= stubEnd ||
        lfarlc + 4u * crlc > stubHdr)
        die("%s: unexpected MS-DOS header", argv[2]);
    modLen = stubEnd - stubHdr;

    if (coffLen < 20 || rd16(coff) != 0x14C)
        die("%s: not a DJGPP COFF image", argv[3]);

    relOff = sizeHdr;
    modOff = align(relOff + 4u * crlc, 16);
    coffOff = modOff + modLen;
    if (coffOff > 0xFFFF)
        die("the COFF image would start beyond 64 KB, where the stub cannot "
            "find it");
    sectOff = align(coffOff + coffLen, fileAlign);
    if (sectOff < first)
        sectOff = first;
    delta = sectOff - first;
    outLen = sectOff + (peLen - first);

    out = (unsigned char *)calloc(outLen, 1);
    if (!out)
        die("out of memory");
    memcpy(out, pe, sizeHdr);
    memcpy(out + relOff, stub + lfarlc, 4u * crlc);
    memcpy(out + modOff, stub + stubHdr, modLen);
    memcpy(out + coffOff, coff, coffLen);
    memcpy(out + sectOff, pe + first, peLen - first);

    for (i = 0; i < nsec; i++) {
        unsigned char *s = out + sectab + 40 * i;

        if (rd32(s + 16) && rd32(s + 20))
            wr32(s + 20, rd32(s + 20) + (uint32_t)delta);
    }
    if (symtab)
        wr32(out + lfanew + 12, symtab + (uint32_t)delta);

    /* MS-DOS header: the stub's load module, its relocations and entry. */
    wr16(out + 0x02, (unsigned)(coffOff % 512));
    wr16(out + 0x04, (unsigned)((coffOff + 511) / 512));
    wr16(out + 0x06, crlc);
    wr16(out + 0x08, (unsigned)(modOff / 16));
    for (i = 0; i < sizeof(keep) / sizeof(keep[0]); i++)
        wr16(out + keep[i], rd16(stub + keep[i]));
    wr16(out + 0x12, 0);
    wr16(out + 0x18, (unsigned)relOff);
    wr16(out + 0x1A, 0);

    if (rd32(pe + opt + 64)) {
        wr32(out + opt + 64, 0);
        wr32(out + opt + 64, pe_checksum(out, outLen));
    }

    g_out = argv[4];
    f = fopen(argv[4], "wb");
    if (!f)
        die("cannot create %s", argv[4]);
    if (fwrite(out, 1, outLen, f) != outLen || fclose(f) != 0)
        die("cannot write %s", argv[4]);
    return 0;
}
