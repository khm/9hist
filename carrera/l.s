#include "mem.h"

#define	SP		R29

#define	NOOP		WORD	$0x27
#define	FCRNOOP		NOOP; NOOP; NOOP
#define	WAIT		NOOP; NOOP
#define	NOOP4		NOOP; NOOP; NOOP; NOOP
#define	ERET		NOOP4;NOOP4;NOOP4;WORD $0x42000018;NOOP4;NOOP4;NOOP4
#define	CONST(x,r)	MOVW $((x)&0xffff0000), r; OR  $((x)&0xffff), r

/*
 *  R4000 instructions
 */
#define	LL(base, rt)		WORD	$((060<<26)|((base)<<21)|((rt)<<16))
#define	SC(base, rt)		WORD	$((070<<26)|((base)<<21)|((rt)<<16))

#define DBG(S)	CONST(0xa0001000, R20);CONST(S, R21);MOVW	R21,(R20)

/*
 * Boot first processor
 */
TEXT	start(SB), $-4

	MOVW	$setR30(SB), R30
	MOVW	$(CU1|INTR7|INTR6|INTR5|INTR4|INTR3|INTR2|INTR1|INTR0), R1
	MOVW	R1, M(STATUS)
	WAIT

	MOVW	$TLBROFF, R1
	MOVW	R1, M(WIRED)

	MOVW	$((0x1C<<7)|(1<<24)), R1
	MOVW	R1, FCR31	/* permit only inexact and underflow */
	NOOP
	MOVD	$0.5, F26
	SUBD	F26, F26, F24
	ADDD	F26, F26, F28
	ADDD	F28, F28, F30

	MOVD	F24, F0
	MOVD	F24, F2
	MOVD	F24, F4
	MOVD	F24, F6
	MOVD	F24, F8
	MOVD	F24, F10
	MOVD	F24, F12
	MOVD	F24, F14
	MOVD	F24, F16
	MOVD	F24, F18
	MOVD	F24, F20
	MOVD	F24, F22

	MOVW	$MACHADDR, R(MACH)
	ADDU	$(BY2PG-4), R(MACH), SP
	MOVW	$0, R(USER)
	MOVW	R0, 0(R(MACH))

	MOVW	$edata(SB), R1
	MOVW	$end(SB), R2

clrbss:
	MOVB	$0, (R1)
	ADDU	$1, R1
	BNE	R1, R2, clrbss

	MOVW	R4, _argc(SB)
	MOVW	R5, _argv(SB)
	MOVW	R6, _env(SB)
	JAL	main(SB)
	JMP	(R0)

/*
 * Take first processor into user mode
 * 	- argument is stack pointer to user
 */
TEXT	touser(SB), $-4

	MOVW	$(UTZERO+32), R2	/* header appears in text */
	MOVW	(R2), R3		/* fault now */
	MOVW	M(STATUS), R4
	WAIT
	AND	$(~KMODEMASK), R4
	OR	$(KUSER|EXL|IE|UX), R4
	MOVW	R4, M(STATUS)
	WAIT
	MOVW	R1, SP
	MOVV	R2, M(EPC)
	ERET

TEXT	firmware(SB), $0
	MOVW	$0xE000D004, R1
	MOVW	R0, 0(R1)
firm:
	JMP	firm

TEXT	splhi(SB), $0
	MOVW	R31, 12(R(MACH))	/* save PC in m->splpc */
	MOVW	M(STATUS), R1
	WAIT
	AND	$~IE, R1, R2
	MOVW	R2, M(STATUS)
	WAIT
	RET

TEXT	splx(SB), $0
	MOVW	R31, 12(R(MACH))	/* save PC in m->splpc */
	MOVW	M(STATUS), R2
	WAIT
	AND	$IE, R1
	AND	$~IE, R2
	OR	R2, R1
	MOVW	R1, M(STATUS)
	WAIT
	RET

