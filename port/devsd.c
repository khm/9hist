/*
 * Storage Device.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "ureg.h"
#include "../port/error.h"

#include "sd.h"

extern Dev sddevtab;
extern SDifc* sdifc[];

static QLock sdqlock;
static SDev* sdlist;
static SDunit** sdunit;
static int* sdunitflg;
static int sdnunit;

enum {
	Rawcmd,
	Rawdata,
	Rawstatus,
};

enum {
	Qtopdir		= 1,		/* top level directory */
	Qtopbase,

	Qunitdir,			/* directory per unit */
	Qunitbase,
	Qctl		= Qunitbase,
	Qraw,
	Qpart,
};

#define TYPE(q)		((q).path & 0x0F)
#define PART(q)		(((q).path>>4) & 0x0F)
#define UNIT(q)		(((q).path>>8) & 0xFF)
#define QID(u, p, t)	(((u)<<8)|((p)<<4)|(t))

static void
sdaddpart(SDunit* unit, char* name, ulong start, ulong end)
{
	SDpart *pp;
	int i, partno;

	/*
	 * Check name not already used
	 * and look for a free slot.
	 */
	if(unit->part != nil){
		partno = -1;
		for(i = 0; i < SDnpart; i++){
			pp = &unit->part[i];
			if(!pp->valid){
				if(partno == -1)
					partno = i;
				break;
			}
			if(strncmp(name, pp->name, NAMELEN) == 0){
				if(pp->start == start && pp->end == end)
					return;
				error(Ebadctl);
			}
		}
	}
	else{
		if((unit->part = malloc(sizeof(SDpart)*SDnpart)) == nil)
			error(Enomem);
		partno = 0;
	}

	/*
	 * Check there is a free slot and size and extent are valid.
	 */
	if(partno == -1 || start > end || end > unit->sectors)
		error(Eio);
	pp = &unit->part[partno];
	pp->start = start;
	pp->end = end;
	strncpy(pp->name, name, NAMELEN);
	strncpy(pp->user, eve, NAMELEN);
	pp->perm = 0640;
	pp->valid = 1;
	unit->npart++;
}

static void
sddelpart(SDunit* unit,  char* name)
{
	int i;
	SDpart *pp;

	/*
	 * Look for the partition to delete.
	 * Can't delete if someone still has it open.
	 * If it's the last valid partition zap the
	 * whole table.
	 */
	pp = unit->part;
	for(i = 0; i < SDnpart; i++){
		if(strncmp(name, pp->name, NAMELEN) == 0)
			break;
		pp++;
	}
	if(i >= SDnpart)
		error(Ebadctl);
	if(strncmp(up->user, pp->user, NAMELEN) && !iseve())
		error(Eperm);
	if(pp->nopen)
		error(Einuse);
	pp->valid = 0;

	unit->npart--;
	if(unit->npart == 0){
		free(unit->part);
		unit->part = nil;
	}
}

static int
sdinitpart(SDunit* unit)
{
	int nf;
	ulong start, end;
	char *f[4], *p, *q, buf[10];

	unit->sectors = unit->secsize = 0;
	unit->npart = 0;
	if(unit->part){
		free(unit->part);
		unit->part = nil;
	}

	if(unit->inquiry[0] & 0xC0)
		return 0;
	switch(unit->inquiry[0] & 0x1F){
	case 0x00:			/* DA */
	case 0x04:			/* WORM */
	case 0x05:			/* CD-ROM */
	case 0x07:			/* MO */
		break;
	default:
		return 0;
	}

	if(unit->dev->ifc->online)
		unit->dev->ifc->online(unit);
	if(unit->sectors){
		sdaddpart(unit, "data", 0, unit->sectors);
	
		/*
		 * Use partitions passed from boot program,
		 * e.g.
		 *	sdC0part=dos 63 123123/plan9 123123 456456
		 * This happens before /boot sets hostname so the
		 * partitions will have the null-string for user.
		 * The gen functions patch it up.
		 */
		snprint(buf, sizeof buf, "%spart", unit->name);
		for(p = getconf(buf); p != nil; p = q){
			if(q = strchr(p, '/'))
				*q++ = '\0';
			nf = getfields(p, f, nelem(f), 1, " \t\r");
			if(nf < 3)
				continue;
		
			start = strtoul(f[1], 0, 0);
			end = strtoul(f[2], 0, 0);
			if(!waserror()){
				sdaddpart(unit, f[0], start, end);
				poperror();
			}
		}			
	}

	return 1;
}

