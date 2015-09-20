PROG=	fbteken
SRCS=	fbteken.c ../src/rop32.c

.if exists(${.OBJDIR}/../libteken)
LIBTEKEN=${.OBJDIR}/../libteken/libteken.a
.else
LIBTEKEN=${.CURDIR}/../libteken/libteken.a
.endif

CFLAGS+=	-I/usr/local/include
CFLAGS+=	-I/usr/local/include/freetype2

WARNS?=	6

LDADD+=	${LIBTEKEN}
LDADD+=	-L/usr/local/lib
LDADD+=	-lpthread -lutil -levent -lwaywin -lfreetype -lxkbcommon

.include <bsd.prog.mk>
