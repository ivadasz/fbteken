PROG=	fbteken
SRCS=	fbteken.c rop32.c
HDRS=	fbdraw.h

.if exists(${.OBJDIR}/../libteken)
LIBTEKEN=${.OBJDIR}/../libteken/libteken.a
.else
LIBTEKEN=${.CURDIR}/../libteken/libteken.a
.endif

CFLAGS+=	-I/usr/pkg/include
CFLAGS+=	-I/usr/local/include
CFLAGS+=	-I/usr/local/include/libdrm
CFLAGS+=	-I/usr/local/include/freetype2

WARNS?=	6

LDADD+=	${LIBTEKEN}
LDADD+=	-L/usr/local/lib -L/usr/pkg/lib
LDADD+=	-lpthread -lutil -levent -ldrm -lfreetype -lxkbcommon -lkbdev

.include <bsd.prog.mk>
