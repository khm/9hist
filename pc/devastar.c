#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"

#include	"devtab.h"

/*
 *  Stargate's Avanstar serial board.  There are ISA, EISA, microchannel
 *  versions.  We only handle the ISA one.
 *
 *  At the expense of performance, I've tried to be careful about
 *  endian-ness to make this convertable to other ISA bus machines.
 *  However, xchngus() is in assembler and will have to be translated.
 */
#define LENDIAN 1

/* unsigned short little endian representation */
#ifdef LENDIAN
#define LEUS(x) (x)
#else
#define LEUS(x) ( (((x)<<8)&0xff00) | (((x)>>8)&0xff) )
#endif LENDIAN

typedef struct Astar Astar;
typedef struct Astarchan Astarchan;

enum
{
	/* ISA control ports */
	ISAid=		0,		/* Id port and its values */
	 ISAid0=	 0xEC,
	 ISAid1=	 0x13,
	ISActl1=	1,		/* board control */
	 ISAien=	 1<<7,		/*  interrupt enable */
	 ISAirq=	 7<<4,		/*  mask for irq code */
	 ISAnotdl=	 1<<1,		/*  download bit (0 == download) */
	 ISApr=		 1<<0,		/*  program ready */
	ISActl2=	2,		/* board control */
	 ISA186ien=	 1<<7,		/*  I186 irq enable bit state */
	 ISA186idata=	 1<<6,		/*  I186 irq data bit state */
	 ISAmen=	 1<<4,		/*  enable memory to respond to ISA cycles */
	 ISAmbank=	 0,		/*  shift for 4 bit memory bank */
	ISAmaddr=	3,		/* bits 14-19 of the boards mem address */
	ISAstat1=	4,		/* board status (1 bit per channel) */
	ISAstat2=	5,		/* board status (1 bit per channel) */

	Maxcard=	8,
	Pramsize=	64*1024,	/* size of program ram */
	Pageshift=	14,		/* footprint of card mem in ISA space */
	Pagesize=	1<<Pageshift,
	Pagemask=	Pagesize-1,
};

#define APAGE(x) ((x)>>Pageshift)

#define LOCKPAGE(a, o) if((a)->needpage){ilock(&(a)->pagelock);setpage(a, o);}
#define UNLOCKPAGE(a) if((a)->needpage)iunlock(&(a)->pagelock)

/* IRQ codes */
static int isairqcode[16] =
{
	-1,	-1,	-1,	0<<4,
	1<<4,	2<<4,	-1,	-1,
	-1,	3<<4,	4<<4,	5<<4,
	6<<4,	-1,	-1,	7<<4,
};

/* control program global control block */
typedef struct GCB GCB;
struct GCB
{
	ushort	cmd;		/* command word */
	ushort	status;		/* status word */
	ushort	serv;		/* service request, must be accessed via exchange 'X' */
	ushort	avail;		/* available buffer space */
	ushort	type;		/* board type */
	ushort	cpvers;		/* control program version */
	ushort	ccbn;		/* control channel block count */
	ushort	ccboff;		/* control channel block offset */
	ushort	ccbsz;		/* control channel block size */
	ushort	cmd2;		/* command word 2 */
	ushort	status2;	/* status word 2 */
	ushort	errserv;	/* comm error service request 'X' */
	ushort	inserv;		/* input buffer service request 'X' */
	ushort	outserv;	/* output buffer service request 'X' */
	ushort	modemserv;	/* modem change service request 'X' */
	ushort	cmdserv;	/* channel command service request 'X' */
};

enum
{
	/* GCB.cmd commands/codes */
	Greadycmd=	0,
	Gdiagcmd=	1,
	Gresetcmd=	2,

	/* GCB.status values */
	Gready=		0,
	Gstopped=	1,
	Gramerr=	2,
	Gbadcmd=	3,
	Gbusy=		4,

	/* GCB.type values */
	Gx00m=		0x6,
	G100e=		0xA,
	Gx00i=		0xC,

	/* GCB.cmd2 bit */
	Gintack=	0x1,

	/* GCB.status2 bits */
	Ghas232=	(1<<0),
	Ghas422=	(1<<1),
	Ghasmodems=	(1<<2),
	Ghasrj11s=	(1<<7),
	Ghasring=	(1<<8),
	Ghasdcd=	(1<<9),
	Ghasdtr=	(1<<10),
	Ghasdsr=	(1<<11),
	Ghascts=	(1<<12),
	Ghasrts=	(1<<13),
};