TEXT	spllo(SB), $0
	MOVW	M(STATUS), R1
	WAIT
	OR	$IE, R1, R2
	MOVW	R2, M(STATUS)
	WAIT
	RET

TEXT	spldone(SB), $0
	RET

TEXT	wbflush(SB), $-4
	RET

TEXT	setlabel(SB), $-4
	MOVW	R29, 0(R1)
	MOVW	R31, 4(R1)
	MOVW	$0, R1
	RET

TEXT	gotolabel(SB), $-4
	MOVW	0(R1), R29
	MOVW	4(R1), R31
	MOVW	$1, R1
	RET

TEXT	gotopc(SB), $8
	MOVW	R1, 0(FP)		/* save arguments for later */
	MOVW	$(64*1024), R7
	MOVW	R7, 8(SP)
	JAL	icflush(SB)
	MOVW	0(FP), R7
	MOVW	_argc(SB), R4
	MOVW	_argv(SB), R5
	MOVW	_env(SB), R6
	MOVW	R0, 4(SP)
	JMP	(R7)

TEXT	puttlb(SB), $0
	MOVW	R1, M(TLBVIRT)
	MOVW	4(FP), R2		/* phys0 */
	MOVW	8(FP), R3		/* phys1 */
	MOVW	R2, M(TLBPHYS0)
	WAIT
	MOVW	$PGSZ4K, R1
	MOVW	R3, M(TLBPHYS1)
	WAIT
	MOVW	R1, M(PAGEMASK)
	OR	R2, R3, R4		/* MTC0 delay slot */
	AND	$PTEVALID, R4		/* MTC0 delay slot */
	NOOP
	TLBP
	NOOP4
	MOVW	M(INDEX), R1
	NOOP4
	BGEZ	R1, index
	BEQ	R4, dont		/* cf. kunmap */
	TLBWR
	NOOP
dont:
	RET
index:
	TLBWI
	NOOP
	RET

TEXT	getwired(SB),$0
	MOVW	M(WIRED), R1
	WAIT
	RET

TEXT	getrandom(SB),$0
	MOVW	M(RANDOM), R1
	WAIT
	RET

TEXT	puttlbx(SB), $0
	MOVW	4(FP), R2
	MOVW	8(FP), R3
	MOVW	12(FP), R4
	MOVW	16(FP), R5
	MOVW	R2, M(TLBVIRT)
	NOOP4
	MOVW	R3, M(TLBPHYS0)
	NOOP4
	MOVW	R4, M(TLBPHYS1)
	NOOP4
	MOVW	R5, M(PAGEMASK)
	NOOP4
	MOVW	R1, M(INDEX)
	NOOP4
	TLBWI
	WAIT
	RET

TEXT	tlbvirt(SB), $0
	NOOP
	MOVW	M(TLBVIRT), R1
	NOOP
	RET

TEXT	gettlbx(SB), $0
	MOVW	4(FP), R5
	MOVW	M(TLBVIRT), R10
	NOOP4
	MOVW	R1, M(INDEX)
	NOOP4
	TLBR
	NOOP4
	MOVW	M(TLBVIRT), R2
	MOVW	M(TLBPHYS0), R3
	MOVW	M(TLBPHYS1), R4
	NOOP4
	MOVW	R2, 0(R5)
	MOVW	R3, 4(R5)
	MOVW	R4, 8(R5)
	MOVW	R10, M(TLBVIRT)
	NOOP4
	RET

TEXT	gettlbp(SB), $0
	MOVW	4(FP), R5
	MOVW	R1, M(TLBVIRT)
	NOOP
	NOOP
	NOOP
	TLBP
	NOOP
	NOOP
	MOVW	M(INDEX), R1
	NOOP
	BLTZ	R1, gettlbp1
	TLBR
	NOOP
	NOOP
	NOOP
	MOVW	M(TLBVIRT), R2
	MOVW	M(TLBPHYS0), R3
	MOVW	M(TLBPHYS1), R4
	MOVW	M(PAGEMASK), R6
	NOOP
	MOVW	R2, 0(R5)
	MOVW	R3, 4(R5)
	MOVW	R4, 8(R5)
	MOVW	R6, 12(R5)
