#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"errno.h"
#include	"devtab.h"
#include	"fcall.h"

typedef struct Mntrpc Mntrpc;
typedef struct Mnt Mnt;

struct Mntrpc
{
	Mntrpc	*list;		/* Free/pending list */
	Fcall	request;	/* Outgoing file system protocol message */
	Fcall	reply;		/* Incoming reply */
	Mnt	*m;		/* Mount device during rpc */
	Rendez	r;		/* Place to hang out */
	char	*rpc;		/* I/O Data buffer */
	char	done;		/* Rpc completed */
	char	bfree;		/* Buffer may be freed after flush */
	char	flushed;	/* Flush was sent */
	ushort	flushtag;	/* Tag to send flush on */
	ushort	flushbase;	/* Base tag of flush window for this buffer */
	char	flush[MAXMSG];	/* Somewhere to build flush */
};

struct Mnt
{
	Ref;			/* Count of attached channels */
	Chan	*c;		/* Channel to file service */
	Proc	*rip;		/* Reader in progress */
	Mntrpc	*queue;		/* Queue of pending requests on this channel */
	int	id;		/* Multiplexor id for channel check */
	Mnt	*list;		/* Free list */
	char	mux;		/* Set if the device aleady does the multiplexing */
};

struct Mntalloc
{
	Lock;
	Mnt	*mntfree;
	Mnt	*mntarena;
	Mntrpc	*rpcfree;
	int	id;
}mntalloc;

#define BITBOTCH	256
#define MAXRPC		(MAXFDATA+MAXMSG+BITBOTCH)
#define limit(n, max)	(n > max ? max : n)

Chan 	*mattach(Mnt*, char*, char*);
Mntrpc	*mntralloc(void);
void	mntfree(Mntrpc*);
int	rpcattn(Mntrpc*);
void	mountrpc(Mnt*, Mntrpc*);
void	mountio(Mnt*, Mntrpc*);
Mnt	*mntchk(Chan*);
void	mountmux(Mnt*, Mntrpc*);
long	mntrdwr(int , Chan*, void*,long , ulong);
int	mntflush(Mnt*, Mntrpc*);
void	mntqrm(Mnt*, Mntrpc*);
void	mntdirfix(uchar*, Chan*);
void	mntgate(Mnt*);
void	mntrpcread(Mnt*, Mntrpc*);

enum
{
	Tagspace = 1,
	Flushspace = 64,
	Flushtag = 512,
};

void
mntreset(void)
{
	Mnt *me, *md;
	Mntrpc *re, *rd;
	ushort tag, ftag;

	mntalloc.mntarena = ialloc(conf.nmntdev*sizeof(Mnt), 0);
	mntalloc.mntfree = mntalloc.mntarena;
	me = &mntalloc.mntfree[conf.nmntdev];
	for(md = mntalloc.mntfree; md < me; md++)
		md->list = md+1;
	me[-1].list = 0;

	if(conf.nmntbuf > Flushtag) {
		print("devmnt: buffers limited to %d\n", Flushtag);
		conf.nmntbuf = Flushtag;
	}

	tag = Tagspace;
	ftag = Flushtag;
	mntalloc.rpcfree = ialloc(conf.nmntbuf*sizeof(Mntrpc), 0);
	re = &mntalloc.rpcfree[conf.nmntbuf];
	for(rd = mntalloc.rpcfree; rd < re; rd++) {
		rd->list = rd+1;
		rd->request.tag = tag++;
		rd->flushbase = ftag;
		rd->flushtag = ftag;
		ftag += Flushspace;
		rd->rpc = ialloc(MAXRPC, 0);
	}
	re[-1].list = 0;

	mntalloc.id = 1;
}

void
mntinit(void)
{
}

Chan*
mntattach(char *muxattach)
{
	Mnt *m, *e;
	struct bogus{
		Chan	*chan;
		char	*spec;
		char	*auth;
	}bogus;

	bogus = *((struct bogus *)muxattach);
	e = &mntalloc.mntarena[conf.nmntdev];
	for(m = mntalloc.mntarena; m < e; m++) {
		if(m->c == bogus.chan && m->id) {
			lock(m);
			if(m->ref > 0 && m->id && m->c == bogus.chan) {
				m->ref++;
				unlock(m);
				return mattach(m, bogus.spec, bogus.auth);
			}
			unlock(m);	
		}
	}
	lock(&mntalloc);
	if(mntalloc.mntfree == 0) {
		unlock(&mntalloc);
		error(Enomntdev);
	}
	m = mntalloc.mntfree;
	mntalloc.mntfree = m->list;	
	m->id = mntalloc.id++;
	lock(m);
	unlock(&mntalloc);
	m->ref = 1;
	m->queue = 0;
	m->rip = 0;
	m->c = bogus.chan;

	switch(devchar[m->c->type]) {
	case 'H':			/* Hotrod */
	case '3':			/* BIT3 */
		m->mux = 1;
		break;
	default:
		m->mux = 0;
	}
	incref(m->c);
	unlock(m);

	return mattach(m, bogus.spec, bogus.auth);
}

