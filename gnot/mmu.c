#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

struct
{
	Lock;
	int	init;
	KMap	*free;
	KMap	arena[MB4/BY2PG];	/* kernel mmu maps up to 4MB */
}kmapalloc;

/*
 * Called splhi, not in Running state
 */
void
mapstack(Proc *p)
{
	ulong tlbvirt, tlbphys;
	ulong next;
	MMU *mm, *mn, *me;


	if(p->upage->va != (USERADDR|(p->pid&0xFFFF)) && p->pid != 0)
		panic("mapstack %d 0x%lux 0x%lux", p->pid, p->upage->pa, p->upage->va);
	tlbvirt = USERADDR;
	tlbphys = PPN(p->upage->pa) | PTEVALID | PTEKERNEL;
	putkmmu(tlbvirt, tlbphys);
	u = (User*)USERADDR;

	if(p->newtlb) {
		flushmmu();
		p->newtlb = 0;
	}

	/*
	 *  if not a kernel process and this process was not the 
	 *  last process on this machine, flush & preload mmu
	 */
	if(!p->kp && p!=m->lproc){
		flushmmu();
		m->lproc = p;
	}
}

void
mmurelease(Proc *p)
{
	USED(p);
}

void
putkmmu(ulong tlbvirt, ulong tlbphys)
{
	if(!(tlbvirt&KZERO))
		panic("putkmmu");
	tlbvirt &= ~KZERO;
	KMAP[(tlbvirt&0x003FE000L)>>2] = tlbphys;
}

void
putmmu(ulong tlbvirt, ulong tlbphys, Page *p)
{
	if(tlbvirt&KZERO)
		panic("putmmu");
	tlbphys |= VTAG(tlbvirt)<<24;
	tlbvirt = (tlbvirt&0x003FE000L)>>2;
	UMAP[tlbvirt] = tlbphys;
}

void
flushmmu(void)
{
	flushcpucache();
	*PARAM &= ~TLBFLUSH_;
	*PARAM |= TLBFLUSH_;
}

void
kmapinit(void)
{
	KMap *k;
	int i, e;

	if(kmapalloc.init == 0){
		k = &kmapalloc.arena[0];
		k->va = KZERO|(MB4-256*1024-BY2PG);
		k->next = 0;
		kmapalloc.free = k;
		kmapalloc.init = 1;
		return;
	}

	e = (MB4 - 256*1024)/BY2PG;	/* screen lives at top 256K */
	i = PGROUND(((ulong)ialloc(0, 0))&~KZERO)/BY2PG;

	print("%lud free map registers\n", e-i);

	kmapalloc.free = 0;
	for(k=&kmapalloc.arena[i]; i<e; i++,k++){
		k->va = i*BY2PG|KZERO;
		kunmap(k);
	}
}

KMap*
kmap(Page *pg)
{
	KMap *k;
	int s;

	s = splhi();
	lock(&kmapalloc);
	k = kmapalloc.free;
	if(k == 0){
		dumpstack();
		panic("kmap");
	}
	kmapalloc.free = k->next;
	unlock(&kmapalloc);
	splx(s);

	k->pa = pg->pa;
	putkmmu(k->va, PPN(k->pa) | PTEVALID | PTEKERNEL);

	return k;
}

void
kunmap(KMap *k)
{
	int s;

	k->pa = 0;
	putkmmu(k->va, INVALIDPTE);

	s = splhi();
	lock(&kmapalloc);
	k->next = kmapalloc.free;
	kmapalloc.free = k;
	unlock(&kmapalloc);
	splx(s);
}

void
invalidateu(void)
{
	putkmmu(USERADDR, INVALIDPTE);
}