gettlbp1:
	RET

TEXT	gettlbvirt(SB), $0
	MOVW	R1, M(INDEX)
	NOOP
	NOOP
	TLBR
	NOOP
	NOOP
	NOOP
	MOVW	M(TLBVIRT), R1
	NOOP
	RET

/* also vector80 */
TEXT	vector0(SB), $-4
	NOOP4
	NOOP4
	MOVW	$utlbmiss(SB), R26
	JMP	(R26)

TEXT	utlbmiss(SB), $-4
	CONST	(MACHADDR, R26)		/* R26 = m-> */
	MOVW	16(R26), R27
	ADDU	$1, R27
	MOVW	R27, 16(R26)			/* m->tlbfault++ */

	MOVW	M(TLBVIRT), R27
	WAIT
	MOVW	R27, R26
	SRL	$13, R27
	XOR	R26, R27
	AND	$(STLBSIZE-1), R27
	SLL	$2, R27
	MOVW	R27, R26
	SLL	$1, R27
	ADDU	R26, R27
	/* R27 = ((tlbvirt^(tlbvirt>>13)) & (STLBSIZE-1)) x 12 */

	CONST	(MACHADDR, R26)		/* R26 = m-> */
	MOVW	4(R26), R26		/* R26 = m->stb */
	ADDU	R26, R27		/* R27 = &m->stb[hash] */

	MOVW	M(BADVADDR), R26
	WAIT
	AND	$BY2PG, R26
	BNE	R26, utlbodd

	MOVW	4(R27), R26
	BEQ	R26, stlbm
	MOVW	R26, M(TLBPHYS0)
	WAIT
	MOVW	8(R27), R26
	MOVW	R26, M(TLBPHYS1)
	JMP	utlbcom

utlbodd:
	MOVW	8(R27), R26
	BEQ	R26, stlbm
	MOVW	R26, M(TLBPHYS1)
	WAIT
	MOVW	4(R27), R26
	MOVW	R26, M(TLBPHYS0)

utlbcom:
	WAIT				/* MTC0/MFC0 hazard */
	MOVW	M(TLBVIRT), R26
	WAIT
	MOVW	(R27), R27
	BEQ	R27, stlbm
	BNE	R26, R27, stlbm 

	MOVW	$PGSZ4K, R27
	MOVW	R27, M(PAGEMASK)
	NOOP4
	TLBP
	NOOP4
	MOVW	M(INDEX), R26
	NOOP4
	BGEZ	R26, utlindex
	TLBWR
	ERET
utlindex:
	TLBWI
	ERET

stlbm:
	MOVW	$exception(SB), R26
	JMP	(R26)

TEXT	vector100(SB), $-4
	NOOP4
	NOOP4
	MOVW	$exception(SB), R26
	JMP	(R26)

TEXT	vector180(SB), $-4
	NOOP4
	NOOP4
	MOVW	$exception(SB), R26
	JMP	(R26)

TEXT	exception(SB), $-4
	MOVW	M(STATUS), R26
	WAIT
	AND	$KUSER, R26
	BEQ	R26, waskernel

wasuser:
	CONST	(MACHADDR, R27)		/* R27 = m-> */
	MOVW	8(R27), R26		/* R26 = m->proc */
	MOVW	8(R26), R27		/* R27 = m->proc->kstack */
	MOVV	SP, R26			/* save user sp */
	ADDU	$(KSTACK-UREGSIZE), R27, SP

	MOVV	R26, Ureg_sp(SP)		/* user SP */
	MOVV	R31, Ureg_r31(SP)
	MOVV	R30, Ureg_r30(SP)
	MOVW	M(CAUSE), R26
	MOVV	R25, Ureg_r25(SP)		/* R(USER), R(MACH) */
	MOVV	R24, Ureg_r24(SP)
	AND	$(EXCMASK<<2), R26
	SUBU	$(CSYS<<2), R26

	JAL	saveregs(SB)

	MOVW	$setR30(SB), R30
	CONST	(MACHADDR, R(MACH))		/* R(MACH) = m-> */
	MOVW	8(R(MACH)), R(USER)		/* up = m->proc */
	MOVW	4(SP), R1			/* first arg for syscall/trap */
	BNE	R26, notsys

	JAL	syscall(SB)

