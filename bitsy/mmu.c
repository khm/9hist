#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"ureg.h"
#include	"../port/error.h"

/*
 *  to avoid mmu and cash flushing, we use the pid register in the MMU
 *  to map all user addresses.  Although there are 64 possible pids, we
 *  can only use 31 because there are only 32 protection domains and we
 *  need one for the kernel.  Pid i is thus associated with domain i.
 *  Domain 0 is used for the kernel.
 */

/* real protection bits */
enum
{
	/* level 1 descriptor bits */
	L1TypeMask=	(3<<0),
	L1Invalid=	(0<<0),
	L1PageTable=	(1<<0),
	L1Section=	(2<<0),
	L1Cached=	(1<<3),
	L1Buffered=	(1<<2),
	L1DomShift=	5,
	L1Domain0=	(0<<L1DomShift),
	L1KernelRW=	(0x1<<10),
	L1UserRO=	(0x2<<10),
	L1UserRW=	(0x3<<10),
	L1SectBaseMask=	(0xFFF<<20),
	L1PTBaseMask=	(0x3FFFFF<<10),
	
	/* level 2 descriptor bits */
	L2TypeMask=	(3<<0),
	L2SmallPage=	(2<<0),
	L2LargePage=	(1<<0),
	L2Cached=	(1<<3),
	L2Buffered=	(1<<2),
	L2KernelRW=	(0x55<<4),
	L2UserRO=	(0xAA<<4),
	L2UserRW=	(0xFF<<4),
	L2PageBaseMask=	(0xFFFFF<<12),

	/* domain values */
	Dnoaccess=	0,
	Dclient=	1,
	Dmanager=	3,
};

ulong *l1table;


/*
 *  We map all of memory, flash, and the zeros area with sections.
 *  Special use space is mapped on the fly with regmap.
 */
void
mmuinit(void)
{
	ulong a, o;
	ulong *t;

	/* get a prototype level 1 page */
	l1table = xspanalloc(16*1024, 16*1024, 0);
	memset(l1table, 0, 16*1024);

	/* map low mem */
	for(o = 0; o < 1*OneMeg; o += OneMeg)
		l1table[(0+o)>>20] = L1Section | L1KernelRW| L1Domain0 
			| L1Cached | L1Buffered
			| ((0+o)&L1SectBaseMask);

	/* map DRAM */
	for(o = 0; o < 128*OneMeg; o += OneMeg)
		l1table[(DRAMZERO+o)>>20] = L1Section | L1KernelRW| L1Domain0 
			| L1Cached | L1Buffered
			| ((PHYSDRAM0+o)&L1SectBaseMask);

	/* map zeros area */
	for(o = 0; o < 128 * OneMeg; o += OneMeg)
		l1table[(NULLZERO+o)>>20] = L1Section | L1KernelRW | L1Domain0
			| ((PHYSNULL0+o)&L1SectBaseMask);

	/* map flash */
	for(o = 0; o < 128 * OneMeg; o += OneMeg)
		l1table[(FLASHZERO+o)>>20] = L1Section | L1KernelRW | L1Domain0
			| L1Cached | L1Buffered
			| ((PHYSFLASH0+o)&L1SectBaseMask);

	/* map peripheral control module regs */
	mapspecial(0x80000000, OneMeg);

	/* map system control module regs */
	mapspecial(0x90000000, OneMeg);

	/*
	 *  double map start of ram to exception vectors
	 */
	a = EVECTORS;
	t = xspanalloc(BY2PG, 1024, 0);
	memset(t, 0, BY2PG);
	l1table[a>>20] = L1PageTable | L1Domain0 | (((ulong)t) & L1PTBaseMask);
	t[(a&0xfffff)>>PGSHIFT] = L2SmallPage | L2KernelRW | (PHYSDRAM0 & L2PageBaseMask);

	/* set up the domain register to cause all domains to obey pte access bits */
	putdac(Dclient);

	/* point to map */
	putttb((ulong)l1table);

	/* enable mmu */
	wbflush();
	flushcache();
	flushmmu();
	mmuenable();
}

/*
 *  map special space uncached, assume that the space isn't already mapped
 */
