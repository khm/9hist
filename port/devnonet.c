#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"errno.h"
#include	"../port/nonet.h"

#define DPRINT if(pnonet)print
#define NOW (MACHP(0)->ticks*MS2HZ)
#define MSUCC(x) (((x)+1)%Nnomsg)

static Noifc *noifc;
int pnonet;

enum {
	/*
	 *  tuning parameters
	 */
	MSrexmit = 250,		/* retranmission interval in ms */

	/*
	 *  relative or immutable
	 */
	Nsubdir = 5,		/* entries in the nonet directory */
	Nmask = Nnomsg - 1,	/* mask for log(Nnomsg) bits */
};

/* predeclared */
static void	nohangup(Noconv*);
static void	noreset(Noconv*);
static Block*	nohdr(Noconv*, int);
static void	nolisten(Chan*, Noifc*);
static void	noannounce(Chan*, char*);
static void	noconnect(Chan*, char*);
static void	norack(Noconv*, int);
static void	nosendctl(Noconv*, int, int);
static void	nosend(Noconv*, Nomsg*);
static void	nostartconv(Noconv*, int, char*, int);
static void	noqack(Noconv*, int);
static void	nokproc(void*);
static void	noiput(Queue*, Block*);
static void	nooput(Queue*, Block*);
static void	noclose(Queue*);
static void	noopen(Queue*, Stream*);

extern Qinfo	noetherinfo;
extern Qinfo	nonetinfo;

/*
 *  nonet directory and subdirectory
 */
enum {
	/*
	 *  per conversation
	 */
	Naddrqid,
	Nlistenqid,
	Nraddrqid,
	Nruserqid,
	Nstatsqid,
	Nchanqid,

	/*
	 *  per noifc
	 */
	Ncloneqid,
};

Dirtab *nonetdir;
Dirtab nosubdir[]={
	"addr",		{Naddrqid},	0,	0600,
	"listen",	{Nlistenqid},	0,	0600,
	"raddr",	{Nraddrqid},	0,	0600,
	"ruser",	{Nruserqid},	0,	0600,
	"stats",	{Nstatsqid},	0,	0600,
};

/*
 *  Nonet conversation states (for Noconv.state)
 */
enum {
	Cclosed=	0,
	Copen=		1,
	Cannounced=	2,
	Cconnected=	3,
	Cconnecting=	4,
	Chungup=	5,
	Creset=		6,
};

/*
 *  nonet kproc
 */
static int kstarted;
static Rendez nonetkr;

/*
 *  nonet file system.  most of the calls use dev.c to access the nonet
 *  directory and stream.c to access the nonet devices.
 *  create the nonet directory.  the files are `clone' and stream
 *  directories '1' to '32' (or whatever conf.noconv is in decimal)
 */
void
nonetreset(void)
{
	int i;

	/*
	 *  allocate the interfaces
	 */
	noifc = (Noifc *)ialloc(sizeof(Noifc) * conf.nnoifc, 0);
	for(i = 0; i < conf.nnoifc; i++)
		noifc[i].conv = (Noconv *)ialloc(sizeof(Noconv) * conf.nnoconv, 0);

	/*
	 *  create the directory.
	 */
	nonetdir = (Dirtab *)ialloc(sizeof(Dirtab) * (conf.nnoconv+1), 0);

	/*
	 *  the circuits
	 */
	for(i = 0; i < conf.nnoconv; i++) {
		sprint(nonetdir[i].name, "%d", i);
		nonetdir[i].qid.path = CHDIR|STREAMQID(i, Nchanqid);
		nonetdir[i].qid.vers = 0;
		nonetdir[i].length = 0;
		nonetdir[i].perm = 0600;
	}

	/*
	 *  the clone device
	 */
	strcpy(nonetdir[i].name, "clone");
	nonetdir[i].qid.path = Ncloneqid;
	nonetdir[i].qid.vers = 0;
	nonetdir[i].length = 0;
	nonetdir[i].perm = 0600;
}

void
nonetinit(void)
{
}

/*
 *  find an noifc and increment its count
 */
Chan*
nonetattach(char *spec)
{
	Noifc *ifc;
	Chan *c;

	/*
	 *  find an noifc with the same name
	 */
	if(*spec == 0)
		spec = "nonet";
	for(ifc = noifc; ifc < &noifc[conf.nnoifc]; ifc++){
		lock(ifc);
		if(strcmp(spec, ifc->name)==0 && ifc->wq) {
			ifc->ref++;
			unlock(ifc);
			break;
		}
		unlock(ifc);
	}
	if(ifc == &noifc[conf.nnoifc])
		error(Enoifc);
	c = devattach('n', spec);
	c->dev = ifc - noifc;

	return c;
}

/*
 *  up the reference count to the noifc
 */