sysrestore:
	MOVV	Ureg_r31(SP), R31
	MOVW	Ureg_status(SP), R26
	MOVV	Ureg_r30(SP), R30
	MOVW	R26, M(STATUS)
	WAIT
	MOVV	Ureg_pc(SP), R26		/* old pc */
	MOVV	Ureg_sp(SP), SP
	MOVV	R26, M(EPC)
	ERET

notsys:
	JAL	trap(SB)
restore:
	JAL	restregs(SB)
	MOVV	Ureg_r31(SP), R31
	MOVV	Ureg_r30(SP), R30
	MOVV	Ureg_r25(SP), R25			/* R(USER), R(MACH) */
	MOVV	Ureg_r24(SP), R24
	MOVV	Ureg_sp(SP), SP
	MOVV	R26, M(EPC)
	ERET

waskernel:
	MOVV	SP, R26
	SUBU	$UREGSIZE, SP
	OR	$7, SP
	XOR	$7, SP
	MOVV	R26, Ureg_sp(SP)
	MOVV	R31, Ureg_r31(SP)
	MOVW	$1, R26			/* not syscall */
	JAL	saveregs(SB)
	MOVW	4(SP), R1		/* first arg for trap */
	JAL	trap(SB)
	JAL	restregs(SB)
	MOVV	Ureg_r31(SP), R31
	MOVV	Ureg_sp(SP), SP
	MOVV	R26, M(EPC)
	ERET

TEXT	forkret(SB), $-4
	MOVV	R0, R1			/* Fake out system call return */
	JMP	sysrestore

TEXT	saveregs(SB), $-4
	MOVV	R1, Ureg_r1(SP)		/* save early to use as temp */
	MOVV	R2, Ureg_r2(SP)

	ADDU	$Uoffset,SP, R1
	MOVW	R1, 4(SP)		/* arg to base of regs */
	MOVW	M(STATUS), R1
	MOVV	M(EPC), R2
	WAIT
	MOVW	R1, Ureg_status(SP)
	MOVV	R2, Ureg_pc(SP)

	MOVW	$(~KMODEMASK),R2	/* don't let him use R28 */
	AND	R2, R1
	MOVW	R1, M(STATUS)
	WAIT
	BEQ	R26, return		/* sys call, don't save */

	MOVW	M(CAUSE), R1
	MOVV	M(BADVADDR), R2
	NOOP
	MOVW	R1, Ureg_cause(SP)
	MOVV	M(TLBVIRT), R1
	NOOP
	MOVV	R2, Ureg_badvaddr(SP)
	MOVV	R1, Ureg_tlbvirt(SP)
	MOVW	HI, R1
	MOVW	LO, R2
	MOVV	R1, Ureg_hi(SP)
	MOVV	R2, Ureg_lo(SP)
					/* LINK,SB,SP missing */
	MOVV	R28, Ureg_r28(SP)
					/* R27, R26 not saved */
					/* R25, R24 missing */
	MOVV	R23, Ureg_r23(SP)
	MOVV	R22, Ureg_r22(SP)
	MOVV	R21, Ureg_r21(SP)
	MOVV	R20, Ureg_r20(SP)
	MOVV	R19, Ureg_r19(SP)
	MOVV	R18, Ureg_r18(SP)
	MOVV	R17, Ureg_r17(SP)
	MOVV	R16, Ureg_r16(SP)
	MOVV	R15, Ureg_r15(SP)
	MOVV	R14, Ureg_r14(SP)
	MOVV	R13, Ureg_r13(SP)
	MOVV	R12, Ureg_r12(SP)
	MOVV	R11, Ureg_r11(SP)
	MOVV	R10, Ureg_r10(SP)
	MOVV	R9, Ureg_r9(SP)
	MOVV	R8, Ureg_r8(SP)
	MOVV	R7, Ureg_r7(SP)
	MOVV	R6, Ureg_r6(SP)
	MOVV	R5, Ureg_r5(SP)
	MOVV	R4, Ureg_r4(SP)
	MOVV	R3, Ureg_r3(SP)
