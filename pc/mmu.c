#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

/*
 *  task state segment.  Plan 9 ignores all the task switching goo and just
 *  uses the tss for esp0 and ss0 on gate's into the kernel, interrupts,
 *  and exceptions.  The rest is completely ignored.
 *
 *  This means that we only need one tss in the whole system.
 */
typedef struct Tss	Tss;
struct Tss
{
	ulong	backlink;	/* unused */
	ulong	sp0;		/* pl0 stack pointer */
	ulong	ss0;		/* pl0 stack selector */
	ulong	sp1;		/* pl1 stack pointer */
	ulong	ss1;		/* pl1 stack selector */
	ulong	sp2;		/* pl2 stack pointer */
	ulong	ss2;		/* pl2 stack selector */
	ulong	cr3;		/* page table descriptor */
	ulong	eip;		/* instruction pointer */
	ulong	eflags;		/* processor flags */
	ulong	eax;		/* general (hah?) registers */
	ulong 	ecx;
	ulong	edx;
	ulong	ebx;
	ulong	esp;
	ulong	ebp;
	ulong	esi;
	ulong	edi;
	ulong	es;		/* segment selectors */
	ulong	cs;
	ulong	ss;
	ulong	ds;
	ulong	fs;
	ulong	gs;
	ulong	ldt;		/* local descriptor table */
	ulong	iomap;		/* io map base */
};
Tss tss;

/*
 *  segment descriptor initializers
 */
#define	DATASEGM(p) 	{ 0xFFFF, SEGG|SEGB|(0xF<<16)|SEGP|SEGPL(p)|SEGDATA|SEGW }
#define	EXECSEGM(p) 	{ 0xFFFF, SEGG|SEGD|(0xF<<16)|SEGP|SEGPL(p)|SEGEXEC|SEGR }
#define CALLGATE(s,o,p)	{ ((o)&0xFFFF)|((s)<<16), (o)&0xFFFF0000|SEGP|SEGPL(p)|SEGCG }
#define	D16SEGM(p) 	{ 0xFFFF, (0x0<<16)|SEGP|SEGPL(p)|SEGDATA|SEGW }
#define	E16SEGM(p) 	{ 0xFFFF, (0x0<<16)|SEGP|SEGPL(p)|SEGEXEC|SEGR }
#define	TSSSEGM(b,p)	{ ((b)<<16)|sizeof(Tss),\
			  ((b)&0xFF000000)|(((b)<<16)&0xFF)|SEGTSS|SEGPL(p)|SEGP }

/*
 *  global descriptor table describing all segments
 */
Segdesc gdt[] =
{
[NULLSEG]	{ 0, 0},		/* null descriptor */
[KDSEG]		DATASEGM(0),		/* kernel data/stack */
[KESEG]		EXECSEGM(0),		/* kernel code */
[UDSEG]		DATASEGM(3),		/* user data/stack */
[UESEG]		EXECSEGM(3),		/* user code */
[SYSGATE]	CALLGATE(KESEL,0,3),	/* call gate for system calls */
[TSSSEG]	TSSSEGM(0,0),		/* tss segment */
};

static Page	ktoppg;		/* prototype top level page table
				 * containing kernel mappings  */
static ulong	*kpt;		/* 2nd level page tables for kernel mem */
static ulong	*upt;		/* 2nd level page table for struct User */

#define ROUNDUP(s,v)	(((s)+(v-1))&~(v-1))
/*
 *  offset of virtual address into
 *  top level page table
 */
#define TOPOFF(v)	((v)>>(2*PGSHIFT-2))

/*
 *  offset of virtual address into
 *  bottom level page table
 */
#define BTMOFF(v)	(((v)>>(PGSHIFT))&(WD2PG-1))

void
mmudump(void)
{
	int i;
	ulong *z;
	z = (ulong*)gdt;
	for(i = 0; i < sizeof(gdt)/4; i+=2)
		print("%8.8lux %8.8lux\n", *z++, *z++);
	print("UESEL %lux UDSEL %lux\n", UESEL, UDSEL);
	print("KESEL %lux KDSEL %lux\n", KESEL, KDSEL);
	panic("done");
}

