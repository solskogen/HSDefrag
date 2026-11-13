CC := i686-w64-mingw32-gcc
OBJDUMP ?= i686-w64-mingw32-objdump
HOSTCC ?= cc
CFLAGS := -O2 -Wall -Wextra -march=i486 -mtune=generic -mfpmath=387 \
          -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
          -D_WIN32_WINNT=0x0400 -DWINVER=0x0400
PEFLAGS := -nostdlib -Wl,--major-subsystem-version,4,--minor-subsystem-version,0 \
           -Wl,--major-os-version,4,--minor-os-version,0,--stack,2097152
DISTDIR := dist
TARGET := $(DISTDIR)/hsdefrag.exe
OBJDIR := build
NATIVE := $(OBJDIR)/hsdefrgb.exe
COMMON := ntfsmft ntfsrecord bootcfg bootlog filemove fatfs fatdefrag fatreport bootjob
WIN32 := main nostd util volume ntfsctl retrieval fileenum analyze defrag mft schedule fatwin9x boot9x
WINOBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(WIN32) $(COMMON))) $(OBJDIR)/embed.o
NATOBJS := $(addprefix $(OBJDIR)/native/,$(addsuffix .o,boot_native bootfiles ntrt bootcfg bootlog filemove ntfsrecord))
HEADERS := $(wildcard src/*.h)
HOSTFLAGS := -std=gnu99 -O2 -Wall -Wextra -Werror -Isrc
# hsdefrag.exe is one file for Windows and MS-DOS: tools/mkdual.c puts the
# MS-DOS build (DJGPP, on the CWSDPMI stub in dos/, patched never to swap to
# disk) behind the MS-DOS header of the Windows program.
#
# The compiler is looked for in this order: $(DJGPP)/bin (default
# ~/.local/djgpp), then i586-pc-msdosdjgpp-gcc and i686-pc-msdosdjgpp-gcc on
# the PATH (e.g. a distribution package). Release images should come from an
# i386/i586 DJGPP: an i686 runtime has cmov/fcmov in libgcc's exception
# unwinder, which a plain C program never runs, but which only a Pentium Pro
# or later could execute. Override with make DJCC=/path/to/gcc.
DJGPP ?= $(HOME)/.local/djgpp
DJCC ?= $(firstword $(wildcard $(DJGPP)/bin/i586-pc-msdosdjgpp-gcc) \
                    $(shell command -v i586-pc-msdosdjgpp-gcc 2>/dev/null) \
                    $(shell command -v i686-pc-msdosdjgpp-gcc 2>/dev/null))
DJTARGET = $(patsubst %-gcc,%,$(notdir $(DJCC)))
# Only needed for linkers that cannot write the COFF image themselves.
EXE2COFF ?= $(firstword $(wildcard $(DJGPP)/i586-pc-msdosdjgpp/bin/exe2coff \
                        /usr/$(DJTARGET)/bin/exe2coff) \
                        $(shell command -v exe2coff 2>/dev/null))
DJMISSING = echo "DJGPP not found: install it, or run make DJGPP=<directory with bin/i586-pc-msdosdjgpp-gcc> or make DJCC=<compiler>" >&2; exit 1
PEIMAGE := $(OBJDIR)/hsdefrag-win.exe
DOSCOFF := $(OBJDIR)/dos/hsdefrag
MKDUAL := $(OBJDIR)/mkdual
DOSSRC := $(addprefix src/,fatdos.c fatdisk.c fatfs.c fatdefrag.c fatreport.c bootjob.c)
DOSOBJ := $(addprefix $(OBJDIR)/dos/,$(addsuffix .o,$(notdir $(DOSSRC:.c=))))
DOSCFLAGS := -std=gnu99 -O2 -Wall -Wextra -march=i486 -Isrc
DOSFLAGS := $(DOSCFLAGS) -s
.PHONY: all windows dos clean dist test test-core test-wine test-floppy check-imports check-dos-insns test-mkdual
all: $(TARGET)

# dist/ is made by the recipes: "dist" is also the name of a phony target.
$(TARGET): $(PEIMAGE) dos/cwsdstub.exe $(DOSCOFF) $(MKDUAL)
	@mkdir -p $(DISTDIR)
	$(MKDUAL) $(PEIMAGE) dos/cwsdstub.exe $(DOSCOFF) $@

# One-toolchain builds. make windows needs only MinGW: the Windows program
# with the NT boot helper, but no MS-DOS part, so it refuses to schedule
# the Windows 9x boot-time pass (which runs hsdefrag.exe as an MS-DOS
# program). make dos needs only DJGPP: the CWSDPMI stub and the MS-DOS
# program. Each goes in its own folder under its usual 8.3 name.
WINONLY := $(DISTDIR)/win/hsdefrag.exe
DOSONLY := $(DISTDIR)/dos/hsdefrag.exe
WINONLYOBJS := $(filter-out $(OBJDIR)/boot9x.o,$(WINOBJS)) $(OBJDIR)/winonly/boot9x.o

windows: $(WINONLY)
dos: $(DOSONLY)

$(WINONLY): $(WINONLYOBJS)
	@mkdir -p $(dir $@)
	$(CC) $(PEFLAGS) -municode -Wl,--subsystem,console,-e,_wmainCRTStartup \
	    -o $@ $^ -lkernel32 -lgcc

$(OBJDIR)/winonly/boot9x.o: src/boot9x.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DHSD_WINDOWS_ONLY -c -o $@ $<

$(DOSONLY): dos/cwsdstub.exe $(DOSCOFF)
	@mkdir -p $(dir $@)
	cat dos/cwsdstub.exe $(DOSCOFF) > $@

$(PEIMAGE): $(WINOBJS)
	$(CC) $(PEFLAGS) -municode -Wl,--subsystem,console,-e,_wmainCRTStartup \
	    -o $@ $^ -lkernel32 -lgcc

$(MKDUAL): tools/mkdual.c | $(OBJDIR)
	$(HOSTCC) $(HOSTFLAGS) -o $@ $<

$(NATIVE): $(NATOBJS)
	$(CC) $(PEFLAGS) -Wl,--subsystem,native,-e,_NtProcessStartup@4 \
	    -o $@ $^ -lntdll -lgcc

$(OBJDIR)/embed.o: src/embed.S $(NATIVE) | $(OBJDIR)
	$(CC) -DHSD_NATIVE_IMAGE=\"$(abspath $(NATIVE))\" -c -o $@ $<

$(OBJDIR)/dos/%.o: src/%.c $(HEADERS) | $(OBJDIR)/dos
	@test -n "$(DJCC)" && test -x "$(DJCC)" || { $(DJMISSING); }
	$(DJCC) $(DOSCFLAGS) -c -o $@ $<

# The MS-DOS image is the COFF program the CWSDPMI stub loads. The linker
# writes it directly (--oformat coff-go32); a linker that does not know that
# option needs exe2coff to take the image out of the linked program.
$(DOSCOFF): $(DOSOBJ) | $(OBJDIR)/dos
	@test -n "$(DJCC)" && test -x "$(DJCC)" || { $(DJMISSING); }
	@if $(DJCC) $(DOSFLAGS) -Wl,--oformat,coff-go32 -o $@ $(DOSOBJ) 2>/dev/null; then \
	    echo "  LD    $@ (COFF image)"; \
	elif test -x "$(EXE2COFF)"; then \
	    echo "  LD    $(OBJDIR)/dos/hsdefrag.exe"; \
	    $(DJCC) $(DOSFLAGS) -o $(OBJDIR)/dos/hsdefrag.exe $(DOSOBJ) && \
	    (cd $(OBJDIR)/dos && $(EXE2COFF) hsdefrag.exe); \
	else \
	    echo "the DJGPP linker needs --oformat coff-go32, or exe2coff" >&2; \
	    exit 1; \
	fi

$(OBJDIR)/%.o: src/%.c $(HEADERS) | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/native/%.o: src/%.c $(HEADERS) | $(OBJDIR)/native
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR) $(OBJDIR)/native $(OBJDIR)/dos:
	mkdir -p $@

$(OBJDIR)/test-bootlog: tests/bootlog.c src/bootlog.c src/ntfsrecord.c $(HEADERS) | $(OBJDIR)
	$(HOSTCC) $(HOSTFLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $@ tests/bootlog.c src/bootlog.c src/ntfsrecord.c

$(OBJDIR)/test-core: tests/core.c src/ntfsmft.c src/ntfsrecord.c src/bootcfg.c src/bootlog.c $(HEADERS) | $(OBJDIR)
	$(HOSTCC) $(HOSTFLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $@ tests/core.c src/ntfsrecord.c src/bootcfg.c src/bootlog.c

$(OBJDIR)/test-argv: tests/argv.c src/nostd.c $(HEADERS) | $(OBJDIR)
	$(HOSTCC) $(HOSTFLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -fno-builtin -o $@ tests/argv.c

$(OBJDIR)/test-moves: tests/move_core.c src/filemove.c $(HEADERS) | $(OBJDIR)
	$(HOSTCC) $(HOSTFLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $@ tests/move_core.c src/filemove.c

$(OBJDIR)/test-diskio: tests/diskio.c src/fatdisk.c $(HEADERS) | $(OBJDIR)
	$(HOSTCC) $(HOSTFLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $@ tests/diskio.c src/fatdisk.c

$(OBJDIR)/test-fat: tests/fat.c src/fatfs.c src/fatdefrag.c $(HEADERS) | $(OBJDIR)
	$(HOSTCC) $(HOSTFLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $@ tests/fat.c src/fatfs.c src/fatdefrag.c

$(OBJDIR)/test-report: tests/report.c src/fatreport.c $(HEADERS) | $(OBJDIR)
	$(HOSTCC) $(HOSTFLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $@ tests/report.c src/fatreport.c

$(OBJDIR)/test-bootjob: tests/bootjob.c src/bootjob.c $(HEADERS) | $(OBJDIR)
	$(HOSTCC) $(HOSTFLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $@ tests/bootjob.c src/bootjob.c

$(OBJDIR)/test-online.exe: tests/online.c $(addprefix src/,$(addsuffix .c,filemove volume retrieval fileenum defrag mft analyze util nostd ntfsctl ntfsrecord ntfsmft)) $(HEADERS) | $(OBJDIR)
	$(CC) $(CFLAGS) -Werror -Isrc $(PEFLAGS) -municode \
	    -Wl,--subsystem,console,-e,_wmainCRTStartup -o $@ tests/online.c \
	    src/nostd.c src/util.c src/ntfsctl.c src/ntfsrecord.c src/ntfsmft.c -lkernel32 -lgcc

$(OBJDIR)/test-output.exe: tests/output.c src/util.c src/nostd.c $(HEADERS) | $(OBJDIR)
	$(CC) $(CFLAGS) -Werror -Isrc $(PEFLAGS) -municode \
	    -Wl,--subsystem,console,-e,_wmainCRTStartup -o $@ tests/output.c \
	    src/nostd.c -lkernel32 -lgcc

$(OBJDIR)/test-boot9x.exe: tests/boot9x.c src/boot9x.c src/bootjob.c src/util.c src/nostd.c $(HEADERS) | $(OBJDIR)
	$(CC) $(CFLAGS) -Werror -Isrc $(PEFLAGS) -municode \
	    -Wl,--subsystem,console,-e,_wmainCRTStartup -o $@ tests/boot9x.c \
	    src/nostd.c src/bootjob.c -lkernel32 -lgcc

$(OBJDIR)/test-boot9x-winonly.exe: tests/boot9x.c src/boot9x.c src/bootjob.c src/util.c src/nostd.c $(HEADERS) | $(OBJDIR)
	$(CC) $(CFLAGS) -Werror -Wno-unused-function -DHSD_WINDOWS_ONLY -Isrc $(PEFLAGS) -municode \
	    -Wl,--subsystem,console,-e,_wmainCRTStartup -o $@ tests/boot9x.c \
	    src/nostd.c src/bootjob.c -lkernel32 -lgcc

$(OBJDIR)/test-win9x.exe: tests/win9x.c src/fatwin9x.c src/util.c src/nostd.c src/fatfs.c src/fatdefrag.c src/fatreport.c $(HEADERS) | $(OBJDIR)
	$(CC) $(CFLAGS) -Werror -Isrc $(PEFLAGS) -municode \
	    -Wl,--subsystem,console,-e,_wmainCRTStartup -o $@ tests/win9x.c \
	    src/nostd.c src/fatfs.c src/fatdefrag.c src/fatreport.c -lkernel32 -lgcc

$(OBJDIR)/test-native.exe: tests/native.c src/boot_native.c src/ntrt.c src/bootcfg.c src/bootlog.c src/ntfsrecord.c $(HEADERS) | $(OBJDIR)
	$(CC) $(CFLAGS) -Werror -Isrc -municode -o $@ tests/native.c src/bootcfg.c \
	    src/bootlog.c src/ntfsrecord.c \
	    -lkernel32 -lntdll

$(OBJDIR)/test-bootfiles.exe: tests/bootfiles.c src/bootfiles.c src/filemove.c src/ntrt.c src/bootlog.c src/ntfsrecord.c $(HEADERS) | $(OBJDIR)
	$(CC) $(CFLAGS) -Werror -Isrc -municode -o $@ tests/bootfiles.c \
	    src/bootlog.c src/ntfsrecord.c -lkernel32 -lntdll

$(OBJDIR)/test-schedule.exe: tests/schedule.c src/schedule.c $(OBJDIR)/embed.o $(HEADERS) $(addprefix src/,$(addsuffix .c,nostd util ntfsrecord bootcfg bootlog)) | $(OBJDIR)
	$(CC) $(CFLAGS) -Werror -Isrc $(PEFLAGS) -municode \
	    -Wl,--subsystem,console,-e,_wmainCRTStartup -o $@ tests/schedule.c \
	    src/nostd.c src/util.c src/ntfsrecord.c src/bootcfg.c src/bootlog.c \
	    $(OBJDIR)/embed.o -lkernel32 -lgcc

$(OBJDIR)/test-cli.exe: tests/cli.c src/main.c $(filter-out $(OBJDIR)/main.o,$(WINOBJS)) $(HEADERS) | $(OBJDIR)
	$(CC) $(CFLAGS) -Werror -Isrc $(PEFLAGS) -municode \
	    -Wl,--subsystem,console,-e,_wmainCRTStartup -o $@ tests/cli.c \
	    $(filter-out $(OBJDIR)/main.o,$(WINOBJS)) -lkernel32 -lgcc

test-core: $(OBJDIR)/test-report $(OBJDIR)/test-core $(OBJDIR)/test-argv $(OBJDIR)/test-moves $(OBJDIR)/test-bootlog $(OBJDIR)/test-fat $(OBJDIR)/test-bootjob $(OBJDIR)/test-diskio
	$(OBJDIR)/test-core
	$(OBJDIR)/test-argv
	$(OBJDIR)/test-moves
	$(OBJDIR)/test-bootlog
	$(OBJDIR)/test-fat
	$(OBJDIR)/test-bootjob
	$(OBJDIR)/test-diskio
	$(OBJDIR)/test-report

# The Windows images import only KERNEL32 / NTDLL and stay i486 code.
check-imports: $(PEIMAGE) $(NATIVE)
	sh tests/imports.sh $(OBJDUMP) $(PEIMAGE) $(NATIVE)

# The MS-DOS image holds only i486 code (an unstripped link of the same
# objects, so instructions can be attributed to functions).
DJOBJDUMP = $(firstword $(wildcard $(dir $(DJCC))../$(DJTARGET)/bin/objdump \
                        /usr/$(DJTARGET)/bin/objdump) \
                        $(shell command -v $(DJTARGET)-objdump 2>/dev/null))
$(OBJDIR)/dos/hsdefrag.sym: $(DOSOBJ)
	$(DJCC) $(DOSCFLAGS) -Wl,--oformat,coff-go32 -o $@ $(DOSOBJ)
check-dos-insns: $(OBJDIR)/dos/hsdefrag.sym
	sh tests/dosinsns.sh $(DJOBJDUMP) $<

# mkdual on the real PE image and stub with a stand-in COFF (no DJGPP).
test-mkdual: $(PEIMAGE) $(MKDUAL) dos/cwsdstub.exe
	sh tests/mkdual.sh $(MKDUAL) $(OBJDUMP) $(PEIMAGE) dos/cwsdstub.exe

test-wine: all check-imports check-dos-insns test-mkdual $(OBJDIR)/test-online.exe $(OBJDIR)/test-output.exe $(OBJDIR)/test-native.exe $(OBJDIR)/test-bootfiles.exe $(OBJDIR)/test-schedule.exe $(OBJDIR)/test-cli.exe $(OBJDIR)/test-win9x.exe $(OBJDIR)/test-boot9x.exe $(OBJDIR)/test-boot9x-winonly.exe
	sh tests/wine.sh $(abspath $(OBJDIR)) $(abspath $(TARGET))

test: test-core test-wine

# A badly fragmented 1.44 MB FAT12 floppy image of dummy files, for trying
# hsdefrag by hand (made with mtools; see tests/mkfloppy.sh).
test-floppy:
	@mkdir -p $(OBJDIR)
	sh tests/mkfloppy.sh $(OBJDIR)/test-floppy.img
	@echo "  $(OBJDIR)/test-floppy.img: a fragmented 1.44 MB FAT12 floppy image."
	@echo "  Write it to a floppy and run hsdefrag a: -a, then hsdefrag a: -d."

# Release files in dist/: the program, a zip and a 1.44 MB floppy image,
# each with every licence and notice that must travel with hsdefrag.exe
# (CWSDPMI and DJGPP require their terms and the location of their
# sources; dos/README gives it). Licence files are copied verbatim. The
# floppy is a plain data disk (not bootable), made with mtools.
VERSION := $(shell sed -n 's/^\#define HSD_VERSION "\(.*\)"/\1/p' src/version.h)
DIST := hsdefrag-$(VERSION)
DISTZIP := $(DISTDIR)/$(DIST).zip
DISTIMG := $(DISTDIR)/$(DIST).img
STAGE := $(OBJDIR)/$(DIST)
DISTDOCS := docs/DESIGN.md docs/QUALIFY.md
DISTFILES := README.md LICENSE $(DISTDOCS) dos/README dos/COPYING \
             dos/COPYING.CWS dos/COPYING.DJ dos/COPYING.LIB

dist: $(DISTZIP) $(DISTIMG)

$(STAGE): $(TARGET) $(DISTFILES)
	rm -rf $@
	mkdir -p $@/DOS $@/DOCS
	cp $(TARGET) $@/HSDEFRAG.EXE
	for f in $(DISTDOCS); do \
	    b=$$(basename $$f .md | tr a-z A-Z); cp $$f $@/DOCS/$$b.MD; done
	cp README.md $@/README.MD
	cp LICENSE $@/LICENSE
	cp dos/README dos/COPYING dos/COPYING.CWS dos/COPYING.DJ dos/COPYING.LIB \
	    $@/DOS/

$(DISTZIP): $(STAGE)
	rm -f $@
	@cd $(OBJDIR) && if command -v 7z >/dev/null 2>&1; then \
	    7z a -tzip -mx=9 -bd -bso0 ../$@ $(DIST); \
	elif command -v zip >/dev/null 2>&1; then \
	    zip -q -r -X ../$@ $(DIST); \
	elif command -v bsdtar >/dev/null 2>&1; then \
	    bsdtar -a -cf ../$@ $(DIST); \
	elif command -v python3 >/dev/null 2>&1; then \
	    python3 -I -m zipfile -c ../$@ $(DIST); \
	else echo "dist: need 7z, zip, bsdtar or python3" >&2; exit 1; fi
	@echo "  DIST  $@"

$(DISTIMG): $(STAGE)
	@command -v mformat >/dev/null 2>&1 || { echo "dist: the floppy image needs mtools (mformat, mcopy)" >&2; exit 1; }
	rm -f $@
	MTOOLS_SKIP_CHECK=1 mformat -i $@ -C -f 1440 -v HSDEFRAG ::
	MTOOLS_SKIP_CHECK=1 mcopy -i $@ $(STAGE)/HSDEFRAG.EXE $(STAGE)/README.MD \
	    $(STAGE)/LICENSE ::
	MTOOLS_SKIP_CHECK=1 mmd -i $@ ::DOS
	MTOOLS_SKIP_CHECK=1 mcopy -i $@ $(STAGE)/DOS/* ::DOS
	MTOOLS_SKIP_CHECK=1 mmd -i $@ ::DOCS
	MTOOLS_SKIP_CHECK=1 mcopy -i $@ $(STAGE)/DOCS/* ::DOCS
	@# Every file must read back exactly as staged.
	@for f in HSDEFRAG.EXE README.MD LICENSE DOS/README DOS/COPYING \
	    DOS/COPYING.CWS DOS/COPYING.DJ DOS/COPYING.LIB \
	    DOCS/DESIGN.MD DOCS/QUALIFY.MD; do \
	    MTOOLS_SKIP_CHECK=1 mtype -i $@ ::$$f | cmp -s - $(STAGE)/$$f || \
	    { echo "dist: $$f differs on the floppy image" >&2; rm -f $@; exit 1; }; \
	done
	@echo "  DIST  $@"

clean:
	rm -rf $(OBJDIR)/hsdefrag-*/ $(DISTDIR)
	rm -rf $(OBJDIR)/winonly
	rm -f $(WINOBJS) $(NATOBJS) $(TARGET) $(NATIVE) $(PEIMAGE) $(MKDUAL) \
	    $(DOSCOFF) $(DOSCOFF).exe $(OBJDIR)/dos/hsdefrag.sym \
	    $(OBJDIR)/test-bootjob $(OBJDIR)/test-boot9x.exe $(OBJDIR)/test-boot9x-winonly.exe \
	    $(OBJDIR)/test-core $(OBJDIR)/test-online.exe \
	    $(OBJDIR)/test-argv $(OBJDIR)/test-output.exe $(OBJDIR)/test-native.exe \
	    $(OBJDIR)/test-schedule.exe $(OBJDIR)/test-cli.exe $(OBJDIR)/test-win9x.exe \
	    $(OBJDIR)/test-bootfiles.exe $(OBJDIR)/test-moves $(OBJDIR)/test-bootlog \
	    $(OBJDIR)/test-fat $(OBJDIR)/test-diskio $(OBJDIR)/test-report $(DOSOBJ)