/* control program channel control block */
typedef struct CCB CCB;
struct CCB
{
	ushort	baud;		/* baud rate */
	ushort	format;		/* data format */
	ushort	proto;		/* line protocol */
	ushort	insize;		/* input buffer size */
	ushort	outsize;	/* output buffer size */
	ushort 	intrigger;	/* input buffer trigger rate */
	ushort	outlow;		/* output buffer low water mark */
	char	xon[2];		/* xon characters */
	ushort	inhigh;		/* input buffer high water mark */
	ushort	inlow;		/* input buffer low water mark */
	ushort	cmd;		/* channel command */
	ushort	status;		/* channel status */
	ushort	inbase;		/* input buffer start addr */
	ushort 	inlim;		/* input buffer ending addr */
	ushort	outbase;	/* output buffer start addr */
	ushort 	outlim;		/* output buffer ending addr */
	ushort	inwp;		/* input read and write pointers */
	ushort	inrp;
	ushort	outwp;		/* output read and write pointers */
	ushort	outrp;
	ushort	errstat;	/* error status */
	ushort	badp;		/* bad character pointer */
	ushort	mctl;		/* modem control */
	ushort	mstat;		/* modem status */
	ushort	bstat;		/* blocking status */
	ushort	rflag;		/* character received flag */
	char	xoff[2];	/* xoff characters */
	ushort	status2;
	char	strip[2];	/* strip/error characters */
};

enum
{
	/* special baud rate codes for CCB.baud */
	Cb76800=	0xff00,
	Cb115200=	0xff01,

	/* CCB.format fields */
	Clenmask=	3<<0,	/* data bits */
	C1stop=		0<<2,	/* stop bits */
	C2stop=		1<<2,
	Cnopar=		0<<3,	/* parity */
	Coddpar=	1<<3,
	Cevenpar=	3<<3,
	Cmarkpar=	5<<3,
	Cspacepar=	7<<3,
	Cparmask=	7<<3,
	Cnormal=	0<<6,	/* normal mode */
	Cecho=		1<<6,	/* echo mode */
	Clloop=		2<<6,	/* local loopback */
	Crloop=		3<<6,	/* remote loopback */

	/* CCB.proto fields */
	Cobeyxon=	1<<0,	/* obey received xoff/xon controls */
	Canyxon=	1<<1,	/* any rcvd character restarts xmit */
	Cgenxon=	1<<2,	/* generate xoff/xon controls */
	Cobeycts=	1<<3,	/* obey hardware flow ctl */
	Cgendtr=	1<<4,	/* dtr off when uart rcvr full */
	C½duplex=	1<<5,	/* rts off while xmitting */
	Cgenrts=	1<<6,	/* generate hardware flow ctl */
	Cmctl=		1<<7,	/* direct modem control via CCB.mctl */
	Cstrip=		1<<12,	/* to strip out characters */
	Ceia422=	1<<13,	/* to select eia 422 lines */

	/* CCB.cmd fields */
	Cconfall=	1<<0,	/* configure channel and UART */
	Cconfchan=	1<<1,	/* configure just channel */
	Cflushin=	1<<2,	/* flush input buffer */
	Cflushout=	1<<3,	/* flush output buffer */
	Crcvena=	1<<4,	/* enable receiver */
	Crcvdis=	1<<5,	/* disable receiver */
	Cxmtena=	1<<6,	/* enable transmitter */
	Cxmtdis=	1<<7,	/* disable transmitter */
	Cmreset=	1<<9,	/* reset modem */

	/* CCB.errstat fields */
	Coverrun=	1<<0,
	Cparity=	1<<1,
	Cframing=	1<<2,
	Cbreak=		1<<3,

	/* CCB.mctl fields */
	Cdtrctl=	1<<0,
	Crtsctl=	1<<1,
	Cbreakctl=	1<<4,

	/* CCB.mstat fields */
	Cctsstat=	1<<0,
	Cdsrstat=	1<<1,
	Cristat=	1<<2,
	Cdcdstat=	1<<3,

	/* CCB.bstat fields */
	Cbrcvoff=	1<<0,
	Cbxmtoff=	1<<1,
	Clbxoff=	1<<2,	/* transmitter blocked by XOFF */
	Clbcts=		1<<3,	/* transmitter blocked by CTS */
	Crbxoff=	1<<4,	/* remote blocked by xoff */
	Crbrts=		1<<4,	/* remote blocked by rts */
};

/* host per controller info */
struct Astar
{
	QLock;				/* lock for rendez */
	Rendez		r;		/* waiting for command completion */