Chan *
mattach(Mnt *m, char *spec, char *auth)
{
	Chan *c;
	Mntrpc *r;

	r = mntralloc();

	c = devattach('M', spec);
	c->dev = m->id;
	c->mntindex = m-mntalloc.mntarena;

	if(waserror()){
		mntfree(r);
		close(c);
		nexterror();
	}
	r->request.type = Tattach;
	r->request.fid = c->fid;
	memmove(r->request.uname, u->p->pgrp->user, NAMELEN);
	strncpy(r->request.aname, spec, NAMELEN);
	strncpy(r->request.auth, auth, NAMELEN);
	mountrpc(m, r);

	c->qid = r->reply.qid;
	c->mchan = m->c;
	c->mqid = c->qid;
	poperror();
	mntfree(r);
	return c;
}

Chan*
mntclone(Chan *c, Chan *nc)
{
	Mnt *m;
	Mntrpc *r;
	int alloc = 0;

	m = mntchk(c);
	r = mntralloc();
	if(nc == 0) {
		nc = newchan();
		alloc = 1;
	}
	if(waserror()){
		mntfree(r);
		if(alloc)
			close(nc);
		nexterror();
	}

	r->request.type = Tclone;
	r->request.fid = c->fid;
	r->request.newfid = nc->fid;
	mountrpc(m, r);

	nc->type = c->type;
	nc->dev = c->dev;
	nc->qid = c->qid;
	nc->mode = c->mode;
	nc->flag = c->flag;
	nc->offset = c->offset;
	nc->mnt = c->mnt;
	nc->mountid = c->mountid;
	nc->aux = c->aux;
	nc->mchan = c->mchan;
	nc->mqid = c->qid;
	incref(m);

	poperror();
	mntfree(r);
	return nc;
}

int	 
mntwalk(Chan *c, char *name)
{
	Mnt *m;
	Mntrpc *r;

	m = mntchk(c);
	r = mntralloc();
	if(waserror()) {
		mntfree(r);
		return 0;
	}
	r->request.type = Twalk;
	r->request.fid = c->fid;
	strncpy(r->request.name, name, NAMELEN);
	mountrpc(m, r);

	c->qid = r->reply.qid;

	poperror();
	mntfree(r);
	return 1;
}

void	 
mntstat(Chan *c, char *dp)
{
	Mnt *m;
	Mntrpc *r;

	m = mntchk(c);
	r = mntralloc();
	if(waserror()) {
		mntfree(r);
		nexterror();
	}
	r->request.type = Tstat;
	r->request.fid = c->fid;
	mountrpc(m, r);

	memmove(dp, r->reply.stat, DIRLEN);
	mntdirfix((uchar*)dp, c);
	poperror();
	mntfree(r);
}

Chan*
mntopen(Chan *c, int omode)
{
	Mnt *m;
	Mntrpc *r;

	m = mntchk(c);
	r = mntralloc();
	if(waserror()) {
		mntfree(r);
		nexterror();
	}
	r->request.type = Topen;
	r->request.fid = c->fid;
	r->request.mode = omode;
	mountrpc(m, r);

	c->qid = r->reply.qid;
	c->offset = 0;
	c->mode = openmode(omode);
	c->flag |= COPEN;
	poperror();
	mntfree(r);
	return c;
}

void	 
mntcreate(Chan *c, char *name, int omode, ulong perm)
{
	Mnt *m;
	Mntrpc *r;

	m = mntchk(c);
	r = mntralloc();
	if(waserror()) {
		mntfree(r);
		nexterror();
	}
	r->request.type = Tcreate;
	r->request.fid = c->fid;
	r->request.mode = omode;
	r->request.perm = perm;
	strncpy(r->request.name, name, NAMELEN);
	mountrpc(m, r);

	c->qid = r->reply.qid;
	c->flag |= COPEN;
	c->mode = openmode(omode);
	poperror();
	mntfree(r);
}

