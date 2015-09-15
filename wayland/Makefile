PROG=	wayteken
SRCS=	wayteken.c
MAN=

CFLAGS+=	-I/usr/local/include

WARNS?=	6

LDADD+=	-L/usr/local/lib
LDADD+=	-lpthread -lutil -levent -lwayland-client -lxkbcommon

.include <bsd.prog.mk>
