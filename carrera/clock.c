#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

#include	"ureg.h"

/*
 *  delay for l milliseconds more or less.  delayloop is set by
 *  clockinit() to match the actual CPU speed.
 */
void
delay(int l)
{
	ulong i, j;

	j = m->delayloop;
	while(l-- > 0)
		for(i=0; i < j; i++)
			;
}

void
clockinit(void)
{
	long x;

	m->delayloop = m->speed*100;
	do {
		x = rdcount();
		delay(10);
		x = rdcount() - x;
	} while(x < 0);

	/*
	 *  fix count
	 */
	m->delayloop = (m->delayloop*m->speed*1000*10)/x;
	if(m->delayloop == 0)
		m->delayloop = 1;

	wrcompare(rdcount()+(m->speed*1000000)/HZ);
}

void
clock(Ureg *ur)
{
	Proc *p;
	int i, nrun;

	wrcompare(rdcount()+(m->speed*1000000)/HZ);

	m->ticks++;
	if(m->proc)
		m->proc->pc = ur->pc;

	nrun = 0;
	if(m->machno == 0) {
		p = m->proc;
		if(p) {
			nrun++;
			p->time[p->insyscall]++;
		}
		for(i=1; i<conf.nmach; i++) {
			if(active.machs & (1<<i)) {
				p = MACHP(i)->proc;
				if(p) {
					p->time[p->insyscall]++;
					nrun++;
				}
			}
		}
		nrun = (nrdy+nrun)*1000;
		m->load = (m->load*19+nrun)/20;
	}

	ifjab();

	kproftimer(ur->pc);

	kmapinval();

	if((active.machs&(1<<m->machno)) == 0)
		return;

	if(active.exiting && (active.machs & (1<<m->machno))) {
		print("someone's exiting\n");
		exit(0);
	}

	checkalarms();
	uartclock();
	mouseclock();

	if(up == 0 || up->state != Running)
		return;

	if(anyready())
		sched();

	/* user profiling clock */
	if(ur->status & KUSER)
		(*(ulong*)(USTKTOP-BY2WD)) += TK2MS(1);	
}