static SDunit*
sdgetunit(SDev* sdev, int subno)
{
	int index;
	SDunit *unit;

	/*
	 * Associate a unit with a given device and sub-unit
	 * number on that device.
	 * The device will be probed if it has not already been
	 * successfully accessed.
	 */
	qlock(&sdqlock);
	index = sdev->index+subno;
	unit = sdunit[index];
	if(unit == nil){
		/*
		 * Probe the unit only once. This decision
		 * may be a little severe and reviewed later.
		 */
		if(sdunitflg[index]){
			qunlock(&sdqlock);
			return nil;
		}
		if((unit = malloc(sizeof(SDunit))) == nil){
			qunlock(&sdqlock);
			return nil;
		}
		sdunitflg[index] = 1;

		if(sdev->enabled == 0 && sdev->ifc->enable)
			sdev->ifc->enable(sdev);
		sdev->enabled = 1;

		snprint(unit->name, NAMELEN, "%s%d", sdev->name, subno);
		unit->subno = subno;
		unit->dev = sdev;

		/*
		 * No need to lock anything here as this is only
		 * called before the unit is made available in the
		 * sdunit[] array.
		 */
		if(unit->dev->ifc->verify(unit) == 0){
			qunlock(&sdqlock);
			free(unit);
			return nil;
		}
		sdunit[index] = unit;
	}
	qunlock(&sdqlock);

	return unit;
}

static SDunit*
sdindex2unit(int index)
{
	SDev *sdev;

	/*
	 * Associate a unit with a given index into the top-level
	 * device directory.
	 * The device will be probed if it has not already been
	 * successfully accessed.
	 */
	for(sdev = sdlist; sdev != nil; sdev = sdev->next){
		if(index >= sdev->index && index < sdev->index+sdev->nunit)
			return sdgetunit(sdev, index-sdev->index);
	}

	return nil;

}

static void
sdreset(void)
{
	int i;
	SDev *sdev, *tail;

	/*
	 * Probe all configured controllers and make a list
	 * of devices found, accumulating a possible maximum number
	 * of units attached and marking each device with an index
	 * into the linear top-level directory array of units.
	 */
	tail = nil;
	for(i = 0; sdifc[i] != nil; i++){
		if(sdifc[i]->pnp == nil || (sdev = sdifc[i]->pnp()) == nil)
			continue;
		if(sdlist != nil)
			tail->next = sdev;
		else
			sdlist = sdev;
		for(tail = sdev; tail->next != nil; tail = tail->next){
			sdev->index = sdnunit;
			sdnunit += tail->nunit;
		}
		tail->index = sdnunit;
		sdnunit += tail->nunit;
	}

	/*
	 * Legacy and option code goes here. This will be hard...
	 */

	/*
	 * The maximum number of possible units is known, allocate
	 * placeholders for their datastructures; the units will be
	 * probed and structures allocated when attached.
	 * Allocate controller names for the different types.
	 */
	if(sdnunit == 0)
		return;
	if((sdunit = malloc(sdnunit*sizeof(SDunit*))) == nil)
		return;
	if((sdunitflg = malloc(sdnunit*sizeof(int))) == nil){
		free(sdunit);
		sdunit = nil;
		return;
	}
	for(i = 0; sdifc[i] != nil; i++){
		/*
		 * BUG: no check is made here or later when a
		 * unit is attached that the id and name are set.
		 */
		if(sdifc[i]->id)
			sdifc[i]->id(sdlist);
	}
}

