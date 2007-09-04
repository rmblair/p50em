/* Pr1me Computer emulator, Jim Wilcoxson (prirun@gmail.com), April 4, 2005
   Copyright (C) 2005-2007, Jim Wilcoxson.  All Rights Reserved.

   Emulates a Prime Computer system by:
   - booting from a Prime disk image (normal usage)
   - booting from a Prime MAGSAV tape
   - restoring a Prime R-mode .save image from the host file system

   This is a project in development, so please don't publish it or
   make it available for others to use.

   Comments, suggestions, corrections, and general notes that you're
   interested in a Prime emulation project are welcome and
   appreciated.

   -------------
   Usage:  (to boot from pdev 2466, dev '26, unit 3):

   $ time ./em -tport 8000 -cpuid 5 -boot 14714 -map MFD.2462/PRIRUN/RING0.MAP MFD.2462/PRIRUN/RING3.MAP 2>err

   Disk boot device is 14uc4, tape is 10005, 
   where u=1/3/5/7 for units 0/1/2/3
   and c=1/3/5/7 for controller 26/27/...
      (See complete boot table below)

   NOTE: the -map command is optional, but is needed to set the
   real-time clock automatically for older models (cpuid < 15).  If
   maps are not available, use the Primos SE command to set the clock
   manually after the system boots: 
        SE -MMDDYY HHMM

   -------------
   Usage:  (to load and start an R-mode runfile directly from the Unix FS)

   $ ./em -ss 114 -boot *DOS64 2>/dev/null   (-ss optional)


   -------------
   Usage:  to load SAM.SAVE from Unix FS and run diagnostics from pdev 2466

$ time ./em  -cpuid 5 -boot SAM.SAVE 2>err
[SAM Rev. 16.2, DTS Release: 0004.A, Copyright (c) 1990, Prime Computer, Inc.]

Enter physical device = 2466

QUICK VERIFY MODE Enabled; Enter 'RESET QVFY' for normal operation. 
Enter 'SET DCM' to display CASE messages. 
Enter 'LOAD;RUN' for Default Execution

SAM> 
   
   --------------
   Usage:  to load initial boot from tape, then prompt for disk pdev

$ time ./em -boot 1005 -tport 8000 -cpuid 5
Boot file is dev14u0             <--- note tape drive boot
Sense switches set to 1005       <--- these cause pdev prompt
[BOOT Rev. 20.2.3 Copyright (c) 1987, Prime Computer, Inc.]

PHYSICAL DEVICE=2466

DISK ERROR, STATUS: 000001 
PHYSICAL DEVICE=

   ---------------
   Usage:  to load .SAVE image from tape:

$ time ./em -boot 10005
[BOOT Rev. 20.2.3 Copyright (c) 1987, Prime Computer, Inc.]

RUN FILE TREENAME=MFD>DOS>DOS.SAVE

BOOTING FROM MT0    MFD>DOS>DOS.SAVE


PRIMOS II REV 20.0 03/15/85 (AT 170000) 
Copyright (c) Prime Computer, Inc. 1985.
PRIMOS II is being phased out.  To boot PRIMOS return to CP mode. 
("BOOT 14xxx" will autoboot PRIMOS.)

OK: 
   ---------------

   Instruction details are spewed to trace.log depending on the trace flags.

   IMPORTANT NOTE: this only runs on a big-endian machine, like the Prime.
*/

#ifdef __APPLE__
  #define OSX 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <setjmp.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>

/* In SR modes, Prime CPU registers are mapped to memory locations
   0-'37, but only 0-7 are user accessible.  In the post-P300
   architecture, these addresses map to the live register file.

   Locations '40-'57 are reserved for 8 DMC channels, 2 words each.
   Locations '60-'77 are interrupt vectors
   Locations '100-'177 are for external device interrupts 
   see p. A-8 of Sys Arch

   In VI mode, locations 0-'17 are trapped and map to the live
   register file (p 5-17, Sys Arch), though only 0-7 are accessible in
   user mode.
*/

#include "regs.h"
typedef unsigned int ea_t;            /* effective address */
typedef unsigned int pa_t;            /* physical address */

/* procs needing forward declarations */

static void fault(unsigned short fvec, unsigned short fcode, ea_t faddr) __attribute__ ((noreturn));
static void fatal(char *msg) __attribute__ ((noreturn));
static void macheck (unsigned short p300vec, unsigned short chkvec, unsigned int dswstat, unsigned int dswrma) __attribute__ ((noreturn));

/* condition code macros */

#define CLEARCC crs[KEYS] &= ~0300
#define CLEAREQ crs[KEYS] &= ~0100
#define CLEARLT crs[KEYS] &= ~0200
#define SETEQ crs[KEYS] |= 0100
#define SETLT crs[KEYS] |= 0200

/* set condition codes based on a 16-bit signed value */

#define SETCC_16(val16) \
  CLEARCC; \
  if ((val16) == 0) \
    SETEQ; \
  else if (*(short *)(&(val16)) < 0) \
    SETLT;

/* set condition codes based on A register (16-bit signed) */

#define SETCC_A SETCC_16(crs[A])

/* set condition codes based on a 32-bit signed value */

#define SETCC_32(val32) \
  CLEARCC; \
  if ((val32) == 0) \
    SETEQ; \
  else if (*(int *)(&(val32)) < 0) \
    SETLT;

/* set condition codes based on L register (32-bit signed) */

#define SETCC_L SETCC_32(crsl[2])

/* set condition codes based on V-mode FP accumulator

   NOTES: 

   -Prime considers anything with a zero fraction to be zero,
   even if the exponent is non-zero (this is a "dirty zero")

   - Prime only tested 32 bits of the fraction, even for double
   precision.  It expected DP floats to be normalized, or mostly
   normalized.
*/
  
#define SETCC_F \
  CLEARCC; \
  if (*(int *)(crs+FLTH) < 0) \
    SETLT; \
  else if (*(int *)(crs+FLTH) == 0) \
    SETEQ;

#define SETCC_D SETCC_F


/* macros for handling the C-bit (overflow) and L-bit (carry out) */

#define EXPC(onoff) \
  if ((onoff)) crs[KEYS] |= 0100000; \
  else crs[KEYS] &= 077777

#define SETC crs[KEYS] |= 0100000
#define CLEARC crs[KEYS] &= 077777

/* XEXPC, XSETC, XCLEARC are stubs to indicate that the C-bit may not be set correctly */

#define XEXPC   EXPC
#define XCLEARC CLEARC
#define XSETC   SETC

/* EXPCL sets both the C and L bits for shift instructions

   NOTE: unlike EXPC, this doesn't clear anything - bits must be cleared
   before executing these macros! */

#define EXPCL(onoff) \
  if ((onoff)) crs[KEYS] |= 0120000

#define SETCL crs[KEYS] |= 0120000
#define CLEARCL crs[KEYS] &= ~0120000

#define SETL(onoff) \
  if ((onoff)) crs[KEYS] |= 020000; \
  else crs[KEYS] &= ~020000

/* XSETL is a dummy to indicate that the L-bit may not be set correctly */

#define XSETL(onoff) SETL(onoff)

/* these macros are for the VI-mode branch insructions */

#define BCLT if   (crs[KEYS] & 0200)  RPL = iget16(RP); else INCRP
#define BCLE if   (crs[KEYS] & 0300)  RPL = iget16(RP); else INCRP
#define BCEQ if   (crs[KEYS] & 0100)  RPL = iget16(RP); else INCRP
#define BCNE if (!(crs[KEYS] & 0100)) RPL = iget16(RP); else INCRP
#define BCGE if (!(crs[KEYS] & 0200)) RPL = iget16(RP); else INCRP
#define BCGT if (!(crs[KEYS] & 0300)) RPL = iget16(RP); else INCRP
#define BLS  if  (crs[KEYS] & 020000) RPL = iget16(RP); else INCRP
#define BXNE if (crs[X] != 0) RPL = iget16(RP); else INCRP
#define BYNE if (crs[Y] != 0) RPL = iget16(RP); else INCRP
#define BHNE(r) if (crs[(r)*2] != 0) RPL = iget16(RP); else INCRP
#define BRNE(r) if (crsl[(r)]  != 0) RPL = iget16(RP); else INCRP

/* expressions for logicize instructions */

#define LCLT ((crs[KEYS] & 0200) != 0)
#define LCLE ((crs[KEYS] & 0300) != 0)
#define LCEQ ((crs[KEYS] & 0100) != 0)
#define LCNE ((crs[KEYS] & 0100) == 0)
#define LCGE !(crs[KEYS] & 0200)
#define LCGT ((crs[KEYS] & 0300) == 0)

/* macro for restricted instructions (uses current program counter) */

#define RESTRICT() if (RP & RINGMASK32) fault(RESTRICTFAULT, 0, 0);

/* same macro, but uses a passed program counter */

#define RESTRICTR(rpring) if ((rpring) & RINGMASK32) fault(RESTRICTFAULT, 0, 0);

/* trace flags to control aspects of emulator tracing:

   T_EAR	trace R-mode effective address calculation
   T_EAV	trace V-mode effective address calculation
   T_EAI	trace I-mode effective address calculation
   T_FLOW       instruction summary
   T_INST	detailed instruction trace
   T_MODE       trace CPU mode changes
   T_EAAP       AP effective address calculation
   T_DIO        disk I/O
   T_TIO        tape I/O
   T_RIO        ring network I/O
   T_TERM       terminal output (tnou[a])
   T_MAP        segmentation
   T_PCL        PCL instructions
   T_FAULT      Faults
   T_PX         Process exchange
*/

#define TB_EAR   0x00000001
#define TB_EAV   0x00000002
#define TB_EAI   0x00000004
#define TB_INST  0x00000008
#define TB_FLOW  0x00000010
#define TB_MODE  0x00000020
#define TB_EAAP  0x00000040
#define TB_DIO   0x00000080
#define TB_MAP   0x00000100
#define TB_PCL   0x00000200
#define TB_FAULT 0x00000400
#define TB_PX    0x00000800
#define TB_TIO   0x00001000
#define TB_TERM  0x00002000
#define TB_RIO   0x00004000

#define T_EAR   TB_EAR
#define T_EAV   TB_EAV
#define T_EAI   TB_EAI
#define T_INST  TB_INST
#define T_FLOW  TB_FLOW
#define T_MODE  TB_MODE
#define T_EAAP  TB_EAAP
#define T_DIO   TB_DIO
#define T_MAP   TB_MAP
#define T_PCL   TB_PCL
#define T_FAULT TB_FAULT
#define T_PX    TB_PX
#define T_TIO   TB_TIO
#define T_TERM  TB_TERM
#define T_RIO   TB_RIO

#define BITMASK16(b) (0x8000 >> ((b)-1))
#define BITMASK32(b) ((unsigned int)(0x80000000) >> ((b)-1))

#ifdef NOTRACE
  #define TRACE(flags, formatargs...)
  #define TRACEA(formatargs...)
#else
  #define TRACE(flags, formatargs...) if (gvp->traceflags & (flags)) fprintf(gvp->tracefile,formatargs)
  #define TRACEA(formatargs...) fprintf(gvp->tracefile,formatargs)
#endif

  /* traceprocs is an array of (operating system) procedure names we're
     tracing, with flags and associated data

     numtraceprocs is the number of entries in traceprocs, 0=none */

#define MAXTRACEPROCS 2
static struct {
  char  name[11];                    /* procedure name */
  int   ecb;                         /* ecb ea of proc */
  int   sb;                          /* sb before the call */
  int   oneshot;                     /* disable trace after call? */
} traceprocs[MAXTRACEPROCS];

#define TRACEUSER (gvp->traceuser == 0 || crs[OWNERL] == gvp->traceuser)

/* NOTE: Primos II gives "NOT FOUND" on STARTUP 2460 command if sense
   switches are set to 014114.  But DIAGS like this setting. :( */

static unsigned short sswitch = 014114;     /* sense switches, set with -ss & -boot */

/* NOTE: the default cpuid is a P750: 1 MIPS, 8MB of memory */

static unsigned short cpuid = 5;            /* STPM CPU model, set with -cpuid */

/* STLB cache structure is defined here; the actual stlb is in gv.
   There are several different styles on Prime models.  This is
   modeled after the 6350 STLB, but is only 1-way associative. */

#define STLBENTS 512

typedef struct {
  char valid;                 /* 1 if STLB entry is valid, zero otherwise */
  char unmodified;            /* 1 if page hasn't been modified, 0 if modified */
  //  char shared;                /* 1 if page is shared and can't be cached */
  char access[4];             /* ring n access rights */
  unsigned short procid;      /* process id for segments >= '4000 */
  unsigned short seg;         /* segment number */
  unsigned int ppn;           /* physical page number */
  unsigned short *pmep;       /* pointer to page table flag word */
  unsigned long load_ic;      /* instruction where STLB was loaded (for debug) */
} stlbe_t;

/* The IOTLB stores translations for each page of the I/O segments 0-3 */

#define IOTLBENTS 64*4

typedef struct {
  char valid;                 /* 1 if IOTLB entry is valid, zero otherwise */
  unsigned int ppn;           /* physical page number */
} iotlbe_t;

/* "gv" is a static structure used to hold "hot" global variables.  These
   are pointed to by a dedicated register, so that the usual PPC global
   variable instructions aren't needed: these variables can be directly
   referenced by instructions */

typedef struct {

/* traceflags is the variable used to test tracing of each instruction
   traceuser is the user number to trace, 0 meaning any user
   traceseg is the procedure segment number to trace, 0 meaning any
   savetraceflags hold the real traceflags, while "traceflags" switches
   on and off for each instruction

   TRACEUSER is a macro that is true if the current user is being traced
*/

  FILE *tracefile;              /* trace.log file */
  int traceflags;               /* each bit is a trace flag */
  int savetraceflags;
  int traceuser;                /* OWNERL to trace */
  int traceseg;                 /* RPH segment # to trace */
  int numtraceprocs;            /* # of procedures we're tracing */

  int intvec;                   /* currently raised interrupt (if >= zero) */

  unsigned long instcount;      /* global instruction count */

  unsigned short inhcount;      /* number of instructions to stay inhibited */

  unsigned int instpermsec;     /* instructions executed per millisecond */

  stlbe_t stlb[STLBENTS];

  iotlbe_t iotlb[IOTLBENTS];

  unsigned int prevpc;          /* backed program counter */

  unsigned short amask;         /* address mask */

  int pmap32bits;               /* true if 32-bit page maps */

  int pmap32mask;               /* mask for 32-bit page maps */

  int csoffset;                 /* concealed stack segment offset */

  int livereglim;               /* 010 if seg enabled, 040 if disabled */
} gv_t;

static gv_t gv;
#if 1
register gv_t *gvp asm ("r28");
#else
gv_t *gvp;
#endif

static  jmp_buf jmpbuf;               /* for longjumps to the fetch loop */


/* The standard Prime physical memory limit on early machines is 8MB.
   Later machines have higher memory capacities, up to 1024MB, using 
   32-bit page tables. 

   NOTE: 
   - rev 20 is limited to a max of 32MB
   - rev 23.4 is limited to a max of 512MB

   "memlimit" is set with the -mem argument, taking an argument which is
   the desired memory limit in MB.  Setting a memory limit is useful to
   speed up system boots and diagnostics during emulator testing.
 */

#define MEMSIZE 512/2*1024*1024  /* 512 MB */

static unsigned short mem[MEMSIZE];     /* system's physical memory */
static int memlimit;                    /* user's desired memory limit (-mem) */

#define MAKEVA(seg,word) ((((int)(seg))<<16) | (word))

/* returns the incremented value of a virtual address, wrapping to word
   zero at the end of a segment (word portion = 0177777) */

#define INCVA(ea,n) (((ea) & 0xFFFF0000) | ((ea)+(n)) & 0xFFFF)

/* RPADD returns an incremented program counter (does NOT increment
   the counter!)  Technically, it should wrap like INCVA, but for this
   special case, we let it wrap.  Executing the last word in a segment
   should wrap to location zero in the segment, but that causes a trap
   and executes code in the X register - extremely unlikely.

   INCRP bumps program counter.  Again, should only increment the
   16-bit word number, but it's faster (fewer instructions) to
   increment the whole thing.

   DIAG cpu.pcl test 42 does check for segment wraparound, so -DFAST
   will cause this test to fail.

   Update: when cpuid=40 (6650), cpu.pcl test 42 *expects* 32-bit
   increment on RP (segment gets incremented too)!
 */

#ifdef FAST
#define RPADD(n) (RP+n)
#define INCRP RP++
#else
#define RPADD(n) MAKEVA(RPH,RPL+n)
#define INCRP RPL++
#endif

#define FAULTMASK32 0x80000000       /* fault bit */
#define RINGMASK32  0x60000000       /* ring bits */
#define EXTMASK32   0x10000000       /* E-bit */
#define SEGMASK32   0x0FFF0000       /* segment number */
#define RINGMASK16  0x6000           /* ring bits */
#define EXTMASK16   0x1000           /* E-bit */

#define DTAR32(ea) (((ea)>>26) & 3)
#define SEGNO32(ea) (((ea)>>16) & 07777)
#define SEGNO16(ea) ((ea) & 07777)
#define PAGENO(ea) (((ea)>>10) & 077)


/* Fault/interrupt vectors */

#define FIRSTFAULT    062
#define RESTRICTFAULT 062
#define PROCESSFAULT  063
#define PAGEFAULT     064
#define SVCFAULT      065
#define UIIFAULT      066
#define SEMFAULT      067   /* note duplicate w/parity */
#define PARITYCHECK   067
#define MACHCHECK     070
#define MISSMEMCHECK  071
#define ILLINSTFAULT  072
#define ACCESSFAULT   073
#define ARITHFAULT    074
#define STACKFAULT    075
#define SEGFAULT      076
#define POINTERFAULT  077
#define LASTFAULT     077

static ea_t tnoua_ea=0, tnou_ea=0;
static int verbose;                         /* -v (not used anymore) */
static int domemdump;                       /* -memdump arg */

static int tport;                           /* -tport option (incoming terminals) */
static int nport;                           /* -nport option (PNC/Ringnet) */

/* load map related data, specified with -map */

#define MAXSYMBOLS 15000
#define MAXSYMLEN 9
static int numsyms = 0;
static struct {
  char symname[MAXSYMLEN];
  ea_t address;
  char symtype;                /* o=other, c=common, e=ecb, p=proc, l=linkbase */
} mapsym[MAXSYMBOLS];


/* fine-grained timer stuff (thanks Jeff!) */

#include "stopwatch.h"


/* returns an index to a symbol, based on an address and type
   match; if the address isn't found exactly, the index returned
   will be the address lower than the requested address, or -1
   if the symbol table is empty or the requested address is
   lower than any in the symbol table */

int findsym(ea_t addr, char type) {
  int low, high, mid, saveix;

  addr &= 0xFFFFFFF;      /* strip fault, ring, E bits */

  low = 0;
  high = numsyms-1;
  mid = -1;
  while (low <= high) {
    mid = (low+high)/2;
    if (addr < mapsym[mid].address)
      high = mid-1;
    else if (addr == mapsym[mid].address)
      break;
    else if (addr > mapsym[mid].address && mid != numsyms-1 && addr >= mapsym[mid+1].address)
      low = mid+1;
    else
      break;
  }
  saveix = mid;
  if (type != 'x' && mid >= 0)
    while (addr > mapsym[saveix].address && saveix != numsyms-1 && addr > mapsym[saveix+1].address && mapsym[saveix].symtype != type)
      saveix++;
  return saveix;
}


addsym(char *sym, unsigned short seg, unsigned short word, char type) {
  short symlen,ix,ix2;
  ea_t addr;

  symlen = strlen(sym);
  if (symlen > 0 && symlen < MAXSYMLEN) {
    addr = MAKEVA(seg, word);
    ix = findsym(addr, 'x');
    if (ix+1 < numsyms)          /* make room for the new symbol */
      for (ix2 = numsyms; ix2 > ix; ix2--)
	mapsym[ix2] = mapsym[ix2-1];
    //TRACEA("%s = %o/%o\n", sym, seg, words);
    strcpy(mapsym[ix+1].symname, sym);
    mapsym[ix+1].address = addr;
    mapsym[ix+1].symtype = type;
    numsyms++;
  }
}


readloadmap(char *filename) {
  FILE *mapf;
  char line[100];
  int lc,ix;
  char sym[100];
  unsigned int segno, wordno, ecbseg, ecbword, pbseg, pbword, lbseg, lbword;
  ea_t lastaddr;

  TRACEA("Reading load map from %s... ", filename);
  if ((mapf = fopen(filename, "r")) == NULL) {
    perror("Map file open");
    fatal(NULL);
  }
  lc = 0;
  while (fgets(line, sizeof(line), mapf) != NULL) {
    lc++;
    if (strstr(line, "*START"))
      break;
    if (sscanf(line, "%s %o %o %o %o %*o %*o %o %o", sym, &ecbseg, &ecbword, &pbseg, &pbword, &lbseg, &lbword) == 7) {
      addsym(sym, ecbseg, ecbword, 'e');
      addsym(sym, pbseg, pbword, 'p');
      addsym(sym, lbseg, lbword, 'l');
      //printf("adding proc symbol, line=%s\n", line);
      if (tnou_ea == 0 && strcmp(sym,"TNOU") == 0)
	tnou_ea = MAKEVA(ecbseg, ecbword);
      if (tnoua_ea == 0 && strcmp(sym,"TNOUA") == 0)
	tnoua_ea = MAKEVA(ecbseg, ecbword);
    } else if (sscanf(line, "%s %o %o", sym, &segno, &wordno) == 3) {
      addsym(sym, segno, wordno, 'x');
      //printf("adding symbol, line=%s\n", line);
    } else if (strcspn(line, " \n") == 0)
      continue;
    else
      TRACEA("Can't parse map line #%d: %s\n", lc, line);
    if (numsyms == MAXSYMBOLS) {
      TRACEA("Symbol table limit!");
      break;
    }
  }
  fclose(mapf);
  TRACEA("%d symbols loaded\n", numsyms);

  lastaddr = 0;
  for (ix=0; ix < numsyms; ix++) {
    if (mapsym[ix].address < lastaddr)
      TRACEA("Symbol table out of order: ix=%d, sym=%s, addr=%o/%o, lastaddr=%o/%o\n", ix, mapsym[ix].symname, mapsym[ix].address>>16, mapsym[ix].address&0xffff, lastaddr>>16, lastaddr&0xffff);
    lastaddr = mapsym[ix].address;
  }
}

/* returns a pointer to a static character string like DSKBLK+25, to
   print with the effective address for an instruction.  There is a
   stack of return results so that if this is called twice on a
   function call, different results can be returned */

char *searchloadmap(int addr, char type) {
  short ix, diff;

#define MAXBUFIX 10

  static char blank = 0;
  static char buf[MAXBUFIX][100];
  static int bufix=-1;

  if ((SEGNO32(addr) <= 01777 | SEGNO32(addr) >= 06000) &&
      (ix = findsym(addr, type)) > 0) {
    diff = addr - mapsym[ix].address;
    if (diff) {
      if (++bufix == MAXBUFIX)
	bufix = 0;
      snprintf(buf[bufix], sizeof(buf[0]), "%s+'%o", mapsym[ix].symname, diff);
      return buf[bufix];
    } else
      return mapsym[ix].symname;
  } else 
    return &blank;
}


/* intended memory access types:
   1 = PCL (PACC)
   2 = read (RACC)
   3 = write (WACC)
   4 = execute (XACC)
*/
#define PACC 0
#define RACC 2
#define WACC 3
#define XACC 4


/* NOTE: this is the 6350 STLB hash function, giving a 9-bit index 0-511 */

#define STLBIX(ea) ((((((ea) >> 12) ^ (ea)) & 0xc000) >> 7) | (((ea) & 0x70000) >> 12) | ((ea) & 0x3c00) >> 10)

/* maps a Prime 28-bit virtual address to a physical memory
   address, checks access, returns actual access (for PCL)

   May cause:
   - segment fault if segment number is too big
   - segment fault if segment's fault bit is set
   - access fault if intended access isn't permitted
   - page fault if page isn't resident

   Typically, the real program counter is passed in and the ring
   bits from it are used.  For some special cases (gate PCL), a
   fake program counter is passed in with the desired ring bits,
   for example, 0, a R0 program counter, or 020000/0, a R1 program
   counter.  (getr,putr)(16,32,64) allow specifying a PC.
*/

static pa_t mapva(ea_t ea, ea_t rp, short intacc, unsigned short *access) {
  short relseg,seg,nsegs,ring;
  unsigned short pte, stlbix, iotlbix;
  stlbe_t *stlbp;
  unsigned int dtar,sdw,staddr,ptaddr,pmaddr,ppn;
  pa_t pa;

  stopwatch_push(&sw_mapva);

  /* map virtual address if segmentation is enabled */

  if (crs[MODALS] & 4) {
    seg = SEGNO32(ea);
    stlbix = STLBIX(ea);
    stlbp = gvp->stlb+stlbix;
#if DBG
    if (stlbix >= STLBENTS) {
      printf("STLB index %d is out of range for va %o/%o!\n", stlbix, ea>>16, ea&0xffff);
      fatal(NULL);
    }
#endif

    /* if the STLB entry isn't valid, or the segments don't match,
       or the segment is private and the process id doesn't match,
       then the STLB has to be loaded first */

    if (!stlbp->valid || stlbp->seg != seg || (seg >= 04000 && stlbp->procid != crs[OWNERL])) {
      dtar = *(unsigned int *)(crs+DTAR0-2*DTAR32(ea));  /* get dtar register */
      nsegs = 1024-(dtar>>22);
      relseg = seg & 0x3FF;     /* segment within segment table */
      TRACE(T_MAP, "   MAP: ea=%o/%o, seg=%o, dtar=%o, nsegs=%d, relseg=%d, page=%d\n", ea>>16, ea&0xFFFF, seg, dtar, nsegs, relseg, PAGENO(ea));
      if (relseg >= nsegs)
	fault(SEGFAULT, 1, ea);   /* fcode = segment too big */
      staddr = (dtar & 0x003F0000) | ((dtar & 0x7FFF)<<1);
      sdw = *(unsigned int *)(mem+staddr+relseg*2);
      TRACE(T_MAP,"        staddr=%o, sdw=%o\n", staddr, sdw);
      if (sdw & 0x8000)
	fault(SEGFAULT, 2, ea);   /* fcode = sdw fault bit set */
      ptaddr = (((sdw & 0x3F)<<10) | (sdw>>22)) << 6;
      if (gvp->pmap32bits) {
	pmaddr = ptaddr + 2*PAGENO(ea);
	pte = mem[pmaddr];

	/* this is probably correct (don't have any references) for
	   the 53xx and later machines that support more than 128MB of
	   physical memory, but it can't be used for earlier machines
	   like the 9950 or they can't run older software (rev 19 for
	   example).  Need to have a mask for each CPU type to make it
	   technically correct. */

	ppn = ((mem[pmaddr] & gvp->pmap32mask) << 16) | mem[pmaddr+1];
      } else {
	pmaddr = ptaddr + PAGENO(ea);
	pte = mem[pmaddr];
	ppn = pte & 0xFFF;
      }
      TRACE(T_MAP,"        ptaddr=%o, pmaddr=%o, pte=%o\n", ptaddr, pmaddr, pte);
      if (!(pte & 0x8000))
	fault(PAGEFAULT, 0, ea);
      mem[pmaddr] |= 040000;     /* set referenced bit */
      stlbp->valid = 1;
      stlbp->unmodified = 1;
      stlbp->access[0] = 7;
      stlbp->access[1] = (sdw >> 12) & 7;
      stlbp->access[3] = (sdw >> 6) & 7;
      stlbp->procid = crs[OWNERL];
      stlbp->seg = seg;
      stlbp->ppn = ppn;
      stlbp->pmep = mem+pmaddr;
      stlbp->load_ic = gvp->instcount;

      /* if this is an I/O segment reference, load the I/O TLB too.
	 This is done because earlier machines didn't have the LIOT
	 instruction to load the IOTLB; instead, they loaded it with
	 regular memory reference instructions like LDA (they also
	 only had a 64-entry IOTLB, so the emulation is not exact) */

      if (seg < 4) {
	iotlbix = (ea & 0x3FFFF) >> 10;
	gvp->iotlb[iotlbix].valid = 1;
	gvp->iotlb[iotlbix].ppn = ppn;
      }
    }
    ring = ((rp | ea) >> 29) & 3;  /* current ring | ea ring = access ring */
    *access = stlbp->access[ring];
    if (((intacc & *access) != intacc) || (intacc == PACC && ((*access & 3) == 0)))
      fault(ACCESSFAULT, 0, ea);
    if (stlbp->unmodified && intacc == WACC) {
      stlbp->unmodified = 0;
      *(stlbp->pmep) &= ~020000;    /* reset unmodified bit in memory */
    }
    pa = (stlbp->ppn << 10) | (ea & 0x3FF);
    TRACE(T_MAP,"        for ea %o/%o, stlbix=%d, pa=%o	loaded at #%d\n", ea>>16, ea&0xffff, stlbix, pa, stlbp->load_ic);
  } else {
    pa = ea;
  }
  stopwatch_pop(&sw_mapva);
#ifndef FAST
  if (pa < memlimit)
#endif
    return pa;
#if DBG
  printf(" map: Memory address '%o (%o/%o) is out of range 0-'%o (%o/%o) at #%d!\n", pa, pa>>16, pa & 0xffff, memlimit-1, (memlimit-1)>>16, (memlimit-1) & 0xffff, gvp->instcount);
#endif

  /* take a missing memory check
     XXX: not sure if dswstat and dswrma are right, but Primos doesn't
     seem to look at them for this check */

  macheck(071, 0310, 0xd000, pa);
  fatal("Return from macheck");
}

#if 0
/* Use fastmap only when intacc = RACC or WACC, ie, get/put, not PACC!): 

  (pa_t) pa = fastmap(ea, intacc, rp);

*/

static pa_t fastmap (ea_t ea, ea_t rp, short intacc) {
  short seg, ring;
  stlbe_t *stlbp;
  unsigned short access;

  if (crs[MODALS] & 4) {
    seg = SEGNO32(ea);
    stlbp = gvp->stlb+STLBIX(ea);

    /* if the STLB entry is valid and the segments match, and the
       segment is common to all or the process id matches, then the STLB
       cache can be used for this access  */

    if (stlbp->valid && (stlbp->seg == seg) && ((seg < 04000) || (stlbp->procid == crs[OWNERL]))) {
      ring = ((rp | ea) >> 29) & 3;  /* current ring | ea ring = access ring */
      access = stlbp->access[ring];
      if ((intacc & access) == intacc) {
	if (stlbp->unmodified && intacc == WACC) {
	  stlbp->unmodified = 0;
	  *(stlbp->pmep) &= ~020000;    /* reset unmodified bit in memory */
	}
	return ((stlbp->ppn << 10) | (ea & 0x3FF));
      }
    }
    return mapva(ea, intacc, rp, &access);
  }
  return ea;
}
#endif

/* for I/O, ea is either an 18-bit physical address (which is just
   returned if not in mapped I/O mode), or a 2-bit segment number and
   16-bit word number for mapped I/O.  A physical address is returned. */

const static unsigned int mapio(ea_t ea) {
  int iotlbix;

  ea &= 0x3FFFF;
  if (crs[MODALS] & 020) {           /* mapped I/O mode? */
    iotlbix = (ea >> 10) & 0xFF;     /* TLB range is 0-255 */
    if (gvp->iotlb[iotlbix].valid)
      return (gvp->iotlb[iotlbix].ppn << 10) | (ea & 0x3FF);
    else {
      printf("Mapped I/O request to %o/%o, but IOTLB is invalid!\n", ea>>16, ea&0xFFFF);
      fatal(NULL);
    }
  }
  return ea;
}


/* these are I/O versions of get/put that use the IOTLB rather than
   the STLB */

#define get16io(ea) mem[mapio((ea))]
#define get32io(ea) *(unsigned int *)(mem+mapio((ea)))
#define put16io(word,ea) mem[mapio((ea))] = word

/* these are shorthand macros for get/put that use the current program
   counter - the typical usage - or Ring 0, the other typical case.
   The other places the ring field is important are PCL (ring may be
   changing) and queue instructions (user process uses current ring,
   while device controllers use Ring 0 (physical queues) */

#define get16r0(ea) (get16r((ea),0))
#define get32r0(ea) (get32r((ea),0))
#define get64(ea) (get64r((ea),RP))
#define get64r0(ea) (get64r((ea),0))
#define put16r0(value, ea) (put16r((value),(ea),0))
#define put32r0(value, ea) (put32r((value),(ea),0))
#define put64(value, ea) (put64r((value),(ea),RP))
#define put64r0(value, ea) (put64r((value),(ea),0))

/* get16t handles 16-bit fetches that might cause address traps.
   These traps can occur:
   - fetching S/R mode instructions
   - fetching V-mode instructions when RPL < 010 or 040 (seg enabled/not)
   - in any S/R mode memory reference or address calculation
   - in V-mode address calculations (16-bit indirects)
   - in V-mode short instruction execution (LDA# 0 for example)
   These traps CANNOT occur:
   - in I-mode
   - in V-mode long instruction address calculation or execution
*/


static unsigned short get16t(ea_t ea) {
  unsigned short access;

  /* sign bit is set for live register access */

  if (*(int *)&ea >= 0)
    return mem[mapva(ea, RP, RACC, &access)];
  ea = ea & 0xFFFF;
  if (ea < 7)
    return crs[memtocrs[ea]];
  if (ea == 7)                   /* PC */
    return RPL;
  RESTRICTR(RP);
  if (ea < 020)                 /* CRS */
    return crs[memtocrs[ea]];
  if (ea < 040)                 /* DMX */
    return regs.sym.regdmx[((ea & 036) << 1) | (ea & 1)];
  printf(" Live register address %o too big!\n", ea);
  fatal(NULL);
}

static inline unsigned short get16(ea_t ea) {
  unsigned short access;

#if DBG
  if (ea & 0x80000000)
    warn("address trap in get16");
#endif

#ifdef FASTxxx
  /* Primos rev 19 won't boot with this enabled... */
  return mem[fastmap(ea, RP, RACC)];
#else
  return mem[mapva(ea, RP, RACC, &access)];
#endif
}

static unsigned short get16r(ea_t ea, ea_t rpring) {
  unsigned short access;

#if DBG
  if (ea & 0x80000000)
    warn("address trap in get16r");
#endif

  return mem[mapva(ea, rpring, RACC, &access)];
}

static unsigned int get32(ea_t ea) {
  pa_t pa;
  unsigned short access;
  unsigned short m[2];

#if DBG
  if (ea & 0x80000000)                 /* check for live register access */
    warn("address trap in get32");
#endif

  pa = mapva(ea, RP, RACC, &access);

  if ((pa & 01777) <= 01776)
    return *(unsigned int *)(mem+pa);
  else {
    m[0] = mem[pa];
    m[1] = get16(INCVA(ea,1));
    return *(unsigned int *)m;
  }
}

static unsigned int get32r(ea_t ea, ea_t rpring) {
  pa_t pa;
  unsigned short access;
  unsigned short m[2];

#if DBG
  if (ea & 0x80000000)                  /* check for live register access */
    warn("address trap in get32r");
#endif

  pa = mapva(ea, rpring, RACC, &access);

  if ((pa & 01777) <= 01776)
    return *(unsigned int *)(mem+pa);
  else {
    m[0] = mem[pa];
    m[1] = get16r(INCVA(ea,1), rpring);
    return *(unsigned int *)m;
  }
}

static long long get64r(ea_t ea, ea_t rpring) {
  pa_t pa;
  unsigned short access;
  unsigned short m[4];

  /* check for live register access */

#if DBG
  if (ea & 0x80000000)
    warn("address trap in get64");
#endif

  pa = mapva(ea, rpring, RACC, &access);
#if FAST
  if ((ea & 01777) <= 01774) {          /* no page wrap */
    *(int *)(m+0) = *(int *)(mem+pa);
    *(int *)(m+2) = *(int *)(mem+pa+2);
  } else                                /* wraps page (maybe seg too) */
    switch (ea & 3) {
    case 1:
      m[0] = mem[pa];
      *(int *)(m+1) = *(int *)(mem+pa+1);
      pa = mapva(INCVA(ea,3), rpring, RACC, &access);
      m[3] = mem[pa];
      break;
    case 2:
      *(int *)(m+0) = *(int *)(mem+pa);
      pa = mapva(INCVA(ea,2), rpring, RACC, &access);
      *(int *)(m+2) = *(int *)(mem+pa);
      break;
    case 3:
      m[0] = mem[pa];
      pa = mapva(INCVA(ea,1), rpring, RACC, &access);
      *(int *)(m+1) = *(int *)(mem+pa);
      m[3] = mem[pa+2];
      break;
    default:
      fatal("Page cross error in get64r");
    }
#else
  if ((pa & 01777) <= 01774) {          /* no page wrap */
    *(int *)(m+0) = *(int *)(mem+pa);
    *(int *)(m+2) = *(int *)(mem+pa+2);
  } else {
    m[0] = mem[pa];
    m[1] = get16r(INCVA(ea,1), rpring);
    m[2] = get16r(INCVA(ea,2), rpring);
    m[3] = get16r(INCVA(ea,3), rpring);
  }
#endif
  return *(long long *)m;
}

/* Instruction version of get16 (can be replaced by get16 too...)
   This needs to be checked more... not sure it actually improves
   performance all that much and is potentially incompatible, ie,
   iget16t isn't implemented (V-mode executing from registers) */

#ifdef FAST
#define iget16t(ea) iget16((ea))

unsigned short iget16(ea_t ea) {
  unsigned short access;
  ea_t thisvpn;
  static unsigned int prevowner = 0xFFFFFFFF;  /* OWNERH|OWNERL */
  static pa_t prevppa = 0xFFFFFC00;   /* physical page address */
  static ea_t prevvpn = 0xFFFFFC00;   /* virtual page address */

  if (*(int *)&ea >= 0) {
    thisvpn = ea & 0x0FFFFC00;          /* segment and page number */
    if ((thisvpn != prevvpn) || (((ea & 0x0FFF0000) >= 0x800) && (crsl[OWNER32] != prevowner))) {
      prevvpn = thisvpn;
      prevowner = crsl[OWNER32];
      prevppa = mapva(ea, RP, RACC, &access) & 0xFFFFFC00;
    }
    return mem[prevppa + (ea & 0x3FF)];
  }
  return get16t(ea);
}
#else
#define iget16(ea) get16((ea))
#define iget16t(ea) get16t((ea))
#endif

/* put16t handles potentially address trapping stores */

static put16t(unsigned short value, ea_t ea) {
  unsigned short access;

  if (*(int *)&ea >= 0)
    mem[mapva(ea, RP, WACC, &access)] = value;
  else {
    ea = ea & 0xFFFF;
    if (ea < 7)
      crs[memtocrs[ea]] = value;
    else if (ea == 7) {
      RPL = value;
    } else {
      RESTRICTR(RP);
      if (ea <= 017)                      /* CRS */
	crs[memtocrs[ea]] = value;
      else if (ea <= 037)                 /* DMX */
	regs.sym.regdmx[((ea & 036) << 1) | (ea & 1)] = value;
      else {
	printf(" Live register store address %o too big!\n", ea);
	fatal(NULL);
      }
    }
  }
}

static put16(unsigned short value, ea_t ea) {
  unsigned short access;

#if DBG
  if (ea & 0x80000000)
    warn("address trap in put16");
#endif

  mem[mapva(ea, RP, WACC, &access)] = value;
}

static put16r(unsigned short value, ea_t ea, ea_t rpring) {
  unsigned short access;

#if DBG
  if (ea & 0x80000000)
    warn("address trap in put16r");
#endif

  mem[mapva(ea, rpring, WACC, &access)] = value;
}

static put32(unsigned int value, ea_t ea) {
  pa_t pa;
  unsigned short access;
  unsigned short *m;

#if DBG
  if (ea & 0x80000000)               /* check for live register access */
    warn("address trap in put32");
#endif

  pa = mapva(ea, RP, WACC, &access);
  if ((pa & 01777) <= 01776)
    *(unsigned int *)(mem+pa) = value;
  else {
    m = (void *)&value;
    mem[pa] = m[0];
    put16(m[1], INCVA(ea,1));
  }
}

static put32r(unsigned int value, ea_t ea, ea_t rpring) {
  pa_t pa;
  unsigned short access;
  unsigned short *m;

#if DBG
  if (ea & 0x80000000)                 /* check for live register access */
    warn("address trap in put32");
#endif

  pa = mapva(ea, rpring, WACC, &access);
  if ((pa & 01777) <= 01776)
    *(unsigned int *)(mem+pa) = value;
  else {
    m = (void *)&value;
    mem[pa] = m[0];
    put16r(m[1], INCVA(ea,1), rpring);
  }
}

static put64r(long long value, ea_t ea, ea_t rpring) {
  pa_t pa;
  unsigned short access;
  unsigned short *m;

  /* check for live register access */

#if DBG
  if (ea & 0x80000000)
    warn("address trap in put64");
#endif

  pa = mapva(ea, rpring, WACC, &access);
  if ((pa & 01777) <= 01774)
    *(long long *)(mem+pa) = value;
  else {
    m = (void *)&value;
    mem[pa] = m[0];
    put16r(m[1], INCVA(ea,1), rpring);
    put16r(m[2], INCVA(ea,2), rpring);
    put16r(m[3], INCVA(ea,3), rpring);
  }
}

/* machine check handler, called with check vector locations
   The first arg is used when PX is disabled, the 2nd when PX is enabled. */

void macheck (unsigned short p300vec, unsigned short chkvec, unsigned int dswstat, unsigned int dswrma) {
  unsigned short m;

  /* set check registers in the register file */

  regs.sym.dswpb = RP;
  regs.sym.dswstat = dswstat;
  regs.sym.dswrma = dswrma;

  /* if process exchange is enabled, follow the standard check protocol;
     if PX not enabled, simulate JST p300vec,* to invoke the check.
     Then longjmp back to the fetch loop */

  if (crs[MODALS] & 010) {
    printf(" map: missing memory while PX enabled\n");
  } else {
    m = get16(p300vec);
    put16(RPL, m);
    RP = m+1;
  }

  /* similar code in the fault handler */

  grp = RP;
  gcrsl = crsl;
  longjmp(jmpbuf, 1);
  fatal("macheck: returned after longjmp\n");
}


static warn(char *msg) {
  printf("emulator warning:\n  instruction #%d at %o/%o: %o %o keys=%o, modals=%o\n  %s\n", gvp->instcount, gvp->prevpc >> 16, gvp->prevpc & 0xFFFF, get16(gvp->prevpc), get16(gvp->prevpc+1),crs[KEYS], crs[MODALS], msg);
}
    

/* queue instructions 

   NOTE: ABQ is typically used in software to add an item to a
   hardware (physical) queue and RTQ is used by DMQ hardware to fetch
   items from the queue.  All of the queue instructions _should_
   support physical queues, but only ABQ and RTQ currently support
   them (they're needed for AMLC boards).  If ICS support is added,
   the other queue instructions will probably need to support physical
   queues. 

   The CPU KEYS are not set here because this would not happen on a
   DMQ request - only when the instruction is executed by software.
*/

static int rtq(ea_t qcbea, unsigned short *qent, ea_t rp) {

  unsigned int qtop, qbot, qtemp;
  unsigned short qseg, qmask;
  ea_t qentea;

  qtop = get16r(qcbea, rp);
  qbot = get16r(qcbea+1, rp);
  if (qtop == qbot) {
    *qent = 0;
    return 0;               /* queue is empty */
  }
  qseg = get16r(qcbea+2, rp);
  qmask = get16r(qcbea+3, rp);
  qentea = MAKEVA(qseg & 0xfff, qtop);
  if (qseg & 0x8000)        /* virtual queue */
    *qent = get16r(qentea, rp);
  else {
    RESTRICTR(rp);
    /* XXX: this should probably go through mapio */
    *qent = mem[qentea];
  }
  qtop = (qtop & ~qmask) | ((qtop+1) & qmask);
  put16r(qtop & 0xFFFF, qcbea, rp);
  return 1;
}


static int abq(ea_t qcbea, unsigned short qent, ea_t rp) {

  unsigned int qtop, qbot, qtemp;
  unsigned short qseg, qmask;
  ea_t qentea;

  qtop = get16r(qcbea, rp);
  qbot = get16r(qcbea+1, rp);
  qseg = get16r(qcbea+2, rp);
  qmask = get16r(qcbea+3, rp);
  qtemp = (qbot & ~qmask) | ((qbot+1) & qmask);
  if (qtemp == qtop)         /* queue full */
    return 0;
  qentea = MAKEVA(qseg & 0xfff,qbot);
  if (qseg & 0x8000)         /* virtual queue */
    put16r(qent, qentea, rp);
  else {
    RESTRICTR(rp);
    /* XXX: this should probably go through mapio */
    mem[qentea] = qent;
  }
  put16r(qtemp, qcbea+1, rp);
  return 1;
}


static int rbq(ea_t qcbea, unsigned short *qent, ea_t rp) {

  unsigned int qtop, qbot, qtemp;
  unsigned short qseg, qmask;
  ea_t qentea;

  qtop = get16(qcbea);
  qbot = get16(qcbea+1);
  if (qtop == qbot) {  /* queue empty */
    *qent = 0;
    return 0;
  }
  qseg = get16(qcbea+2) & 0x7FFF;
  qmask = get16(qcbea+3);
  qbot = (qbot & ~qmask) | ((qbot-1) & qmask);
  qentea = MAKEVA(qseg,qbot);
  *qent = get16(qentea);
  put16(qbot, qcbea+1);
  return 1;
}

static int atq(ea_t qcbea, unsigned short qent, ea_t rp) {

  unsigned int qtop, qbot, qtemp;
  unsigned short qseg, qmask;
  ea_t qentea;

  qtop = get16(qcbea);
  qbot = get16(qcbea+1);
  qseg = get16(qcbea+2) & 0x7FFF;
  qmask = get16(qcbea+3);
  qtemp = (qtop & ~qmask) | ((qtop-1) & qmask);
  if (qtemp == qbot)   /* queue full */
    return 0;
  qentea = MAKEVA(qseg,qtemp);
  put16(qent,qentea);
  put16(qtemp, qcbea);
  return 1;
}

static unsigned short tstq(ea_t qcbea) {

  unsigned int qtop, qbot, qmask;

  qtop = get16(qcbea);
  qbot = get16(qcbea+1);
  qmask = get16(qcbea+3);
  return (qbot-qtop) & qmask;
}


/* I/O device map table, containing function pointers to handle device I/O */

static int devpoll[64] = {0};

#include "emdev.h"

#if 0

/* this is the "full system" controller configuration */

static int (*devmap[64])(int, int, int) = {
  /* '0x */ devnone,devnone,devnone,devnone,devasr,devnone,devnone,devpnc,
  /* '1x */ devnone,devnone,devnone,devnone,devmt,devamlc, devamlc, devamlc,
  /* '2x */ devcp,devnone,devdisk,devdisk,devdisk,devdisk,devdisk,devdisk,
  /* '3x */ devnone,devnone,devamlc,devnone,devnone,devamlc,devnone,devnone,
  /* '4x */ devnone,devnone,devnone,devnone,devnone,devnone,devnone,devnone,
  /* '5x */ devnone,devnone,devamlc,devamlc,devamlc,devnone,devnone,devnone,
  /* '6x */ devnone,devnone,devnone,devnone,devnone,devnone,devnone,devnone,
  /* '7x */ devnone,devnone,devnone,devnone,devnone,devnone,devnone,devnone};

#else

/* this is the "minimum system" controller configuration */

static int (*devmap[64])(int, int, int) = {
  /* '0x */ devnone,devnone,devnone,devnone,devasr,devnone,devnone,devpnc,
#if 1
  /* '1x */ devnone,devnone,devnone,devnone,devmt,devnone, devnone, devnone,
#else
  /* '1x */ devnone,devnone,devnone,devnone,devnone,devnone, devnone, devnone,
#endif
  /* '2x */ devcp,devnone,devnone,devnone,devnone,devnone,devdisk,devnone,
  /* '3x */ devnone,devnone,devnone,devnone,devnone,devnone,devnone,devnone,
  /* '4x */ devnone,devnone,devnone,devnone,devnone,devnone,devnone,devnone,
  /* '5x */ devnone,devnone,devnone,devnone,devamlc,devnone,devnone,devnone,
  /* '6x */ devnone,devnone,devnone,devnone,devnone,devnone,devnone,devnone,
  /* '7x */ devnone,devnone,devnone,devnone,devnone,devnone,devnone,devnone};
#endif



static void fatal(char *msg) {
  ea_t pcbp, csea;
  unsigned short first,next,last,this;
  unsigned short cs[6];
  int i;

  stopwatch_stop(&sw_all);
  printf("\n");
  stopwatch_report(&sw_all);
  stopwatch_report(&sw_fault);
  stopwatch_report(&sw_ea);
  stopwatch_report(&sw_mapva);
  stopwatch_report(&sw_io);
  stopwatch_report(&sw_add16);
  stopwatch_report(&sw_cas);
  stopwatch_report(&sw_irs);
  stopwatch_report(&sw_zmv);
  stopwatch_report(&sw_zfil);
  stopwatch_report(&sw_zmvd);
  stopwatch_report(&sw_pcl);
  stopwatch_report(&sw_idle);

  printf("Fatal error: instruction #%d at %o/%o %s: %o %o\nowner=%o %s, keys=%o, modals=%o\n", gvp->instcount, gvp->prevpc >> 16, gvp->prevpc & 0xFFFF, searchloadmap(gvp->prevpc,' '), get16(gvp->prevpc), get16(gvp->prevpc+1), crs[OWNERL], searchloadmap(*(unsigned int *)(crs+OWNER),' '), crs[KEYS], crs[MODALS]);
  
  /* dump concealed stack entries */

  if (crs[MODALS] & 010) {   /* process exchange is enabled */
    pcbp = *(ea_t *)(crs+OWNER);    /* my pcb pointer */
    first = get16r0(pcbp+PCBCSFIRST);
    next = get16r0(pcbp+PCBCSNEXT);
    last = get16r0(pcbp+PCBCSLAST);
    while (next != first) {
      this = next-6;
      csea = MAKEVA(crs[OWNERH]+gvp->csoffset, this);
      *(unsigned int *)(cs+0) = get32r0(csea+0);
      *(long long *)(cs+2) = get64r0(csea+2);
      printf("Fault: RP=%o/%o, keys=%06o, fcode=%o, faddr=%o/%o\n", cs[0], cs[1], cs[2], cs[3], cs[4], cs[5]);
      next = this;
    }
  }

  if (msg)
    printf("%s\n", msg);
  /* should do a register dump, RL dump, PCB dump, etc. here... */

  /* call all devices with a request to terminate */

  for (i=0; i<64; i++)
    devmap[i](-2, 0, i);

  fclose(gvp->tracefile);
  exit(1);
}



/* set new processor keys */

static newkeys (unsigned short new) {

  switch ((new & 016000) >> 10) {
  case 0:                     /* 16S */
    TRACE(T_MODE, "Entering 16S mode, keys=%o\n", new);
    gvp->amask = 037777;
    break;
  case 1:                     /* 32S */
    TRACE(T_MODE, "Entering 32S mode, keys=%o\n", new);
    gvp->amask = 077777;
    break;
  case 2:                     /* 64R */
    TRACE(T_MODE, "Entering 64R mode, keys=%o\n", new);
    gvp->amask = 0177777;
    break;
  case 3:                     /* 32R */
    TRACE(T_MODE, "Entering 32R mode, keys=%o\n", new);
    gvp->amask = 077777;
    break;
  case 4:                     /* 32I */
    TRACE(T_MODE, "Entering 32I mode, keys=%o\n", new);
    gvp->amask = 0177777;
    break;
  case 6:                     /* 64V */
    TRACE(T_MODE, "Entering 64V mode, keys=%o\n", new);
    gvp->amask = 0177777;
    break;
  default:                    /* invalid */
    printf("Invalid CPU mode: %o\n", new);
    fatal(NULL);
  }
  crs[KEYS] = new;
}

static void fault(unsigned short fvec, unsigned short fcode, ea_t faddr) {
  static unsigned char faultname[LASTFAULT-FIRSTFAULT+2][4] = 
    {"RXM", "PRC", "PAG", "SVC", "UII", "SEM", "MCK", "MM", "ILL", "ACC", "ARI", "STK", "SEG", "PTR", "-?-"};
  unsigned char *faultnamep;
  ea_t pcbp, pxfvec, csea, ea;
  unsigned short first, next, last;
  unsigned short m;
  unsigned short ring;
  int i,namlen;
  unsigned short name[128];
  ea_t faultrp;

  /* NOTE: Prime Hackers Guide says RP is backed for SVC fault, other
     docs say it is current */

  stopwatch_push(&sw_fault);
  if (fvec == PROCESSFAULT || fvec == SVCFAULT || fvec == ARITHFAULT)
    faultrp = RP;
  else
    faultrp = gvp->prevpc;

  /* save RP, keys in regfile, fcode and faddr in crs */

  regs.sym.pswpb = faultrp;
  regs.sym.pswkeys = crs[KEYS];
  crs[FCODE] = fcode;
  *(unsigned int *)(crs+FADDR) = faddr;
  
  if (FIRSTFAULT <= fvec && fvec <= LASTFAULT)
    faultnamep = faultname[fvec-FIRSTFAULT];
  else
    faultnamep = faultname[LASTFAULT-FIRSTFAULT+1];
  TRACE(T_FAULT, "#%d: fault '%o (%s), fcode=%o, faddr=%o/%o, faultrp=%o/%o\n", gvp->instcount, fvec, faultnamep, fcode, faddr>>16, faddr&0xFFFF, faultrp>>16, faultrp&0xFFFF);

  if (crs[MODALS] & 010) {   /* process exchange is enabled */
    ring = (RPH>>13) & 3;                     /* save current ring */
    pcbp = *(ea_t *)(crs+OWNER);
    if (fvec == PROCESSFAULT || fvec == SEMFAULT || fvec == ACCESSFAULT || fvec == STACKFAULT || fvec == SEGFAULT)
      pxfvec = get32r0(pcbp+PCBFVR0);         /* use R0 handler */
    else if (fvec == PAGEFAULT)
      pxfvec = get32r0(pcbp+PCBFVPF);         /* use page fault handler, also R0 */
    else {
      pxfvec = get32r0(pcbp+PCBFVEC+2*ring);  /* use current ring handler */
      pxfvec |= ((int)ring) << 29;            /* weaken */
    }

    /* push a concealed stack entry */

    first = get16r0(pcbp+PCBCSFIRST);
    next = get16r0(pcbp+PCBCSNEXT);
    last = get16r0(pcbp+PCBCSLAST);
    TRACE(T_FAULT, "fault: PX enabled, pcbp=%o/%o, cs first=%o, next=%o, last=%o\n", pcbp>>16, pcbp&0xFFFF, first, next, last);
    if (next > last) {
#if 1
      /* this is better for debugging */
      TRACE(T_FAULT, "fault: Concealed stack wraparound to first");
      fatal("fault: Concealed stack wraparound to first");
#else
      /* this is the normal mode of operation & necessary for DIAG */
      TRACE(T_FAULT, "fault: Concealed stack wraparound to first");
      next = first;
#endif
    }
    csea = MAKEVA(crs[OWNERH]+gvp->csoffset, next);
    put32r0(faultrp, csea);
    put16r0(crs[KEYS], csea+2);
    put16r0(fcode, csea+3);
    put32r0(faddr, csea+4);
    put16r0(next+6, pcbp+PCBCSNEXT);
    TRACE(T_FAULT, "fault: updated cs next=%o\n", get16r0(pcbp+PCBCSNEXT));

    /* update RP to jump to the fault vector in the fault table */

    RP = pxfvec + (fvec-062)*4;
    newkeys(014000);      /* V-mode */
    gvp->inhcount = 1;         /* supposed to do this only for Ring 0, but shouldn't hurt */

#if 0
    if (T_FAULT && fvec == POINTERFAULT) {
      ea = get32(faddr);
      if ((ea & 0xF0000000) == 0x80000000) {
	ea &= 0x0FFFFFFF;
	namlen = get16(ea);
	for (i=0; i<(namlen+1)/2; i++)
	  name[i] = get16(ea+i+1) & 0x7f7f;
	name[i] = 0;
	TRACE(T_FAULT, "fault: DYNT addr=%o/%o, length=%d, name=%s\n", ea>>16, ea&0xffff, namlen, name);
      }
    }
#endif

    TRACE(T_FAULT, "fault: jumping to fault table entry at RP=%o/%o\n", RPH, RPL);

  } else {                   /* process exchange is disabled */
    //TRACE(T_FAULT, "fault '%o occurred at %o/%o, instruction=%o, modals=%o\n", fvec, faultrp>>16, faultrp&0xffff, get16(faultrp), crs[MODALS]);
    /* XXX: need to check for standard/vectored interrupt mode here... */
    m = get16(fvec);
    if (m != 0) {
      TRACE(T_FLOW, " fault JST* '%o [%o]\n", fvec, m);
      put16(faultrp & 0xFFFF, m);
      /* NOTE: should this set RP to m (segment 0), or just set RPL? */
#if 0
      RPL = m;
#else
      RP = m;
#endif
      INCRP;
    } else {
      printf("#%d: fault '%o, fcode=%o, faddr=%o/%o, faultrp=%o/%o\n", gvp->instcount, fvec, fcode, faddr>>16, faddr&0xFFFF, faultrp>>16, faultrp&0xFFFF);
      fatal("Fault vector is zero, process exchange is disabled.");
    }
  }

  /* on longjmp, register globals are reset (PPC); save them before jumping
     See also macheck */

  grp = RP;
  gcrsl = crsl;
  longjmp(jmpbuf, 1);
  fatal("fault: returned after longjmp\n");
}


/* 16S Addressing Mode */

static ea_t ea16s (unsigned short inst, short x) {
  
  unsigned short ea, m, rpl, amask, i;
  ea_t va;

  i = inst & 0100000;                            /* indirect */
  amask = 037777;
  rpl = gvp->prevpc;
  if (inst & 001000)
    ea = (rpl & 037000) | (inst & 0777);         /* current sector */
  else
    ea = (inst & 0777);                          /* sector 0 */
  while (1) {
    if (x)                                       /* indexed */
      ea += crs[X];
    if (!i)                                      /* not indirect */
      break;
    if (ea < gvp->livereglim)
      m = get16t(0x80000000|ea);
    else
      m = get16t(MAKEVA(RPH,ea));
    i = m & 0100000;
    x = m & 040000;
    ea = m & 037777;                             /* go indirect */
  }
  va = MAKEVA(RPH, ea);
  if (ea < gvp->livereglim)                      /* flag live register ea */
    return va | 0x80000000;
  return va;
}


/* 32S Addressing Mode */

static ea_t ea32s (unsigned short inst, short x) {
  
  unsigned short ea, m,rpl, amask, i;
  ea_t va;

  i = inst & 0100000;                            /* indirect */
  amask = 077777;
  rpl = gvp->prevpc;
  if (inst & 001000)
    ea = (rpl & 077000) | (inst & 0777);         /* current sector */
  else {
    ea = (inst & 0777);                          /* sector 0 */
    if (ea < 0100 && x) {                        /* preindex by X */
      ea += crs[X];
      x = 0;
    }
  }
  while (i) {
    if (ea < gvp->livereglim)
      m = get16t(0x80000000|ea);
    else
      m = get16t(MAKEVA(RPH,ea));
    i = m & 0100000;
    ea = m & 077777;                             /* go indirect */
  }
  if (x)                                         /* postindex */
    ea += crs[X];
  ea &= amask;
  va = MAKEVA(RPH, ea);
  if (ea < gvp->livereglim)                      /* flag live register ea */
    return va | 0x80000000;
  return va;
}


/* NOTE: the difference between 32R and 64R, besides the extra address
   bit, is that 32R indirect words have an indirect bit for multi-level
   indirects */

static ea_t ea32r64r (ea_t earp, unsigned short inst, short x, unsigned short *opcode) {

  unsigned short ea, m, rph, rpl, amask, class, i;
  ea_t va;

  i = inst & 0100000;                            /* indirect */
  amask = 0177777;
  if ((crs[KEYS] & 016000) == 06000)             /* 32R mode? */
    amask = 077777;
  rpl = earp;
  rph = (earp >> 16) & 0x7FFF;     /* clear fault (live register) bit from RP */
  TRACE(T_EAR, " ea32r64r: i=%o, x=%o, amask=%o\n", i!= 0, x!=0, amask);
  if (inst & 001000)                             /* sector bit 7 set? */
    if ((inst & 0760) != 0400) {                 /* PC relative? */
      ea = rpl + (((short) (inst << 7)) >> 7);   /* yes, sign extend D */
      TRACE(T_EAR, " PC relative, P=%o, new ea=%o\n", rpl, ea);
    }
    else 
      goto special;                              /* special cases */
  else {
    ea = (inst & 0777);                          /* sector 0 */
    TRACE(T_EAR, " Sector 0, new ea=%o\n", ea);
    if (ea < 0100 && x) {                        /* preindex by X */
      TRACE(T_EAR, " Preindex, ea=%o, X='%o/%d\n", ea, crs[X], *(short *)(crs+X));
      ea += crs[X];
      TRACE(T_EAR, " Preindex, new ea=%o\n", ea);
      x = 0;
    }
  }
  while (i) {
    if (ea < gvp->livereglim)
      m = get16t(0x80000000|ea);
    else
      m = get16t(MAKEVA(rph,ea));
    TRACE(T_EAR, " Indirect, old ea=%o, [ea]=%o\n", ea, m);
    if ((crs[KEYS] & 016000) == 06000)           /* 32R mode? */
      i = m & 0100000;                           /* yes, multiple indirects */
    else
      i = 0;                                     /* no, 64R mode, single indirect */
    ea = m & amask;                              /* go indirect */
    TRACE(T_EAR, " Indirect, new i=%d, new ea=%o\n", i!=0, ea);
  }
  if (x) {
    TRACE(T_EAR, " Postindex, old ea=%o, X='%o/%d\n", ea, crs[X], *(short *)(crs+X));
    ea += crs[X];
    TRACE(T_EAR, " Postindex, new ea=%o\n", ea);
  }
  ea &= amask;
  va = MAKEVA(rph, ea);
  if (ea < gvp->livereglim)                      /* flag live register ea */
    return va | 0x80000000;
  return va;

special:
  class = inst & 3;                              /* class bits = 15 & 16 */
  *opcode = *opcode | ((inst >> 2) & 3);         /* opcode extension */
  TRACE(T_EAR, " special, new opcode=%5#0o, class=%d\n", *opcode, class);

  if (class < 2) {                               /* class 0/1 */
    ea = get16t(RP);                             /* get A from next word */
    INCRP;
    TRACE(T_EAR, " Class %d, new ea=%o\n", class, ea);
    if (class == 1)
      ea += crs[S];
    if (x) {
      TRACE(T_EAR, " Preindex, ea=%o, X='%o/%d\n", ea, crs[X], *(short *)(crs+X));
      ea += crs[X];
      TRACE(T_EAR, " Preindex, new ea=%o\n", ea);
    }
    while (i) {
      if (ea < gvp->livereglim)
	m = get16t(0x80000000|ea);
      else
	m = get16t(MAKEVA(rph,ea));
      TRACE(T_EAR, " Indirect, old ea=%o, [ea]=%o\n", ea, m);
      if ((crs[KEYS] & 016000) == 06000)
	i = m & 0100000;
      else
	i = 0;
      ea = m & amask;
      TRACE(T_EAR, " Indirect, new i=%d, new ea=%o\n", i!=0, ea);
    }

  } else if (i && x) {                           /* class 2/3, ix=11 */
    TRACE(T_EAR, " class 2/3, ix=11\n");
    ea = get16t(RP);                             /* get A from next word */
    INCRP;
    TRACE(T_EAR, " ea=%o\n", ea);
    if (class == 3)
      ea += (short) crs[S];
    while (i) {
      if (ea < gvp->livereglim)
	m = get16t(0x80000000|ea);
      else
	m = get16t(MAKEVA(rph,ea));
      TRACE(T_EAR, " Indirect, ea=%o, [ea]=%o\n", ea, m);
      if ((crs[KEYS] & 016000) == 06000)
	i = m & 0100000;
      else
	i = 0;
      ea = m & amask;
      TRACE(T_EAR, " Indirect, new i=%d, new ea=%o\n", i!=0, ea);
    }
    TRACE(T_EAR, " Postindex, old ea=%o, X='%o/%d\n", ea, crs[X], *(short *)(crs+X));
    ea += (short) crs[X];
    TRACE(T_EAR, " Postindex, new ea=%o\n", ea);

  } else {                                       /* class 2/3, ix != 11 */
    if (class == 2)
      ea = crs[S]++;
    else
      ea = --crs[S];
    TRACE(T_EAR, " Class 2/3, new ea=%o, new S=%o\n", ea, crs[S]);
    if (x) {
      if (ea < gvp->livereglim)
	m = get16t(0x80000000|ea);
      else
	m = get16t(MAKEVA(rph,ea));
      if ((crs[KEYS] & 016000) == 06000)
	i = m & 0100000;
      ea = m & amask;
    }
    while (i) {
      if (ea < gvp->livereglim)
	m = get16t(0x80000000|ea);
      else
	m = get16t(MAKEVA(rph,ea));
      if ((crs[KEYS] & 016000) == 06000)
	i = m & 0100000;
      else
	i = 0;
      ea = m & amask;
    }
    if (x)
      ea += crs[X];
  }
  ea &= amask;
  va = MAKEVA(rph, ea);
  if (ea < gvp->livereglim)                     /* flag live register ea */
    return va | 0x80000000;
  return va;
}

#include "ea64v.h"
#include "ea32i.h"

static ea_t apea(unsigned short *bitarg) {
  unsigned short ibr, ea_s, ea_w, bit, br, a;
  unsigned int utempl;
  ea_t ea, ip;

#if 0
  ibr = iget16(RP);
  RPL++;
  a = iget16(RP);
  RPL++;
#else
  utempl = get32(RP);
  INCRP; INCRP;
  ibr = utempl >> 16;
  a = utempl & 0xffff;
#endif
  bit = (ibr >> 12) & 0xF;
  br = (ibr >> 8) & 3;
  TRACE(T_EAAP, " AP ibr=%o, br=%d, i=%d, bit=%d, a=%o\n", ibr, br, (ibr & 004000) != 0, bit, a);

  /* XXX: should ea ring be weakened with RP ring? */

  ea_s = crs[PBH + 2*br];
  ea_w = crs[PBL + 2*br] + a;
  ea = MAKEVA(ea_s, ea_w);
  TRACE(T_EAAP, " AP ea = %o/%o  %s\n", ea_s, ea_w, searchloadmap(ea,' '));
  if (ibr & 004000) {
    if (ea & 0x80000000)
      fault(POINTERFAULT, ea>>16, 0);    /* XXX: faddr=0? */
    ip = get32(ea);
    if (ip & EXTMASK32)
      bit = get16(INCVA(ea,2)) >> 12;
    else
      bit = 0;
    ea = ip;
    TRACE(T_EAAP, " After indirect, AP ea = %o/%o, bit=%d  %s\n", ea>>16, ea & 0xFFFF, bit, searchloadmap(ea,' '));
  }
  if (bit)
    ea |= EXTMASK32;
  if (bitarg != NULL)
    *bitarg = bit;
  return ea;
}

/* exception handler types:

  'i' = integer exception
  'd' = decimal exception
  'f' = floating point exception

  Depending on the keys settings, take the appropriate fault.
  Always sets the C-bit.
*/

#define FC_SFP_OFLOW 0400    /* 0x100 */
#define FC_SFP_ZDIV  0401    /* 0x101 */
#define FC_SFP_STORE 0402    /* 0x102 */
#define FC_INT_CONV  0403    /* 0x103 */
#define FC_DFP_OFLOW 01000   /* 0x200 */
#define FC_DFP_ZDIV  01001   /* 0x201 */
#define FC_INT_OFLOW 01400   /* 0x300 */
#define FC_INT_ZDIV  01401   /* 0x301 */
#define FC_FUNC_EXC  03000   /* 0x600 */
#define FC_DEC_OFLOW 03400   /* 0x700 */
#define FC_DEC_ZDIV  03401   /* 0x701 */
#define FC_DEC_CONV  03402   /* 0x702 */
#define FC_QFP_OFLOW 04000   /* 0x800 */
#define FC_QFP_ZDIV  04001   /* 0x801 */
#define FC_QFP_QINQ  04003   /* 0x803 */


static inline void mathexception(unsigned char extype, unsigned short fcode, ea_t faddr)
{
  crs[KEYS] |= 0x8000;
  switch (extype) {
  case 'i':
    if (crs[KEYS] & 0400) 
      fault(ARITHFAULT, fcode, faddr);
    break;
  case 'd':
    if (crs[KEYS] & 040)
      fault(ARITHFAULT, fcode, faddr);
    break;
  case 'f':
    if (!(crs[KEYS] & 01000))
      fault(ARITHFAULT, fcode, faddr);
    break;
  default:
    printf(" Unrecognized exception type '%c'\n", extype);
    fatal(NULL);
  }
}

#include "fp.h"

static memdump(int start, int end) {
  int ea;

  if (domemdump) {

    /* dump sector zero for debugging */

    TRACEA("\nSector 0:\n");
    for (ea=0; ea<01000; ea=ea+8)
      if (mem[ea]|mem[ea+1]|mem[ea+2]|mem[ea+3]|mem[ea+4]|mem[ea+5]|mem[ea+6]|mem[ea+7])
	TRACEA("%3o: %6o %6o %6o %6o %6o %6o %6o %6o\n", ea, mem[ea], mem[ea+1], mem[ea+2], mem[ea+3], mem[ea+4], mem[ea+5], mem[ea+6], mem[ea+7]);

    /* dump main memory for debugging */

    TRACEA("\nMain memory:\n");
    for (ea=start; ea<=end; ea=ea+8)
      if (mem[ea]|mem[ea+1]|mem[ea+2]|mem[ea+3]|mem[ea+4]|mem[ea+5]|mem[ea+6]|mem[ea+7])
	TRACEA("%o: %6o %6o %6o %6o %6o %6o %6o %6o\n", ea, mem[ea], mem[ea+1], mem[ea+2], mem[ea+3], mem[ea+4], mem[ea+5], mem[ea+6], mem[ea+7]);
  }
}


static dumpsegs() {
  short seg,nsegs,i,page,segno;
  unsigned short pte,xxx;
  unsigned int dtar,staddr,sdw,ptaddr,pmaddr;

  TRACEA("\nSEGMENT TABLE DUMP:\n");
  for (i=0; i<4; i++) {
    dtar = *(unsigned int *)(crs+DTAR0-2*i);  /* get dtar register */
    nsegs = 1024-(dtar>>22);
    staddr = (dtar & 0x003F0000) | ((dtar & 0xFFFF)<<1);
    TRACEA("DTAR %d: register=%o, size=%d, seg table addr=%o\n", i, dtar, nsegs, staddr);
    for (seg=0; seg<nsegs; seg++) {
      segno = (i<<10)+seg;
      sdw = *(unsigned int *)(mem+staddr);
      ptaddr = ((sdw & 0x3F)<<10) | (sdw>>22);
      TRACEA("Segment '%o: F=%d, R1:%o R3:%o PT = %o\n", segno, (sdw>>15)&1, (sdw>>12)&7, (sdw>>6)&7, ptaddr);
      xxx = (sdw>>16)&0x3F;
      if (xxx != 0) TRACEA("WARNING: X=%o\n", xxx);
      if (ptaddr != 0)
	for (page=0; page<64; page++) {
	  pmaddr = (ptaddr<<6) + page;
	  pte = mem[pmaddr];
	  TRACEA(" Seg %o page %d: pmaddr=%o, V=%d R=%d U=%d S=%d PPA=%o\n", segno, page, pmaddr, pte>>15, (pte>>14)&1, (pte>>13)&1, (pte>>12)&1, pte&0xFFF);
	}
      staddr += 2;
    }
  }
}


/* NOTE: this needs get16r0 */

static unsigned short dumppcb(unsigned short pcb) {
  short i;
  unsigned short nextpcb;
  ea_t ea;

  ea = MAKEVA(crs[OWNERH],pcb);
  TRACEA("PCB %06o:\n", pcb);
  TRACEA("  Level: %o\n", get16(ea+0));
  nextpcb = get16(ea+1);
  TRACEA("  Link: %o\n", nextpcb);
  TRACEA("  Wait list: %o/%o\n", get16(ea+2), get16(ea+3));
  TRACEA("  Abort flags: %o\n", get16(ea+4));
  TRACEA("  CPU flags: %o\n", get16(ea+5));
  TRACEA("  6,7 (reserved): %o %o\n", get16(ea+6), get16(ea+7));
  TRACEA("  Elapsed timers: %d %d\n", get16(ea+8), get16(ea+9));
  TRACEA("  DTAR 2 & 3: %o|%o  %o|%o\n", get16(ea+10), get16(ea+11), get16(ea+12), get16(ea+13));
  TRACEA("  Process interval timer: %o\n", get16(ea+14));
  TRACEA("  15 (reserved): %o\n", get16(ea+15));
  TRACEA("  Save mask: %o\n", get16(ea+16));
  TRACEA("  Keys: %o\n", get16(ea+17));
  for (i=0; i<16; i++) {
    TRACEA("  %06o %06o", get16(ea+18+2*i), get16(ea+19+2*i));
    if (i==7 || i==15)
      TRACEA("\n");
  }
  TRACEA("  R0 Fault vec: %o/%o\n", get16(ea+50), get16(ea+51));
  TRACEA("  R1 Fault vec: %o/%o\n", get16(ea+52), get16(ea+53));
  TRACEA("  R2 Fault vec: %o/%o\n", get16(ea+54), get16(ea+55));
  TRACEA("  R3 Fault vec: %o/%o\n", get16(ea+56), get16(ea+57));
  TRACEA("  PG Fault vec: %o/%o\n", get16(ea+58), get16(ea+59));
  TRACEA("  Conc. Stack Hdr: %o %o %o\n", get16(ea+60), get16(ea+61), get16(ea+62));
  TRACEA("\n");
  return nextpcb;
}

/* stack extension, called with size of extension in 16-bit words,
   returns a pointer to the extension */

static ea_t stex(unsigned int extsize) {
  short stackrootseg;
  ea_t stackrootp, stackfp;

  if (extsize & 1) extsize++;
  stackrootseg = get16((*(unsigned int *)(crs+SB))+1);
  stackrootp = MAKEVA(stackrootseg,0);
  stackfp = get32(stackrootp);

  /* find a stack segment where this extension will fit */

  while (stackfp != 0 && (stackfp & 0xFFFF) + extsize > 0xFFFF) {
    stackfp = get32(MAKEVA(stackfp>>16, 2));
    TRACE(T_INST, " no room for frame, extension pointer is %o/%o\n", stackfp>>16, stackfp&0xFFFF);
  }
  if (stackfp == 0)
    fault(STACKFAULT, 0, MAKEVA(stackrootseg,0) | (RP & RINGMASK32));

  /* update the stack free pointer */

  put32((stackfp+extsize) & ~RINGMASK32, stackrootp);
  TRACE(T_INST, " stack extension is at %o/%o\n", stackfp>>16, stackfp&0xffff);
  return stackfp;
}

/* for PRTN, load values into temps first so that if any faults occur,
   PRTN can be restarted

   XXX: the order of this look wrong - stack free pointer shouldn't
   be updated if a fault occurs fetching base registers
 */

static void prtn() {
  unsigned short stackrootseg;
  ea_t newrp,newsb,newlb;
  unsigned short keys;

  stackrootseg = get16(*(unsigned int *)(crs+SB)+1);
  put32(*(unsigned int *)(crs+SB), MAKEVA(stackrootseg,0));
  newrp = get32(*(unsigned int *)(crs+SB)+2);
  newsb = get32(*(unsigned int *)(crs+SB)+4);
  newlb = get32(*(unsigned int *)(crs+SB)+6);
  keys = get16(*(unsigned int *)(crs+SB)+8);
  RP = newrp | (RP & RINGMASK32);
  *(unsigned int *)(crs+SB) = newsb;
  *(unsigned int *)(crs+LB) = newlb;
  newkeys(keys & 0177770);
  TRACE(T_INST, " Finished PRTN, RP=%o/%o\n", RPH, RPL);
}



/* NOTE: the brsave array contains copies of the PB, SB, and LB base
   registers at the time of the PCL, to compute argument effective
   addresses.  If the PCL faults during argument transfer, the ARGT
   instruction will reload this array from the new stack frame
   header. */

static ea_t pclea(unsigned short brsave[6], ea_t rp, unsigned short *bitarg, short *store, short *lastarg) {
  unsigned short ibr, br, ea_s, ea_w, bit, a;
  unsigned int utempl;
  ea_t ea, iwea;

  iwea = 0;
  *store = 0;
  utempl = get32(rp);
  ibr = utempl >> 16;
  a = utempl & 0xFFFF;
  bit = (ibr >> 12) & 0xF;
  *store = ibr & 0100;
  *lastarg = ibr & 0200;
  br = (ibr >> 8) & 3;
  TRACE(T_PCL, " PCLAP @ %o/%o, ibr=%o, br=%d, i=%d, bit=%d, store=%d, lastarg=%d, a=%o\n", rp>>16, rp&0xffff, ibr, br, (ibr & 004000) != 0, bit, (*store != 0), (*lastarg != 0), a);
  if (br != 3) {
    ea_s = brsave[2*br] | (RPH & RINGMASK16);
    ea_w = brsave[2*br + 1];
    ea_w += a;
  } else {
    ea_s = crs[XBH] | (RPH & RINGMASK16);
    ea_w = crs[XBL];
    ea_w += a;
    if (crs[XB] & EXTMASK16) {
      bit += crs[X];
      if (bit > 15) {
	bit -= 16;
	ea_w++;
      }
      if (bit == 0)
	ea_s &= ~EXTMASK16;
    }
  }
  ea = MAKEVA(ea_s, ea_w);
  if (bit)
    ea |= EXTMASK32;
  TRACE(T_PCL, " PCLAP ea = %o/%o, bit=%d\n", ea_s, ea_w, bit);
  if (ibr & 004000) {             /* indirect */
    if (ea & 0x80000000)
      fault(POINTERFAULT, ea>>16, 0);    /* XXX: faddr=0? */
    iwea = ea;
    ea = get32(iwea);
    TRACE(T_PCL, " Indirect pointer is %o/%o\n", ea>>16, ea & 0xFFFF);
#if 1
    if (ea & 0x80000000) {
      if (!*store || (ea & 0x8FFF0000) != 0x80000000)
	fault(POINTERFAULT, ea>>16, iwea);
    } else
      ea |= (RP & RINGMASK32);             /* weaken */
#endif
    bit = 0;
    if (ea & EXTMASK32)
      bit = get16(iwea+2) >> 12;
    TRACE(T_PCL, " After indirect, PCLAP ea = %o/%o, bit=%d\n", ea>>16, ea & 0xFFFF, bit);
  }

  if (bit) {
    ea |= EXTMASK32;
    *bitarg = bit;
  } else
    *bitarg = 0;

  if (!*store) {
    *(unsigned int *)(crs+XB) = ea | (RP & RINGMASK32);;
    crs[X] = bit;
  }

  return ea;
}


/* for ARGT:
   Registers:
   - RP points to the ARGT instruction
   - SB points to the new stack frame
   - LB is for the called procedure
   - Y is new frame offset of the next argument
   - YL is the number of arguments left to transfer (HACK!)
   - X is used to store the EA bit offset (for unstored AP)
   - XL is used to store the "lastarg seen" flag
   - XB is used to store the EA seg/word (for unstored AP)
   Stack frame:
   - PB points to the next argument template to be evaluated
   - SB is the caller's saved SB
   - LB is the caller's saved LB
*/

static argt() {
  unsigned short brsave[6];
  unsigned short argsleft, argdisp, bit;
  short lastarg, store;
  unsigned int utempl;
  unsigned short ecby;          /* last offset where ecb temp ea was stored */
  ea_t ea, stackfp, rp, ecbea;
  unsigned short advancepb, advancey;

  TRACE(T_PCL, "Entered ARGT\n");

  /* stackfp is the new stack frame, rp is in the middle of
     argument templates and is advanced after each transfer */

  stackfp = *(unsigned int *)(crs+SB);
  rp = get32(stackfp+2);

  /* reload the caller's base registers for EA calculations */
  
  brsave[0] = rp >> 16;    brsave[1] = 0;
  *(long long *)(brsave+2) = get64(stackfp+4);
  
  argdisp = crs[Y];
  argsleft = crs[YL];
  while (argsleft > 0 || !crs[XL]) {

    TRACE(T_PCL, " Transferring arg, %d left, Y=%o\n", argsleft, crs[Y]);

    advancey = 0;
    if (crs[XL]) {
      ea = 0x80000000;
      store = 1;
      advancepb = 0;
    } else {
      ea = pclea(brsave, rp, &bit, &store, &lastarg);
      advancepb = 1;
    }
    if (argsleft > 0 && store) {
      TRACE(T_PCL, " Storing arg, %d left, Y=%o\n", argsleft, crs[Y]);

      /* NOTE: some version of ucode only store 16 bits for omitted args.
	 Set EHDB to prevent this error.

	 Case 29 wants ring/E-bits preserved for omitted arguments */

      /* 6650 (cpuid 40) strips ring bits on omitted arguments in
	 V-mode, but leaves them in I-mode :-( */

#define OMITTEDARG_MASK1 0x8FFFFFFF
#define OMITTEDARG_MASK2 0xEFFFFFFF

      if ((ea & 0x8FFF0000) == 0x80000000) {
	ea = ea & OMITTEDARG_MASK2;      /* keep ring bits */
#if 0
	if ((crs[KEYS] & 0016000) == 0010000)
	  ea = ea & OMITTEDARG_MASK2;      /* I-mode keeps ring bits */
	else
	  ea = ea & OMITTEDARG_MASK1;      /* V-mode strips ring bits */
#endif
#if 0
	ea = MAKEVA(0100000,0);            /* some machines do this instead */
#endif
	put32(ea, stackfp+crs[Y]);
      } else {
	put32(ea, stackfp+crs[Y]);
	if (ea & EXTMASK32)
	  put16(bit<<12, stackfp+crs[Y]+2);
      }
      TRACE(T_PCL, " Stored arg IP at %o/%o\n\n", stackfp>>16, (stackfp+crs[Y]) & 0xFFFF);
      argsleft--;
      advancey = 1;
    }

    /* advance rp/pb in new stack frame past this template, and
       advance Y to the next arg displacement in the stack.  Y
       has to be advanced last because the PB store may fault.
       If it does, the ARGT starts over, and this argument will
       have to be transferred again. */

    if (advancepb) {
      rp += 2;
      put32(rp, stackfp+2);
      crs[XL] = lastarg;
    }
    if (advancey) {
      crs[Y] += 3;
      crs[YL]--;
    }
  }

  TRACE(T_PCL, " Return RP=%o/%o\n", rp>>16, rp&0xffff);
}


static pcl (ea_t ecbea) {
  short i,j;
  unsigned short access;
  unsigned short ecb[9];
  short bit;                  /* bit offset for args */
  ea_t newrp;                 /* start of new proc */
  ea_t ea;
  ea_t rp;                    /* return pointer */
  short stackrootseg;
  unsigned short stacksize;
  short store;                /* true if store bit set on AP */
  short storedargs;           /* # of arguments that have been stored */
  short lastarg;              /* true if "last" bit seen in PCL arglist */
  ea_t argp;                  /* where to store next arg in new frame */
  ea_t stackfp;               /* new stack frame pointer */
  pa_t pa;                    /* physical address of ecb */
  unsigned short brsave[6];   /* old PB,SB,LB */
  unsigned short utempa;
  unsigned char tnstring[500];
  unsigned short tnlen, tnword;
  unsigned char tnchar;

#define UNWIND_ MAKEVA(013,0106577)

#if 0
  if (ecbea == UNWIND_) {
    printf("pcl: calling unwind_ at %d\n", gvp->instcount);
    gvp->savetraceflags = ~TB_MAP;
  }
#endif

  /* get segment access; mapva ensures either read or gate */

  pa = mapva(ecbea, RP, PACC, &access);
  TRACE(T_PCL, " ecb @ %o/%o, access=%d\n", ecbea>>16, ecbea&0xFFFF, access);

  /* get a copy of the ecb.  gates must be aligned on a 16-word
     boundary, therefore can't cross a page boundary, and mapva has
     already ensured that the ecb page is resident.  For a non-gate
     ecb, check to see if it crosses a page boundary.  If not, a 
     memcpy is okay; if it does, do fetches */

  if (access == 1) {
    if ((ecbea & 0xF) != 0)
      fault(ACCESSFAULT, 0, ecbea);
    memcpy(ecb,mem+pa,sizeof(ecb));
  } else if ((pa & 01777) <= 01750) {
    memcpy(ecb,mem+pa,sizeof(ecb));
  } else {
    *(long long *)(ecb+0) = get64(ecbea+0);
    *(long long *)(ecb+4) = get64(ecbea+4);
    ecb[8] = get16(ecbea+8);
  }

  TRACE(T_PCL, " ecb.pb: %o/%o\n ecb.framesize: %d\n ecb.stackroot %o\n ecb.argdisp: %o\n ecb.nargs: %d\n ecb.lb: %o/%o\n ecb.keys: %o\n", ecb[0], ecb[1], ecb[2], ecb[3], ecb[4], ecb[5], ecb[6], ecb[7], ecb[8]);

  newrp = *(unsigned int *)(ecb+0);
  if (access != 1)    /* not a gate, but weaken ring (outward calls) */
    newrp = newrp | (RP & RINGMASK32);

  /* setup stack frame
     NOTE: newrp must be used here so that accesses succeed when calling 
     an inner ring procedure. */

  stackrootseg = ecb[3];
  if (stackrootseg == 0) {
    stackrootseg = get16((*(unsigned int *)(crs+SB)) + 1);
    TRACE(T_PCL, " stack root in ecb was zero, stack root from caller is %o\n", stackrootseg);
  }
  stackfp = get32r(MAKEVA(stackrootseg,0), newrp);
  TRACE(T_PCL, " stack free pointer: %o/%o, current ring=%o, new ring=%o\n", stackfp>>16, stackfp&0xFFFF, (RPH&RINGMASK16)>>13, (newrp&RINGMASK32)>>29);
  stacksize = ecb[2];

  /* if there isn't room for this frame, check the stack extension
     pointer */

  if ((stackfp & 0xFFFF) + stacksize > 0xFFFF) {
    stackfp = get32r(MAKEVA(stackrootseg,2), newrp);
    TRACE(T_PCL, " no room for frame, extension pointer is %o/%o\n", stackfp>>16, stackfp&0xFFFF);

    /* XXX: faddr may need to be the last segment tried when this is changed to loop.
       CPU.PCL Case 26 wants fault address word number to be 3; set EHDB */

    if (stackfp == 0 || (stackfp & 0xFFFF) + stacksize > 0xFFFF)
      fault(STACKFAULT, 0, MAKEVA(stackrootseg,0) | (newrp & RINGMASK32));
  }

  /* setup the new stack frame at stackfp

     NOTE: Ring must be added to stackfp so that any page faults that
     occur while setting up the stack will have the correct ring for
     CPU.PCL tests */

  stackfp |= (newrp & RINGMASK32);
  put16r(0, stackfp, newrp);
  put16r(stackrootseg, stackfp+1, newrp);
  put32r(RP, stackfp+2, newrp);
  put32r(*(unsigned int *)(crs+SB), stackfp+4, newrp);
  put32r(*(unsigned int *)(crs+LB), stackfp+6, newrp);
  put16r(crs[KEYS], stackfp+8, newrp);
  put16r(RPL, stackfp+9, newrp);

#if 0
  /* LATER: save caller's base registers for address calculations, and
     pass to argt */

  if (ecb[5] > 0) {
    brsave[0] = RPH;       brsave[1] = 0;
    brsave[2] = crs[SBH];  brsave[3] = crs[SBL];
    brsave[4] = crs[LBH];  brsave[5] = crs[LBL];
  }
#endif

  /* load new execution state from ecb */

  TRACE(T_PCL, " before update, stackfp=%o/%o, SB=%o/%o\n", stackfp>>16, stackfp&0xFFFF, crs[SBH], crs[SBL]);
  if (access == 1)                 /* for gate access, don't weaken ring */
    *(unsigned int *)(crs+SB) = stackfp;
  else
    *(unsigned int *)(crs+SB) = (stackfp & ~RINGMASK32) | (RP & RINGMASK32);
  TRACE(T_PCL, " new SB=%o/%o\n", crs[SBH], crs[SBL]);
  *(unsigned int *)(crs+LB) = *(unsigned int *)(ecb+6);
  newkeys(ecb[8] & 0177770);

  /* update the stack free pointer; this has to wait until after all
     memory accesses, in case of stack page faults (PCL restarts).

     Some ucode versions incorrectly store the ring in the free
     pointer if the extension pointer was followed.  Try setting EHDB
     to suppress this spurious DIAG error. */

  ea = MAKEVA(stackrootseg,0) | (newrp & RINGMASK32);
  if (cpuid == 15)
    put32r(stackfp+stacksize, ea, newrp);
  else
    put32r((stackfp+stacksize) & ~RINGMASK32, ea, newrp);

  /* transfer arguments if arguments are expected.  There is no
     documentation explaining how the Y register is used during
     argument transfer, so:
     Y(high) = stack frame offset to store next argument
     Y(low) = number of arguments left to transfer (JW hack!) */


  /* if a page fault occurs during argument transfer, we need to 
     make sure to use the current RP, which points to the ARGT
     instruction.  Otherwise, the return from the page fault
     is to the PCL instruction, which has already completed at
     this point */

  RP = newrp;
  gvp->prevpc = RP;
  TRACE(T_PCL, " new RP=%o/%o\n", RPH, RPL);

  if (ecb[5] > 0) {
    crs[Y] = ecb[4];
    crs[YL] = ecb[5];
    crs[XL] = 0;
    argt();

    /* if tracing terminal output, display it now.  This has to occur
     after ARGT has setup the argument pointers.  Note that if a fault
     occurs while accessing the arguments here, it will return to ARGT
     in the main emulator loop and nothing will be logged. */

    if (TRACEUSER && ((ecbea & 0xFFFFFFF) == tnou_ea || (ecbea & 0xFFFFFFF) == tnoua_ea)) {
      ea = *(unsigned int *)(crs+SB) + ecb[4];
      utempa = get16(get32(ea));       /* 1st arg: userid */
      if (utempa == ((crs[OWNERL]>>6) & 0xff)) {
	ea = ea + 6;                   /* 3rd arg: length */
	tnlen = get16(get32(ea));
	ea = get32(ea-3);              /* 2nd arg: string */
	j = 0;
	for (i=0; i<tnlen; i++) {
	  if (i & 1)
	    tnchar = tnword & 0x7f;
	  else {
	    tnword = get16(ea+i/2);
	    tnchar = (tnword >> 8) & 0x7f;
	  }
	  if (j > sizeof(tnstring)-5)
	    j = sizeof(tnstring)-5;
	  if (tnchar >= ' ' && tnchar < 0177)
	    tnstring[j++] = tnchar;
	    else {
	      sprintf((char *)(tnstring+j), "%03o ", tnchar);
	      j = j+4;
	    }
	}
	tnstring[j] = 0;
	TRACE(T_TERM, " TNOUx user %d, len %d: %s\n", utempa, tnlen, tnstring);
      }
    }

    INCRP;    /* advance real RP past ARGT after argument transfer */
  }
}

/* NOTE: the calf instruction may be running in an outer ring, so
   accesses to protected data need to use get16r0 */

static void calf(ea_t ea) {
  ea_t pcbp, stackfp, csea;
  unsigned short first,next,last,this;
  unsigned short cs[6];

  pcbp = *(ea_t *)(crs+OWNER);    /* my pcb pointer */

  /* get concealed stack entry address */

  first = get16r0(pcbp+PCBCSFIRST);
  next = get16r0(pcbp+PCBCSNEXT);
  last = get16r0(pcbp+PCBCSLAST);
  TRACE(T_FAULT, "CALF: first=%o, next=%o, last=%o\n", first, next, last);
  if (next == first)
    this = last;
  else
    this = next-6;
  csea = MAKEVA(crs[OWNERH]+gvp->csoffset, this);
  TRACE(T_FAULT,"CALF: cs frame is at %o/%o\n", csea>>16, csea&0xFFFF);

  /* make sure ecb specifies zero args (not part of the architecture)

     NOTE: this check needs get16r0 too because in Rev 19, segment 5
     only has gate access and this read caused an access fault when an
     R-mode I/O instruction occurs under Primos (causing a restricted
     inst fault that is handled in the outer ring). */

  if (get16r0(ea+5) != 0) {
    printf("CALF ecb at %o/%o has arguments!\n", ea>>16, ea&0xFFFF);
    fatal(NULL);
  }

  pcl(ea);

  /* get the concealed stack entries and adjust the new stack frame */

  *(unsigned int *)(cs+0) = get32r0(csea+0);
  *(long long *)(cs+2) = get64r0(csea+2);

  TRACE(T_FAULT, "CALF: cs entry: retpb=%o/%o, retkeys=%o, fcode=%o, faddr=%o/%o\n", cs[0], cs[1], cs[2], cs[3], cs[4], cs[5]);

  stackfp = *(unsigned int *)(crs+SB);
  put16(1, stackfp+0);                          /* flag it as CALF frame */
  put32(*(unsigned int *)(cs+0), stackfp+2);    /* return PB */
  put16(cs[2], stackfp+8);                      /* return keys */
  put16(cs[3], stackfp+10);                     /* fault code */
  put32(*(unsigned int *)(cs+4), stackfp+11);   /* fault address */

  /* pop the concealed stack */

  put16r0(this, pcbp+PCBCSNEXT);
}


/* process exchange register save:  saves the current register
   set to the process pcb. 
   NOTES:
   - adding "wait" arg and only saving base registers fixed Case 63
*/

static pxregsave(unsigned short wait) {
  ea_t pcbp, regp;
  int i;
  unsigned short mask;

  /* if registers aren't owned or are already saved, return */

  if (crs[OWNERL] == 0) {
    TRACE(T_PX, "pxregsave: OWNERL is zero: no save\n");
    return;
  }
  if (crs[KEYS] & 1) {
    TRACE(T_PX, "pxregsave: SD=1: no save\n");
    return;
  }

  TRACE(T_PX, "pxregsave: saving registers owned by %o (wait=%d)\n", crs[OWNERL], wait);
    
  /* NB: I think hardware might save the base registers in a predictable
     location in the PCB register save area, rather than compressed in a
     random order, because IIRC, Primos sometimes looks at a waiting
     process' PB to see where it is waiting */

  pcbp = *(unsigned int *)(crs+OWNER);
  regp = pcbp+PCBREGS;
  mask = 0;
  for (i=(wait?014:0); i<020; i++) {
    if (crsl[i] != 0) {
      mask |= BITMASK16(i+1);
      put32r0(crsl[i], regp);
      regp += 2;
    }
  }
  put16r0(mask, pcbp+PCBMASK);
  put32r0(*(unsigned int *)(crs+TIMER), pcbp+PCBIT);  /* save interval timer */
  crs[KEYS] |= 1;                     /* set save done bit */
  put16r0(crs[KEYS], pcbp+PCBKEYS);
}

/* pxregload: load pcbp's registers from their pcb to the current
   register set, set OWNERL

   NOTE: RP must be set by the caller since this happens whenever
   a process is dispatched - not just when registers are loaded */

static pxregload (ea_t pcbp) {
  ea_t regp;
  unsigned short mask, modals;
  int i;

  TRACE(T_PX, "pxregload loading registers for process %o/%o\n", pcbp>>16, pcbp&0xFFFF);
  regp = pcbp+PCBREGS;
  mask = get16r0(pcbp+PCBMASK);
  for (i=0; i<020; i++) {
    if (mask & BITMASK16(i+1)) {
      crsl[i] = get32r0(regp);
      regp += 2;
    } else {
      crsl[i] = 0;
    }
  }
  newkeys(get16r0(pcbp+PCBKEYS));
  *(unsigned int *)(crs+DTAR2) = get32r0(pcbp+PCBDTAR2);
  *(unsigned int *)(crs+DTAR3) = get32r0(pcbp+PCBDTAR3);
  *(unsigned int *)(crs+TIMER) = get32r0(pcbp+PCBIT);
  crs[OWNERL] = pcbp & 0xFFFF;

  TRACE(T_PX, "pxregload: registers loaded, ownerl=%o, modals=%o\n", crs[OWNERL], crs[MODALS]);
}


/* selects a register set and sets modals and crs/crsl to that register set.
   pcbw is OWNERL of the process that will use the register set. */

static ors(unsigned short pcbw) {
  static short regq[] = {0,1,2,3,4,5,6,7};
  short i,rx;
  unsigned short ownerl, currs, rs;
  short ownedx, freex, savedx;
  unsigned short modals;

#define NUREGS 8

  currs = (crs[MODALS] & 0340) >> 5;
  TRACE(T_PX, "ors: currs = %d, modals = %o\n", currs, crs[MODALS]);
#if 0

  /* this is the code for handling more than 2 register sets.  It is
     "smarter" than the Prime u-code, so probably doesn't pass DIAG
     tests.  I haven't tested whether the extra overhead of keeping a
     LRU queue for register sets is worth it vs. the simpler Prime
     way.  One problem is that different models had different #'s of
     registers, and the emulator needs a table of these values.
     Either that, or it probably could initialize all register sets
     using the first (user) register set as a template when process
     exchange is enabled.  (Primos only initializes the number of
     user register sets that a particular model actually has.) */

  ownedx = freex = savedx = -1;
  for (rx = NUREGS-1; rx >= 0; rx--) {   /* search LRU first */
    rs = regq[rx];
    TRACE(T_PX, "ors: check rs %d: owner=%o/%o, saved=%d\n", rs, regs.sym.userregs[rs][21]>>16, regs.sym.userregs[rs][21] & 0xFFFF, regs.sym.userregs[rs][20] & 1);
    ownerl = regs.sym.userregs[rs][21] & 0xFFFF;          /* OWNERH/OWNERL */

    /* NOTE: could stick breaks after a rs is found, except that for
       debug, I wanted to make sure a process never owns 2 register sets */

    if (ownerl == pcbw) {
      if (ownedx >= 0) 
	fatal("Process owns more than 1 register set!");
      ownedx = rx;
    } else if (ownerl == 0)
      freex = rx;
    else if (savedx < 0 && regs.sym.userregs[rs][20] & 1) /* KEYS/MODALS */
      savedx = rx;
  }
  if (ownedx >= 0) {
    rx = ownedx;
    TRACE(T_PX, "ors: using owned reg set %d\n", regq[rx]);
  } else if (freex >= 0) {
    rx = freex;
    TRACE(T_PX, "ors: using free reg set %d\n", regq[rx]);
  } else if (savedx >= 0) {
    rx = savedx;
    TRACE(T_PX, "ors: using saved reg set %d\n", regq[rx]);
  } else {
    rx = NUREGS-1;                           /* least recently used */
    TRACE(T_PX, "ors: no reg set found; using %d\n", regq[rx]);
  }
  rs = regq[rx];
  if (rs > NUREGS)
    fatal("ors: rs chosen is too big");
  modals = (crs[MODALS] & ~0340) | (rs << 5);

  /* put the register set selected at the front of the queue */

  for (i=rx; i>0; i--)
    regq[i] = regq[i-1];
  regq[0] = rs;
#else
  modals = crs[MODALS] ^ 040;
  rs = (modals & 0340) >> 5;
#endif
  crsl = regs.sym.userregs[rs];
  //NOTE: following is unnecessary because crs is an alias for crsl
  //crs = (void *)crsl;
  crs[MODALS] = modals;
  TRACE(T_PX, "ors: rs = %d, reg set in modals = %d, modals = %o\n", rs, (crs[MODALS] & 0340)>>5, crs[MODALS]);
#if 0
  if (rs > 1)
    gvp->savetraceflags = ~0;
#endif
}


/* the process exchange dispatcher's job is to:
   - determine the highest priority process ready to run
   - find a register set to use
   - save the registers if they are currently owned and not already saved
   - load this process' registers into the register set
   - clear the save done bit in keys
   - cause a process fault if any of this process' pcb abort flags are set

   If no process can be found to run, the dispatcher idles and waits
   for an external interrupt.
 */

static dispatcher() {
  ea_t pcbp, rlp;
  unsigned short pcbw;      /* pcb word address */
  unsigned short rsnum;
  unsigned short rlbol;
  unsigned short utempa;


  crs[MODALS] |= 0100000;               /* ISG says dispatcher enables int. */

  if (regs.sym.pcba != 0) {
    pcbp = MAKEVA(crs[OWNERH], regs.sym.pcba);
    TRACE(T_PX, "disp: dispatching PPA, pcba=%o, pla=%o\n", regs.sym.pcba, regs.sym.pla);

  } else if (regs.sym.pcbb != 0) {
    pcbp = MAKEVA(crs[OWNERH], regs.sym.pcbb);
    regs.sym.pcba = regs.sym.pcbb;
    regs.sym.pla = regs.sym.plb;
    regs.sym.pcbb = 0;
    TRACE(T_PX, "disp: dispatching PPB, pcba=%o, pla=%o\n", regs.sym.pcba, regs.sym.pla);

  } else {
    TRACE(T_PX, "disp: scanning RL\n");
    if (regs.sym.pla != 0)
      rlp = MAKEVA(crs[OWNERH], regs.sym.pla);
    else if (regs.sym.plb != 0)
      fatal("disp: pla is invalid, plb is valid?");
    else
      fatal("dispatch: both pla and plb are zero; can't locate ready list");
    while(1) {
      rlbol = get16r0(rlp);
      if (rlbol != 0)
	break;
      rlp += 2;
    }
    if (rlbol == 1)
      goto idle;
    pcbp = MAKEVA(crs[OWNERH], rlbol);
    regs.sym.pcba = rlbol;
    regs.sym.pla = rlp & 0xFFFF;
  }
  pcbw = pcbp & 0xFFFF;
  TRACE(T_PX, "disp: process %o/%o selected\n", pcbp>>16, pcbw);

#if 0
  /* debug tests to verify ready list structure
     NOTE: this test causes some DIAGS to fail, so has been disabled */

  rlp = MAKEVA(crs[OWNERH], regs.sym.pla);
  rlbol = get16r0(rlp);
  if (rlbol != pcbw) {
    printf("disp: rl bol=%o, != process dispatched=%o\n", rlbol, pcbw);
    fatal(NULL);
  }
#endif
#if 0
  /* NOTE: if a running process has its priority changed (in the pcb), this
     test fails, so it has been disabled  */

  if (get16r0(pcbp+PCBLEV) != regs.sym.pla) {
    printf("disp: dispatched process level=%o, != pla=%o\n", get16r0(pcbp+PCBLEV), regs.sym.pla);
    fatal(NULL);
  }
#endif
  
  /* pcbp now points to the process we're going to run (pcbw is the
     16-bit word number that will go in OWNERL).  By definition, this
     process should not be on any wait lists, so pcb.waitlist(seg)
     should be zero.  Check it */

#if 1
  /* NOTE: CPU.PXT1 can fail with this enabled */

  utempa = get16r0(pcbp+PCBWAIT);
  if (utempa != 0) {
    printf("disp: pcb %o/%o selected, but wait segno = %o\n", pcbp>>16, pcbp&0xFFFF, utempa);
    fatal(NULL);
  }
#endif

  /* save RP in current register set before possibly switching */

  *(unsigned int *)(crs+PB) = RP;

  /* find a register set for this process */

#if 0
  rsnum = (crs[MODALS] & 0340)>>5;
  if (crs[OWNERL] != pcbw && crs[OWNERL] != 0)
    if (regs.rs16[rsnum ^ 1][OWNERL] == 0 || (regs.rs16[rsnum ^ 1][OWNERL] == pcbw && (regs.rs16[rsnum ^ 1][KEYS] & 1)) || ((regs.rs16[rsnum ^ 1][KEYS] & 1) && !(crs[KEYS] & 1)))
      ors();
#endif

  /* Cases that fail w/o any register switch:
     - 3   err dispatch
     - 5   reg data not saved correctly
     - 7   crs.modals
     - 11  crs.modals
     - 13  crs.modals
     - 17  err dispatch
     - 19  crs.modals
     - 25  crs.modals
     - 27  err dispatch
     - 31  crs.modals
     - 45  err dispatch
     - 47  crs.modals
     - 49  err dispatch
     - 51  crs.modals
     - 61  crs.modals
     - 63  ors.X wrong
     - 81  save mask wrong
     - 83  crs.modals

     Cases that fail with simple register switch below:
     - 9   crs.modals
     - 23  crs.modals
     - 29  crs.modals
  *  - 31  saved X wrong
  *  - 83  crs.modals
     - 85  bad
     - 89  crs.modals  - switches register sets
     - 90  crs.modals

  Adding "&& crs[OWNERL] != 0" and setting this to zero in IRTN if PPA is
  invalid fixes case 81 w/o breaking any other tests.

  */

#if 1
  if (crs[OWNERL] != pcbw && crs[OWNERL] != 0)
    ors(pcbw);
#endif

  /* If the selected register set is owned and hasn't been saved, save
     it before taking it */

  if (crs[OWNERL] == pcbw) {
    TRACE(T_PX, "disp: reg set already owned by %o: no save or load\n", crs[OWNERL]);
    /* NOTE: call newkeys to make sure amask gets set correctly!  Otherwise, 32R mode programs
       are flaky */
    newkeys(crs[KEYS]);
  } else {
    pxregsave(0);
    pxregload(pcbp);
  }

  RP = *(unsigned int *)(crs+PB);
  crs[PBL] = 0;
  crs[KEYS] &= ~3;                           /* erase "in dispatcher" and "save done" */
  TRACE(T_PX, "disp: returning from dispatcher, running process %o/%o at %o/%o, modals=%o, ppa=%o, pla=%o, ppb=%o, plb=%o\n", crs[OWNERH], crs[OWNERL], RPH, RPL, crs[MODALS], regs.sym.pcba, regs.sym.pla, regs.sym.pcbb, regs.sym.plb);

  /* if this process' abort flags are set, clear them and take process fault */

  utempa = get16r0(pcbp+PCBABT);
  if (utempa != 0) {
    TRACE(T_PX, "dispatch: abort flags for %o are %o\n", crs[OWNERL], utempa);
    //printf("dispatch: abort flags for %o are %o\n", crs[OWNERL], utempa);
    put16r0(0, pcbp+PCBABT);
    fault(PROCESSFAULT, utempa, 0);
    fatal("fault returned after process fault");    
  }

  return;

idle:
  fatal("dispatch idle...");
}

/* take me off the ready list, setting my pcb link pointer to the arg
   passed in.  The dispatcher should always be entered after this
   routine. */

static unready (ea_t waitlist, unsigned short newlink) {
  unsigned short level, bol, eol;
  unsigned int rl;
  ea_t rlp, pcbp;

#if 0
  /* this fails with rev 23.4:
Fatal error: instruction #86286965 at 6/15274 UNLOAD+'120: 315 1400
owner=71600 DUMPCB, keys=14000, modals=37
unready: pcba mismatch
  */
  if (regs.sym.pcba != crs[OWNERL])
    fatal("unready: pcba mismatch");
#endif

  pcbp = *(ea_t *)(crs+OWNER);
  rlp = MAKEVA(crs[OWNERH], regs.sym.pla);
  rl = get32r0(rlp);
  bol = rl >> 16;
  eol = rl & 0xFFFF;
#if 0
  /* this fails with rev 23.4:
rlp=240/136, bol=100500, eol=100500, pcbp=240/71600, pla=136, pcba=100500
Fatal error: instruction #86212270 at 6/15274 UNLOAD+'120: 315 1400
owner=71600 DUMPCB, keys=14000, modals=77
unready: I'm not first on the ready list
  */
  if (bol != (pcbp & 0xFFFF)) {
    printf("rlp=%o/%o, bol=%o, eol=%o, pcbp=%o/%o, pla=%o, pcba=%o\n", rlp>>16, rlp&0xFFFF, bol, eol, pcbp>>16, pcbp&0xFFFF, regs.sym.pla, regs.sym.pcba);
    fatal("unready: I'm not first on the ready list");
  }
#endif
  if (bol == eol) {
    bol = 0;
    eol = 0;
  } else {
    bol = get16r0(pcbp+1);
  }
  rl = (bol<<16) | eol;
  put32r0(rl, rlp);           /* update ready list */
  TRACE(T_PX, "unready: new rl bol/eol = %o/%o\n", rl>>16, rl&0xFFFF);
  put16r0(newlink, pcbp+1);   /* update my pcb link */
  put32r0(waitlist, pcbp+2);  /* update my pcb wait address */
  *(unsigned int *)(crs+PB) = RP;
  pxregsave(1);
  regs.sym.pcba = 0;
}


/* pcbp points to the pcb to put on the ready list
   begend is 1 for beginning, 0 for end
   returns true if this process is higher priority than me
*/

static unsigned short ready (ea_t pcbp, unsigned short begend) {
  ea_t rlp;
  ea_t xpcbp;
  unsigned short bol,eol,pcbw,level,resched;
  unsigned int rl;

  if ((pcbp & 0xFFFF) == crs[OWNERL])
    fatal("Tried to put myself on the ready list!");
#if 0
  /* NOTE: restore drive b, boot 14314, halts here after login_server */
  if (regs.sym.pcba != crs[OWNERL])
    fatal("I'm running, but not regs.sym.pcba!");
#endif

  level = get16r0(pcbp+PCBLEV);
  rlp = MAKEVA(crs[OWNERH],level);
  rl = get32r0(rlp);
  TRACE(T_PX, "ready: pcbp=%o/%o\n", pcbp>>16, pcbp&0xFFFF);
  TRACE(T_PX, "ready: old bol/eol for level %o = %o/%o\n", level, rl>>16, rl&0xFFFF);
  pcbw = pcbp;                            /* pcb word number */
  if ((rl>>16) == 0) {                    /* bol=0: this RL level was empty */
    put32r0(0, pcbp+1);                   /* set link and wait SN in pcb */
    rl = (pcbw<<16) | pcbw;               /* set beg=end */
  } else if (begend) {                    /* notify to beginning */
    put32r0(rl & 0xFFFF0000, pcbp+1);     /* set link and wait SN in pcb */
    rl = (pcbw<<16) | rl&0xFFFF;          /* new is bol, eol is unchanged */
  } else {                                /* notify to end */
    put32r0(0, pcbp+1);                   /* set link and wait SN in pcb */
    xpcbp = MAKEVA(crs[OWNERH],rl&0xFFFF); /* get ptr to last pcb at this level */
    put16r0(pcbw,xpcbp+1);                /* set last pcb's forward link */
    rl = (rl & 0xFFFF0000) | pcbw;        /* rl bol is unchanged, eol is new */
  }
  put32r0(rl, rlp);
  TRACE(T_PX, "ready: new bol/eol for level %o = %o/%o, pcb's link is %o\n", level, rl>>16, rl&0xFFFF, get16r0(pcbp+1));

  /* is this new process higher priority than me?  If so, return 1
     so that the dispatcher is entered.  If not, check for new plb/pcbb */

  resched = 0;
  if (level < regs.sym.pla || (level == regs.sym.pla && begend)) {
    regs.sym.plb = regs.sym.pla;
    regs.sym.pcbb = regs.sym.pcba;
    regs.sym.pla = level;
    regs.sym.pcba = pcbw;
    resched = 1;
  } else if (level < regs.sym.plb || (level == regs.sym.plb && begend)) {
    regs.sym.plb = level;
    regs.sym.pcbb = pcbw;
  }
  return resched;
}


static pwait() {
  ea_t ea;
  ea_t pcbp, prevpcbp;
  unsigned int utempl;
  unsigned int pcblevnext;      /* pcb level and link */
  unsigned short bol;
  unsigned short pcblev;
  unsigned short pcbnext;
  unsigned short mylev;
  short count;

  ea = apea(NULL);
  TRACE(T_PX, "%o/%o: wait on %o/%o, pcb %o, keys=%o, modals=%o\n", RPH, RPL, ea>>16, ea&0xFFFF, crs[OWNERL], crs[KEYS], crs[MODALS]);
  utempl = get32r0(ea);       /* get count and BOL */
  count = utempl>>16;         /* count (signed) */
  bol = utempl & 0xFFFF;      /* beginning of wait list */
  TRACE(T_PX, " wait list count was %d, bol was %o\n", count, bol);
  count++;
  if (count > 0) {      /* I have to wait */
    if (count == 1 && bol != 0)
      fatal("WAIT: count == 1 but bol != 0");
    if (count > 1 && bol == 0)
      fatal("WAIT: count > 1 but bol == 0");
    if (regs.sym.pcba == 0)
      fatal("WAIT: pcba is zero");
#if 0
    /* enabling this causes rev 23.4 to fail:
WAIT: pcba=100500 != ownerl=71600
Fatal error: instruction #86137885 at 6/15274 UNLOAD+'120: 315 1400
owner=71600 DUMPCB, keys=14000, modals=77           */

    if (regs.sym.pcba != crs[OWNERL]) {
      printf("WAIT: pcba=%o != ownerl=%o\n", regs.sym.pcba, crs[OWNERL]);
      fatal(NULL);
    }
#endif
    mylev = get16r0(*(ea_t *)(crs+OWNER));

    if (bol != 0) {
      pcbp = MAKEVA(crs[OWNERH],bol);
      pcblevnext = get32r0(pcbp);
      pcblev = pcblevnext >> 16;
    }
    TRACE(T_PX, " my level=%o, pcblev=%o\n", mylev, pcblev);

    if (count == 1 || mylev < pcblev) {   /* add me to the beginning */
      utempl = (count<<16) | crs[OWNERL];
      put32r0(utempl, ea);    /* update semaphore count/bol */
    } else {
      /* do a priority scan... */
      while (pcblev <= mylev && bol != 0) {
	prevpcbp = pcbp;
	bol = pcblevnext & 0xFFFF;
	if (bol != 0) {
	  pcbp = MAKEVA(crs[OWNERH],bol);
	  pcblevnext = get32r0(pcbp);
	  pcblev = pcblevnext >> 16;
	}
      }
      put16r0(crs[OWNERL], prevpcbp+PCBLINK);
      put16r0(*(unsigned short *)&count, ea);    /* update count */
      TRACE(T_PX, " new count=%d, new link for pcb %o=%o, bol=%o\n", count, prevpcbp&0xffff, crs[OWNERL], bol);
    }
    unready(ea, bol);
    dispatcher();
  } else
    put16(*(unsigned short *)&count, ea); /* just update count and continue */
}

/* this handles several forms of notify:
   - 001210 = NFYE
   - 001211 = NFYB
   - 001214 = INEN, notify to end, no CAI
   - 001215 = INBN, notify to beg, no CAI
   - 001216 = INEC, notify to end, CAI
   - 001217 = INBC, notify to beg, CAI
*/

static nfy(unsigned short inst) {
  unsigned short resched, begend, bol, rsnum;
  ea_t ea, pcbp;
  unsigned int utempl;
  short scount;
  static char *nfyname[] = {"nfye","nfyb"," "," ","inen","inbn","inec","inbc"};

  resched = 0;
  begend = inst & 1;
  if (regs.sym.pcba != crs[OWNERL]) {
    printf("NFY: regs.pcba = %o, but crs[OWNERL] = %o\n", regs.sym.pcba, crs[OWNERL]);
    fatal(NULL);
  }
  ea = apea(NULL);
  utempl = get32r0(ea);       /* get count and BOL */
  scount = utempl>>16;        /* count (signed) */
  bol = utempl & 0xFFFF;      /* beginning of wait list */
  TRACE(T_PX, "%o/%o: opcode %o %s, ea=%o/%o, count=%d, bol=%o, I am %o\n", RPH, RPL, inst, nfyname[inst-01210], ea>>16, ea&0xFFFF, scount, bol, crs[OWNERL]);

  /* on later models, semaphore overflow should cause a fault */

  if (scount == -32768) {
    printf("NFY: semaphore overflow at ea %o/%o %s\n", ea>>16, ea&0xFFFF, searchloadmap(ea, 'x'));
    fatal(NULL);
  }

  if (scount > 0) {
    if (bol == 0) {
      printf("NFY: bol is zero, count is %d for semaphore at %o/%o\n", scount, ea>>16, ea&0xFFFF);
      fatal(NULL);
    }
    pcbp = MAKEVA(crs[OWNERH], bol);
    utempl = get32r0(pcbp+PCBWAIT);
    if (utempl != ea) {
      printf("NFY: bol=%o, pcb waiting on %o/%o != ea %o/%o\n", utempl>>16, utempl&0xFFFF, ea>>16, ea&0xFFFF);
      fatal(NULL);
    }
    bol = get16r0(pcbp+PCBLINK);     /* get new beginning of wait list */
    resched = ready(pcbp, begend);     /* put this pcb on the ready list */
  }

  scount = scount-1;
  utempl = (scount<<16) | bol;
  put32r0(utempl, ea);           /* update the semaphore */

  if (inst & 4) {                /* interrupt notify */
    if (inst & 2)                /* clear active interrupt */
      gvp->intvec = -1;
    /* not sure about all this... Case 85/87 */
    RP = regs.sym.pswpb;
    crs[PBH] = RPH;
    newkeys(regs.sym.pswkeys);
  }

  if (resched || (inst & 4))
    dispatcher();
}


static lpsw() {
  ea_t ea;
  unsigned short m;

  TRACE(T_PX, "\n%o/%o: LPSW issued\n", RPH, RPL);
  TRACE(T_PX, "LPSW: before load, RPH=%o, RPL=%o, keys=%o, modals=%o\n", RPH, RPL, crs[KEYS], crs[MODALS]);
  TRACE(T_PX, "LPSW: crs=%d, ownerl[2]=%o, keys[2]=%o, modals[2]=%o, ownerl[3]=%o, keys[3]=%o, modals[3]=%o\n", crs==regs.rs16[2]? 2:3, regs.rs16[2][OWNERL], regs.rs16[2][KEYS], regs.rs16[2][MODALS], regs.rs16[3][OWNERL], regs.rs16[3][KEYS], regs.rs16[3][MODALS]);

  ea = apea(NULL);
  RPH = get16(ea);
  RPL = get16(INCVA(ea,1));
  newkeys(get16(INCVA(ea,2)));
  m = get16(INCVA(ea,3));
  if ((m & 0340) != (crs[MODALS] & 0340)) {
    TRACE(T_PX, "LPSW: WARNING: changed current register set: current modals=%o, new modals=%o\n", crs[MODALS], m);
#if 1
    /* not sure about doing this... */
    printf("WARNING: LPSW changed current register set: current modals=%o, new modals=%o\n", crs[MODALS], m);
    crsl = regs.sym.userregs[(m & 0340) >> 5];
#endif
  }

  crs[MODALS] = m;
  gvp->inhcount = 1;

  TRACE(T_PX, "LPSW:    NEW RPH=%o, RPL=%o, keys=%o, modals=%o\n", RPH, RPL, crs[KEYS], crs[MODALS]);
  TRACE(T_PX, "LPSW: crs=%d, ownerl[2]=%o, keys[2]=%o, modals[2]=%o, ownerl[3]=%o, keys[3]=%o, modals[3]=%o\n", crs==regs.rs16[2]? 2:3, regs.rs16[2][OWNERL], regs.rs16[2][KEYS], regs.rs16[2][MODALS], regs.rs16[3][OWNERL], regs.rs16[3][KEYS], regs.rs16[3][MODALS]);
  if (crs[MODALS] & 020)
    TRACE(T_PX, "Mapped I/O enabled\n");
  if (crs[MODALS] & 4) {
    TRACE(T_PX, "Segmentation enabled\n");
    gvp->livereglim = 010;
  } else 
    gvp->livereglim = 040;
#if 0
  gvp->savetraceflags |= TB_FLOW;    /****/
#endif
  if (crs[MODALS] & 010) {
    TRACE(T_PX, "Process exchange enabled:\n");
    TRACE(T_PX, "LPSW: PLA=%o, PCBA=%o, PLB=%o, PCBB=%o\n", regs.sym.pla, regs.sym.pcba, regs.sym.plb, regs.sym.pcbb);
#if 0
    for (i=regs.sym.pla;; i += 2) {
      ea = MAKEVA(crs[OWNERH], i);
      utempa = get16(ea);
      TRACE(T_PX, " Level %o: BOL=%o, EOL=%o\n", i, utempa, get16(ea+1));
      if (utempa == 1)
	break;
      while (utempa > 0)
	utempa = dumppcb(utempa);
    }
#endif
    if (crs[KEYS] & 2) {
      TRACE(T_PX, "LPSW: before disp, RPH=%o, RPL=%o, keys=%o, modals=%o\n", RPH, RPL, crs[KEYS], crs[MODALS]);
      dispatcher();
      TRACE(T_PX, "LPSW: after disp, RPH=%o, RPL=%o, keys=%o, modals=%o\n", RPH, RPL, crs[KEYS], crs[MODALS]);
      TRACE(T_PX, "LPSW: crs=%d, ownerl[2]=%o, keys[2]=%o, modals[2]=%o, ownerl[3]=%o, keys[3]=%o, modals[3]=%o\n", crs==regs.rs16[2]? 2:3, regs.rs16[2][OWNERL], regs.rs16[2][KEYS], regs.rs16[2][MODALS], regs.rs16[3][OWNERL], regs.rs16[3][KEYS], regs.rs16[3][MODALS]);
    }
  }
#if 0
  /* XXX: hack to disable serial number checking if a cpuid > 4 is used.
     This code is very rev and/or build dependent; this is for 23.4.
     Look for ERA/ANA sequence after SSSN, set the ANA operand to zero. */

  ea = MAKEVA(014,040747);
  printf("Current value of 14/40747 is: %o\n", get16(ea));
  put16(0,ea);

  /* patch SBL instruction to clear L instead */
  ea = MAKEVA(014,020104);
  printf("Current value of 14/20104 is: %o\n", get16(ea));
  put16(0140010, ea);
  put16(0140010, ea+1);
#endif
}


static sssn() {
  ea_t ea;
  int i;

  printf("SSSN @ %o/%o\n", RPH, RPL);
  /* gvp->savetraceflags = traceflags = ~TB_MAP;    /*****/
  TRACE(T_FLOW, " SSSN\n");
#if 1
  ea = *(unsigned int *)(crs+XB);
  for (i=0; i<16; i++) {
    put16(0, ea+i);
  }
#else
  fault(UIIFAULT, RPL, RP);
#endif
}


/* C-Pointer conversion macros

   EACP: convert Prime effective address to C-Pointer value (the bit offset
   moves to the right bit so that byte math can be performed)

   CPEA: convert a C-Pointer back to an effective address.  The fault and
   ring bits from the ea argument are used (can pass in zero)
*/

#define EACP(v) (((v) & 0x0FFFFFFF)<<1) | (((v)>>28) & 1)
#define CPEA(ea,v) ((ea) & 0xE0000000) | (((v) & 1)<<28) | (((v)>>1) & 0x0FFFFFFF)

/* Character instructions */

#define GETFLR(n) (((crsl[FLR0+2*(n)] >> 11) & 0x1FFFE0) | (crsl[FLR0+2*(n)] & 0x1F))
#define PUTFLR(n,v) crsl[FLR0+2*(n)] = (((v) << 11) & 0xFFFF0000) | (crsl[FLR0+2*(n)] & 0xF000) | ((v) & 0x1F)

static inline unsigned short ldc(int n, unsigned short result) {
  unsigned int utempl;
  unsigned short m;
  unsigned int far, flr;
  ea_t ea;

  far = FAR0;
  flr = FLR0;
  if (n) {
    far = FAR1;
    flr = FLR1;
  }

  utempl = GETFLR(n);
  if (utempl > 0) {
    ea = crsl[far];
    m = get16(crsl[far]);
    if (crsl[flr] & 0x8000) {
      result = m & 0xFF;
      crsl[flr] &= 0xFFFF0FFF;
      crsl[far] = (crsl[far] & 0x6FFF0000) | ((crsl[far]+1) & 0xFFFF); \
      TRACE(T_INST, " ldc %d = '%o (%c) from %o/%o right\n", n, result, result&0x7f, ea>>16, ea&0xffff);
      //printf(" ldc %d = '%o (%c) from %o/%o right\n", n, result, result&0x7f, ea>>16, ea&0xffff);
    } else {
      result = m >> 8;
      crsl[flr] |= 0x8000;      /* set bit offset */
      TRACE(T_INST, " ldc %d = '%o (%c) from %o/%o left\n", n, result, result&0x7f, ea>>16, ea&0xffff);
      //printf(" ldc %d = '%o (%c) from %o/%o left\n", n, result, result&0x7f, ea>>16, ea&0xffff);
    }
    utempl--;
    PUTFLR(n,utempl);
    CLEAREQ;
  } else {                  /* utempl == 0 */
    TRACE(T_INST, " LDC %d limit\n", n);
    //printf(" LDC %d limit\n", n);
    SETEQ;
  }
  return result;
}


static inline stc(int n, unsigned short ch) {
  unsigned int utempl;
  unsigned short m;
  unsigned int far, flr;
  ea_t ea;

  far = FAR0;
  flr = FLR0;
  if (n) {
    far = FAR1;
    flr = FLR1;
  }

  utempl = GETFLR(n);
  if (utempl > 0) {
    ea = crsl[far];
    m = get16(crsl[far]);
    if (crsl[flr] & 0x8000) {
      TRACE(T_INST, " stc %d =  '%o (%c) to %o/%o right\n", n, ch, ch&0x7f, ea>>16, ea&0xffff);
      //printf(" stc %d =  '%o (%c) to %o/%o right\n", n, ch, ch&0x7f, ea>>16, ea&0xffff);
      m = (m & 0xFF00) | (ch & 0xFF);
      put16(m,crsl[far]);
      crsl[flr] &= 0xFFFF0FFF;
      crsl[far] = (crsl[far] & 0x6FFF0000) | ((crsl[far]+1) & 0xFFFF);
    } else {
      TRACE(T_INST, " stc %d = '%o (%c) to %o/%o left\n", n, ch, ch&0x7f, ea>>16, ea&0xffff);
      //printf(" stc %d = '%o (%c) to %o/%o left\n", n, ch, ch&0x7f, ea>>16, ea&0xffff);
      m = (ch << 8) | (m & 0xFF);
      put16(m,crsl[far]);
      crsl[flr] |= 0x8000;      /* set bit offset */
    }
    utempl--;
    PUTFLR(n,utempl);
    CLEAREQ;
  } else {                  /* utempl == 0 */
    TRACE(T_INST, " STC %d limit\n", n);
    //printf(" STC %d limit\n", n);
    SETEQ;
  }
}

/* add a bit offset, passed in "val", to field address register n */

static inline arfa(int n, int val) {
  int utempl;

  TRACE(T_INST, " before add, FAR=%o/%o, FLR=%o\n", crsl[FAR0+2*n]>>16, crsl[FAR0+2*n]&0xFFFF, crsl[FLR0+2*n]);
  utempl = ((crsl[FAR0+2*n] & 0xFFFF) << 4) | ((crsl[FLR0+2*n] >> 12) & 0xF);
  utempl += val;
  crsl[FAR0+2*n] = (crsl[FAR0+2*n] & 0xFFFF0000) | ((utempl >> 4) & 0xFFFF);
  crsl[FLR0+2*n] = (crsl[FLR0+2*n] & 0xFFFF0FFF) | ((utempl & 0xF) << 12);
  TRACE(T_INST, " after add, FAR0=%o/%o, FLR=%o\n", crsl[FAR0+2*n]>>16, crsl[FAR0+2*n]&0xFFFF, crsl[FLR0+2*n]);
}


/* 32-bit shifts */

static unsigned int lrs(unsigned int val, short scount) {

  CLEARCL;
  if (scount <= 32) {
    EXPCL(val & (((unsigned int)0x80000000) >> (32-scount)));
    return (*(int *)&val) >> scount;
  } else if (val & 0x80000000) {
    SETCL;
    return 0xFFFFFFFF;
  } else
    return 0;
}

static unsigned int lls(unsigned int val, short scount) {
  int templ;

  CLEARCL;
  if (scount < 32) {
    templ = 0x80000000;
    templ = templ >> scount;         /* create mask */
    templ = templ & val;             /* grab bits */
    templ = templ >> (31-scount);    /* extend them */
    EXPCL(!(templ == -1 || templ == 0));
    return *(int *)&val << scount;
  } else {
    EXPCL(val != 0);
    return 0;
  }
}

static unsigned int lll(unsigned int val, short scount) {

  CLEARCL;
  if (scount <= 32) {
    EXPCL(val & (((unsigned int)0x80000000) >> (scount-1)));
    return val << scount;
  } else
    return 0;
}

static unsigned int lrl(unsigned int val, short scount) {

  CLEARCL;
  if (scount <= 32) {
    EXPCL(val & (((unsigned int)0x80000000) >> (32-scount)));
    return val >> scount;
  } else
    return 0;
}

/* 16-bit shifts */

static unsigned short arl (unsigned short val, short scount) {

  CLEARCL;
  if (scount <= 16) {
    EXPCL(val & (((unsigned short)0x8000) >> (16-scount)));
    return val >> scount;
  } else {
    return 0;
  }
}

static unsigned short all (unsigned short val, short scount) {

  CLEARCL;
  if (scount <= 16) {
    EXPCL(val & (((unsigned short)0x8000) >> (scount-1)));
    return val << scount;
  } else {
    return 0;
  }
}

static unsigned short als (unsigned short val, short scount) {

  short tempa;

  CLEARCL;
  if (scount <= 15) {
    tempa = 0100000;
    tempa = tempa >> scount;         /* create mask */
    tempa = tempa & val;             /* grab bits */
    tempa = tempa >> (15-scount);    /* extend them */
    EXPCL(!(tempa == -1 || tempa == 0));
    return val << scount;
  }
  if (val != 0)
    SETCL;
  return 0;
}

static unsigned short ars (unsigned short val, short scount) {

  CLEARCL;
  if (scount <= 16) {
    EXPCL(val & (((unsigned short)0x8000) >> (16-scount)));
    return (*(short *)&val) >> scount;
  } else if (val & 0x8000) {
    SETCL;
    return 0xFFFF;
  } else
    return 0;
}

/* 32-bit rotates */

static unsigned int lrr(unsigned int val, short scount) {

  CLEARCL;
  scount = ((scount-1)%32)+1;         /* make scount 1-32 */
  EXPCL(val & (((unsigned int)0x80000000) >> (32-scount)));
  return (val >> scount) | (val << (32-scount));
}

static unsigned int llr(unsigned int val, short scount) {

  CLEARCL;
  scount = ((scount-1)%32)+1;         /* make scount 1-32 */
  EXPCL(val & (((unsigned int)0x80000000) >> (scount-1)));
  return (val << scount) | (val >> (32-scount));
}

/* 16-bit rotates */

static unsigned int alr(unsigned short val, short scount) {

  CLEARCL;
  scount = ((scount-1)%16)+1;         /* make scount 1-16 */
  EXPCL(val & (((unsigned short)0x8000) >> (scount-1)));
  return (val << scount) | (val >> (16-scount));
}

static unsigned int arr(unsigned short val, short scount) {

  CLEARCL;
  scount = ((scount-1)%16)+1;         /* make scount 1-16 */
  EXPCL(val & (((unsigned short)0x8000) >> (16-scount)));
  return (val >> scount) | (val << (16-scount));
}

/* math functions */

static tcr(unsigned int *un) {

  unsigned int utempl;

  utempl = - (*(int *)un);
  *un = utempl;
  SETCC_32(utempl);
  SETL(utempl == 0);
  if (utempl != 0x80000000) {
    CLEARC;
  } else {
    CLEARLT;
    mathexception('i', FC_INT_OFLOW, 0);
  }
}

static tch (unsigned short *un) {

  unsigned short utemp;

  utemp = - (*(short *)un);
  *un = utemp;
  SETCC_16(utemp);
  SETL(utemp == 0);
  if (utemp != 0x8000) {
    CLEARC;
  } else {
    CLEARLT;
    mathexception('i', FC_INT_OFLOW, 0);
  }
}

/* NOTE: ea is only used to set faddr should an arithmetic exception occur */

static int add32(unsigned int *a1, unsigned int a2, unsigned int a3, ea_t ea) {

  unsigned int uorig, uresult;
  unsigned long long utemp;
  short link, eq, lt;

  crs[KEYS] &= ~0120300;
  link = eq = lt = 0;
  uorig = *a1;                             /* save original for sign check */
  utemp = uorig;                           /* expand to higher precision */
  utemp += a2;                             /* double-precision add */
  utemp += a3;                             /* again, for subtract */
  uresult = utemp;                         /* truncate result to result size */
  *a1 = uresult;                           /* store result */
  if (utemp & 0x100000000LL)               /* set L-bit if carry occurred */
    link = 020000;  
  if (uresult == 0)                        /* set EQ? */
    eq = 0100; 
  if (((~uorig ^ a2) & (uorig ^ uresult) & 0x80000000) == 0) {
    if (*(int *)&uresult < 0)
      lt = 0200;
    crs[KEYS] = crs[KEYS] | link | eq | lt;
  } else {
    if (*(int *)&uresult >= 0)
      lt = 0200;
    crs[KEYS] = crs[KEYS] | link | eq | lt;
    mathexception('i', FC_INT_OFLOW, 0);
  }
}

static int add16(unsigned short *a1, unsigned short a2, unsigned short a3, ea_t ea) {

  unsigned short uorig, uresult;
  unsigned int utemp;
  short link, eq, lt;

  stopwatch_push(&sw_add16);
  crs[KEYS] &= ~0120300;
  link = eq = lt = 0;
  uorig = *a1;                             /* save original for sign check */
  utemp = uorig;                           /* expand to higher precision */
  utemp += a2;                             /* double-precision add */
  utemp += a3;                             /* again, for subtract */
  uresult = utemp;                         /* truncate result to result size */
  *a1 = uresult;                           /* store result */
  if (utemp & 0x10000)                     /* set L-bit if carry occurred */
    link = 020000;  
  if (uresult == 0)                        /* set EQ? */
    eq = 0100; 
  if (((~uorig ^ a2) & (uorig ^ uresult) & 0x8000) == 0) { /* no overflow */
    if (*(int *)&uresult < 0)
      lt = 0200;
    crs[KEYS] = crs[KEYS] | link | eq | lt;
  } else {
    if (*(int *)&uresult >= 0)
      lt = 0200;
    crs[KEYS] = crs[KEYS] | link | eq | lt;
    mathexception('i', FC_INT_OFLOW, ea);
  }
  stopwatch_pop(&sw_add16);
}

static adlr(int dr) {

  if (crs[KEYS] & 020000)
    add32(crsl+dr, 1, 0, 0);
  else {
    crs[KEYS] &= ~0120300;                 /* clear C, L, LT, EQ */
    SETCC_32(crsl[dr]);
  }
}

/* NOTE: PMA manuals say the range for absolute RF addressing is
   0-'377, but this does not allow addressing a machine with 8 user
   register sets.  The range should probably be an emulator config
   variable, based on the cpuid */

static int ldar(ea_t ea) {
  unsigned short utempa;
  unsigned int result;

  if (ea & 040000) {       /* absolute RF addressing */
    RESTRICT();

    if ((ea & 0777) > 0477) {
      printf("em: LDLR ea '%o is out of range for this CPU model\n", ea);
      fatal(NULL);
    }
    ea &= 0777;
    if (ea == 020)
      result = 1;
    else if (ea == 024)
      result = -1;
    else
      result = regs.u32[ea];
  } else {
    ea &= 037;
    if (ea > 017) RESTRICT();
    result = crsl[ea];
  }
  return result;
}


static star(unsigned int val32, ea_t ea) {

  if (ea & 040000) {       /* absolute RF addressing */
    RESTRICT();
    if ((ea & 0777) > 0477) {
      printf("em: STLR ea '%o is out of range for this cpu model.\nThis -cpuid may not be supported by this version of software\nTry a lower -cpuid", ea);
      fatal(NULL);
    }
    regs.u32[ea & 0777] = val32;
  } else {
    ea &= 037;
    if (ea > 017) RESTRICT();
    crsl[ea] = val32;
  }
}


/* here for PIO instructions: OCP, SKS, INA, OTA.  The instruction
   word is passed in as an argument to handle EIO (Execute I/O) in
   V/I modes. */

static pio(unsigned int inst) {
  int class;
  int func;
  int device;

  stopwatch_push(&sw_io);
  RESTRICT();
  class = inst >> 14;
  func = (inst >> 6) & 017;
  device = inst & 077;
  TRACE(T_INST, " pio, class=%d, func='%o, device='%o\n", class, func, device);
  devmap[device](class, func, device);
  stopwatch_pop(&sw_io);
}

main (int argc, char **argv) {

  static short bootdiskctrl[4] = {026, 027, 022, 023};

  /* the dispatch table for generic instructions:
     - bits 1-2 are the class (0-3)
     - bits 3-6 are always zero
     - bits 7-16 are the opcode
     the index into the table is bits 1-2, 7-16, for a 12-bit index */

  void *disp_gen[4096];                /* generic dispatch table */
  int boot;                            /* true if reading a boot record */
  char *bootarg;                       /* argument to -boot, if any */
  char bootfile[16];                   /* boot file to load (optional) */
  int bootfd=-1;                       /* initial boot file fd */
  int bootctrl, bootunit;              /* boot device controller and unit */
  int bootskip=0;                      /* skip this many bytes on boot dev */

  short tempa,tempa1,tempa2;
  unsigned short utempa,utempa1,utempa2;
  int templ,templ1,templ2;
  long long templl,templl1,templl2;
  unsigned long long utempll, utempll1, utempll2;
  unsigned int utempl,utempl1,utempl2,utempl3,utempl4;
  double tempd,tempd1,tempd2;
  ea_t tempea;
  ea_t ea;                             /* final MR effective address */
  ea_t earp;                           /* RP to use for eff address calcs */
  pa_t pa;
  int brop;
  int dr;
  unsigned short eabit;
  unsigned short opcode;
  short i,j,x;
  unsigned short savemask;
  unsigned short class;
  int nw,nw2;
  unsigned short rvec[9];    /* SA, EA, P, A, B, X, keys, dummy, dummy */
  unsigned short inst;
  unsigned short m,m2;
  unsigned short qtop,qbot,qseg,qmask,qtemp;
  ea_t qea;
  short scount;                          /* shift count */
  unsigned short trapvalue;
  ea_t trapaddr;
  unsigned short stpm[8];
  unsigned short access;
  unsigned long immu32;
  unsigned long long immu64;
  short fcode;
  unsigned short zresult, zclen1, zclen2, zaccess;
  unsigned int zlen1, zlen2;
  ea_t zea1, zea2;
  unsigned char zch1, zch2, *zcp1, *zcp2, zspace;
  unsigned char xsc, xfc, xsign, xsig;

  /* Prime ASCII constants for decimal instructions */

#define XPLUS 0253
#define XMINUS 0255
#define XZERO 0260
#define XONE 0261
#define XJ 0312
#define XRBRACE 0375

  struct timeval boot_tv;
  struct timezone tz;

  /* initialize global variables */

  gvp = &gv;
  gvp->traceflags = 0;
  gvp->savetraceflags = 0;
  gvp->traceuser = 0;
  gvp->traceseg = 0;
  gvp->numtraceprocs = 0;
  gvp->intvec = -1;
  gvp->instcount = 0;
  gvp->inhcount = 0;
  gvp->instpermsec = 2000;
  gvp->livereglim = 040;

  /* ignore SIGPIPE signals (sockets) or they'll kill the emulator */

  signal (SIGPIPE, SIG_IGN);

  /* open trace log */

  if ((gvp->tracefile=fopen("trace.log", "w")) == NULL) {
    perror("Unable to open trace.log");
    exit(1);
  }

  /* initialize dispatch tables */

#include "dispatch.h"

  /* master clear:
     - clear all registers
     - user register set is 0
     - modals:
     -- interrupts inhibited
     -- standard interrupt mode
     -- user register set is 0
     -- non-mapped I/O
     -- process exchange disabled
     -- segmentation disabled
     -- machine checks disabled
     - keys:
     -- C, L, LT, EQ clear
     -- single precision
     -- 16S mode
     -- take fault on FP exception
     -- no fault on integer or decimal exception
     -- characters have high bit on
     -- FP rounding disabled
     -- not in dispatcher
     -- register set is not saved
     - set P to '1000
     - all stlb entries are invalid
     - all iotlb entries are invalid
     - clear 64K words of memory
  */

  for (i=0; i < 32*REGSETS; i++)
    regs.u32[i] = 0;

  crsl = (void *)regs.sym.userregs[0]; /* first user register set */
  
  crs[MODALS] = 0;                    /* interrupts inhibited */
  newkeys(0);
  RP = 01000;
  for (i=0; i < STLBENTS; i++)
    gvp->stlb[i].valid = 0;
  for (i=0; i < IOTLBENTS; i++)
    gvp->iotlb[i].valid = 0;
  bzero(mem, 64*1024*2);              /* zero first 64K words */

  verbose = 0;
  domemdump = 0;
  boot = 0;
  bootarg = NULL;
  bootfile[0] = 0;
  gvp->pmap32bits = 0;
  gvp->pmap32mask = 0;
  gvp->csoffset = 0;
  tport = 0;
  nport = 0;
  memlimit = MEMSIZE;

  /* check args */

  for (i=1; i<argc; i++) {
    if (strcmp(argv[i],"-vv") == 0)
      verbose = 2;
    else if (strcmp(argv[i],"-v") == 0)
      verbose = 1;
    else if ((strcmp(argv[i],"-map") == 0) || (strcmp(argv[i],"-maps") == 0)) {
      while (i+1 < argc && argv[i+1][0] != '-')
	readloadmap(argv[++i]);
    } else if (strcmp(argv[i],"-memdump") == 0)
      domemdump = 1;
    else if (strcmp(argv[i],"-ss") == 0) {
      if (i+1 < argc && argv[i+1][0] != '-') {
	sscanf(argv[++i],"%o", &templ);
	sswitch = templ;
      } else
	sswitch = 0;
    } else if (strcmp(argv[i],"-cpuid") == 0) {
      if (i+1 < argc && argv[i+1][0] != '-') {
	sscanf(argv[++i],"%d", &templ);
	if (0 <= templ && templ <= 44)
	  cpuid = templ;
	else
	  fatal("-cpuid arg range is 0 to 44\n");
      } else
	fatal("-cpuid needs an argument\n");
#ifndef FAST
    } else if (strcmp(argv[i],"-mem") == 0) {
      if (i+1 < argc && argv[i+1][0] != '-') {
	sscanf(argv[++i],"%d", &templ);
	if (1 <= templ && templ < 1024)
	  memlimit = templ*1024*1024/2;
	else
	  fatal("-mem arg range is 1 to 1024 (megabytes)\n");
      } else
	fatal("-mem needs an argument\n");
#endif
    } else if (strcmp(argv[i],"-tport") == 0) {
      if (i+1 < argc && argv[i+1][0] != '-') {
	sscanf(argv[++i],"%d", &templ);
	tport = templ;
      } else
	fatal("-tport needs an argument\n");
    } else if (strcmp(argv[i],"-nport") == 0) {
      if (i+1 < argc && argv[i+1][0] != '-') {
	sscanf(argv[++i],"%d", &templ);
	nport = templ;
      } else
	fatal("-nport needs an argument\n");
    } else if (strcmp(argv[i],"-trace") == 0)
      while (i+1 < argc && argv[i+1][0] != '-') {
	i++;
	if (strcmp(argv[i],"ear") == 0)
	  gvp->traceflags |= TB_EAR;
	else if (strcmp(argv[i],"eav") == 0)
	  gvp->traceflags |= TB_EAV;
	else if (strcmp(argv[i],"eai") == 0)
	  gvp->traceflags |= TB_EAI;
	else if (strcmp(argv[i],"inst") == 0)
	  gvp->traceflags |= TB_INST;
	else if (strcmp(argv[i],"flow") == 0)
	  gvp->traceflags |= TB_FLOW;
	else if (strcmp(argv[i],"mode") == 0)
	  gvp->traceflags |= TB_MODE;
	else if (strcmp(argv[i],"eaap") == 0)
	  gvp->traceflags |= TB_EAAP;
	else if (strcmp(argv[i],"dio") == 0)
	  gvp->traceflags |= TB_DIO;
	else if (strcmp(argv[i],"map") == 0)
	  gvp->traceflags |= TB_MAP;
	else if (strcmp(argv[i],"pcl") == 0)
	  gvp->traceflags |= TB_PCL;
	else if (strcmp(argv[i],"fault") == 0)
	  gvp->traceflags |= TB_FAULT;
	else if (strcmp(argv[i],"px") == 0)
	  gvp->traceflags |= TB_PX;
	else if (strcmp(argv[i],"tio") == 0)
	  gvp->traceflags |= TB_TIO;
	else if (strcmp(argv[i],"term") == 0)
	  gvp->traceflags |= TB_TERM;
	else if (strcmp(argv[i],"rio") == 0)
	  gvp->traceflags |= TB_RIO;
	else if (strcmp(argv[i],"all") == 0)
	  gvp->traceflags = ~0;
	else if (isdigit(argv[i][0]) && strlen(argv[i]) < 2 && sscanf(argv[i],"%d", &templ) == 1)
	  gvp->traceuser = 0100000 | (templ<<6);   /* form OWNERL for user # */
	else if (strlen(argv[i]) == 6 && sscanf(argv[i],"%o", &templ) == 1)
	  gvp->traceuser = templ;                  /* specify OWNERL directly */
	else if (strlen(argv[i]) == 4 && sscanf(argv[i],"%o", &templ) == 1)
	  gvp->traceseg = templ;                   /* specify RPH segno */
	else if (strlen(argv[i]) <= 8 && argv[i][0] != '-') {
	  if (gvp->numtraceprocs >= MAXTRACEPROCS)
	    fprintf(stderr,"Only %d trace procs are allowed\n", MAXTRACEPROCS);
	  else {
	    printf("Request to trace proc %s\n", argv[i]);
	    traceprocs[gvp->numtraceprocs].oneshot = 1;
	    for (j=0; argv[i][j]; j++)
	      if (argv[i][j] == '+')
		traceprocs[gvp->numtraceprocs].oneshot = 0;
	      else
		traceprocs[gvp->numtraceprocs].name[j] = argv[i][j];
	    traceprocs[gvp->numtraceprocs].name[j] = 0;
	    traceprocs[gvp->numtraceprocs].sb = -1;
	    traceprocs[gvp->numtraceprocs].ecb = 0;
	    gvp->numtraceprocs++;
	  }
	} else {
	  fprintf(stderr,"Unrecognized trace flag: %s\n", argv[i]);
	  printf("Unrecognized trace flag: %s\n", argv[i]);
	}
      }
    else if (strcmp(argv[i],"-boot") == 0) {
      boot = 1;
      if (i+1 < argc && argv[i+1][0] != '-') {
	i++;
	if (strlen(argv[i]) <= 6 && sscanf(argv[i],"%o", &templ) == 1)
	  sswitch = templ;
	else
	  bootarg = argv[i];
      }

    } else {
      printf("Unrecognized argument: %s\n", argv[i]);
      fatal(NULL);
    }
  }

  /* finish setting up tracing after all options are read, ie, maps */

  if (gvp->traceuser != 0)
    TRACEA("Tracing enabled for OWNERL %o\n", gvp->traceuser);
  else
    TRACEA("Tracing enabled for all users\n");
  gvp->savetraceflags = gvp->traceflags;
  TRACEA("Trace flags = 04x%x\n", gvp->savetraceflags);
  for (i=0; i<gvp->numtraceprocs; i++) {
    for (j=0; j<numsyms; j++) {
      if (strcasecmp(mapsym[j].symname, traceprocs[i].name) == 0 && mapsym[j].symtype == 'e') {
	ea = mapsym[j].address;
	traceprocs[i].ecb = ea;
	TRACEA("Tracing procedure %s ecb ea '%o/%o\n", traceprocs[i].name, SEGNO32(ea), ea&0xFFFF);
	printf("Tracing procedure %s ecb ea '%o/%o\n", traceprocs[i].name, SEGNO32(ea), ea&0xFFFF);
	break;
      }
    }
    if (j == numsyms) {
      fprintf(stderr,"Can't find procedure %s in load maps for tracing.\n", traceprocs[i].name);
      printf("Can't find procedure %s in load maps for tracing.\n", traceprocs[i].name);
    }
  }
    
  /* set some vars after the options have been read */

  if (cpuid == 15 || cpuid == 18 || cpuid == 19 || cpuid == 24 || cpuid >= 26)
    gvp->pmap32bits = 1;
  if (cpuid == 33 || cpuid == 37 || cpuid == 39 || cpuid >= 43)
    gvp->pmap32mask = 0x3;
  if ((26 <= cpuid && cpuid <= 29) || cpuid >= 35)
    gvp->csoffset = 1;

  /* initialize all devices */

  for (i=0; i<64; i++)
    if (devmap[i](-1, 0, i)) {   /* if initialization fails, */
      devmap[i] = devnone;       /* remove device */
      fprintf(stderr, "emulator: device '%o failed initialization - device removed\n", i);
    }


  /* if a filename follows -boot, load and execute this R-mode runfile
     image.  For example, -boot *DOS64 would load *DOS64 from the Unix
     file system and begin executing Primos II.

     SECURITY: check that boot filename isn't a pathname?
  */

  if (bootarg) {
    if ((bootfd=open(bootarg, O_RDONLY)) == -1) {
      perror("Error opening boot file");
      fatal(NULL);
    }
    if (read(bootfd, rvec, 18) != 18) {
      perror("Error reading boot file's rvec header");
      fatal(NULL);
    }

  } else {

    /* If no filename follows -boot, then the sense switches are used to
       determine whether the boot record should be read from tape or disk
       and select the controller and drive unit.

       Bits 14-16 are 4 for disk boot, 5 for tape boot
       Bit 13 is 1 for disk boot, don't care for tape boot
       Bits 11-12 are the unit number, 0-4
    */

    bootunit = (sswitch>>7) & 3;
    rvec[2] = 01000;                  /* starting address */
    rvec[3] = rvec[4] = rvec[5] = rvec[6] = 0;

    if ((sswitch & 0x7) == 4) {         /* disk boot */
      bootctrl = bootdiskctrl[(sswitch>>4) & 3];
      rvec[0] = 0760;                   /* disk load starts at '760 */
      rvec[1] = rvec[0]+1040-1;         /* read 1 disk block */
      /* setup DMA register '20 (address only) for the next boot record */
      regs.sym.regdmx[041] = 03000;
      if (globdisk(bootfile, sizeof(bootfile), bootctrl, bootunit) != 0)
	fatal("Can't find disk boot device file");

    } else if ((sswitch & 0x7) == 5) {  /* tape boot */
      bootctrl = 014;
      rvec[0] = 0200;                   /* tape load starts at '200 */
      rvec[1] = rvec[0]+2355-1;         /* read in at most 3 pages (6K) */
      bootskip = 4;                     /* to skip .TAP header */
      /* setup DMA register '20 (address only) for the next boot record */
      regs.sym.regdmx[041] = 0200+2355;;
      snprintf(bootfile, sizeof(bootfile), "dev%ou%d", bootctrl, bootunit);

    } else {
      printf("\
\n\
The -boot option is used to boot from disk, tape, or to load a Prime\n\
runfile directly from the Unix file system.  For example:\n\
\n\
  -boot 14xx4 to boot from disk (see below)\n\
  -boot 10005 to boot from tape.\n\
  -boot *DOS64 to load *DOS64 from the Unix file and execute it\n\
\n\
For disk boots, the last 3 digits can be:\n\
\n\
  114 = dev26u0 ctrl '26 unit 0       154 = dev22u0 ctrl '22 unit 0\n\
  314 = dev26u1 ctrl '26 unit 1       354 = dev22u1 ctrl '22 unit 1\n\
  514 = dev26u2 ctrl '26 unit 2       554 = dev22u2 ctrl '22 unit 2\n\
  714 = dev26u3 ctrl '26 unit 3       754 = dev22u3 ctrl '22 unit 3\n\
\n\
  134 = dev27u0 ctrl '27 unit 0       174 = dev23u0 ctrl '23 unit 0\n\
  334 = dev27u1 ctrl '27 unit 1       374 = dev23u1 ctrl '23 unit 1\n\
  534 = dev27u2 ctrl '27 unit 2       574 = dev23u2 ctrl '23 unit 2\n\
  734 = dev27u3 ctrl '27 unit 3       774 = dev23u3 ctrl '23 unit 3\n\
\n\
  The default option is -boot 14114, to boot from disk dev26u0\n");
      exit(1);
    }

    TRACEA("Boot file is %s\n", bootfile);
    if ((bootfd=open(bootfile, O_RDONLY)) == -1) {
      perror("Error opening boot device file");
      fatal(NULL);
    }
    if (lseek(bootfd, bootskip, SEEK_CUR) == -1) {
      perror("Error skipping on boot device");
      fatal(NULL);
    }
  }
  TRACEA("Sense switches set to %o\n", sswitch);
  TRACE(T_FLOW, "Boot SA=%o, EA=%o, P=%o, A=%o, B=%o, X=%o, K=%o\n\n", rvec[0], rvec[1], rvec[2], rvec[3], rvec[4], rvec[5], rvec[6]);
  if (rvec[2] > rvec[1])
    fatal("Program start > ending: boot image is corrupt");

  /* read memory image from SA to EA inclusive */

  nw = rvec[1]-rvec[0]+1;
  if ((nw2=read(bootfd, mem+rvec[0], nw*2)) == -1) {
    perror("Error reading memory image");
    fatal(NULL);
  }
  close(bootfd);

  /* check we got it all, except for tape boots; the boot program size
     is unpredictable on tape */
  
  if (nw2 != nw*2 && ((sswitch & 0x7) == 4 || bootarg)) {
    printf("rvec[0]=%d, rvec[1]=%d, nw2=%d, nw=%d, nw*2=%d\n", rvec[0], rvec[1], nw2, nw, nw*2);
    fatal("Didn't read entire boot program");
  }

  /* setup execution (registers, keys, address mask, etc.) from rvec */

  crs[A] = rvec[3];
  crs[B] = rvec[4];
  crs[X] = rvec[5];
  newkeys(rvec[6]);
  RPL = rvec[2];

  memdump(rvec[0], rvec[1]);

  /* initialize the timer stuff */

  if (gettimeofday(&boot_tv, &tz) != 0) {
    perror("gettimeofday failed");
    fatal(NULL);
  }

  stopwatch_init(&sw_all, "All");
  stopwatch_init(&sw_ea, "ea");
  stopwatch_init(&sw_mapva, "mapva");
  stopwatch_init(&sw_add16, "add16");
  stopwatch_init(&sw_cas, "cas");
  stopwatch_init(&sw_irs, "irs");
  stopwatch_init(&sw_zmv, "zmv");
  stopwatch_init(&sw_zfil, "zfil");
  stopwatch_init(&sw_zmvd, "zmvd");
  stopwatch_init(&sw_io, "io");
  stopwatch_init(&sw_pcl, "pcl");
  stopwatch_init(&sw_fault, "fault");   sw_fault.m_bClear = 1;
  stopwatch_init(&sw_idle, "idle");

  stopwatch_start(&sw_all);

  /* main instruction decode loop */

  grp = RP;             /* see similar assignments in fault, before longjmp */
  gcrsl = crsl;

  /* faults longjmp back here: the top of the instruction fetch loop.
     Stop the fault timer and restore dedicated registers trashed by longjmp */

  if (setjmp(jmpbuf)) {              /* returns 1 on longjmp */
    gvp = &gv;
    crsl = gcrsl;
    RP = grp;
    stopwatch_stop(&sw_fault);
  }


fetch:

#if 0
  if (gvp->instcount > 32727500)
    gvp->savetraceflags = ~0;
#endif

#if 0
  /* NOTE: doing something like this causes Primos to do a controlled
	 shutdown, flushing disk buffers, etc. */
  RPH = 07777;
#endif

#if 0
    /* trace AC$SET call not working

     NOTE: a 2-word range is needed for RPL because a procedure with
     arguments may start executing at the ARGT instruction (listed
     in the load map as procedure start), or at the instruction
     following ARGT (if PCL completes w/o faults) */

  if (TRACEUSER && SEGNO16(RPH) == 041 && 06200 <= RPL && RPL <= 06201) { /* ac$set */
    gvp->savetraceflags = ~TB_MAP;
    printf("enable trace, RPH=%o, RPL=%o\n", SEGNO16(RPH), RPL);
  }
  if (TRACEUSER && SEGNO16(RPH) == 013 && 044030 <= RPL && RPL <= 044031) { /* setrc$ */
    gvp->savetraceflags = 0;
    printf("disable trace, RPH=%o, RPL=%o\n", SEGNO16(RPH), RPL);
  }
#endif

#if 0
  /* this is for FTN Generic 3 trace */
  if (SEGNO16(RPH) == 04000 && RPL >= 034750 && RPL <= 034760)
    gvp->savetraceflags = ~TB_MAP;
  else
    gvp->savetraceflags = 0;
#endif

#if 0
  /* NOTE: this tends to cause a page fault loop if the location
     being monitored isn't wired */

  if (trapaddr != 0 && (crs[OWNERL] & 0100000) && (crs[MODALS] & 010)) {
    gvp->traceflags = -1;
    printf("TRAP: at #%d\n", gvp->instcount);
    utempa = get16(trapaddr);
    if (utempa != trapvalue) {
      printf("TRAP: at #%d, old value of %o/%o was %o; new value is %o\n", gvp->instcount, trapaddr>>16, trapaddr&0xffff, trapvalue, utempa);
      trapvalue = utempa;
      printf("TRAP: new trap value is %o\n", trapvalue);
    }
  }
#endif

#ifndef NOTRACE
  /* is this user being traced? */

  if (TRACEUSER && ((gvp->traceseg == 0) || (gvp->traceseg == (RPH & 0xFFF))))
    gvp->traceflags = gvp->savetraceflags;
  else
    gvp->traceflags = 0;
#endif

  /* hack to activate trace in 32I mode */

#if 0
  if ((crs[KEYS] & 0016000) == 0010000)
    gvp->traceflags = gvp->savetraceflags;
  else
    gvp->traceflags = 0;
#endif

#if 0
  /* NOTE: rev 23.4 halts at inst #75379065 with the error:
     "System Serial Number does not agree with this version of Primos."
     To track this down, turn on tracing just before this instruction. */

  if (75370000 < gvp->instcount && gvp->instcount < 75380000)
    gvp->traceflags = ~TB_MAP;
#endif

#if 0
  /* turn on tracing  near instruction #47704931 to debug I/O TLB error
     in rev 22.1 */

  if (gvp->instcount > 47700000)
    gvp->traceflags = ~0;
#endif

  /* poll any devices that requested a poll */

  if ((gvp->instcount & 0777) == 0) {
    stopwatch_push(&sw_io);
    for (i=0; i<64; i++)
      if (devpoll[i] && ((devpoll[i] -= 0777) <= 0)) {
	devpoll[i] = 0;
	devmap[i](4, 0, i);
      }
    stopwatch_pop(&sw_io);
  }

  /* is an interrupt pending, with interrupts enabled? */

  if (gvp->inhcount)
    gvp->inhcount--;
  else if (gvp->intvec >= 0 && (crs[MODALS] & 0100000) /* && gvp->inhcount == 0 */) {
    //printf("fetch: taking interrupt vector '%o, modals='%o\n", gvp->intvec, crs[MODALS]);
    TRACE(T_INST, "\nfetch: taking interrupt vector '%o, modals='%o\n", gvp->intvec, crs[MODALS]);
    regs.sym.pswpb = RP;
    regs.sym.pswkeys = crs[KEYS];

    if (crs[MODALS] & 010) {              /* PX enabled */
      //gvp->traceflags = ~TB_MAP;
      newkeys(014000);
      RPH = 4;
      RPL = gvp->intvec;

    } else if (crs[MODALS] & 040000) {    /* vectored interrupt mode */
      m = get16(gvp->intvec);
      if (m != 0) {
	put16(RPL, m);
	RP = m+1;
      } else {
	printf("fetch: interrupt vector '%o = 0 in vectored interrupt mode\n", gvp->intvec);
	fatal(NULL);
      }

    } else {                              /* standard interrupt mode */
      m = get16(063);
      printf("Standard mode interrupt vector loc = %o\n", m);
      //gvp->traceflags = ~TB_MAP;
      if (m != 0) {
	put16(RPL, m);
	RP = m+1;
      } else {
	fatal("fetch: loc '63 = 0 in standard interrupt mode");
      }
    }
    crs[MODALS] &= 077777;   /* inhibit interrupts */
  }

  gvp->prevpc = RP;
#if 0
  /* NOTE: Rev 21 Sys Arch Guide, 2nd Ed, pg 3-32 says:

     "When bits 17 to 32 of the program counter contain a value within
     the ATR (address trap range) and the processor is reading an 
     instruction, an address trap always occurs.  The only exception
     to this is if the machine is operating in 32I mode."

     However, if this code is enabled, the Primos boot fails very
     early, before verifying memory.

     NOTE 8/21/07: I think the problem here is that the test should
     be:

     if !i-mode
       if segmented and ealow < 010 or !segmented and ealow < 040
         set ea to trap
  */

  ea = RP;
  if ((ea & 0xFFFF) < 010)
    ea = 0x80000000 | (ea & 0xFFFF);
#endif

  /* the Prime allows executing instructions from register locations in
     R-mode (BASIC TRACE ON does this), so iget16t is needed here */

  inst = iget16t(RP);
  INCRP;
  gvp->instcount++;

  /* while a process is running, RP is the real program counter, PBH
     is the active procedure segment, and PBL is zero.  When a
     process stops running, RP is copied to PB.  When a process
     starts running again, PB is copied to RP. */

#ifdef FAST
  *(int *)(crs+PBH) = RP & 0xFFFF0000;
#else
  crs[PBH] = RPH;
  crs[PBL] = 0;
#endif
  earp = RP;

  if (crs[MODALS] & 010) {     /* px enabled, bump 1ms process timer */
    if (crs[TIMERL]++ > gvp->instpermsec) {
      crs[TIMERL] = 0;

      /* if 1ms resolution process timer overflows, set pcb abort flag */

      crs[TIMER]++;
      if (crs[TIMER] == 0) {
	TRACE(T_PX,  "#%d: pcb %o timer overflow\n", gvp->instcount, crs[OWNERL]);
	ea = *(ea_t *)(crs+OWNER);
	m = get16r0(ea+4) | 1;       /* set process abort flag */
	put16r0(m, ea+4);
      }
    }
  }

xec:
  /* NOTE: don't trace JMP * instructions (used to test PX) */

#if 0
  if (inst == 03777)
    gvp->traceflags = 0;
#endif

#if 0
  if (crs[OWNERL] == 0100200 && inst == 001114 && gvp->savetraceflags)
    gvp->traceflags = ~0;
  else
    gvp->traceflags = 0;
#endif

  TRACE(T_FLOW, "\n			#%u [%s %o] IT=%d SB: %o/%o LB: %o/%o %s XB: %o/%o\n%o/%o: %o		A='%o/%:0d B='%o/%d L='%o/%d E='%o/%d X=%o/%d Y=%o/%d C=%d L=%d LT=%d EQ=%d K=%o M=%o\n", gvp->instcount, searchloadmap(*(unsigned int *)(crs+OWNER),'x'), crs[OWNERL], *(short *)(crs+TIMER), crs[SBH], crs[SBL], crs[LBH], crs[LBL], searchloadmap(*(unsigned int *)(crs+LBH),'l'), crs[XBH], crs[XBL], RPH, RPL-1, inst, crs[A], *(short *)(crs+A), crs[B], *(short *)(crs+B), *(unsigned int *)(crs+L), *(int *)(crs+L), *(unsigned int *)(crs+E), *(int *)(crs+E), crs[X], *(short *)(crs+X), crs[Y], *(short *)(crs+Y), (crs[KEYS]&0100000) != 0, (crs[KEYS]&020000) != 0, (crs[KEYS]&0200) != 0, (crs[KEYS]&0100) != 0, crs[KEYS], crs[MODALS]);

  /* begin instruction decode: generic? */

  if ((inst & 036000) != 0)
    if ((crs[KEYS] & 0016000) == 0010000)
      goto imode;
    else
      goto nonimode;

  TRACE(T_INST, " generic class %d\n", inst>>14);
  goto *disp_gen[GENIX(inst)];
    
  /* V-mode/frequent instructions */

d_iab:  /* 000201 */
  TRACE(T_FLOW, " IAB\n");
  utempa = crs[B];
  crs[B] = crs[A];
  crs[A] = utempa;
  goto fetch;

d_cgt:  /* 001314 */
  TRACE(T_FLOW, " CGT\n");
  utempa = iget16(RP);              /* get number of words */
  if (1 <= crs[A] && crs[A] < utempa)
    RPL = iget16(RPADD(crs[A]));
  else
    RP = RPADD(utempa);
  goto fetch;

d_pida:  /* 000115 */
  TRACE(T_FLOW, " PIDA\n");
  *(int *)(crs+L) = *(short *)(crs+A);
  goto fetch;

d_pidl:  /* 000305 */
  TRACE(T_FLOW, " PIDL\n");
  *(long long *)(crs+L) = *(int *)(crs+L);
  goto fetch;

  /* NOTE: PMA manual says copy B reg to A reg, but DIAG seems
     to indicate a swap */

d_pima: /* 000015 */
  TRACE(T_FLOW, " PIMA\n");
  templ = *(int *)(crsl+GR2);
  crsl[GR2] = (crsl[GR2] << 16) | (crsl[GR2] >> 16);
  templ2 = (templ << 16) >> 16;
  if (templ != templ2)
    mathexception('i', FC_INT_OFLOW, 0);
  else
    CLEARC;
  goto fetch;

d_piml:  /* 000301 */
  TRACE(T_FLOW, " PIML\n");
  templ = *(int *)(crs+L);
  *(int *)(crs+L) = *(int *)(crs+E);
  if (((templ ^ *(int *)(crs+E)) & 0x80000000) || (templ != 0 && templ != -1))
    mathexception('i', FC_INT_OFLOW, 0);
  else
    CLEARC;
  goto fetch;

  /* character/field instructions */

d_ldc0:  /* 001302 */
  TRACE(T_FLOW, " LDC 0\n");
  crs[A] = ldc(0, crs[A]);
  goto fetch;

d_ldc1:  /* 001312 */
  TRACE(T_FLOW, " LDC 1\n");
  crs[A] = ldc(1, crs[A]);
  goto fetch;

d_stc0: /* 001322 */
  TRACE(T_FLOW, " STC 0\n");
  stc(0, crs[A]);
  goto fetch;

d_stc1:  /* 001332 */
  TRACE(T_FLOW, " STC 1\n");
  stc(1, crs[A]);
  goto fetch;

d_eafa0:  /* 001300 */
  TRACE(T_FLOW, " EAFA 0\n");
  ea = apea(&eabit);
  crsl[FAR0] = ea & 0x6FFFFFFF;
  crsl[FLR0] = (crsl[FLR0] & 0xFFFF0FFF) | (eabit << 12);
  TRACE(T_INST, " FAR0=%o/%o, eabit=%d, FLR=%x\n", crsl[FAR0]>>16, crsl[FAR0]&0xFFFF, eabit, crsl[FLR0]);
  goto fetch;

d_eafa1:  /* 001310 */
  TRACE(T_FLOW, " EAFA 1\n");
  ea = apea(&eabit);
  crsl[FAR1] = ea & 0x6FFFFFFF;
  crsl[FLR1] = (crsl[FLR1] & 0xFFFF0FFF) | (eabit << 12);
  TRACE(T_INST, " FAR1=%o/%o, eabit=%d, FLR=%x\n", crsl[FAR1]>>16, crsl[FAR1]&0xFFFF, eabit, crsl[FLR1]);
  goto fetch;

d_alfa0:  /* 001301 */
  TRACE(T_FLOW, " ALFA 0\n");
  arfa(0, *(int *)(crs+L));
  goto fetch;

d_alfa1:  /* 001311 */
  TRACE(T_FLOW, " ALFA 1\n");
  arfa(1, *(int *)(crs+L));
  goto fetch;

d_lfli0:  /* 001303 */
  TRACE(T_FLOW, " LFLI 0\n");
  utempa = iget16(RP);
  PUTFLR(0,utempa);
  RP++;
  TRACE(T_INST, " Load Field length with %d, FLR=%x, actual = %d\n", utempa, crsl[FLR0], GETFLR(0));
  goto fetch;

d_lfli1:  /* 001313 */
  TRACE(T_FLOW, " LFLI 1\n");
  utempa = iget16(RP);
  INCRP;
  PUTFLR(1,utempa);
  TRACE(T_INST, " Load Field length with %d, FLR=%x, actual = %d\n", utempa, crsl[FLR1], GETFLR(1));
  goto fetch;

d_stfa0:  /* 001320 */
  TRACE(T_FLOW, " STFA 0\n");
  ea = apea(NULL);
  utempl = crsl[FAR0] & 0x6FFFFFFF;
  utempa = crsl[FLR0] & 0xF000;
stfa:
  if (utempa != 0) {
    utempl = utempl | EXTMASK32;
    put16(utempa,INCVA(ea,2));
    TRACE(T_INST, " stored 3-word pointer %o/%o %o\n", utempl>>16, utempl&0xffff, utempa);
  } else {
    TRACE(T_INST, " stored 2-word pointer %o/%o\n", utempl>>16, utempl&0xffff);
  }
  put32(utempl,ea);
  goto fetch;

d_stfa1:  /* 001330 */
  TRACE(T_FLOW, " STFA 1\n");
  ea = apea(NULL);
  utempl = crsl[FAR1] & 0x6FFFFFFF;
  utempa = crsl[FLR1] & 0xF000;
  goto stfa;

d_tlfl0:  /* 001321 */
  TRACE(T_FLOW, " TLFL 0\n");
  PUTFLR(0,*(unsigned int *)(crs+L));
  TRACE(T_INST, " Transfer %d to FLR0, FLR=%x, actual = %d\n", *(unsigned int *)(crs+L), crsl[FLR0], GETFLR(0));
  goto fetch;

d_tlfl1:  /* 001331 */
  TRACE(T_FLOW, " TLFL 1\n");
  PUTFLR(1,*(unsigned int *)(crs+L));
  TRACE(T_INST, " Transfer %d to FLR1, FLR=%x, actual = %d\n", *(unsigned int *)(crs+L), crsl[FLR1], GETFLR(1));
  goto fetch;

d_tfll0:  /* 001323 */
  TRACE(T_FLOW, " TFLL 0\n");
  *(unsigned int *)(crs+L) = GETFLR(0);
  goto fetch;

d_tfll1:  /* 001333 */
  TRACE(T_FLOW, " TFLL 1\n");
  *(unsigned int *)(crs+L) = GETFLR(1);
  goto fetch;

d_prtn:  /* 000611 */
  TRACE(T_FLOW, " PRTN\n");
  stopwatch_push(&sw_pcl);
  prtn();

  /* if this PRTN is for a procedure being traced, disable
     tracing if one-shot is true */

  if (gvp->numtraceprocs > 0 && TRACEUSER)
    for (i=0; i<gvp->numtraceprocs; i++)
      if (*(int *)(crs+SB) == traceprocs[i].sb) {
	traceprocs[i].sb = -1;
	fflush(gvp->tracefile);
	if (traceprocs[i].oneshot) {
	  printf("Disabled trace for %s at sb '%o/%o\n", traceprocs[i].name, crs[SBH], crs[SBL]);
	  gvp->savetraceflags = 0;
	}
	break;
      }
  stopwatch_pop(&sw_pcl);
  goto fetch;

d_tka:  /* 001005 */
  TRACE(T_FLOW, " TKA\n");
  crs[A] = crs[KEYS];
  goto fetch;

d_tak:  /* 001015 */
  TRACE(T_FLOW, " TAK\n");
  newkeys(crs[A] & 0177770);
  goto fetch;

d_nop:  /* 000001 */
  TRACE(T_FLOW, " NOP 1\n");
  goto fetch;

d_rsav:  /* 000715 */
  TRACE(T_FLOW, " RSAV\n");
  ea = apea(NULL);
  j = 1;
  savemask = 0;
  for (i = 11; i >= 0; i--) {
    if (crsl[i] != 0) {
      TRACE(T_INST, " crsl[%d] saved, value=%o (%o/%o)\n", i, crsl[i], crsl[i]>>16, crsl[i]&0xffff);
      put32(crsl[i], INCVA(ea,j));
      savemask |= BITMASK16(16-i);
    }
    j += 2;
  }
  put32(*(int *)(crs+XB), INCVA(ea,25));
  TRACE(T_INST, " XB saved, value=%o/%o\n", crs[XBH], crs[XBL]);
  put16(savemask, ea);
  TRACE(T_INST, " Saved, mask=%o\n", savemask);
  goto fetch;

d_rrst:  /* 000717 */
  TRACE(T_FLOW, " RRST\n");
  ea = apea(NULL);
  savemask = get16(ea);
  TRACE(T_INST, " Save mask=%o\n", savemask);
  j = 1;
  for (i = 11; i >= 0; i--) {
    if (savemask & BITMASK16(16-i)) {
      crsl[i] = get32(INCVA(ea,j));
      TRACE(T_INST, " crsl[%d] restored, value=%o (%o/%o)\n", i, crsl[i], crsl[i]>>16, crsl[i]&0xffff);
    } else {
      crsl[i] = 0;
    }
    j += 2;
  }
  *(unsigned int *)(crs+XB) = get32(INCVA(ea,25));
  TRACE(T_INST, " XB restored, value=%o/%o\n", crs[XBH], crs[XBL]);
  goto fetch;

d_enb:  /* 000400 (enbm), 000401 (enbl), 000402 (enbp) */
  TRACE(T_FLOW, " ENB\n");
  RESTRICT();
  crs[MODALS] |= 0100000;
  gvp->inhcount = 1;
  goto fetch;

d_inh:  /* 001000 (inhm), 001001 (inhl), 001002 (inhp) */
  TRACE(T_FLOW, " INH\n");
  RESTRICT();
  crs[MODALS] &= ~0100000;
  goto fetch;

d_stac:  /* 001200 */
  TRACE(T_FLOW, " STAC\n");
  ea = apea(NULL);
  if (get16(ea) == crs[B]) {
    put16(crs[A], ea);
    SETEQ;
  } else 
    CLEAREQ;
  goto fetch;

d_stlc:  /* 001204 */
  TRACE(T_FLOW, " STLC\n");
  ea = apea(NULL);
  if (get32(ea) == *(unsigned int *)(crs+E)){
    put32(*(unsigned int *)(crs+L), ea);
    SETEQ;
  } else 
    CLEAREQ;
  goto fetch;

  /* NOTE: when ARGT is executed as an instruction, it means
     that a fault occurred during PCL argument processing. */

d_argt:  /* 000605 */
  TRACE(T_FLOW|T_PCL, " ARGT\n");
  stopwatch_push(&sw_pcl);
  argt();
  stopwatch_pop(&sw_pcl);
  goto fetch;

d_calf:  /* 000705 */
  TRACE(T_FLOW|T_PCL, " CALF\n");
  ea = apea(NULL);
  calf(ea);
  goto fetch;

  /* Decimal and character instructions

    IMPORTANT NOTE: when using the Z macros, be sure to use curly
    braces, ie,

	 Instead of:

	   if (cond)
	     ZPUTC ...

	 use:

	   if (cond) {
	     ZPUTC ...
	   }
  */

#define ZSTEP(zea, zlen, zcp, zclen, zacc) \
  zcp = (unsigned char *) (mem+mapva(zea, RP, zacc, &zaccess)); \
  zclen = 2048 - (zea & 01777)*2; \
  if (zea & EXTMASK32) { \
    zcp++; \
    zclen--; \
  } \
  if (zclen >= zlen) \
    zclen = zlen; \
  else \
    zea = (zea & 0xEFFF0000) | ((zea+0x400) & 0xFC00) \

#define ZGETC(zea, zlen, zcp, zclen, zch) \
  if (zclen == 0) { \
    ZSTEP(zea, zlen, zcp, zclen, RACC); \
  } \
  zch = *zcp; \
  zcp++; \
  zclen--; \
  zlen--

#define ZPUTC(zea, zlen, zcp, zclen, zch) \
  if (zclen == 0) { \
    ZSTEP(zea, zlen, zcp, zclen, WACC); \
  } \
  *zcp = (zch); \
  zcp++; \
  zclen--; \
  zlen--

d_zmv:  /* 001114 */
  //gvp->traceflags = -1; /***/
  stopwatch_push(&sw_zmv);
  TRACE(T_FLOW, " ZMV\n");
  zspace = 0240;
  if (crs[KEYS] & 020)
    zspace = 040;
  TRACE(T_INST, "ZMV: source=%o/%o, len=%d, dest=%o/%o, len=%d, keys=%o\n", crsl[FAR0]>>16, crsl[FAR0]&0xffff, GETFLR(0), crsl[FAR1]>>16, crsl[FAR1]&0xffff, GETFLR(1), crs[KEYS]);

  zlen1 = GETFLR(0);
  zlen2 = GETFLR(1);
  zea1 = crsl[FAR0];
  if (crsl[FLR0] & 0x8000)
    zea1 |= EXTMASK32;
  zea2 = crsl[FAR1];
  if (crsl[FLR1] & 0x8000)
    zea2 |= EXTMASK32;
  TRACE(T_INST, "     ea1=%o/%o, len1=%d, ea2=%o/%o, len2=%d\n", zea1>>16, zea1&0xffff, zlen1, zea2>>16, zea2&0xffff, zlen2);
  zclen1 = 0;
  zclen2 = 0;
  while (zlen2) {
    if (zlen1) {
      ZGETC(zea1, zlen1, zcp1, zclen1, zch1);
    } else
      zch1 = zspace;
    TRACE(T_INST, " zch1=%o (%c)\n", zch1, zch1&0x7f);
    ZPUTC(zea2, zlen2, zcp2, zclen2, zch1);
  }
  gvp->traceflags = 0;
  stopwatch_pop(&sw_zmv);
  goto fetch;

d_zmvd:  /* 001115 */
  stopwatch_push(&sw_zmvd);
  //gvp->traceflags = -1; /***/
  TRACE(T_FLOW, " ZMVD\n");
  zlen1 = GETFLR(1);
  zlen2 = zlen1;
  zea1 = crsl[FAR0];
  if (crsl[FLR0] & 0x8000)
    zea1 |= EXTMASK32;
  zea2 = crsl[FAR1];
  if (crsl[FLR1] & 0x8000)
    zea2 |= EXTMASK32;
  TRACE(T_INST, " ea1=%o/%o, ea2=%o/%o, len=%d\n", zea1>>16, zea1&0xffff, zea2>>16, zea2&0xffff, zlen1);
  zclen1 = 0;
  zclen2 = 0;
  while (zlen2) {
#if 0
    /* this code doesn't run much faster than the 3-line ZGETC/PUTC loop,
       and sometimes slower.  Suspect there are cache differences. */

    if (zclen1 == 0) {
      ZSTEP(zea1, zlen1, zcp1, zclen1, RACC);
    }
    if (zclen2 == 0) {
      ZSTEP(zea2, zlen2, zcp2, zclen2, WACC);
    }
    if (zclen1 < zclen2)
      utempa = zclen1;
    else
      utempa = zclen2;
    zclen1 -= utempa;
    zclen2 -= utempa;
    zlen1 -= utempa;
    zlen2 -= utempa;
#if 1
    /* this works */
    while (utempa--)
      *zcp2++ = *zcp1++;
#else
    /* this causes error: 
          Coldstarting PRIMOS, Please wait...
	  Unable to initialize gate segment.  (GATE_INIT) */
    memcpy(zcp2, zcp1, utempa);
#endif
#else
    ZGETC(zea1, zlen1, zcp1, zclen1, zch1);
    TRACE(T_INST, " zch1=%o (%c)\n", zch1, zch1&0x7f);
    ZPUTC(zea2, zlen2, zcp2, zclen2, zch1);
#endif
  }
  stopwatch_pop(&sw_zmvd);
  gvp->traceflags = 0;
  goto fetch;

  /* NOTE: ZFIL is used early after PX enabled, and can be used to cause
     a UII fault to debug CALF etc.

     I tried using memset, but it was much slower than a manual loop.

     It may be worthwhile to special case a 2048-byte fill that is
     page-aligned, since ZFIL is often used this way by Primos, but
     it isn't a significant overall performance issue. */

d_zfil:  /* 001116 */
  stopwatch_push(&sw_zfil);
  TRACE(T_FLOW, " ZFIL\n");
  zlen2 = GETFLR(1);
  zea2 = crsl[FAR1];
  if (crsl[FLR1] & 0x8000)
    zea2 |= EXTMASK32;
  zch2 = crs[A];
  TRACE(T_INST, " ea=%o/%o, len=%d, fill=%o (%c)\n", zea2>>16, zea2&0xffff, zlen2, zch2, zch2&0x7f);
  zclen2 = 0;
  while (zlen2) {
#if 1
    ZSTEP(zea2, zlen2, zcp2, zclen2, WACC);
    zlen2 -= zclen2;
    while (zclen2--) {
      *zcp2++ = zch2;
    }
#else
    ZPUTC(zea2, zlen2, zcp2, zclen2, zch2);
#endif
  }
  stopwatch_pop(&sw_zfil);
  goto fetch;

d_zcm:  /* 001117 */
  TRACE(T_FLOW, " ZCM\n");
  if (crs[KEYS] & 020)
    zspace = 040;
  else
    zspace = 0240;
  zlen1 = GETFLR(0);
  zlen2 = GETFLR(1);
  zea1 = crsl[FAR0];
  if (crsl[FLR0] & 0x8000)
    zea1 |= EXTMASK32;
  zea2 = crsl[FAR1];
  if (crsl[FLR1] & 0x8000)
    zea2 |= EXTMASK32;
  TRACE(T_INST, " ea1=%o/%o, len1=%d, ea2=%o/%o, len2=%d\n", zea1>>16, zea1&0xffff, zlen1, zea2>>16, zea2&0xffff, zlen2);
  zresult = 0100;                /* assume equal */
  zclen1 = 0;
  zclen2 = 0;
  while (zlen1 || zlen2) {
    if (zlen1) {
      ZGETC(zea1, zlen1, zcp1, zclen1, zch1);
    } else
      zch1 = zspace;
    if (zlen2) {
      ZGETC(zea2, zlen2, zcp2, zclen2, zch2);
    } else
      zch2 = zspace;
    TRACE(T_INST, " zch1=%o (%c), zch2=%o (%c)\n", zch1, zch1&0x7f, zch2, zch2&0x7f);
    if (zch1 < zch2) {
      zresult = 0200;
      break;
    } else if (zch1 > zch2) {
      zresult = 0;
      break;
    }
  }
  crs[KEYS] = (crs[KEYS] & ~0300) | zresult;
  goto fetch;

d_ztrn:  /* 001110 */
  TRACE(T_FLOW, " ZTRN\n");
  zlen1 = GETFLR(1);
  zlen2 = zlen1;
  utempl = zlen1;
  zea1 = crsl[FAR0];
  if (crsl[FLR0] & 0x8000)
    zea1 |= EXTMASK32;
  zea2 = crsl[FAR1];
  if (crsl[FLR1] & 0x8000)
    zea2 |= EXTMASK32;
  TRACE(T_INST, " ea1=%o/%o, len1=%d, ea2=%o/%o, len2=%d\n", zea1>>16, zea1&0xffff, zlen1, zea2>>16, zea2&0xffff, zlen2);
  zclen1 = 0;
  zclen2 = 0;
  ea = *(ea_t *)(crs+XB);
  while (zlen2) {
    ZGETC(zea1, zlen1, zcp1, zclen1, zch1);
    utempa = get16(INCVA(ea,zch1/2));
    if (zch1 & 1)
      zch2 = utempa & 0xFF;
    else
      zch2 = utempa >> 8;
    TRACE(T_INST, " zch1=%o (%c), zch2=%o (%c)\n", zch1, zch1&0x7f, zch2, zch2&0x7f);
    ZPUTC(zea2, zlen2, zcp2, zclen2, zch2);
  }
  PUTFLR(1, 0);
  arfa(0, utempl);
  arfa(1, utempl);
  goto fetch;

d_zed:  /* 001111 */
  TRACE(T_FLOW, " ZED\n");
  zlen1 = GETFLR(0);
  zlen2 = 128*1024;      /* XXX: not sure about max length of result */
  zea1 = crsl[FAR0];
  if (crsl[FLR0] & 0x8000)
    zea1 |= EXTMASK32;
  zea2 = crsl[FAR1];
  if (crsl[FLR1] & 0x8000)
    zea2 |= EXTMASK32;
  TRACE(T_INST, " ea1=%o/%o, len1=%d, ea2=%o/%o, len2=%d\n", zea1>>16, zea1&0xffff, zlen1, zea2>>16, zea2&0xffff, zlen2);
  if (crs[KEYS] & 020)
    zspace = 040;
  else
    zspace = 0240;
  zclen1 = 0;
  zclen2 = 0;
  ea = *(ea_t *)(crs+XB);
  for (i=0; i < 32767; i++) {     /* do edit pgms have a size limit? */
    utempa = get16(INCVA(ea, i));
    m = utempa & 0xFF;
    switch ((utempa >> 8) & 3) {
    case 0:  /* copy M chars */
      while (m && zlen1) {
	ZGETC(zea1, zlen1, zcp1, zclen1, zch1);
	ZPUTC(zea2, zlen2, zcp2, zclen2, zch1);
	m--;
      }
      while (m) {
	ZPUTC(zea2, zlen2, zcp2, zclen2, zspace);
	m--;
      }
      break;

    case 1:  /* insert character M */
      ZPUTC(zea2, zlen2, zcp2, zclen2, m);
      break;

    case 2:  /* skip M characters */
      if (m >= zlen1)
	zlen1 = 0;
      else while (m) {
	ZGETC(zea1, zlen1, zcp1, zclen1, zch1);
	m--;
      }
      break;

    case 3:  /* insert M blanks */
      while (m) {
	ZPUTC(zea2, zlen2, zcp2, zclen2, zspace);
	m--;
      }
      break;

    default:
      fatal("ZED em bug");
    }
    if (utempa & 0x8000)
      break;
  }
  goto fetch;

d_xed:  /* 001112 */

  /* XED has some support for chars w/o parity by checking the
     keys before setting the zero suppress character, but it's
     not clear if it should ignore all character parity */

  TRACE(T_FLOW, " XED\n");
  zlen1 = zlen2 = 128*1024;
  zea1 = crsl[FAR0];
  if (crsl[FLR0] & 0x8000)
    zea1 |= EXTMASK32;
  zea2 = crsl[FAR1];
  if (crsl[FLR1] & 0x8000)
    zea2 |= EXTMASK32;
  zclen1 = 0;
  zclen2 = 0;
  TRACE(T_INST, " ea1=%o/%o, len1=%d, ea2=%o/%o, len2=%d\n", zea1>>16, zea1&0xffff, zlen1, zea2>>16, zea2&0xffff, zlen2);
  if (crs[KEYS] & 020)
    xsc = 040;
  else
    xsc = 0240;
  xfc = 0;
  ZGETC(zea1, zlen1, zcp1, zclen1, zch1);
  //printf("xed: first char = '%o\n", zch1);
  xsign = (zch1 == XMINUS);
  xsig = 0;
  ea = *(ea_t *)(crs+XB);
  for (i=0; i < 32767; i++) {     /* do edit pgms have a size limit? */
    utempa = get16(INCVA(ea, i));
    m = utempa & 0xFF;
    //printf("\nxed: %d: opcode = %o, m=%o\n", i, (utempa>>8) & 037, m);
    switch ((utempa >> 8) & 037) {
    case 0:  /* Zero Suppress */
      while (m) {
	ZGETC(zea1, zlen1, zcp1, zclen1, zch1);
	if (!xsig)
	  if (zch1 == XZERO)
	    zch1 = xsc;
	  else {
	    xsig = 1;
	    if (xfc) {
	      ZPUTC(zea2, zlen2, zcp2, zclen2, xfc);
	    }
	  }
	ZPUTC(zea2, zlen2, zcp2, zclen2, zch1);
	m--;
      }
      break;

    case 1:  /* insert character M */
      ZPUTC(zea2, zlen2, zcp2, zclen2, m);
      break;

    case 2:  /* set supression character */
      xsc = m;
      break;

    case 3:  /* insert character */
      if (xsig)
	zch1 = m;
      else
	zch1 = xsc;
      ZPUTC(zea2, zlen2, zcp2, zclen2, zch1);
      break;

    case 4:  /* insert digits */
      if (!xsig && xfc) {
	ZPUTC(zea2, zlen2, zcp2, zclen2, xfc);
      }
      while (m) {
	ZGETC(zea1, zlen1, zcp1, zclen1, zch1);
	ZPUTC(zea2, zlen2, zcp2, zclen2, zch1);
	m--;
      }
      xsig = 1;
      break;

    case 5:  /* insert char if minus */
      if (xsign)
	zch1 = m;
      else
	zch1 = xsc;
      ZPUTC(zea2, zlen2, zcp2, zclen2, zch1);
      break;

    case 6:  /* insert char if plus */
      if (!xsign)
	zch1 = m;
      else
	zch1 = xsc;
      ZPUTC(zea2, zlen2, zcp2, zclen2, zch1);
      break;

    case 7:  /* set floating char */
      xfc = m;
      break;

    case 010:  /* set floating if plus */
      if (!xsign)
	xfc = m;
      else
	xfc = xsc;
      break;

    case 011:  /* set floating if minus */
      if (xsign)
	xfc = m;
      else
	xfc = xsc;
      break;

    case 012:  /* set floating to sign */
      if (xsign)
	xfc = XMINUS;
      else
	xfc = XPLUS;
      break;

    case 013:  /* jump if zero */
      if (crs[A])
	i += m;
      break;

    case 014:  /* fill with suppress */
      while (m) {
	ZPUTC(zea2, zlen2, zcp2, zclen2, xsc);
	m--;
      }
      break;

    case 015:  /* set significance */
      if (!xsig && xfc) {
	ZPUTC(zea2, zlen2, zcp2, zclen2, xfc);
      }
      xsig = 1;
      break;

    case 016:  /* insert sign */
      if (xsign)
	zch1 = XMINUS;
      else
	zch1 = XPLUS;
      ZPUTC(zea2, zlen2, zcp2, zclen2, zch1);
      break;

    case 017:  /* suppress digits */
      while (m) {
	ZGETC(zea1, zlen1, zcp1, zclen1, zch1);
	if (zch1 == XZERO)
	  zch1 = xsc;
	ZPUTC(zea2, zlen2, zcp2, zclen2, zch1);
	m--;
      }
      break;

    case 020:  /* embed sign */
      while (m) {
	ZGETC(zea1, zlen1, zcp1, zclen1, zch1);
	if (xsign)
	  if (zch1 == XZERO)
	    zch1 = XRBRACE;
	  else
	    zch1 = zch1-XONE+XJ;
	ZPUTC(zea2, zlen2, zcp2, zclen2, zch1);
	m--;
      }
      break;

    default:
      warn("xed: unrecognized subprogram opcode ignored");
    }
    if (utempa & 0x8000)
      break;
  }
  goto fetch;

  /* unimplemented decimal instructions:

     - 001100 : XAD
     - 001101 : XMV
     - 001102 : XCM
     - 001104 : XMP
     - 001107 : XDV
     - 001145 : XBTD
     - 001146 : XDTB
  */

d_xuii:
  TRACE(T_FLOW, " X/Z UII %o\n", inst);
  fault(UIIFAULT, RPL, RP);
  fatal("Return from XZUII fault");

d_sttm:  /* 000510 */
  TRACE(T_FLOW, " STTM\n", inst);
  ea = *(ea_t *)(crs+OWNER);
  utempl = get32r0(ea+PCBPET);      /* get PCB elapsed timer */
  utempl += crs[TIMERH];            /* add live timer */
  ea = *(ea_t *)(crs+XB);
  put32(utempl, ea);                /* store process time */
  put16(crs[TIMERL], INCVA(ea,2));  /* and live timer residue */
  goto fetch;

  /* OS/restricted instructions */

d_rts:  /* 000511 */
  TRACE(T_FLOW, " RTS\n", inst);
  RESTRICT();
  tempa = crs[TIMERH];
  templ = tempa - *(short *)(crs+A);
  ea = *(ea_t *)(crs+OWNER);
  templ += get32r0(ea+PCBPET);
  put32r0(templ, ea+PCBPET);
  crs[TIMERH] = crs[A];
  goto fetch;

d_wait:  /* 000315 */
  TRACE(T_FLOW, " WAIT\n", inst);
  RESTRICT();
  pwait();
  goto fetch;

d_nfy:  /* 1210 (nfye), 1211 (nfyb), 
	   1214 (inen), 1215 (inbn), 1216 (inec), 1217 (inbc) */
  TRACE(T_FLOW, " NFY\n", inst);
  RESTRICT();
  nfy(inst);
  goto fetch;

d_stex:  /* 001315 */
  TRACE(T_FLOW, " STEX\n");
  *(ea_t *)(crs+L) = stex(*(unsigned int *)(crs+L));
  goto fetch;

  /* NOTE: L contains target virtual address, which is used to
     determine which pages of cache to invalidate.  Since this
     emulator does not have a memory cache, L is unused. */

d_liot:  /* 000044 */
  TRACE(T_FLOW, " LIOT\n");
  RESTRICT();
  ea = apea(NULL);
  utempa = STLBIX(ea);
  gvp->stlb[utempa].valid = 0;
  TRACE(T_INST, " invalidated STLB index %d\n", utempa);
  mapva(ea, RP, RACC, &access);
  TRACE(T_INST, " loaded STLB for %o/%o\n", ea>>16, ea&0xffff);
  goto fetch;

d_ptlb:  /* 000064 */
  TRACE(T_FLOW, " PTLB\n");
  RESTRICT();
  utempl = *(unsigned int *)(crs+L);
  for (utempa = 0; utempa < STLBENTS; utempa++)
    if ((utempl & 0x80000000) || gvp->stlb[utempa].ppn == utempl)
      gvp->stlb[utempa].valid = 0;
  goto fetch;

d_itlb:  /* 000615 */
  TRACE(T_FLOW, " ITLB\n");
  RESTRICT();
  utempl = *(unsigned int *)(crs+L);

  /* NOTE: Primos substitutes an ITLB loop for PTLB, and the ITLB
     segno is 1, ie, it looks like using segment 1 invalidates all
     pages that match, ignoring segment number??  Instead of doing
     that, we purge the STLB whenever address 1/0 is invalidated. */

  if (utempl == 0x10000) {
    for (utempa = 0; utempa < STLBENTS; utempa++)
      gvp->stlb[utempa].valid = 0;
    TRACE(T_INST, " purged entire STLB\n");
  } else {
    utempa = STLBIX(utempl);
    gvp->stlb[utempa].valid = 0;
    TRACE(T_INST, " invalidated STLB index %d\n", utempa);
    if (((utempl >> 16) & 07777) < 4)
      gvp->iotlb[(utempl >> 10) & 0xFF].valid = 0;
  }
  goto fetch;

d_lpsw:  /* 000711 */
  TRACE(T_FLOW, " LPSW\n");
  RESTRICT();
  lpsw();
  goto fetch;

d_stpm:  /* 000024 */
  TRACE(T_FLOW, " STPM\n", inst);
  RESTRICT();
  for (i=0; i<8; i++)
    stpm[i] = 0;
  stpm[1] = cpuid;
  ea = *(unsigned int *)(crs+XB);
  put64(*(long long *)(stpm+0), ea);
  put64(*(long long *)(stpm+4), INCVA(ea,4));
  goto fetch;

d_dbgill:  /*  001700, 001701 */
  TRACE(T_FLOW, " DBGILL\n", inst);
  fault(ILLINSTFAULT, RPL, RP);
  fatal(NULL);

      /* JW: I think 1702 is an invalid opcode that Prime uses as
	 an assertion when unexpected things happen, for example:

	 LDA modals        get modals
	 SAS 1             interrupts enabled?
	 1702              no, they should be, die
      */

d_pbug:  /* 001702 */
  TRACE(T_FLOW, " 1702?\n", inst);
  if (RP & RINGMASK32)
    fault(ILLINSTFAULT, RPL, RP);
  else
    fatal("Primos software assertion failure");
  fatal(NULL);  /* just in case of a bogus return (coding error) */

d_irtn:  /* 000601 */
  TRACE(T_FLOW, " IRTN\n", inst);
  RESTRICT();
  //fatal("IRTN causes a loop in CPU.CACHE Case 4");
irtn:
  RP = regs.sym.pswpb;
  crs[PBH] = RPH;
  newkeys(regs.sym.pswkeys);
  crs[MODALS] |= 0100000;
#if 0
  if (regs.sym.pcba != 0) {
    RP = regs.sym.pswpb;
    newkeys(regs.sym.pswkeys);
  } else
    crs[OWNERL] = 0;
#endif
  dispatcher();
  goto fetch;

d_irtc:  /* 000603 */
  TRACE(T_FLOW, " IRTC\n", inst);
  RESTRICT();
  gvp->intvec = -1;
  goto irtn;

d_cai:  /* 000411 */
  TRACE(T_FLOW, " CAI\n", inst);
  RESTRICT();
  gvp->intvec = -1;
  goto fetch;

  /* R-mode/infrequent gen 0 instructions */

d_sgl:  /* 000005 */
  TRACE(T_FLOW, " SGL\n");
  crs[KEYS] &= ~040000;
  goto fetch;

d_e16s:  /* 000011 */
  TRACE(T_FLOW, " E16S\n");
  newkeys(crs[KEYS] & 0161777);
  goto fetch;

d_e32s:  /* 000013 */
  TRACE(T_FLOW, " E32S\n");
  newkeys((crs[KEYS] & 0161777) | 1<<10);
  goto fetch;

d_e32r:  /* 001013 */
  TRACE(T_FLOW, " E32R\n");
  newkeys((crs[KEYS] & 0161777) | 3<<10);
  goto fetch;

d_e64r:  /* 001011 */
  TRACE(T_FLOW, " E64R\n");
  newkeys((crs[KEYS] & 0161777) | 2<<10);
  goto fetch;

d_e64v:  /* 000010 */
  TRACE(T_FLOW, " E64V\n");
  newkeys((crs[KEYS] & 0161777) | 6<<10);
  goto fetch;

d_e32i:  /* 001010 */
  TRACE(T_FLOW, " E32I\n");

  /* NOTE: this fault needs to occur on older models even in
     Ring 0, so the RESTRICT() macro can't be used here.

     XXX: for a P500 (cpuid=0), this shouldn't fault! */

  if (cpuid < 4)
    fault(RESTRICTFAULT, 0, 0);
  else
    newkeys((crs[KEYS] & 0161777) | 4<<10);
  goto fetch;

d_svc:  /* 000505 */
  TRACE(T_FLOW, " SVC\n");
  fault(SVCFAULT, 0, 0);
  fatal("Returned from SVC fault");
  
d_cea:  /* 000111 */
  TRACE(T_FLOW, " CEA\n");
  switch ((crs[KEYS] & 016000) >> 10) {
  case 0:                       /* 16S */
    ea = crs[A];
    i = ea & 0100000;
    x = ea & 040000;
    ea &= 037777;
    while (1) {
      if (x)                           /* indexed */
	ea += crs[X];
      if (!i)                          /* not indirect */
	break;
      if (ea < 040)
	m = get16(0x80000000|ea);
      else
	m = get16(MAKEVA(RPH,ea));
      i = m & 0100000;
      x = m & 040000;
      ea = m & 037777;                 /* go indirect */
    }
    crs[A] = ea;
    break;
  case 1:                       /* 32S */
  case 3:                       /* 32R */
    while (crs[A] & 0100000) {
      ea = crs[A] & 077777;
      if (ea < 040)
	crs[A] = get16(0x80000000|ea);
      else
	crs[A] = get16(MAKEVA(RPH,ea));
    }
  }
  goto fetch;

d_hlt:  /* 000000 */
  TRACE(T_FLOW, " HLT\n");
  RESTRICT();
  memdump(0,0xFFFF);
  fatal("CPU halt");

d_pim:  /* 000205 (R-mode) */
  TRACE(T_FLOW, " PIM\n");
#if 0
  /* NOTE: this fits the description in the Rev 21 ISG, but fails
     DIAG test CPU.INTEGER, Case 12 */

  crs[A] = (crs[A] & 0x8000) | (crs[B] & 0x7FFF);
#else
  crs[A] = (crs[A] & 0x8000) | crs[B];
#endif
  goto fetch;

d_pid:  /* 000211 (R-mode) */
  TRACE(T_FLOW, " PID\n");
  *(int *)(crs+L) = *(short *)(crs+A);
  crs[B] &= 0x7fff;
  goto fetch;

d_dbl:  /* 000007 (R-mode) */

  /* DBL activates 31-bit mode (R-mode only):

      LDA -> DLD (double load)
      STA -> DST (double store)
      ADD -> DAD (double add)
      SUB -> DSB (double subtract)

      Other R-mode, 31-bit instructions include:

      PID, DIV, MPY, PIM, INT, FLOT
  */

  TRACE(T_FLOW, " DBL\n");
  crs[KEYS] |= 040000;
  goto fetch;

d_sca:  /* 000041 */
  TRACE(T_FLOW, " SCA\n");
  crs[A] = crs[VSC] & 0xFF;
  goto fetch;

d_inkr:  /* 000043 */
  TRACE(T_FLOW, " INKr\n");
  crs[A] = (crs[KEYS] & 0xFF00) | (crs[VSC] & 0xFF);
  goto fetch;

 d_otkr:  /* 000405 */
  TRACE(T_FLOW, " OTKr\n");
  newkeys((crs[A] & 0xFF00) | (crs[KEYS] & 0xFF));
  crs[VSC] = (crs[VSC] & 0xFF00) | (crs[A] & 0xFF);
  if ((RP & RINGMASK32) == 0)
    gvp->inhcount = 1;
  goto fetch;

d_esim:  /* 000415 */
  TRACE(T_FLOW, " ESIM\n");
  RESTRICT();
  crs[MODALS] &= ~040000;
  goto fetch;

d_evim:  /* 000417 */
  TRACE(T_FLOW, " EVIM\n");
  RESTRICT();
  crs[MODALS] |= 040000;
  goto fetch;

d_nrm:  /* 000101 */
  TRACE(T_FLOW, " NRM\n");
  crs[VSC] = 0;
  if (crsl[GR2] != 0) {
    while (!((crs[A] ^ (crs[A] << 1)) & 0x8000)) {
      TRACE(T_INST,  " step %d: crs[A]=%o, crs[B]=%o\n", crs[VSC], crs[A], crs[B]);
      crs[B] = crs[B] << 1;
      crs[A] = (crs[A] & 0x8000) | ((crs[A] << 1) & 0x7FFE) | (crs[B] >> 15);
      crs[VSC]++;
    }
    crs[B] &= 0x7FFF;
    TRACE(T_INST,  " finished with %d shifts: crs[A]=%o, crs[B]=%o\n", crs[VSC], crs[A], crs[B]);
  }
  goto fetch;

d_rtn:  /* 000105 */
  TRACE(T_FLOW, " RTN\n");
  m = get16(crs[S]+1);
  if (m == 0)
    fatal("RTN stack underflow");
  crs[S] = get16(crs[S]);
  goto fetch;

  /* unusual instructions */

d_sync:  /* 000003 */
  TRACE(T_FLOW, " SYNC\n");

  /* After looking at the simh Honeywell 315/516 simulator, I
     decided that instruction 3 must be some kind of no-op on
     the Prime.  I did verify that it is a legal instruction and
     doesn't generate a UII fault, even though it is not
     documented anywhere that I could find.

     FTN executes this instruction (by JMP to wrong relative
     address I think), and newer versions of Primos execute it
     in the disk driver code, to sync the cache on multi-processor
     systems (something like that).
  */

  goto fetch;

d_bclt:  /* 0141604 */
  TRACE(T_FLOW, " BCLT\n");
  BCLT;
  goto fetch;

d_bcle:  /* 0141600 */
  TRACE(T_FLOW, " BCLE\n");
  BCLE;
  goto fetch;

d_bceq:  /* 0141602 */
  TRACE(T_FLOW, " BCEQ\n");
  BCEQ;
  goto fetch;

d_bcne:  /* 0141603 */
  TRACE(T_FLOW, " BCNE\n");
  BCNE;
  goto fetch;

d_bcge:  /* 0141605 */
  TRACE(T_FLOW, " BCGE\n");
  BCGE;
  goto fetch;

d_bcgt:  /* 0141601 */
  TRACE(T_FLOW, " BCGT\n");
  BCGT;
  goto fetch;

d_bcr:  /* 0141705 */
  TRACE(T_FLOW, " BCR\n");
  if (!(crs[KEYS] & 0100000))
    RPL = iget16(RP);
  else
    INCRP;
  goto fetch;

d_bcs:  /* 0141704 */
  TRACE(T_FLOW, " BCS\n");
  if (crs[KEYS] & 0100000)
    RPL = iget16(RP);
  else
    INCRP;
  goto fetch;

d_blr:  /* 0141707 */
  TRACE(T_FLOW, " BMLT/BLR\n");
  if (!(crs[KEYS] & 020000))
    RPL = iget16(RP);
  else
    INCRP;
  goto fetch;

d_bls:  /* 0141706 */
  TRACE(T_FLOW, " BLS\n");
  BLS;
  goto fetch;

d_blt:  /* 0140614 */
  TRACE(T_FLOW, " BLT\n");
  SETCC_A;
  BCLT;
  goto fetch;

d_ble:  /* 0140610 */
  TRACE(T_FLOW, " BLE\n");
  SETCC_A;
  BCLE;
  goto fetch;

d_beq:  /* 0140612 */
  TRACE(T_FLOW, " BEQ\n");
  SETCC_A;
  BCEQ;
  goto fetch;

d_bne:  /* 0140613 */
  TRACE(T_FLOW, " BNE\n");
  SETCC_A;
  BCNE;
  goto fetch;

d_bge:  /* 0140615 */
  TRACE(T_FLOW, " BGE\n");
  SETCC_A;
  BCGE;
  goto fetch;

d_bgt:  /* 0140611 */
  TRACE(T_FLOW, " BGT\n");
  SETCC_A;
  BCGT;
  goto fetch;

d_blle:  /* 0140700 */
  TRACE(T_FLOW, " BLLE\n");
  SETCC_L;
  BCLE;
  goto fetch;

d_bleq:  /* 0140702 */
  TRACE(T_FLOW, " BLEQ\n");
  SETCC_L;
  BCEQ;
  goto fetch;

d_blne:  /* 0140703 */
  TRACE(T_FLOW, " BLNE\n");
  SETCC_L;
  BCNE;
  goto fetch;

d_blgt:  /* 0140701 */
  TRACE(T_FLOW, " BLGT\n");
  SETCC_L;
  BCGT;
  goto fetch;

d_bflt:  /* 0141614 */
  TRACE(T_FLOW, " BFLT\n");
  SETCC_F;
  BCLT;
  goto fetch;

d_bfle:  /* 0141610 */
  TRACE(T_FLOW, " BFLE\n");
  SETCC_F;
  BCLE;
  goto fetch;

d_bfeq:  /* 0141612 */
  TRACE(T_FLOW, " BFEQ\n");
  SETCC_F;
  BCEQ;
  goto fetch;

d_bfne:  /* 0141613 */
  TRACE(T_FLOW, " BFNE\n");
  SETCC_F;
  BCNE;
  goto fetch;

d_bfge:  /* 0141615 */
  TRACE(T_FLOW, " BFGE\n");
  SETCC_F;
  BCGE;
  goto fetch;

d_bfgt:  /* 0141611 */
  TRACE(T_FLOW, " BFGT\n");
  SETCC_F;
  BCGT;
  goto fetch;

d_bix:  /* 0141334 */
  TRACE(T_FLOW, " BIX\n");
  crs[X]++;
  BXNE;
  goto fetch;

d_biy:  /* 0141324 */
  TRACE(T_FLOW, " BIY\n");
  crs[Y]++;
  BYNE;
  goto fetch;

d_bdy:  /* 0140724 */
  TRACE(T_FLOW, " BDY\n");
  crs[Y]--;
  BYNE;
  goto fetch;

d_bdx:  /* 0140734 */
  TRACE(T_FLOW, " BDX\n");
  crs[X]--;
#if 0
  m = iget16(RP);
  if (crs[X] > 100 && m == RPL-1) {
    struct timeval tv0,tv1;
    long delayusec, actualmsec;

    /* for BDX *-1 loop (backstop process mainly), we want to change
       this to a long sleep so that the emulation host's CPU isn't 
       pegged the whole time the emulator is running.

       So first, check to see if any device times expire sooner than
       this, and if so, limit the sleep time to the lowest expiration
       value (this is stored as number of instructions left until the
       timer expires).

       NOTE: In practice, the clock device ticks at 330 times a sec
       under standard Primos so we only get to delay about 3ms here,
       but it still keeps CPU usage to 4-5% on a 1.5GHz Mac.  Primos
       mods to make the clock tick 20 times per second allows for
       much longer sleeps here, ie, CPU overhead is 0.7% while idle.
    */

    stopwatch_start(&sw_idle);
    utempl = gvp->instpermsec*100;         /* limit delay to 100 msecs */
    for (i=0; i<64; i++)              /* check device timers */
      if (devpoll[i])                 /* poll set? */
	if (devpoll[i] <= 100) {      /* too fast! */
	  utempl = 1;
	  break;
	} else if (devpoll[i] < utempl)
	  utempl = devpoll[i];
    utempl--;                         /* utempl = # instructions */
    delayusec = utempl*1000/gvp->instpermsec;
    if (delayusec > 1000) {
      if (gettimeofday(&tv0, NULL) != 0)
	fatal("em: gettimeofday 0 failed");
      usleep(delayusec);
      if (gettimeofday(&tv1, NULL) != 0)
	fatal("em: gettimeofday 1 failed");
      actualmsec = (tv1.tv_sec-tv0.tv_sec-1)*1000 + (tv1.tv_usec+1000000-tv0.tv_usec)/1000;
      // TRACEA(" BDX loop at %o/%o, remainder=%d, owner=%o, utempl=%d, wanted %d us, got %d ms\n", gvp->prevpc>>16, gvp->prevpc&0xffff, crs[X], crs[OWNERL], utempl, delayusec, actualusec);

      /* do timer bookkeeping that would have occurred if we had 
	 actually looped on BDX utempl times */

      for (i=0; i<64; i++)
	if (devpoll[i] > 0)
	  devpoll[i] -= utempl;
      crs[X] = 0;
      utempa = crs[TIMER];
      if (actualmsec > 0) {
	crs[TIMER] += actualmsec;
	if (crs[TIMER] < utempa) {
	  tempea = *(ea_t *)(crs+OWNER);
	  utempa = get16r0(tempea+4) | 1;    /* set process abort flag */
	  put16r0(utempa, tempea+4);
	}
      } else {
	crs[TIMERL] += utempl;
      }
      gvp->instcount += actualmsec*gvp->instpermsec;
    }
    stopwatch_stop(&sw_idle);
  }
#endif
  BXNE;
  goto fetch;

d_a1a:  /* 0141206 */
  TRACE(T_FLOW, " A1A\n");
a1a:
  add16(crs+A, 1, 0, 0);
  goto fetch;

d_a2a:  /* 0140304 */
  TRACE(T_FLOW, " A2A\n");
  add16(crs+A, 2, 0, 0);
  goto fetch;

d_aca:  /* 0141216 */
  TRACE(T_FLOW, " ACA\n");
  if (crs[KEYS] & 0100000)
    goto a1a;
  crs[KEYS] &= ~0120300;                 /* clear C, L, LT, EQ */
  SETCC_A;
  goto fetch;

d_s1a:  /* 0140110 */
  TRACE(T_FLOW, " S1A\n");
  add16(crs+A, 0xFFFF, 0, 0);
  goto fetch;

d_s2a:  /* 0140310 */
  TRACE(T_FLOW, " S2A\n");
  add16(crs+A, 0xFFFE, 0, 0);
  goto fetch;

d_cal:  /* 0141050 */
  TRACE(T_FLOW, " CAL\n");
  crs[A] &= 0xFF;
  goto fetch;

d_car:  /* 0141044 */
  TRACE(T_FLOW, " CAR\n");
  crs[A] &= 0xFF00;
  goto fetch;

d_cra:  /* 0140040 */
  TRACE(T_FLOW, " CRA\n");
  crs[A] = 0;
  goto fetch;

  /* On the P300, the B register is the low-order word of the
     DP floating pt fraction, so CRB was used to convert SPFP
     numbers to DPFP.  On the P400 and up, the B register and
     DPFP accumulator do not overlap.  For compatibility, there
     are 3 related instructions:

     '14 clears B and the low-order DPFP register
     '15 clears only B
     '16 clears only the low-order DPFP register
  */

d_crb300:  /* 0140014 */
  TRACE(T_FLOW, " P300CRB\n");
  crs[B] = 0;
  crs[FLTD] = 0;
  goto fetch;

d_crb:  /* 0140015 */
  TRACE(T_FLOW, " CRB\n");
  crs[B] = 0;
  goto fetch;

d_fdbl:  /* 0140016 */
  TRACE(T_FLOW, " FDBL\n");
  crs[FLTD] = 0;
  goto fetch;

d_crl:  /* 0140010 */
  TRACE(T_FLOW, " CRL\n");
  *(int *)(crs+L) = 0;
  goto fetch;

d_caz:  /* 0140214 */
  TRACE(T_FLOW, " CAZ\n");
  /* set keys like CAS =0 would (subtract) */
  crs[KEYS] = (crs[KEYS] & ~0100) | 020200;   /* clear EQ, set L, LT */
  if (crs[A] == 0) {                  /* if zero, set EQ */
    SETEQ;
    INCRP;
  } else if (*(short *)(crs+A) < 0)
    RPL += 2;
  goto fetch;


d_irx:  /* 0140114 */

  /* NOTE: using "if (crs[X]++ == 0)" doesn't work because of
     unsigned short type promotion! */

  TRACE(T_FLOW, " IRX\n");
  crs[X]++;
  if (crs[X] == 0)
    INCRP;
  goto fetch;

d_drx:  /* 0140210 */
  TRACE(T_FLOW, " DRX\n");
  crs[X]--;
  if (crs[X] == 0)
    INCRP;
  goto fetch;

d_icr:  /* 0141240 */
  TRACE(T_FLOW, " ICR\n");
  crs[A] = crs[A] << 8;
  goto fetch;

d_icl:  /* 0141140 */
  TRACE(T_FLOW, " ICL\n");
  crs[A] = crs[A] >> 8;
  goto fetch;

d_ica:  /* 0141340 */
  TRACE(T_FLOW, " ICA\n");
  crs[A] = (crs[A] >> 8) | (crs[A] << 8);
  goto fetch;

  /* NOTE: Rev 21 Inst. Guide says CC are indeterminate, other
     references say they are set */

d_lt:  /* 0140417 */
  TRACE(T_FLOW, " LT\n");
  crs[A] = 1;
  goto fetch;

d_lf:  /* 0140416 */
  TRACE(T_FLOW, " LF\n");
  crs[A] = 0;
  goto fetch;

d_tab:  /* 0140314 */
  TRACE(T_FLOW, " TAB\n");
  crs[B] = crs[A];
  goto fetch;

d_tax:  /* 0140504 */
  TRACE(T_FLOW, " TAX\n");
  crs[X] = crs[A];
  goto fetch;

d_tay:  /* 0140505 */
  TRACE(T_FLOW, " TAY\n");
  crs[Y] = crs[A];
  goto fetch;

d_tba:  /* 0140604 */
  TRACE(T_FLOW, " TBA\n");
  crs[A] = crs[B];
  goto fetch;

d_txa:  /* 0141034 */
  TRACE(T_FLOW, " TXA\n");
  crs[A] = crs[X];
  goto fetch;

d_tya:  /* 0141124 */
  TRACE(T_FLOW, " TYA\n");
  crs[A] = crs[Y];
  goto fetch;

d_xca:  /* 0140104 */
  TRACE(T_FLOW, " XCA\n");
#if 1
  crsl[GR2] = crsl[GR2] >> 16;
#else
  crs[B] = crs[A];
  crs[A] = 0;
#endif
  goto fetch;

d_xcb:  /* 0140204 */
  TRACE(T_FLOW, " XCB\n");
#if 1
  crsl[GR2] = crsl[GR2] << 16;
#else
  crs[A] = crs[B];
  crs[B] = 0;
#endif
  goto fetch;

d_tca:  /* 0140407 */
  TRACE(T_FLOW, " TCA\n");
  tch(crs+A);
  goto fetch;

d_tcl:  /* 0141210 */
  TRACE(T_FLOW, " TCL\n");
  tcr(crsl+GR2);
  goto fetch;

d_scb:  /* 0140600 */
  TRACE(T_FLOW, " SCB\n");
  crs[KEYS] |= 0100000;
  goto fetch;

d_rcb:  /* 0140200 */
  TRACE(T_FLOW, " RCB\n");
  crs[KEYS] &= 077777;
  goto fetch;

d_chs:  /* 0140024 */
  TRACE(T_FLOW, " CHS\n");
  crs[A] ^= 0x8000;
  goto fetch;

d_ssm:  /* 0140500 */
  TRACE(T_FLOW, " SSM\n");
  crs[A] |= 0100000;
  goto fetch;

d_ssp:  /* 0140100 */
  TRACE(T_FLOW, " SSP\n");
  crs[A] &= 077777;
  goto fetch;

d_cma:  /* 0140401 */
  TRACE(T_FLOW, " CMA\n");
  crs[A] = ~crs[A];
  goto fetch;

d_csa:  /* 0140320 */
  TRACE(T_FLOW, " CSA\n");
  crs[KEYS] = (crs[KEYS] & 077777) | (crs[A] & 0x8000);
  crs[A] = crs[A] & 077777;
  goto fetch;

d_lclt:  /* 0141500 */
  TRACE(T_FLOW, " LCLT\n");
  crs[A] = LCLT;
  goto fetch;

d_lcle:  /* 0141501 */
  TRACE(T_FLOW, " LCLE\n");
  crs[A] = LCLE;
  goto fetch;

d_lceq:  /* 0141503 */
  TRACE(T_FLOW, " LCEQ\n");
  crs[A] = LCEQ;
  goto fetch;

d_lcne:  /* 0141502 */
  TRACE(T_FLOW, " LCNE\n");
  crs[A] = LCNE;
  goto fetch;

d_lcge:  /* 0141504 */
  TRACE(T_FLOW, " LCGE\n");
  crs[A] = LCGE;
  goto fetch;

d_lcgt:  /* 0141505 */
  TRACE(T_FLOW, " LCGT\n");
  crs[A] = LCGT;
  goto fetch;

d_llt:  /* 0140410 */
  TRACE(T_FLOW, " LLT\n");
  SETCC_A;
  crs[A] = LCLT;
  goto fetch;

d_lle:  /* 0140411 */
  TRACE(T_FLOW, " LLE\n");
  SETCC_A;
  crs[A] = LCLE;
  goto fetch;

d_lne:  /* 0140412 */
  TRACE(T_FLOW, " LNE\n");
  SETCC_A;
  crs[A] = LCNE;
  goto fetch;

d_leq:  /* 0140413 */
  TRACE(T_FLOW, " LEQ\n");
  SETCC_A;
  crs[A] = LCEQ;
  goto fetch;

d_lge:  /* 0140414 */
  TRACE(T_FLOW, " LGE\n");
  SETCC_A;
  crs[A] = LCGE;
  goto fetch;

d_lgt:  /* 0140415 */
  TRACE(T_FLOW, " LGT\n");
  SETCC_A;
  crs[A] = LCGT;
  goto fetch;

d_llle:  /* 0141511 */
  TRACE(T_FLOW, " LLLE\n");
  SETCC_L;
  crs[A] = LCLE;
  goto fetch;

d_lleq:  /* 0141513 */
  TRACE(T_FLOW, " LLEQ\n");
  SETCC_L;
  crs[A] = LCEQ;
  goto fetch;

d_llne:  /* 0141512 */
  TRACE(T_FLOW, " LLNE\n");
  SETCC_L;
  crs[A] = LCNE;
  goto fetch;

d_llgt:  /* 0141515 */
  TRACE(T_FLOW, " LLGT\n");
  SETCC_L;
  crs[A] = LCGT;
  goto fetch;

d_lflt:  /* 0141110 */
  TRACE(T_FLOW, " LFLT\n");
  SETCC_F;
  crs[A] = LCLT;
  goto fetch;

d_lfle:  /* 0141111 */
  TRACE(T_FLOW, " LFLE\n");
  SETCC_F;
  crs[A] = LCLE;
  goto fetch;

d_lfeq:  /* 0141113 */
  TRACE(T_FLOW, " LFEQ\n");
  SETCC_F;
  crs[A] = LCEQ;
  goto fetch;

d_lfne:  /* 0141112 */
  TRACE(T_FLOW, " LFNE\n");
  SETCC_F;
  crs[A] = LCNE;
  goto fetch;

d_lfge:  /* 0141114 */
  TRACE(T_FLOW, " LFGE\n");
  SETCC_F;
  crs[A] = LCGE;
  goto fetch;

d_lfgt:  /* 0141115 */
  TRACE(T_FLOW, " LFGT\n");
  SETCC_F;
  crs[A] = LCGT;
  goto fetch;

d_flot:  /* 0140550 */
  TRACE(T_FLOW, " FLOT\n");
  templ = *(short *)(crs+A);
  templ = (templ<<15) | crs[B];
  *(double *)(crs+FLTH) = fltl(templ);
  goto fetch;

d_frn:  /* 0140534 */
  TRACE(T_FLOW, " FRN\n");
  CLEARC;
  frn(crsl+FAC1);
  goto fetch;

d_dfcm:  /* 0140574 */
  TRACE(T_FLOW, " DFCM\n");
  dfcm(crsl+FAC1);
  goto fetch;

d_adll:  /* 0141000 */
  TRACE(T_FLOW, " ADLL\n");
  adlr(2);
  goto fetch;

d_fcmv:  /* 0140530 */
  TRACE(T_FLOW, " FCMv\n");
  dfcm(crsl+FAC1);
  goto fetch;

d_fsze:  /* 0140510 */
  TRACE(T_FLOW, " FSZE\n");
  if (*(int *)(crs+FLTH) == 0)
    INCRP;
  goto fetch;

d_fsnz:  /* 0140511 */
  TRACE(T_FLOW, " FSNZ\n");
  if (*(int *)(crs+FLTH) != 0)
    INCRP;
  goto fetch;

d_fsmi:  /* 0140512 */
  TRACE(T_FLOW, " FSMI\n");
  if (*(int *)(crs+FLTH) < 0)
    INCRP;
  goto fetch;

d_fspl:  /* 0140513 */
  TRACE(T_FLOW, " FSPL\n");
  if (*(int *)(crs+FLTH) >= 0)
    INCRP;
  goto fetch;

d_fsle:  /* 0140514 */
  TRACE(T_FLOW, " FSLE\n");
  if (*(int *)(crs+FLTH) <= 0)
    INCRP;
  goto fetch;

d_fsgt:  /* 0140515 */
  TRACE(T_FLOW, " FSGT\n");
  if (*(int *)(crs+FLTH) > 0)
    INCRP;
  goto fetch;

d_int:  /* 0140554 */
  TRACE(T_FLOW, " INT\n");
  /* XXX: do -1073741824.5 and 1073741823.5 work on Prime, or overflow? */
  if (prieee8(crs+FLTH, &tempd) && -1073741824.0 <= tempd && tempd <= 1073741823.0) {
    templ = tempd;
    crs[B] = templ & 0x7FFF;
    crs[A] = templ >> 15;
    CLEARC;
  } else
    mathexception('f', FC_INT_CONV, ea);
  goto fetch;

d_inta:  /* 0140531 */
  TRACE(T_FLOW, " INTA\n");
  /* XXX: do 32767.5 and -32768.5 work on Prime, or overflow? */
  if (prieee8(crs+FLTH, &tempd) && -32768.0 <= tempd && tempd <= 32767.0) {
    *(short *)(crs+A) = tempd;
    CLEARC;
  } else
    mathexception('f', FC_INT_CONV, ea);
  goto fetch;

d_flta:  /* 0140532 */
  TRACE(T_FLOW, " FLTA\n");
  tempd = *(short *)(crs+A);
  *(double *)(crs+FLTH) = ieeepr8(tempd);
  goto fetch;

d_intl:  /* 0140533 */
  TRACE(T_FLOW, " INTL\n");
  if (prieee8(crs+FLTH, &tempd) && -2147483648.0 <= tempd && tempd <= 2147483647.0) {
    *(int *)(crs+L) = tempd;
    CLEARC;
  } else
    mathexception('f', FC_INT_CONV, ea);
  goto fetch;

d_fltl:  /* 0140535 */
  TRACE(T_FLOW, " FLTL\n");
  *(double *)(crs+FLTH) = fltl(crsl[GR2]);
  goto fetch;

d_bmle:  /* 0141711 */
  TRACE(T_FLOW, " BMLE\n");
  if (!(crs[KEYS] & 020000))
    RPL = iget16(RP);
  else
    BCEQ;
  goto fetch;

#if 0
d_bmeq:  /* 0141602 */   /* same opcode as BCEQ */
  TRACE(T_FLOW, " BMEQ\n");
  goto bceq;

d_bmne:  /* 0141603 */   /* same opcode as BCNE */
  TRACE(T_FLOW, " BMNE\n");
  goto bcne;

  /* NOTE: BMGE is equivalent to BLS; this opcode doesn't exist
     in newer manuals */

d_bmge:  /* 0141606 */
  TRACE(T_FLOW, " BMGE\n");
  goto bls;
#endif

d_bmgt:  /* 0141710 */
  TRACE(T_FLOW, " BMGT\n");
  if (crs[KEYS] & 020000)
    BCNE;
  else
    INCRP;
  goto fetch;

d_cre:  /* 0141404 */
  TRACE(T_FLOW, " CRE\n");
  *(int *)(crs+E) = 0;
  goto fetch;

d_crle:  /* 0141410 */
  TRACE(T_FLOW, " CRLE\n");
  *(int *)(crs+L) = 0;
  *(int *)(crs+E) = 0;
  goto fetch;

d_ile:  /* 0141414 */
  TRACE(T_FLOW, " ILE\n");
  templ = *(int *)(crs+L);
  *(int *)(crs+L) = *(int *)(crs+E);
  *(int *)(crs+E) = templ;
  goto fetch;

  /* these next 4 are V/I-mode quad mode:

      0140570:  QFCM - quad complement
      0140571:  DRNM - round minus Q to D
      0140572:  QINQ - trucate Q fraction
      0140573:  QIQR - round and remove Q fraction
  */
d_quii:
  TRACE(T_FLOW, " QFCM DRNM QINQ QIQR UII\n");
  fault(UIIFAULT, RPL, RP);
  fatal("Return from d_quii");

  /* queue instructions */

d_rtq:  /* 0141714 */
  TRACE(T_FLOW, " RTQ\n");
  ea = apea(NULL);
  if (rtq(ea, crs+A, RP))
    CLEAREQ;
  else
    SETEQ;
  goto fetch;

d_rbq:  /* 0141715 */
  TRACE(T_FLOW, " RBQ\n");
  ea = apea(NULL);
  if (rbq(ea, crs+A, RP))
    CLEAREQ;
  else
    SETEQ;
  goto fetch;

d_abq:  /* 0141716 */
  TRACE(T_FLOW, " ABQ\n");
  ea = apea(NULL);
  if (abq(ea, crs[A], RP))
    CLEAREQ;
  else
    SETEQ;
  goto fetch;

d_atq:  /* 0141717 */
  TRACE(T_FLOW, " ATQ\n");
  ea = apea(NULL);
  if (atq(ea, crs[A], RP))
    CLEAREQ;
  else
    SETEQ;
  goto fetch;

d_tstq:  /* 0141757 */
  TRACE(T_FLOW, " TSTQ\n");
  ea = apea(NULL);
  crs[A] = tstq(ea);
  SETCC_A;
  goto fetch;

 d_diagill:  /* 0141700 */

  /* XXX: hack for CPU.FAULT; not sure how to determine
     whether an instruction is illegal or unimplemented */
  
  fault(ILLINSTFAULT, RPL, RP);
  fatal("Return from 0141700 fault");

d_emcm:  /* 000503 - enter machine check mode */
  TRACE(T_FLOW, " EMCM\n");
  goto fetch;

d_lmcm:  /* 000501 - leave machine check mode */
  TRACE(T_FLOW, " LMCM\n");
  goto fetch;

d_rmc:  /* 000021 - reset machine check FF */
  TRACE(T_FLOW, " RMC\n");
  goto fetch;

d_viry:  /* 000311 - ucode verify */
  TRACE(T_FLOW, " VIRY\n");
  goto fetch;

d_xvfy:  /* 001113 - extended ucode verify */
  TRACE(T_FLOW, " XVFY\n");
  goto fetch;

  /* memory diagnostic opcodes:
      001304:  MDEI - enable interleave
      001305:  MDII - inhibit interleave
      001306:  MDRS - reset syndrome bits
      001307:  MDWC - write cache
      001324:  MDIW - inhibit wide-word mode?
  */

d_mdxx:  /* 01304-01307, 01324 */
  TRACE(T_FLOW, " MDxx\n");
  goto fetch;

d_gen1:

  /* this is a bit weird here: the shift group is really only for
     V-mode instructions, but Prime put some I-mode generics in
     the same instruction space.  Not sure, but I think a real
     Prime would probably take an illegal instruction fault on
     something like LRL executed in I-mode, but the emulator will
     just do it.  */

  TRACE(T_INST, " shift group\n");
  scount = -inst & 077;
  if (scount == 0)
    scount = 0100;
  switch (inst & 01700) {

  case 00000: /* LRL */
    TRACE(T_FLOW, " LRL %d\n", scount);
    crsl[GR2] = lrl(crsl[GR2], scount);
    break;

  case 00100: /* LRS (different in R & V modes) */
    TRACE(T_FLOW, " LRS %d\n", scount);
    if (crs[KEYS] & 010000) {          /* V/I mode */
      crsl[GR2] = lrs(crsl[GR2], scount);
    } else {
      CLEARCL;
      utempa = crs[B] & 0x8000;        /* save B bit 1 */
      if (scount <= 31) {
	templ = (crs[A]<<16) | ((crs[B] & 0x7FFF)<<1);
	EXPCL(templ & BITMASK32(32-scount));
	templ = templ >> (scount+1);
	crs[A] = templ >> 15;
	crs[B] = (templ & 0x7FFF) | utempa;
      } else if (crs[A] & 0x8000) {
	*(int *)(crs+A) = 0xFFFF7FFF | utempa;
	SETCL;
      } else {
	*(int *)(crs+A) = utempa;
      }
    }
    break;

  case 00200: /* LRR */
    TRACE(T_FLOW, " LRR %d\n", scount);
    crsl[GR2] = lrr(crsl[GR2], scount);
    break;

  case 00300: 
    switch (inst) {

    case 0040310: /* SSSN */
      sssn();
      break;

    case 0040300: /* DRN */
    case 0040301: /* DRNP */
    case 0040302: /* DRNZ */
    case 0040303: /* FRNP */
    case 0040320: /* FRNM */
    case 0040321: /* FRNZ */
      TRACE(T_FLOW, " DRNx/FRNx(V) UII\n");
      fault(UIIFAULT, RPL, RP);
      break;

    default:
      goto badshift;
    }
    break;

  case 00400: /* ARL */
    TRACE(T_FLOW, " ARL %d\n", scount);
    crs[A] = arl(crs[A], scount);
    break;

  case 00500: /* ARS */
    TRACE(T_FLOW, " ARS %d\n", scount);
    crs[A] = ars(crs[A], scount);
    break;

  case 00600: /* ARR */
    TRACE(T_FLOW, " ARR %d\n", scount);
    crs[A] = arr(crs[A], scount);
    break;

  case 01000: /* LLL */
    TRACE(T_FLOW, " LLL %d\n", scount);
    crsl[GR2] = lll(crsl[GR2], scount);
    break;

  case 01100: /* LLS (different in R/V modes) */
    TRACE(T_FLOW, " LLS %d\n", scount);
    if (crs[KEYS] & 010000)                /* V/I mode */
      crsl[GR2] = lls(crsl[GR2], scount);
    else {
      CLEARCL;
      utempa = crs[B] & 0x8000;            /* save B bit 1 */
      if (scount < 31) {
	utempl = (crs[A]<<16) | ((crs[B] & 0x7FFF)<<1);
	templ2 = 0x80000000;
	templ2 = templ2 >> scount;         /* create mask */
	templ2 = templ2 & utempl;          /* grab bits */
	templ2 = templ2 >> (31-scount);    /* sign extend them */
	EXPCL(!(templ2 == -1 || templ2 == 0));
	//printf(" before: A=%x, B=%x, utempl=%x, ", crs[A], crs[B], utempl);
	utempl = utempl << scount;
	crs[A] = utempl >> 16;
	crs[B] = ((utempl >> 1) & 0x7FFF) | utempa;
	//printf(" after: A=%x, B=%x, utempl=%x\n", crs[A], crs[B], utempl);
      } else {
	EXPCL(*(unsigned int *)(crs+A) != 0);
	*(unsigned int *)(crs+A) = utempa;
      }
    }
    if ((crs[KEYS] & 0100400) == 0100400)
      mathexception('i', FC_INT_OFLOW, 0);
    break;

  case 01200: /* LLR */
    TRACE(T_FLOW, " LLR %d\n", scount);
    crsl[GR2] = llr(crsl[GR2], scount);
    break;

  case 01400: /* ALL */
    TRACE(T_FLOW, " ALL %d\n", scount);
    crs[A] = all(crs[A], scount);
    break;

  case 01500: /* ALS */
    TRACE(T_FLOW, " ALS %d\n", scount);
    crs[A] = als(crs[A], scount);
    if ((crs[KEYS] & 0100400) == 0100400)
      mathexception('i', FC_INT_OFLOW, 0);
    break;

  case 01600: /* ALR */
    TRACE(T_FLOW, " ALR %d\n", scount);
    crs[A] = alr(crs[A], scount);
    break;

  default:
badshift:
    printf("emulator warning: unrecognized class 1 (shift) generic instruction %06o at %o/%o\n", inst, RPH, RPL);
    TRACE(T_FLOW, " unrecognized shift instruction!: %o\n", inst);
  }
  goto fetch;

  /* class 2 generic instructions (skip group) */


d_nopskp:  /* 0101000 */
  TRACE(T_FLOW, " NOP-SKP\n");
  goto fetch;

d_skp:  /* 0100000 */
  TRACE(T_FLOW, " SKP\n");
  INCRP;
  goto fetch;

d_smi:  /* 0101400 */
  TRACE(T_FLOW, " SMI/SLT\n");
  if (*(short *)(crs+A) < 0)
    INCRP;
  goto fetch;

d_spl:  /* 0100400 */
  TRACE(T_FLOW, " SPL/SGE\n");
  if (*(short *)(crs+A) >= 0)
    INCRP;
  goto fetch;

d_sln:  /* 0101100 */
  TRACE(T_FLOW, " SLN\n");
  if (crs[A] & 1)
    INCRP;
  goto fetch;

d_slz:  /* 0100100 */
  TRACE(T_FLOW, " SLZ\n");
  if (!(crs[A] & 1))
    INCRP;
  goto fetch;

d_snz:  /* 0101040 */
  TRACE(T_FLOW, " SNZ/SNE\n");
  if (crs[A] != 0)
    INCRP;
  goto fetch;

d_sze:  /* 0100040 */
  TRACE(T_FLOW, " SZE/SEQ\n");
  if (crs[A] == 0)
    INCRP;
  goto fetch;

d_sle:  /* 0101220 */
  TRACE(T_FLOW, " SLE\n");
  if (*(short *)(crs+A) <= 0)
    INCRP;
  goto fetch;

d_sgt:  /* 0100220 */
  TRACE(T_FLOW, " SGT\n");
  if (*(short *)(crs+A) > 0)
    INCRP;
  goto fetch;

d_ssc:  /* 0101001 */
  TRACE(T_FLOW, " SSC\n");
  if (crs[KEYS] & 0100000)
    INCRP;
  goto fetch;

d_src:  /* 0100001 */
  TRACE(T_FLOW, " SRC\n");
  if (!(crs[KEYS] & 0100000))
    INCRP;
  goto fetch;

d_sar:  /* 0100260 - 0100277 */
  m = (inst & 017)+1;
  TRACE(T_FLOW, " SAR %d\n", m);
  if (!(crs[A] & BITMASK16(m)))
    INCRP;
  goto fetch;

d_sas:  /* 0101260 - 0101277 */
  m = (inst & 017)+1;
  TRACE(T_FLOW, " SAS %d\n", m);
  if (crs[A] & BITMASK16(m))
    INCRP;
  goto fetch;

d_snr:  /* 0100240 - 0100257 */
  m = (inst & 017)+1;
  TRACE(T_FLOW, " SNR %d\n", m);
  RESTRICT();
  if (!(sswitch & BITMASK16(m)))
    INCRP;
  goto fetch;

d_sns:  /* 0101240 - 0101257 */
  m = (inst & 017)+1;
  TRACE(T_FLOW, " SNS %d\n", m);
  RESTRICT();
  if (sswitch & BITMASK16(m))
    INCRP;
  goto fetch;

d_smcr:  /* 0100200 */
  TRACE(T_FLOW, " SMCR\n");
  RESTRICT();
  INCRP;
  goto fetch;

d_smcs:  /* 0101200 */
  TRACE(T_FLOW, " SMCS\n");
  RESTRICT();
  goto fetch;

d_badgen:
  TRACEA(" unrecognized generic instruction!\n");
  printf("em: #%d %o/%o: Unrecognized generic instruction '%o!\n", gvp->instcount, RPH, RPL, inst);
  //gvp->traceflags = ~TB_MAP;
  fault(UIIFAULT, RPL, RP);
  fatal(NULL);


imode:

    /* branch and register generic instructions don't have ea, so they
       are tested outside the main switch, before an ea is computed */

    opcode = inst >> 10;
    dr = (inst >> 7) & 7;

    if (opcode == 010) {               /* register branch */
      brop = inst & 0177;
      switch (brop) {
      case 0100:
	TRACE(T_FLOW, " BRLE\n");
	SETCC_32(crsl[dr]);
	BCLE;
	break;

      case 0101:
	TRACE(T_FLOW, " BRGT\n");
	SETCC_32(crsl[dr]);
	BCGT;
	break;

      case 0102:
	TRACE(T_FLOW, " BREQ\n");
	SETCC_32(crsl[dr]);
	BCEQ;
	break;

      case 0103:
	TRACE(T_FLOW, " BRNE\n");
	SETCC_32(crsl[dr]);
	BCNE;
	break;

      case 0104:
	TRACE(T_FLOW, " BRLT\n");
	SETCC_32(crsl[dr]);
	BCLT;
	break;

      case 0105:
	TRACE(T_FLOW, " BRGE\n");
	SETCC_32(crsl[dr]);
	BCGE;
	break;

      case 0110:
	TRACE(T_FLOW, " BHLE\n");
	SETCC_16(crs[dr*2]);
	BCLE;
	break;

      case 0111:
	TRACE(T_FLOW, " BHGT\n");
	SETCC_16(crs[dr*2]);
	BCGT;
	break;

      case 0112:
	TRACE(T_FLOW, " BHEQ\n");
	SETCC_16(crs[dr*2]);
	BCEQ;
	break;

      case 0113:
	TRACE(T_FLOW, " BHNE\n");
	SETCC_16(crs[dr*2]);
	BCNE;
	break;

      case 0114:
	TRACE(T_FLOW, " BHLT\n");
	SETCC_16(crs[dr*2]);
	BCLT;
	break;

      case 0115:
	TRACE(T_FLOW, " BHGE\n");
	SETCC_16(crs[dr*2]);
	BCGE;
	break;

      case 0120:
	TRACE(T_FLOW, " BFLE\n");
	SETCC_32(crsl[FAC0+dr]);
	BCLE;
	break;

      case 0121:
	TRACE(T_FLOW, " BFGT\n");
	SETCC_32(crsl[FAC0+dr]);
	BCGT;
	break;

      case 0122:
	TRACE(T_FLOW, " BFEQ\n");
	SETCC_32(crsl[FAC0+dr]);
	BCEQ;
	break;

      case 0123:
	TRACE(T_FLOW, " BFNE\n");
	SETCC_32(crsl[FAC0+dr]);
	BCNE;
	break;

      case 0124:
	TRACE(T_FLOW, " BFLT\n");
	SETCC_32(crsl[FAC0+dr]);
	BCLT;
	break;

      case 0125:
	TRACE(T_FLOW, " BFGE\n");
	SETCC_32(crsl[FAC0+dr]);
	BCGE;
	break;

      case 0130:
	TRACE(T_FLOW, " BRI1\n");
	crsl[dr]++;
	BRNE(dr);
	break;

      case 0131:
	TRACE(T_FLOW, " BRI2\n");
	crsl[dr] += 2;
	BRNE(dr);
	break;

      case 0132:
	TRACE(T_FLOW, " BRI4\n");
	crsl[dr] += 4;
	BRNE(dr);
	break;

      case 0134:
	TRACE(T_FLOW, " BRD1\n");
	crsl[dr]--;
	BRNE(dr);
	break;

      case 0135:
	TRACE(T_FLOW, " BRD2\n");
	crsl[dr] -= 2;
	BRNE(dr);
	break;

      case 0136:
	TRACE(T_FLOW, " BRD4\n");
	crsl[dr] -= 4;
	BRNE(dr);
	break;

      case 0140:
	TRACE(T_FLOW, " BHI1\n");
	crs[dr*2]++;
	BHNE(dr);
	break;

      case 0141:
	TRACE(T_FLOW, " BHI2\n");
	crs[dr*2] += 2;
	BHNE(dr);
	break;

      case 0142:
	TRACE(T_FLOW, " BHI4\n");
	crs[dr*2] += 4;
	BHNE(dr);
	break;

      case 0144:
	TRACE(T_FLOW, " BHD1\n");
	crs[dr*2]--;
	BHNE(dr);
	break;

      case 0145:
	TRACE(T_FLOW, " BHD2\n");
	crs[dr*2] -= 2;
	BHNE(dr);
	break;

      case 0146:
	TRACE(T_FLOW, " BHD4\n");
	crs[dr*2] -= 4;
	BHNE(dr);
	break;

      default:
	if (brop <= 037) {
	  TRACE(T_FLOW, " BRBS\n");
	  if (crsl[dr] & BITMASK32(brop+1))
	    RPL = iget16(RP);
	  else
	    INCRP;
	} else if (brop <= 077) {
	  TRACE(T_FLOW, " BRBR\n");
	  if (crsl[dr] & BITMASK32(brop-040+1))
	    INCRP;
	  else
	    RPL = iget16(RP);
	} else
	  fault(UIIFAULT, RPL, RP);
      }
      goto fetch;
    }

    if (opcode == 030) {       /* register generic */
      switch (inst & 0177) {
      case 0134:
	TRACE(T_FLOW, " ABQ\n");
	ea = apea(NULL);
	if (abq(ea, crs[dr*2], RP))
	  CLEAREQ;
	else
	  SETEQ;
	break;

      case 0014:
	TRACE(T_FLOW, " ADLR\n");
	adlr(dr);
	break;

      case 0161:
	TRACE(T_FLOW, " ARFA 0\n");
	arfa(0, crsl[dr]);
	break;

      case 0171:
	TRACE(T_FLOW, " ARFA 1\n");
	arfa(1, crsl[dr]);
	break;

      case 0135:
	TRACE(T_FLOW, " ATQ\n");
	ea = apea(NULL);
	if (atq(ea, crs[dr*2], RP))
	  CLEAREQ;
	else
	  SETEQ;
	break;

      case 0026:
	TRACE(T_FLOW, " CGT\n");
	utempa = iget16(RP);              /* get number of words */
	if (1 <= crs[dr*2] && crs[dr*2] < utempa)
	  RPL = iget16(INCVA(RP,crs[dr*2]));
	else
	  RPL += utempa;
	break;

      case 0040:
	TRACE(T_FLOW, " CHS\n");
	crsl[dr] ^= 0x80000000;
	break;

      case 045:
	TRACE(T_FLOW, " CMH\n");
	crs[dr*2] = ~crs[dr*2];
	break;

      case 044:
	TRACE(T_FLOW, " CMR\n");
	crsl[dr] = ~crsl[dr];
	break;

      case 0056:
	TRACE(T_FLOW, " CR\n");
	crsl[dr] = 0;
	break;

      case 0062:
	TRACE(T_FLOW, " CRBL\n");
	crsl[dr] &= 0x00FFFFFF;
	break;

      case 0063:
	TRACE(T_FLOW, " CRBR\n");
	crsl[dr] &= 0xFF00FFFF;
	break;

      case 0054:
	TRACE(T_FLOW, " CRHL\n");
	crsl[dr] &= 0x0000FFFF;
	break;

      case 0055:
	TRACE(T_FLOW, " CRHR\n");
	crsl[dr] &= 0xFFFF0000;
	break;

      case 0041:
	TRACE(T_FLOW, " CSR\n");
	crs[KEYS] = (crs[KEYS] & 0x7FFF) | (crs[dr*2] & 0x8000);
	crsl[dr] &= 0x7FFFFFFF;
	break;

      case 0106:
	TRACE(T_FLOW, " DBLE\n");
	crsl[FAC0+dr+1] &= 0x0000FFFF;
	break;

      case 0160:
	TRACE(T_FLOW, " DCP\n");
#if 0
	utempl = EACP(crsl[dr]);
	utempl--;
	crsl[dr] = CPEA(crsl[dr], utempl);
#else
	crsl[dr] ^= EXTMASK32;
	if (crsl[dr] & EXTMASK32)
	  crsl[dr]--;
#endif
	break;

      case 0144:
	TRACE(T_FLOW, " DFCM\n");
	dfcm(crsl+FAC0+dr);
	break;

      case 0130:
	TRACE(T_FLOW, " DH1\n");
	add16(crs+dr*2, 0xFFFF, 0, 0);
	break;

      case 0131:
	TRACE(T_FLOW, " DH2\n");
	add16(crs+dr*2, 0xFFFE, 0, 0);
	break;

      case 0124:
	TRACE(T_FLOW, " DR1\n");
	add32(crsl+dr, 0xFFFFFFFF, 0, 0);
	break;

      case 0125:
	TRACE(T_FLOW, " DR2\n");
	add32(crsl+dr, 0xFFFFFFFE, 0, 0);
	break;

      case 0100:
	TRACE(T_FLOW, " FCM\n");
	dfcm(crsl+FAC0+dr);
	break;

      case 0105:
	TRACE(T_FLOW, " FLT 0\n");
	*(double *)(crsl+FAC0) = fltl(crsl[dr]);
	break;

      case 0115:
	TRACE(T_FLOW, " FLT 1\n");
	*(double *)(crsl+FAC1) = fltl(crsl[dr]);
	break;

      case 0102:
	TRACE(T_FLOW, " FLTH 0\n");
	*(double *)(crsl+FAC0) = fltl(*(short *)(crs+dr*2));
	break;

      case 0112:
	TRACE(T_FLOW, " FLTH 1\n");
	*(double *)(crsl+FAC1) = fltl(*(short *)(crs+dr*2));
	break;

      case 0107:
	TRACE(T_FLOW, " FRN\n");
	CLEARC;
	frn(crsl+FAC0+dr);
	break;

      case 0146:  /* I-mode FRNM */
      case 0145:  /* I-mode FRNP */
      case 0147:  /* I-mode FRNZ */
	TRACE(T_FLOW, " FRNx(I) UII\n");
	fault(UIIFAULT, RPL, RP);
	break;

      case 0065:
	TRACE(T_FLOW, " ICBL\n");
	crs[dr*2] = crs[dr*2]>>8;
	break;

      case 0066:
	TRACE(T_FLOW, " ICBR\n");
	crs[dr*2] = crs[dr*2]<<8;
	break;

      case 0060:
	TRACE(T_FLOW, " ICHL\n");
	crsl[dr] = crsl[dr]>>16;
	break;

      case 0061:
	TRACE(T_FLOW, " ICHR\n");
	crsl[dr] = crsl[dr]<<16;
	break;

      case 0167:
	TRACE(T_FLOW, " ICP\n");
#if 0
	utempl = EACP(crsl[dr]);
	utempl++;
	crsl[dr] = CPEA(crsl[dr], utempl);
#else
	crsl[dr] ^= EXTMASK32;
	if ((crsl[dr] & EXTMASK32) == 0)
	  crsl[dr]++;
#endif
	break;

      case 0126:
	TRACE(T_FLOW, " IH1\n");
	add16(crs+dr*2, 1, 0, 0);
	break;

      case 0127:
	TRACE(T_FLOW, " IH2\n");
	add16(crs+dr*2, 2, 0, 0);
	break;

      case 0070:
	TRACE(T_FLOW, " INK\n");
	crs[dr*2] = crs[KEYS];    /* IXX: says to read S register? */
	break;

      case 0103:
	TRACE(T_FLOW, " INT 0\n");
	if (prieee8(crsl+FAC0, &tempd) && -2147483648.0 <= tempd && tempd <= 2147483647.0) {
	  *(int *)(crsl+dr) = tempd;
	  CLEARC;
	} else
	  mathexception('f', FC_INT_CONV, ea);
	break;

      case 0113:
	TRACE(T_FLOW, " INT 1\n");
	if (prieee8(crsl+FAC1, &tempd) && -2147483648.0 <= tempd && tempd <= 2147483647.0) {
	  *(int *)(crsl+dr) = tempd;
	  CLEARC;
	} else
	  mathexception('f', FC_INT_CONV, ea);
	break;

      case 0101:
	TRACE(T_FLOW, " INTH 0\n");
	if (prieee8(crsl+FAC0, &tempd) && -32768.0 <= tempd && tempd <= 32767.0) {
	  *(short *)(crs+dr*2) = tempd;
	  CLEARC;
	} else
	  mathexception('f', FC_INT_CONV, ea);
	break;

      case 0111:
	TRACE(T_FLOW, " INTH 1\n");
	if (prieee8(crsl+FAC1, &tempd) && -32768.0 <= tempd && tempd <= 32767.0) {
	  *(short *)(crs+dr*2) = tempd;
	  CLEARC;
	} else
	  mathexception('f', FC_INT_CONV, ea);
	break;

      case 0122:
	TRACE(T_FLOW, " IR1\n");
	add32(crsl+dr, 1, 0, 0);
	break;

      case 0123:
	TRACE(T_FLOW, " IR2\n");
	add32(crsl+dr, 2, 0, 0);
	break;

      case 0064:
	TRACE(T_FLOW, " IRB\n");
	crs[dr*2] = (crs[dr*2]>>8) | (crs[dr*2]<<8);
	break;

      case 0057:
	TRACE(T_FLOW, " IRH\n");
	crsl[dr] = (crsl[dr]>>16) | (crsl[dr]<<16);
	break;

      case 0153:
	TRACE(T_FLOW, " LCEQ\n");
	crs[dr*2] = LCEQ;
	break;

      case 0154:
	TRACE(T_FLOW, " LCGE\n");
	crs[dr*2] = LCGE;
	break;

      case 0155:
	TRACE(T_FLOW, " LCGT\n");
	crs[dr*2] = LCGT;
	break;

      case 0151:
	TRACE(T_FLOW, " LCLE\n");
	crs[dr*2] = LCLE;
	break;

      case 0150:
	TRACE(T_FLOW, " LCLT\n");
	crs[dr*2] = LCLT;
	break;

      case 0152:
	TRACE(T_FLOW, " LCNE\n");
	crs[dr*2] = LCNE;
	break;

      case 0162:
	TRACE(T_FLOW, " LDC 0\n");
	crs[dr*2] = ldc(0, crs[dr*2]);
	break;

      case 0172:
	TRACE(T_FLOW, " LDC 1\n");
	crs[dr*2] = ldc(1, crs[dr*2]);
	break;

      case 0003:
	TRACE(T_FLOW, " LEQ\n");
	SETCC_32(crsl[dr]);
	crs[dr*2] = LCEQ;
	break;

      case 0016:
	TRACE(T_FLOW, " LF\n");
	crs[dr*2] = 0;
	break;

      case 0023:
	TRACE(T_FLOW, " LFEQ 0\n");
	SETCC_32(crsl[FAC0]);
	crs[dr*2] = LCEQ;
	break;

      case 0033:
	TRACE(T_FLOW, " LFEQ 1\n");
	SETCC_32(crsl[FAC1]);
	crs[dr*2] = LCEQ;
	break;

      case 0024:
	TRACE(T_FLOW, " LFGE 0\n");
	SETCC_32(crsl[FAC0]);
	crs[dr*2] = LCGE;
	break;

      case 0034:
	TRACE(T_FLOW, " LFGE 1\n");
	SETCC_32(crsl[FAC1]);
	crs[dr*2] = LCGE;
	break;

      case 0025:
	TRACE(T_FLOW, " LFGT 0\n");
	SETCC_32(crsl[FAC0]);
	crs[dr*2] = LCGT;
	break;

      case 0035:
	TRACE(T_FLOW, " LFGT 1\n");
	SETCC_32(crsl[FAC1]);
	crs[dr*2] = LCGT;
	break;

      case 0021:
	TRACE(T_FLOW, " LFLE 0\n");
	SETCC_32(crsl[FAC0]);
	crs[dr*2] = LCLE;
	break;

      case 0031:
	TRACE(T_FLOW, " LFLE 1\n");
	SETCC_32(crsl[FAC1]);
	crs[dr*2] = LCLE;
	break;

      case 0020:
	TRACE(T_FLOW, " LFLT 0\n");
	SETCC_32(crsl[FAC0]);
	crs[dr*2] = LCLT;
	break;

      case 0030:
	TRACE(T_FLOW, " LFLT 1\n");
	SETCC_32(crsl[FAC1]);
	crs[dr*2] = LCLT;
	break;

      case 0022:
	TRACE(T_FLOW, " LFNE 0\n");
	SETCC_32(crsl[FAC0]);
	crs[dr*2] = LCNE;
	break;

      case 0032:
	TRACE(T_FLOW, " LFNE 1\n");
	SETCC_32(crsl[FAC1]);
	crs[dr*2] = LCNE;
	break;

      case 0004:
	TRACE(T_FLOW, " LGE/LHGE\n");
	SETCC_32(crsl[dr]);
	crs[dr*2] = LCGE;
	break;

      case 0005:
	TRACE(T_FLOW, " LGT\n");
	SETCC_32(crsl[dr]);
	crs[dr*2] = LCGT;
	break;

      case 0013:
	TRACE(T_FLOW, " LHEQ\n");
	SETCC_16(crs[dr*2]);
	crs[dr*2] = LCEQ;
	break;

      case 0015:
	TRACE(T_FLOW, " LHGT\n");
	SETCC_16(crs[dr*2]);
	crs[dr*2] = LCGT;
	break;

      case 0011:
	TRACE(T_FLOW, " LHLE\n");
	SETCC_16(crs[dr*2]);
	crs[dr*2] = LCLE;
	break;

      case 0000:
	TRACE(T_FLOW, " LLT/LHLT\n");
	SETCC_32(crsl[dr]);
	crs[dr*2] = LCLT;
	break;

      case 0012:
	TRACE(T_FLOW, " LHNE\n");
	SETCC_16(crs[dr*2]);
	crs[dr*2] = LCNE;
	break;

      case 0001:
	TRACE(T_FLOW, " LLE\n");
	SETCC_32(crsl[dr]);
	crs[dr*2] = LCLE;
	break;

      case 0002:
	TRACE(T_FLOW, " LNE\n");
	SETCC_32(crsl[dr]);
	crs[dr*2] = LCNE;
	break;

      case 0017:
	TRACE(T_FLOW, " LT\n");
	crs[dr*2] = 1;
	break;

      case 0071:
	TRACE(T_FLOW, " OTK\n");
	newkeys(crs[dr*2] & 0177770);
	gvp->inhcount = 1;
	break;

      case 0052:
	TRACE(T_FLOW, " PID\n");
	*(long long *)(crsl+dr) = *(int *)(crsl+dr);
	break;

      case 0053:
	TRACE(T_FLOW, " PIDH\n");
	*(int *)(crsl+dr) = *(short *)(crsl+dr);
	break;

      case 0050:
	TRACE(T_FLOW, " PIM\n");
	dr &= 6;                    /* force dr to be even */
	utempl = crsl[dr];
	crsl[dr] = crsl[dr+1];
	if (((utempl ^ crsl[dr+1]) & 0x80000000) || (utempl != 0 && ~utempl != 0))
	  mathexception('i', FC_INT_OFLOW, 0);
	else
	  CLEARC;
	break;

      case 0051:
	TRACE(T_FLOW, " PIMH\n");
	templ = crsl[dr];
	/* NOTE: PIMH could be implemented as a left shift, but Prime DIAG
	   tests require a swap - hence the "or" below */
	crsl[dr] = (crsl[dr] << 16) | (crsl[dr] >> 16);
	/* check that bits 1-16 were equal to bit 17 before PIMH */
	templ2 = (templ << 16) >> 16;
	if (templ2 != templ)
	  mathexception('i', FC_INT_OFLOW, 0);
	else
	  CLEARC;
	break;

      case 0133:
	TRACE(T_FLOW, " RBQ\n");
	ea = apea(NULL);
	if (rbq(ea, crs+dr*2, RP))
	  CLEAREQ;
	else
	  SETEQ;
	break;

      case 0132:
	TRACE(T_FLOW, " RTQ\n");
	ea = apea(NULL);
	if (rtq(ea,crs+dr*2,RP))
	  CLEAREQ;
	else
	  SETEQ;
	break;

      case 0076:
	TRACE(T_FLOW, " SHL1\n");
	if (crs[dr*2] & 0x8000)
	  SETCL;
	else
	  CLEARCL;
	crs[dr*2] = crs[dr*2] << 1;
	break;

      case 0077:
	TRACE(T_FLOW, " SHL2\n");
	if (crs[dr*2] & 0x4000)
	  SETCL;
	else
	  CLEARCL;
	crs[dr*2] = crs[dr*2] << 2;
	break;

      case 0120:
	TRACE(T_FLOW, " SHR1\n");
	if (crs[dr*2] & 0x0001)
	  SETCL;
	else
	  CLEARCL;
	crs[dr*2] = crs[dr*2] >> 1;
	break;

      case 0121:
	TRACE(T_FLOW, " SHR2\n");
	if (crs[dr*2] & 0x0002)
	  SETCL;
	else
	  CLEARCL;
	crs[dr*2] = crs[dr*2] >> 2;
	break;

      case 0072:
	TRACE(T_FLOW, " SL1\n");
	if (crsl[dr] & 0x80000000)
	  SETCL;
	else
	  CLEARCL;
	crsl[dr] = crsl[dr] << 1;
	break;

      case 0073:
	TRACE(T_FLOW, " SL2\n");
	if (crsl[dr] & 0x40000000)
	  SETCL;
	else
	  CLEARCL;
	crsl[dr] = crsl[dr] << 2;
	break;

      case 0074:
	TRACE(T_FLOW, " SR1\n");
	if (crsl[dr] & 0x00000001)
	  SETCL;
	else
	  CLEARCL;
	crsl[dr] = crsl[dr] >> 1;
	break;

      case 0075:
	TRACE(T_FLOW, " SR2\n");
	if (crsl[dr] & 0x00000002)
	  SETCL;
	else
	  CLEARCL;
	crsl[dr] = crsl[dr] >> 2;
	break;

      case 0042:
	TRACE(T_FLOW, " SSM\n");
	crsl[dr] |= 0x80000000;
	break;

      case 0043:
	TRACE(T_FLOW, " SSP\n");
	crsl[dr] &= 0x7FFFFFFF;
	break;

      case 0166:
	TRACE(T_FLOW, " STC 0\n");
	stc(0, crs[dr*2]);
	break;

      case 0176:
	TRACE(T_FLOW, " STC 1\n");
	stc(1, crs[dr*2]);
	break;

      case 0137:
	TRACE(T_FLOW, " STCD\n");
	ea = apea(NULL);
	if (get32(ea) == crsl[dr+1]) {
	  put32(crsl[dr], ea);
	  SETEQ;
	} else 
	  CLEAREQ;
	break;

      case 0136:
	TRACE(T_FLOW, " STCH\n");
	ea = apea(NULL);
	if (get16(ea) == (crs[dr*2+1])) {
	  put16(crs[dr*2], ea);
	  SETEQ;
	} else 
	  CLEAREQ;
	break;

      case 0027:
	TRACE(T_FLOW, " STEX\n");
	*(ea_t *)(crsl+dr) = stex(crsl[dr]);
	break;

      case 0046:
	TRACE(T_FLOW, " TC\n");
	tcr(crsl+dr);
	break;

      case 0047:
	TRACE(T_FLOW, " TCH\n");
	tch(crs+dr*2);
	break;

      case 0170:
	TRACE(T_FLOW, " TCNP\n");
	if ((crsl[dr] & 0x1FFFFFFF) == 0)
	  SETEQ;
	else
	  CLEAREQ;
	break;

      case 0163:
	TRACE(T_FLOW, " TFLR 0\n");
	crsl[dr] = GETFLR(0);
	break;

      case 0173:
	TRACE(T_FLOW, " TFLR 1\n");
	crsl[dr] = GETFLR(1);
	break;

      case 0165:
	TRACE(T_FLOW, " TRFL 0\n");
	PUTFLR(0, crsl[dr]);
	break;

      case 0175:
	TRACE(T_FLOW, " TRFL 1\n");
	PUTFLR(1, crsl[dr]);
	break;


      case 0104:
	TRACE(T_FLOW, " TSTQ\n");
	ea = apea(NULL);
	crs[dr*2] = tstq(ea);
	SETCC_16(crs[dr*2]);
	break;

      default:
	warn("IXX 030");
	fault(ILLINSTFAULT, RPL, RP);
      }
      goto fetch;
    }

    ea = ea32i(earp, inst, &immu32, &immu64);

    switch (opcode) {

    case 000:
      /* this should have been handled already! */
      fatal("I-mode generic class 0?");

    case 001:
      TRACE(T_FLOW, " L\n");
      if (*(int *)&ea < 0)
	crsl[dr] = immu32;
      else
	crsl[dr] = get32(ea);
      goto fetch;

    case 002:
      TRACE(T_FLOW, " A\n");
      if (*(int *)&ea < 0)
	utempl = immu32;
      else
        utempl = get32(ea);
      add32(crsl+dr, utempl, 0, ea);
      goto fetch;

    case 003:
      TRACE(T_FLOW, " N\n");
      if (*(int *)&ea < 0)
	crsl[dr] &= immu32;
      else
        crsl[dr] &= get32(ea);
      goto fetch;

    case 004:
      TRACE(T_FLOW, " LHL1\n");
      if (*(int *)&ea < 0)
	crs[dr*2] = (immu32 >> 16) << 1;
      else
	crs[dr*2] = get16(ea) << 1;
      goto fetch;

    case 005:
      TRACE(T_FLOW, " SHL\n");
      scount = -ea & 077;
      if (scount == 0)
	scount = 0100;
      switch ((ea >> 14) & 3) {
      case 0:
	crsl[dr] = lll(crsl[dr], scount);
	break;
      case 1:
	crs[dr*2] = all(crs[dr*2], scount);
	break;
      case 2:
	crsl[dr] = lrl(crsl[dr], scount);
	break;
      case 3:
	crs[dr*2] = arl(crs[dr*2], scount);
	break;
      default:
	warn("I-mode SHL switch?");
	fault(ILLINSTFAULT, RPL, RP);
      }
      goto fetch;

    case 006:  /* Special MR FP format */
      /* FL, DFL, FC, DFC */
      switch (dr) {
      case 0:
      case 2:
	dr &= 2;
	TRACE(T_INST, " FL\n");
	if (*(int *)&ea < 0)
	  *(long long *)(crsl+FAC0+dr) = immu64;
	else {
	  utempl = get32(ea);
	  crsl[FAC0+dr]   = utempl & 0xFFFFFF00;
	  crsl[FAC0+dr+1] = utempl & 0x000000FF;
	}
	break;

      case 1:
      case 3:
	dr &= 2;
	TRACE(T_INST, " DFL\n");
	if (*(int *)&ea < 0)
	  *(long long *)(crsl+FAC0+dr) = immu64;
	else
	  *(long long *)(crsl+FAC0+dr) = get64(ea);
	break;

      case 4:
      case 6:
	dr &= 2;
	TRACE(T_INST, " FC\n");
	if (*(int *)&ea < 0)
	  utempl = ((immu64 >> 32) & 0xffffff00) | (immu64 & 0xff);
	else
	  utempl = get32(ea);
	fcs(crsl+FAC0+dr, utempl);
	break;

      case 5:
      case 7:
	dr &= 2;
	TRACE(T_INST, " DFC\n");
	if (*(int *)&ea >= 0)
	  immu64 = get64(ea);
	dfcs(crsl+FAC0+dr, immu64);
      break;

      default:
	warn("I-mode 006 switch?");
	fault(ILLINSTFAULT, RPL, RP);
      }
      goto fetch;

    case 007:
      warn("I-mode opcode 007?");
      fault(ILLINSTFAULT, RPL, RP);

    case 010:                            /* register generic branch */
      /* this should have been handled already! */
      fatal("I-mode RGBR?");

    case 011:
      TRACE(T_FLOW, " LH\n");
      if (*(int *)&ea < 0) {
	TRACE(T_INST, " ea=%x, immu32=%x, crsl[%d]=%x\n", ea, immu32, dr, crsl[dr]);
	crs[dr*2] = immu32 >> 16;
      } else
	crs[dr*2] = get16(ea);
      goto fetch;

    case 012:
      TRACE(T_FLOW, " AH\n");
      if (*(int *)&ea < 0)
	utempa = (immu32 >> 16);
      else
        utempa = get16(ea);
      add16(crs+dr*2, utempa, 0, ea);
      goto fetch;

    case 013:
      TRACE(T_FLOW, " NH\n");
      if (*(int *)&ea < 0)
	crs[dr*2] &= (immu32 >> 16);
      else
        crs[dr*2] &= get16(ea);
      goto fetch;

    case 014:
      TRACE(T_FLOW, " LHL2\n");
      if (*(int *)&ea < 0)
	crs[dr*2] = (immu32 >> 16) << 2;
      else
	crs[dr*2] = get16(ea) << 2;
      goto fetch;

    case 015:
      TRACE(T_FLOW, " SHA\n");
      scount = -ea & 077;
      if (scount == 0)
	scount = 0100;
      switch ((ea >> 14) & 3) {
      case 0:
	crsl[dr] = lls(crsl[dr], scount);
	if ((crs[KEYS] & 0100400) == 0100400)
	  mathexception('i', FC_INT_OFLOW, 0);
	break;
      case 1:
	crs[dr*2] = als(crs[dr*2], scount);
	if ((crs[KEYS] & 0100400) == 0100400)
	  mathexception('i', FC_INT_OFLOW, 0);
	break;
      case 2:
	crsl[dr] = lrs(crsl[dr], scount);
	break;
      case 3:
	crs[dr*2] = ars(crs[dr*2], scount);
	break;
      default:
	fatal("SHA?");
      }
      goto fetch;

    case 016:  /* Special MR FP format */
      /* FST, DFST, FA, DFA */
      switch (dr) {
      case 0:
      case 2:
	dr &= 2;
	TRACE(T_INST, " FST\n");
	CLEARC;
	if (*(int *)&ea >= 0) {
	  if (crs[KEYS] & 010)
	    frn(crsl+FAC0+dr);
	  if ((crsl[FAC0+dr+1] & 0xFF00) == 0)
	    put32((crsl[FAC0+dr] & 0xFFFFFF00) | (crsl[FAC0+dr+1] & 0xFF), ea);
	  else
	    mathexception('f', FC_SFP_STORE, ea);
	} else {
	  warn("I-mode immediate FST?");
	  fault(ILLINSTFAULT, RPL, RP);
	}
	break;

      case 1:
      case 3:
	dr &= 2;
	TRACE(T_INST, " DFST\n");
	if (*(int *)&ea >= 0)
	  put64(*(long long *)(crsl+FAC0+dr), ea);
	else {
	  warn("I-mode immediate DFST?");
	  fault(ILLINSTFAULT, RPL, RP);
	}
	break;

      case 4:
      case 6:
	dr &= 2;
	TRACE(T_INST, " FA\n");
	CLEARC;
	if (*(int *)&ea >= 0) {
	  immu64 = get32(ea);
	  immu64 = ((immu64 << 32) & 0xffffff0000000000LL) | (immu64 & 0xff);
	}
	if (*(int *)&immu64)
	  if (*(int *)(crsl+FAC0+dr)) {
	    tempa1 = crsl[FAC0+dr+1] & 0xffff;
	    tempa2 = immu64 & 0xffff;
	    if (abs(tempa1-tempa2) < 48)
	      if (prieee8(crsl+FAC0+dr, &tempd1) && prieee8(&immu64, &tempd2)) {
		*(double *)(crsl+FAC0+dr) = ieeepr8(tempd1+tempd2);
		XCLEARC;   /* XXX: test overflow */
	      } else
		mathexception('f', FC_SFP_OFLOW, ea);
	  else if (tempa1 < tempa2)
	    *(long long *)(crsl+FAC0+dr) = immu64;
	} else
	    *(long long *)(crsl+FAC0+dr) = immu64;
	else if (*(int *)(crsl+FAC0+dr) == 0)
	  *(long long *)(crsl+FAC0+dr) = 0;
	break;

      case 5:
      case 7:
	dr &= 2;
	TRACE(T_INST, " DFA\n");
	CLEARC;
	if (*(int *)&ea >= 0)
	  immu64 = get64(ea);
	if (*(int *)&immu64)
	  if (*(int *)(crsl+FAC0+dr))
	    if (prieee8(crsl+FAC0+dr, &tempd1) && prieee8(&immu64, &tempd2)) {
	      *(double *)(crsl+FAC0+dr) = ieeepr8(tempd1+tempd2);
	      XCLEARC;   /* XXX: test overflow */
	    } else
	      mathexception('f', FC_DFP_OFLOW, ea);
	  else
	    *(long long *)(crsl+FAC0+dr) = immu64;
	else if (*(int *)(crsl+FAC0+dr) == 0)
	  *(long long *)(crsl+FAC0+dr) = 0;
	break;

      default:
	warn("I-mode 016 switch?");
	fault(ILLINSTFAULT, RPL, RP);
      }
      goto fetch;

    case 017:
      warn("I-mode opcode 017?");
      fault(ILLINSTFAULT, RPL, RP);

    case 020:
      /* this should have been handled already! */
      fatal("I-mode generic class 1?");

    case 021:
      TRACE(T_FLOW, " ST\n");
      if (*(int *)&ea < 0)
	crsl[(inst >> 2) & 7] = crsl[dr];
      else
	put32(crsl[dr],ea);
      goto fetch;

    case 022:
      TRACE(T_FLOW, " S\n");
      if (*(int *)&ea < 0)
	utempl = immu32;
      else
        utempl = get32(ea);
      add32(crsl+dr, ~utempl, 1, ea);
      goto fetch;

    case 023:
      TRACE(T_FLOW, " O\n");
      if (*(int *)&ea < 0)
	crsl[dr] |= immu32;
      else
        crsl[dr] |= get32(ea);
      goto fetch;

    case 024:
      TRACE(T_FLOW, " ROT\n");
      scount = -ea & 077;
      if (scount == 0)
	scount = 0100;
      switch ((ea >> 14) & 3) {
      case 0:
	crsl[dr] = llr(crsl[dr], scount);
	break;
      case 1:
	crs[dr*2] = alr(crs[dr*2], scount);
	break;
      case 2:
	crsl[dr] = lrr(crsl[dr], scount);
	break;
      case 3:
	crs[dr*2] = arr(crs[dr*2], scount);
	break;
      default:
	warn("I-mode ROT switch?");
	fault(ILLINSTFAULT, RPL, RP);
      }
      goto fetch;

    case 025:
      warn("I-mode opcode 025?");
      fault(ILLINSTFAULT, RPL, RP);

    case 026:  /* Special MR FP format */
      /* FS, DFS, FM, DFM */
      switch (dr) {
      case 0:
      case 2:
	dr &= 2;
	TRACE(T_INST, " FS\n");
	CLEARC;
	if (*(int *)&ea >= 0) {
	  immu64 = get32(ea);
	  immu64 = ((immu64 << 32) & 0xffffff0000000000LL) | (immu64 & 0xff);
	}
	if (*(int *)&immu64)
	  if (*(int *)(crsl+FAC0+dr)) {
	    tempa1 = crsl[FAC0+dr+1] & 0xffff;
	    tempa2 = immu64 & 0xffff;
	    if (abs(tempa1-tempa2) < 48)
	      if (prieee8(crsl+FAC0+dr, &tempd1) && prieee8(&immu64, &tempd2)) {
		*(double *)(crsl+FAC0+dr) = ieeepr8(tempd1-tempd2);
		XCLEARC;   /* XXX: test overflow */
	      } else
		mathexception('f', FC_SFP_OFLOW, ea);
	    else if (tempa1 < tempa2) {
	      *(long long *)(crsl+FAC0+dr) = immu64;
	      dfcm(crsl+FAC0+dr);
	    }
	  } else {
	    *(long long *)(crsl+FAC0+dr) = immu64;
	    dfcm(crsl+FAC0+dr);
	  }
	else if (*(int *)(crsl+FAC0+dr) == 0)
	  *(long long *)(crsl+FAC0+dr) = 0;
	break;

      case 1:
      case 3:
	dr &= 2;
	TRACE(T_INST, " DFS\n");
	CLEARC;
	if (*(int *)&ea >= 0)
	  immu64 = get64(ea);
	if (*(int *)&immu64)
	  if (*(int *)(crsl+FAC0+dr))
	    if (prieee8(crsl+FAC0+dr, &tempd1) && prieee8(&immu64, &tempd2)) {
	      *(double *)(crsl+FAC0+dr) = ieeepr8(tempd1-tempd2);
	      XCLEARC;   /* XXX: test overflow */
	    } else
	      mathexception('f', FC_DFP_OFLOW, ea);
	  else {
	    *(long long *)(crsl+FAC0+dr) = immu64;
	    dfcm(crsl+FAC0+dr);
	  }
	else if (*(int *)(crsl+FAC0+dr) == 0)
	  *(long long *)(crsl+FAC0+dr) = 0;
	break;

      case 4:
      case 6:
	dr &= 2;
	TRACE(T_INST, " FM\n");
	CLEARC;
	if (*(int *)(crsl+FAC0+dr)) {
	  if (*(int *)&ea >= 0) {
	    immu64 = get32(ea);
	    immu64 = ((immu64 << 32) & 0xffffff0000000000LL) | (immu64 & 0xff);
	  }
	  if (*(int *)&immu64)
	    if (prieee8(&immu64, &tempd2) && prieee8(crsl+FAC0+dr, &tempd1)) {
	      *(double *)(crsl+FAC0+dr) = ieeepr8(tempd1*tempd2);
	      XCLEARC;   /* XXX: test overflow */
	    } else
	      mathexception('f', FC_SFP_OFLOW, ea);
	  else            /* operand = 0.0: no multiply */
	    *(long long *)(crsl+FAC0+dr) = 0;
	} else            /* clean up (maybe) dirty zero */
	  *(long long *)(crsl+FAC0+dr) = 0;
	break;

      case 5:
      case 7:
	dr &= 2;
	TRACE(T_INST, " DFM\n");
	CLEARC;
	if (*(int *)(crsl+FAC0+dr)) {
	  if (*(int *)&ea >= 0)
	    immu64 = get64(ea);
	  if (*(int *)&immu64)
	    if (prieee8(&immu64, &tempd2) && prieee8(crsl+FAC0+dr, &tempd1)) {
	      *(double *)(crsl+FAC0+dr) = ieeepr8(tempd1*tempd2);
	      XCLEARC;   /* XXX: test overflow */
	    } else
	      mathexception('f', FC_DFP_OFLOW, ea);
	  else             /* operand = 0.0: no multiply */
	    *(long long *)(crsl+FAC0+dr) = 0;
	} else
	  *(long long *)(crsl+FAC0+dr) = 0;
	break;

      default:
	warn("I-mode 026 switch?");
	fault(ILLINSTFAULT, RPL, RP);
      }
      goto fetch;

    case 027:
      warn("I-mode opcode 027?");
      fault(ILLINSTFAULT, RPL, RP);

    case 030:  /* register generic */
      /* this should have been handled already! */
      fatal("I-mode RGEN?");

    case 031:
      TRACE(T_FLOW, " STH\n");
      if (*(int *)&ea < 0)
	crs[((inst >> 2) & 7)*2] = crs[dr*2];
      else
	put16(crs[dr*2], ea);
      goto fetch;

    case 032:
      TRACE(T_FLOW, " SH\n");
      if (*(int *)&ea < 0)
	utempa = (immu32 >> 16);
      else
        utempa = get16(ea);
      add16(crs+dr*2, ~utempa, 1, ea);
      goto fetch;

    case 033:
      TRACE(T_FLOW, " OH\n");
      if (*(int *)&ea < 0)
	crs[dr*2] |= (immu32 >> 16);
      else
        crs[dr*2] |= get16(ea);
      goto fetch;

    case 034:
      TRACE(T_FLOW, " EIO\n");
      pio(ea & 0xFFFF);
      goto fetch;

    case 035:
      TRACE(T_FLOW, " LHL3\n");
      if (*(int *)&ea < 0)
	crs[dr*2] = (immu32 >> 16) << 3;
      else
	crs[dr*2] = get16(ea) << 3;
      goto fetch;

    case 036:  /* Special MR FP format */
      /* FD, DFD, QFLD, QFST, QFSB, QFAD */
      switch (dr) {
      case 0:
      case 2:
	dr &= 2;
	TRACE(T_INST, " FD\n");
	CLEARC;
	if (*(int *)&ea >= 0) {
	  immu64 = get32(ea);
	  immu64 = ((immu64 << 32) & 0xffffff0000000000LL) | (immu64 & 0xff);
	}
	if (*(int *)&immu64)
	  if (*(int *)(crsl+FAC0+dr))
	    if (prieee8(&immu64, &tempd2) && prieee8(crsl+FAC0+dr, &tempd1)) {
	      *(double *)(crsl+FAC0+dr) = ieeepr8(tempd1/tempd2);
	      XCLEARC;   /* XXX: test overflow */
	    } else
	      mathexception('f', FC_SFP_OFLOW, ea);
	  else            /* operand = 0.0 */
	    *(long long *)(crsl+FAC0+dr) = 0;
	else            /* clean up (maybe) dirty zero */
	  mathexception('f', FC_SFP_ZDIV, ea);
	break;

      case 1:
      case 3:
	dr &= 2;
	TRACE(T_INST, " DFD\n");
	CLEARC;
	if (*(int *)&ea >= 0)
	  immu64 = get64(ea);
	if (*(int *)&immu64)
	  if (*(int *)(crsl+FAC0+dr))
	    if (prieee8(&immu64, &tempd2) && prieee8(crsl+FAC0+dr, &tempd1)) {
	      *(double *)(crsl+FAC0+dr) = ieeepr8(tempd1/tempd2);
	      XCLEARC;   /* XXX: test overflow */
	    } else
	      mathexception('f', FC_DFP_OFLOW, ea);
	  else
	    *(long long *)(crsl+FAC0+dr) = 0;
	else
	  mathexception('f', FC_DFP_ZDIV, ea);
	break;

      case 4:  /* QFLD */
      case 5:  /* QFST */
      case 6:  /* QFSB */
      case 7:  /* QFAD */
	fault(UIIFAULT, RPL, RP);

      default:
	warn("I-mode 036 switch?");
	fault(ILLINSTFAULT, RPL, RP);
      }
      goto fetch;

    case 037:
      warn("I-mode opcode 037?");
      fault(ILLINSTFAULT, RPL, RP);

    case 040:  /* generic class 2, overlays skip group */
      /* this should have been handled already! */
      fatal("I-mode generic class 2?");

    case 041:
      TRACE(T_FLOW, " I\n");
      utempl = crsl[dr];
      if (*(int *)&ea < 0) {   /* register-to-register form */
	crsl[dr] = immu32;
	crsl[(inst >> 2) & 7] = utempl;
      } else {
	crsl[dr] = get32(ea);
	put32(utempl, ea);
      }
      goto fetch;

    case 042:
      TRACE(T_FLOW, " M\n");
      dr &= 6;                  /* force dr even */
      if (*(int *)&ea < 0)
	templ = immu32;
      else
        templ = get32(ea);
      *(long long *)(crsl+dr) = (long long)templ * (long long)(*(int *)(crsl+dr));
      CLEARC;
      goto fetch;

    case 043:
      TRACE(T_FLOW, " X\n");
      if (*(int *)&ea < 0)
	crsl[dr] ^= immu32;
      else
        crsl[dr] ^= get32(ea);
      goto fetch;

    case 044:
      TRACE(T_FLOW, " LDAR\n");
      crsl[dr] = ldar(ea);
      goto fetch;

    case 045:
      if (*(int *)&ea < 0) {
	TRACE(T_FLOW, " CCP\n");
	utempl1 = EACP(crsl[dr]);
	utempl2 = EACP(immu32);
	if (utempl1 < utempl2)
	  crs[KEYS] = crs[KEYS] & ~0300 | 0200;
	else if (utempl1 == utempl2)
	  crs[KEYS] = crs[KEYS] & ~0300 | 0100;
	else
	  crs[KEYS] = crs[KEYS] & ~0300;
      } else {
	TRACE(T_FLOW, " LCC\n");
	utempa = get16(ea);
	TRACE(T_INST, " before load, keys=%o, ea=%o/%o, [ea]=0x%x, dr=%d, [dr]=0x%x\n", crs[KEYS], ea>>16, ea&0xFFFF, utempa, dr, crsl[dr]);
	if (ea & EXTMASK32)
	  utempa &= 0xFF;
	else
	  utempa >>= 8;
	crs[dr*2] = utempa;
	if (utempa == 0)
	  SETEQ;
	else
	  CLEAREQ;
	TRACE(T_INST, " after load, keys=%o, ea=%o/%o, utempa=0x%x, dr=%d, [dr]=0x%x\n", crs[KEYS], ea>>16, ea&0xFFFF, utempa, dr, crsl[dr]);
      }
      goto fetch;

    case 046:  /* I special MR, GR format: IM, PCL, EALB, ZM, TM, QFMP, QFDV, QFC */
      switch (dr) {
      case 0:
	TRACE(T_FLOW, " IM\n");
	templ = get32(ea);
	put32(templ+1, ea);
	CLEARCC;
	/* NOTE: test pre-incremented values to get true LT (overflow) */
	if (templ == -1)
	  SETEQ;
	else if (templ < 0)
	  SETLT;
	break;

      /* NOTE: V-mode PCL may jump here! */

      case 1:
imodepcl:
	stopwatch_push(&sw_pcl);
	TRACE(T_FLOW|T_PCL, "#%d %o/%o: PCL %o/%o %s\n", gvp->instcount, RPH, RPL-2, ea>>16, ea&0xFFFF, searchloadmap(ea, 'e'));
	if (gvp->numtraceprocs > 0 && TRACEUSER)
	  for (i=0; i<gvp->numtraceprocs; i++)
	    if (traceprocs[i].ecb == (ea & 0xFFFFFFF) && traceprocs[i].sb == -1) {
	      gvp->traceflags = ~TB_MAP;
	      gvp->savetraceflags = gvp->traceflags;
	      traceprocs[i].sb = *(int *)(crs+SB);
	      printf("Enabled trace for %s at sb '%o/%o\n", traceprocs[i].name, crs[SBH], crs[SBL]);
	      break;
	    }
	pcl(ea);
	stopwatch_pop(&sw_pcl);
	break;

      case 2:
	TRACE(T_FLOW, " EALB\n");
	*(ea_t *)(crs+LB) = ea;
	break;

      case 3:
	TRACE(T_FLOW, " ZM\n");
	put32(0, ea);
	break;

      case 4:
	TRACE(T_FLOW, " TM\n");
	utempl = get32(ea);
	SETCC_32(utempl);
	break;

      case 5:
	TRACE(T_FLOW, " QFMP\n");
	fault(UIIFAULT, RPL, RP);
	//warn("IXX QFMP");
	break;

      case 6:
	TRACE(T_FLOW, " QFDV\n");
	fault(UIIFAULT, RPL, RP);
	//warn("IXX QFDV");
	break;

      case 7:
	TRACE(T_FLOW, " QFC\n");
	fault(UIIFAULT, RPL, RP);
	//warn("IXX QFC");
	break;

      default:
	warn("I-mode 006 switch?");
	fault(ILLINSTFAULT, RPL, RP);
      }
      goto fetch;

    case 047:
      warn("I-mode opcode 047?");
      fault(ILLINSTFAULT, RPL, RP);

    case 050:
      warn("I-mode opcode 050?");
      fault(ILLINSTFAULT, RPL, RP);

    case 051:
      TRACE(T_FLOW, " IH\n");
      utempa = crs[dr*2];
      if (*(int *)&ea < 0) {
	crs[dr*2] = immu32 >> 16;
	crs[((inst >> 2) & 7)*2] = utempa;
      } else {
	crs[dr*2] = get16(ea);
	put16(utempa, ea);
      }
      goto fetch;

    case 052:
      TRACE(T_FLOW, " MH\n");
      if (*(int *)&ea < 0)
	tempa = (immu32 >> 16);
      else
        tempa = get16(ea);
      crsl[dr] = *(short *)(crs+dr*2) * tempa;
      CLEARC;
      goto fetch;

    case 053:
      TRACE(T_FLOW, " XH\n");
      if (*(int *)&ea < 0)
	crs[dr*2] ^= (immu32 >> 16);
      else
        crs[dr*2] ^= get16(ea);
      goto fetch;

    case 054:
      TRACE(T_FLOW, " STAR\n");
      star(crsl[dr], ea);
      goto fetch;

    case 055:
      if (*(int *)&ea < 0) {
	TRACE(T_FLOW, " ACP\n");
	TRACE(T_INST, " before acp, crsl[%d]=%o/%o, immu32=%d, CP(dr)=%d\n", dr, crsl[dr]>>16, crsl[dr]&0xFFFF, immu32, EACP(crsl[dr]));
	utempl = EACP(crsl[dr]);
	utempl += *(int *)&immu32;
	crsl[dr] = CPEA(crsl[dr], utempl);
	TRACE(T_INST, " after acp, utempl=%d, crsl[dr]=%o/%o\n", utempl, crsl[dr]>>16, crsl[dr]&0xFFFF);
      } else {
	TRACE(T_FLOW, " SCC\n");
	utempa = get16(ea);
	if (ea & EXTMASK32)
	  utempa = (utempa & 0xFF00) | (crs[dr*2] & 0xFF);
	else
	  utempa = (crs[dr*2] << 8) | (utempa & 0xFF);
	put16(utempa, ea);
      }
      goto fetch;

    case 056:  /* I special MR, GR format: IMH, JMP, EAXB, ZMH, TMH */
      switch (dr) {
      case 0:
	TRACE(T_FLOW, " IMH\n");
	tempa = get16(ea);
	put16(tempa+1, ea);
	CLEARCC;
	if (tempa == -1)
	  SETEQ;
	else if (tempa < 0)
	  SETLT;
	break;

      case 1:
	TRACE(T_FLOW, " JMP\n");
	RP = ea;
	break;

      case 2:
	TRACE(T_FLOW, " EAXB\n");
	*(ea_t *)(crs+XB) = ea;
	break;

      case 3:
	TRACE(T_FLOW, " ZMH\n");
	put16(0, ea);
	break;

      case 4:
	TRACE(T_FLOW, " TMH\n");
	utempa = get16(ea);
	SETCC_16(utempa);
	break;

      default:
	fault(ILLINSTFAULT, RPL, RP);
      }
      goto fetch;

    case 057:
      warn("I-mode opcode 057?");
      fault(ILLINSTFAULT, RPL, RP);

    case 060:
      /* this should have been handled already! */
      fatal("I-mode generic class 3?");

    case 061:
      TRACE(T_FLOW, " C\n");
      if (*(int *)&ea < 0)
	utempl = immu32;
      else
        utempl = get32(ea);
      crs[KEYS] &= ~020300;     /* clear L, EQ LT */
      utempll = crsl[dr];
      if ((utempll + (~utempl & 0xFFFFFFFF) + 1) & 0x100000000LL)
	crs[KEYS] |= 020000;
      if (crsl[dr] == utempl)
	SETEQ;
      else if (*(int *)(crsl+dr) < *(int *)&utempl)
	SETLT;
      goto fetch;

    case 062:
      TRACE(T_FLOW, " D\n");
      if (*(int *)&ea < 0)
	templ = immu32;
      else
        templ = get32(ea);
      dr &= 6;                  /* force dr even */
      if (templ != 0) {
	templl1 = *(long long *)(crsl+dr);
	templl2 = templl1 / templ;
	crsl[dr] = templl2;
	crsl[dr+1] = templl1 % templ;
	if (-2147483648LL <= templl2 && templl2 <= 2147483647LL)
	  CLEARC;
	else
	  mathexception('i', FC_INT_OFLOW, 0);
      } else
	mathexception('i', FC_INT_ZDIV, 0);
      goto fetch;

    case 063:
      TRACE(T_FLOW, " EAR\n");
      if (*(int *)&ea >= 0)
	crsl[dr] = ea;
      else {
	warn("Immediate mode EAR?");
	fault(ILLINSTFAULT, RPL, RP);
      }
      goto fetch;

    case 064:
      TRACE(T_FLOW, " MIA\n");
      fault(UIIFAULT, RPL, RP);
      goto fetch;

    case 065:
      TRACE(T_FLOW, " LIP\n");
      utempl = get32(ea);
      if (utempl & 0x80000000)
	fault(POINTERFAULT, utempl>>16, ea);
      crsl[dr] = utempl | (RP & RINGMASK32);  /* CPU.AMGRR, cpuid=26+ */
      goto fetch;

    case 066:  /* I-mode special MR: DM, JSXB */
      switch (dr) {
      case 0:
	TRACE(T_FLOW, " DM\n");
	templ = get32(ea);
	put32(templ-1, ea);
	/* NOTE: test pre-decremented values to get true LT (overflow) */
	CLEARCC;
	if (templ == 1)
	  SETEQ;
	else if (templ <= 0)
	  SETLT;
	break;

      case 1:
	TRACE(T_FLOW, " JSXB\n");
	*(unsigned int *)(crs+XB) = RP;
	RP = ea;
	break;

      default:
	fault(ILLINSTFAULT, RPL, RP);
      }
      goto fetch;

    case 067:
      warn("I-mode opcode 067?");
      fault(ILLINSTFAULT, RPL, RP);

    case 070:
      warn("I-mode opcode 070?");
      fault(ILLINSTFAULT, RPL, RP);

    case 071:
      TRACE(T_FLOW, " CH\n");
      if (*(int *)&ea < 0)
	utempa = (immu32 >> 16);
      else
        utempa = get16(ea);
      crs[KEYS] &= ~020300;
      utempl = crs[dr*2];
      if ((utempl + (~utempa & 0xFFFF) + 1) & 0x10000)
	crs[KEYS] |= 020000;
      if (crs[dr*2] == utempa)
	SETEQ;
      else if (*(short *)(crs+dr*2) < *(short *)&utempa)
	SETLT;
      goto fetch;

    case 072:
      TRACE(T_FLOW, " DH\n");
      if (*(int *)&ea < 0)
	tempa = (immu32 >> 16);
      else
        tempa = get16(ea);
      if (tempa != 0) {
	templ1 = *(int *)(crsl+dr);
	templ2 = templ1 / tempa;
	crsl[dr] = (templ2 << 16) | (templ1 % tempa);
	if (-32768 <= templ2 && templ2 <= 32767)
	  CLEARC;
	else
	  mathexception('i', FC_INT_OFLOW, 0);
      } else
	mathexception('i', FC_INT_ZDIV, 0);
      goto fetch;

    case 073:
      TRACE(T_FLOW, " JSR\n");
      crs[dr*2] = RPL;
      RP = ea;
      goto fetch;

    case 074:
      TRACE(T_FLOW, " MIB\n");
      fault(UIIFAULT, RPL, RP);
      goto fetch;

    case 075:
      TRACE(T_FLOW, " AIP\n");
      if (*(int *)&ea < 0)
	utempl = immu32;
      else
	utempl = get32(ea);
      utempl += crsl[dr];
      if (utempl & 0x80000000)
	fault(POINTERFAULT, utempl>>16, ea);
      /* IXX: ISG says C & L are set, ring needs to be weakened */
      crsl[dr] = utempl;
      goto fetch;

    case 076:  /* I-mode special MR: DMH, TCNP */
      switch (dr) {
      case 0:
	TRACE(T_FLOW, " DMH\n");
	tempa = get16(ea);
	put16(tempa-1, ea);
	CLEARCC;
	/* NOTE: test pre-decremented values to get true LT (overflow) */
	if (tempa == 1)
	  SETEQ;
	else if (tempa <= 0)
	  SETLT;
	break;

      case 6:
	TRACE(T_FLOW, " TCNP\n");
	if (*(int *)&ea > 0)
	  if ((get32(ea) & 0x1FFFFFFF) == 0)
	    SETEQ;
	  else
	    CLEAREQ;
	else
	  fault(UIIFAULT, RPL, RP);
	break;

      default:
	fault(ILLINSTFAULT, RPL, RP);
      }
      goto fetch;

    case 077:
      warn("I-mode opcode 077");
      fault(ILLINSTFAULT, RPL, RP);
    }
    fatal("I-mode fall-through?");


nonimode:

    /* get x bit and adjust opcode so that PMA manual opcode
       references can be used directly, ie, if the PMA manual says the
       opcode is '15 02, then 01502 can be used here.  If the PMA
       manual says the opcode is '11, then use 01100 (the XX extended
       opcode bits are zero) */

    x = inst & 040000;            /* indexed is bit 2 */
    opcode = (inst & 036000) >> 4;  /* isolate opcode bits */

    /* fix ldx/stx (opcode '15): these instructions cannot be indexed
       so if an instruction specifies indexing by X, it acts like an
       opcode extension.  Opcodes listed as '35 02 for example (sty in
       V-mode, jdx in R-mode) have X=1 with the 4 opcode bits 1101
       ('15)

         x=0, opcode='15 -> stx (SRV)
         x=1, opcode='15 -> ldx (SRV) (aka '35)

	 x=0, opcode='15 01 -> flx (RV)
	 x=1, opcode='15 01 -> ldy (V) (aka '35 01)

	 x=0, opcode='15 02 -> dflx (V)
	 x=0, opcode='15 02 -> jdx (R)
         x=1, opcode='15 02 -> sty (V) (aka '35 02)

	 x=0, opcode='15 03 -> jix (R)
	 x=1, opcode='15 03 -> jsx (RV) (aka '35 03)
    */
    
    if (opcode == 01500) {
      opcode = opcode | ((inst & 040000)>>4);   /* if X set, expand opcode */
      x = 0;                        /* clear X bit (these can't be indexed) */
      TRACE(T_INST, " ldx/stx opcode adjusted\n");
    }

    TRACE(T_INST, " opcode=%5#0o, i=%o, x=%o\n", opcode, inst & 0100000, x);
    stopwatch_push(&sw_ea);

    if (crs[KEYS] & 010000)                 /* really is only V-mode */
      ea = ea64v(earp, inst, x, &opcode);

    else if (opcode == 01400) {             /* check for pio in R-mode */
      pio(inst);
      goto fetch;

    } else if (crs[KEYS] & 004000)
      ea = ea32r64r(earp, inst, x, &opcode);

    else if (crs[KEYS] & 002000)
      ea = ea32s(inst, x);

    else if ((crs[KEYS] & 016000) == 0)
      ea = ea16s(inst, x);

    else {
      printf("Bad CPU mode in EA calculation, keys = %o\n", crs[KEYS]);
      fatal(NULL);
    }
    stopwatch_pop(&sw_ea);

    TRACE(T_INST, " EA: %o/%o  %s\n",ea>>16, ea & 0xFFFF, searchloadmap(ea,' '));


    /* NOTE: basic and dbasic execute instructions from the register file 
       with TRACE ON */

    switch (opcode) {

    case 00100:
      TRACE(T_FLOW, " JMP\n");
      RP = ea;
      goto fetch;

    /* NOTE: don't use get32 for DLD/DST, because it doesn't handle register
       address traps */

    case 00200:
      crs[A] = get16t(ea);
      if ((crs[KEYS] & 050000) != 040000) {  /* not R-mode or not DP */
	TRACE(T_FLOW, " LDA ='%o/%d\n", crs[A], *(short *)(crs+A));
      } else {
	TRACE(T_FLOW, " DLD\n");
	crs[B] = get16t(INCVA(ea,1));
      }
      goto fetch;

    case 00400:
      put16t(crs[A],ea);
      if ((crs[KEYS] & 050000) != 040000) {
	TRACE(T_FLOW, " STA\n");
      } else {
	TRACE(T_FLOW, " DST\n");
	put16t(crs[B],INCVA(ea,1));
      }
      goto fetch;

    /* NOTE: EQ and LT can be set in the same instruction if overflow
       occurs, for example, '100000+'100000 */

    case 00600:
      crs[KEYS] &= ~0120300;                 /* clear C, L, LT, EQ */
      utempa = crs[A];
      m = get16t(ea);
      if ((crs[KEYS] & 050000) != 040000) {     /* V/I mode or SP */
	TRACE(T_FLOW, " ADD ='%o/%d\n", m, *(short *)&m);
	add16(crs+A, m, 0, ea);
      } else {                                  /* R-mode and DP */
	TRACE(T_FLOW, " DAD\n");
	crs[B] += get16t(INCVA(ea,1));
	utempl = crs[A];
	if (crs[B] & 0x8000) {
	  utempl++;
	  crs[B] &= 0x7fff;
	}
	utempl += m;
	crs[A] = utempl;
	if (utempl & 0x10000)                  /* set L-bit if carry */
	  crs[KEYS] |= 020000;  
	/* NOTE: this EQ test prevents reusing the ADD code :( */
	if (*(int *)(crs+L) == 0)              /* set EQ? */
	  SETEQ; 
	if (((~utempa ^ m) & (utempa ^ crs[A])) & 0x8000) {
	  if (*(int *)(crs+L) >= 0)
	    SETLT;
	  mathexception('i', FC_INT_OFLOW, 0);
	} else if (*(int *)(crs+L) < 0)
	  SETLT;
      }
      goto fetch;

    case 00700:
      crs[KEYS] &= ~0120300;   /* clear C, L, and CC */
      utempa = crs[A];
      m = get16t(ea);
      if ((crs[KEYS] & 050000) != 040000) {
	TRACE(T_FLOW, " SUB ='%o/%d\n", m, *(short *)&m);
	add16(crs+A, ~m, 1, ea);
      } else {
	TRACE(T_FLOW, " DSB\n");
	crs[B] -= get16t(INCVA(ea,1));
	utempl = crs[A];
	if (crs[B] & 0x8000) {
	  utempl += 0xFFFF;
	  crs[B] &= 0x7fff;
	}
	utempl += (unsigned short) ~m;
	utempl += 1;
	crs[A] = utempl;                       /* truncate results */
	if (utempl & 0x10000)                  /* set L-bit if carry */
	  crs[KEYS] |= 020000;  
	if (*(int *)(crs+L) == 0)              /* set EQ? */
	  SETEQ; 
	if (((utempa ^ m) & (utempa ^ crs[A])) & 0x8000) {
	  if (*(int *)(crs+L) >= 0)
	    SETLT;
	  mathexception('i', FC_INT_OFLOW, 0);
	} else if (*(int *)(crs+L) < 0)
	  SETLT;
      }
      goto fetch;

    case 00300:
      m = get16t(ea);
      TRACE(T_FLOW, " ANA ='%o\n",m);
      crs[A] &= m;
      goto fetch;

    case 00500:
      m = get16t(ea);
      TRACE(T_FLOW, " ERA ='%o\n", m);
      crs[A] ^= m;
      goto fetch;

    case 00302:
      m = get16t(ea);
      TRACE(T_FLOW, " ORA ='%o\n", m);
      crs[A] |= m;
      goto fetch;

    case 01000:
      TRACE(T_FLOW, " JST\n");

      /* NOTE: amask should be recomputed here if in R/S mode, so it
	 can be removed as a global variable.  Flaky errors occur if
	 keys are changed w/o calling newkeys(), because amask would
	 be wrong (see dispatcher comment) */

      if (gvp->amask == 0177777)
	m = RPL;
      else
	m = (get16t(ea) & ~gvp->amask) | RPL;
      put16t(m, ea);
      RP = INCVA(ea,1);
      if ((RP & RINGMASK32) == 0)
	gvp->inhcount = 1;
      goto fetch;

    case 01100:
      stopwatch_push(&sw_cas);
      m = get16t(ea);
      TRACE(T_FLOW, " CAS ='%o/%d\n", m, *(short *)&m);
#ifdef FAST
      CLEARCC;
      if (crs[A] == m) {
	INCRP;
	SETEQ;
      } else if (*(short *)(crs+A) < *(short *)&m) {
	RPL += 2;
	SETLT;
      }
      XSETL(0);
#else
      crs[KEYS] &= ~020300;   /* clear L, and CC */
      utempa = crs[A];
      utempl = crs[A];
      utempl += (unsigned short) ~m;
      utempl += 1;
      crs[A] = utempl;                       /* truncate results */
      if (utempl & 0x10000)                  /* set L-bit if carry */
	crs[KEYS] |= 020000;  
      if (crs[A] == 0)                       /* set EQ? */
	SETEQ; 
      if (((utempa ^ m) & (utempa ^ crs[A])) & 0x8000) {
	if (*(short *)(crs+A) >= 0)
	  SETLT;
      } else if (*(short *)(crs+A) < 0)
	SETLT;
      crs[A] = utempa;                       /* restore A reg */
      if (crs[A] == m)
	INCRP;
      else if (*(short *)(crs+A) < *(short *)&m)
	RPL += 2;
#endif
      stopwatch_pop(&sw_cas);
      goto fetch;

    case 01200:
      stopwatch_push(&sw_irs);
      TRACE(T_FLOW, " IRS\n");
      if (*(int *)&ea >= 0)
	utempa = ++mem[mapva(ea, RP, WACC, &access)];
      else {
	utempa = get16t(ea) + 1;
	put16t(utempa, ea);
      }
      if (utempa == 0)
	INCRP;
      stopwatch_pop(&sw_irs);
      goto fetch;

    case 01300:
      TRACE(T_FLOW, " IMA\n");
      if (*(int *)&ea >= 0) {
	pa = mapva(ea, RP, WACC, &access);
	utempa = mem[pa];
	mem[pa] = crs[A];
      } else {
	utempa = get16t(ea);
	put16t(crs[A],ea);
      }
      crs[A] = utempa;
      goto fetch;

    case 01400:
      TRACE(T_FLOW, " JSY\n");
      crs[Y] = RPL;
      RP = ea;
      goto fetch;

    case 01402:
      TRACE(T_FLOW, " JSXB\n");
      *(unsigned int *)(crs+XB) = RP;
      RP = ea;
      goto fetch;

    case 01500:
      TRACE(T_FLOW, " STX\n");
      put16t(crs[X],ea);
      goto fetch;

    /* MPY can't overflow in V-mode, but in R-mode (31 bits),
       -32768*-32768 can overflow and yields 0x8000/0x0000 */

    case 01600:
      m = get16t(ea);
      TRACE(T_FLOW, " MPY ='%o/%d\n", m, *(short *)&m);
      templ = *(short *)(crs+A) * *(short *)&m;
      CLEARC;
      if (crs[KEYS] & 010000) {          /* V/I mode */
	*(int *)(crs+L) = templ;
      } else {                           /* R/S mode */
	utempa = crs[A];
	crs[A] = (templ >> 15);
	crs[B] = templ & 077777;
	if (utempa == 0x8000 && m == 0x8000)
	  mathexception('i', FC_INT_OFLOW, 0);
      }
      goto fetch;

    case 01603:

      /* DIAG CPU.FAULT uses this instruction in R-mode to triggger a
	 UII fault, so this instruction (unlike most) checks the keys.
	 This is a general problem with all instructions: they don't
	 ensure the instruction is legal in the current mode */

      if ((crs[KEYS] & 010000)) {        /* V/I mode */
	templ = get32(ea);
	TRACE(T_FLOW, " MPL ='%o/%d\n", templ, *(int *)&templ);
	*(long long *)(crs+L) = (long long)(*(int *)(crs+L)) * (long long)templ;
	CLEARC;
      } else 
	fault(UIIFAULT, RPL, RP);
      goto fetch;

    case 01700:
      tempa = get16t(ea);
      TRACE(T_FLOW, " DIV ='%o/%d\n", *(unsigned short *)&tempa, tempa);
      if (crs[KEYS] & 010000) {          /* V/I mode */
	templ = *(int *)(crs+A);
      } else {                           /* R/S mode */
	templ = *(short *)(crs+A);       /* convert to 32-bit signed */
	templ = (templ<<15) | (crs[B] & 0x7FFF);
      }
      if (tempa != 0) {
	templ2 = templ / tempa;
	crs[A] = templ2;
	crs[B] = templ % tempa;
	if (-32768 <= templ2 && templ2 <= 32767)
	  CLEARC;
	else
	  mathexception('i', FC_INT_OFLOW, 0);
      } else
	mathexception('i', FC_INT_ZDIV, 0);
      goto fetch;

    /* NOTE:  RESET QVFY, DVL runs okay with cpuid=5 (P750), but
       fails with default cpuid (P4450) */

    case 01703:
      templ = get32(ea);
      TRACE(T_FLOW, " DVL ='%o/%d\n", templ, templ);
      if (templ != 0) {
	templl1 = *(long long *)(crs+L);
	templl2 = templl1 / templ;
	*(int *)(crs+L) = templl2;
	*(int *)(crs+E) = templl1 % templ;
	if (-2147483648LL <= templl2 && templl2 <= 2147483647LL)
	  CLEARC;
	else
	  mathexception('i', FC_INT_OFLOW, 0);
      } else
	mathexception('i', FC_INT_ZDIV, 0);
      goto fetch;

    case 03500:
      TRACE(T_FLOW, " LDX\n");
      crs[X] = get16t(ea);
      goto fetch;

    case 00101:
      if (crs[KEYS] & 010000) {          /* V/I mode */
	TRACE(T_FLOW, " EAL\n");
	*(ea_t *)(crs+L) = ea;
      } else {
	TRACE(T_FLOW, " EAA\n");
	crs[A] = ea;
      }
      goto fetch;

    case 00203:
      if (crs[KEYS] & 010000) {          /* V/I mode */
	*(unsigned int *)(crs+L) = get32(ea);
	TRACE(T_FLOW, " LDL ='%o/%d\n", *(unsigned int *)(crs+A), *(int *)(crs+A));
      } else {
	TRACE(T_FLOW, " JEQ\n");
	if (*(short *)(crs+A) == 0)
	  RPL = ea;
      }
      goto fetch;

    case 00703:
      if (crs[KEYS] & 010000) {          /* V/I mode */
	utempl = get32(ea);
	TRACE(T_FLOW, " SBL ='%o/%d\n", utempl2, *(int *)&utempl2);
	add32(crsl+GR2, ~utempl, 1, ea);
      } else {
	TRACE(T_FLOW, " JGE\n");
	if (*(short *)(crs+A) >= 0)
	  RPL = ea;
      }
      goto fetch;

    case 01002:

      /* NOTE: real PCL code is in I-mode section! */

      if (crs[KEYS] & 010000)            /* V/I mode */
	goto imodepcl;
      TRACE(T_FLOW, " CREP\n");
      put16t(RPL,crs[S]++);
      RPL = ea;
      goto fetch;

    case 00503:
      if (crs[KEYS] & 010000) {          /* V/I mode */
	utempl = get32(ea);
	TRACE(T_FLOW, " ERL ='%o/%d  '%o/'%o  %d/%d\n", utempl, *(int *)&utempl, utempl>>16, utempl&0xFFFF, utempl>>16, utempl&0xFFFF);
	*(unsigned int *)(crs+L) ^= utempl;
      } else {
	TRACE(T_FLOW, " JGT\n");
	if (*(short *)(crs+A) > 0)
	  RPL = ea;
      }
      goto fetch;

    case 00403:
      if (crs[KEYS] & 010000) {          /* V/I mode */
	TRACE(T_FLOW, " STL\n");
	put32(*(unsigned int *)(crs+L),ea);
      } else {
	TRACE(T_FLOW, " JLE\n");
	if (*(short *)(crs+A) <= 0)
	  RPL = ea;
      }
      goto fetch;

    case 00603:
      if (crs[KEYS] & 010000) {          /* V/I mode */
	utempl = get32(ea);
	TRACE(T_FLOW, " ADL ='%o/%d\n", utempl, *(int *)&utempl);
	add32(crsl+GR2, utempl, 0, ea);
      } else {
	TRACE(T_FLOW, " JLT\n");
	if (*(short *)(crs+A) < 0)
	  RPL = ea;
      }
      goto fetch;

    case 00303:
      if (crs[KEYS] & 010000) {          /* V/I mode */
	utempl = get32(ea);
	TRACE(T_FLOW, " ANL ='%o\n", utempl);
	*(unsigned int *)(crs+L) &= utempl;
      } else {
	TRACE(T_FLOW, " JNE\n");
	if (*(short *)(crs+A) != 0)
	  RPL = ea;
      }
      goto fetch;

    case 01202:
      TRACE(T_FLOW, " EAXB\n");
      *(ea_t *)(crs+XB) = ea;
      goto fetch;

    case 01502:
      if (crs[KEYS] & 010000) {          /* V/I mode */
	TRACE(T_FLOW, " DFLX\n");
	crs[X] = get16(ea) * 4;
      } else {
	TRACE(T_FLOW, " JDX\n");
	crs[X]--;
	if (crs[X] != 0)
	  RPL = ea;
      }
      goto fetch;

    case 03502:
      TRACE(T_FLOW, " STY\n");
      put16(crs[Y],ea);
      goto fetch;

    case 01503:
      if (crs[KEYS] & 010000) {          /* V/I mode */
	TRACE(T_FLOW, " QFLX\n");
	crs[X] = get16(ea) * 8;
      } else {
	TRACE(T_FLOW, " JIX\n");
	crs[X]++;
	if (crs[X] != 0)
	  RPL = ea;
      }
      goto fetch;

    case 01501:
      TRACE(T_FLOW, " FLX\n");
      crs[X] = get16(ea) * 2;
      goto fetch;

    case 03501:
      TRACE(T_FLOW, " LDY\n");
      crs[Y] = get16(ea);
      goto fetch;

    case 03503:
      TRACE(T_FLOW, " JSX\n");
      crs[X] = RPL;
      RP = ea;
      goto fetch;

    /* XXX: this should set the L bit like subtract */

    case 01103:
      TRACE(T_FLOW, " CLS\n");
      templ = get32(ea);
      CLEARCC;
      if (*(int *)(crs+L) == templ) {
	INCRP;
	SETEQ;
      } else if (*(int *)(crs+L) < templ) {
	RPL += 2;
	SETLT;
      }
      XSETL(0);
      goto fetch;

    case 00601:
      TRACE(T_FLOW, " FAD\n");
      CLEARC;
      immu64 = get32(ea);
      immu64 = ((immu64 << 32) & 0xffffff0000000000LL) | (immu64 & 0xff);
      if (*(int *)&immu64)
	if (*(int *)(crsl+FAC1)) {
	  tempa1 = crs[FEXP];
	  tempa2 = immu64 & 0xffff;
	  if (abs(tempa1-tempa2) < 48)
	    if (prieee8(crsl+FAC1, &tempd1) && prieee8(&immu64, &tempd2)) {
	      *(double *)(crsl+FAC1) = ieeepr8(tempd1+tempd2);
	      XCLEARC;   /* XXX: test overflow */
	    } else
	      mathexception('f', FC_SFP_OFLOW, ea);
	  else if (tempa1 < tempa2)
	    *(long long *)(crsl+FAC1) = immu64;
	} else
	  *(long long *)(crsl+FAC1) = immu64;
      else if (*(int *)(crsl+FAC1) == 0)
	*(long long *)(crsl+FAC1) = 0;
      goto fetch;

    /* this is implemented as a subtract on some models */

    case 01101:
      TRACE(T_FLOW, " FCS\n");
      templ = get32(ea);
      RPL += fcs(crsl+FAC1, templ);
      goto fetch;

    case 01701:
      TRACE(T_FLOW, " FDV\n");
      CLEARC;
      immu64 = get32(ea);
      immu64 = ((immu64 << 32) & 0xffffff0000000000LL) | (immu64 & 0xff);
      if (*(int *)&immu64)
	if (*(int *)(crsl+FAC1))
	  if (prieee8(&immu64, &tempd2) && prieee8(crsl+FAC1, &tempd1)) {
	    *(double *)(crsl+FAC1) = ieeepr8(tempd1/tempd2);
	    XCLEARC;   /* XXX: test overflow */
	  } else
	    mathexception('f', FC_SFP_OFLOW, ea);
	else            /* operand = 0.0 */
	  *(long long *)(crsl+FAC1) = 0;
      else            /* clean up (maybe) dirty zero */
	mathexception('f', FC_SFP_ZDIV, ea);
      goto fetch;

    case 0201:
      TRACE(T_FLOW, " FLD\n");
      utempl = get32(ea);
      crsl[FAC1] = utempl & 0xFFFFFF00;
      crsl[FAC1+1] = utempl & 0x00FF;
      goto fetch;

    case 01601:
      TRACE(T_FLOW, " FMP\n");
      CLEARC;
      if (*(int *)(crsl+FAC1)) {
	immu64 = get32(ea);
	immu64 = ((immu64 << 32) & 0xffffff0000000000LL) | (immu64 & 0xff);
	if (*(int *)&immu64)
	  if (prieee8(&immu64, &tempd2) && prieee8(crsl+FAC1, &tempd1)) {
	    *(double *)(crsl+FAC1) = ieeepr8(tempd1*tempd2);
	    XCLEARC;   /* XXX: test overflow */
	  } else
	    mathexception('f', FC_SFP_OFLOW, ea);
	else            /* operand = 0.0: no multiply */
	  *(long long *)(crsl+FAC1) = 0;
      } else            /* clean up (maybe) dirty zero */
	*(long long *)(crsl+FAC1) = 0;
      goto fetch;

    case 00701:
      TRACE(T_FLOW, " FSB\n");
      CLEARC;
      immu64 = get32(ea);
      immu64 = ((immu64 << 32) & 0xffffff0000000000LL) | (immu64 & 0xff);
      if (*(int *)&immu64)
	if (*(int *)(crsl+FAC1)) {
	  tempa1 = crs[FEXP];
	  tempa2 = immu64 & 0xffff;
	  if (abs(tempa1-tempa2) < 48)
	    if (prieee8(crsl+FAC1, &tempd1) && prieee8(&immu64, &tempd2)) {
	      *(double *)(crsl+FAC1) = ieeepr8(tempd1-tempd2);
	      XCLEARC;   /* XXX: test overflow */
	    } else
	      mathexception('f', FC_SFP_OFLOW, ea);
	  else if (tempa1 < tempa2) {
	    *(long long *)(crsl+FAC1) = immu64;
	    dfcm(crsl+FAC1);
	  }
	} else {
	  *(long long *)(crsl+FAC1) = immu64;
	  dfcm(crsl+FAC1);
	}
      else if (*(int *)(crsl+FAC1) == 0)
	*(long long *)(crsl+FAC1) = 0;
      goto fetch;

    case 0401:
      TRACE(T_FLOW, " FST\n");
      CLEARC;
      if (crs[KEYS] & 010)
	frn(crsl+FAC1);
      if ((crsl[FAC1+1] & 0xFF00) == 0)
	put32((crsl[FAC1] & 0xFFFFFF00) | (crsl[FAC1+1] & 0xFF), ea);
      else
	mathexception('f', FC_SFP_STORE, ea);
      goto fetch;

    case 0602:
      TRACE(T_FLOW, " DFAD\n");
      CLEARC;
      immu64 = get64(ea);
      if (*(int *)&immu64)
	if (*(int *)(crsl+FAC1))
	  if (prieee8(crsl+FAC1, &tempd1) && prieee8(&immu64, &tempd2)) {
	    *(double *)(crsl+FAC1) = ieeepr8(tempd1+tempd2);
	    XCLEARC;   /* XXX: test overflow */
	  } else
	    mathexception('f', FC_DFP_OFLOW, ea);
	else
	  *(long long *)(crsl+FAC1) = immu64;
      else if (*(int *)(crsl+FAC1) == 0)
	*(long long *)(crsl+FAC1) = 0;
      goto fetch;

    case 01102:
      TRACE(T_FLOW,  " DFCS\n");
      templl = get64(ea);
      RPL += dfcs(crsl+FAC1, templl);
      goto fetch;

    case 01702:
      TRACE(T_FLOW, " DFDV\n");
      CLEARC;
      if (*(int *)&ea >= 0)
	immu64 = get64(ea);
      if (*(int *)&immu64)
	if (*(int *)(crsl+FAC1))
	  if (prieee8(&immu64, &tempd2) && prieee8(crsl+FAC1, &tempd1)) {
	    *(double *)(crsl+FAC1) = ieeepr8(tempd1/tempd2);
	    XCLEARC;   /* XXX: test overflow */
	  } else
	    mathexception('f', FC_DFP_OFLOW, ea);
	else
	  *(long long *)(crsl+FAC1) = 0;
      else
	mathexception('f', FC_DFP_ZDIV, ea);
      goto fetch;

    case 0202:
      TRACE(T_FLOW, " DFLD\n");
      *(long long *)(crs+FLTH) = get64(ea);
      goto fetch;

    case 01602:
      TRACE(T_FLOW, " DFMP\n");
      CLEARC;
      if (*(int *)(crsl+FAC1)) {
	immu64 = get64(ea);
	if (*(int *)&immu64)
	  if (prieee8(&immu64, &tempd2) && prieee8(crsl+FAC1, &tempd1)) {
	    *(double *)(crsl+FAC1) = ieeepr8(tempd1*tempd2);
	    XCLEARC;   /* XXX: test overflow */
	  } else
	    mathexception('f', FC_DFP_OFLOW, ea);
	else             /* operand = 0.0: no multiply */
	  *(long long *)(crsl+FAC1) = 0;
      } else
	*(long long *)(crsl+FAC1) = 0;
      goto fetch;

    case 0702:
      TRACE(T_FLOW, " DFSB\n");
      CLEARC;
      immu64 = get64(ea);
      if (*(int *)&immu64)
	if (*(int *)(crsl+FAC1))
	  if (prieee8(crsl+FAC1, &tempd1) && prieee8(&immu64, &tempd2)) {
	    *(double *)(crsl+FAC1) = ieeepr8(tempd1-tempd2);
	    XCLEARC;   /* XXX: test overflow */
	  } else
	    mathexception('f', FC_DFP_OFLOW, ea);
	else {
	  *(long long *)(crsl+FAC1) = immu64;
	  dfcm(crsl+FAC1);
	}
      else if (*(int *)(crsl+FAC1) == 0)
	*(long long *)(crsl+FAC1) = 0;
      goto fetch;

    case 0402:
      TRACE(T_FLOW, " DFST\n");
      put64(*(long long *)(crs+FLTH), ea);
      goto fetch;

    case 01302:
      TRACE(T_FLOW, " EALB\n");
      *(ea_t *)(crs+LB) = ea;
      goto fetch;

    case 0301:
      TRACE(T_FLOW, " STLR '%06o\n", ea & 0xFFFF);
      star(*(int *)(crs+L), ea);
      goto fetch;

    case 0501:
      TRACE(T_FLOW, " LDLR '%06o\n", ea & 0xFFFF);
      *(int *)(crs+L) = ldar(ea);
      goto fetch;

    case 0502:
      TRACE(T_FLOW, " QFxx '%06o\n", ea & 0xFFFF);
      fault(UIIFAULT, RPL, RP);

    case 01401:
      TRACE(T_FLOW, " EIO\n");
      CLEAREQ;
      pio(ea & 0xFFFF);
      goto fetch;

    case 00102:
      TRACE(T_FLOW, " XEC\n");
      utempa = get16t(ea);
      //utempl = RP-2;
      //printf("RPL %o/%o: XEC instruction %o|%o, ea is %o/%o, new inst = %o \n", utempl>>16, utempl&0xFFFF, inst, get16t(utempl+1), ea>>16, ea&0xFFFF, utempa);
      inst = utempa;
      earp = INCVA(ea,1);
      goto xec;

    case 00103:
      TRACE(T_FLOW, " ENTR\n");
      utempa = crs[S];
      crs[S] -= ea;
      put16t(utempa,crs[S]);
      goto fetch;

    default:
      printf("em: unknown memory reference opcode: %o\n", opcode);
      fault(UIIFAULT, RPL, RP);
      fatal(NULL);
    }
}
