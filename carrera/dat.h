typedef struct Conf	Conf;
typedef struct FPsave	FPsave;
typedef struct Cycmsg	Cycmsg;
typedef struct KMap	KMap;
typedef struct Lance	Lance;
typedef struct Lancemem	Lancemem;
typedef struct Label	Label;
typedef struct Lock	Lock;
typedef struct Mach	Mach;
typedef struct MMU	MMU;
typedef struct PMMU	PMMU;
typedef struct Softtlb	Softtlb;
typedef struct Ureg	Ureg;

/*
 *  parameters for sysproc.c
 */
#define AOUT_MAGIC	V_MAGIC

/*
 *  machine dependent definitions used by ../port/dat.h
 */

struct Lock
{
	ulong	key;			/* semaphore (non-zero = locked) */
	ulong	pc;
	void	*upa;
};

struct Label
{
	ulong	sp;
	ulong	pc;
};

struct Conf
{
	ulong	nmach;		/* processors */
	ulong	nproc;		/* processes */
	ulong	npage0;		/* total physical pages of memory */
	ulong	npage1;		/* total physical pages of memory */
	ulong	npage;		/* total physical pages of memory */
	ulong	upages;		/* user page pool */
	ulong	nimage;		/* number of page cache image headers */
	ulong	nswap;		/* number of swap pages */
	ulong	base0;		/* base of bank 0 */
	ulong	base1;		/* base of bank 1 */
	ulong	copymode;	/* 0 is copy on write, 1 is copy on reference */
	int	monitor;

	ulong	ipif;		/* Ip protocol interfaces */
	ulong	ip;		/* Ip conversations per interface */
	ulong	arp;		/* Arp table size */
	ulong	frag;		/* Ip fragment assemble queue size */
};

/*
 * floating point registers
 */
enum
{
	FPinit,
	FPactive,
	FPinactive,
};

struct	FPsave
{
	long	fpreg[32];
	long	fpstatus;
};

/*
 *  mmu goo in the Proc structure
 */
struct PMMU
{
	int	pidonmach[MAXMACH];
	/*
	 * I/O point for hotrod interfaces.
	 * This is the easiest way to allocate
	 * them, but not the prettiest or most general.
	 */
	Cycmsg	*kcyc;
	Cycmsg	*ucyc;
	Cycmsg	*fcyc;
};

#include "../port/portdat.h"

struct Cycmsg
{
	ulong	cmd;
	ulong	param[5];
	Rendez	r;
	uchar	intr;			/* flag: interrupt has occurred */
};

/* First FOUR members offsets known by l.s */
struct Mach
{
	/* the following are all known by l.s and cannot be moved */
	int	machno;			/* physical id of processor FIRST */
	Softtlb*stb;			/* Software tlb simulation SECOND */
	Proc*	proc;			/* process on this processor THIRD */
	ulong	splpc;			/* pc that called splhi() FOURTH */
	int	tlbfault;		/* # of tlb faults FIFTH */
	int	tlbpurge;		/* MUST BE SIXTH */

	/* the following is safe to move */
	ulong	ticks;			/* of the clock since boot time */
	Label	sched;			/* scheduler wakeup */
	Lock	alarmlock;		/* access to alarm list */
	void*	alarm;			/* alarms bound to this clock */
	int	lastpid;		/* last pid allocated on this machine */
	Proc*	pidproc[NTLBPID];	/* proc that owns tlbpid on this mach */
	Page*	ufreeme;		/* address of upage of exited process */
	Ureg*	ur;
	KMap*	kactive;		/* active on this machine */
	int	knext;
	uchar	ktlbx[NTLB];		/* tlb index used for kmap */
	uchar	ktlbnext;
	int	speed;			/* cpu speed */
	ulong	delayloop;		/* for the delay() routine */

	int	pfault;
	int	cs;
	int	syscall;
	int	load;
	int	intr;
	int	ledval;			/* value last written to LED */

	int	stack[1];
};

struct KMap
{
	Ref;
	ulong	virt;
	ulong	phys0;
	ulong	phys1;
	KMap*	next;
	KMap*	konmach[MAXMACH];
	Page*	pg;
	char	inuse;			/* number of procs using kmap */
};

#define	VA(k)		((k)->virt)
#define PPN(x)		((ulong)(x)>>6)

struct Softtlb
{
	ulong	virt;
	ulong	phys0;
	ulong	phys1;
};



struct
{
	Lock;
	short	machs;
	short	exiting;
}active;

extern KMap kpte[];
extern register Mach	*m;
extern register Proc	*up;