void	 
mntclunk(Chan *c, int t)
{
	Mnt *m;
	Mntrpc *r, *n, *q;
		
	m = mntchk(c);
	r = mntralloc();
	if(waserror()){
		mntfree(r);
		if(decref(m) == 0) {
			for(q = m->queue; q; q = r) {
				r = q->list;
				q->flushed = 0;
				mntfree(q);
			}
			m->id = 0;
			close(m->c);
			lock(&mntalloc);
			m->list = mntalloc.mntfree;
			mntalloc.mntfree = m;
			unlock(&mntalloc);
		}
		return;
	}

	r->request.type = t;
	r->request.fid = c->fid;
	mountrpc(m, r);
	nexterror();
}

void
mntclose(Chan *c)
{
	mntclunk(c, Tclunk);
}

void	 
mntremove(Chan *c)
{
	mntclunk(c, Tremove);
}

void
mntwstat(Chan *c, char *dp)
{
	Mnt *m;
	Mntrpc *r;

	m = mntchk(c);
	r = mntralloc();
	if(waserror()) {
		mntfree(r);
		nexterror();
	}
	r->request.type = Twstat;
	r->request.fid = c->fid;
	memmove(r->request.stat, dp, DIRLEN);
	mountrpc(m, r);
	poperror();
	mntfree(r);
}

long	 
mntread(Chan *c, void *buf, long n, ulong offset)
{
	uchar *p, *e;

	n = mntrdwr(Tread, c, buf, n, offset);
	if(c->qid.path & CHDIR) 
		for(p = (uchar*)buf, e = &p[n]; p < e; p += DIRLEN)
			mntdirfix(p, c);

	return n;
}

long	 
mntwrite(Chan *c, void *buf, long n, ulong offset)
{
	return mntrdwr(Twrite, c, buf, n, offset);	
}

long
mntrdwr(int type, Chan *c, void *buf, long n, ulong offset)
{
	Mnt *m;
	Mntrpc *r;
	ulong cnt, nr;
	char *uba;

	r = mntralloc();
	m = mntchk(c);
	if(waserror()) {
		mntfree(r);
		nexterror();
	}
	r->request.type = type;
	r->request.fid = c->fid;
	r->request.offset = offset;
	uba = buf;
	for(cnt = 0; n; n -= nr) {
		r->request.data = uba;
		r->request.count = limit(n, MAXFDATA);
		mountrpc(m, r);
		nr = r->reply.count;
		if(type == Tread)
			memmove(uba, r->reply.data, nr);
		r->request.offset += nr;
		uba += nr;
		cnt += nr;
		if(nr != r->request.count)
			break;
	}
	poperror();
	mntfree(r);
	return cnt;
}

void
mountrpc(Mnt *m, Mntrpc *r)
{
	mountio(m, r);
	if(r->reply.type == Rerror)
		errors(r->reply.ename);
	if(r->reply.type != r->request.type+1) {
		print("devmnt: mismatched reply T%d R%d tags req %d fls %d rep %d\n",
		r->request.type, r->reply.type, r->request.tag, r->flushtag, r->reply.tag);			errors("protocol error");
	}
}

void
mountio(Mnt *m, Mntrpc *r)
{
	int n;

	lock(m);
	r->m = m;
	r->list = m->queue;
	m->queue = r;
	unlock(m);

	/* Transmit a file system rpc */
	n = convS2M(&r->request, r->rpc);
	if(waserror()) {
		mntqrm(m, r);
		nexterror();
	}
	if((*devtab[m->c->type].write)(m->c, r->rpc, n, 0) != n)
		error(Eshortmsg);
	poperror();

	if(m->mux) {
		mntrpcread(m, r);
		return;
	}

	/* Gate readers onto the mount point one at a time */
	for(;;) {
		lock(m);
		if(m->rip == 0)
			break;
		unlock(m);
		if(waserror()) {
			if(mntflush(m, r) == 0)
				nexterror();
			continue;
		}
		sleep(&r->r, rpcattn, r);
		poperror();
		if(r->done)
			return;
	}
	m->rip = u->p;
	unlock(m);

	while(r->done == 0) {
		mntrpcread(m, r);
		mountmux(m, r);
	}
	mntgate(m);
}

