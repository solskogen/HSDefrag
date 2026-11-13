---
name: hsdefrag-dev
description: How to build, test and safely change House Soft Defrag (hsdefrag.exe), the NT 4.0 / Windows 9x / MS-DOS volume defragmenter in C. Use when editing anything under src/, tests/, tools/ or the Makefile; when adding CLI options, changing the NTFS move engine, the FAT engine, boot-time scheduling (BootExecute or AUTOEXEC.BAT); or when running make, make test-core or test-wine.
---

# House Soft Defrag development

Rules and workflow for changing this codebase. `README.md` is the user documentation; read it when behaviour details matter.

## 1. What the project is

`hsdefrag.exe` is a whole-volume defragmenter that runs on **Windows NT 4.0,
Windows 95/98/Me and MS-DOS** from one file:

| Platform | Engine | Disk access |
|---|---|---|
| NT 4.0 (online) | NTFS/FAT through NT's defrag FSCTLs | `NtFsControlFile`: `FSCTL_GET_VOLUME_BITMAP`, `FSCTL_GET_RETRIEVAL_POINTERS`, `FSCTL_MOVE_FILE` |
| NT 4.0 (boot) | Native NTDLL-only helper `hsdefrgb.exe`, run from `BootExecute` | Same FSCTLs, before Win32 starts |
| Windows 9x | Its own FAT12/16/32 engine | Raw sectors through VWIN32 (`Int 21h AX=7305h`, `Int 25h/26h`) plus volume locks |
| MS-DOS | The same FAT engine, built with DJGPP | `Int 21h AX=7305h` / `Int 25h/26h` from DPMI |

The tool's goal is **to never corrupt a volume**. Correctness and verification
come before speed and before how complete the defrag is.

## 2. Source map

```
src/
  main.c            CLI parsing, picks the OS engine (NT vs 9x), exit codes
  nostd.[ch]        Freestanding libc replacement + wmainCRTStartup (KERNEL32 only)
  util.[ch]         Console output, wide printf, error reporting (no stdio)
  compat.h          Pins _WIN32_WINNT=0x0400; fallback FSCTL definitions
  version.h         HSD_VERSION

  -- NT online (Win32) --
  volume.[ch]       Open volume, geometry, cluster bitmap, excluded ranges
  ntfsctl.[ch]      NtFsControlFile wrappers (gives exact NTSTATUS values)
  retrieval.[ch]    VCN->LCN extent maps
  fileenum.[ch]     Recursive Unicode enumeration
  analyze.[ch]      Fragmentation report
  defrag.[ch]       Volume defrag driver
  mft.[ch]          Read-only $MFT report (-m)
  schedule.[ch]     --boot-* on NT: installs helper, edits BootExecute (ADVAPI32 loaded dynamically)
  embed.S           .incbin of build/hsdefrgb.exe into the Win32 image

  -- Shared NT (online + native) --
  filemove.[ch]     Move engine: best-fit free runs, fallback consolidation, refused-target tracking
  ntfsrecord.[ch]   NTFS record/attribute parsing
  ntfsmft.[ch]      Read-only $MFT validation (callback-based I/O)
  bootcfg.[ch]      BootExecute MULTI_SZ editing (keeps unrelated entries byte-for-byte)
  bootlog.[ch]      4-KiB checksummed boot result record (C:\hsdefrag.boot)

  -- NT native boot helper (NTDLL only) --
  boot_native.c     NtProcessStartup entry, pending/running/completed state machine
  bootfiles.c       Boot-time stream enumeration + moves
  ntrt.[ch]         Native runtime: RtlAllocateHeap, NtDisplayString

  -- FAT (portable, no Win32) --
  fatfs.[ch]        FAT12/16/32 reader/writer over abstract `fat_dev`
  fatdefrag.[ch]    Analysis, planning, moves, free-space consolidation (largest file)
  bootjob.[ch]      9x job file + AUTOEXEC.BAT line text handling (Win32 and DOS)

  -- FAT front ends --
  fatwin9x.[ch]     9x: VWIN32 sector I/O, lock levels 0-3, files-in-use lists
  boot9x.[ch]       9x --boot-defrag: copies the exe, writes job and AUTOEXEC.BAT
  fatdos.c          MS-DOS main (DJGPP): DPMI thunks, locking, SMARTDRV coherence
  fatdisk.[ch]      DOS absolute disk request layout + SFT/CDS entry decoding (host-tested)

tools/mkdual.c      Merges PE image + CWSDPMI stub + DJGPP COFF into one hsdefrag.exe
dos/                cwsdstub.exe (checked in) + licences; ship COPYING* with the exe
docs/               DESIGN.md (internals, safety rules, removed features), TESTING.md,
                    QUALIFY.md (real-hardware checklist);
                    keep names 8.3: DESIGN/QUALIFY ship on the floppy
tests/              Host (ASan/UBSan) tests, Wine tests, mkfloppy.sh (test floppy)
build/              Intermediate outputs and many old *.log files (gitignored)
dist/               hsdefrag.exe, release zip and floppy image (gitignored)
```