return:
	RET

TEXT	restregs(SB), $-4
					/* LINK,SB,SP missing */
	MOVV	Ureg_r28(SP), R28
					/* R27, R26 not saved */
					/* R25, R24 missing */
	MOVV	Ureg_r23(SP), R23
	MOVV	Ureg_r22(SP), R22
	MOVV	Ureg_r21(SP), R21
	MOVV	Ureg_r20(SP), R20
	MOVV	Ureg_r19(SP), R19
	MOVV	Ureg_r18(SP), R18
	MOVV	Ureg_r17(SP), R17
	MOVV	Ureg_r16(SP), R16
	MOVV	Ureg_r15(SP), R15
	MOVV	Ureg_r14(SP), R14
	MOVV	Ureg_r13(SP), R13
	MOVV	Ureg_r12(SP), R12
	MOVV	Ureg_r11(SP), R11
	MOVV	Ureg_r10(SP), R10
	MOVV	Ureg_r9(SP), R9
	MOVV	Ureg_r8(SP), R8
	MOVV	Ureg_r7(SP), R7
	MOVV	Ureg_r6(SP), R6
	MOVV	Ureg_r5(SP), R5
	MOVV	Ureg_r4(SP), R4
	MOVV	Ureg_r3(SP), R3

	MOVV	Ureg_lo(SP), R2
	MOVV	Ureg_hi(SP), R1
	MOVW	R2, LO
	MOVW	R1, HI

	MOVW	Ureg_status(SP), R1
	MOVV	Ureg_r2(SP), R2
	MOVW	R1, M(STATUS)
	WAIT
	MOVV	Ureg_r1(SP), R1
	MOVV	Ureg_pc(SP), R26		/* old pc */
	RET

TEXT	rfnote(SB), $-4
	MOVW	R1, R26			/* 1st arg is &uregpointer */
	SUBU	$(BY2WD), R26, SP	/* pc hole */
	JMP	restore
	

TEXT	clrfpintr(SB), $0
	MOVW	M(STATUS), R3
	WAIT
	OR	$CU1, R3
	MOVW	R3, M(STATUS)
	NOOP
	NOOP
	NOOP

	MOVW	FCR31, R1
	FCRNOOP
	MOVW	R1, R2
	AND	$~(0x3F<<12), R2
	MOVW	R2, FCR31

	AND	$~CU1, R3
	MOVW	R3, M(STATUS)
	WAIT
	RET

TEXT	getstatus(SB), $0
	MOVW	M(STATUS), R1
	WAIT
	RET

TEXT	savefpregs(SB), $0
	MOVW	FCR31, R2			/* 3 delays before R2 ok */
	MOVW	M(STATUS), R3
	WAIT
	AND	$~(0x3F<<12), R2, R4
	MOVW	R4, FCR31

	MOVD	F0, 0x00(R1)
	MOVD	F2, 0x08(R1)
	MOVD	F4, 0x10(R1)
	MOVD	F6, 0x18(R1)
	MOVD	F8, 0x20(R1)
	MOVD	F10, 0x28(R1)
	MOVD	F12, 0x30(R1)
	MOVD	F14, 0x38(R1)
	MOVD	F16, 0x40(R1)
	MOVD	F18, 0x48(R1)
	MOVD	F20, 0x50(R1)
	MOVD	F22, 0x58(R1)
	MOVD	F24, 0x60(R1)
	MOVD	F26, 0x68(R1)
	MOVD	F28, 0x70(R1)
	MOVD	F30, 0x78(R1)

	MOVW	R2, 0x80(R1)
	AND	$~CU1, R3
	MOVW	R3, M(STATUS)
	WAIT
	RET