static int
sd2gen(Chan* c, int i, Dir* dp)
{
	Qid q;
	vlong l;
	SDpart *pp;
	SDunit *unit;

	switch(i){
	case Qctl:
		q = (Qid){QID(UNIT(c->qid), PART(c->qid), Qctl), c->qid.vers};
		devdir(c, q, "ctl", 0, eve, 0640, dp);
		return 1;
	case Qraw:
		q = (Qid){QID(UNIT(c->qid), PART(c->qid), Qraw), c->qid.vers};
		devdir(c, q, "raw", 0, eve, CHEXCL|0600, dp);
		return 1;
	case Qpart:
		unit = sdunit[UNIT(c->qid)];
		if(unit->changed)
			break;
		pp = &unit->part[PART(c->qid)];
		l = (pp->end - pp->start) * (vlong)unit->secsize;
		q = (Qid){QID(UNIT(c->qid), PART(c->qid), Qpart), c->qid.vers};
		if(pp->user[0] == '\0')
			strncpy(pp->user, eve, NAMELEN);
		devdir(c, q, pp->name, l, pp->user, pp->perm, dp);
		return 1;
	}	
	return -1;
}

static int
sd1gen(Chan*, int i, Dir*)
{
	switch(i){
	default:
		return -1;
	}
	return -1;
}

static int
sdgen(Chan* c, Dirtab*, int, int s, Dir* dp)
{
	Qid q;
	vlong l;
	int i, r;
	SDpart *pp;
	SDunit *unit;
	char name[NAMELEN];

	switch(TYPE(c->qid)){
	case Qtopdir:
		if(s == DEVDOTDOT){
			q = (Qid){QID(0, 0, Qtopdir)|CHDIR, 0};
			snprint(name, NAMELEN, "#%C", sddevtab.dc);
			devdir(c, q, name, 0, eve, 0555, dp);
			return 1;
		}
		if(s < sdnunit){
			if(sdunit[s] == nil && sdindex2unit(s) == nil)
				return 0;
			q = (Qid){QID(s, 0, Qunitdir)|CHDIR, 0};
			devdir(c, q, sdunit[s]->name, 0, eve, 0555, dp);
			return 1;
		}
		s -= sdnunit;
		return sd1gen(c, s+Qtopbase, dp);
	case Qunitdir:
		if(s == DEVDOTDOT){
			q = (Qid){QID(0, 0, Qtopdir)|CHDIR, 0};
			snprint(name, NAMELEN, "#%C", sddevtab.dc);
			devdir(c, q, name, 0, eve, 0555, dp);
			return 1;
		}
		unit = sdunit[UNIT(c->qid)];
		qlock(&unit->ctl);
		if(!unit->changed && unit->sectors == 0)
			sdinitpart(unit);
		i = s+Qunitbase;
		if(i < Qpart){
			r = sd2gen(c, i, dp);
			qunlock(&unit->ctl);
			return r;
		}
		i -= Qpart;
		if(unit->npart == 0 || i >= SDnpart){
			qunlock(&unit->ctl);
			break;
		}
		pp = &unit->part[i];
		if(unit->changed || !pp->valid){
			qunlock(&unit->ctl);
			return 0;
		}
		l = (pp->end - pp->start) * (vlong)unit->secsize;
		q = (Qid){QID(UNIT(c->qid), i, Qpart), c->qid.vers};
		if(pp->user[0] == '\0')
			strncpy(pp->user, eve, NAMELEN);
		devdir(c, q, pp->name, l, pp->user, pp->perm, dp);
		qunlock(&unit->ctl);
		return 1;
	case Qraw:
	case Qctl:
	case Qpart:
		unit = sdunit[UNIT(c->qid)];
		qlock(&unit->ctl);
		r = sd2gen(c, TYPE(c->qid), dp);
		qunlock(&unit->ctl);
		return r;
	default:
		break;
	}

	return -1;
}