/*
 *  Create a prototype page map that maps all of memory into
 *  kernel (KZERO) space.  This is the default map.  It is used
 *  whenever the processor not running a process or whenever running
 *  a process which does not yet have its own map.
 */
void
mmuinit(void)
{
	int i, nkpt, npage, nbytes;
	ulong x;
	ulong y;
	ulong *top;

	/*
	 *  set up the global descriptor table
	 */
	x = (ulong)systrap;
	gdt[SYSGATE].d0 = (x&0xFFFF)|(KESEL<<16);
	gdt[SYSGATE].d1 = (x&0xFFFF0000)|SEGP|SEGPL(3)|SEGCG;
	x = (ulong)&tss;
	gdt[TSSSEG].d0 = (x<<16)|sizeof(Tss);
	gdt[TSSSEG].d1 = (x&0xFF000000)|((x>>16)&0xFF)|SEGTSS|SEGPL(0)|SEGP;
	putgdt(gdt, sizeof gdt);

	/*
	 *  set up system page tables.
	 *  map all of physical memory to start at KZERO.
	 *  map ROM BIOS at the usual place (F0000000).
	 *  leave a map entry for a user area.
	 */

	/*  allocate top level table */
	top = ialloc(BY2PG, 1);
	ktoppg.va = (ulong)top;
	ktoppg.pa = ktoppg.va & ~KZERO;

	/*  map all memory to KZERO */
	npage = conf.base1/BY2PG + conf.npage1;
	nbytes = PGROUND(npage*BY2WD);		/* words of page map */
	nkpt = nbytes/BY2PG;			/* pages of page map */
	kpt = ialloc(nbytes, 1);
	for(i = 0; i < npage; i++)
		kpt[i] = (0+i*BY2PG) | PTEVALID | PTEKERNEL | PTEWRITE;
	x = TOPOFF(KZERO);
	y = ((ulong)kpt)&~KZERO;
	for(i = 0; i < nkpt; i++)
		top[x+i] = (y+i*BY2PG) | PTEVALID | PTEKERNEL | PTEWRITE;

	/*  page table for u-> */
	upt = ialloc(BY2PG, 1);
	x = TOPOFF(USERADDR);
	y = ((ulong)upt)&~KZERO;
	top[x] = y | PTEVALID | PTEKERNEL | PTEWRITE;

	putcr3(ktoppg.pa);

	/*
	 *  set up the task segment
	 */
	tss.sp0 = USERADDR+BY2PG;
	tss.ss0 = KDSEL;
	tss.cr3 = ktoppg.pa;
	puttr(TSSSEL);
}

/*
 *  Get a page for a process's page map.
 *
 *  Each process maintains its own free list of page
 *  table pages.  All page table pages are put on
 *  this list in flushmmu().  flushmmu() doesn't
 *  putpage() the pages since the process will soon need
 *  them back.  Also, this avoids worrying about deadlocks
 *  twixt flushmmu() and putpage().
 *
 *  mmurelease() will give back the pages when the process
 *  exits.
 */
static Page*
mmugetpage(int clear)
{
	Proc *p = u->p;
	Page *pg;

	if(p->mmufree){
		pg = p->mmufree;
		p->mmufree = pg->next;
		if(clear)
			memset((void*)pg->va, 0, BY2PG);
	} else {
		pg = newpage(clear, 0, 0);
		pg->va = VA(kmap(pg));
	}
	return pg;
}

/*
 *  Put all page map pages on the process's free list and
 *  call mapstack to set up the prototype page map.  This
 *  effectively forgets all of the process's mappings.
 */