TEXT	restfpregs(SB), $0
	MOVW	M(STATUS), R3
	WAIT
	OR	$CU1, R3
	MOVW	R3, M(STATUS)
	WAIT
	MOVW	fpstat+4(FP), R2
	NOOP

	MOVD	0x00(R1), F0
	MOVD	0x08(R1), F2
	MOVD	0x10(R1), F4
	MOVD	0x18(R1), F6
	MOVD	0x20(R1), F8
	MOVD	0x28(R1), F10
	MOVD	0x30(R1), F12
	MOVD	0x38(R1), F14
	MOVD	0x40(R1), F16
	MOVD	0x48(R1), F18
	MOVD	0x50(R1), F20
	MOVD	0x58(R1), F22
	MOVD	0x60(R1), F24
	MOVD	0x68(R1), F26
	MOVD	0x70(R1), F28
	MOVD	0x78(R1), F30

	MOVW	R2, FCR31
	AND	$~CU1, R3
	MOVW	R3, M(STATUS)
	WAIT
	RET

TEXT	fcr31(SB), $0
	MOVW	FCR31, R1		/* 3 delays before using R1 */
	MOVW	M(STATUS), R3
	WAIT
	AND	$~CU1, R3
	MOVW	R3, M(STATUS)
	WAIT
	RET

/*
 * Emulate 68020 test and set: load linked / store conditional
 */

TEXT	tas(SB), $0
	MOVW	R1, R2		/* address of key */
tas1:
	MOVW	$1, R3
	LL(2, 1)
	NOOP
	SC(2, 3)
	NOOP
	BEQ	R3, tas1
	RET

/*
 *  cache manipulation
 */

#define	CACHE	BREAK		/* overloaded op-code */

#define	PI	R((0		/* primary I cache */
#define	PD	R((1		/* primary D cache */
#define	SD	R((3		/* secondary combined I/D cache */

#define	IWBI	(0<<2)))	/* index write-back invalidate */
#define	ILT	(1<<2)))	/* index load tag */
#define	IST	(2<<2)))	/* index store tag */
#define	CDE	(3<<2)))	/* create dirty exclusive */
#define	HI	(4<<2)))	/* hit invalidate */
#define	HWBI	(5<<2)))	/* hit write back invalidate */
#define	HWB	(6<<2)))	/* hit write back */
#define	HSV	(7<<2)))	/* hit set virtual */

/*
 *  we avoid using R4, R5, R6, and R7 so gotopc can call us without saving them
 */
TEXT	icflush(SB), $-4			/* icflush(virtaddr, count) */
	MOVW	M(STATUS), R10
	WAIT
	MOVW	4(FP), R9
	MOVW	$0, M(STATUS)
	WAIT
	WAIT
	WAIT
	ADDU	R1, R9			/* R9 = last address */
	MOVW	$(~0x3f), R8
	AND	R1, R8			/* R8 = first address, rounded down */
	ADDU	$0x3f, R9
	AND	$(~0x3f), R9		/* round last address up */
	SUBU	R8, R9			/* R9 = revised count */
icflush1:			/* primary cache line size is 16 bytes */
	CACHE	PD+HWB, 0x00(R8)
	CACHE	PI+HI, 0x00(R8)
	CACHE	PD+HWB, 0x10(R8)
	CACHE	PI+HI, 0x10(R8)
	CACHE	PD+HWB, 0x20(R8)
	CACHE	PI+HI, 0x20(R8)
	CACHE	PD+HWB, 0x30(R8)
	CACHE	PI+HI, 0x30(R8)
	SUBU	$0x40, R9
	ADDU	$0x40, R8
	BGTZ	R9, icflush1
	MOVW	R10, M(STATUS)
	WAIT
	WAIT
	WAIT
	RET