Chan*
nonetclone(Chan *c, Chan *nc)
{
	Noifc *ifc;

	c = devclone(c, nc);
	ifc = &noifc[c->dev];
	lock(ifc);
	ifc->ref++;
	unlock(ifc);
	return c;
}

int	 
nonetwalk(Chan *c, char *name)
{
	if(c->qid.path == CHDIR)
		return devwalk(c, name, nonetdir, conf.nnoconv+1, devgen);
	else
		return devwalk(c, name, nosubdir, Nsubdir, streamgen);
}

void	 
nonetstat(Chan *c, char *dp)
{
	if(c->qid.path == CHDIR)
		devstat(c, dp, nonetdir, conf.nnoconv+1, devgen);
	else if(c->qid.path == Ncloneqid)
		devstat(c, dp, &nonetdir[conf.nnoconv], 1, devgen);
	else
		devstat(c, dp, nosubdir, Nsubdir, streamgen);
}

/*
 *  opening a nonet device allocates a Noconv.  Opening the `clone'
 *  device is a ``macro'' for finding a free Noconv and opening
 *  it's ctl file.
 */
Noconv *
nonetopenclone(Chan *c, Noifc *ifc)
{
	Noconv *cp;
	Noconv *ep;

	ep = &ifc->conv[conf.nnoconv];
	for(cp = &ifc->conv[0]; cp < ep; cp++){
		if(cp->state == Cclosed && canqlock(cp)){
			if(cp->state != Cclosed){
				qunlock(cp);
				continue;
			}
			c->qid.path = CHDIR|STREAMQID(cp-ifc->conv, Nchanqid);
			devwalk(c, "ctl", 0, 0, streamgen);
			streamopen(c, &nonetinfo);
			qunlock(cp);
			break;
		}
	}
	if(cp == ep)
		error(Enodev);;
	return cp;
}