	ISAConf;
	Lock		pagelock;	/* lock for setting page */
	int		page;		/* page currently mapped */
	int		id;		/* from plan9.ini */
	int		nchan;		/* number of channels */
	Astarchan	*c;		/* channels */
	int		ramsize;	/* 16k or 256k */
	int		memsize;	/* size of memory currently mapped */
	int		needpage;
	GCB		*gcb;		/* global board comm area */
	uchar		*addr;		/* base of memory area */
	int		running;
};

/* host per channel info */
struct Astarchan
{
	QLock;		/* lock for rendez */
	Rendez	r;	/* waiting for command completion */

	Astar	*a;	/* controller */
	CCB	*ccb;	/* channel control block */
	int	perm;
	int	opens;
	int	baud;

	Queue	*iq;
	Queue	*oq;
};

Astar *astar[Maxcard];
static int nastar;

enum
{
	Qmem=	0,
	Qbctl,
	Qdata,
	Qctl,
	Qstat,
};
#define TYPE(x)		((x)&0xff)
#define BOARD(x)	((((x)&~CHDIR)>>16)&0xff)
#define CHAN(x)		((((x)&~CHDIR)>>8)&0xff)
#define QID(b,c,t)	(((b)<<16)|((c)<<8)|(t))

static int	astarsetup(Astar*);
static void	astarintr(Ureg*, void*);
static void	astarkick(Astarchan*);
static void	astarkickin(Astarchan*);
static void	enable(Astarchan*);
static void	disable(Astarchan*);
static void	astarctl(Astarchan*, char*);

/*
 *  Only 16k maps into ISA space
 */
static void
setpage(Astar *a, ulong offset)
{
	int i;

	i = APAGE(offset);
	if(i == a->page)
		return;
	outb(a->port+ISActl2, ISAmen|i);
	a->page = i;
}

/*
 *  generate the astar directory entries
 */
int
astargen(Chan *c, Dirtab *tab, int ntab, int i, Dir *db)
{
	int dev, sofar, ch, t;
	extern ulong kerndate;

	memset(db, 0, sizeof(Dir));

	USED(tab, ntab);
	sofar = 0;

	for(dev = 0; dev < nastar; dev++){
		if(sofar == i){
			sprint(db->name, "astar%dmem", astar[dev]->id);
			db->qid.path = QID(dev, 0, Qmem);
			db->mode = 0660;
			db->length = astar[dev]->memsize;
			break;
		}
		sofar++;

		if(sofar == i){
			sprint(db->name, "astar%dctl", astar[dev]->id);
			db->qid.path = QID(dev, 0, Qbctl);
			db->mode = 0660;
			break;
		}
		sofar++;

		if(i - sofar < 3*astar[dev]->nchan){
			i -= sofar;
			ch = i/3;
			t = i%3;
			switch(t){
			case 0:
				sprint(db->name, "eia%d%2.2d", dev, ch);
				db->mode = astar[dev]->c[ch].perm;
				db->qid.path = QID(dev, ch, Qdata);
				break;
			case 1:
				sprint(db->name, "eia%d%2.2dctl", dev, ch);
				db->mode = astar[dev]->c[ch].perm;
				db->qid.path = QID(dev, ch, Qctl);
				break;
			case 2:
				sprint(db->name, "eia%d%2.2dstat", dev, ch);
				db->mode = 0444;
				db->qid.path = QID(dev, ch, Qstat);
				break;
			}
			break;
		}
		sofar += 3*astar[dev]->nchan;
	}

	if(dev == nastar)
		return -1;

	db->qid.vers = 0;
	db->atime = seconds();
	db->mtime = kerndate;
	memmove(db->uid, eve, NAMELEN);
	memmove(db->gid, eve, NAMELEN);
	db->type = devchar[c->type];
	db->dev = c->dev;
	if(c->flag&CMSG)
		db->mode |= CHMOUNT;

	return 1;
}

void
astarreset(void)
{
	int i, c;
	Astar *a;

	for(i = 0; i < Maxcard; i++){
		a = astar[nastar] = xalloc(sizeof(Astar));
		if(isaconfig("serial", i, a) == 0){
			xfree(a);
			astar[nastar] = 0;
			continue;
		}

		if(strcmp(a->type, "a100i") == 0 || strcmp(a->type,"A100I") == 0)
			a->ramsize = 16*1024;
		else if(strcmp(a->type, "a200i") == 0 || strcmp(a->type,"A200I") == 0)
			a->ramsize = 256*1024;
		else {
			xfree(a);
			astar[nastar] = 0;
			continue;
		}

		if(a->mem == 0)
			a->mem = 0xD4000;
		if(a->irq == 0)
			a->irq = 15;
		a->id = i;

		if(astarsetup(a) < 0){
			xfree(a);
			astar[nastar] = 0;
			continue;
		}
		print("serial%d avanstar port %d addr %lux irq %d\n", i, a->port,
			a->addr, a->irq);
		nastar++;

		c = inb(a->port+ISActl1);
		outb(a->port+ISActl1, c & ~ISAnotdl);
		a->memsize = Pramsize;

		c = inb(a->port+ISActl2);
		outb(a->port+ISActl2, c & ~ISAmen);
		a->page = -1;
	}
}

