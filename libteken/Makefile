# $FreeBSD: head/sys/teken/libteken/Makefile 221698 2011-05-09 16:27:39Z ed $

LIB=	teken

INTERNALLIB=	YES

CFLAGS+=	-I${.OBJDIR}

WARNS?=	5

SRCS=	teken.c teken_state.h
CLEANFILES=	teken_state.h

MAN=

teken_state.h: ${.CURDIR}/gensequences ${.CURDIR}/sequences
	awk -f ${.CURDIR}/gensequences ${.CURDIR}/sequences > ${.TARGET}

.include <bsd.lib.mk>
