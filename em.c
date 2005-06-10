/* Pr1me Computer emulator, Jim Wilcoxson (jim@meritnet.com), April 4, 2005
   Copyright (C) 2005, Jim Wilcoxson (jim@meritnet.com).  All Rights Reserved.

   Restores a Prime R-mode .save image from stdin to memory and
   emulates execution.

   PLEASE NOTE: this is a very rough prototype still in development.
   The main goal for the prototype is to get an understanding of the
   kinds of design issues faced by an emulator, before doing a "real"
   Prime 50-series emulator.

   You are welcome to pass this along to friends for fun and
   amusement, but please don't publish it.  Comments, suggestions,
   corrections, and general notes that you're interested in a Prime
   emulation project are also welcome and appreciated.

   Usage:

   $ ./em <smad.save 2>/dev/null

   Lots of instruction details are spewed to stderr.


   Left to do (way more to do ... this just gives an idea):

   - only runs on a big-endian machine, like the Prime

   - svc doesn't handle alternate returns, LOC(blah) args might not be
   handled correctly, and it doesn't handle missing or extra arguments
   correctly (by looking for the terminating zero)

   - not all C-bit, L-bit, and condition code updates are correct

   - restricted-mode instruction checking is missing

*/

#include <stdio.h>
#include <errno.h>
#include <setjmp.h>
#include "syscom/keys.ins.cc"
#include "syscom/errd.ins.cc"

/* Prime CPU registers are mapped to memory locations 0-'37, but only
   0-7 are accessible in SR user mode.  In VI user mode, locations
   0-'17 are trapped and map to the live register file (p 5-17, Sys Arch),
   though only 0-7 are accessible in user mode.

   Locations '40-'57 are reserved for 8 DMC channels, 2 words each.
   Locations '60-'77 are interrupt vectors
   Locations '100-'177 are for external device interrupts 
   see p. A-8 of Sys Arch
*/

#include "regs.h"
typedef unsigned int ea_t;
typedef unsigned int pa_t;

/* the live program counter register is aka microcode scratch register TR7 */

#define RP regs.sym.tr7
#define RPH regs.u16[14]
#define RPL regs.u16[15]

#define SETCC_A \
  crs[KEYS] &= ~0300; \
  if (crs[A] == 0) \
    crs[KEYS] |= 0100; \
  else if (*(short *)(crs+A) < 0) \
    crs[KEYS] |= 0200;

#define SETCC_X \
  crs[KEYS] &= ~0300; \
  if (crs[X] == 0) \
    crs[KEYS] |= 0100; \
  else if (*(short *)(crs+X) < 0) \
    crs[KEYS] |= 0200;

#define SETCC_L \
  crs[KEYS] &= ~0300; \
  if (*(int *)(crs+L) == 0) \
    crs[KEYS] |= 0100; \
  else if (*(int *)(crs+L) < 0) \
    crs[KEYS] |= 0200;

#define SETCC_LE \
  crs[KEYS] &= ~0300; \
  if (*(int *)(crs+L) == 0 && *(int *)(crs+E) == 0) \
    crs[KEYS] |= 0100; \
  else if (*(int *)(crs+L) < 0) \
    crs[KEYS] |= 0200;

/* NOTE: in previous versions, exponent must be zero for 0.0, but DIAG
   test CPU.FLOAT.V considers a zero fraction and non-zero exponent to
   also be 0.0 */
  
#define SETCC_F \
  crs[KEYS] &= ~0300; \
  if (*(short *)(crs+FLTH) < 0) \
    crs[KEYS] |= 0200; \
  else if (*(int *)(crs+FLTH) == 0) \
    crs[KEYS] |= 0100;

#define SETCC_D SETCC_F

/* XEXPC is a dummy to indicate that the C-bit may not be set correctly */

#define XEXPC(onoff) \
  if ((onoff)) crs[KEYS] |= 0100000; \
  else crs[KEYS] &= 077777

#define EXPC(onoff) \
  if ((onoff)) crs[KEYS] |= 0100000; \
  else crs[KEYS] &= 077777

#define SETC crs[KEYS] |= 0100000
#define CLEARC crs[KEYS] &= 077777

/* XSETL is a dummy to indicate that the L-bit may not be set correctly */

#define XSETL(onoff) \
  if ((onoff)) crs[KEYS] |= 020000; \
  else crs[KEYS] &= ~020000;

#define SETL(onoff) \
  if ((onoff)) crs[KEYS] |= 020000; \
  else crs[KEYS] &= ~020000;

/* Table for unusual instructions that aren't implemented but we want to
   print something when they're encountered */

unsigned short gen0tab[] = {
  000302,          /* SSSN? (store system serial number); occurs early in hard drive boot */
  001172,          /* ? */
  000764,          /* ? */
  000615,          /* ITLB */
  001367,          /* ? */
  000024,          /* STPM */
  000503,          /* EMCM */
  000501,          /* LMCM */
  001304,          /* MDEI */
  001305,          /* MDII */
  001324,          /* MDIW */
  001306,          /* MDRS */
  001307,          /* MDWC */
  000021,          /* RMC */
  0101200,         /* SMCS */
  000311,          /* VIFY */
  001113,          /* XVFY */
  000411};          /* CAI */

#define GEN0TABSIZE sizeof(gen0tab)/sizeof(unsigned short)

char gen0nam[][5] = {
  "302",
  "1172",
  "764",
  "ITLB",
  "1367",
  "STPM",
  "EMCM",
  "LMCM",
  "MDEI",
  "MDII",
  "MDIW",
  "MDRS",
  "MDWC",
  "RMC",
  "SMCS",
  "VIFY",
  "XVFY",
  "CAI"};


/* trace flags to control various aspects of emulator tracing:

   T_EAR	trace R-mode effective address calculation
   T_EAV	trace V-mode effective address calculation
   T_EAI	trace I-mode effective address calculation
   T_FLOW       instruction summary
   T_INST	detailed instruction trace
   T_MODE       trace CPU mode changes
   T_EAAP       AP effective address calculation
   T_DIO        disk I/O
   T_MAP        segmentation
   T_PCL        PCL instructions
*/

#define TB_EAR 0x00000001
#define TB_EAV 0x00000002
#define TB_EAI 0x00000004
#define TB_INST 0x00000008
#define TB_FLOW 0x00000010
#define TB_MODE 0x00000020
#define TB_EAAP 0x00000040
#define TB_DIO 0x00000080
#define TB_MAP 0x00000100
#define TB_PCL 0x00000200

#define T_EAR  (traceflags & TB_EAR)
#define T_EAV  (traceflags & TB_EAV)
#define T_EAI  (traceflags & TB_EAI)
#define T_INST (traceflags & TB_INST)
#define T_FLOW (traceflags & TB_FLOW)
#define T_MODE (traceflags & TB_MODE)
#define T_EAAP (traceflags & TB_EAAP)
#define T_DIO (traceflags & TB_DIO)
#define T_MAP (traceflags & TB_MAP)
#define T_PCL (traceflags & TB_PCL)

int traceflags=0;                    /* each bit is a trace flag */



/* NOTE: Primos II gives "NOT FOUND" on startup 2460 if sense switches
   are set to 014114.  But DIAGS like this sense switch setting. :( */

unsigned short sswitch = 0;

unsigned int instcount=0;

jmp_buf jmpbuf;

/* define a 4 million 16-bit word system memory; later systems seem to
   have had more memory, like 32MB, but I'm not sure how this was
   implemented since it would require more bits in the page map entry
   for the physical page number.  If the page size was made larger,
   like 8K instead of 2K, the number of entries in a page table would
   change from 64 to 16, which also seems drastic... */

#if 0
#define MEMSIZE 07776*64*1024
#else
#define MEMSIZE 4*1024*1024
#endif
unsigned short mem[MEMSIZE];   /* system's physical memory */

unsigned int bitmask32[33] = {0,
			    0x80000000, 0x40000000, 0x20000000, 0x10000000,
			    0x08000000, 0x04000000, 0x02000000, 0x01000000,
			    0x00800000, 0x00400000, 0x00200000, 0x00100000,
			    0x00080000, 0x00040000, 0x00020000, 0x00010000,
			    0x00008000, 0x00004000, 0x00002000, 0x00001000,
			    0x00000800, 0x00000400, 0x00000200, 0x00000100,
			    0x00000080, 0x00000040, 0x00000020, 0x00000010,
			    0x00000008, 0x00000004, 0x00000002, 0x00000001};

unsigned int bitmask16[17] = {0,
			    0x8000, 0x4000, 0x2000, 0x1000,
			    0x0800, 0x0400, 0x0200, 0x0100,
			    0x0080, 0x0040, 0x0020, 0x0010,
			    0x0008, 0x0004, 0x0002, 0x0001};

unsigned int prevpc;                 /* backed program counter */

unsigned short amask;                /* address mask */
#define FAULTMASK32 0x80000000       /* fault bit */
#define RINGMASK32 0x60000000        /* ring bits */
#define EXTMASK32 0x10000000         /* E-bit */
#define SEGMASK32 0x0FFF0000         /* segment number */
#define RINGMASK16 0x6000            /* ring bits */
#define EXTMASK16 0x1000             /* E-bit */

/* Fault codes */

#define RESTRICTFAULT 062
#define PROCESSFAULT  063
#define PAGEFAULT     064
#define SVCFAULT      065
#define UIIFAULT      066
#define PARITYCHECK   067
#define MACHCHECK     070
#define MISSMEMCHECK  071
#define ILLINSTFAULT  072
#define ACCESSFAULT   073
#define ARITHFAULT    074
#define STACKFAULT    075
#define SEGFAULT      076
#define POINTERFAULT  077

int verbose;
int domemdump;                         /* -memdump arg */
int boot;                            /* true if reading a boot record */

#define DTAR(ea) (((ea)>>26) & 3)
#define SEGNO(ea) (((ea)>>16) & 07777)
#define PAGENO(ea) (((ea)>>10) & 077)

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

void fault(unsigned short fvec, unsigned short fcode, ea_t faddr);


/* maps a Prime 28-bit virtual address to a physical memory
   address, checks access, returns actual access (for PCL)
   May cause:
   - segment fault if segment number is too big
   - segment fault if segment's fault bit is set
   - access fault if intended access isn't permitted
   - page fault if page isn't resident
*/

pa_t mapva(ea_t ea, short intacc, short *access) {
  short relseg,seg,nsegs,ring;
  unsigned short pte;
  unsigned int dtar,sdw,staddr,ptaddr,pmaddr;
  pa_t pa;

  seg = SEGNO(ea);
  if ((seg > 0 && (crs[MODALS] & 4)) || (seg == 0 && (crs[MODALS] & 020))) {
    dtar = *(unsigned int *)(crs+DTAR0-2*DTAR(ea));  /* get dtar register */
    nsegs = 1024-(dtar>>22);
    relseg = seg & 0x3FF;     /* segment within segment table */
    if (T_MAP) fprintf(stderr,"MAP: ea=%o/%o, seg=%o, dtar=%o, nsegs=%d, relseg=%d, page=%d\n", ea>>16, ea&0xFFFF, seg, dtar, nsegs, relseg, PAGENO(ea));
    if (relseg >= nsegs)
      fault(SEGFAULT, 1, ea);   /* fcode = segment too big */
    staddr = (dtar & 0x003F0000) | ((dtar & 0x7FFF)<<1);
    sdw = *(unsigned int *)(mem+staddr+relseg*2);
    if (T_MAP) fprintf(stderr,"  staddr=%o, sdw=%o\n", staddr, sdw);
    if (sdw & 0x8000)
      fault(SEGFAULT, 2, ea);   /* fcode = fault bit set */

    /* XXX: should ring be weakened with ea ring? */

    ring = (RPH>>13) & 3;  /* current ring */
    if (ring == 0)
      *access = 7;
    else
      *access = (sdw >> (3-ring)*3+6) & 7;
    if (((intacc & *access) != intacc) || (intacc == PACC && ((*access & 3) == 0)))
      fault(ACCESSFAULT, 0, ea);
    ptaddr = (((sdw & 0x3F)<<10) | (sdw>>22)) << 6;
    pmaddr = ptaddr + PAGENO(ea);
    pte = mem[pmaddr];
    if (T_MAP) fprintf(stderr,"  ptaddr=%o, pmaddr=%o, pte=%o\n", ptaddr, pmaddr, pte);
    if (!(pte & 0x8000))
      fault(PAGEFAULT, 0, ea);
    mem[pmaddr] |= 020000;       /* set referenced bit */
    if (intacc == WACC)
      mem[pmaddr] &= ~020000;    /* reset unmodified bit */
    pa = ((pte & 0xFFF) << 10) | (ea & 0x3FF);
  } else {
    pa = ea & 0x3FFFFF;
  }
  if (pa <= MEMSIZE)
    return pa;
  printf(" map: Memory address %o (%o/%o) is out of range!\n", ea, ea>>16, ea & 0xffff);
  exit(1);
  /* NOTE: should we take a fault here or return zeroes instead? */
}


/* Can V-mode do JMP# 4 to start executing from the register file?
   Might need to "or" 0x80000000 with PBH to indicate this?? */ 

unsigned short get16(ea_t ea) {
  pa_t pa;
  unsigned short access;

  /* check for live register access */

  if (ea & 0x80000000 || (!(crs[KEYS] & 010000) && (ea & 0xFFFF) <= 037)) {
    ea = ea & 0xFFFF;
    if (ea < 7)
      return crs[memtocrs[ea]];
    if (ea == 7)                   /* PC */
      return RPL;
    restrict();
    if (ea <= 017)                 /* CRS */
      return crs[memtocrs[ea]];
    if (ea <= 037)                 /* DMX */
      return regs.sym.regdmx[((ea & 036) << 1) | (ea & 1)];
    printf(" Live register address %o too big!\n", ea);
    fatal(NULL);
  }
  return mem[mapva(ea, RACC, &access)];
}

unsigned int get32(ea_t ea) {
  pa_t pa;
  unsigned short access;
  unsigned short m[2];

  pa = mapva(ea, RACC, &access);
  if ((pa & 01777) <= 01776)
    return *(unsigned int *)(mem+pa);
  else {
    m[0] = mem[pa];
    m[1] = get16(ea+1);
    return *(unsigned int *)m;
  }
}

double get64(ea_t ea) {
  pa_t pa;
  unsigned short access;
  unsigned short m[4];

  pa = mapva(ea, RACC, &access);
  if ((pa & 01777) <= 01774)
    return *(double *)(mem+pa);
  else {
    m[0] = mem[pa];
    m[1] = get16(ea+1);
    m[2] = get16(ea+2);
    m[3] = get16(ea+3);
    return *(double *)m;
  }
}


put16(unsigned short value, ea_t ea) {
  pa_t pa;
  unsigned short access;

  if (ea & 0x80000000) {         /* flag for live register address */
    ea = ea & 0x7FFFFFFF;
    if (ea < 7)
      crs[memtocrs[ea]] = value;
    else if (ea == 7) {
      RPL = value;
    } else {
      restrict();
      if (ea <= 017)                      /* CRS */
	crs[memtocrs[ea]] = value;
      else if (ea <= 037)                 /* DMX */
	regs.sym.regdmx[((ea & 036) << 1) | (ea & 1)] = value;
      else {
	printf(" Live register store address %o too big!\n", ea);
	exit(1);
      }
    }
  } else {
    pa = mapva(ea, WACC, &access);
    mem[pa] = value;
  }
}

put32(unsigned int value, ea_t ea) {
  pa_t pa;
  unsigned short access;
  unsigned short *m;

  pa = mapva(ea, WACC, &access);
  if ((pa & 01777) <= 01776)
    *(unsigned int *)(mem+pa) = value;
  else {
    m = (void *)&value;
    mem[pa] = m[0];
    put16(m[1], ea+1);
  }
}

put64(double value, ea_t ea) {
  pa_t pa;
  unsigned short access;
  unsigned short *m;

  pa = mapva(ea, WACC, &access);
  if ((pa & 01777) <= 01774)
    *(double *)(mem+pa) = value;
  else {
    m = (void *)&value;
    mem[pa] = m[0];
    put16(m[1], ea+1);
    put16(m[2], ea+2);
    put16(m[3], ea+3);
  }
}

void fault(unsigned short fvec, unsigned short fcode, ea_t faddr) {
  unsigned short m;
  ea_t faultrp;

  if (fvec == PROCESSFAULT || fvec == SVCFAULT || fvec == ARITHFAULT)
    faultrp = RP;
  else
    faultrp = prevpc;

  /* save RP and keys in regfile */

  regs.sym.pswpb = faultrp;
  regs.sym.pswkeys = crs[KEYS];
  
  printf("#%d: fault '%o, fcode=%o, faddr=%o/%o, faultrp=%o/%o\n", instcount, fvec, fcode, faddr>>16, faddr&0xFFFF, faultrp>>16, faultrp&0xFFFF);
  fprintf(stderr,"fault '%o, fcode=%o, faddr=%o/%o, faultrp=%o/%o\n", fvec, fcode, faddr>>16, faddr&0xFFFF, faultrp>>16, faultrp&0xFFFF);
  crs[FCODE] = fcode;
  *(unsigned int *)(crs+FADDR) = faddr;
  if (crs[MODALS] & 010) {   /* process exchange is enabled */
    fatal("fault: fault '%o at RP=%o/%o with PX enabled\n", fvec, RPH, RPL);
  } else {                   /* process exchange is disabled */
    m = get16(fvec);
    if (m != 0) {
      if (1 || T_FLOW) fprintf(stderr," JST* '%o [%o]\n", fvec, m);
      put16(faultrp & 0xFFFF, m);
      RP = m;
      RPL++;
      longjmp(jmpbuf, 1);
      fatal("fault: returned after longjmp\n");
    }
    printf("#%d: fault '%o, fcode=%o, faddr=%o/%o, faultrp=%o/%o\n", instcount, fvec, fcode, faddr>>16, faddr&0xFFFF, faultrp>>16, faultrp&0xFFFF);
    fatal("Fault vector is zero, process exchange is disabled.");
  }
}


restrict() {
  if (!(RP & RINGMASK32))
    return 0;
  fault(RESTRICTFAULT, 0, 0);
}


fatal(char *msg) {
  printf("Fatal error: instruction #%d at %o/%o: %o %o\n", instcount, prevpc >> 16, prevpc & 0xFFFF, get16(prevpc), get16(prevpc+1));
  if (msg)
    printf("%s\n", msg);
  printf("keys = %o\n", crs[KEYS]);
  /* should do a register dump here... */
  exit(1);
}
    
/* I/O device map table, containing function pointers to handle device I/O */

#include "emdev.h"

void (*devmap[64])(short, short, short) = {
  0,0,0,0,devasr,0,0,0,
  0,0,0,devmt,devmt,0,0,0,
  devcp,0,0,0,0,0,devdisk,devdisk,
  0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0};


/* read a short (16-bit) integer in big-endian from stdin */

unsigned short readshort () {

  return getchar()<<8 | getchar();
}


/* set new processor keys */

newkeys (unsigned short new) {

  switch ((new & 016000) >> 10) {
  case 0:                     /* 16S */
    if (T_MODE) fprintf(stderr,"Entering 16S mode, keys=%o\n", new);
    amask = 037777;
    break;
  case 1:                     /* 32S */
    if (T_MODE) fprintf(stderr,"Entering 32S mode, keys=%o\n", new);
    amask = 077777;
    break;
  case 2:                     /* 64R */
    if (T_MODE) fprintf(stderr,"Entering 64R mode, keys=%o\n", new);
    amask = 0177777;
    break;
  case 3:                     /* 32R */
    if (T_MODE) fprintf(stderr,"Entering 32R mode, keys=%o\n", new);
    amask = 077777;
    break;
  case 4:                     /* 32I */
    if (T_MODE) fprintf(stderr,"Entering 32I mode, keys=%o\n", new);
    amask = 0177777;
    break;
  case 6:                     /* 64V */
    if (T_MODE) fprintf(stderr,"Entering 64V mode, keys=%o\n", new);
    amask = 0177777;
    break;
  default:                    /* invalid */
    printf("Invalid CPU mode: %o\n", new);
    fatal(NULL);
  }
  crs[KEYS] = new;
}



/* NOTE: This code is untested!  Unclear whether RPL (advanced) or
 prevpc (backed) should be used for address calculations... */

ea_t ea16s (unsigned short inst, short i, short x) {
  
  unsigned short ea,m;

  if (inst & 001000)
    ea = (RPL & 037000) | (inst & 0777);      /* current sector */
  else
    ea = (inst & 0777);                          /* sector 0 */
  while (1) {
    if (x)                                       /* indexed */
      ea += crs[X];
    if (!i)                                      /* not indirect */
      break;
    /* NOTE: this test is already in get16... */
    if (ea < 040)
      m = get16(0x80000000|ea);
    else
      m = get16(ea);
    i = m & 0100000;
    x = m & 040000;
    ea = m & 037777;                             /* go indirect */
  }
  if (ea < 040)                                  /* flag live register ea */
    return ea | 0x80000000;
  return ea;
}


/* NOTE: This code is untested! */

ea_t ea32s (unsigned short inst, short i, short x) {
  
  unsigned short ea,m;

  if (inst & 001000)
    ea = (RPL & 077000) | (inst & 0777);      /* current sector */
  else {
    ea = (inst & 0777);                          /* sector 0 */
    if (ea < 0100 && x) {                        /* preindex by X */
      ea += crs[X];
      x = 0;
    }
  }
  while (i) {
    if (ea < 040)
      m = get16(0x80000000|ea);
    else
      m = get16(ea);
    i = m & 0100000;
    ea = m & 077777;                             /* go indirect */
  }
  if (x)                                         /* postindex */
    ea += crs[X];
  ea &= amask;
  if (ea < 040)                                  /* flag live register ea */
    return ea | 0x80000000;
  return ea;
}


/* NOTE: the difference between 32R and 64R, besides the extra address
   bit, is that 32R indirect words have an indirect bit for multi-level
   indirects */