/* isa ports an ax00i can appear at */
int isaport[] = { 0x200, 0x208, 0x300, 0x308, 0x600, 0x608, 0x700, 0x708, 0 };

static int
astarprobe(int port)
{
	uchar c, c1;

	if(port < 0)
		return 0;

	c = inb(port+ISAid);
	c1 = inb(port+ISAid);
	return (c == ISAid0 && c1 == ISAid1)
		|| (c == ISAid1 && c1 == ISAid0);
}

static int
astarsetup(Astar *a)
{
	int i, found;

	/* see if the card exists */
	found = 0;
	if(a->port == 0)
		for(i = 0; isaport[i]; i++){
			a->port = isaport[i];
			found = astarprobe(isaport[i]);
			if(found){
				isaport[i] = -1;
				break;
			}
		}
	else
		found = astarprobe(a->port);
	if(!found){
		print("avanstar %d not found\n", a->id);
		return -1;
	}

	/* set memory address */
	outb(a->port+ISAmaddr, (a->mem>>12) & 0xfc);
	a->gcb = (GCB*)(KZERO | a->mem);
	a->addr = (uchar*)(KZERO | a->mem);

	/* set interrupt level */
	if(isairqcode[a->irq] == -1){
		print("Avanstar %d bad irq %d\n", a->id, a->irq);
		return -1;
	}

	return 0;
}

void
astarinit(void)
{
}

Chan*
astarattach(char *spec)
{
	return devattach('G', spec);
}

Chan*
astarclone(Chan *c, Chan *nc)
{
	return devclone(c, nc);
}

int
astarwalk(Chan *c, char *name)
{
	return devwalk(c, name, 0, 0, astargen);
}

void
astarstat(Chan *c, char *dp)
{
	devstat(c, dp, 0, 0, astargen);
}

Chan*
astaropen(Chan *c, int omode)
{
	Astar *a;
	Astarchan *ac;

	c = devopen(c, omode, 0, 0, astargen);
	a = astar[BOARD(c->qid.path)];

	switch(TYPE(c->qid.path)){
	case Qmem:
	case Qbctl:
		if(!iseve())
			error(Eperm);
		break;
	case Qdata:
	case Qctl:
		ac = a->c + CHAN(c->qid.path);
		qlock(ac);
		if(waserror()){
			qunlock(ac);
			nexterror();
		}
		if(ac->opens++ == 0){
			enable(ac);
			qreopen(ac->iq);
			qreopen(ac->oq);
		}
		qunlock(ac);
		poperror();
		break;
	}

	return c;
}

void
astarclose(Chan *c)
{
	Astar *a;
	Astarchan *ac;

	a = astar[BOARD(c->qid.path)];

	switch(TYPE(c->qid.path)){
	case Qmem:
		break;
	case Qdata:
	case Qctl:
		ac = a->c + CHAN(c->qid.path);
		qlock(ac);
		if(waserror()){
			qunlock(ac);
			nexterror();
		}
		if(--ac->opens == 0){
			disable(ac);
			qclose(ac->iq);
			qclose(ac->oq);
		}
		qunlock(ac);
		poperror();
		break;
	}
}

/*
 *  read ISA mapped memory
 */
static long
memread(Astar *a, uchar *to, long n, ulong offset)
{
	uchar *from, *e, *tp;
	int i, rem;
	uchar tmp[256];

	if(offset+n > a->memsize){
		if(offset >= a->memsize)
			return 0;
		n = a->memsize - offset;
	}

	for(rem = n; rem > 0; rem -= i){

		/* map in right piece of memory */
		i = offset&Pagemask;
		from = a->addr + i;
		i = Pagesize - i;
		if(i > rem)
			i = rem;
		if(i > sizeof(tmp))
			i = sizeof(tmp);

		/*
		 *  byte at a time so endian doesn't matter,
		 *  go via tmp to avoid pagefaults while ilock'd
		 */
		tp = tmp;
		LOCKPAGE(a, offset);
		for(e = tp + i; tp < e;)
			*tp++ = *from++;
		UNLOCKPAGE(a);
		memmove(to, tmp, i);
		to += i;

		offset += i;
	}

	return n;
}

/*
 *  read ISA status
 */
