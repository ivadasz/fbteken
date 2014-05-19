PROG=	fbteken
SRCS=	fbteken.c rop32.c
HDRS=	fbdraw.h
MAN=	fbteken.1

CFLAGS+=	-I/usr/include/libdrm
CFLAGS+=	-I/usr/include/libkms
CFLAGS+=	-I/usr/include/freetype2
CFLAGS+=	-I/home/vadaszi/teken

LDADD+=	-L/home/vadaszi/teken/libteken -Wl,-rpath=/home/vadaszi/teken/libteken
LDADD+=	-lpthread -lutil -lkms -ldrm -lfreetype -lteken

.include <bsd.prog.mk>