static Chan*
sdattach(char* spec)
{
	Chan *c;
	char *p;
	SDev *sdev;
	int idno, subno;

	if(sdnunit == 0 || *spec == '\0'){
		c = devattach(sddevtab.dc, spec);
		c->qid = (Qid){QID(0, 0, Qtopdir)|CHDIR, 0};
		return c;
	}

	if(spec[0] != 's' || spec[1] != 'd')
		error(Ebadspec);
	idno = spec[2];
	subno = strtol(&spec[3], &p, 0);
	if(p == &spec[3])
		error(Ebadspec);
	for(sdev = sdlist; sdev != nil; sdev = sdev->next){
		if(sdev->idno == idno && subno < sdev->nunit)
			break;
	}
	if(sdev == nil || sdgetunit(sdev, subno) == nil)
		error(Enonexist);

	c = devattach(sddevtab.dc, spec);
	c->qid = (Qid){QID(sdev->index+subno, 0, Qunitdir)|CHDIR, 0};
	c->dev = sdev->index+subno;
	return c;
}

static Chan*
sdclone(Chan* c, Chan* nc)
{
	return devclone(c, nc);
}

static int
sdwalk(Chan* c, char* name)
{
	return devwalk(c, name, nil, 0, sdgen);
}

static void
sdstat(Chan* c, char* db)
{
	devstat(c, db, nil, 0, sdgen);
}

static Chan*
sdopen(Chan* c, int omode)
{
	SDpart *pp;
	SDunit *unit;

	c = devopen(c, omode, 0, 0, sdgen);
	switch(TYPE(c->qid)){
	default:
		break;
	case Qraw:
		unit = sdunit[UNIT(c->qid)];
		if(!canlock(&unit->rawinuse)){
			c->flag &= ~COPEN;
			error(Einuse);
		}
		unit->state = Rawcmd;
		break;
	case Qpart:
		unit = sdunit[UNIT(c->qid)];
		qlock(&unit->ctl);
		if(waserror()){
			qunlock(&unit->ctl);
			c->flag &= ~COPEN;
			nexterror();
		}
		if(unit->changed)
			error(Eio);
		pp = &unit->part[PART(c->qid)];
		pp->nopen++;
		unit->nopen++;
		qunlock(&unit->ctl);
		poperror();
		break;
	}
	return c;
}

static void
sdclose(Chan* c)
{
	SDpart *pp;
	SDunit *unit;

	if(c->qid.path & CHDIR)
		return;
	if(!(c->flag & COPEN))
		return;

	switch(TYPE(c->qid)){
	default:
		break;
	case Qraw:
		unit = sdunit[UNIT(c->qid)];
		unlock(&unit->rawinuse);
		break;
	case Qpart:
		unit = sdunit[UNIT(c->qid)];
		qlock(&unit->ctl);
		if(waserror()){
			qunlock(&unit->ctl);
			c->flag &= ~COPEN;
			nexterror();
		}
		pp = &unit->part[PART(c->qid)];
		pp->nopen--;
		unit->nopen--;
		if(unit->nopen == 0)
			unit->changed = 0;
		qunlock(&unit->ctl);
		poperror();
		break;
	}
}

