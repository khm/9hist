enum {
	MaxEther	= 4,
	Ntypes		= 8,
};

typedef struct Ether Ether;
struct Ether {
	ISAConf;			/* hardware info */

	void	(*attach)(Ether*);	/* filled in by reset routine */
	long	(*write)(Ether*, void*, long);
	void	(*interrupt)(Ureg*, void*);
	void	*private;

	Etherpkt tpkt;			/* transmit buffer */
	Etherpkt rpkt;			/* receive buffer */

	QLock	tlock;			/* lock for grabbing transmitter queue */
	Rendez	tr;			/* wait here for free xmit buffer */
	long	tlen;			/* length of data in tpkt */

	Netif;
};

typedef struct {
	uchar	bit16;			/* true if a 16 bit interface */
	uchar	ram;			/* true if card has shared memory */

	ulong	dp8390;			/* I/O address of 8390 */
	ulong	data;			/* I/O data port if no shared memory */

	uchar	nxtpkt;			/* receive: software bndry */
	uchar	tstart;			/* 8390 ring addresses */
	uchar	pstart;
	uchar	pstop;
} Dp8390;

#define Dp8390BufSz	256

extern int dp8390reset(Ether*);
extern void dp8390getea(Ether*);
extern void dp8390setea(Ether*);

#define NEXT(x, l)	(((x)+1)%(l))
#define	HOWMANY(x, y)	(((x)+((y)-1))/(y))
#define ROUNDUP(x, y)	(HOWMANY((x), (y))*(y))

extern void addethercard(char*, int(*)(Ether*));