TEXT	dcflush(SB), $-4			/* dcflush(virtaddr, count) */
	MOVW	M(STATUS), R10
	WAIT
	MOVW	4(FP), R9
	MOVW	$0, M(STATUS)
	WAIT
	ADDU	R1, R9			/* R9 = last address */
	MOVW	$(~0x3f), R8
	AND	R1, R8			/* R8 = first address, rounded down */
	ADDU	$0x3f, R9
	AND	$(~0x3f), R9		/* round last address up */
	SUBU	R8, R9			/* R9 = revised count */
dcflush1:				/* primary cache line is 16 bytes */
	CACHE	PI+HI, 0x00(R8)
	CACHE	PI+HI, 0x10(R8)
	CACHE	PI+HI, 0x20(R8)
	CACHE	PI+HI, 0x30(R8)
	CACHE	PD+HWBI, 0x00(R8)
	CACHE	PD+HWBI, 0x10(R8)
	CACHE	PD+HWBI, 0x20(R8)
	CACHE	PD+HWBI, 0x30(R8)
	SUBU	$0x40, R9
	ADDU	$0x40, R8
	BGTZ	R9, dcflush1
	MOVW	R10, M(STATUS)
	WAIT
	RET

TEXT	cleancache(SB), $-4
	MOVW	$KZERO, R1
	MOVW	M(STATUS), R10
	WAIT
	MOVW	$0, M(STATUS)
	WAIT
	MOVW	$(32*1024), R9
ccache:
	CACHE	PD+IWBI, 0x00(R1)
	WAIT
	CACHE	PI+IWBI, 0x00(R1)
	WAIT
	SUBU	$16, R9
	ADDU	$16, R1
	BGTZ	R9, ccache
	MOVW	R10, M(STATUS)
	WAIT
	MOVW	M(CONFIG), R1
	OR	$((1<<4)|(1<<5)), R1
	MOVW	R1, M(CONFIG)
	RET
	
TEXT	getcallerpc(SB), $0
	MOVW	0(SP), R1
	RET

TEXT	rdcount(SB), $0
	MOVW	M(COUNT), R1
	NOOP
	RET

TEXT	wrcompare(SB), $0
	MOVW	R1, M(COMPARE)
	RET

TEXT	uvld(SB), $-4		/* uvld(address, dst) */
	MOVW	4(FP), R2
	MOVV	0(R1), R5
	MOVV	R5, 0(R2)
	RET

TEXT	uvst(SB), $-4		/* uvst(address, src) */
	MOVW	4(FP), R2
	MOVV	0(R2), R5
	MOVV	R5, 0(R1)
	RET

TEXT	fwblock(SB), $-4	/* fwblock(void*port, void *block, csum) */
	MOVW	4(FP), R2
	MOVW	8(FP), R6

	MOVW	$32, R4
fwloop:
	MOVV	0(R2), R5
	MOVV	R5, 0(R1)
	XOR	R5, R6
	MOVV	8(R2), R5
	MOVV	R5, 0(R1)
	XOR	R5, R6

	ADD	$16, R2
	SUB	$1, R4
	BNE	R4, fwloop

	MOVW	R6, R1
	SRLV	$32, R6
	XOR	R6, R1
	RET

TEXT	frblock(SB), $-4	/* frblock(void*port, void *block, csum) */
	MOVW	4(FP), R2
	MOVW	8(FP), R6

	MOVW	$32, R4
frloop:
	MOVV	0(R1), R5
	MOVV	R5, 0(R2)
	XOR	R5, R6
	MOVV	0(R1), R5
	MOVV	R5, 8(R2)
	XOR	R5, R6
	ADD	$16, R2
	SUB	$1, R4
	BNE	R4, frloop

	MOVW	R6, R1
	SRLV	$32, R6
	XOR	R6, R1
	RET