void*
mapspecial(ulong pa, int len)
{
	ulong *t;
	ulong va, i, base, end, off, entry;
	int livelarge;
	ulong* rv;

	rv = nil;
	livelarge = len >= 128*1024;
	if(livelarge){
		base = pa & ~(OneMeg-1);
		end = (pa+len-1) & ~(OneMeg-1);
	} else {
		base = pa & ~(BY2PG-1);
		end = (pa+len-1) & ~(BY2PG-1);
	}
	off = pa - base;

	for(va = REGZERO; va < REGTOP && base <= end; va += OneMeg){
		switch(l1table[va>>20] & L1TypeMask){
		default:
			/* found unused entry on level 1 table */
			if(livelarge){
				if(rv == nil)
					rv = (ulong*)(va+off);
				l1table[va>>20] = L1Section | L1KernelRW | L1Domain0 |
							(base & L1SectBaseMask);
				base += OneMeg;
				continue;
			} else {

				/* create an L2 page table and keep going */
				t = xspanalloc(BY2PG, 1024, 0);
				memset(t, 0, BY2PG);
				l1table[va>>20] = L1PageTable | L1Domain0 |
							(((ulong)t) & L1PTBaseMask);
			}
			break;
		case L1Section:
			/* if it's already mapped in a one meg area, don't remap */
			entry = l1table[va>>20];
			i = entry & L1SectBaseMask;
			if(pa >= i && (pa+len) <= i + OneMeg)
			if((entry & ~L1SectBaseMask) == (L1Section | L1KernelRW | L1Domain0))
				return (void*)(va + (pa & (OneMeg-1)));
				
			continue;
		case L1PageTable:
			if(livelarge)
				continue;
			break;
		}

		/* here if we're using page maps instead of sections */
		t = (ulong*)(l1table[va>>20] & L1PTBaseMask);
		for(i = 0; i < OneMeg; i += BY2PG){
			entry = t[i>>PGSHIFT];

			/* found unused entry on level 2 table */
			if((entry & L2TypeMask) != L2SmallPage){
				if(rv == nil)
					rv = (ulong*)(va+i+off);
				t[i>>PGSHIFT] = L2SmallPage | L2KernelRW | 
						(base & L2PageBaseMask);
				base += BY2PG;
				continue;
			}
		}
	}

	/* didn't fit */
	if(base <= end)
		return nil;

	return rv;
}

/*
 *  table to map fault.c bits to physical bits
 */
static ulong mmubits[16] =
{
	[PTEVALID]				L2SmallPage|L2Cached|L2Buffered|L2UserRO,
	[PTEVALID|PTEWRITE]			L2SmallPage|L2Cached|L2Buffered|L2UserRW,
	[PTEVALID|PTEUNCACHED]			L2SmallPage|L2UserRO,
	[PTEVALID|PTEUNCACHED|PTEWRITE]		L2SmallPage|L2UserRW,

	[PTEKERNEL|PTEVALID]			L2SmallPage|L2Cached|L2Buffered|L2KernelRW,
	[PTEKERNEL|PTEVALID|PTEWRITE]		L2SmallPage|L2Cached|L2Buffered|L2KernelRW,
	[PTEKERNEL|PTEVALID|PTEUNCACHED]		L2SmallPage|L2KernelRW,
	[PTEKERNEL|PTEVALID|PTEUNCACHED|PTEWRITE]	L2SmallPage|L2KernelRW,
};

/*
 *  add an entry to the current map
 */
void
putmmu(ulong va, ulong pa, Page*)
{
	Page *p;
	ulong *t;

iprint("putmmu(0x%.8lux, 0x%.8lux)\n", va, pa);
	/* always point L1 entry to L2 page, can't hurt */
	p = up->l1page[va>>20];
	if(p == nil){
		p = auxpage();
		if(p == nil)
			pexit("out of memory", 1);
		p->va = VA(kmap(p));
		up->l1page[va>>20] = p;
	}
	l1table[va>>20] = L1PageTable | L1Domain0 | (p->pa & L1PTBaseMask);
iprint("%lux[%lux] = %lux\n", l1table, va>>20, l1table[va>>20]);
	up->l1table[va>>20] = l1table[va>>20];
	t = (ulong*)p->va;

	/* set L2 entry */
	t[(va & (OneMeg-1))>>PGSHIFT] = mmubits[pa & (PTEKERNEL|PTEVALID|PTEUNCACHED|PTEWRITE)]
		| (pa & ~(PTEKERNEL|PTEVALID|PTEUNCACHED|PTEWRITE));
iprint("%lux[%lux] = %lux\n", (ulong)t, (va & (OneMeg-1))>>PGSHIFT, t[(va & (OneMeg-1))>>PGSHIFT]);

	flushmmu();
}

/*
 *  this is called with palloc locked so the pagechainhead is kosher
 */
void
mmurelease(Proc* p)
{
	Page *pg;
	int i;

	for(i = 0; i < nelem(p->l1page); i++){
		pg = p->l1page[i];
		if(pg == nil)
			continue;
		if(--pg->ref)
			panic("mmurelease: pg->ref %d\n", pg->ref);
		pagechainhead(pg);
		p->l1page[i] = nil;
	}
}

void
mmuswitch(Proc* p)
{
//	flushcache();	/* drain and flush the cache */
//	flushmmu();
//	memmove(l1table, p->l1table, sizeof(p->l1table));
}