static long
bctlread(Astar *a, void *buf, long n, ulong offset)
{
	char s[128];

	sprint(s, "id %4.4ux ctl1 %2.2ux ctl2 %2.2ux maddr %2.2ux stat %4.4ux",
		(inb(a->port+ISAid)<<8)|inb(a->port+ISAid),
		inb(a->port+ISActl1), inb(a->port+ISActl2), 
		inb(a->port+ISAmaddr),
		(inb(a->port+ISAstat2)<<8)|inb(a->port+ISAstat1));
	return readstr(offset, buf, n, s);
}

long
astarread(Chan *c, void *buf, long n, ulong offset)
{
	Astar *a;
	Astarchan *ac;

	if(c->qid.path & CHDIR)
		return devdirread(c, buf, n, 0, 0, astargen);

	switch(TYPE(c->qid.path)){
	case Qmem:
		return memread(astar[BOARD(c->qid.path)], buf, n, offset);
	case Qbctl:
		return bctlread(astar[BOARD(c->qid.path)], buf, n, offset);
	case Qdata:
		a = astar[BOARD(c->qid.path)];
		ac = a->c + CHAN(c->qid.path);
		return qread(ac->iq, buf, n);
	}

	return 0;
}

/*
 *  write ISA mapped memory
 */
static long
memwrite(Astar *a, uchar *from, long n, ulong offset)
{
	uchar *to, *e, *tp;
	int i, rem;
	uchar tmp[256];

	if(offset+n > a->memsize){
		if(offset >= a->memsize)
			return 0;
		n = a->memsize - offset;
	}

	for(rem = n; rem > 0; rem -= i){

		/* map in right piece of memory */
		i = offset&Pagemask;
		to = a->addr + i;
		i = Pagesize - i;
		if(i > rem)
			i = rem;
		if(i > sizeof(tmp))
			i = sizeof(tmp);
		
		/*
		 *  byte at a time so endian doesn't matter,
		 *  go via tmp to avoid pagefaults while ilock'd
		 */
		memmove(tmp, from, i);
		tp = tmp;
		LOCKPAGE(a, offset);
		for(e = tp + i; tp < e;)
			*to++ = *tp++;
		UNLOCKPAGE(a);
		from += i;

		offset += i;
	}

	return n;
}

/*
 *  start control progarm
 */
static void
startcp(Astar *a)
{
	int n, i, sz;
	uchar *x;
	CCB *ccb;
	Astarchan *ac;

	if(a->running)
		error(Eio);

	/* take board out of download mode and enable IRQ */
	outb(a->port+ISActl1, ISAien|isairqcode[a->irq]|ISAnotdl);
	a->memsize = a->ramsize;
	setpage(a, 0);
	if(a->memsize <= Pagesize)
		a->needpage = 0;
	else
		a->needpage = 1;

	/* wait for control program to signal life */
	for(i = 0; i < 21; i++){
		if(inb(a->port+ISActl1) & ISApr)
			break;
		tsleep(&a->r, return0, 0, 500);
	}
	if((inb(a->port+ISActl1) & ISApr) == 0){
		print("astar%d program not ready\n", a->id);
		error(Eio);
	}

	if(waserror()){
		UNLOCKPAGE(a);
		poperror();
	}
	LOCKPAGE(a, 0);
	i = LEUS(a->gcb->type);
	switch(i){
	default:
		print("astar%d wrong board type %ux\n", a->id, i);
		error(Eio);
	case 0xc:
		break;
	}

	/* check assumptions */
	n = LEUS(a->gcb->ccbn);
	if(n != 8 && n != 16){
		print("astar%d had %d channels?\n", a->id, i);
		error(Eio);
	}
	x = a->addr + LEUS(a->gcb->ccboff);
	sz = LEUS(a->gcb->ccbsz);
	if(x+n*sz > a->addr+Pagesize){
		print("astar%d ccb's not in 1st page\n", a->id);
		error(Eio);
	}
	for(i = 0; i < n; i++){
		ccb = (CCB*)(x + i*sz);
		if(APAGE(LEUS(ccb->inbase)) != APAGE(LEUS(ccb->inlim)) ||
		   APAGE(LEUS(ccb->outbase)) != APAGE(LEUS(ccb->outlim))){
			print("astar%d chan buffer spans pages\n", a->id);
			error(Eio);
		}
	}

	UNLOCKPAGE(a);
	poperror();

	/* setup the channels */
	a->running = 1;
	a->nchan = i;
	a->c = xalloc(a->nchan * sizeof(Astarchan));
	for(i = 0; i < a->nchan; i++){
		ac = &a->c[i];
		ac->a = a;
		ac->ccb = (CCB*)x;
		ac->perm = 0660;
		ac->iq = qopen(4*1024, 0, astarkickin, ac);
		ac->oq = qopen(4*1024, 0, astarkick, ac);
		x += sz;
	}

	/* enable control program interrupt generation */
	setvec(Int0vec + a->irq, astarintr, a);
	LOCKPAGE(a, 0);
	a->gcb->cmd2 = LEUS(Gintack);
	UNLOCKPAGE(a);
}

