typedef	unsigned short	ushort;
typedef	unsigned char	uchar;
typedef unsigned long	ulong;
typedef	long		vlong;
typedef union Length	Length;

union Length
{
	char	clength[8];
	vlong	vlength;
	struct{
		long	hlength;
		long	length;
	};
};