Layering rule: `fatfs.c`, `fatdefrag.c`, `bootjob.c`, `filemove.c`,
`ntfsrecord.c`, `ntfsmft.c`, `bootcfg.c` and `bootlog.c` are **portable cores**
that the host tests build with the system `cc`. Keep OS calls out of them.
They get I/O through callbacks or abstract devices (`fat_dev`, the
`ntfsmft` read callback, the filemove ops).

## 3. Toolchain and build

Requirements:
- `i686-w64-mingw32-gcc` for the Win32 image and the native helper.
- Host `cc` (gcc/clang with ASan/UBSan) for `tools/mkdual` and the host tests.
- **DJGPP** for the MS-DOS part. The Makefile finds it on its own:
  `$(DJGPP)/bin/i586-pc-msdosdjgpp-gcc` (default `~/.local/djgpp`), then
  `i586-` and `i686-pc-msdosdjgpp-gcc` on the PATH (the Arch `djgpp-gcc`
  package installs the i686 one in `/usr/bin`). `make DJCC=...` overrides.
  Any of them gives a pure i486 DOS image: `fatdos.c` defines empty
  `__register_frame_info`/`__deregister_frame_info` so libgcc's unwinder
  (cmov in i686 builds) is never linked; `make check-dos-insns` enforces it.
- `wine` for `make test-wine`; mtools for `make dist` and `make test-floppy`.

```sh
make              # dist/hsdefrag.exe (needs MinGW and DJGPP)
make windows      # dist/win/hsdefrag.exe, MinGW only (boot9x.c built with -DHSD_WINDOWS_ONLY:
                  #   9x --boot-defrag refuses, as there is no MS-DOS part to run)
make dos          # dist/dos/hsdefrag.exe, DJGPP only (cwsdstub.exe + the COFF image)
make dist         # + dist/hsdefrag-<ver>.zip and .img (1.44 MB floppy; needs mtools, 7z preferred)
make test-core    # host unit tests under ASan/UBSan; no DJGPP or Wine needed
make test-wine    # builds everything (needs DJGPP), runs Win32 tests under Wine
make test-floppy  # build/test-floppy.img: a fragmented floppy for trying hsdefrag by hand
make test         # test-core + test-wine
make check-imports  # KERNEL32-only / NTDLL-only imports, no cmov/SSE (no DJGPP needed)
make test-mkdual    # mkdual layout check with a stand-in COFF (no DJGPP needed)
make check-dos-insns  # MS-DOS image is i486-only (needs DJGPP)
make clean
```

Build pipeline:
1. `build/native/*.o` → `build/hsdefrgb.exe` (subsystem native, `-lntdll`).
2. `embed.S` pulls that helper into `build/embed.o`.
3. Win32 objects → `build/hsdefrag-win.exe` (console, `-lkernel32 -lgcc` only).
4. DJGPP objects → `build/dos/hsdefrag` COFF (`--oformat coff-go32`, or `exe2coff`).
5. `build/mkdual` merges them into `dist/hsdefrag.exe`.

Headers are a dependency of every object (`$(HEADERS)`). When you add a
source file, add it to `COMMON`, `WIN32`, `NATOBJS` or `DOSSRC`, and to each
test target that links it.

## 4. Hard constraints (do not break these)

**Binary compatibility**
- Target is NT 4.0 / Win95: `-march=i486 -mfpmath=387`, PE subsystem 4.0.
  No SSE, no `cmov`, no APIs newer than NT4/Win95. If an API might be missing,
  load it dynamically (as `schedule.c` does with ADVAPI32).