static void
bctlwrite(Astar *a, char *cmsg)
{
	uchar c;

	if(waserror()){
		qunlock(a);
		nexterror();
	}
	qlock(a);

	if(a->running)
		error(Eio);

	if(strncmp(cmsg, "download", 8) == 0){
		/* put board in download mode */
		c = inb(a->port+ISActl1);
		outb(a->port+ISActl1, c & ~ISAnotdl);
		a->memsize = Pramsize;
		a->needpage = 1;

		/* enable ISA access to first 16k */
		setpage(a, 0);

	} else if(strncmp(cmsg, "sharedmem", 9) == 0){
		/* map shared memory */
		c = inb(a->port+ISActl1);
		outb(a->port+ISActl1, c | ISAnotdl);
		a->memsize = a->ramsize;

		/* enable ISA access to first 16k */
		outb(a->port+ISActl2, ISAmen);

	} else if(strncmp(cmsg, "run", 3) == 0){
		/* start up downloaded program */
		startcp(a);

	} else
		error(Ebadarg);

	qunlock(a);
	poperror();
}

/*
 *  Send a command to a channel
 *  (must be called with ac qlocked)
 */
static int
chancmddone(void *arg)
{
	Astarchan *ac = arg;
	int x;

	LOCKPAGE(ac->a, 0);
	x = ac->ccb->cmd;
	UNLOCKPAGE(ac->a);

	return !x;
}
static void
chancmd(Astarchan *ac, int cmd)
{
	CCB *ccb;
	int status;

	ccb = ac->ccb;

	LOCKPAGE(ac->a, 0);
	ccb->cmd = cmd;
	UNLOCKPAGE(ac->a);

	/* wait outside of lock */
	tsleep(&ac->r, chancmddone, ac, 1000);

	LOCKPAGE(ac->a, 0);
	status = ccb->status;
	cmd = ccb->cmd;
	UNLOCKPAGE(ac->a);
	if(cmd){
		print("astar%d cmd didn't terminate\n", ac->a->id);
		error(Eio);
	}
	if(status){
		print("astar%d cmd status %ux\n", ac->a->id, status);
		error(Eio);
	}
}

/*
 *  enable a channel for IO, set standard params.
 *  (must be called with ac qlocked)
 */
static void
enable(Astarchan *ac)
{
	Astar *a = ac->a;
	int n;

	/* make sure we control RTS, DTR and break */
	LOCKPAGE(a, 0);
	n = LEUS(ac->ccb->proto) | Cmctl;
	ac->ccb->proto = LEUS(n);
	ac->ccb->outlow = 64;
	UNLOCKPAGE(a);
	chancmd(ac, Cconfall);

	astarctl(ac, "b9600");
	astarctl(ac, "l8");
	astarctl(ac, "p0");
	astarctl(ac, "d1");
	astarctl(ac, "r1");

	chancmd(ac, Crcvena|Cxmtena|Cconfall);
}

/*
 *  disable a channel for IO
 *  (must be called with ac qlocked)
 */
static void
disable(Astarchan *ac)
{
	int n;

	astarctl(ac, "d0");
	astarctl(ac, "r0");

	LOCKPAGE(ac->a, 0);
	n = LEUS(ac->ccb->proto) | Cmctl;
	ac->ccb->proto = LEUS(n);
	UNLOCKPAGE(ac->a);
	chancmd(ac, Crcvdis|Cxmtdis|Cflushin|Cflushout|Cconfall);
}

/*
 *  change channel parameters
 *  (must be called with ac qlocked)
 */