ea_t ea32r64r (ea_t earp, unsigned short inst, short i, short x, short *opcode) {

  short class;
  unsigned short ea,m,rpl;

  rpl = earp;
  if (T_EAR) fprintf(stderr," ea32r64r: i=%o, x=%o\n", i!= 0, x!=0);
  if (inst & 001000)                             /* sector bit 7 set? */
    if ((inst & 0760) != 0400) {                 /* PC relative? */
      ea = RPL + (((short) (inst << 7)) >> 7);   /* yes, sign extend D */
      if (T_EAR) fprintf(stderr," PC relative, P=%o, new ea=%o\n", rpl, ea);
    }
    else 
      goto special;                              /* special cases */
  else {
    ea = (inst & 0777);                          /* sector 0 */
    if (T_EAR) fprintf(stderr," Sector 0, new ea=%o\n", ea);
    if (ea < 0100 && x) {                        /* preindex by X */
      if (T_EAR) fprintf(stderr," Preindex, ea=%o, X='%o/%d\n", ea, crs[X], *(short *)(crs+X));
      ea += crs[X];
      if (T_EAR) fprintf(stderr," Preindex, new ea=%o\n", ea);
      x = 0;
    }
  }
  while (i) {
    if (ea < 040)
      m = get16(0x80000000|ea);
    else
      m = get16(ea);
    if (T_EAR) fprintf(stderr," Indirect, old ea=%o, [ea]=%o\n", ea, m);
    if ((crs[KEYS] & 016000) == 06000)
      i = m & 0100000;
    else
      i = 0;
    ea = m & amask;                              /* go indirect */
    if (T_EAR) fprintf(stderr," Indirect, new i=%d, new ea=%o\n", i!=0, ea);
  }
  if (x) {
    if (T_EAR) fprintf(stderr," Postindex, old ea=%o, X='%o/%d\n", ea, crs[X], *(short *)(crs+X));
    ea += crs[X];
    if (T_EAR) fprintf(stderr," Postindex, new ea=%o\n", ea);
  }
  ea &= amask;
  if (ea < 040)                                  /* flag live register ea */
    return ea | 0x80000000;
  return ea;

special:
  class = inst & 3;                              /* class bits = 15 & 16 */
  *opcode = *opcode | ((inst >> 2) & 3);         /* opcode extension */
  if (T_EAR) fprintf(stderr," special, new opcode=%5#0o, class=%d\n", *opcode, class);

  if (class < 2) {                               /* class 0/1 */
    ea = get16(RPL++);                           /* get A from next word */
    if (T_EAR) fprintf(stderr," Class %d, new ea=%o\n", class, ea);
    if (class == 1)
      ea += crs[S];
    if (x) {
      if (T_EAR) fprintf(stderr," Preindex, ea=%o, X='%o/%d\n", ea, crs[X], *(short *)(crs+X));
      ea += crs[X];
      if (T_EAR) fprintf(stderr," Preindex, new ea=%o\n", ea);
    }
    while (i) {
      if (ea < 040)
	m = get16(0x80000000|ea);
      else
	m = get16(ea);
      if (T_EAR) fprintf(stderr," Indirect, old ea=%o, [ea]=%o\n", ea, m);
      if ((crs[KEYS] & 016000) == 06000)
	i = m & 0100000;
      else
	i = 0;
      ea = m & amask;
      if (T_EAR) fprintf(stderr," Indirect, new i=%d, new ea=%o\n", i!=0, ea);
    }

  } else if (i && x) {                           /* class 2/3, ix=11 */
    if (T_EAR) fprintf(stderr," class 2/3, ix=11\n");
    ea = get16(RPL++);                           /* get A from next word */
    if (T_EAR) fprintf(stderr," ea=%o\n", ea);
    if (class == 3)
      ea += (short) crs[S];
    while (i) {
      if (ea < 040)
	m = get16(0x80000000|ea);
      else
	m = get16(ea);
      if (T_EAR) fprintf(stderr," Indirect, ea=%o, [ea]=%o\n", ea, m);
      if ((crs[KEYS] & 016000) == 06000)
	i = m & 0100000;
      else
	i = 0;
      ea = m & amask;
      if (T_EAR) fprintf(stderr," Indirect, new i=%d, new ea=%o\n", i!=0, ea);
    }
    if (T_EAR) fprintf(stderr," Postindex, old ea=%o, X='%o/%d\n", ea, crs[X], *(short *)(crs+X));
    ea += (short) crs[X];
    if (T_EAR) fprintf(stderr," Postindex, new ea=%o\n", ea);

  } else {                                       /* class 2/3, ix != 11 */
    if (class == 2)
      ea = crs[S]++;
    else
      ea = --crs[S];
    if (T_EAR) fprintf(stderr," Class 2/3, new ea=%o, new S=%o\n", ea, crs[S]);
    if (x) {
      if (ea < 040)
	m = get16(0x80000000|ea);
      else
	m = get16(ea);
      if ((crs[KEYS] & 016000) == 06000)
	i = m & 0100000;
      ea = m & amask;
    }
    while (i) {
      if (ea < 040)
	m = get16(0x80000000|ea);
      else
	m = get16(ea);
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
  if (ea < 040)                                  /* flag live register ea */
    return ea | 0x80000000;
  return ea;
}


#define MAKEVA(seg,word) (((int)(seg))<<16) | (word)

ea_t ea64v (ea_t earp, unsigned short inst, short i, short x, short *opcode, short *bit) {

  ea_t ea;                                       /* full seg/word va */
  unsigned short ea_s;                           /* eff address segno */
  unsigned short ea_r;                           /* eff address ring */
  unsigned short ea_w;                           /* eff address wordno */
  unsigned short br;
  unsigned short live;                           /* max live register addr */
  unsigned short y;
  unsigned short xok;
  unsigned short a;
  unsigned short ixy;
  unsigned short m;
  unsigned short rph,rpl;

  *bit = 0;

  /* rph/rpl (and earp) are usually = RPH/RPL in the register file,
     except for the case of an XEC instruction; in that case, these
     will point to 1 after the instruction being executed */

  rph = earp >> 16;
  rpl = earp & 0xFFFF;
  //if (T_EAV) fprintf(stderr," inst=%o, rph=%o, rpl=%o\n", inst, rph, rpl);

  if (crs[MODALS] & 4)
    live = 010;
  else
    live = 040;

  ea_s = rph;
  ea_w = rpl;
  if (inst & 001000)                             /* sector bit 7 set? */
    if ((inst & 0740) != 0400) {                 /* PC relative? */
      ea_w = rpl + (((short) (inst << 7)) >> 7);   /* yes, sign extend D */
      if (T_EAV) fprintf(stderr," PC relative, P=%o, new ea_w=%o\n", rpl, ea_w);
    }
    else 
      goto labB;                                 /* special cases */
  else if (i) {
    ea_w = (inst & 0777);                        /* sector 0 */
    if (T_EAV) fprintf(stderr," Sector 0, new ea_w=%o\n", ea_w);
    if (ea_w < 0100 && x) {                      /* preindex by X */
      if (T_EAV) fprintf(stderr," Preindex, ea_w=%o, X='%o/%d\n", ea_w, crs[X], *(short *)(crs+X));
      ea_w += crs[X];
      if (T_EAV) fprintf(stderr," Preindex, new ea_w=%o\n", ea_w);
      x = 0;
    }
  } else 
    goto labA;

  if (i) {
    if (ea_w < live) {
      if (T_EAV) fprintf(stderr," Indirect through live register '%o\n", ea_w);
      ea_w = get16(0x80000000 | ea_w);
    } else {
      if (T_EAV) fprintf(stderr," Indirect, ea_s=%o, ea_w=%o\n", ea_s, ea_w);
      ea_w = get16(MAKEVA(ea_s, ea_w));
    }
    if (T_EAV) fprintf(stderr," After indirect, new ea_w=%o\n", ea_w);
  }
  if (x) {
    if (T_EAV) fprintf(stderr," Postindex, old ea_w=%o, X='%o/%d\n", ea_w, crs[X], *(short *)(crs+X));
    ea_w += crs[X];
    if (T_EAV) fprintf(stderr," Postindex, new ea_w=%o\n", ea_w);
  }

  if (ea_w < live) {
    if (T_EAV) fprintf(stderr," Live register '%o\n", ea_w);
    return 0x80000000 | ea_w;
  }
  return MAKEVA(ea_s, ea_w);


labA:
  ea_w = (inst & 0777);
  if (x) {
    if (T_EAV) fprintf(stderr," Postindex, old ea_w=%o, X='%o/%d\n", ea_w, crs[X], *(short *)(crs+X));
    ea_w += crs[X];
    if (T_EAV) fprintf(stderr," Postindex, new ea_w=%o\n", ea_w);
  }
  if ((inst & 0777) >= 0400) {
    ea_s = crs[LBH] | (ea_s & RINGMASK16);
    ea_w += crs[LBL];
    if (T_EAV) fprintf(stderr," Short LB relative, LB=%o/%o\n", crs[LBH], crs[LBL]);
    return MAKEVA(ea_s, ea_w);
  }
  if (ea_w < live) {
    if (T_EAV) fprintf(stderr," Live register '%o\n", ea_w);
    return 0x80000000 | ea_w;
  }
  ea_s = crs[SBH] | (ea_s & RINGMASK16);
  ea_w += crs[SBL];
  if (T_EAV) fprintf(stderr," Short SB relative, SB=%o/%o\n", crs[SBH], crs[SBL]);
  return MAKEVA(ea_s, ea_w);
  

  /* here for long, 2-word, V-mode memory reference */

labB:
  if (T_EAV) fprintf(stderr," 2-word format\n");
  //  x = inst & 040000;            /* get real bit 2 again for ixy?? */
  y = (inst & 020);
  ixy = ((i != 0)<<2) | ((x != 0)<<1) | (y != 0);
  xok = ((*opcode & 01700) != 01500);        /* true if indexing is okay */

  *opcode = *opcode | ((inst >> 2) & 3);         /* opcode extension */
  br = (inst & 3);
  a = get16(RP);
  if (T_EAV) fprintf(stderr," new opcode=%5#0o, y=%d, br=%d, ixy=%d, xok=%d, a=%o\n", *opcode, (y != 0), br, ixy, xok, a);
  RPL++;

  ea_s = crs[PBH+br*2] | (ea_s & RINGMASK16);
  ea_w = crs[PBL+br*2] + a;

  if (xok)
    if (ixy == 1 || ixy == 4)
      ea_w += crs[Y];
    else if (ixy == 2 || ixy == 6)
      ea_w += crs[X];

  if (ixy >= 3) {
    ea = MAKEVA(ea_s, ea_w);
    if (T_EAV) fprintf(stderr," Long indirect, ea=%o/%o, ea_s=%o, ea_w=%o\n", ea>>16, ea&0xFFFF, ea_s, ea_w);
    m = get16(ea);
    if (m & 0x8000) {
      printf(" Pointer fault, ea_s=%o, ea_w=%o, [ea]=%o\n", ea_s, ea_w, m);
      fault(POINTERFAULT, m, ea);
    }
    ea_s = m | (ea_s & RINGMASK16);
    ea_w = get16(ea+1);              /* XXX: should do 16-bit add */
    if (ea_s & EXTMASK16)
      *bit = get16(ea+2) >> 12;
    if (T_EAV) fprintf(stderr," After indirect, ea_s=%o, ea_w=%o, bit=%d\n", ea_s, ea_w, *bit);
    if (xok)
      if (ixy == 5)
	ea_w += crs[Y];
      else if (ixy == 7)
	ea_w += crs[X];
  }
  return MAKEVA(ea_s, ea_w);
}

unsigned int ea32i (ea_t earp, unsigned short inst, short i, short x) {
  if (T_EAI) fprintf(stderr,"Mode 32I not implemented\n");
}


ea_t apea(unsigned short *bitarg) {
  unsigned short ibr, ea_s, ea_w, bit, br;
  ea_t ea;
  
  ibr = get16(RP);
  RPL++;
  bit = (ibr >> 12) & 0xF;
  br = (ibr >> 8) & 3;
  if (T_EAAP) fprintf(stderr," AP ibr=%o, br=%d, i=%d, bit=%d\n", ibr, br, (ibr & 004000) != 0, bit);

  /* XXX: should ea ring be weakened with RP ring? */

  ea_s = crs[PBH + 2*br];
  ea_w = crs[PBL + 2*br];
  ea_w += get16(RP);
  RPL++;
  ea = MAKEVA(ea_s, ea_w);
  if (T_EAAP) fprintf(stderr," AP ea = %o/%o\n", ea_s, ea_w);
  if (ibr & 004000) {
    if (ea & 0x80000000)
      fault(POINTERFAULT, ea>>16, 0);
    ea = get32(ea);
    if (ea & EXTMASK32)
      bit = get16(ea+2) >> 12;
    else
      bit = 0;
    if (T_EAAP) fprintf(stderr," After indirect, AP ea = %o/%o, bit=%d\n", ea>>16, ea & 0xFFFF, bit);
  }
  if (bit)
    ea |= EXTMASK32;
  if (bitarg != NULL)
    *bitarg = bit;
  return ea;
}

#define GETFLR(n) (((crsl[FLR0+2*(n)] >> 11) & 0x1FFFE0) | (crsl[FLR0+2*(n)] & 0x1F))
#define PUTFLR(n,v) crsl[FLR0+2*(n)] = (((v) << 11) & 0xFFFF0000) | (crsl[FLR0+2*(n)] & 0xF000) | ((v) & 0x1F);

/* exception handler types:

  'i' = integer exception
  'd' = decimal exception
  'f' = floating point exception

  Depending on the keys settings, either set the C-bit or take the
  appropriate fault.

  Questions:

  1. Should the C-bit always get set, even if a fault is taken?
  2. Should the operation occur first, updating all registers, then
     the exception occurs?  (see PIMA)
*/

#define FC_SFP_OFLOW 0400
#define FC_SFP_ZDIV  0401
#define FC_SFP_STORE 0402
#define FC_SFP_INT   0403
#define FC_DFP_OFLOW 01000
#define FC_DFP_ZDIV  01001
#define FC_INT_OFLOW 01400
#define FC_INT_ZDIV  01401
#define FC_DEC_OFLOW 03400
#define FC_DEC_ZDIV  03401
#define FC_DEC_CONV  03402

mathexception(unsigned char extype, unsigned short fcode, ea_t faddr)
{
  crs[KEYS] |= 0x8000;
  switch (extype) {
  case 'i':
    if (crs[KEYS] & 0400) 
      fault(ARITHFAULT, fcode, faddr);
    break;
  case 'd':
    if (crs[KEYS] & 40)
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



memdump(int start, int end) {
  int ea;

  /* dump sector zero for debugging */

  fprintf(stderr,"\nSector 0:\n");
  for (ea=0; ea<01000; ea=ea+8)
    if (mem[ea]|mem[ea+1]|mem[ea+2]|mem[ea+3]|mem[ea+4]|mem[ea+5]|mem[ea+6]|mem[ea+7])
      fprintf(stderr,"%3o: %6o %6o %6o %6o %6o %6o %6o %6o\n", ea, mem[ea], mem[ea+1], mem[ea+2], mem[ea+3], mem[ea+4], mem[ea+5], mem[ea+6], mem[ea+7]);
    
  /* dump main memory for debugging */

  fprintf(stderr,"\nMain memory:\n");
  for (ea=start; ea<=end; ea=ea+8)
    if (mem[ea]|mem[ea+1]|mem[ea+2]|mem[ea+3]|mem[ea+4]|mem[ea+5]|mem[ea+6]|mem[ea+7])
      fprintf(stderr,"%o: %6o %6o %6o %6o %6o %6o %6o %6o\n", ea, mem[ea], mem[ea+1], mem[ea+2], mem[ea+3], mem[ea+4], mem[ea+5], mem[ea+6], mem[ea+7]);
}


dumpsegs() {
  short seg,nsegs,i,page,segno;
  unsigned short pte,xxx;
  unsigned int dtar,staddr,sdw,ptaddr,pmaddr;

  for (i=0; i<4; i++) {
    dtar = *(unsigned int *)(crs+DTAR0-2*i);  /* get dtar register */
    nsegs = 1024-(dtar>>22);
    staddr = (dtar & 0x003F0000) | ((dtar & 0xFFFF)<<1);
    fprintf(stderr,"DTAR %d: register=%o, size=%d, seg table addr=%o\n", i, dtar, nsegs, staddr);
    for (seg=0; seg<nsegs; seg++) {
      segno = (i<<10)+seg;
      sdw = *(unsigned int *)(mem+staddr);
#if 1
      /* according to mapgen.ftn (rev18):

C   CALCULATE THE MAPS PHYSICAL ADDRESS.
C
  L=LS(RT(HMAP(J,HMHM),12),4) + RS(RT((I-1)*64+HOFF,10),6)
C
C  NOW PLUG THAT ADDRESS INTO THE SDW.
C
  SDW(1)=RT(SDW(1),6) + LS(RT(L,10),6)
  SDW(2)=LT(SDW(2),10) + RS(LT(L,6),10)
      */
      ptaddr = ((sdw & 0x3F)<<10) | (sdw>>22);
#else
      ptaddr = ((sdw>>16) & 0xFFC0) | (sdw & 0x3F);
#endif
      fprintf(stderr,"Segment '%o: F=%d, R1:%o R3:%o PT = %o\n", segno, (sdw>>15)&1, (sdw>>12)&7, (sdw>>6)&7, ptaddr);
      xxx = (sdw>>16)&0x3F;
      if (xxx != 0) fprintf(stderr,"WARNING: X=%o\n", xxx);
      if (ptaddr != 0)
	for (page=0; page<64; page++) {
	  pmaddr = (ptaddr<<6) + page;
	  pte = mem[pmaddr];
	  fprintf(stderr," Seg %o page %d: pmaddr=%o, V=%d R=%d U=%d S=%d PPA=%o\n", segno, page, pmaddr, pte>>15, (pte>>14)&1, (pte>>13)&1, (pte>>12)&1, pte&0xFFF);
	}
      staddr += 2;
    }
  }
}


ea_t pclea(ea_t *rp, unsigned short *bitarg, short *store, short *lastarg) {
  unsigned short ibr, br, ea_s, ea_w, bit, a;
  ea_t xbsave;
  ea_t ea, newea;

  xbsave = *(unsigned int *)(crs+XB);
  *store = 0;
  while (!*store && !*lastarg) {
    ibr = get16(*rp);
    (*rp)++;
    a = get16(*rp);
    (*rp)++;
    bit = (ibr >> 12) & 0xF;
    *store = ibr & 0100;
    *lastarg = ibr & 0200;
    br = (ibr >> 8) & 3;
    if (T_PCL) fprintf(stderr," PCLAP ibr=%o, br=%d, i=%d, bit=%d, store=%d, lastarg=%d, a=%o\n", ibr, br, (ibr & 004000) != 0, bit, (*store != 0), (*lastarg != 0), a);
    ea_s = crs[PBH + 2*br] | (RPH & RINGMASK16);
    ea_w = crs[PBL + 2*br];
    ea_w += a;
#if 1
    /* this hack is for CPU.PCL Case 32:
4000/26434: PCL 4000/26543
 ecb @ 4000/26543, access=7
 ecb.pb: 4000/26532
 ecb.framesize: 16
 ecb.stackroot 4000
 ecb.argdisp: 12
 ecb.nargs: 1
 ecb.lb: 4000/60000
 ecb.keys: 14000
 stack free pointer: 4000/70000, current ring=0
 PCLAP ibr=11000, br=2, i=0, bit=1, store=0, lastarg=0, a=0
 PCLAP ea = 0/0
 PCLAP ibr=1700, br=3, i=0, bit=0, store=1, lastarg=1, a=0
 PCLAP ea = 10000/0
 new RP=4000/26533
    */
    if (br == 3 && (crs[XB] & EXTMASK16)) {
      bit += crs[X];
      if (bit > 15) {
	bit -= 16;
	ea_w++;
      }
      if (bit == 0)
	ea_s &= ~EXTMASK16;
    }
#endif
    ea = MAKEVA(ea_s, ea_w);
    if (bit)
      ea |= EXTMASK32;
    if (T_PCL) fprintf(stderr," PCLAP ea = %o/%o\n", ea_s, ea_w);
    if (ibr & 004000) {
      if (ea & 0x80000000)
	fault(POINTERFAULT, ea>>16, 0);
      newea = get32(ea) | (RP & RINGMASK32);
      if (T_PCL) fprintf(stderr," Indirect pointer is %o/%o\n", newea>>16, newea & 0xFFFF);
#if 1
      /* Case 37 doesn't like this, Cases 35 & 36 seem to want it.  Looks wrong to me */
      if ((newea & 0x80000000) && (newea>>16) != 0x8000)
	fault(POINTERFAULT, newea>>16, ea);
#endif
      ea = newea;
      bit = 0;
#if 0
      /* CPU.PCL Case 33 shows that the bit field is not stored in the stack frame
	 for a 3-word indirect pointer, even though the E bit remains set */
      if (ea & EXTMASK32)
	bit = get16(ea+2) >> 12;
#endif
      if (T_PCL) fprintf(stderr," After indirect, PCLAP ea = %o/%o, bit=%d\n", ea>>16, ea & 0xFFFF, bit);
    }
    if (!*store)
      *(unsigned int *)(crs+XB) = ea;
    crs[X] = bit;
  }
  if (bit) {
    ea |= EXTMASK32;
    *bitarg = bit;
  } else {
    *bitarg = 0;
  }
  /* NOTE: doing xb save/restore makes test 30 work, but 31 fails */
#if 0
  if (*store)
    *(unsigned int *)(crs+XB) = xbsave;
#endif
  return ea;
}


pcl (ea_t ea) {
  short i;
  short access;
  unsigned short ecb[9];
  short bit;                  /* bit offset for args */
  ea_t newrp;                 /* start of new proc */
  ea_t rp;                    /* return pointer */
  short stackrootseg;
  short stacksize;
  short store;                /* true if store bit set on AP */
  short storedargs;           /* # of arguments that have been stored */
  short lastarg;              /* true if "last" bit seen in PCL arglist */
  ea_t argp;                  /* where to store next arg in new frame */
  ea_t stackfp;               /* new stack frame pointer */
  pa_t pa;                    /* physical address of ecb */
  short extension;            /* true if extension pointer was used */
  ea_t xbsave;

  /* get segment access; mapva ensures either read or gate */

  pa = mapva(ea, PACC, &access);
  if (T_PCL) fprintf(stderr," ecb @ %o/%o, access=%d\n", ea>>16, ea&0xFFFF, access);

  /* get a copy of the ecb.  gates must be aligned on a 16-word
     boundary, therefore can't cross a page boundary, and mapva has
     already ensured that the ecb page is resident.  For a non-gate
     ecb, use individual memory accesses instead of memcpy.  (Could be
     optimized later by checking to see if the ecb crosses a page,
     accessing the last word, then doing the memcpy.) */

  if (access == 1) {
    if ((ea & 0xF) != 0)
      fault(ACCESSFAULT, 0, ea);
    memcpy(ecb,mem+pa,sizeof(ecb));
  } else {
    for (i=0; i<9; i++)
      ecb[i]=get16(ea+i);
  }

  /* XXX: P400 docs say "no ring change takes place if not a
     gate"; does that mean that if R0 calls a R3 ecb, it's
     still in R0, or should it be weakened to the ecb ring?
     (Case 24 of CPU.PCL indicates it should be weakened) */

  if (T_PCL) fprintf(stderr," ecb.pb: %o/%o\n ecb.framesize: %d\n ecb.stackroot %o\n ecb.argdisp: %o\n ecb.nargs: %d\n ecb.lb: %o/%o\n ecb.keys: %o\n", ecb[0], ecb[1], ecb[2], ecb[3], ecb[4], ecb[5], ecb[6], ecb[7], ecb[8]);

  newrp = *(unsigned int *)(ecb+0);
  if (access != 1)
#if 0
    newrp = (newrp & ~RINGMASK32) | (RP & RINGMASK32);  /* no ring change */
#else
    newrp = newrp | (RP & RINGMASK32);    /* Case 24 indicates to weaken ring */
#endif

  /* setup stack frame */

  stackrootseg = ecb[3];
  if (stackrootseg == 0) {
    stackrootseg = get16((*(unsigned int *)(crs+SB)) + 1);
    if (T_PCL) fprintf(stderr," stack root in ecb was zero, stack root from caller is %o\n", stackrootseg);
  }
  if (stackrootseg == 0)
    fatal("Stack base register root segment is zero");
  stackfp = get32(MAKEVA(stackrootseg,0));
  if (stackfp == 0)
    fatal("Stack free pointer is zero");
  if (T_PCL) fprintf(stderr," stack free pointer: %o/%o, current ring=%o\n", stackfp>>16, stackfp&0xFFFF, RPH&RINGMASK16);
  stacksize = ecb[2];

  /* if there isn't room for this frame, check the stack extension
     pointer */

  extension = 0;
  if ((stackfp & 0xFFFF) + stacksize > 0xFFFF) {
    stackfp = get32(MAKEVA(stackrootseg,2));
    if (T_PCL) fprintf(stderr," no room for frame, extension pointer is %o/%o\n", stackfp>>16, stackfp&0xFFFF);

    /* XXX: faddr may need to be the last segment tried when this is changed to loop.
       CPU.PCL Case 26 wants fault address word number to be 3 (?) */

    if (stackfp == 0 || (stackfp & 0xFFFF) + stacksize > 0xFFFF)
      fault(STACKFAULT, 0, MAKEVA(stackrootseg,3) | (newrp & RINGMASK32));
    extension = 1;
  }

  /* setup the new stack frame at stackfp

     NOTE: this RP store is unnecessary since it'll be updated later,
     but CPU.PCL Case 45 fails with an incorrect page fault address if
     the store is omitted.  Ring must be added to stackfp so that any
     page faults that occur while setting up the stack will have the
     correct ring for CPU.PCL tests */

  stackfp |= (newrp & RINGMASK32);   /* do this earlier?? */
  put16(0, stackfp);
  put16(stackrootseg, stackfp+1);
  put32(RP, stackfp+2);
  put32(*(unsigned int *)(crs+SB), stackfp+4);
  put32(*(unsigned int *)(crs+LB), stackfp+6);
  put16(crs[KEYS], stackfp+8);
  put16(RPL, stackfp+9);
  rp = RP;

  /* transfer arguments if arguments are expected */

  if (ecb[5] > 0) {
    xbsave = *(unsigned int *)(crs+XB);
    storedargs = 0;
    lastarg = 0;
    argp = stackfp + ecb[4];
    while (storedargs < ecb[5] || !lastarg) {
      if (lastarg) {
	ea = 0x80000000;
	store = 1;
      } else {
	ea = pclea(&rp, &bit, &store, &lastarg) | (newrp & RINGMASK32);
      }
      if (storedargs < ecb[5] && store) {
	put32(ea, argp);
	if (ea & EXTMASK32)
	  put16(bit<<12, argp+2);
	argp += 3;
	storedargs++;
      }
    }

    /* since proc has args, advance newrp past ARGT */

    if (get16(newrp) != 0605)
      printf("WARNING: proc at %o/%o has %d args but doesn't start with ARGT\n", newrp>>16, newrp&0xFFFF, ecb[5]);
    newrp++;
  }

  /* store possibly updated return pointer now */

  put32(rp, stackfp+2);

  /* update the stack free pointer; this has to wait until after all
     memory accesses, in case of stack page faults (PCL restarts).
     The ring is stored only if the extension pointer was followed.
     This is probably some goofy microcode bug: CPU.PCL Case 25 expects
     no ring number, but Case 27 does. */

  ea = MAKEVA(stackrootseg,0) | (newrp & RINGMASK32);
  if (extension)
    put32(stackfp+stacksize, ea);
  else
    put32((stackfp+stacksize) & ~RINGMASK32, ea);

  /* load new execution state from ecb */

  if (T_PCL) fprintf(stderr," before update, stackfp=%o/%o, SB=%o/%o\n", stackfp>>16, stackfp&0xFFFF, crs[SBH], crs[SBL]);
#if 0
  if (access == 1)
    /* NOTE: Case 21 shows that SB(r) should be 3 when calling from R1 to R3, but
       Case 24 shows that SB(r) should be 0 when calling from R0 to R1 (??) */

    *(unsigned int *)(crs+SB) = stackfp;
  else
    *(unsigned int *)(crs+SB) = stackfp |= newrp & RINGMASK32;
#else
  if (access == 1)
    *(unsigned int *)(crs+SB) = stackfp;
  else
    *(unsigned int *)(crs+SB) = (stackfp & ~RINGMASK32) | (RP & RINGMASK32);
#endif
  if (T_PCL) fprintf(stderr," new SB=%o/%o\n", crs[SBH], crs[SBL]);
  *(unsigned int *)(crs+LB) = *(unsigned int *)(ecb+6);
  newkeys(ecb[8] & 0177760);
  RP = newrp;
  if (T_PCL) fprintf(stderr," new RP=%o/%o\n", RPH, RPL);
}


void prtn() {
  unsigned short stackrootseg;
  ea_t newrp,newsb,newlb;
  unsigned short newkeys;

  stackrootseg = get16(*(unsigned int *)(crs+SB)+1);
  put32(*(unsigned int *)(crs+SB), MAKEVA(stackrootseg,0));
  newrp = get32(*(unsigned int *)(crs+SB)+2);
  newsb = get32(*(unsigned int *)(crs+SB)+4);
  newlb = get32(*(unsigned int *)(crs+SB)+6);
  newkeys = get16(*(unsigned int *)(crs+SB)+8);
  RP = newrp | (RP & RINGMASK32);
  *(unsigned int *)(crs+SB) = newsb;
  *(unsigned int *)(crs+LB) = newlb;
  crs[KEYS] = newkeys & 0177760;
  if (T_INST) fprintf(stderr," Finished PRTN, RP=%o/%o\n", RPH, RPL);
}

main (int argc, char **argv) {

  short tempa,tempa1,tempa2;
  unsigned short utempa;
  int templ,templ1,templ2;
  long long templl;
  unsigned int utempl,utempl1,utempl2;
  float tempf,tempf1,tempf2;
  double tempd,tempd1,tempd2;
  unsigned short tempda[4],tempda1[4];
  unsigned int ea32;                   /* full V/I mode eff address */
  ea_t ea;                             /* final MR effective address */
  ea_t earp;                           /* RP to use for eff address calcs */
  unsigned short eabit;
  unsigned short opcode;
  short i,j,x;
  unsigned short savemask;
  unsigned short class;
  int nw;
  unsigned short rvec[9];    /* SA, EA, P, A, B, X, keys, dummy, dummy */
  unsigned short inst;
  unsigned short m,m1,m2;
  unsigned short qtop,qbot,qseg,qmask,qtemp;
  ea_t qea;
  int scount;                          /* shift count */
  unsigned short trapvalue;
  ea_t trapaddr;

  /* master clear:
     - clear all registers
     - set register set to 2
     - set P to '1000
     - 16S mode, single precision
     - interrupts and machine checks inhibited
     - standard interrupt mode
  */

  for (i=0; i < 32*REGSETS; i++)
    regs.u32[i] = 0;
  crs = (void *)regs.rs[2];
  crsl = (void *)crs;
  crs[MODALS] = 000100;         /* boot w/ CURREG=2 (current register set) */
  newkeys(0);
  RPL = 01000;

  verbose = 0;
  domemdump = 0;
  boot = 0;

  /* check args */

  for (i=1; i<argc; i++) {
    if (strcmp(argv[i],"--vv") == 0)
      verbose = 2;
    else if (strcmp(argv[i],"--v") == 0)
      verbose = 1;
    else if (strcmp(argv[i],"--memdump") == 0)
      domemdump = 1;
    else if (strcmp(argv[i],"--ss") == 0) {
      if (i+1 < argc && argv[i+1][0] != '-') {
	sscanf(argv[i+1],"%o", &templ);
	sswitch = templ;
      } else
	sswitch = 0;
    } else if (strcmp(argv[i],"--trace") == 0)
      while (i+1 < argc && argv[i+1][0] != '-') {
	if (strcmp(argv[i+1],"ear") == 0)
	  traceflags |= TB_EAR;
	else if (strcmp(argv[i+1],"eav") == 0)
	  traceflags |= TB_EAV;
	else if (strcmp(argv[i+1],"eai") == 0)
	  traceflags |= TB_EAI;
	else if (strcmp(argv[i+1],"inst") == 0)
	  traceflags |= TB_INST;
	else if (strcmp(argv[i+1],"flow") == 0)
	  traceflags |= TB_FLOW;
	else if (strcmp(argv[i+1],"mode") == 0)
	  traceflags |= TB_MODE;
	else if (strcmp(argv[i+1],"eaap") == 0)
	  traceflags |= TB_EAAP;
	else if (strcmp(argv[i+1],"dio") == 0)
	  traceflags |= TB_DIO;
	else if (strcmp(argv[i+1],"map") == 0)
	  traceflags |= TB_MAP;
	else if (strcmp(argv[i+1],"pcl") == 0)
	  traceflags |= TB_PCL;
	else if (strcmp(argv[i+1],"all") == 0)
	  traceflags = -1;
	else
	  fprintf(stderr,"Unrecognized trace flag: %s\n", argv[i+1]);
	i++;
      }
    else if (strcmp(argv[i],"--boot") == 0)
      boot = 1;
    else if (argv[i][0] == '-' && argv[i][1] == '-')
      fprintf(stderr,"Unrecognized argument: %s\n", argv[i]);
  }

  fprintf(stderr,"Sense switches set to %o\n", sswitch);

  os_init();

  if (boot) {
    rvec[0] = 0760;
    rvec[1] = 0760+1040-1;
    rvec[2] = 01000;
    rvec[3] = rvec[4] = rvec[5] = 0;
    rvec[6] = 0;
    /* setup DMA register '21 for the next boot record */
    regs.sym.regdmx[041] = 03000;

  } else {

    /* read 9-word rvec header */

    for (i=0; i<9; i++)
      rvec[i] = readshort();
    if (T_FLOW) fprintf(stderr,"SA=%o, EA=%o, P=%o, A=%o, B=%o, X=%o, K=%o\n\n", rvec[0], rvec[1],
	    rvec[2], rvec[3], rvec[4], rvec[5], rvec[6]);
    if (rvec[2] > rvec[1])
      fatal("Program start > EA: runfile is trashed");
  }

  /* read memory image from SA to EA inclusive */

  nw = rvec[1]-rvec[0]+1;
  if (fread(mem+rvec[0], sizeof(short), nw, stdin) != nw) {
    perror("Error reading memory image");
    fatal(NULL);
  }

  /* setup execution (registers, keys, address mask, etc.) from rvec */

  crs[A] = rvec[3];
  crs[B] = rvec[4];
  crs[X] = rvec[5];
  newkeys(rvec[6]);
  RPL = rvec[2];

  if (RPL == 0161000)      /* hack for *DOS64; P is off by 3?? */
    RPL = 0161003;

  if (domemdump)
    memdump(rvec[0], rvec[1]);

  /* main instruction decode loop */

  trapaddr = 0144003;
  trapvalue = -12345;
  trapaddr = 0;

  if (setjmp(jmpbuf))
    ;

  while (1) {

    if (trapaddr != 0 && mem[trapaddr] != trapvalue) {
      printf("TRAP: at #%d, old value of '%o was %o; new value is %o\n", instcount, trapaddr, trapvalue, mem[trapaddr]);
      trapvalue = mem[trapaddr];
    }
#if 0
    if (*(int *)(crs+XB) != 0) {
      fprintf(stderr, "TRAP: at #%d, XB% changed to %o/%o\n", instcount, crs[XBH], crs[XBL]);
    }
#endif
    prevpc = RP;
    inst = get16(RP);
    RPL++;
    //m = get16(RP);   /* this is dangerous and should be removed! */
    m = -1;

    /* while a process is running, RP is the real program counter, PBH
       is the active procedure segment, and PBL is zero.  When a
       process stops running, RP is copied to PB.  When a process
       starts running again, PB is copied to RP.  See seg14.pma,
       WRMSAV. */

    crs[PBH] = RPH;
    crs[PBL] = 0;
    earp = RP;

xec:
    instcount++;
    if (T_FLOW) fprintf(stderr,"\n%o/%o: %o %o		A='%o/%:0d B='%o/%d X=%o/%d Y=%o/%d C=%d L=%d LT=%d EQ=%d	#%d\n", RPH, RPL-1, inst, m, crs[A], *(short *)(crs+A), crs[B], *(short *)(crs+B), crs[X], *(short *)(crs+X), crs[Y], *(short *)(crs+Y), (crs[KEYS]&0100000) != 0, (crs[KEYS]&040000) != 0, (crs[KEYS]&0200) != 0, (crs[KEYS]&0100) != 0, instcount);

    /* generic? */

    if ((inst & 036000) == 0) {
      if (T_INST) fprintf(stderr," generic\n");
      class = inst>>14;
      if (class == 0) {
	if (T_INST) fprintf(stderr," generic class 0\n");

	if (inst == 000005) {                 /* SGL */
	  if (T_FLOW) fprintf(stderr," SGL\n");
	  newkeys(crs[KEYS] & ~040000);
	  continue;
	}

	if (inst == 000011) {                 /* E16S */
	  if (T_FLOW) fprintf(stderr," E16S\n");
	  newkeys(crs[KEYS] & 0161777);
	  continue;
	}

	if (inst == 000013) {                 /* E32S */
	  if (T_FLOW) fprintf(stderr," E32S\n");
	  newkeys((crs[KEYS] & 0161777) | 1<<10);
	  continue;
	}

	if (inst == 001013) {                 /* E32R */
	  if (T_FLOW) fprintf(stderr," E32R\n");
	  newkeys((crs[KEYS] & 0161777) | 3<<10);
	  continue;
	}

	if (inst == 001011) {                 /* E64R */
	  if (T_FLOW) fprintf(stderr," E64R\n");
	  newkeys((crs[KEYS] & 0161777) | 2<<10);
	  continue;
	}

	if (inst == 000010) {                 /* E64V */
	  if (T_FLOW) fprintf(stderr," E64V\n");
	  newkeys((crs[KEYS] & 0161777) | 6<<10);
	  continue;
	}

	if (inst == 001010) {                 /* E32I */
	  if (T_FLOW) fprintf(stderr," E32I\n");
	  fault(RESTRICTFAULT, 0, 0);
	  newkeys((crs[KEYS] & 0161777) | 4<<10);
	  continue;
	}

	if (inst == 000505) {                 /* SVC */
	  if (T_FLOW) fprintf(stderr," SVC\n");
#if 1
	  fault(SVCFAULT, 0, 0);
#else
	  svc();
#endif
	  continue;
	}

	if (inst == 000111) {                  /* CEA */
	  if (T_FLOW) fprintf(stderr," CEA\n");
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
		m = get16(ea);
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
		crs[A] = get16(ea);
	    }
	  }
	  continue;
	}

	if (inst == 000000) {
	  if (T_FLOW) fprintf(stderr," HLT\n");
	  fatal("Program halt");
	}

	if (inst == 000201) {
	  if (T_FLOW) fprintf(stderr," IAB\n");
	  tempa = crs[B];
	  crs[B] = crs[A];
	  crs[A] = tempa;
	  continue;
	}

	if (inst == 000205) {                /* PIM (R-mode) */
	  if (T_FLOW) fprintf(stderr," PIM\n");
	  crs[A] = crs[B] | (crs[A] & 0x8000);
	  continue;
	}

	if (inst == 000211) {                /* PID (R-mode) */
	  if (T_FLOW) fprintf(stderr," PID\n");
	  *(int *)(crs+L) = *(short *)(crs+A);
	  crs[B] &= 0x7fff;
	  continue;
	}

	/* DBL activates 31-bit mode (R-mode only):

	   LDA -> DLD (double load)
	   STA -> DST (double store)
	   ADD -> DAD (double add)
	   SUB -> DSB (double subtract)

	   Other R-mode, 31-bit instructions include:
	   
	   PID, DIV, MPY, PIM, INT, FLOT
	*/

	if (inst == 000007) {                 /* DBL */
	  if (T_FLOW) fprintf(stderr," DBL\n");
	  newkeys(crs[KEYS] | 040000);
	  continue;
	}

	/* NOTE: this should do 16-bit adds on RPL in get16 calls
	   rather than 32-bit adds */

	if (inst == 001314) {
	  if (T_FLOW) fprintf(stderr," CGT\n");
	  tempa = get16(RP);              /* get number of words */
	  if (1 <= crs[A] && crs[A] < tempa)
	    RPL = get16(RP+crs[A]);
	  else
	    RPL += tempa;
	  continue;
	}

	if (inst == 000115) {
	  if (T_FLOW) fprintf(stderr," PIDA\n");
	  *(int *)(crs+L) = *(short *)(crs+A);
	  continue;
	}

	if (inst == 000305) {
	  if (T_FLOW) fprintf(stderr," PIDL\n");
	  *(int *)(crs+E) = *(int *)(crs+L);
	  *(int *)(crs+L) = (int)(*(int *)(crs+L) & 0x80000000) >> 31;
	  continue;
	}

	/* XXX: how does PIMA affect registers when overflow occurs?
	   NOTE: PMA manual says copy B reg to A reg, but DIAG seems
	   to indicate a swap */

	if (inst == 000015) {
	  if (T_FLOW) fprintf(stderr," PIMA\n");
	  tempa = crs[B];
	  crs[B] = crs[A];
	  crs[A] = tempa;
	  SETL(0);
	  SETCC_A;
	  if (((crs[A] ^ crs[B]) & 0x8000) || (crs[B] != 0 && crs[B] != 0xFFFF))
	    mathexception('i', FC_INT_OFLOW, 0);
	  else
	    CLEARC;
	  continue;
	}

	if (inst == 000301) {
	  if (T_FLOW) fprintf(stderr," PIML\n");
	  *(int *)(crs+L) = *(int *)(crs+E);
	  SETL(0);
	  SETCC_L;
	  if (((crs[L] ^ crs[E]) & 0x8000) || (*(int *)(crs+L) != 0 && *(int *)(crs+L) != -1))
	    mathexception('i', FC_INT_OFLOW, 0);
	  else
	    CLEARC;
	  continue;
	}

	if (inst == 001300) {
	  if (T_FLOW) fprintf(stderr," EAFA 0\n");
	  ea = apea(&eabit);
	  crsl[FAR0] = ea;
	  crsl[FLR0] = (crs[FLR0] & 0xFFFF0FFF) | (eabit << 12);
	  if (T_INST) fprintf(stderr," FAR=%o, eabit=%d, FLR=%x\n", crsl[FAR0], eabit, crsl[FLR0]);
	  continue;
	}

	if (inst == 001310) {
	  if (T_FLOW) fprintf(stderr," EAFA 1\n");
	  ea = apea(&eabit);
	  crsl[FAR1] = ea;
	  crsl[FLR1] = (crs[FLR1] & 0xFFFF0FFF) | (eabit << 12);
	  if (T_INST) fprintf(stderr," FAR=%o, eabit=%d, FLR=%x\n", crsl[FAR1], eabit, crsl[FLR1]);
	  continue;
	}

	if (inst == 001301) {
	  if (T_FLOW) fprintf(stderr," ALFA 0\n");
	  utempl = ((crsl[FAR0] & 0xFFFF) << 4) | ((crsl[FLR0] >> 12) & 0xF);
	  utempl += *(int *)(crs+L);
	  crsl[FAR0] = (crsl[FAR0] & 0xFFFF0000) | ((utempl >> 4) & 0xFFFF);
	  crsl[FLR0] = (crsl[FLR0] & 0xFFFF0FFF) | ((utempl & 0xF) << 12);
	  continue;
	}

	if (inst == 001311) {
	  if (T_FLOW) fprintf(stderr," ALFA 1\n");
	  utempl = ((crsl[FAR1] & 0xFFFF) << 4) | ((crsl[FLR1] >> 12) & 0xF);
	  utempl += *(int *)(crs+L);
	  crsl[FAR1] = (crsl[FAR1] & 0xFFFF0000) | ((utempl >> 4) & 0xFFFF);
	  crsl[FLR1] = (crsl[FLR1] & 0xFFFF0FFF) | ((utempl & 0xF) << 12);
	  continue;
	}

	if (inst == 001303) {
	  if (T_FLOW) fprintf(stderr," LFLI 0\n");
#if 0
	  for (utempa=0; utempa<256; utempa++) {
	    PUTFLR(0,utempa);
	    crsl[FLR0] |= 0x4000;
	    utempl = GETFLR(0);
	    if (utempa != utempl) {
	      fprintf(stderr," loaded %d, fetched %d\n", utempa, utempl);
	      exit(1);
	    }
	  }
#endif
	  utempa = get16(RP);
	  RPL++;
	  PUTFLR(0,utempa);
	  utempl = GETFLR(0);
	  if (T_INST) fprintf(stderr," Load Field length with %d, FLR=%x, actual = %d\n", utempa, crsl[FLR0], utempl);
	  if (utempa != utempl)
	    fatal("LFLI 0 error");
	  continue;
	}

	if (inst == 001313) {
	  if (T_FLOW) fprintf(stderr," LFLI 1\n");
#if 0
	  for (utempa=0; utempa<256; utempa++) {
	    PUTFLR(1,utempa);
	    utempl = GETFLR(1);
	    if (utempa != utempl) {
	      fprintf(stderr," loaded %d, fetched %d\n", utempa, utempl);
	      exit(1);
	    }
	  }
#endif
	  utempa = get16(RP);
	  RPL++;
	  PUTFLR(1,utempa);
	  utempl = GETFLR(1);
	  if (T_INST) fprintf(stderr," Load Field length with %d, FLR=%x, actual = %d\n", utempa, crsl[FLR1], utempl);
	  if (utempa != utempl)
	    fatal("LFLI 0 error");
	  continue;
	}

	if (inst == 001320) {
	  if (T_FLOW) fprintf(stderr," STFA 0\n");
	  ea = apea(NULL);
	  utempl = crsl[FAR0] & 0x6FFFFFFF;
	  utempa = crsl[FLR0] & 0xF000;
stfa:
	  if (utempa != 0) {
	    utempl = utempl | EXTMASK32;
	    put16(utempa,ea+2);
	  }
	  put32(utempl,ea);
	  continue;
	}


	if (inst == 001330) {
	  if (T_FLOW) fprintf(stderr," STFA 1\n");
	  ea = apea(NULL);
	  utempl = crsl[FAR1] & 0x6FFFFFFF;
	  utempa = crsl[FLR1] & 0xF000;
	  goto stfa;
	}

	if (inst == 001321) {
	  if (T_FLOW) fprintf(stderr," TLFL 0\n");
	  PUTFLR(0,*(unsigned int *)(crs+L));
	  continue;
	}

	if (inst == 001331) {
	  if (T_FLOW) fprintf(stderr," TLFL 1\n");
	  PUTFLR(1,*(unsigned int *)(crs+L));
	  continue;
	}


	if (inst == 001323) {
	  if (T_FLOW) fprintf(stderr," TFLL 0\n");
	  *(unsigned int *)(crs+L) = GETFLR(0);
	  continue;
	}

	if (inst == 001333) {
	  if (T_FLOW) fprintf(stderr," TFLL 1\n");
	  *(unsigned int *)(crs+L) = GETFLR(1);
	  continue;
	}
	
	if (inst == 001302) {
	  if (T_FLOW) fprintf(stderr," LDC 0\n");
	  utempl = GETFLR(0);
	  if (T_INST) fprintf(stderr," Before load, FLR=%d\n", utempl);
	  if (utempl > 0) {
	    m = get16(crsl[FAR0]);
	    if (crsl[FLR0] & 0x8000) {
	      if (T_INST) fprintf(stderr," Fetching right byte\n");
	      crs[A] = m & 0xFF;
	      crsl[FLR0] &= 0xFFFF0FFF;
	      crsl[FAR0] = (crsl[FAR0]+1) & 0x6FFFFFFF;  /* sb 16-bit add */
	    } else {
	      if (T_INST) fprintf(stderr," Fetching left byte\n");
	      crs[A] = m >> 8;
	      crsl[FLR0] |= 0x8000;
	    }
	    utempl--;
	    PUTFLR(0,utempl);
	    crs[KEYS] &= ~0100;      /* reset EQ */
	    if (T_INST) fprintf(stderr," Loaded character %c, FLR=%d, actual=%d\n", crs[A] & 0x7f, utempl, GETFLR(0));
	  } else if (utempl == 0) {
	    crs[A] = 0;
	    crs[KEYS] |= 0100;      /* set EQ */
	  }
	  continue;
	}

	if (inst == 001312) {
	  if (T_FLOW) fprintf(stderr," LDC 1\n");
	  utempl = GETFLR(1);
	  if (utempl > 0) {
	    m = get16(crsl[FAR1]);
	    if (crsl[FLR1] & 0x8000) {
	      crs[A] = m & 0xFF;
	      crsl[FLR1] &= 0xFFFF0FFF;
	      crsl[FAR1] = (crsl[FAR1]+1) & 0x6FFFFFFF;
	    } else {
	      crs[A] = m >> 8;
	      crsl[FLR1] |= 0x8000;
	    }
	    utempl--;
	    PUTFLR(1,utempl);
	    crs[KEYS] &= ~0100;      /* reset EQ */
	  } else if (utempl == 0) {
	    crs[A] = 0;
	    crs[KEYS] |= 0100;      /* set EQ */
	  }
	  continue;
	}

	if (inst == 001322) {
	  if (T_FLOW) fprintf(stderr," STC 0\n");
	  utempl = GETFLR(0);
	  if (utempl > 0) {
	    m = get16(crsl[FAR0]);
	    if (crsl[FLR0] & 0x8000) {
	      m = (m & 0xFF00) | (crs[A] & 0xFF);
	      put16(m,crsl[FAR0]);
	      crsl[FLR0] &= 0xFFFF0FFF;
	      crsl[FAR0] = (crsl[FAR0]+1) & 0x6FFFFFFF;
	    } else {
	      m = (m & 0xFF) | (crs[A] << 8);
	      put16(m,crsl[FAR0]);
	      crsl[FLR0] |= 0x8000;
	    }
	    utempl--;
	    PUTFLR(0,utempl);
	    crs[KEYS] &= ~0100;      /* reset EQ */
	  } else if (utempl == 0) {
	    crs[A] = 0;
	    crs[KEYS] |= 0100;      /* set EQ */
	  }
	  continue;
	}
	    
	if (inst == 001332) {
	  if (T_FLOW) fprintf(stderr," STC 1\n");
	  utempl = GETFLR(1);
	  if (T_INST) fprintf(stderr," Before store, FLR=%d\n", utempl);
	  if (utempl > 0) {
	    m = get16(crsl[FAR1]);
	    if (crsl[FLR1] & 0x8000) {
	      if (T_INST) fprintf(stderr," Storing right byte\n");
	      m = (m & 0xFF00) | (crs[A] & 0xFF);
	      put16(m,crsl[FAR1]);
	      crsl[FLR1] &= 0xFFFF0FFF;
	      crsl[FAR1] = (crsl[FAR1]+1) & 0x6FFFFFFF;
	    } else {
	      if (T_INST) fprintf(stderr," Storing left byte\n");
	      m = (m & 0xFF) | (crs[A] << 8);
	      put16(m,crsl[FAR1]);
	      crsl[FLR1] |= 0x8000;
	    }
	    utempl--;
	    PUTFLR(1,utempl);
	    if (T_INST) fprintf(stderr," After store, FLR=%d\n", GETFLR(1));
	    crs[KEYS] &= ~0100;      /* reset EQ */
	  } else if (utempl == 0) {
	    crs[A] = 0;
	    crs[KEYS] |= 0100;      /* set EQ */
	  }
	  continue;
	}

	if (inst == 000611) {
	  //traceflags = ~TB_MAP;
	  if (T_FLOW) fprintf(stderr," PRTN\n");
	  prtn();
	  continue;
	}

	if (inst == 000041) {
	  if (T_FLOW) fprintf(stderr," SCA\n");
	  crs[A] = crs[VSC] & 0xFF;
	  continue;
	}

	if (inst == 000043) {
	  if (T_FLOW) fprintf(stderr," INK\n");
	  crs[A] = (crs[KEYS] & 0xFF00) | (crs[VSC] & 0xFF);
	  continue;
	}

	if (inst == 001005) {
	  if (T_FLOW) fprintf(stderr," TKA\n");
	  //printf("TKA: %o -> A\n", crs[KEYS]);
	  crs[A] = crs[KEYS];
	  continue;
	}

	if (inst == 000405) {
	  if (T_FLOW) fprintf(stderr," OTK\n");
	  newkeys((crs[A] & 0xFF00) | (crs[KEYS] & 0xFF));
	  crs[VSC] = crs[A] & 0xFF;
	  continue;
	}

	if (inst == 001015) {
	  if (T_FLOW) fprintf(stderr," TAK\n");
	  //printf("TAK: %o -> KEYS\n", crs[A]);
	  newkeys(crs[A] & 0177760);
	  continue;
	}

	if (inst == 000001) {
	  if (T_FLOW) fprintf(stderr," NOP\n");
	  continue;
	}

	if (inst == 000715) {
	  if (T_FLOW) fprintf(stderr," RSAV\n");
	  ea = apea(NULL);
	  j = 1;
	  savemask = 0;
	  for (i = 11; i >= 0; i--) {
	    if (crsl[i] != 0) {
	      if (T_INST) fprintf(stderr," crsl[%d] saved, value=%o\n", i, crsl[i]);
	      put32(crsl[i], ea+j);
	      savemask |= bitmask16[16-i];
	    }
	    j += 2;
	  }
	  put32(*(int *)(crs+XB), ea+25);
	  put16(savemask, ea);
	  if (T_INST) fprintf(stderr," Saved, mask=%o\n", savemask);
	  continue;
	}

	if (inst == 000717) {
	  if (T_FLOW) fprintf(stderr," RRST\n");
	  ea = apea(NULL);
	  savemask = get16(ea);
	  //if (T_INST) fprintf(stderr," %o/%o: RRST %o/%o, mask=%o, modals=%o\n", RPH,RPL,ea>>16,ea&0xffff,savemask,crs[MODALS]);
	  if (T_INST) fprintf(stderr," Save mask=%o\n", savemask);
	  j = 1;
	  for (i = 11; i >= 0; i--) {
	    if (savemask & bitmask16[16-i]) {
	      crsl[i] = get32(ea+j);
	      if (T_INST) fprintf(stderr," crsl[%d] restored, value=%o\n", i, crsl[i]);
	    } else {
	      crsl[i] = 0;
	    }
	    j += 2;
	  }
	  *(unsigned int *)(crs+XB) = get32(ea+25);
	  if (T_INST) fprintf(stderr," XB restored, value=%o/%o\n", crs[XBH], crs[XBL]);
	  continue;
	}

	if (inst == 000101) {
	  if (T_FLOW) fprintf(stderr," NRM\n");
	  crs[VSC] = 0;
	  if (crs[A] == 0 && crs[B] == 0)
	    continue;
	  while (!((crs[A] ^ (crs[A] << 1)) & 0x8000)) {
	    if (T_INST) fprintf(stderr, " step %d: crs[A]=%o, crs[B]=%o\n", crs[VSC], crs[A], crs[B]);
	    crs[B] = crs[B] << 1;
	    crs[A] = (crs[A] & 0x8000) | ((crs[A] << 1) & 0x7FFE) | (crs[B] >> 15);
	    crs[VSC]++;
	  }
	  crs[B] &= 0x7FFF;
	  if (T_INST) fprintf(stderr, " finished with %d shifts: crs[A]=%o, crs[B]=%o\n", crs[VSC], crs[A], crs[B]);
	  continue;
	}

	if (000400 <= inst && inst <= 000402) {
	  if (T_FLOW) fprintf(stderr," ENB\n");
	  crs[MODALS] &= ~0100000;
	  continue;
	}

	if (001000 <= inst && inst <= 001002) {
	  if (T_FLOW) fprintf(stderr," INH\n");
	  crs[MODALS] |= 0100000;
	  continue;
	}

	if (inst == 01200) {
	  if (T_FLOW) fprintf(stderr," STAC\n");
	  ea = apea(NULL);
	  if (get16(ea) == crs[B]) {
	    put16(crs[A], ea);
	    crs[KEYS] |= 0100;       /* set EQ */
	  } else 
	    crs[KEYS] &= ~0100;      /* reset EQ */
	  continue;
	}

	if (inst == 01204) {
	  if (T_FLOW) fprintf(stderr," STLC\n");
	  ea = apea(NULL);
	  if (get32(ea) == *(int *)(crs+E)){
	    put32(*(int *)(crs+L), ea);
	    crs[KEYS] |= 0100;       /* set EQ */
	  } else 
	    crs[KEYS] &= ~0100;      /* reset EQ */
	  continue;
	}

	if (inst == 000415) {
	  if (T_FLOW) fprintf(stderr," ESIM\n");
	  crs[MODALS] &= ~040000;
	  continue;
	}

	if (inst == 000417) {
	  if (T_FLOW) fprintf(stderr," EVIM\n");
	  crs[MODALS] |= 040000;
	  continue;
	}

	if (inst == 000711) {
	  if (T_FLOW) fprintf(stderr," LPSW\n");
	  ea = apea(NULL);
	  RPH = get16(ea);
	  RPL = get16(ea+1);
	  newkeys(get16(ea+2));
	  crs[MODALS] = get16(ea+3);
	  if (1 || T_INST) fprintf(stderr," RPH=%o, RPL=%o, keys=%o, modals=%o\n", RPH, RPL, crs[KEYS], crs[MODALS]);
	  if (crs[MODALS] & 010) {
	    printf("Process exchange enabled:\n");
	    traceflags = -1;
	    printf(" OWNERH=%o, PLA=%o, PCBA=%o, PLB=%o, PCBB=%o\n", crs[OWNERH], regs.sym.pla, regs.sym.pcba, regs.sym.plb, regs.sym.pcbb);
	    for (i=regs.sym.pla;; i += 2) {
	      ea = MAKEVA(crs[OWNERH], i);
	      utempa = get16(ea);
	      printf(" Level %o: BOL=%o, EOL=%o\n", i, utempa, get16(ea+1));
	      if (utempa == 1)
		break;
	    }
	  }
	  if (crs[MODALS] & 020)
	    printf("Mapped I/O enabled\n");
	  if (crs[MODALS] & 4) {
	    printf("Segmentation enabled\n");
	    if (domemdump) dumpsegs();
	    //traceflags = ~TB_MAP;
	  }
#if 0
	  /* XXX: hack to disable serial number checking if E32I is enabled.
	     Look for ERA/ANA sequence after illegal shift instruction, set
	     the ANA operand to zero. */

	  ea = MAKEVA(014,040747);
	  put16(0,ea);
#endif
	  continue;
	}

	if (inst == 000105) {
	  if (T_FLOW) fprintf(stderr," RTN\n");
	  m = get16(crs[S]+1);
	  if (m == 0)
	    fatal("RTN stack underflow");
	  crs[S] = get16(crs[S]);
	  continue;
	}

	/* Decimal and character instructions */

	if (001100 <= inst && inst <= 001146) {
	  //traceflags = -1;
	  if (T_FLOW) fprintf(stderr," X/Z UII %o\n", inst);
	  fault(UIIFAULT, RPL, 0);
	  continue;
	}

	if (inst == 001702) {
	  if (T_FLOW) fprintf(stderr," IDLE?\n", inst);
	  fatal("IDLE loop");
	  continue;
	}

	/* unusual restricted instructions */

	for (i=0; i<GEN0TABSIZE; i++) {
	  if (inst == gen0tab[i]) {
	    if (T_FLOW) fprintf(stderr," %s\n", gen0nam[i]);
	    break;
	  }
	}
	if (i < GEN0TABSIZE)
	  continue;

	if (T_FLOW) fprintf(stderr," unrecognized generic class 0 instruction!\n");
	printf(" unrecognized generic class 0 instruction %o!\n", inst);
#if 0
	m = get16(066);
	if (m != 0) {
	  if (T_FLOW) fprintf(stderr," JST* '66 [%o]\n", m);
	  put16(RPL,m);
	  RPL = m+1;
	  continue;
	}
#endif
	fatal(NULL);
	
      }

      if (class == 3) {
	if (T_INST) fprintf(stderr," generic class 3\n");

	if (inst == 0141604) {
	  if (T_FLOW) fprintf(stderr," BCLT\n");
bclt:
	  if (crs[KEYS] & 0200)
	    RPL = get16(RP);
	  else
	    RPL++;
	  continue;
	}

	if (inst == 0141600) {
	  if (T_FLOW) fprintf(stderr," BCLE\n");
bcle:
	  if (crs[KEYS] & 0300)
	    RPL = get16(RP);
	  else
	    RPL++;
	  continue;
	}

	if (inst == 0141602) {
	  if (T_FLOW) fprintf(stderr," BCEQ\n");
bceq:
	  if (crs[KEYS] & 0100)
	    RPL = get16(RP);
	  else
	    RPL++;
	  continue;
	}

	if (inst == 0141603) {
	  if (T_FLOW) fprintf(stderr," BCNE\n");
bcne:
	  if (!(crs[KEYS] & 0100))
	    RPL = get16(RP);
	  else
	    RPL++;
	  continue;
	}

	if (inst == 0141605) {
	  if (T_FLOW) fprintf(stderr," BCGE\n");
bcge:
	  if (!(crs[KEYS] & 0200))
	    RPL = get16(RP);
	  else
	    RPL++;
	  continue;
	}

	if (inst == 0141601) {
	  if (T_FLOW) fprintf(stderr," BCGT\n");
bcgt:
	  if (!(crs[KEYS] & 0300))
	    RPL = get16(RP);
	  else
	    RPL++;
	  continue;
	}

	if (inst == 0141705) {
	  if (T_FLOW) fprintf(stderr," BCR\n");
	  if (!(crs[KEYS] & 0100000))
	    RPL = get16(RP);
	  else
	    RPL++;
	  continue;
	}

	if (inst == 0141704) {
	  if (T_FLOW) fprintf(stderr," BCS\n");
	  if (crs[KEYS] & 0100000)
	    RPL = get16(RP);
	  else
	    RPL++;
	  continue;
	}

	if (inst == 0141707) {
	  if (T_FLOW) fprintf(stderr," BLR\n");
	  if (!(crs[KEYS] & 020000))
	    RPL = get16(RP);
	  else
	    RPL++;
	  continue;
	}

	if (inst == 0141706) {
	  if (T_FLOW) fprintf(stderr," BLS\n");
	  if (crs[KEYS] & 020000)
	    RPL = get16(RP);
	  else
	    RPL++;
	  continue;
	}

	if (inst == 0140614) {
	  if (T_FLOW) fprintf(stderr," BLT\n");
	  SETCC_A;
	  goto bclt;
	}

	if (inst == 0140610) {
	  if (T_FLOW) fprintf(stderr," BLE\n");
	  SETCC_A;
	  goto bcle;
	}

	if (inst == 0140612) {
	  if (T_FLOW) fprintf(stderr," BEQ\n");
	  SETCC_A;
	  goto bceq;
	}

	if (inst == 0140613) {
	  if (T_FLOW) fprintf(stderr," BNE\n");
	  SETCC_A;
	  goto bcne;
	}

	if (inst == 0140615) {
	  if (T_FLOW) fprintf(stderr," BGE\n");
	  SETCC_A;
	  goto bcge;
	}

	if (inst == 0140611) {
	  if (T_FLOW) fprintf(stderr," BGT\n");
	  SETCC_A;
	  goto bcgt;
	  continue;
	}

	if (inst == 0140700) {
	  if (T_FLOW) fprintf(stderr," BLLE\n");
	  SETCC_L;
	  goto bcle;
	}

	if (inst == 0140702) {
	  if (T_FLOW) fprintf(stderr," BLEQ\n");
	  SETCC_L;
	  goto bceq;
	}

	if (inst == 0140703) {
	  if (T_FLOW) fprintf(stderr," BLNE\n");
	  SETCC_L;
	  goto bcne;
	}

	if (inst == 0140701) {
	  if (T_FLOW) fprintf(stderr," BLGT\n");
	  SETCC_L;
	  goto bcgt;
	}

	if (inst == 0141614) {
	  if (T_FLOW) fprintf(stderr," BFLT\n");
	  SETCC_F;
	  goto bclt;
	}

	if (inst == 0141610) {
	  if (T_FLOW) fprintf(stderr," BFLE\n");
	  SETCC_F;
	  goto bcle;
	}

	if (inst == 0141612) {
	  if (T_FLOW) fprintf(stderr," BFEQ\n");
	  SETCC_F;
	  goto bceq;
	}

	if (inst == 0141613) {
	  if (T_FLOW) fprintf(stderr," BFNE\n");
	  SETCC_F;
	  goto bcne;
	}

	if (inst == 0141615) {
	  if (T_FLOW) fprintf(stderr," BFGE\n");
	  SETCC_F;
	  goto bcge;
	}

	if (inst == 0141611) {
	  if (T_FLOW) fprintf(stderr," BFGT\n");
	  SETCC_F;
	  goto bcgt;
	}

	if (inst == 0141334) {
	  if (T_FLOW) fprintf(stderr," BIX\n");
	  crs[X]++;
bidx:
	  if (crs[X] != 0)
	    RPL = get16(RP);
	  else
	    RPL++;
	  continue;
	}

	if (inst == 0141324) {
	  if (T_FLOW) fprintf(stderr," BIY\n");
	  crs[Y]++;
bidy:
	  if (crs[Y] != 0)
	    RPL = get16(RP);
	  else
	    RPL++;
	  continue;
	}

	if (inst == 0140724) {
	  if (T_FLOW) fprintf(stderr," BDY\n");
	  crs[Y]--;
	  goto bidy;
	}

	if (inst == 0140734) {
	  if (T_FLOW) fprintf(stderr," BDX\n");
	  crs[X]--;
	  goto bidx;
	}

	if (inst == 0141206) {                /* A1A */
	  if (T_FLOW) fprintf(stderr," A1A\n");
	  crs[A]++;
	  EXPC(crs[A] == 32768);
	  SETCC_A;
	  XSETL(0);
	  continue;
	}

	if (inst == 0140304) {                /* A2A */
	  if (T_FLOW) fprintf(stderr," A2A\n");
	  crs[A] += 2;
	  EXPC(*(short *)(crs+A) <= -32767);
	  SETCC_A;
	  XSETL(0);
	  continue;
	}

	/* NOTE: confusion here about what changes on overflow */

	if (inst == 0141216) {                /* ACA */
	  if (T_FLOW) fprintf(stderr," ACA\n");
	  if (crs[KEYS] & 0100000) {
	    crs[A]++;
	    if (crs[A] == 0100000)
	      mathexception('i', FC_INT_OFLOW, 0);
	    else
	      CLEARC;
	  } else 
	    CLEARC;
	  SETCC_A;
	  XSETL(0);
	  continue;
	}

	if (inst == 0140110) {                /* S1A */
	  if (T_FLOW) fprintf(stderr," S1A\n");
	  crs[A]--;
	  EXPC(crs[A] == 32767);
	  SETCC_A;
	  XSETL(0);
	  continue;
	}

	if (inst == 0140310) {                /* S2A */
	  if (T_FLOW) fprintf(stderr," S2A\n");
	  crs[A] -= 2;
	  EXPC(*(short *)(crs+A) >= -32766);
	  SETCC_A;
	  XSETL(0);
	  continue;
	}

	if (inst == 0141050) {                /* CAL */
	  if (T_FLOW) fprintf(stderr," CAL\n");
	  crs[A] &= 0xFF;
	  continue;
	}

	if (inst == 0141044) {                /* CAR */
	  if (T_FLOW) fprintf(stderr," CAR\n");
	  crs[A] &= 0xFF00;
	  continue;
	}

	if (inst == 0140040) {                /* CRA */
	  if (T_FLOW) fprintf(stderr," CRA\n");
	  crs[A] = 0;
	  continue;
	}

	/* On the P300, the B register is the low-order word of the
	   DP floating pt fraction, so CRB was used to convert SPFP
	   numbers to DPFP.  On the P400 and up, the B register and
	   DPFP accumulator do not overlap.  For compatibility, there
	   are 3 related instructions:

	   '14 clears B and the low-order DPFP register
	   '15 clears only B
	   '16 clears only the low-order DPFP register
	*/

	if (inst == 0140014) {                /* P300 CRB */
	  if (T_FLOW) fprintf(stderr," P300CRB\n");
	  crs[B] = 0;
	  crs[FLTD] = 0;
	  continue;
	}

	if (inst == 0140015) {                /* CRB */
	  if (T_FLOW) fprintf(stderr," CRB\n");
	  crs[B] = 0;
	  continue;
	}

	if (inst == 0140016) {                /* FDBL */
	  if (T_FLOW) fprintf(stderr," FDBL\n");
	  crs[FLTD] = 0;
	  continue;
	}

	if (inst == 0140010) {                /* CRL */
	  if (T_FLOW) fprintf(stderr," CRL\n");
	  *(int *)(crs+L) = 0;
	  continue;
	}

	/* is this supposed to set C, L, and CC? */

	if (inst == 0140214) {                /* CAZ */
	  if (T_FLOW) fprintf(stderr," CAZ\n");
	  SETCC_A;
	  XSETL(0);
compskip:
	  if (crs[KEYS] & 0200)
	    RPL += 2;
	  else if (crs[KEYS] & 0100)
	    RPL++;
	  continue;
	}

	/* NOTE: using "if crs[X]++ == 0" doesn't work because of unsigned
	   short type promotion! */

	if (inst == 0140114) {                /* IRX */
	  if (T_FLOW) fprintf(stderr," IRX\n");
	  crs[X]++;
	  if (crs[X] == 0)
	    RPL++;
	  continue;
	}

	if (inst == 0140210) {                /* DRX */
	  if (T_FLOW) fprintf(stderr," DRX\n");
	  crs[X]--;
	  if (crs[X] == 0)
	    RPL++;
	  continue;
	}

	if (inst == 0141240) {                /* ICR */
	  if (T_FLOW) fprintf(stderr," ICR\n");
	  crs[A] = crs[A] << 8;
	  continue;
	}

	if (inst == 0141140) {                /* ICL */
	  if (T_FLOW) fprintf(stderr," ICL\n");
	  crs[A] = crs[A] >> 8;
	  continue;
	}

	if (inst == 0141340) {                /* ICA */
	  if (T_FLOW) fprintf(stderr," ICA\n");
	  crs[A] = (crs[A] >> 8) | (crs[A] << 8);
	  continue;
	}

	if (inst == 0140417) {                /* LT */
	  if (T_FLOW) fprintf(stderr," LT\n");
	  crs[A] = 1;
	  crs[KEYS] = (crs[KEYS] & ~0300);
	  continue;
	}

	if (inst == 0140416) {                /* LF */
	  if (T_FLOW) fprintf(stderr," LF\n");
	  crs[A] = 0;
	  crs[KEYS] = (crs[KEYS] & ~0300) | 0100;
	  continue;
	}

	if (inst == 0140314) {
	  if (T_FLOW) fprintf(stderr," TAB\n");
	  crs[B] = crs[A];
	  continue;
	}

	if (inst == 0140504) {
	  if (T_FLOW) fprintf(stderr," TAX\n");
	  crs[X] = crs[A];
	  continue;
	}

	if (inst == 0140505) {
	  if (T_FLOW) fprintf(stderr," TAY\n");
	  crs[Y] = crs[A];
	  continue;
	}

	if (inst == 0140604) {
	  if (T_FLOW) fprintf(stderr," TBA\n");
	  crs[A] = crs[B];
	  continue;
	}

	if (inst == 0141034) {
	  if (T_FLOW) fprintf(stderr," TXA\n");
	  crs[A] = crs[X];
	  continue;
	}

	if (inst == 0141124) {
	  if (T_FLOW) fprintf(stderr," TYA\n");
	  crs[A] = crs[Y];
	  continue;
	}

	if (inst == 0140104) {
	  if (T_FLOW) fprintf(stderr," XCA\n");
	  crs[B] = crs[A];
	  crs[A] = 0;
	  continue;
	}

	if (inst == 0140204) {
	  if (T_FLOW) fprintf(stderr," XCB\n");
	  crs[A] = crs[B];
	  crs[B] = 0;
	  continue;
	}

	if (inst == 0140407) {
	  if (T_FLOW) fprintf(stderr," TCA\n");
	  *(short *)(crs+A) = - (*(short *)(crs+A));
	  SETCC_A;
	  XSETL(0);
	  if (crs[A] != 0x8000) {
	    CLEARC;
	  } else {
	    mathexception('i', FC_INT_OFLOW, 0);
	  }
	  continue;
	}

	if (inst == 0141210) {
	  if (T_FLOW) fprintf(stderr," TCL\n");
	  *(int *)(crs+L) = - (*(int *)(crs+L));
	  SETCC_L;
	  SETL(*(int *)(crs+L) == 0);
	  if (*(unsigned int *)(crs+L) != 0x80000000) {
	    CLEARC;
	  } else {
	    crs[KEYS] &= ~0200;
	    mathexception('i', FC_INT_OFLOW, 0);
	  }
	  continue;
	}

	if (inst == 0140600) {
	  if (T_FLOW) fprintf(stderr," SCB\n");
	  newkeys(crs[KEYS] | 0100000);
	  continue;
	}

	if (inst == 0140200) {
	  if (T_FLOW) fprintf(stderr," RCB\n");
	  newkeys(crs[KEYS] & 077777);
	  continue;
	}

	if (inst == 0140024) {
	  if (T_FLOW) fprintf(stderr," CHS\n");
	  crs[A] ^= 0x8000;
	  continue;
	}

	if (inst == 0140500) {
	  if (T_FLOW) fprintf(stderr," SSM\n");
	  crs[A] |= 0100000;
	  continue;
	}

	if (inst == 0140100) {
	  if (T_FLOW) fprintf(stderr," SSP\n");
	  crs[A] &= 077777;
	  continue;
	}

	if (inst == 0140401) {
	  if (T_FLOW) fprintf(stderr," CMA\n");
	  crs[A] = ~crs[A];
	  continue;
	}

	if (inst == 0140320) {
	  if (T_FLOW) fprintf(stderr," CSA\n");
	  newkeys((crs[KEYS] & 077777) | (crs[A] & 0x8000));
	  crs[A] = crs[A] & 077777;
	  continue;
	}

	if (inst == 0141500) {
	  if (T_FLOW) fprintf(stderr," LCLT\n");
lclt:
	  crs[A] = ((crs[KEYS] & 0300) == 0200);
	  continue;
	}

	if (inst == 0141501) {
	  if (T_FLOW) fprintf(stderr," LCLE\n");
lcle:
	  crs[A] = ((crs[KEYS] & 0300) != 0);
	  continue;
	}

	if (inst == 0141503) {
	  if (T_FLOW) fprintf(stderr," LCEQ\n");
lceq:
	  crs[A] = ((crs[KEYS] & 0100) != 0);
	  continue;
	}

	if (inst == 0141502) {
	  if (T_FLOW) fprintf(stderr," LCNE\n");
lcne:
	  crs[A] = ((crs[KEYS] & 0100) == 0);
	  continue;
	}

	if (inst == 0141504) {
	  if (T_FLOW) fprintf(stderr," LCGE\n");
lcge:
	  crs[A] = !(crs[KEYS] & 0200) || (crs[KEYS] & 0100);
	  continue;
	}

	if (inst == 0141505) {
	  if (T_FLOW) fprintf(stderr," LCGT\n");
lcgt:
	  crs[A] = ((crs[KEYS] & 0300) == 0);
	  continue;
	}

	if (inst == 0140410) {
	  if (T_FLOW) fprintf(stderr," LLT\n");
	  SETCC_A;
	  goto lclt;
	}

	if (inst == 0140411) {
	  if (T_FLOW) fprintf(stderr," LLE\n");
	  SETCC_A;
	  goto lcle;
	}

	if (inst == 0140412) {
	  if (T_FLOW) fprintf(stderr," LNE\n");
	  SETCC_A;
	  goto lcne;
	}

	if (inst == 0140413) {
	  if (T_FLOW) fprintf(stderr," LEQ\n");
	  SETCC_A;
	  goto lceq;
	}

	if (inst == 0140414) {
	  if (T_FLOW) fprintf(stderr," LGE\n");
	  SETCC_A;
	  goto lcge;
	}

	if (inst == 0140415) {
	  if (T_FLOW) fprintf(stderr," LGT\n");
	  SETCC_A;
	  goto lcgt;
	}

	if (inst == 0141511) {
	  if (T_FLOW) fprintf(stderr," LLLE\n");
	  SETCC_L;
	  goto lcle;
	}

	if (inst == 0141513) {
	  if (T_FLOW) fprintf(stderr," LLEQ\n");
	  SETCC_L;
	  goto lceq;
	}

	if (inst == 0141512) {
	  if (T_FLOW) fprintf(stderr," LLNE\n");
	  SETCC_L;
	  goto lcne;
	}

	if (inst == 0141515) {
	  if (T_FLOW) fprintf(stderr," LLGT\n");
	  SETCC_L;
	  goto lcgt;
	}

	if (inst == 0141110) {
	  if (T_FLOW) fprintf(stderr," LFLT\n");
	  SETCC_F;
	  goto lclt;
	}

	if (inst == 0141111) {
	  if (T_FLOW) fprintf(stderr," LFLE\n");
	  SETCC_F;
	  goto lcle;
	}

	if (inst == 0141113) {
	  if (T_FLOW) fprintf(stderr," LFEQ\n");
	  SETCC_F;
	  goto lceq;
	}

	if (inst == 0141112) {
	  if (T_FLOW) fprintf(stderr," LFNE\n");
	  SETCC_F;
	  goto lcne;
	}

	if (inst == 0141114) {
	  if (T_FLOW) fprintf(stderr," LFGE\n");
	  SETCC_F;
	  goto lcge;
	}

	if (inst == 0141115) {
	  if (T_FLOW) fprintf(stderr," LFGT\n");
	  SETCC_F;
	  goto lcgt;
	}

	if (inst == 0140550) {
	  if (T_FLOW) fprintf(stderr," FLOT\n");
	  templ = crs[A];
	  templ = crs[B] | (templ<<15);
	  tempf = templ;
	  ieeepr4(&tempf);
	  crs[FLTH] = (*(unsigned int *)&tempf) >> 16;
	  crs[FLTL] = (*(unsigned int *)&tempf) & 0xFF00;
	  crs[FEXP] = (*(unsigned int *)&tempf) & 0xFF;
	  SETCC_L;
	  continue;
	}

	if (inst == 0140534) {
	  if (T_FLOW) fprintf(stderr," FRN\n");
	  continue;
	}

	if (inst == 0140574) {
	  if (T_FLOW) fprintf(stderr," DFCM\n");
	  if (T_INST) fprintf(stderr," FEXP=%d (dec), FLTH='%o, FLTL='%o\n", crs[FEXP], crs[FLTH], crs[FLTL]);
	  tempda[0] = crs[FLTH];
	  tempda[1] = crs[FLTL];
	  tempda[2] = crs[FLTD];
	  tempda[3] = crs[FEXP];
	  prieee8(tempda); 
	  *(double *)tempda = -(*(double *)tempda);
	  ieeepr8(tempda);
	  crs[FLTH] = tempda[0];
	  crs[FLTL] = tempda[1];
	  crs[FLTD] = tempda[2];
	  crs[FEXP] = tempda[3];
	  XEXPC(0);
	  continue;
	}

	if (inst == 0141000) {
	  if (T_FLOW) fprintf(stderr," ADLL\n");
	  if (crs[KEYS] & 020000) {
	    crs[KEYS] &= ~0120300;   /* clear C, L, and CC */
	    utempl = *(unsigned int *)(crs+L);
	    (*(int *)(crs+L))++;
	    if (utempl == 0x7FFFFFFF) {
	      SETC;
	    } else if (utempl == 0xFFFFFFFF)
	      crs[KEYS] |= 020100;   /* set L and EQ */
	    else if (utempl & 0x80000000)
	      crs[KEYS] |= 0200;     /* set LT */
	  } else {
	    CLEARC;
	    SETCC_L;
	  }
	  continue;
	}

	if (inst == 0140530) {
	  if (T_FLOW) fprintf(stderr," FCM\n");
	  if (T_INST) fprintf(stderr," FEXP=%d (dec), FLTH='%o, FLTL='%o\n", crs[FEXP], crs[FLTH], crs[FLTL]);
	  *(int *)&tempf = (crs[FLTH]<<16) | (crs[FLTL] & 0xFF00) | (crs[FEXP] & 0xFF);
	  prieee4(&tempf);
	  tempf = -tempf;
	  ieeepr4(&tempf);
	  crs[FLTH] = (*(unsigned int *)&tempf) >> 16;
	  crs[FLTL] = (*(unsigned int *)&tempf) & 0xFF00;
	  crs[FEXP] = (*(unsigned int *)&tempf) & 0xFF;
	  XEXPC(0);
	  continue;
	}

	if (inst == 0140510) {
	  if (T_FLOW) fprintf(stderr," FSZE\n");
	  if (*(int *)(crs+FLTH) == 0)
	    RPL++;
	  continue;
	}

	if (inst == 0140511) {
	  if (T_FLOW) fprintf(stderr," FSNZ\n");
	  if (*(int *)(crs+FLTH) != 0)
	    RPL++;
	  continue;
	}

	if (inst == 0140512) {
	  if (T_FLOW) fprintf(stderr," FSMI\n");
	  if (*(int *)(crs+FLTH) < 0)
	    RPL++;
	  continue;
	}

	if (inst == 0140513) {
	  if (T_FLOW) fprintf(stderr," FSPL\n");
	  if (*(int *)(crs+FLTH) >= 0)
	    RPL++;
	  continue;
	}

	if (inst == 0140514) {
	  if (T_FLOW) fprintf(stderr," FSLE\n");
	  if (*(int *)(crs+FLTH) < 0 || *(int *)(crs+FLTH) == 0)
	    RPL++;
	  continue;
	}

	if (inst == 0140515) {
	  if (T_FLOW) fprintf(stderr," FSGT\n");
	  if (*(int *)(crs+FLTH) > 0)
	    RPL++;
	  continue;
	}

	if (inst == 0140554) {
	  if (T_FLOW) fprintf(stderr," INT\n");
	  *(int *)&tempf = (crs[FLTH]<<16) | (crs[FLTL] & 0xFF00) | (crs[FEXP] & 0xFF);
	  prieee4(&tempf);
	  templ = tempf;
	  EXPC(tempf > 1073741823 || tempf < -1073741824);
	  crs[B] = templ & 0x7FFF;
	  crs[A] = templ >> 15;
	  continue;
	}

	if (inst == 0140531) {
	  if (T_FLOW) fprintf(stderr," INTA\n");
	  *(int *)&tempf = (crs[FLTH]<<16) | (crs[FLTL] & 0xFF00) | (crs[FEXP] & 0xFF);
	  prieee4(&tempf);
	  tempa = tempf;
	  EXPC(tempf > 32767 || tempf < -32768);
	  continue;
	}

	if (inst == 0140532) {
	  if (T_FLOW) fprintf(stderr," FLTA\n");
	  tempf = *(short *)(crs+A);
	  ieeepr4(&tempf);
	  crs[FLTH] = (*(unsigned int *)&tempf) >> 16;
	  crs[FLTL] = (*(unsigned int *)&tempf) & 0xFF00;
	  crs[FEXP] = (*(unsigned int *)&tempf) & 0xFF;
	  CLEARC;
	}

	if (inst == 0140533) {
	  if (T_FLOW) fprintf(stderr," INTL\n");
	  *(int *)&tempf = (crs[FLTH]<<16) | (crs[FLTL] & 0xFF00) | (crs[FEXP] & 0xFF);
	  prieee4(&tempf);
	  templ = tempf;
	  EXPC(tempf > 2147483647 || tempf < -2147483648);
	  continue;
	}

	if (inst == 0140535) {
	  if (T_FLOW) fprintf(stderr," FLTL\n");
	  tempf = *(int *)(crs+L);
	  ieeepr4(&tempf);
	  crs[FLTH] = (*(unsigned int *)&tempf) >> 16;
	  crs[FLTL] = (*(unsigned int *)&tempf) & 0xFF00;
	  crs[FEXP] = (*(unsigned int *)&tempf) & 0xFF;
	  CLEARC;
	}

	if (inst == 0141414) {
	  if (T_FLOW) fprintf(stderr," ILE\n");
	  templ = *(int *)(crs+L);
	  *(int *)(crs+L) = *(int *)(crs+E);
	  *(int *)(crs+E) = templ;
	  continue;
	}

	if (inst == 0141707) {
	  if (T_FLOW) fprintf(stderr," BMLT\n");
	  if (crs[KEYS] & 020000)
	    goto bclt;
	  RPL++;
	  continue;
	}

	if (inst == 0141711) {
	  if (T_FLOW) fprintf(stderr," BMLE\n");
	  if (crs[KEYS] & 020000)
	    goto bcle;
	  RPL++;
	  continue;
	}

	if (inst == 0141602) {
	  if (T_FLOW) fprintf(stderr," BMEQ\n");
	  if (crs[KEYS] & 020000)
	    goto bceq;
	  RPL++;
	  continue;
	}

	if (inst == 0141603) {
	  if (T_FLOW) fprintf(stderr," BMNE\n");
	  if (crs[KEYS] & 020000)
	    goto bcne;
	  RPL++;
	  continue;
	}

	if (inst == 0141606) {
	  if (T_FLOW) fprintf(stderr," BMGE\n");
	  if (crs[KEYS] & 020000)
	    goto bcge;
	  RPL++;
	  continue;
	}

	if (inst == 0141710) {
	  if (T_FLOW) fprintf(stderr," BMGT\n");
	  if (crs[KEYS] & 020000)
	    goto bcgt;
	  RPL++;
	  continue;
	}

	if (inst == 0141404) {
	  if (T_FLOW) fprintf(stderr," CRE\n");
	  *(int *)(crs+E) = 0;
	  continue;
	}

	if (inst == 0141410) {
	  if (T_FLOW) fprintf(stderr," CRLE\n");
	  *(int *)(crs+L) = *(int *)(crs+E) = 0;
	  continue;
	}

	if (inst == 0141414) {
	  if (T_FLOW) fprintf(stderr," ILE\n");
	  templ = *(int *)(crs+L);
	  *(int *)(crs+L) = *(int *)(crs+E);
	  *(int *)(crs+E) = templ;
	  continue;
	}

	/* queue instructions */

	if (inst == 0141714) {
	  if (T_FLOW) fprintf(stderr," RTQ\n");
	  ea = apea(NULL);
	  qtop = get16(ea);
	  qbot = get16(ea+1);
	  if (qtop == qbot) {
	    crs[A] = 0;
	    crs[KEYS] |= 0100;
	  } else {
	    qseg = get16(ea+2) & 0x7FFF;
	    qmask = get16(ea+3);
	    qea = MAKEVA(qseg,qtop);
	    crs[A] = get16(qea);
	    qtop = (qtop & ~qmask) | ((qtop+1) & qmask);
	    put16(qtop, ea);
	    crs[KEYS] &= ~0100;
	  }
	  continue;
	}

	if (inst == 0141715) {
	  if (T_FLOW) fprintf(stderr," RBQ\n");
	  ea = apea(NULL);
	  qtop = get16(ea);
	  qbot = get16(ea+1);
	  if (qtop == qbot) {  /* queue empty */
	    crs[A] = 0;
	    crs[KEYS] |= 0100;
	  } else {
	    qseg = get16(ea+2) & 0x7FFF;
	    qmask = get16(ea+3);
	    qbot = (qbot & ~qmask) | ((qbot-1) & qmask);
	    qea = MAKEVA(qseg,qbot);
	    crs[A] = get16(qea);
	    put16(qbot, ea+1);
	    crs[KEYS] &= ~0100;
	  }
	  continue;
	}

	if (inst == 0141716) {
	  if (T_FLOW) fprintf(stderr," ABQ\n");
	  ea = apea(NULL);
	  qtop = get16(ea);
	  qbot = get16(ea+1);
	  qseg = get16(ea+2) & 0x7FFF;
	  qmask = get16(ea+3);
	  qtemp = (qbot & ~qmask) | ((qbot+1) & qmask);
	  if (qtemp == qtop) {  /* queue full */
	    crs[KEYS] |= 0100;
	  } else {
	    qea = MAKEVA(qseg,qbot);
	    put16(crs[A],qea);
	    put16(qtemp, ea+1);
	    crs[KEYS] &= ~0100;
	  }
	  continue;
	}

	if (inst == 0141717) {
	  if (T_FLOW) fprintf(stderr," ATQ\n");
	  ea = apea(NULL);
	  qtop = get16(ea);
	  qbot = get16(ea+1);
	  qseg = get16(ea+2) & 0x7FFF;
	  qmask = get16(ea+3);
	  qtemp = (qtop & ~qmask) | ((qtop-1) & qmask);
	  if (qtemp == qbot) {  /* queue full */
	    crs[KEYS] |= 0100;
	  } else {
	    qea = MAKEVA(qseg,qtemp);
	    put16(crs[A],qea);
	    put16(qtemp, ea);
	    crs[KEYS] &= ~0100;
	  }
	  continue;
	}

	if (inst == 0141757) {
	  if (T_FLOW) fprintf(stderr," TSTQ\n");
	  ea = apea(NULL);
	  qtop = get16(ea);
	  qbot = get16(ea+1);
	  qmask = get16(ea+3);
	  crs[A] = (qbot-qtop) & qmask;
	  SETCC_A;
	  continue;
	}

	if (T_INST) fprintf(stderr," unrecognized generic class 3 instruction!\n");
	printf(" unrecognized generic class 3 instruction %o!\n", inst);

	/* XXX: these are hacks for CPU.FAULT; not sure how to determine whether
	   an instruction is illegal or unimplemented... */
	if (inst == 0141700)
	  fault(ILLINSTFAULT, RPL, 0);
	else
	  fault(UIIFAULT, RPL, 0);
	fatal(NULL);
	
      }


      if (class == 1) {
	if (T_INST) fprintf(stderr," shift group\n");
	scount = -inst & 077;
	if (scount == 0)
	  scount = 0100;
	switch (inst & 01700) {

	case 00000: /* LRL */
	  if (T_FLOW) fprintf(stderr," LRL %d\n", scount);
	  if (scount <= 32) {
	    utempl = *(unsigned int *)(crs+A);
	    EXPC(utempl & bitmask32[33-scount]);
	    utempl = utempl >> scount;
	    *(unsigned int *)(crs+A) = utempl;
	  } else {
	    *(unsigned int *)(crs+A) = 0;
	    CLEARC;
	  }
	  break;

	case 00100: /* LRS (different in R & V modes) */
	  if (T_FLOW) fprintf(stderr," LRS %d\n", scount);
	  if (crs[KEYS] & 010000) {          /* V/I mode */
	    if (scount <= 32) {
	      templ = *(int *)(crs+A);
	      EXPC(templ & bitmask32[33-scount]);
	      templ = templ >> scount;
	      *(int *)(crs+A) = templ;
	    } else if (crs[A] & 0x8000) {
	      *(int *)(crs+A) = 0xFFFFFFFF;
	      SETC;
	    } else {
	      *(int *)(crs+A) = 0;
	      CLEARC;
	    }
	  } else {
	    utempa = crs[B] & 0x8000;        /* save B bit 1 */
	    if (scount <= 31) {
	      templ = (crs[A]<<16) | ((crs[B] & 0x7FFF)<<1);
	      EXPC(templ & bitmask32[32-scount]);
	      templ = templ >> (scount+1);
	      crs[A] = templ >> 15;
	      crs[B] = (templ & 0x7FFF) | utempa;
	    } else if (crs[A] & 0x8000) {
	      *(int *)(crs+A) = 0xFFFF7FFF | utempa;
	      SETC;
	    } else {
	      *(int *)(crs+A) = utempa;
	      CLEARC;
	    }
	  }
	  break;

	case 00200: /* LRR */
	  if (T_FLOW) fprintf(stderr," LRR %d\n", scount);
	  if (scount > 32)
	    scount = scount - 32;
	  utempl = *(unsigned int *)(crs+A);
	  EXPC(utempl & bitmask32[33-scount]);
	  utempl = (utempl >> scount) | (utempl << (32-scount));
	  *(unsigned int *)(crs+A) = utempl;
	  break;

	case 00400: /* ARL */
	  if (T_FLOW) fprintf(stderr," ARL %d\n", scount);
	  if (scount <= 16) {
	    utempa = crs[A];
	    EXPC(utempa & bitmask16[17-scount]);
	    utempa = utempa >> scount;
	    crs[A] = utempa;
	  } else {
	    crs[A] = 0;
	    CLEARC;
	  }
	  break;

	case 00500: /* ARS */
	  if (T_FLOW) fprintf(stderr," ARS %d\n", scount);
	  if (scount <= 16) {
	    tempa = *(short *)(crs+A);
	    EXPC(tempa & bitmask16[17-scount]);
	    tempa = tempa >> scount;
	    *(short *)(crs+A) = tempa;
	  } else if (crs[A] & 0x8000) {
	    *(short *)(crs+A) = 0xFFFF;
	    SETC;
	  } else {
	    *(short *)(crs+A) = 0;
	    CLEARC;
	  }
	  break;

	case 00600: /* ARR */
	  if (T_FLOW) fprintf(stderr," ARR %d\n", scount);
	  scount = ((scount-1)%16)+1;   /* make scount 1-16 */
	  EXPC(crs[A] & bitmask16[17-scount]);
	  crs[A] = (crs[A] >> scount) | (crs[A] << (16-scount));
	  break;

	case 01000: /* LLL */
	  if (T_FLOW) fprintf(stderr," LLL %d\n", scount);
	  if (scount <= 32) {
	    utempl = *(unsigned int *)(crs+A);
	    EXPC(utempl & bitmask32[scount]);
	    utempl = utempl << scount;
	    *(unsigned int *)(crs+A) = utempl;
	  } else {
	    *(unsigned int *)(crs+A) = 0;
	    CLEARC;
	  }
	  break;

	case 01100: /* LLS (different in R/V modes) */
	  if (T_FLOW) fprintf(stderr," LLS %d\n", scount);
	  if (crs[KEYS] & 010000) {          /* V/I mode */
	    if (scount < 32) {
	      templ = 0x80000000;
	      templ = templ >> scount;         /* create mask */
	      templ = templ & *(int *)(crs+A); /* grab bits */
	      templ = templ >> (31-scount);    /* extend them */
	      EXPC(!(templ == -1 || templ == 0));
	      *(int *)(crs+A) = *(int *)(crs+A) << scount;
	    } else {
	      EXPC(*(int *)(crs+A) != 0);
	      *(int *)(crs+A) = 0;
	    }
	  } else {
	    utempa = crs[B] & 0x8000;            /* save B bit 1 */
	    if (scount < 31) {
	      utempl = (crs[A]<<16) | ((crs[B] & 0x7FFF)<<1);
	      templ2 = 0x80000000;
	      templ2 = templ2 >> scount;         /* create mask */
	      templ2 = templ2 & utempl;          /* grab bits */
	      templ2 = templ2 >> (31-scount);    /* sign extend them */
	      EXPC(!(templ2 == -1 || templ2 == 0));
	      //printf(" before: A=%x, B=%x, utempl=%x, ", crs[A], crs[B], utempl);
	      utempl = utempl << scount;
	      crs[A] = utempl >> 16;
	      crs[B] = ((utempl >> 1) & 0x7FFF) | utempa;
	      //printf(" after: A=%x, B=%x, utempl=%x\n", crs[A], crs[B], utempl);
	    } else {
	      EXPC(*(unsigned int *)(crs+A) != 0);
	      *(unsigned int *)(crs+A) = utempa;
	    }
	  }
	  break;

	case 01200: /* LLR */
	  if (T_FLOW) fprintf(stderr," LLR %d\n", scount);
	  if (scount > 32)
	    scount = scount - 32;
	  utempl = *(unsigned int *)(crs+A);
	  EXPC(utempl & bitmask32[scount]);
	  utempl = (utempl << scount) | (utempl >> (32-scount));
	  *(unsigned int *)(crs+A) = utempl;
	  break;

	case 01400: /* ALL */
	  if (T_FLOW) fprintf(stderr," ALL %d\n", scount);
	  if (scount <= 16) {
	    EXPC(crs[A] & bitmask16[scount]);
	    crs[A] = crs[A] << scount;
	  } else {
	    crs[A] = 0;
	    CLEARC;
	  }
	  break;

	case 01500: /* ALS */
	  if (T_FLOW) fprintf(stderr," ALS %d\n", scount);
	  if (scount <= 15) {
	    tempa = 0100000;
	    tempa = tempa >> scount;         /* create mask */
	    tempa = tempa & crs[A];          /* grab bits */
	    tempa = tempa >> (15-scount);    /* extend them */
	    EXPC(!(tempa == -1 || tempa == 0));
	    crs[A] = crs[A] << scount;
	  } else if (crs[A] == 0) {
	    CLEARC;
	  } else {
	    crs[A] = 0;
	    SETC;
	  }
	  break;

	case 01600: /* ALR */
	  if (T_FLOW) fprintf(stderr," ALR %d\n", scount);
	  scount = ((scount-1)%16)+1;   /* make scount 1-16 */
	  EXPC(crs[A] & bitmask16[scount]);
	  crs[A] = (crs[A] << scount) | (crs[A] >> (16-scount));
	  break;

	default:
	  printf("WARNING: unrecognized shift instruction %o at %o/%o\n", inst, RPH, RPL);
	  if (T_INST) fprintf(stderr," unrecognized shift instruction!: %o\n", inst);
	}
	continue;
      }

      if (class == 2) {
	if (T_INST) fprintf(stderr," skip group\n");

	if (inst == 0101000) {
	  if (T_FLOW) fprintf(stderr," NOP\n");
	  continue;
	}

	if (inst == 0100000) {
	  if (T_FLOW) fprintf(stderr," SKP\n");
	  RPL++;
	  continue;
	}

	if (inst == 0101400) {
	  if (T_FLOW) fprintf(stderr," SMI/SLT\n");
	  if (*(short *)(crs+A) < 0)
	    RPL++;
	  continue;
	}

	if (inst == 0100400) {
	  if (T_FLOW) fprintf(stderr," SPL/SGE\n");
	  if (*(short *)(crs+A) >= 0)
	    RPL++;
	  continue;
	}

	if (inst == 0101100) {
	  if (T_FLOW) fprintf(stderr," SLN\n");
	  if (crs[A] & 1)
	    RPL++;
	  continue;
	}

	if (inst == 0100100) {
	  if (T_FLOW) fprintf(stderr," SLZ\n");
	  if (!(crs[A] & 1))
	    RPL++;
	  continue;
	}

	if (inst == 0101040) {
	  if (T_FLOW) fprintf(stderr," SNZ/SNE\n");
	  if (crs[A] != 0)
	    RPL++;
	  continue;
	}

	if (inst == 0100040) {
	  if (T_FLOW) fprintf(stderr," SZE/SEQ\n");
	  if (crs[A] == 0)
	    RPL++;
	  continue;
	}

	if (inst == 0101220) {
	  if (T_FLOW) fprintf(stderr," SLE\n");
	  if (*(short *)(crs+A) <= 0)
	    RPL++;
	  continue;
	}

	if (inst == 0100220) {
	  if (T_FLOW) fprintf(stderr," SGT\n");
	  if (*(short *)(crs+A) > 0)
	    RPL++;
	  continue;
	}

	if (inst == 0101001) {
	  if (T_FLOW) fprintf(stderr," SSC\n");
	  if (crs[KEYS] & 0100000)
	    RPL++;
	  continue;
	}

	if (inst == 0100001) {
	  if (T_FLOW) fprintf(stderr," SRC\n");
	  if (!(crs[KEYS] & 0100000))
	    RPL++;
	  continue;
	}

	if ((inst & 0177760) == 0100260) {
	  if (T_FLOW) fprintf(stderr," SAR %d\n", (inst & 017)+1);
	  if (!(crs[A] & bitmask16[(inst & 017)+1]))
	    RPL++;
	  continue;
	}

	if ((inst & 0177760) == 0101260) {
	  if (T_FLOW) fprintf(stderr," SAS %d\n", (inst & 017)+1);
	  if (crs[A] & bitmask16[(inst & 017)+1])
	    RPL++;
	  continue;
	}

	if ((inst & 0177760) == 0100240) {
	  if (T_FLOW) fprintf(stderr," SNR %d\n", (inst & 017)+1);
	  if (!(sswitch & bitmask16[(inst & 017)+1]))
	    RPL++;
	  continue;
	}

	if ((inst & 0177760) == 0101240) {
	  if (T_FLOW) fprintf(stderr," SNS %d\n", (inst & 017)+1);
	  if (sswitch & bitmask16[(inst & 017)+1])
	    RPL++;
	  continue;
	}

	if (inst == 0100200) {    /* skip if machine check flop is reset */
	  if (T_FLOW) fprintf(stderr," SMCR\n");
	  RPL++;
	  continue;
	}

	printf(" unrecognized skip instruction %o at %o/%o\n", inst, RPH, RPL);
	fatal(NULL);

      }
    }

    /* here for non-generic instructions: memory references or pio */
    /* pio can only occur in S/R modes */

    if (!(crs[KEYS] & 010000) && (inst & 036000) == 030000) {
      pio(inst);
      continue;
    }

    /* get ix bits and adjust opcode so that PMA manual opcode
       references can be used directly, ie, if the PMA manual says the
       opcode is '15 02, then 01502 can be used here.  If the PMA
       manual says the opcode is '11, then use 01100 (the XX extended
       opcode bits are zero) */

    i = inst & 0100000;           /* indirect is bit 1 (left/MS bit) */
    x = inst & 040000;            /* indexed is bit 2 */
    opcode = (inst & 036000) >> 4;  /* isolate opcode bits */

    /* fix ldx/stx (opcode '15): these instructions cannot be indexed
       by X, so if an instruction specifies indexing by X, it acts
       like an opcode extension.  Opcodes listed as '35 02 for example
       (sty in V-mode, jdx in R-mode) have X=1 with the 4 opcode bits
       1101 ('15)

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
      if (T_INST) fprintf(stderr," ldx/stx opcode adjusted\n");
    }

    if (T_INST) fprintf(stderr," opcode=%5#0o, i=%o, x=%o\n", opcode, i, x);

    switch ((crs[KEYS] & 016000) >> 10) {
    case 0:  /* 16S */
      ea = ea16s(inst, i, x);
      break;
    case 1:  /* 32S */
      ea = ea32s(inst, i, x);
      break;
    case 2:  /* 64R */
    case 3:  /* 32R */
      ea = ea32r64r(earp, inst, i, x, &opcode);
      break;
    case 4:  /* 32I */
      ea = ea32i(earp, inst, i, x);
      fatal("32I not supported");
      break;
    case 6:  /* 64V */
      ea = ea64v(earp, inst, i, x, &opcode, &eabit);
      break;
    default:
      printf("Bad CPU mode in EA calculation, keys = %o\n", crs[KEYS]);
      fatal(NULL);
    }

    if (T_INST) fprintf(stderr," EA: %o/%o\n",ea>>16, ea & 0xFFFF);


    /* NOTE: basic and dbasic execute instructions from the register file 
       with TRACE ON */

    if (opcode == 00100) {
      if (T_FLOW) fprintf(stderr," JMP\n");
      if (prevpc+1 == RP)
	RPL = ea;
      else
	RP = ea;
      continue;
    }

    if (opcode == 00200) {
      crs[A] = get16(ea);
      if ((crs[KEYS] & 050000) == 040000) {  /* R-mode and DP */
	if (T_FLOW) fprintf(stderr," DLD\n");
	crs[B] = get16(ea+1);
      } else {
	if (T_FLOW) fprintf(stderr," LDA\n");
      }
      continue;
    }

    if (opcode == 00400) {
      put16(crs[A],ea);
      if ((crs[KEYS] & 050000) == 040000) {
	if (T_FLOW) fprintf(stderr," DST\n");
	put16(crs[B],ea+1);
      } else {
	if (T_FLOW) fprintf(stderr," STA\n");
      }
      continue;
    }

    if (opcode == 00600) {
      utempa = crs[A];
      m = get16(ea);
      if ((crs[KEYS] & 050000) == 040000) {
	if (T_FLOW) fprintf(stderr," DAD\n");
	crs[B] += get16(ea+1);
	if (crs[B] & 0x8000) {
	  crs[A]++;
	  crs[B] &= 0x7fff;
	}
	crs[A] += m;
	SETCC_L;
      } else {
	if (T_FLOW) fprintf(stderr," ADD\n");
	crs[A] += m;
	SETCC_A;
      }
      EXPC(((~utempa ^ m) & (utempa ^ crs[A])) & 0x8000);
      XSETL(0);
      continue;
    }

    if (opcode == 00700) {
      utempa = crs[A];
      m = get16(ea);
      if ((crs[KEYS] & 050000) == 040000) {
	if (T_FLOW) fprintf(stderr," DSB\n");
	crs[B] -= get16(ea+1);
	if (crs[B] & 0x8000) {
	  crs[A]--;
	  crs[B] &= 0x7fff;
	}
	crs[A] -= m;
	SETCC_L;
      } else {
	if (T_FLOW) fprintf(stderr," SUB\n");
	crs[A] -= m;
	SETCC_A;
      }
      crs[KEYS] &= ~0300;
      if (utempa == m) {
	crs[KEYS] |= 0100;
	SETL(1);
	CLEARC;
      } else {
	EXPC(((utempa ^ m) & (utempa ^ *(int *)(crs+L))) & 0x80000000);
	SETCC_L;
	if (crs[KEYS] & 0100000)
	  if (*(int *)(crs+L) > 0)
	    crs[KEYS] |= 0200;
	  else
	    crs[KEYS] &= ~0200;
	if (utempa > m) {
	  SETL(1);
	} else {
	  SETL(0);
	}
      }
      EXPC(((utempa ^ m) & (utempa ^ crs[A])) & 0x8000);
      /* NOTE: setting L to zero causes Primos boot to fail with bad rvec */
      SETL(utempa > m);
      continue;
    }

    if (opcode == 00300) {
      if (T_FLOW) fprintf(stderr," ANA\n");
      crs[A] &= get16(ea);
      continue;
    }

    if (opcode == 00500) {
      if (T_FLOW) fprintf(stderr," ERA\n");
      crs[A] ^= get16(ea);
      continue;
    }

    if (opcode == 00302) {
      if (T_FLOW) fprintf(stderr," ORA\n");
      crs[A] |= get16(ea);
      continue;
    }

    if (opcode == 01000) {
      if (T_FLOW) fprintf(stderr," JST\n");
      put16(RPL,ea);
      RPL = ea+1;
      continue;
    }

    /* this should set the CC and L bits, and is implemented in u-code
       with a subtract.  However, that operation can overflow and give
       the wrong result */

    if (opcode == 01100) {
      if (T_FLOW) fprintf(stderr," CAS\n");
      m = get16(ea);
      crs[KEYS] &= ~0300;
      if (crs[A] == m) {
	RPL++;
	crs[KEYS] |= 0100;
      } else if (*(short *)(crs+A) < *(short *)&m) {
	RPL += 2;
	crs[KEYS] |= 0200;
      }
      XSETL(0);
      continue;
    }

    if (opcode == 01200) {
      if (T_FLOW) fprintf(stderr," IRS\n");
      m = get16(ea) + 1;
      put16(m,ea);
      if (m == 0)
	RPL++;
      continue;
    }

    if (opcode == 01300) {
      if (T_FLOW) fprintf(stderr," IMA\n");
      m = get16(ea);
      put16(crs[A],ea);
      crs[A] = m;
      continue;
    }

    if (opcode == 01400) {
      /* if V-mode, JSY, else (LDX in R-mode?) */
      if (T_FLOW) fprintf(stderr," JSY\n");
      crs[Y] = RPL;
      RPL = ea;
      continue;
    }

    if (opcode == 01402) {
      if (T_FLOW) fprintf(stderr," JSXB\n");
      //printf("RP=%o/%o, ea=%o/%o\n", RPH, RPL, ea>>16, ea&0xFFFF);
      *(unsigned int *)(crs+XB) = RP;
      RP = ea;
      continue;
    }

    if (opcode == 01500) {
      if (T_FLOW) fprintf(stderr," STX\n");
      put16(crs[X],ea);
      continue;
    }

    if (opcode == 01600) {
      if (T_FLOW) fprintf(stderr," MPY\n");
      m = get16(ea);
      templ = *(short *)(crs+A) * *(short *)&m;
      if (crs[KEYS] & 010000) {          /* V/I mode */
	*(int *)(crs+L) = templ;
	CLEARC;
      } else {                      /* R/S mode */
	EXPC(templ > 1073741823 || templ < -1073741824);
	crs[A] = (templ >> 15);
	crs[B] = templ & 077777;
      }
      SETCC_L;
      continue;
    }

    if (opcode == 01603) {
      if (T_FLOW) fprintf(stderr," MPL\n");
      templ = get32(ea);
      templl = (long long)(*(int *)(crs+L)) * (long long)templ;
      *(long long *)(crs+L) = templl;
      SETCC_LE;
      CLEARC;
      continue;
    }

    if (opcode == 01700) {
      if (T_FLOW) fprintf(stderr," DIV\n");
      if (crs[KEYS] & 010000) {          /* V/I mode */
	templ = *(int *)(crs+A);
      } else {                      /* R/S mode */
	templ = *(short *)(crs+A);  /* convert to 32-bit signed */
	templ = (templ<<15) | (crs[B] & 0x7FFF);
	//printf("\nR-mode DIV: A='%o/%d, B='%o/%d, templ='%o/%d\n", crs[A], *(short *)(crs+A), crs[B], *(short *)(crs+B), templ, templ);
      }
      m = get16(ea);
      if (m != 0 && abs(*(short *)(crs+A)) < abs(*(short *)&m)) {
	crs[A] = templ / *(short *)&m;
	crs[B] = templ % *(short *)&m;
	CLEARC;
	//printf("DIV results: m='%o/%d, A='%o/%d, B='%o/%d\n", m, *(short *)&m, crs[A], *(short *)(crs+A), crs[B], *(short *)(crs+B));
      } else {
	SETC;
	//printf("DIV overflow\n");
      }
      continue;
    }

    if (opcode == 01703) {
      if (T_FLOW) fprintf(stderr," DVL\n");
      templl = *(long long *)(crs+L);
      templ = get32(ea);
      if (templ != 0) {
	*(int *)(crs+L) = templl / templ;
	*(int *)(crs+E) = templl % templ;
	CLEARC;
      } else
	SETC;
      continue;
    }

    if (opcode == 03500) {
      if (T_FLOW) fprintf(stderr," LDX\n");
      crs[X] = get16(ea);
      continue;
    }

    if (opcode == 00101) {
      if (crs[KEYS] & 010000) {          /* V/I mode */
	if (T_FLOW) fprintf(stderr," EAL\n");
	*(unsigned int *)(crs+L) = ea;
      } else {
	if (T_FLOW) fprintf(stderr," EAA\n");
	crs[A] = ea;
      }
      continue;
    }

    /* NOTE: P300 u-code listings show CC set on Jxx instructions */

    if (opcode == 00203) {
      if (crs[KEYS] & 010000) {          /* V/I mode */
	if (T_FLOW) fprintf(stderr," LDL\n");
	*(unsigned int *)(crs+L) = get32(ea);
      } else {
	if (T_FLOW) fprintf(stderr," JEQ\n");
	if (*(short *)(crs+A) == 0)
	  RPL = ea;
      }
      continue;
    }

    if (opcode == 00703) {
      if (crs[KEYS] & 010000) {          /* V/I mode */
	if (T_FLOW) fprintf(stderr," SBL\n");
	utempl = *(unsigned int *)(crs+L);
	utempl2 = get32(ea);
	*(int *)(crs+L) -= *(int *)&utempl2;
	crs[KEYS] &= ~0300;
	if (utempl == utempl2) {
	  crs[KEYS] |= 0100;
	  SETL(1);
	  CLEARC;
	} else {
	  EXPC(((utempl ^ utempl2) & (utempl ^ *(int *)(crs+L))) & 0x80000000);
	  SETCC_L;
	  if (crs[KEYS] & 0100000)
	    if (*(int *)(crs+L) > 0)
	      crs[KEYS] |= 0200;
	    else
	      crs[KEYS] &= ~0200;
	  if (utempl > utempl2) {
	    SETL(1);
	  } else {
	    SETL(0);
	  }
	}
      } else {
	if (T_FLOW) fprintf(stderr," JGE\n");
	if (*(short *)(crs+A) >= 0)
	  RPL = ea;
      }
      continue;
    }

    if (opcode == 01002) {
      if (crs[KEYS] & 010000) {          /* V/I mode */
	//traceflags = ~TB_MAP;
	if (T_FLOW || T_PCL) fprintf(stderr,"%o/%o: PCL %o/%o\n", RPH, RPL-2, ea>>16, ea&0xFFFF);
	pcl(ea);
      } else {
	if (T_FLOW) fprintf(stderr," CREP\n");
	put16(RPL,crs[S]++);
	RPL = ea;
      }
      continue;
    }

    if (opcode == 00503) {
      if (crs[KEYS] & 010000) {          /* V/I mode */
	if (T_FLOW) fprintf(stderr," ERL\n");
	templ = get32(ea);
	*(int *)(crs+A) ^= templ;
      } else {
	if (T_FLOW) fprintf(stderr," JGT\n");
	if (*(short *)(crs+A) > 0)
	  RPL = ea;
      }
      continue;
    }

    if (opcode == 00403) {
      if (crs[KEYS] & 010000) {          /* V/I mode */
	if (T_FLOW) fprintf(stderr," STL\n");
	put32(*(unsigned int *)(crs+L),ea);
      } else {
	if (T_FLOW) fprintf(stderr," JLE\n");
	if (*(short *)(crs+A) <= 0)
	  RPL = ea;
      }
      continue;
    }

    if (opcode == 00603) {
      if (crs[KEYS] & 010000) {          /* V/I mode */
	if (T_FLOW) fprintf(stderr," ADL\n");
	utempl = *(unsigned int *)(crs+L);
	templ = get32(ea);
	*(int *)(crs+L) += templ;
	EXPC(((~utempl ^ templ) & (utempl ^ *(int *)(crs+L))) & 0x80000000);
	SETCC_L;
	XSETL(0);
	/* NOTE: correct LT if overflow occurred */
	if (crs[KEYS] && 0x8000)
	  if (utempl & templ & 0x80000000)  /* signs were both neg */
	    crs[KEYS] |= 020200;   /* set LT and L */
	  else
	    crs[KEYS] &= ~0200;
      } else {
	if (T_FLOW) fprintf(stderr," JLT\n");
	if (*(short *)(crs+A) < 0)
	  RPL = ea;
      }
      continue;
    }

    if (opcode == 00303) {
      if (crs[KEYS] & 010000) {          /* V/I mode */
	if (T_FLOW) fprintf(stderr," ANL\n");
	*(unsigned int *)(crs+L) &= get32(ea);
      } else {
	if (T_FLOW) fprintf(stderr," JNE\n");
	if (*(short *)(crs+A) != 0)
	  RPL = ea;
      }
      continue;
    }

    /* NOTE: P300 u-code shows CC set for JIX/JDX */

    if (opcode == 01502) {
      if (crs[KEYS] & 010000) {          /* V/I mode */
	if (T_FLOW) fprintf(stderr," DFLX\n");
	crs[X] = get16(ea) * 4;
      } else {
	if (T_FLOW) fprintf(stderr," JDX\n");
	crs[X]--;
	if (crs[X] != 0)
	  RPL = ea;
      }
      continue;
    }

    if (opcode == 03502) {
      if (T_FLOW) fprintf(stderr," STY\n");
      //if (T_FLOW) printf("STY, RPL=%o, inst=%o, next word=%o, Y=%o, ea=%o, [ea]=%o\n", RPL-1, inst, get16(RP), crs[Y], ea, get16(ea));
      put16(crs[Y],ea);
      //if (T_FLOW) printf("executed STY, [ea]=%o\n", get16(ea));
      continue;
    }

    if (opcode == 01503) {
      if (crs[KEYS] & 010000) {          /* V/I mode */
	if (T_FLOW) fprintf(stderr," QFLX\n");
	//	printf("QFLX, RPL=%o, inst=%o, next word=%o, X=%o, ea=%o, [ea]=%o\n", RPL-1, inst, get16(RP), crs[X], ea, get16(ea));
	crs[X] = get16(ea) * 8;
	//	printf("executed QFLX, X=%o\n", crs[X]);
      } else {
	if (T_FLOW) fprintf(stderr," JIX\n");
	crs[X]++;
	if (crs[X] != 0)
	  RPL = ea;
      }
      continue;
    }

    if (opcode == 01501) {
      if (T_FLOW) fprintf(stderr," FLX\n");
      //      printf("FLX, RPL=%o, inst=%o, next word=%o, X=%o, ea=%o, [ea]=%o\n", RPL-1, inst, get16(RP), crs[X], ea, get16(ea));
      crs[X] = get16(ea) * 2;
      //      printf("executed FLX, X=%o\n", crs[X]);
      continue;
    }

    if (opcode == 03501) {
      if (T_FLOW) fprintf(stderr," LDY\n");
      //      printf("LDY, RPL=%o, inst=%o, next word=%o, Y=%o, ea=%o, [ea]=%o\n", RPL-1, inst, get16(RP), crs[Y], ea, get16(ea));
      crs[Y] = get16(ea);
      //      printf("executed LDY, Y=%o\n", crs[Y]);
      continue;
    }

    if (opcode == 03503) {
      if (T_FLOW) fprintf(stderr," JSX\n");
      crs[X] = RPL;
      RPL = ea;
      continue;
    }

    /* this should set the C and L bits, and is implemented in u-code
       with a subtract.  However, that operation can overflow and give
       the wrong result */

    if (opcode == 01103) {
      if (T_FLOW) fprintf(stderr," CLS\n");
      templ = get32(ea);
      if (*(int *)(crs+L) == templ)
	RPL++;
      else if (*(int *)(crs+L) < templ)
	RPL += 2;
      XEXPC(0);
      XSETL(0);
      continue;
    }

    if (opcode == 00601) {
      if (T_FLOW) fprintf(stderr," FAD\n");
      if (T_INST) fprintf(stderr," FEXP=%d (dec), FLTH='%o, FLTL='%o\n", crs[FEXP], crs[FLTH], crs[FLTL]);
      if (T_INST) fprintf(stderr," ea EXP=%d (dec), ea H='%o, ea L='%o\n", (mem[ea+1] & 0xFF), mem[ea], (mem[ea+1] & 0xFF00));
      *(int *)&tempf = (crs[FLTH]<<16) | (crs[FLTL] & 0xFF00) | (crs[FEXP] & 0xFF);
      prieee4(&tempf);
      *(int *)&tempf1 = get32(ea);
      prieee4(&tempf1);
      tempf += tempf1;
      ieeepr4(&tempf);
      crs[FLTH] = (*(unsigned int *)&tempf) >> 16;
      crs[FLTL] = (*(unsigned int *)&tempf) & 0xFF00;
      crs[FEXP] = (*(unsigned int *)&tempf) & 0xFF;
      XEXPC(0);
      continue;
    }

    if (opcode == 01101) {
      if (T_FLOW) fprintf(stderr," FCS\n");
      if (T_INST) fprintf(stderr," FEXP=%d (dec), FLTH='%o, FLTL='%o\n", crs[FEXP], crs[FLTH], crs[FLTL]);
      if (T_INST) fprintf(stderr," ea EXP=%d (dec), ea H='%o, ea L='%o\n", (mem[ea+1] & 0xFF), mem[ea], (mem[ea+1] & 0xFF00));
      *(int *)&tempf = (crs[FLTH]<<16) | (crs[FLTL] & 0xFF00) | (crs[FEXP] & 0xFF);
      prieee4(&tempf);
      *(int *)&tempf1 = get32(ea);
      prieee4(&tempf1);
      crs[KEYS] &= ~0300;
      if (tempf == tempf1) {
	RPL++;
	crs[KEYS] |= 0100;
      } else if (tempf < tempf1) {
	RPL += 2;
	crs[KEYS] |= 0200;
      }
      continue;
    }

    if (opcode == 01701) {
      if (T_FLOW) fprintf(stderr," FDV\n");
      if (T_INST) fprintf(stderr," FEXP=%d (dec), FLTH='%o, FLTL='%o\n", crs[FEXP], crs[FLTH], crs[FLTL]);
      if (T_INST) fprintf(stderr," ea EXP=%d (dec), ea H='%o, ea L='%o\n", (mem[ea+1] & 0xFF), mem[ea], (mem[ea+1] & 0xFF00));
      *(int *)&tempf = (crs[FLTH]<<16) | (crs[FLTL] & 0xFF00) | (crs[FEXP] & 0xFF);
      prieee4(&tempf);
      *(int *)&tempf1 = get32(ea);
      prieee4(&tempf1);
      tempf /= tempf1;
      ieeepr4(&tempf);
      crs[FLTH] = (*(unsigned int *)&tempf) >> 16;
      crs[FLTL] = (*(unsigned int *)&tempf) & 0xFF00;
      crs[FEXP] = (*(unsigned int *)&tempf) & 0xFF;
      XEXPC(0);
      continue;
    }

    if (opcode == 0201) {
      if (T_FLOW) fprintf(stderr," FLD\n");
      crs[FLTH] = get16(ea);
      m = get16(ea+1);
      crs[FLTL] = m & 0xFF00;
      crs[FEXP] = m & 0xFF;
      continue;
    }

    if (opcode == 01601) {
      if (T_FLOW) fprintf(stderr," FMP\n");
      if (T_INST) fprintf(stderr," FEXP=%d (dec), FLTH='%o, FLTL='%o\n", crs[FEXP], crs[FLTH], crs[FLTL]);
      if (T_INST) fprintf(stderr," ea EXP=%d (dec), ea H='%o, ea L='%o\n", (mem[ea+1] & 0xFF), mem[ea], (mem[ea+1] & 0xFF00));
      *(int *)&tempf = (crs[FLTH]<<16) | (crs[FLTL] & 0xFF00) | (crs[FEXP] & 0xFF);
      prieee4(&tempf);
      *(int *)&tempf1 = get32(ea);
      prieee4(&tempf1);
      tempf *= tempf1;
      ieeepr4(&tempf);
      crs[FLTH] = (*(unsigned int *)&tempf) >> 16;
      crs[FLTL] = (*(unsigned int *)&tempf) & 0xFF00;
      crs[FEXP] = (*(unsigned int *)&tempf) & 0xFF;
      XEXPC(0);
      continue;
    }

    if (opcode == 00701) {
      if (T_FLOW) fprintf(stderr," FSB\n");
      if (T_INST) fprintf(stderr," FEXP=%d (dec), FLTH='%o, FLTL='%o\n", crs[FEXP], crs[FLTH], crs[FLTL]);
      if (T_INST) fprintf(stderr," ea EXP=%d (dec), ea H='%o, ea L='%o\n", (mem[ea+1] & 0xFF), mem[ea], (mem[ea+1] & 0xFF00));
      *(int *)&tempf = (crs[FLTH]<<16) | (crs[FLTL] & 0xFF00) | (crs[FEXP] & 0xFF);
      prieee4(&tempf);
      *(int *)&tempf1 = get32(ea);
      prieee4(&tempf1);
      tempf -= tempf1;
      ieeepr4(&tempf);
      crs[FLTH] = (*(unsigned int *)&tempf) >> 16;
      crs[FLTL] = (*(unsigned int *)&tempf) & 0xFF00;
      crs[FEXP] = (*(unsigned int *)&tempf) & 0xFF;
      XEXPC(0);
      continue;
    }

    if (opcode == 0401) {
      if (T_FLOW) fprintf(stderr," FST\n");
      if (crs[FEXP] & 0xFF00)
	mathexception('f', FC_SFP_STORE, ea);
      put16(crs[FLTH],ea);
      put16((crs[FLTL] & 0xFF00) | crs[FEXP],ea+1);
      continue;
    }

    if (opcode == 0602) {
      if (T_FLOW) fprintf(stderr," DFAD\n");
      if (T_INST) fprintf(stderr," FEXP=%d (dec), FLTH='%o, FLTL='%o\n", crs[FEXP], crs[FLTH], crs[FLTL]);
      if (T_INST) fprintf(stderr," ea EXP=%d (dec), ea H='%o, ea L='%o\n", (mem[ea+1] & 0xFF), mem[ea], (mem[ea+1] & 0xFF00));
      tempda[0] = crs[FLTH];
      tempda[1] = crs[FLTL];
      tempda[2] = crs[FLTD];
      tempda[3] = crs[FEXP];
      prieee8(tempda);
      tempd = get64(ea);
      prieee8(&tempd);
      *(double *)tempda += tempd;
      ieeepr8(tempda);
      crs[FLTH] = tempda[0];
      crs[FLTL] = tempda[1];
      crs[FLTD] = tempda[2];
      crs[FEXP] = tempda[3];
      XEXPC(0);
      continue;
    }

    if (opcode == 01102) {
      if (T_FLOW) fprintf(stderr, " DFCS\n");
      m = get16(ea);
      if ((crs[FLTH] & 0x8000) == (m & 0x8000)) {
	m1 = get16(ea+3);
	if (m1 == crs[FEXP]) {
	  if (m == crs[FLTH]) {
	    utempl = get32(ea+1);
	    if ((unsigned int)((crs[FLTL]<<16) | crs[FLTD]) == utempl) {
	      RPL += 1;
	      crs[KEYS] |= 0100;
	    } else if ((unsigned int)((crs[FLTL]<<16) | crs[FLTD]) < utempl) {
	      RPL += 2;
	      crs[KEYS] |= 0200;
	    }
	  } else if (crs[FLTH] < m) {
	    RPL += 2;
	    crs[KEYS] |= 0200;
	  }
	} else if (crs[FEXP] < m1) {       /* this line breaks CPU.FLOAT.V */
	  RPL += 2;
	  crs[KEYS] |= 0200;
	}
      } else if (crs[FLTH] & 0x8000) {   /* DAC < mem */
	RPL += 2;
	crs[KEYS] |= 0200;
      }

#if 0
      tempda[0] = crs[FLTH];
      tempda[1] = crs[FLTL];
      tempda[2] = crs[FLTD];
      tempda[3] = crs[FEXP];
      prieee8(tempda);
      printf(" FLTH=%x FLTL=%x FLTD=%x FEXP=%x, value=%e\n", crs[FLTH], crs[FLTL], crs[FLTD], crs[FEXP], *(double *)tempda);
      tempd = get64(ea);
      prieee8(&tempd);
      printf(" ea H=%x ea L=%x ea D=%x ea X=%x, value=%e\n", get16(ea), get16(ea+1), get16(ea+2), get16(ea+3), tempd);
      crs[KEYS] &= ~0300;
      if (*(double *)tempda == tempd) {
	RPL++;
	crs[KEYS] |= 0100;
      } else if (*(double *)tempda < tempd) {
	RPL += 2;
	crs[KEYS] |= 0200;
      }
#endif
      continue;
    }

    if (opcode == 01702) {
      if (T_FLOW) fprintf(stderr," DFDV\n");
      if (T_INST) fprintf(stderr," FEXP=%d (dec), FLTH='%o, FLTL='%o\n", crs[FEXP], crs[FLTH], crs[FLTL]);
      if (T_INST) fprintf(stderr," ea EXP=%d (dec), ea H='%o, ea L='%o\n", (mem[ea+1] & 0xFF), mem[ea], (mem[ea+1] & 0xFF00));
      tempda[0] = crs[FLTH];
      tempda[1] = crs[FLTL];
      tempda[2] = crs[FLTD];
      tempda[3] = crs[FEXP];
      prieee8(tempda);
      tempd = get64(ea);
      prieee8(&tempd);
      *(double *)tempda /= tempd;
      ieeepr8(tempda);
      crs[FLTH] = tempda[0];
      crs[FLTL] = tempda[1];
      crs[FLTD] = tempda[2];
      crs[FEXP] = tempda[3];
      XEXPC(0);
      continue;
    }

    if (opcode == 0202) {
      if (T_FLOW) fprintf(stderr," DFLD\n");
      *(double *)tempda = get64(ea);
      crs[FLTH] = tempda[0];
      crs[FLTL] = tempda[1];
      crs[FLTD] = tempda[2];
      crs[FEXP] = tempda[3];
      continue;
    }

    if (opcode == 01602) {
      if (T_FLOW) fprintf(stderr," DFMP\n");
      if (T_INST) fprintf(stderr," FEXP=%d (dec), FLTH='%o, FLTL='%o\n", crs[FEXP], crs[FLTH], crs[FLTL]);
      if (T_INST) fprintf(stderr," ea EXP=%d (dec), ea H='%o, ea L='%o\n", (mem[ea+1] & 0xFF), mem[ea], (mem[ea+1] & 0xFF00));
      tempda[0] = crs[FLTH];
      tempda[1] = crs[FLTL];
      tempda[2] = crs[FLTD];
      tempda[3] = crs[FEXP];
      prieee8(tempda);
      tempd = get64(ea);
      prieee8(&tempd);
      *(double *)tempda *= tempd;
      ieeepr8(tempda);
      crs[FLTH] = tempda[0];
      crs[FLTL] = tempda[1];
      crs[FLTD] = tempda[2];
      crs[FEXP] = tempda[3];
      XEXPC(0);
      continue;
    }

    if (opcode == 0702) {
      if (T_FLOW) fprintf(stderr," DFSB\n");
      if (T_INST) fprintf(stderr," FEXP=%d (dec), FLTH='%o, FLTL='%o\n", crs[FEXP], crs[FLTH], crs[FLTL]);
      if (T_INST) fprintf(stderr," ea EXP=%d (dec), ea H='%o, ea L='%o\n", (mem[ea+1] & 0xFF), mem[ea], (mem[ea+1] & 0xFF00));
      tempda[0] = crs[FLTH];
      tempda[1] = crs[FLTL];
      tempda[2] = crs[FLTD];
      tempda[3] = crs[FEXP];
      prieee8(tempda);
      tempd = get64(ea);
      prieee8(&tempd);
      *(double *)tempda -= tempd;
      ieeepr8(tempda);
      crs[FLTH] = tempda[0];
      crs[FLTL] = tempda[1];
      crs[FLTD] = tempda[2];
      crs[FEXP] = tempda[3];
      XEXPC(0);
      continue;
    }

    if (opcode == 0402) {
      if (T_FLOW) fprintf(stderr," DFST\n");
      tempda[0] = crs[FLTH];
      tempda[1] = crs[FLTL];
      tempda[2] = crs[FLTD];
      tempda[3] = crs[FEXP];
      put64(*(double *)tempda, ea);
#if 0
      put16(crs[FLTH], ea);
      put16(crs[FLTL], ea+1);
      put16(crs[FLTD], ea+2);
      put16(crs[FEXP], ea+3);
#endif
      continue;
    }

    if (opcode == 01202) {
      if (T_FLOW) fprintf(stderr," EAXB\n");
      *(ea_t *)(crs+XB) = ea;
      continue;
    }

    if (opcode == 01302) {
      if (T_FLOW) fprintf(stderr," EALB\n");
      *(ea_t *)(crs+LB) = ea;
      continue;
    }

    if (opcode == 0101) {
      if (T_FLOW) fprintf(stderr," EAL\n");
      *(ea_t *)(crs+L) = ea;
      continue;
    }

    if (opcode == 0301) {
      if (T_FLOW) fprintf(stderr," STLR\n");
      utempa = ea;                 /* word number portion only */
      if (utempa & 040000) {       /* absolute RF addressing */
	if (restrict()) break;
	regs.s32[utempa & 0377] = *(int *)(crs+L);
      } else {
	utempa &= 037;
	if (utempa > 017 && restrict()) break;
	*(((int *)crs)+utempa) = *(int *)(crs+L);
      }
      continue;
    }

    if (opcode == 0501) {
      if (T_FLOW) fprintf(stderr," LDLR\n");
      utempa = ea;                 /* word number portion only */
      if (utempa & 040000) {       /* absolute RF addressing */
	if (restrict()) break;
	*(int *)(crs+L) = regs.s32[utempa & 0377];
      } else {
	utempa &= 037;
	if (utempa > 017 && restrict()) break;
	*(int *)(crs+L) = *(((int *)crs)+utempa);
      }
      continue;
    }

    if (opcode == 01401) {
      if (T_FLOW) fprintf(stderr," EIO\n");
      if (restrict()) break;
      crs[KEYS] &= ~0100;      /* reset EQ */
      pio(ea & 0xFFFF);
      continue;
    }

    if (opcode == 00102) {
      if (T_FLOW) fprintf(stderr," XEC\n");
      utempl = RP-2;
      utempa = get16(ea);
      //printf("RPL %o/%o: XEC instruction %o|%o, ea is %o/%o, new inst = %o \n", utempl>>16, utempl&0xFFFF, inst, get16(utempl+1), ea>>16, ea&0xFFFF, utempa);
      inst = utempa;
      earp = ea + 1;       /* sb 16-bit increment */
      goto xec;
    }

    if (opcode == 00103) {
      if (T_FLOW) fprintf(stderr," ENTR\n");
      utempa = crs[S];
      crs[S] -= ea;
      put16(utempa,crs[S]);
      continue;
    }

#if 0
    if (mem[066] != 0) {
      if (T_FLOW) fprintf(stderr," JST* '66 [%o]\n", mem[066]);
      mem[mem[066]] = RPL;
      RPL = mem[066]+1;
      continue;
    }
#endif
    printf("Unknown memory reference opcode: %o\n", opcode);
    fatal(NULL);
  }
}
    
  
/* Handle SVC instruction.  For real hardware emulation on an R-mode
   such as the P300, SVC would interrupt (JST*) through location '75
   (in vectored mode) or would fault on the P400.  

   Since we're running programs without an OS underneath us, handle
   the SVC's here.

   Typical usage:

     CALL TNOUA ('MESSAGE', 7)
         JST TNOUA
         DAC =C'MESSAGE'
         DAC =7
         OCT 0
         ... 

     The library would resolve the TNOUA reference like this:
 
     TNOUA DAC  **        RETURN ADDRESS
           SVC            ENTER THE OS
           OCT  140703
           JMP# BADCALL   HERE FOR BAD SVC
           ...

   The SVC code word follows the SVC instruction:

     '100000 = this is an interlude; the actual arguments are found
     at the address preceeding the SVC instruction (ie, the caller)

     '040000 = there is an instruction following the SVC code, before
     the argument list.  If there is an SVC error, like a bad function
     code, the SVC handler will return to the location following the
     code word.

     Bits 3-4 are ignored

     Bits 5-10 are the SVC group, 1-15.
     Bits 11-16 are the SVC function within the group.

  Interludes are used because at the time a program is compiled, the
  compiler doesn't know whether a call is going to the OS or not.  So the
  call is compiled like any other.  It's dumb for the library TNOUA to
  muck around with all the arguments, so bit 1 is set as a shortcut and
  the address of the argument list is just before the SVC itself.

  If a program knows it's going to call the OS, a direct SVC can be used,
  with bit 1 clear:

	 SVC
	 OCT  '040703
         JMP# BADSVC
         DAC  =C'MESSAGE'
         DAC  =7
         OCT  0
  GOOD   ...

  BADSVC EQU  *

*/


svc() {

#define MAXCLASS 015
#define MAXFUNC 027
#define MAXSVCARGS 10

  unsigned short code;             /* code word following SVC instruction */
  unsigned short argl;             /* address of argument list */
  void *arg[MAXSVCARGS];           /* arg address array */
  short actargs;                   /* number of actual arguments */
  short class;                     /* SVC class, from bits 5-10 */
  short func;                      /* SVC func within class, bits 11-16 */
  short temp16;

  static struct {
    char name[8];                  /* svc routine name */
    char numargs;                  /* number of arguments for this svc */
    short locargs;                 /* bit mask for LOC(X) arguments */
  } svcinfo[MAXCLASS+1][MAXFUNC+1] = {

    /* class 0 is a dummy: it's mapped to class 1 below */

    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,

    /* class 1 */

    "ATTACH", -5, 0, /* func 0 */
    "SEARCH", -4, 0, /* func 1 */
    "SAVE  ",  2, 0, /* func 2 */
    "RESTOR", -3, 0, /* func 3 */
    "RESUME",  1, 0, /* func 4 */
    "EXIT  ",  0, 0, /* func 5 */
    "ERRRTN",  4, 0, /* func 6 */
    "UPDATE", 99, 0, /* func 7 */
    "GETERR",  2, 0, /* func 010 */
    "PRERR ",  0, 0, /* func 011 */
    "GINFO ",  2, 0, /* func 012 */
    "CNAME$", -3, 0, /* func 013 */
    "ERRSET",  5, 0, /* func 014 */
    "FORCEW",  2, 0, /* func 015 */
    "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,

    /* class 2 */

    "READ  ", 99, 0, /* func 0 */
    "WRITE ", 99, 0, /* func 1 */
    "RDLIN ", -4, 0, /* func 2 */
    "WTLIN ", -4, 0, /* func 3 */
    "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,

    /* class 3 */

    "PRWFIL", -6, 020000, /* func 0 */
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,

    /* class 4 */
    
    "FAMSVC", 99, 0, /* func 0 */
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,

    /* class 5 */

    "RREC  ", -6, 0, /* func 0 */
    "WREC  ", -6, 0, /* func 1 */
    "TIMDAT",  2, 0, /* func 2 */
    "DIGIN ", -4, 0, /* func 3 */
    "DIGINW", -4, 0, /* func 4 */
    "RCYCL ",  0, 0, /* func 5 */
    "D$INIT",  1, 0, /* func 6 */
    "BREAK$",  1, 0, /* func 7 */
    "T$MT  ",  6, 040000, /* func 010 */
    "T$LMPC",  5, 040000, /* func 011 */
    "T$CMPC",  5, 040000, /* func 012 */
    "T$AMLC",  5, 040000, /* func 013 */
    "T$VG  ",  5, 040000, /* func 014 */
    "T$PMPC",  5, 040000, /* func 015 */
    "RRECL ", -6, 0, /* func 016 */
    "WRECL ", -6, 0, /* func 017 */
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,

    /* class 6 */

    "COMANL",  0, 0, /* func 0 */
    "C1IN  ",  1, 0, /* func 1 */
    "CMREAD",  1, 0, /* func 2 */
    "COMINP", -3, 0, /* func 3 */
    "CNIN$ ",  3, 0, /* func 4 */
    "PHANT$",  5, 0, /* func 5 */
    "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,

    /* class 7 */

    "T1IN  ",  1, 0, /* func 0 */
    "T1OU  ",  1, 0, /* func 1 */
    "TNOU  ",  2, 0, /* func 2 */
    "TNOUA ",  2, 0, /* func 3 */
    "TOOCT ",  1, 0, /* func 4 */
    "DUPLX$",  1, 0, /* func 5 */
    "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,

    /* class 010 */

    "T$MT  ",  6, 040000, /* func 0 */
    "T$SMLC",  4, 020000, /* func 1 */
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,

    /* class 011 */

    "T$LMPC",  6, 040000, /* func 0 */
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,

    /* class 012 */

    "T$CMPC",  6, 040000, /* func 0 */
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,

    /* class 013 (user defined) */

    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,

    /* class 014 */

    "ATTAC$",  6, 002000, /* func 0 */
    "CREAT$",  5, 004000, /* func 1 */
    "ERRPR$",  6, 0, /* func 2 */
    "ERRST$", 99, 0, /* func 3 */
    "GPASS$",  5, 004000, /* func 4 */
    "GQUOT$", 99, 0, /* func 5 */
    "PRWFL$",  7, 021000, /* func 6 */
    "RDENT$",  8, 000400, /* func 7 */
    "SATTR$",  5, 004000, /* func 010 */
    "SEARC$",  6, 002000, /* func 011 */
    "SEGDR$",  5, 004000, /* func 012 */
    "SPASS$",  3, 020000, /* func 013 */
    "SQUOT$", 99, 0, /* func 014 */
    "CNGNM$",  5, 004000, /* func 015 */
    "COMIN$",  4, 010000, /* func 016 */
    "RDTKN$",  5, 004000, /* func 017 */
    "RESTO$",  4, 010000, /* func 020 */
    "RESUM$",  2, 0,  /* func 021 */
    "SAVE$ ",  4, 010000,  /* func 022 */
    "",99,0, "",99,0, "",99,0, "",99,0, "",99,0,

    /* class 015 */

    "ATCH$$",  6, 0,  /* func 0 */
    "CREA$$",  5, 0,  /* func 1 */
    "",99,0,           /* func 2 */
    "",99,0,           /* func 3 */
    "GPAS$$",  5, 0,  /* func 4 */
    "",99,0,           /* func 5 */
    "PRWF$$",  7, 020000,  /* func 6 */
    "RDEN$$",  8, 0,  /* func 7 */
    "SATR$$",  5, 0,  /* func 010 */
    "SRCH$$",  6, 0,  /* func 011 */
    "SGDR$$",  5, 0,  /* func 012 */
    "SPAS$$",  3, 0,  /* func 013 */
    "",99,0,  /* func 014 */
    "CNAM$$",  5, 0,  /* func 015 */
    "COMI$$",  4, 0,  /* func 016 */
    "RDTK$$",  5, 0,  /* func 017 */
    "REST$$",  4, 0,  /* func 020 */
    "RESU$$",  2, 0,  /* func 021 */
    "SAVE$$",  4, 0,  /* func 022 */
    "COMO$$",  5, 0,  /* func 023 */
    "ERKL$$",  4, 0,  /* func 024 */
    "RDLIN$",  4, 0,  /* func 025 */
    "WTLIN$",  4, 0,  /* func 026 */
    "",99,0
  };

#if 0
  for (class=0; class<=MAXCLASS; class++)
    for (func=0; func<=MAXFUNC; func++)
      printf("Class %o, func %o: %s %d args %o\n", class,func, svcinfo[class][func].name, svcinfo[class][func].numargs, svcinfo[class][func].locargs);
#endif

  /* get svc code word, break into class and function */

  code = mem[RPL];
  class = (code >> 6) & 077;
  if (class == 0)
    class = 1;
  func = code & 077;

  /* determine argument list location and create arg list vector */

  if (code & 0100000)
    argl = mem[RPL-2];
  else if (code & 040000)
    argl = RPL+2;
  else
    argl = RPL+1;

  if (T_INST) fprintf(stderr," code=%o, class=%o, func=%o, argl=%o\n", code, class, func, argl);
  if (class > MAXCLASS || func > MAXFUNC)
    goto badsvc;

  if (T_FLOW) fprintf(stderr," name=%s, #args=%d, LOC args=%o\n", svcinfo[class][func].name, svcinfo[class][func].numargs, svcinfo[class][func].locargs);

  /* if location '65 is set, do indirect JST to handle svc */

  if (mem[065] != 0) {
    if (T_INST) fprintf(stderr," JST* '65 [%o]\n", mem[065]);
    mem[mem[065]] = RPL;
    RPL = mem[065]+1;
    return;
  }

  if (svcinfo[class][func].numargs == 99)
    goto badsvc;

  if (svcinfo[class][func].locargs != 0)
    fatal("Can't handle LOC() args");

  actargs = 0;
  if (svcinfo[class][func].numargs == 1) {
    actargs = 1;
    arg[0] = mem+mem[argl++];
    if (mem[argl] == 0)     /* single arg: terminating zero is optional */
      argl++;
  } else if (svcinfo[class][func].numargs > 0) {
    while (mem[argl] != 0) {
      if (actargs < MAXSVCARGS)
	arg[actargs++] = mem+mem[argl];
      argl++;
    }
    argl++;                 /* skip terminating zero */
    while (actargs < svcinfo[class][func].numargs)
      arg[actargs++] == NULL;
  }

  if (T_INST) fprintf(stderr," return=%o, actargs=%d\n", argl, actargs);

  switch (class) {
  case 0: /* same as class 1 */
  case 1: /* funcs 0-'15 */
    switch (func) {
    case 0:  /* ATTACH (NAME,DEV,PASSWD,HOMESET,ALTRTN) */
      goto unimp;
    case 1:  /* SEARCH (KEY,NAME,UNIT,ALTRTN) */
      goto unimp;
    case 2:  /* SAVE (RVEC,NAME) */
      goto unimp;
    case 3:  /* RESTOR (RVEC,NAME,ALTRTN) */
      goto unimp;
    case 4:  /* RESUME (NAME) */
      goto unimp;
    case 5:  /* EXIT () */
      os_exit();
    case 6:  /* ERRRTN (ALTRTN,'XXXXXX',MSG,MSGLEN) */
      goto unimp;
    case 7:  /* UPDATE */
      goto unimp;
    case 010:  /* GETERR (BUFF,N) */
      goto unimp;
    case 011:  /* PRERR () */  
      goto unimp;
    case 012:  /* GINFO (BUFF,N) */
      os_ginfo(arg[0], arg[1]);
      break;
    case 013:  /* CNAME$ (OLDNAM,NEWNAM,ALTRTN) */
      goto unimp;
    case 014:  /* ERRSET (ALTVAL,ALTRTN,'XXXXXX','MSG',MSGLEN) */
      goto unimp;
    case 015:  /* FORCEW (KEY,UNIT) */
      goto unimp;
    default:
      goto unimp;
    }
    break;

  case 2: /* funcs 0-3 */
    goto unimp;
    break;

  case 3: /* func 0 = PRWFIL */
    switch (func) {
    case 0:  /* PRWFIL (KEY,UNIT,LOC(BUF),N,POS,ALTRTN) */
      goto unimp;
    default:
      goto unimp;
    }
    break;

  case 4: /* func 0 = FAMSVC (obsolete) */
    goto unimp;
    break;

  case 5: /* funcs 0-'17 */
    switch (func) {
    case 0:  /* RREC (PBAV,NWV,N,RA16,PDEV,ALTRTN) */
      goto unimp;
    case 1:  /* WREC (PBAV,NWV,N,RA16,PDEV,ALTRTN) */
      goto unimp;
    case 2:  /* TIMDAT (BUFF,N) */
      os_timdat(arg[0], arg[1]);
      break;
    case 3:  /* DIGIN (N,ADRVEC,DATVEC,ALTRTN) */
      goto unimp;
    case 4:  /* DIGINW (same, but initial arg of 1 instead of 0 to DIGIN) */
      goto unimp;
    case 5:  /* RECYCL */
      goto unimp;
    case 6:  /* D$INIT (PDEV) */
      goto unimp;
    case 7:  /* BREAK$ (ONOFF) */
      os_break$(arg[0]);
      break;
    case 010:  /* T$MT (UNIT,LOC(BUF),NW,INST,STATV,CODE) */
      goto unimp;
    case 011:  /* T$LMPC (UNIT,LOC(BUF),NW,INST,STATV) */
      goto unimp;
    case 012:  /* T$CMPC (UNIT,LOC(BUF),NW,INST,STATV) */
      goto unimp;
    case 013:  /* T$AMLC (LINE,LOC(BUF),NW,INST,STATV) */
      goto unimp;
    case 014:  /* T$VG (UNIT,LOC(BUF),NW,INST,STATV) */
      goto unimp;
    case 015:  /* T$PMPC (UNIT,LOC(BUF),NW,INST,STATV) */
      goto unimp;
    case 016:  /* RRECL (PBAV,NWV,N,RA32,PDEV,ALTRTN) */
      goto unimp;
    case 017:  /* WRECL (PBAV,NWV,N,RA32,PDEV,ALTRTN) */
      goto unimp;
    default:
      goto unimp;
    }
    break;

  case 6: /* funcs 0-5 */
    switch (func) {
    case 0:  /* COMANL () */
      os_comanl();
      break;
    case 1:  /* C1IN (CHAR) */
      os_c1in(arg[0]);
      break;
    case 2:  /* CMREAD (BUFF) */
      strncpy(arg[0], "      ", 6);
      break;
    case 3:  /* COMINP (FILNAM,UNIT,ALTRTN) */
      goto unimp;
    case 4:  /* CNIN$ (BUF,MAXCHARS,ACTCHARS) */
      os_cnin$(arg[0], arg[1], arg[2]);
      break;
    case 5:  /* PHANT$ (FILNAM,NAMLEN,UNIT,USER,CODE) */
      goto unimp;
    default:
      goto unimp;
    }
    break;

  case 7:
    switch (func) {
    case 0:  /* T1IN (CHAR) */
      os_c1in(arg[0]);
      break;
    case 1:  /* T1OU (CHAR) */
      os_t1ou(arg[0]);
      break;
    case 2: /* TNOU */                       
    case 3: /* TNOUA */
      os_tnoua(arg[0],arg[1]);
      if (func == 2) {
	temp16 = 1;
	os_tnoua("\n", &temp16);
      }
      break;
    case 4:  /* TOOCT (NUMBER) */
      goto unimp;
    case 5:  /* DUPLX$ (KEY) */
      goto unimp;
    default:
      goto unimp;
    }
    break;

  case 010:
    switch (func) {
    case 0:  /* T$MT (UNIT,LOC(BUF),NW,INST,STATV,CODE) */
      goto unimp;
    case 1:  /* T$SLC1 (KEY,LINE,LOC(BUF),NWORDS) */
      goto unimp;
    default:
      goto unimp;
    }
    break;

  case 011:
    switch (func) {
    case 0:  /* T$LMPC (UNIT,LOC(BUF),NW,INST,STATV,CODE) */
      goto unimp;
    default:
      goto unimp;
    }
    break;

  case 012:
    switch (func) {
    case 0:  /* T$CMPC (UNIT,LOC(BUF),NW,INST,STATV,CODE) */
      goto unimp;
    default:
      goto unimp;
    }
    break;

  case 013:
    switch (func) {  /* func is 0-2 */
    default:
      goto unimp;
    }
    break;

  case 014:
    switch (func) {
    case 0:  /* ATTAC$ (NAME,NAMLEN,LDEV,PASSWD,KEY,LOC(CODE)) */
      goto unimp;
    case 1:  /* CREAT$ (NAME,NAMLEN,OWNER,NONOWNER,LOC(CODE)) */
      goto unimp;
    case 2:  /* ERRPR$ (KEY,CODE,TEXT,TEXTLEN,PROGNAME,NAMLEN) */
      os_errpr$ (arg[0], arg[1], arg[2], arg[3], arg[4], arg[5]);
      break;
    case 4:  /* GPASS$ (NAME,NAMLEN,OWNER,NONOWNER,LOC(CODE) */
      goto unimp;
    case 6:  /* PRWFL$ (KEY,UNIT,LOC(BUF),NW,POS,RNW,LOC(CODE)) */
      goto unimp;
    case 7:  /* RDENT$ (KEY,UNIT,BUF,BUFSIZ,RNW,NAME,NAMLEN,LOC(CODE)) */
      goto unimp;
    case 010:  /* SATTR$ (KEY,NAME,NAMLEN,ARRAY,LOC(CODE)) */
      goto unimp;
    case 011:  /* SEARC$ (KEY,NAME,NAMLEN,UNIT,TYPE,LOC(CODE) */
      goto unimp;
    case 012:  /* SEGDR$ (KEY,UNIT,ENTRYA,ENTRYB,LOC(CODE)) */
      goto unimp;
    case 013:  /* SPASS$ (OWNER,NONOWNER,LOC(CODE)) */
      goto unimp;
    case 015:  /* CNGNM$ (OLDNAM,OLDLEN,NEWNAM,NEWLEN,LOC(CODE)) */
      goto unimp;
    case 016:  /* COMIN$ (FILNAM,NAMLEN,UNIT,LOC(CODE)) */
      goto unimp;
    case 017:  /* RDTKN$ (KEY,INFO(5),BUF,BUFLEN,LOC(CODE)) */
      goto unimp;
    case 020:  /* RESTO$ (RVEC(9),FILNAM,NAMLEN,LOC(CODE)) */
      goto unimp;
    case 021:  /* RESUM$ (NAME,NAMLEN) */
      goto unimp;
    case 022:  /* SAVE$ (RVEC,FILNAM,NAMLEN,LOC(CODE)) */
      goto unimp;
    default:
      goto unimp;
    }
    break;

  case 015:
    switch (func) {
    case 0:  /* ATCH$$ (UFDNAM,NAMLEN,LDEV,PASSWD,KEY,CODE) */
      goto unimp;
    case 1:  /* CREA$$ (NAME,NAMLEN,OPASS,NPASS,CODE) */
      goto unimp;
    case 4:  /* GPAS$$ (UFDNAM,NAMLEN,OPASS,NPASS,CODE) */
      goto unimp;
    case 6:  /* PRWF$$ (KEY,FUNIT,LOC(BUF),BUFLEN,POS32,RNW,CODE) */
      goto unimp;
    case 7:  /* RDEN$$ (KEY,FUNIT,BUF,BUFLEN,RNW,NAME32,NAMLEN,CODE) */
      goto unimp;
    case 010:  /* SATR$$ (KEY,NAME,NAMLEN,ARRAY,CODE) */
      goto unimp;
    case 011:  /* SRCH$$ (KEY,NAME,NAMLEN,FUNIT,TYPE,CODE) */
      goto unimp;
    case 012:  /* SGDR$$ (KEY,FUNIT,ENTRYA,ENTRYB,CODE) */
      goto unimp;
    case 013:  /* SPAS$$ (OWNER,NON-OWNER,CODE) */
      goto unimp;
    case 015:  /* CNAM$$ (OLDNAM,OLDLEN,NEWNAM,NEWLEN,CODE) */
      goto unimp;
    case 016:  /* COMI$$ (FILNAM,NAMLEN,UNIT,CODE) */
      goto unimp;
    case 017:  /* RDTK$$ (KEY,INFO(5),BUF,BUFLEN,CODE) */
      os_rdtk$$ (arg[0], arg[1], arg[2], arg[3], arg[4]);
      break;
    case 020:  /* REST$$ (RVEC,NAME,NAMLEN,CODE) */
      goto unimp;
    case 021:  /* RESU$$ (NAME,NAMLEN) */
      goto unimp;
    case 022:  /* SAVE$$ (RVEC,NAME,NAMLEN,CODE) */
      goto unimp;
    case 023:  /* COMO$$ (KEY,NAME,NAMLEN,XXXXXX,CODE) */
      goto unimp;
    case 024:  /* ERKL$$ (KEY,ERASECH,KILLCH,CODE) */
      os_erkl$$(arg[0], arg[1], arg[2], arg[3]);
      break;
    case 025:  /* RDLIN$ (UNIT,LINE,NWDS,CODE) */
      goto unimp;
    case 026:  /* WTLIN$ (UNIT,LINE,NWDS,CODE) */
      goto unimp;
    default:
      goto unimp;
    }
    break;

  default:
    goto unimp;                   /* bad class */
  }

  /* after the SVC, argl is the return address */

  if (T_INST) fprintf(stderr," returning from SVC to %o\n", argl);
  RPL = argl;
  return;


unimp:

  printf(" svc not implemented, class=%o, func=%o\n", class, func);
  fatal(NULL);

  /* here on a bad svc; if the bounce bit (bit 2) is set in the code word,
     jump to the location following the code word (which is typically a
     JMP instruction).  If the bounce bit isn't set, we have to halt */

badsvc:

  if (code & 040000) {
    RPL++;
    if (T_INST) fprintf(stderr," bouncing svc error to address %o\n", RPL);
    return;
  }
  
  printf(" halting on bad svc, class=%o, func=%o\n", class, func);
  fatal(NULL);
}



/* here for PIO instructions: OCP, SKS, INA, OTA.  The instruction
   word is passed in as an argument to handle EIO (Execute I/O) in
   V-mode.
*/

pio(unsigned int inst) {
  short class;
  short func;
  short device;

  class = inst >> 14;
  func = (inst >> 6) & 017;
  device = inst & 077;
  if (T_INST) fprintf(stderr," pio, class=%d, func='%o, device='%o\n", class, func, device);
  if (devmap[device])
    devmap[device](class, func, device);
  else {
    printf("pio: no handler for device '%o\n", device);
    fatal(NULL);
  }
}