- The Win32 image is `-nostdlib -ffreestanding -fno-builtin`. **No CRT, no
  stdio, no `<string.h>` from the toolchain.** Use `nostd.h` (malloc, mem*,
  wcs*) and `util.h` (`uinfo`, the wide printf). If you need a libc function,
  add a minimal version to `nostd.c` (and `ntrt.c` for the native helper).
- **No floating point anywhere** (Win32, native, DOS): format numbers with
  integers (`uformat_bytes`, `fat_format_bytes`). `u_vsnwprintf` has no
  `%f`. `check-imports` fails on any x87 instruction in the Windows images.
- `-fno-tree-loop-distribute-patterns` stops GCC turning loops into calls to
  `memset`/`memcpy`. Keep it.
- The Win32 exe imports **KERNEL32 only**. The native helper imports
  **NTDLL only**: no KERNEL32 at all, its output goes through `NtDisplayString`.
- Entry points: `_wmainCRTStartup` (in `nostd.c`, `-municode`) and
  `_NtProcessStartup@4`.
- Code is C (gnu99 for host/DOS). Host test builds use `-Werror`, as do all
  Wine test targets, so keep the code warning-free.

**Data safety**
- NT: data moves **only** through `FSCTL_MOVE_FILE` (the filesystem moves
  it). Never add raw NTFS metadata writes, volume locks/dismounts at boot, MFT
  relocation or an "experimental" path. These were removed on purpose
  (README "Removed functionality"). `-m` stays read-only.
- Verify every move against a **fresh** extent map. Report a partial
  reduction as "partially improved", never as defragmented (exit 4).
- FAT: keep **copy-before-switch** ordering: copy → read back → link new FAT
  chain → switch dir entry / previous FAT entry → free old clusters. Read back
  every metadata write. Keep FAT mirrors identical and respect `BPB_ExtFlags`
  (the active FAT).
- Refuse to write when the initial scan finds any inconsistency
  (cross-links, bad chains, size mismatches, differing FATs, nesting ≥ 64).
- Excluded file attributes (compressed, sparse, encrypted, device, reparse)
  are one shared mask, `FM_SPECIAL_ATTRS` in `filemove.h`, used by the online
  and boot passes alike.
- Fail closed: when something can't be confirmed (an in-use file that can't
  be found, an unexpected `..`, an unreadable child folder), stop or skip the
  move. Never assume the unconfirmed thing is harmless.
- Never move unmovable boot files: `IO.SYS`, binary `MSDOS.SYS`, `NTLDR`,
  `WINBOOT.SYS`, `IBMBIO/IBMDOS.COM`, DriveSpace volumes, `LDLINUX.SYS`,
  the first cluster of the FAT32 root. On 9x live, also leave the swap file,
  the registry and files in use alone.
- 9x locking follows Microsoft's "Exclusive Volume Locking" protocol: level 1
  between bursts, levels 2→3 for writes, re-read the FAT if the lock flag
  says the volume changed. **Print nothing and wait for nothing while holding
  level 3.**
- Scheduling (BootExecute, AUTOEXEC.BAT, job/helper files) must keep
  unrelated content exactly as it was, stage writes (`.$HS`, `.HST`), keep
  recovery copies (`.HS0`, `.HSB`), and refuse to overwrite leftover
  staging/recovery files.
- Never cause data loss on purpose and never weaken a check to make a test
  pass.

**Exit codes** (shared by all front ends; tests check them):
`0` ok · `1` usage · `2` volume/bitmap inaccessible · `3` unsupported ·
`4` incomplete/interrupted · `5` runtime/scheduling/I/O/output failure.
When results are merged (e.g. `-d -m`), use `worse()` in `main.c`:
the most serious wins, in the order 5 > 3 > 2 > 4 > 0.

## 5. Testing workflow

| Change touches | Run at least |
|---|---|
| `fatfs.c`, `fatdefrag.c` | `make test-core` (`test-fat` runs about 2.8M checks: random FAT images, an independent fsck, injected I/O faults, dropped writes) |
| `bootjob.c` | `make test-core` (`test-bootjob`), then `test-wine` (`test-boot9x`) |
| `fatdisk.c` / DOS request layout / SFT-CDS decoding | `make test-core` (`test-diskio`) |
| `fatdos.c` | No DJGPP? Syntax-check against stand-in DJGPP headers; keep byte-level parsing in `fatdisk.c` so it is host-testable |
| `filemove.c` | `make test-core` (`test-moves`), then `test-wine` (`test-online`, `test-bootfiles`) |
| `ntfsrecord`, `ntfsmft`, `bootcfg`, `bootlog` | `make test-core` |
| CLI / `main.c` | `make test-wine` (`test-cli` + the `expect_usage` cases in `tests/wine.sh`) |
| `fatwin9x.c` | `make test-wine` (`test-win9x` uses a simulated VWIN32) |
| `schedule.c`, `boot_native.c`, `bootfiles.c` | `make test-wine` |
| Build, PE layout, imports | `make check-imports test-mkdual` |
| Anything | `make test` before you call it done |

