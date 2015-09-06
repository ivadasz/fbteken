PROG=	fbteken
SRCS=	fbteken.c rop32.c
HDRS=	fbdraw.h

.if exists(${.OBJDIR}/../libteken)
LIBTEKEN=${.OBJDIR}/../libteken/libteken.a
.else
LIBTEKEN=${.CURDIR}/../libteken/libteken.a
.endif

.if exists(${.OBJDIR}/../libkbdev)
LIBKBDEV=${.OBJDIR}/../libkbdev/libkbdev.a
.else
LIBKBDEV=${.CURDIR}/../libkbdev/libkbdev.a
.endif

CFLAGS+=	-I/usr/local/include
CFLAGS+=	-I/usr/local/include/libdrm
CFLAGS+=	-I/usr/local/include/freetype2

WARNS?=	6

LDADD+=	${LIBTEKEN}
LDADD+=	${LIBKBDEV}
LDADD+=	-L/usr/local/lib
LDADD+=	-lpthread -lutil -levent -lkms -ldrm -lfreetype -lxkbcommon

.include <bsd.prog.mk>