void
mntrpcread(Mnt *m, Mntrpc *r)
{
	int n;

	for(;;) {
		if(waserror()) {
			if(mntflush(m, r) == 0) {
				if(m->mux == 0)
					mntgate(m);
				nexterror();
			}
			continue;
		}
		n = (*devtab[m->c->type].read)(m->c, r->rpc, MAXRPC, 0);
		poperror();
		if(n == 0)
			continue;
		if(convM2S(r->rpc, &r->reply, n) != 0)
			break;
	}
}

void
mntgate(Mnt *m)
{
	Mntrpc *q;

	lock(m);
	m->rip = 0;
	for(q = m->queue; q; q = q->list)
		if(q->done == 0) {
			lock(&q->r);
			if(q->r.p) {
				unlock(&q->r);
				unlock(m);
				wakeup(&q->r);
				return;
			}
			unlock(&q->r);
		}
	unlock(m);
}

void
mountmux(Mnt *m, Mntrpc *r)
{
	Mntrpc **l, *q;
	int done;
	char *dp;

	lock(m);
	l = &m->queue;
	for(q = *l; q; q = q->list) {
		if(q->request.tag == r->reply.tag) {
			if(q->flushed == 0)
				*l = q->list;
			q->done = 1;
			unlock(m);
			goto dispatch;
		}
		if(q->flushtag == r->reply.tag) {
			*l = q->list;
			q->flushed = 0;
			done = q->done;
			q->done = 1;
			unlock(m);
			if(done == 0) {
				r->reply.type = Rerror;
				strcpy(r->reply.ename, errstrtab[Eintr]);
				goto dispatch;
			}
			if(q->bfree)
				mntfree(q);
			return;
		}
		l = &q->list;
	}
	unlock(m);
	return;

dispatch:
	if(q != r) {		/* Completed someone else */
		dp = q->rpc;
		q->rpc = r->rpc;
		r->rpc = dp;
		memmove(&q->reply, &r->reply, sizeof(Fcall));
		wakeup(&q->r);					
	}

}

int
mntflush(Mnt *m, Mntrpc *r)
{
	Fcall flush;
	int n;

	r->flushtag++;
	if((r->flushtag-r->flushbase) == Flushspace)
		r->flushtag -= Flushspace;

	flush.type = Tflush;
	flush.tag = r->flushtag;
	flush.oldtag = r->request.tag;
	n = convS2M(&flush, r->flush);

	if(waserror()) {
		if(strcmp(u->error, errstrtab[Eintr]) == 0)
			return 1;
		mntqrm(m, r);
		return 0;
	}
	(*devtab[m->c->type].write)(m->c, r->flush, n, 0);
	poperror();
	lock(m);
	if(!r->done)
		r->flushed = 1;
	unlock(m);
	return 1;
}

Mntrpc *
mntralloc(void)
{
	Mntrpc *new;

	for(;;) {
		lock(&mntalloc);
		if(new = mntalloc.rpcfree) {
			mntalloc.rpcfree = new->list;
			unlock(&mntalloc);
			new->done = 0;
			new->bfree = 0;
			return new;
		}
		unlock(&mntalloc);
		resrcwait("no mount buffers");
	}
}

void
mntfree(Mntrpc *r)
{
	Mntrpc *q;
	Mnt *m, *e;
	int i;

	r->bfree = 1;
	if(r->flushed)
		return;

	lock(&mntalloc);
	r->list = mntalloc.rpcfree;
	mntalloc.rpcfree = r;
	unlock(&mntalloc);
}

void
mntqrm(Mnt *m, Mntrpc *r)
{
	Mntrpc **l, *f;

	lock(m);
	r->done = 1;
	r->flushed = 0;

	l = &m->queue;
	for(f = *l; f; f = f->list) {
		if(f == r) {
			*l = r->list;
			break;
		}
		l = &f->list;
	}
	unlock(m);
}

Mnt *
mntchk(Chan *c)
{
	Mnt *m;

	m = &mntalloc.mntarena[c->mntindex];
	if(m->id != c->dev)
		error(Eshutdown);
	return m;
}

void
mntdirfix(uchar *dirbuf, Chan *c)
{
	dirbuf[DIRLEN-4] = devchar[c->type];
	dirbuf[DIRLEN-3] = 0;
	dirbuf[DIRLEN-2] = c->dev;
	dirbuf[DIRLEN-1] = c->dev>>8;
}

int
rpcattn(Mntrpc *r)
{
	return r->done || r->m->rip == 0;
}

void
mntdump(void)
{
}

