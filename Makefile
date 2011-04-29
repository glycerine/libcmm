
LIBNAME = cmm
LIBFILENAME = lib${LIBNAME}.so

HEADERS = src/mm.h src/mm_private.h
SOURCES = src/mm.c
OBJECTS = ${SOURCES:.c=.o}

CFLAGS   += -std=c99 -Wall -Werror -march=i686 -fPIC -O2
LDFLAGS  += -g

CFLAGS += ${CPPFLAGS}

SHAREDLIBFLAG = `if [ \`uname\` = Darwin ] ; then echo -n '-dynamiclib' ; else echo -n '-shared' ; fi`

${LIBNAME}: ${OBJECTS}
	${CC} ${LDFLAGS} ${OBJECTS} ${SHAREDLIBFLAG} -o ${LIBFILENAME}

${OBJECTS}: ${SOURCES} ${HEADERS}

example: demos/top-down-size-splay-mm.c ${OBJECTS}
	${CC} -Isrc demos/top-down-size-splay-mm.c ${OBJECTS} -o example

sl-example: demos/top-down-size-splay-mm.c ${LIBNAME}
	${CC} -Isrc demos/top-down-size-splay-mm.c -l${LIBNAME} -o example