static void
astarctl(Astarchan *ac, char *cmd)
{
	int i, n;
	int command;
	CCB *ccb;
	Astar *a;

	/* let output drain for a while */
	for(i = 0; i < 16 && qlen(ac->oq); i++)
		tsleep(&ac->r, qlen, ac->oq, 125);

	if(strncmp(cmd, "break", 5) == 0)
		cmd = "k";

	ccb = ac->ccb;
	command = 0;
	i = atoi(cmd+1);

	a = ac->a;
	LOCKPAGE(a, 0);

	switch(*cmd){
	case 'B':
	case 'b':
		/* set baud rate (high rates are special - only 16 bits) */
		switch(i){
		case 76800:
			ccb->baud = LEUS(Cb76800);
			break;
		case 115200:
			ccb->baud = LEUS(Cb115200);
			break;
		default:
			ccb->baud = LEUS(i);
			break;
		}
		ac->baud = i;

		/* set trigger level to about 50  per second */
		n = i/500;
		i = (LEUS(ccb->inlim) - LEUS(ccb->inbase))/2;
		if(n > i)
			n = i;
		ccb->intrigger = LEUS(n);

		command = Cconfall;
		break;
	case 'D':
	case 'd':
		n = LEUS(ccb->mctl);
		if(i)
			 n |= Cdtrctl;
		else
			 n &= ~Cdtrctl;
		ccb->mctl = LEUS(n);
		break;
	case 'f':
	case 'F':
		qflush(ac->oq);
		break;
	case 'H':
	case 'h':
		qhangup(ac->iq);
		qhangup(ac->oq);
		break;
	case 'L':
	case 'l':
		n = i - 5;
		if(n < 0 || n > 3)
			error(Ebadarg);
		n |= LEUS(ccb->format) & ~Clenmask;
		ccb->format = LEUS(n);
		command = Cconfall;
		break;
	case 'm':
	case 'M':
		/* turn on cts */
		n = LEUS(ccb->proto);
		if(i){
			n |= Cobeycts|Cgenrts;
			n &= ~Cmctl;
		} else {
			n &= ~(Cobeycts|Cgenrts);
			n |= Cmctl;
		}
		ccb->proto = LEUS(n);

		command = Cconfall;
		break;
	case 'n':
	case 'N':
		qnoblock(ac->oq, i);
		break;
	case 'P':
	case 'p':
		switch(*(cmd+1)){
		case 'e':
			n = Cevenpar;
			break;
		case 'o':
			n = Coddpar;
			break;
		default:
			n = Cnopar;
			break;
		}
		n |= LEUS(ccb->format) & ~Cparmask;
		ccb->format = LEUS(n);
		command = Cconfall;
		break;
	case 'K':
	case 'k':
		if(i <= 0)
			i = 250;
		n = LEUS(ccb->mctl) | Cbreakctl;
		ccb->mctl = LEUS(n);
		UNLOCKPAGE(a);

		tsleep(&ac->r, return0, 0, i);

		LOCKPAGE(a, 0);
		n &= ~Cbreakctl;
		ccb->mctl = LEUS(n);
		break;
	case 'R':
	case 'r':
		n = LEUS(ccb->mctl);
		if(i)
			 n |= Crtsctl;
		else
			 n &= ~Crtsctl;
		ccb->mctl = LEUS(n);
		break;
	case 'Q':
	case 'q':
		qsetlimit(ac->iq, i);
		qsetlimit(ac->oq, i);
		break;
	case 'X':
	case 'x':
		n = LEUS(ccb->proto);
		if(i)
			n |= Cobeyxon;
		else
			n &= ~Cobeyxon;
		ccb->proto = LEUS(n);
		command = Cconfall;
		break;
	}
	UNLOCKPAGE(a);

	if(command)
		chancmd(ac, command);
}

long
astarwrite(Chan *c, void *buf, long n, ulong offset)
{
	Astar *a;
	Astarchan *ac;
	char cmsg[32];

	if(c->qid.path & CHDIR)
		error(Eperm);

	a = astar[BOARD(c->qid.path)];
	switch(TYPE(c->qid.path)){
	case Qmem:
		return memwrite(a, buf, n, offset);
	case Qbctl:
		if(n > sizeof cmsg)
			n = sizeof(cmsg) - 1;
		memmove(cmsg, buf, n);
		cmsg[n] = 0;
		bctlwrite(a, cmsg);
		return n;
	case Qdata:
		ac = a->c + CHAN(c->qid.path);
		return qwrite(ac->oq, buf, n);
	case Qctl:
		ac = a->c + CHAN(c->qid.path);
		if(n > sizeof cmsg)
			n = sizeof(cmsg) - 1;
		memmove(cmsg, buf, n);
		cmsg[n] = 0;

		if(waserror()){
			qunlock(ac);
			nexterror();
		}
		qlock(ac);
		astarctl(ac, cmsg);
		qunlock(ac);
		poperror();
	}

	return 0;
}

void
astarcreate(Chan *c, char *name, int omode, ulong perm)
{
	USED(c, name, omode, perm);
	error(Eperm);
}

void
astarremove(Chan *c)
{
	USED(c);
	error(Eperm);
}