Chan*
nonetopen(Chan *c, int omode)
{
	Stream *s;
	Noifc *ifc;
	int line;

	if(!kstarted){
		kproc("nonetack", nokproc, 0);
		kstarted = 1;
	}

	if(c->qid.path & CHDIR){
		/*
		 *  directories are read only
		 */
		if(omode != OREAD)
			error(Ebadarg);
	} else switch(STREAMTYPE(c->qid.path)){
	case Ncloneqid:
		/*
		 *  get an unused device and open it's control file
		 */
		ifc = &noifc[c->dev];
		nonetopenclone(c, ifc);
		break;
	case Nlistenqid:
		/*
		 *  listen for a call and open the control file for the
		 *  channel on which the call arrived.
		 */
		line = STREAMID(c->qid.path);
		ifc = &noifc[c->dev];
		if(ifc->conv[line].state != Cannounced)
			errors("channel not announced");
		nolisten(c, ifc);
		break;
	case Nraddrqid:
		/*
		 *  read only files
		 */
		if(omode != OREAD)
			error(Ebadarg);
		break;
	default:
		/*
		 *  open whatever c points to
		 */
		streamopen(c, &nonetinfo);
		break;
	}

	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void	 
nonetcreate(Chan *c, char *name, int omode, ulong perm)
{
	error(Eperm);
}

void	 
nonetclose(Chan *c)
{
	Noifc *ifc;

	/*
	 *  real closing happens in noclose
	 */
	if(c->qid.path != CHDIR)
		streamclose(c);

	/*
	 *  let go of the multiplexor
	 */
	nonetfreeifc(&noifc[c->dev]);
}

long	 
nonetread(Chan *c, void *a, long n, ulong offset)
{
	int t;
	Noconv *cp;
	char stats[256];

	/*
	 *  pass data/ctl reads onto the stream code
	 */
	t = STREAMTYPE(c->qid.path);
	if(t >= Slowqid)
		return streamread(c, a, n);

	if(c->qid.path == CHDIR)
		return devdirread(c, a, n, nonetdir, conf.nnoconv+1, devgen);
	if(c->qid.path & CHDIR)
		return devdirread(c, a, n, nosubdir, Nsubdir, streamgen);

	cp = &noifc[c->dev].conv[STREAMID(c->qid.path)];
	switch(t){
	case Nraddrqid:
		return stringread(c, a, n, cp->raddr, offset);
	case Naddrqid:
		return stringread(c, a, n, cp->addr, offset);
	case Nruserqid:
		return stringread(c, a, n, cp->ruser, offset);
	case Nstatsqid:
		sprint(stats, "sent: %d\nrcved: %d\nrexmit: %d\nbad: %d\n",
			cp->sent, cp->rcvd, cp->rexmit, cp->bad);
		return stringread(c, a, n, stats, offset);
	}
	error(Eperm);
}

long	 
nonetwrite(Chan *c, void *a, long n, ulong offset)
{
	int t;
	int m;
	char buf[256];
	char *field[5];

	t = STREAMTYPE(c->qid.path);

	/*
	 *  get data dispatched as quickly as possible
	 */
	if(t == Sdataqid)
		return streamwrite(c, a, n, 0);

	/*
	 *  easier to do here than in nooput
	 */
	if(t == Sctlqid){
		strncpy(buf, a, sizeof buf);
		m = getfields(buf, field, 5, ' ');
		if(strcmp(field[0], "connect")==0){
			if(m < 2)
				error(Ebadarg);
			noconnect(c, field[1]);
		} else if(strcmp(field[0], "announce")==0){
			noannounce(c, field[1]);
		} else if(strcmp(field[0], "accept")==0){
			/* ignore */;
		} else if(strcmp(field[0], "reject")==0){
			/* ignore */;
		} else
			return streamwrite(c, a, n, 0);
		return n;
	}
	
	error(Eperm);
}

void	 
nonetremove(Chan *c)
{
	error(Eperm);
}

void	 
nonetwstat(Chan *c, char *dp)
{
	error(Eperm);
}

/*
 *  the device stream module definition
 */
Qinfo nonetinfo =
{
	noiput,
	nooput,
	noopen,
	noclose,
	"nonet"
};

/*
 *  store the device end of the stream so that the multiplexor can
 *  send blocks upstream.
 *
 *	State Transition:	Cclosed -> Copen
 */
static void
noopen(Queue *q, Stream *s)
{
	Noifc *ifc;
	Noconv *cp;

	ifc = &noifc[s->dev];
	cp = &ifc->conv[s->id];
	cp->s = s;
	cp->ifc = ifc;
	cp->rq = RD(q);
	cp->state = Copen;
	RD(q)->ptr = WR(q)->ptr = (void *)cp;
}


static int
ishungup(void *a)
{
	Noconv *cp;

	cp = (Noconv *)a;
	switch(cp->state){
	case Chungup:
	case Creset:
	case Cclosed:
		return 1;
	}
	return 0;
}
/*
 *  wait until a hangup is received.
 *  then send a hangup message (until one is received).
 *
 *	State Transitions:	* -> Cclosed
 */
static void
noclose(Queue *q)
{
	Noconv *cp;
	Nomsg *mp;
	int i;

	cp = (Noconv *)q->ptr;

	if(waserror()){
		cp->rcvcircuit = -1;
		cp->state = Cclosed;
		nexterror();
	}

	/*
	 *  send hangup messages to the other side
	 *  until it hangs up or we get tired.
	 */
	switch(cp->state){
	case Cconnected:
		/*
		 *  send close till we get one back
		 */
		nosendctl(cp, NO_HANGUP, 1);
		for(i=0; i<10 && !ishungup(cp); i++){
			nosendctl(cp, NO_HANGUP, 0);
			tsleep(&cp->r, ishungup, cp, MSrexmit);
		}
		break;
	case Chungup:
		/*
		 *  ack any close
		 */
		nosendctl(cp, NO_HANGUP, 1);
		break;
	}

	qlock(cp);
	/*
	 *  we give up, ack any unacked messages
	 */
	for(i = cp->first; i != cp->next; i = MSUCC(i))
		norack(cp, cp->out[i].mid);
	cp->rcvcircuit = -1;
	cp->state = Cclosed;
	if(cp->media){
		freeb(cp->media);
		cp->media = 0;
	}
	qunlock(cp);

	poperror();
}

/*
 *  send all messages up stream.  this should only be control messages
 *
 *	State Transition:	(on M_HANGUP) * -> Chungup
 */
static void
noiput(Queue *q, Block *bp)
{
	Noconv *cp;

	if(bp->type == M_HANGUP){
		cp = (Noconv *)q->ptr;
		cp->state = Chungup;
	}
	PUTNEXT(q, bp);
}

/*
 *  queue a block
 */
enum {
	Window=	1,
};

static int
windowopen(void *a)
{
	Noconv *cp;
	int i;

	cp = (Noconv *)a;
	i = cp->next - cp->first;
	if(i>=0 && i<Window)	
		return 1;
	if(i<0 && Nnomsg+i<Window)
		return 1;
	return 0;	
}
static void
nooput(Queue *q, Block *bp)
{
	Noconv *cp;
	int next;
	Nomsg *mp;
	Block *first, *last;
	int len;

	cp = (Noconv *)(q->ptr);

	/*
	 *  do all control functions
	 */
	if(bp->type != M_DATA){
		freeb(bp);
		error(Ebadctl);
	}

	/*
	 *  collect till we see a delim
	 */
	qlock(&cp->mlock);
	if(!putb(q, bp)){
		qunlock(&cp->mlock);
		return;
	}

	/*
	 *  take the collected message out of the queue
	 */
	first = q->first;
	last = q->last;
	len = q->len;
	q->len = q->nb = 0;
	q->first = q->last = 0;
	if(len == 0){
		freeb(first);
		qunlock(&cp->mlock);
		return;
	}

	/*
	 *  block till we get an output buffer
	 */
	if(waserror()){
		freeb(first);
		qunlock(&cp->mlock);
		nexterror();
	}
	while(!windowopen(cp))
		sleep(&cp->r, windowopen, cp);
	mp = &cp->out[cp->next];
	cp->next = MSUCC(cp->next);
	qlock(cp);
	qunlock(&cp->mlock);
	poperror();

	/*
	 *  point the output buffer to the message
	 */
	mp->time = NOW + MSrexmit;
	mp->first = first;
	mp->last = last;
	mp->len = len;
	mp->mid ^= Nnomsg;
	mp->acked = 0;

	cp->sent++;

	/*
	 *  send the message, the kproc will retry
	 */
	if(!waserror()){
		nosend(cp, mp);
		poperror();
	}
	qunlock(cp);
}

/*
 *  start a new conversation.  start an ack/retransmit process if
 *  none already exists for this circuit.
 */
void
nostartconv(Noconv *cp, int circuit, char *raddr, int state)
{
	int i;
	char name[32];
	Noifc *ifc;

	ifc = cp->ifc;

	/*
	 *  allocate the prototype header
	 */
	cp->media = allocb(ifc->hsize + NO_HDRSIZE);
	cp->media->wptr += ifc->hsize + NO_HDRSIZE;
	cp->hdr = (Nohdr *)(cp->media->rptr + ifc->hsize);

	/*
	 *  fill in the circuit number
	 */
	cp->hdr->flag = NO_NEWCALL|NO_ACKME;
	cp->hdr->circuit[2] = circuit>>16;
	cp->hdr->circuit[1] = circuit>>8;
	cp->hdr->circuit[0] = circuit;

	/*
	 *  set the state variables
	 */
	cp->state = state;
	for(i = 1; i < Nnomsg; i++){
		cp->in[i].mid = i;
		cp->in[i].acked = 0;
		cp->in[i].rem = 0;
		cp->out[i].mid = i | Nnomsg;
		cp->out[i].acked = 1;
		cp->out[i].rem = 0;
	}
	cp->in[0].mid = Nnomsg;
	cp->in[0].acked = 0;
	cp->in[0].rem = 0;
	cp->out[0].mid = 0;
	cp->out[0].acked = 1;
	cp->out[0].rem = 0;
	cp->ack = 0;
	cp->sendack = 0;
	cp->first = cp->next = 1;
	cp->rexmit = cp->bad = cp->sent = cp->rcvd = 0;
	cp->lastacked = Nnomsg|(Nnomsg-1);

	/*
	 *  used for demultiplexing
	 */
	cp->rcvcircuit = circuit ^ 1;

	/*
	 *  media dependent header
	 */
	(*ifc->connect)(cp, raddr);

	/*
	 *  status files
	 */
	strncpy(cp->raddr, raddr, sizeof(cp->raddr));
	strcpy(cp->ruser, "none");
}

/*
 *  announce willingness to take calls
 *
 *	State Transition:	Copen -> Chungup
 */
static void
noannounce(Chan *c, char *addr)
{
	Noconv *cp;

	cp = (Noconv *)(c->stream->devq->ptr);
	if(cp->state != Copen)
		error(Einuse);
	cp->state = Cannounced;
}

/*
 *  connect to the destination whose name is pointed to by bp->rptr.
 *
 *  a service is separated from the destination system by a '!'
 *
 *	State Transition:	Copen -> Cconnecting
 */
static void
noconnect(Chan *c, char *addr)
{
	Noifc *ifc;
	Noconv *cp;
	char *service;
	char buf[2*NAMELEN+2];

	cp = (Noconv *)(c->stream->devq->ptr);
	if(cp->state != Copen)
		error(Einuse);
	ifc = cp->ifc;
	service = strchr(addr, '!');
	if(service){
		*service++ = 0;
		if(strchr(service, ' '))
			error(Ebadctl);
		if(strlen(service) > NAMELEN)
			error(Ebadctl);
	}

	nostartconv(cp, 2*(cp - ifc->conv), addr, Cconnecting);

	if(service){
		/*
		 *  send service name and user name
		 */
		cp->hdr->flag |= NO_SERVICE;
		sprint(buf, "%s %s", service, u->p->pgrp->user);
		c->qid.path = STREAMQID(STREAMID(c->qid.path), Sdataqid);
		streamwrite(c, buf, strlen(buf), 1);
		c->qid.path = STREAMQID(STREAMID(c->qid.path), Sctlqid);
	}
}

/*
 *  listen for a call.  There can be many listeners, but only one can sleep
 *  on the circular queue at a time.  ifc->listenl lets only one at a time into
 *  the sleep.
 *
 *	State Transition:	Cclosed -> Copen -> Cconnecting
 */
static int
iscall(void *a)
{
	Noifc *ifc;

	ifc = (Noifc *)a;
	return ifc->rptr != ifc->wptr;
}
static void
nolisten(Chan *c, Noifc *ifc)
{
	Noconv *cp, *ep;
	Nocall call;
	int f;
	char buf[2*NAMELEN+4];
	char *user;
	long n;

	call.msg = 0;

	if(waserror()){
		if(call.msg)
			freeb(call.msg);
		qunlock(&ifc->listenl);
		nexterror();
	}

	for(;;){
		/*
		 *  one listener at a time
		 */
		qlock(&ifc->listenl);

		/*
		 *  wait for a call
		 */
		sleep(&ifc->listenr, iscall, ifc);
		call = ifc->call[ifc->rptr];
		ifc->rptr = (ifc->rptr+1) % Nnocalls;
		qunlock(&ifc->listenl);

		/*
		 *  make sure we aren't already using this circuit
		 */
		ep = &ifc->conv[conf.nnoconv];
		for(cp = &ifc->conv[0]; cp < ep; cp++){
			if(cp->state>Cannounced && (call.circuit^1)==cp->rcvcircuit
			&& strcmp(call.raddr, cp->raddr)==0)
				break;
		}
		if(cp != ep){
			freeb(call.msg);
			call.msg = 0;
			continue;
		}

		/*
		 *  get a free channel
		 */
		cp = nonetopenclone(c, ifc);
		c->qid.path = STREAMQID(cp - ifc->conv, Sctlqid);

		/*
		 *  start the protocol and
		 *  stuff the connect message into it
		 */
		f = ((Nohdr *)(call.msg->rptr))->flag;
		DPRINT("call from %d %s\n", call.circuit, call.raddr);
		nostartconv(cp, call.circuit, call.raddr, Cconnecting);
		DPRINT("rcving %d byte message\n", call.msg->wptr - call.msg->rptr);
		nonetrcvmsg(cp, call.msg);
		call.msg = 0;

		/*
		 *  if a service and remote user were specified,
		 *  grab them
		 */
		if(f & NO_SERVICE){
			DPRINT("reading service\n");
			c->qid.path = STREAMQID(cp - ifc->conv, Sdataqid);
			n = streamread(c, buf, sizeof(buf));
			c->qid.path = STREAMQID(cp - ifc->conv, Sctlqid);
			if(n <= 0)
				error(Ebadctl);
			buf[n] = 0;
			user = strchr(buf, ' ');
			if(user){
				*user++ = 0;
				strncpy(cp->ruser, user, NAMELEN);
			} else
				strcpy(cp->ruser, "none");
			strncpy(cp->addr, buf, NAMELEN);
		}
		break;
	}
	poperror();
}

/*
 *  send a hangup signal up the stream to get all line disciplines
 *  to cease and desist
 *
 *	State Transition:	{Cconnected, Cconnecting} -> Chungup
 */
static void
nohangup(Noconv *cp)
{
	Block *bp;
	Queue *q;

	switch(cp->state){
	case Cconnected:
	case Cconnecting:
		cp->state = Chungup;
		bp = allocb(0);
		bp->type = M_HANGUP;
		q = cp->rq;
		PUTNEXT(q, bp);
		break;
	}
	wakeup(&cp->r);
}

/*
 *  Send a hangup signal up the stream to get all line disciplines
 *  to cease and desist.  Disassociate this conversation from a circuit
 *  number.  Any subsequent close of the conversation will
 *  not send hangup messages.
 *
 *	State Transition:	{Cconnected, Cconnecting, Chungup} -> Creset
 */
static void
noreset(Noconv *cp)
{
	Block *bp;
	Queue *q;

	switch(cp->state){
	case Cconnected:
	case Cconnecting:
	case Chungup:
		cp->state = Creset;
		cp->rcvcircuit = -1;
 		bp = allocb(0);
		bp->type = M_HANGUP;
		q = cp->rq;
		PUTNEXT(q, bp);
		break;
	}
	wakeup(&cp->r);
}

/*
 *  process a message acknowledgement.  if the message
 *  has any xmit buffers queued, free them.
 */
static void
norack(Noconv *cp, int mid)
{
	Nomsg *mp;
	Block *bp;
	int i;

	mp = &cp->out[mid & Nmask];

	/*
	 *  if already acked, ignore
	 */
	if(mp->acked || mp->mid != mid)
		return;

	/*
 	 *  free it
	 */
	cp->rexmit = 0;
	cp->lastacked = mid;
	mp->acked = 1;
	if(mp->first)
		freeb(mp->first);
	mp->first = 0;

	/*
	 *  advance first if this is the first
	 */
	if((mid&Nmask) == cp->first){
		while(cp->first != cp->next){
			if(cp->out[cp->first].acked == 0)
				break;
			cp->first = MSUCC(cp->first);
		}
		wakeup(&cp->r);
	}
}

/*
 *  queue an acknowledgement to be sent.  ignore it if we already have Nnomsg
 *  acknowledgements queued.
 */
static void
noqack(Noconv *cp, int mid)
{
	cp->ack = mid;
	cp->sendack = 1;
}

/*
 *  make a packet header
 */
Block *
nohdr(Noconv *cp, int rem)
{
	Block *bp;
	Nohdr *hp;

	bp = allocb(cp->ifc->hsize + NO_HDRSIZE + cp->ifc->mintu);
	memmove(bp->wptr, cp->media->rptr, cp->ifc->hsize + NO_HDRSIZE);
	bp->wptr += cp->ifc->hsize + NO_HDRSIZE;
	hp = (Nohdr *)(bp->rptr + cp->ifc->hsize);
	hp->remain[1] = rem>>8;
	hp->remain[0] = rem;
	hp->sum[0] = hp->sum[1] = 0;

	return bp;
}

/*
 *  transmit a message.  this involves breaking a possibly multi-block message into
 *  a train of packets on the media.
 *
 *  called by nooput().  the qlock(mp) synchronizes these two
 *  processes.
 */
static void
nosend(Noconv *cp, Nomsg *mp)
{
	Noifc *ifc;
	Queue *wq;
	int msgrem;
	int pktrem;
	int n;
	Block *bp, *pkt, *last;
	uchar *rptr;

	ifc = cp->ifc;
	if(ifc == 0)
		return;
	wq = ifc->wq->next;

	/*
	 *  get the next acknowledge to use if the next queue up
	 *  is not full.
	 */
	cp->sendack = 0;
	cp->hdr->ack = cp->ack;
	cp->hdr->mid = mp->mid;

	/*
	 *  package n blocks into m packets.  make sure
	 *  no packet is < mintu or > maxtu in length.
	 */
	if(ifc->mintu > mp->len) {
		/*
		 *  short message:
		 *  copy the whole message into the header block
		 */
		last = pkt = nohdr(cp, mp->len);
		for(bp = mp->first; bp; bp = bp->next){
			memmove(pkt->wptr, bp->rptr, n = BLEN(bp));
			pkt->wptr += n;
		}
		/*
		 *  round up to mintu
		 */
		memset(pkt->wptr, 0, n = ifc->mintu-mp->len);
		pkt->wptr += n;
	} else {
		/*
		 *  long message:
		 *  break up the message into noifc packets and send them.
		 *  once around this loop for each non-header block generated.
		 */
		msgrem = mp->len;
		pktrem = msgrem > ifc->maxtu ? ifc->maxtu : msgrem;
		bp = mp->first;
		SET(rptr);
		if(bp)
			rptr = bp->rptr;
		last = pkt = nohdr(cp, msgrem);
		n = 0;
		while(bp){
			/*
			 *  if pkt full, send and create new header block
			 */
			if(pktrem == 0){
				nonetcksum(pkt, ifc->hsize);
				last->flags |= S_DELIM;
				(*wq->put)(wq, pkt);
				last = pkt = nohdr(cp, -msgrem);
				pktrem = msgrem > ifc->maxtu ? ifc->maxtu : msgrem;
			}
			n = bp->wptr - rptr;
			if(n > pktrem)
				n = pktrem;
			last = last->next = allocb(0);
			last->rptr = rptr;
			last->wptr = rptr = rptr + n;
			msgrem -= n;
			pktrem -= n;
			if(rptr >= bp->wptr){
				bp = bp->next;
				if(bp)
					rptr = bp->rptr;
			}
		}
		/*
		 *  round up last packet to mintu
		 */
		if(n < ifc->mintu){
			n = ifc->mintu - n;
			last = last->next = allocb(n);
			memset(last->wptr, 0, n);
			last->wptr += n;
		}
	}
	nonetcksum(pkt, ifc->hsize);
	last->flags |= S_DELIM;
	if(cp->rexmit > 10)
		mp->time = NOW + 10*MSrexmit;
	else
		mp->time = NOW + (cp->rexmit+1)*MSrexmit;

	(*wq->put)(wq, pkt);
}

/*
 *  send a control message (hangup or acknowledgement).
 */
static void
nosendctl(Noconv *cp, int flag, int new)
{
	Nomsg ctl;

	ctl.len = 0;
	ctl.first = 0;
	ctl.acked = 0;
	if(new)
		ctl.mid = Nnomsg^cp->out[cp->next].mid;
	else
		ctl.mid = cp->lastacked;
	cp->hdr->flag |= flag;
	nosend(cp, &ctl);
}

/*
 *  receive a message (called by the multiplexor; noetheriput, nofddiiput, ...)
 *
 *	State Transition:	(no NO_NEWCALL in msg) Cconnecting -> Cconnected
 */
void
nonetrcvmsg(Noconv *cp, Block *bp)
{
	Block *nbp;
	Nohdr *h;
	short r;
	int c;
	Nomsg *mp;
	int f;
	Queue *q;

	q = cp->rq;

	/*
	 *  grab the packet header, push the pointer past the nonet header
	 */
	h = (Nohdr *)bp->rptr;
	bp->rptr += NO_HDRSIZE;
	mp = &cp->in[h->mid & Nmask];
	r = (h->remain[1]<<8) | h->remain[0];
	f = h->flag;

	/*
	 *  Obey reset even if the message id is bogus
	 */
	if(f & NO_RESET){
		DPRINT("reset received\n");
		noreset(cp);
		freeb(bp);
		return;
	}

	/*
	 *  A new call request (maybe), treat it as a reset if seen on a
	 *  connected, hungup, or reset channel.
	 *
 	 *  On a connecting channel, treat as a reset if this is an
	 *  invalid message ID.
	 */
	if(f & NO_NEWCALL){
		switch(cp->state){
		case Cclosed:
		case Copen:
		case Cannounced:
		case Creset:
			DPRINT("nonetrcvmsg %d %d\n", cp->rcvcircuit, cp - cp->ifc->conv);
			freeb(bp);
			return;
		case Chungup:
		case Cconnected:
			DPRINT("Nonet call on connected/hanging-up circ %d conv %d\n",
				cp->rcvcircuit, cp - cp->ifc->conv); 
			freeb(bp);
			noreset(cp);
			return;
		case Cconnecting:
			if(h->mid != mp->mid){
				DPRINT("Nonet call on connecting circ %d conv %d\n",
					cp->rcvcircuit, cp - cp->ifc->conv); 
				freeb(bp);
				noreset(cp);
				return;
			}
			break;
		}
	}

	/*
	 *  ignore old messages and process the acknowledgement
	 */
	if(h->mid!=mp->mid || (f&NO_NULL)){
		DPRINT("old msg %d instead of %d r==%d\n", h->mid, mp->mid, r);
		if(r == 0){
			norack(cp, h->ack);
			if(f & NO_HANGUP)
				nohangup(cp);
		} else {
			if(r>0){
				norack(cp, h->ack);
				noqack(cp, h->mid);
			}
			cp->bad++;
		}
		freeb(bp);
		return;
	}

	if(r>=0){
		/*
		 *  start of message packet
		 */
		if(mp->first){
			DPRINT("mp->mid==%d mp->rem==%d r==%d\n", mp->mid, mp->rem, r);
			freeb(mp->first);
			mp->first = mp->last = 0;
			mp->len = 0;
		}
		mp->rem = r;
	} else {
		/*
		 *  a continuation
		 */
		if(-r != mp->rem) {
			DPRINT("mp->mid==%d mp->rem==%d r==%d\n", mp->mid, mp->rem, r);
			cp->bad++;
			freeb(bp);
			return;
		}
	}

	/*
	 *  take care of packets that were padded up
	 */
	mp->rem -= BLEN(bp);
	if(mp->rem < 0){
		if(-mp->rem <= BLEN(bp)){
			bp->wptr += mp->rem;
			mp->rem = 0;
		} else
			panic("nonetrcvmsg: short packet");
	}
	putb(mp, bp);

	/*
	 *  if the last chunk - pass it up the stream and wake any
	 *  waiting process.
	 *
	 *  if not, strip off the delimiter.
	 */
	if(mp->rem == 0){
		cp->hdr->flag &= ~(NO_NEWCALL|NO_SERVICE);
		norack(cp, h->ack);
		if(f & NO_ACKME)
			noqack(cp, h->mid);
		mp->last->flags |= S_DELIM;
		PUTNEXT(q, mp->first);
		mp->first = mp->last = 0;
		mp->len = 0;
		cp->rcvd++;

		/*
		 *  cycle bufffer to next expected mid
		 */
		mp->mid ^= Nnomsg;

		/*
		 *  any NO_NEWCALL after this is another call
		 */
		if(cp->state==Cconnecting && !(f&NO_NEWCALL))
			cp->state = Cconnected;

		/*
		 *  hangup (after processing message)
		 */
		if(f & NO_HANGUP){
			DPRINT("hangup with message\n");
			nohangup(cp);
		}
	} else
		mp->last->flags &= ~S_DELIM;

}


/*
 *  noifc
 */
/*
 *  Create an ifc.
 */
Noifc *
nonetnewifc(Queue *q, Stream *s, int maxtu, int mintu, int hsize,
	void (*connect)(Noconv *, char *))
{
	Noifc *ifc;
	int i;

	for(ifc = noifc; ifc < &noifc[conf.nnoifc]; ifc++){
		if(ifc->ref == 0){
			lock(ifc);
			if(ifc->ref) {
				/* someone was faster than us */
				unlock(ifc);
				continue;
			}
			RD(q)->ptr = WR(q)->ptr = (void *)ifc;
			for(i = 0; i < conf.nnoconv; i++)
				ifc->conv[i].rcvcircuit = -1;
			ifc->maxtu = maxtu - hsize - NO_HDRSIZE;
			ifc->mintu = mintu - hsize - NO_HDRSIZE;
			ifc->hsize = hsize;
			ifc->connect = connect;
			ifc->name[0] = 0;
			ifc->wq = WR(q);
			ifc->ref = 1;
			unlock(ifc);
			return ifc;
		}
	}
	error(Enoifc);
}

/*
 *  Free an noifc.
 */
void
nonetfreeifc(Noifc *ifc)
{
	lock(ifc);
	ifc->ref--;
	if(ifc->ref == 0)
		ifc->wq = 0;
	unlock(ifc);
}

/*
 *  calculate the checksum of a list of blocks.  ignore the first `offset' bytes.
 */
int
nonetcksum(Block *bp, int offset)
{
	uchar *ep, *p;
	int n;
	ulong s;
	Nohdr *hp;
	Block *first;

	s = 0;
	p = bp->rptr + offset;
	n = bp->wptr - p;
	hp = (Nohdr *)p;
	hp->sum[0] = hp->sum[1] = 0;
	for(;;){
		ep = p+(n&~0x7);
		while(p < ep) {
			s = s + s + p[0];
			s = s + s + p[1];
			s = s + s + p[2];
			s = s + s + p[3];
			s = s + s + p[4];
			s = s + s + p[5];
			s = s + s + p[6];
			s = s + s + p[7];
			s = (s&0xffff) + (s>>16);
			p += 8;
		}
		ep = p+(n&0x7);
		while(p < ep) {
			s = s + s + *p;
			p++;
		}
		s = (s&0xffff) + (s>>16);
		bp = bp->next;
		if(bp == 0)
			break;
		p = bp->rptr;
		n = BLEN(bp);
	}
	s = (s&0xffff) + (s>>16);
	hp->sum[1] = s>>8;
	hp->sum[0] = s;
	s &= 0xffff;
if(0)
	switch(s){
	case 0xac9f:
	case 0xc1a4:
	case 0xc41c:
	case 0xc46d:
		{ int i;
		print("%lux s,", s);
		for(bp = first; bp; bp = bp->next)
			for(i = 0; i < BLEN(bp); i++)
				print(" %ux", bp->rptr[i]);
		}
	}
	return s;
}

/*
 *  send acknowledges that need to be sent.  this happens at 1/2
 *  the retransmission interval.
 */
static void
nokproc(void *arg)
{
	Noifc *ifc;
	Noconv *cp, *ep;
	Nomsg *mp;

	cp = 0;
	ifc = 0;
	if(waserror()){
		if(ifc)
			unlock(ifc);
		if(cp)
			qunlock(cp);
	}

	for(;;){
		/*
		 *  loop through all active interfaces
		 */
		for(ifc = noifc; ifc < &noifc[conf.nnoifc]; ifc++){
			if(ifc->wq==0 || !canlock(ifc))
				continue;
	
			/*
			 *  loop through all active conversations
			 */
			ep = ifc->conv + conf.nnoconv;
			for(cp = ifc->conv; cp < ep; cp++){
				if(cp->state<=Copen || !canqlock(cp))
					continue;
				if(cp->state <= Copen){
					qunlock(cp);
					continue;
				}
	
				/*
				 *  resend the first message
				 */
				if(cp->first!=cp->next && NOW>=cp->out[cp->first].time){
					mp = &(cp->out[cp->first]);
					if(cp->rexmit++ > 15){
						norack(cp, mp->mid);
						noreset(cp);
					} else
						nosend(cp, mp);
				}
	
				/*
				 *  get the acknowledges out
				 */
				if(cp->sendack){
					DPRINT("sending ack %d\n", cp->ack);
					nosendctl(cp, /*NO_NULL*/0, 0);
				}
				qunlock(cp);
			}
			unlock(ifc);
		}
		tsleep(&nonetkr, return0, 0, MSrexmit/2);
	}
}

void
nonettoggle(void)
{
	pnonet ^= 1;
}

