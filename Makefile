PROG=	fbteken
SRCS=	fbteken.c rop32.c
HDRS=	fbdraw.h
MAN=

CFLAGS+=	-I/usr/local/include
CFLAGS+=	-I/usr/local/include/libdrm
CFLAGS+=	-I/usr/local/include/libkms
CFLAGS+=	-I/usr/local/include/freetype2
#CFLAGS+=	-I/home/vadaszi/teken

#LDADD+=	-L/home/vadaszi/teken/libteken -Wl,-rpath=/home/vadaszi/teken/libteken
LDADD+=	-L/usr/local/lib
LDADD+=	-lpthread -lutil -lkms -ldrm -lfreetype -lteken

.include <bsd.prog.mk>