void
astarwstat(Chan *c, char *dp)
{
	Dir d;
	Astarchan *ac;

	if(!iseve())
		error(Eperm);
	if(CHDIR & c->qid.path)
		error(Eperm);
	if(TYPE(c->qid.path) != Qdata && TYPE(c->qid.path) != Qctl)
		error(Eperm);
	ac = astar[BOARD(c->qid.path)]->c + CHAN(c->qid.path);

	convM2D(dp, &d);
	d.mode &= 0666;
	ac->perm = d.mode;
}

/*
 *  get output going
 */
static void
astaroutput(Astarchan *ac)
{
	Astar *a = ac->a;
	CCB *ccb = ac->ccb;
	uchar buf[256];
	uchar *rp, *wp, *bp, *ep, *p, *e;
	int n;

	if(a->needpage)
		setpage(a, 0);
	ep = a->addr;
	rp = ep + LEUS(ccb->outrp);
	wp = ep + LEUS(ccb->outwp);
	bp = ep + LEUS(ccb->outbase);
	ep = ep + LEUS(ccb->outlim);
	for(;;){
		n = rp - wp - 1;
		if(n < 0)
			n += ep - bp + 1;
		if(n == 0)
			break;
		if(n > sizeof(buf))
			n = sizeof(buf);
		n = qconsume(ac->oq, buf, n);
		if(n <= 0)
			break;
		if(a->needpage)
			setpage(a, bp - a->addr);
		e = buf + n;
		for(p = buf; p < e;){
			*wp++ = *p++;
			if(wp > ep)
				wp = bp;
		}
		if(a->needpage)
			setpage(a, 0);
		ccb->outwp = LEUS(wp - a->addr);
	}
}
static void
astarkick(Astarchan *ac)
{
	ilock(&ac->a->pagelock);
	astaroutput(ac);
	iunlock(&ac->a->pagelock);
}

/*
 *  process input
 */
static void
astarinput(Astarchan *ac)
{
	Astar *a = ac->a;
	CCB *ccb = ac->ccb;
	uchar buf[256];
	uchar *rp, *wp, *bp, *ep, *p, *e;
	int n;

	if(a->needpage)
		setpage(a, 0);
	ep = a->addr;
	rp = ep + LEUS(ccb->inrp);
	wp = ep + LEUS(ccb->inwp);
	bp = ep + LEUS(ccb->inbase);
	ep = ep + LEUS(ccb->inlim);
	for(;;){
		n = wp - rp;
		if(n == 0)
			break;
		if(n < 0)
			n += ep - bp + 1;
		if(n > sizeof(buf))
			n = sizeof(buf);
		if(a->needpage)
			setpage(a, bp - a->addr);
		e = buf + n;
		for(p = buf; p < e;){
			*p++ = *rp++;
			if(rp > ep)
				rp = bp;
		}
		if(qproduce(ac->iq, buf, n) < 0)
			break;	/* flow controlled */
		if(a->needpage)
			setpage(a, 0);
		ccb->inrp = LEUS(rp - a->addr);
	}
	if(a->needpage)
		setpage(a, 0);
}

/*
 *  get flow controlled input going again
 */
static void
astarkickin(Astarchan *ac)
{
	ilock(&ac->a->pagelock);
	astarinput(ac);
	iunlock(&ac->a->pagelock);
}

/*
 *  handle an interrupt
 */
static void
astarintr(Ureg *ur, void *arg)
{
	Astar *a = arg;
	Astarchan *ac;
	ulong vec, invec, outvec, errvec, mvec, cmdvec;

	USED(ur);
	lock(&a->pagelock);
	if(a->needpage)
		setpage(a, 0);

	/* get causes */
	invec = LEUS(xchgw(&a->gcb->inserv, 0));
	outvec = LEUS(xchgw(&a->gcb->outserv, 0));
	errvec = LEUS(xchgw(&a->gcb->errserv, 0));
	mvec = LEUS(xchgw(&a->gcb->modemserv, 0));
	cmdvec = LEUS(xchgw(&a->gcb->cmdserv, 0));
	USED(mvec);
	USED(cmdvec);
	USED(errvec);

	/* reenable interrupts */
	a->gcb->cmd2 = LEUS(Gintack);

	/* service interrupts */
	ac = a->c;
	for(vec = invec; vec; vec >>= 1){
		if(vec&1)
			astarinput(ac);
		ac++;
	}
	ac = a->c;
	for(vec = outvec; vec; vec >>= 1){
		if(vec&1)
			astaroutput(ac);
		ac++;
	}
	ac = a->c;
	for(vec = cmdvec; vec; vec >>= 1){
		if(vec&1)
			wakeup(&ac->r);
		ac++;
	}
	unlock(&a->pagelock);
}