Testing notes:
- Tests mock all disk, registry and AUTOEXEC access. They never touch real
  disks, System32 or the registry. Keep it that way: new tests must use the
  in-memory images and mocks.
- Win32 test executables `#include` the `.c` under test, or link it, and
  provide mocks. See `tests/online.c` and `tests/win9x.c` for the pattern.
- A new CLI rejection rule needs an `expect_usage` line in `tests/wine.sh`.
- `fat:` tests also check that `-a` predictions equal what `-d` does. If you
  change the planner, both must still agree.
- Passing tests do **not** qualify a change for real hardware. Moves, boot
  behaviour and drive durability still need testing on an NT4/9x target
  machine (say so in summaries).
- Without DJGPP, `make`/`test-wine` stop at the DOS step with "DJGPP not
  found". You can still run `make test-core`, `make check-imports
  test-mkdual`, and build each `build/test-*.exe` target and run it with
  `WINEDEBUG=-all wine`.

## 6. Code conventions

- Each file starts with a block comment: `name.c - purpose`, then the reasons
  behind the design. Comments explain *why* (OS quirks, ordering, safety),
  often in plain prose. Keep that style.
- 4-space indent, K&R braces, `static` helpers, C89-style declarations at the
  top of blocks in older files.
- Win32 side uses wide strings (`wchar_t`, `L"..."`, `%ls`). FAT core uses
  `fat_u32`/`fat_u64` and plain byte buffers with explicit little-endian helpers.
- Errors: return codes, not asserts. Check every allocation. Bound every
  loop over on-disk data. Validate every FSCTL reply (length, run count,
  VCN continuity) before use.
- User-facing text is plain English. README.md is the user guide;
  docs/DESIGN.md holds the internals. **Update them when behaviour, options
  or messages change**, and update `usage()` in `main.c` (plus the DOS usage in `fatdos.c`).
- Version string lives only in `src/version.h`.

## 7. Common tasks

**Add a CLI option**: parse it in `main.c` (and `fatdos.c` if it applies to
DOS), reject invalid combinations with exit 1, update `usage()`, add
`expect_usage` cases and/or `tests/cli.c`, then document it in the README.

**Change FAT defrag behaviour**: work in `fatdefrag.c` against `fat_volume`
only. Extend `tests/fat.c` (it has random image generators and an fsck).
Check that `-a` planning and `-d` results still agree. Run under ASan.

**Add a libc-like helper**: put it in `nostd.c`/`nostd.h` for Win32 and in
`ntrt.c`/`ntrt.h` if the native helper needs it. DJGPP has a real libc.

**Touch the NT move path**: `filemove.c` is shared by online (`defrag.c`) and
boot (`bootfiles.c`) through an ops table. Keep the NT4 rules: VCN boundaries
on move-granularity alignment, no sparse/compressed streams, and give back
reserved clusters once a fresh map shows the data is not there.

**Debug a target failure**: ask for the output of
`hsdefrag.exe C: -a -v > out.txt 2>&1` and `--boot-status`. For bitmap
trouble, use `--no-bitmap/--no-files/--no-meta` to narrow it down.
`explain_bitmap_failure()` in `main.c` maps NTSTATUS codes to likely causes.

## 8. Gotchas

- **Licences:** never copy code from other projects. FreeDOS DEFRAG is GPL
  v2-or-later, and this project has no licence file of its own. Adopt ideas
  only, write the code independently, and say where an idea came from.

- `build/` has many stale `*.log` files from earlier review rounds. They are
  history, not current results.
- `dos/cwsdstub.exe` is checked in on purpose (`!dos/cwsdstub.exe` in
  `.gitignore`). Do not delete it.
- Windows Me cannot run real-mode AUTOEXEC.BAT, so 9x boot scheduling is
  refused there on purpose.