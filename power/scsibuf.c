#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "io.h"

/*
 * The following routines
 *	scsibufreset, scsibuf, scsifree and scsialloc
 * provide a pool of buffers used for scsi i/o.
 * This is simplistic at best, and opportunities for
 * deadlock abound.
 */
#define Nbuf	2

struct {
	Lock;
	Scsibuf	*free;
} scsibufalloc;

Scsibuf *
scsialloc(ulong n)
{
	void *x;
	Scsibuf *b;

	if(n > 512*1024)
		panic("scsialloc too big: %d", n);

	b = xalloc(sizeof(Scsibuf));
	x = xalloc(n);

	b->phys = x;
	b->virt = x;

	return b;
}

void
scsibufreset(ulong datasize)
{
	int i;

	for(i = 0; i < Nbuf; i++)
		scsifree(scsialloc(datasize));
}

/*
 * get a scsi io buffer of DATASIZE size
 */
Scsibuf *
scsibuf(void)
{
	Scsibuf *b;

	for(;;) {
		lock(&scsibufalloc);
		b = scsibufalloc.free;
		if(b != 0) {
			scsibufalloc.free = b->next;
			unlock(&scsibufalloc);
			return b;
		}
		unlock(&scsibufalloc);
		resrcwait(0);
	}
	return 0;		/* not reached */
}

void
scsifree(Scsibuf *b)
{
	lock(&scsibufalloc);
	b->next = scsibufalloc.free;
	scsibufalloc.free = b;
	unlock(&scsibufalloc);
}