void
flushmmu(void)
{
	int s;
	Proc *p;
	Page *pg;

	if(u == 0)
		return;

	p = u->p;
	s = splhi();
	if(p->mmutop){
		p->mmutop->next = p->mmufree;
		p->mmufree = p->mmutop;
		for(pg = p->mmufree; pg->next; pg = pg->next)
			;
		pg->next = p->mmuused;
		p->mmutop = 0;
		p->mmuused = 0;
	}
	mapstack(u->p);
	splx(s);
}

/*
 *  Switch to a process's memory map.  If the process doesn't
 *  have a map yet, just use the prototype one that contains
 *  mappings for only the kernel and the User struct.
 */
void
mapstack(Proc *p)
{
	ulong tlbphys;
	int i;
	Page *pg;

	if(p->upage->va != (USERADDR|(p->pid&0xFFFF)) && p->pid != 0)
		panic("mapstack %d 0x%lux 0x%lux", p->pid, p->upage->pa, p->upage->va);

	if(p->mmutop)
		pg = p->mmutop;
	else
		pg = &ktoppg;

	/* map in u area */
	upt[0] = PPN(p->upage->pa) | PTEVALID | PTEKERNEL | PTEWRITE;

	/* tell processor about new page table (flushes cached entries) */
	putcr3(pg->pa);

	u = (User*)USERADDR;
}

/*
 *  give all page table pages back to the free pool.  This is called in sched()
 *  with palloc locked.
 */
void
mmurelease(Proc *p)
{
	Page *pg;
	Page *next;

	/* point 386 to protoype page map */
	putcr3(ktoppg.pa);

	/* give away page table pages */
	for(pg = p->mmufree; pg; pg = next){
		next = pg->next;
		simpleputpage(pg);
	}
	p->mmufree = 0;
	for(pg = p->mmuused; pg; pg = next){
		next = pg->next;
		simpleputpage(pg);
	}
	p->mmuused = 0;
	if(p->mmutop)
		simpleputpage(p->mmutop);
	p->mmutop = 0;
}

/*
 *  Add an entry into the mmu.
 */
#define FOURMEG (4*1024*1024)
void
putmmu(ulong va, ulong pa, Page *pg)
{
	int topoff;
	ulong *top;
	ulong *pt;
	Proc *p;
	char err[64];
	int x;

	if(u==0)
		panic("putmmu");

	p = u->p;

	if(va >= USERADDR && va < USERADDR + FOURMEG)
		print("putmmu in USERADDR page table 0x%lux\n", va);
	if((va & 0xF0000000) == KZERO)
		print("putmmu in kernel page table 0x%lux\n", va);

	/*
	 *  if no top level page, allocate one and copy the prototype
	 *  into it.
	 */
	if(p->mmutop == 0){
		/*
		 *  N.B. The assignment to pg is neccessary.
		 *  We can't assign to p->mmutop until after
		 *  copying ktoppg into the new page since we might
		 *  get scheded in this code and p->mmutop will be
		 *  pointing to a bad map.
		 */
		pg = mmugetpage(0);
		memmove((void*)pg->va, (void*)ktoppg.va, BY2PG);
		p->mmutop = pg;
	}
	top = (ulong*)p->mmutop->va;

	/*
	 *  if bottom level page table missing, allocate one and point
	 *  the top level page at it.
	 */
	topoff = TOPOFF(va);
	if(top[topoff] == 0){
		pg = mmugetpage(1);
		top[topoff] = PPN(pg->pa) | PTEVALID | PTEUSER | PTEWRITE;
		pg->next = p->mmuused;
		p->mmuused = pg;
	}

	/*
	 *  put in new mmu entry
	 */
	pt = (ulong*)(PPN(top[topoff])|KZERO);
	pt[BTMOFF(va)] = pa | PTEUSER;

	/* flush cached mmu entries */
	putcr3(p->mmutop->pa);
}

void
invalidateu(void)
{
	/* unmap u area */
	upt[0] = 0;

	/* flush cached mmu entries */
	putcr3(ktoppg.pa);
}

void
systrap(void)
{
	panic("system trap from user");
}