static long
sdbio(Chan* c, int write, char* a, long len, vlong off)
{
	long l;
	uchar *b;
	SDpart *pp;
	SDunit *unit;
	ulong bno, max, nb, offset;

	unit = sdunit[UNIT(c->qid)];

	qlock(&unit->ctl);
	if(waserror()){
		qunlock(&unit->ctl);
		nexterror();
	}
	if(unit->changed)
		error(Eio);

	/*
	 * Check the request is within bounds.
	 * Removeable drives are locked throughout the I/O
	 * in case the media changes unexpectedly.
	 * Non-removeable drives are not locked during the I/O
	 * to allow the hardware to optimise if it can; this is
	 * a little fast and loose.
	 * It's assumed that non-removeable media parameters
	 * (sectors, secsize) can't change once the drive has
	 * been brought online.
	 */
	pp = &unit->part[PART(c->qid)];
	bno = (off/unit->secsize) + pp->start;
	nb = ((off+len+unit->secsize-1)/unit->secsize) + pp->start - bno;
	max = SDmaxio/unit->secsize;
	if(nb > max)
		nb = max;
	if(bno+nb > pp->end)
		nb = pp->end - bno;
	if(bno >= pp->end || nb == 0){
		if(write)
			error(Eio);
		qunlock(&unit->ctl);
		poperror();
		return 0;
	}
	if(!(unit->inquiry[1] & 0x80)){
		qunlock(&unit->ctl);
		poperror();
	}

	b = sdmalloc(nb*unit->secsize);
	if(b == nil)
		error(Enomem);
	if(waserror()){
		sdfree(b);
		nexterror();
	}

	offset = off%unit->secsize;
	if(write){
		if(offset || (len%unit->secsize)){
			l = unit->dev->ifc->bio(unit, 0, 0, b, nb, bno);
			if(l < 0)
				error(Eio);
			if(l < (nb*unit->secsize)){
				nb = l/unit->secsize;
				l = nb*unit->secsize - offset;
				if(len > l)
					len = l;
			}
		}
		memmove(b+offset, a, len);
		l = unit->dev->ifc->bio(unit, 0, 1, b, nb, bno);
		if(l < 0)
			error(Eio);
		if(l < offset)
			len = 0;
		else if(len > l - offset)
			len = l - offset;
	}
	else {
		l = unit->dev->ifc->bio(unit, 0, 0, b, nb, bno);
		if(l < 0)
			error(Eio);
		if(l < offset)
			len = 0;
		else if(len > l - offset)
			len = l - offset;
		memmove(a, b+offset, len);
	}
	sdfree(b);
	poperror();

	if(unit->inquiry[1] & 0x80){
		qunlock(&unit->ctl);
		poperror();
	}

	return len;
}

static long
sdrio(SDreq* r, void* a, long n)
{
	void *data;

	if(n >= SDmaxio || n < 0)
		error(Etoobig);

	data = nil;
	if(n){
		if((data = sdmalloc(n)) == nil)
			error(Enomem);
		if(r->write)
			memmove(data, a, n);
	}
	r->data = data;
	r->dlen = n;

	if(waserror()){
		if(data != nil){
			sdfree(data);
			r->data = nil;
		}
		nexterror();
	}

	if(r->unit->dev->ifc->rio(r) != SDok)
		error(Eio);

	if(!r->write && r->rlen > 0)
		memmove(a, data, r->rlen);
	if(data != nil){
		sdfree(data);
		r->data = nil;
	}
	poperror();

	return r->rlen;
}

static long
sdread(Chan *c, void *a, long n, vlong off)
{
	char *p;
	SDpart *pp;
	SDunit *unit;
	ulong offset;
	int i, l, status;

	offset = off;
	switch(TYPE(c->qid)){
	default:
		error(Eperm);
	case Qtopdir:
	case Qunitdir:
		return devdirread(c, a, n, 0, 0, sdgen);
	case Qctl:
		unit = sdunit[UNIT(c->qid)];
		p = malloc(READSTR);
		l = snprint(p, READSTR, "inquiry %.48s\n",
			(char*)unit->inquiry+8);
		qlock(&unit->ctl);
		if(!unit->changed && unit->sectors){
			/*
			 * If there's a device specific routine it must
			 * provide all information pertaining to night geometry
			 * and the garscadden trains.
			 */
			if(unit->dev->ifc->rctl)
				l += unit->dev->ifc->rctl(unit, p+l, READSTR-l);
			else
				l += snprint(p+l, READSTR-l,
					"geometry %ld %ld\n",
					unit->sectors, unit->secsize);
			pp = unit->part;
			for(i = 0; i < SDnpart; i++){
				if(pp->valid)
					l += snprint(p+l, READSTR-l,
						"part %.*s %lud %lud\n",
						NAMELEN, pp->name,
						pp->start, pp->end);
				pp++;
			}
		}
		qunlock(&unit->ctl);
		l = readstr(offset, a, n, p);
		free(p);
		return l;
	case Qraw:
		unit = sdunit[UNIT(c->qid)];
		if(unit->state == Rawdata){
			unit->state = Rawstatus;
			return sdrio(unit->req, a, n);
		}
		else if(unit->state == Rawstatus){
			status = unit->req->status;
			unit->state = Rawcmd;
			free(unit->req);
			unit->req = nil;
			return readnum(0, a, n, status, NUMSIZE);
		}
		break;
	case Qpart:
		return sdbio(c, 0, a, n, off);
	}

	return 0;
}

static long
sdwrite(Chan *c, void *a, long n, vlong off)
{
	Cmdbuf *cb;
	SDreq *req;
	SDunit *unit;
	ulong end, start;

	switch(TYPE(c->qid)){
	default:
		error(Eperm);
	case Qctl:
		cb = parsecmd(a, n);
		unit = sdunit[UNIT(c->qid)];

		qlock(&unit->ctl);
		if(waserror()){
			qunlock(&unit->ctl);
			free(cb);
			nexterror();
		}
		if(unit->changed)
			error(Eio);

		if(cb->nf < 1)
			error(Ebadctl);
		if(strcmp(cb->f[0], "part") == 0){
			if(cb->nf != 4 || unit->npart >= SDnpart)
				error(Ebadctl);
			if(unit->sectors == 0 && !sdinitpart(unit))
				error(Eio);
			start = strtoul(cb->f[2], 0, 0);
			end = strtoul(cb->f[3], 0, 0);
			sdaddpart(unit, cb->f[1], start, end);
		}
		else if(strcmp(cb->f[0], "delpart") == 0){
			if(cb->nf != 2 || unit->part == nil)
				error(Ebadctl);
			sddelpart(unit, cb->f[1]);
		}
		else if(unit->dev->ifc->wctl)
			unit->dev->ifc->wctl(unit, cb);
		else
			error(Ebadctl);
		qunlock(&unit->ctl);
		poperror();
		free(cb);
		break;

	case Qraw:
		unit = sdunit[UNIT(c->qid)];
		switch(unit->state){
		case Rawcmd:
			if(n < 6 || n > sizeof(req->cmd))
				error(Ebadarg);
			if((req = malloc(sizeof(SDreq))) == nil)
				error(Enomem);
			req->unit = unit;
			memmove(req->cmd, a, n);
			req->clen = n;
			req->flags = SDnosense;
			req->status = ~0;

			unit->req = req;
			unit->state = Rawdata;
			break;

		case Rawstatus:
			unit->state = Rawcmd;
			free(unit->req);
			unit->req = nil;
			error(Ebadusefd);

		case Rawdata:
			if(unit->state != Rawdata)
				error(Ebadusefd);
			unit->state = Rawstatus;

			unit->req->write = 1;
			return sdrio(unit->req, a, n);
		}
		break;
	case Qpart:
		return sdbio(c, 1, a, n, off);
	}

	return n;
}

static void
sdwstat(Chan* c, char* dp)
{
	Dir d;
	SDpart *pp;
	SDunit *unit;

	if((c->qid.path & CHDIR) || TYPE(c->qid) != Qpart)
		error(Eperm);

	unit = sdunit[UNIT(c->qid)];
	qlock(&unit->ctl);
	if(waserror()){
		qunlock(&unit->ctl);
		nexterror();
	}

	if(unit->changed)
		error(Enonexist);
	pp = &unit->part[PART(c->qid)];
	if(!pp->valid)
		error(Enonexist);
	if(strncmp(up->user, pp->user, NAMELEN) && !iseve())
		error(Eperm);

	convM2D(dp, &d);
	strncpy(pp->user, d.uid, NAMELEN);
	pp->perm = d.mode&0777;

	qunlock(&unit->ctl);
	poperror();
}

Dev sddevtab = {
	'S',
	"sd",

	sdreset,
	devinit,
	sdattach,
	sdclone,
	sdwalk,
	sdstat,
	sdopen,
	devcreate,
	sdclose,
	sdread,
	devbread,
	sdwrite,
	devbwrite,
	devremove,
	sdwstat,
};
