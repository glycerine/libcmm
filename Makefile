CC = g++ -g
LIBNAME = cmm
LIBFILENAME = lib${LIBNAME}.so

HEADERS = src/cmm.h src/cmm_private.h
SOURCES = src/cmm.cpp
OBJECTS = ${SOURCES:.cpp=.o}

#CFLAGS   += -std=c99 -Wall -Werror -march=i686 -fPIC -O2
#CFLAGS   += -std=c99 -Wall -Werror -fpic -O2
#CFLAGS   += -std=c99 -Wall -Werror -fpic -gdwarf-2 -g3
CFLAGS   +=  -Wall -Werror -fPIC -gdwarf-2 -g3
LDFLAGS  += -gdwarf-2 -g3 -fPIC
CPPFLAGS := -gdwarf-2 -g3 -Wall -fPIC

CFLAGS += ${CPPFLAGS}

#SHAREDLIBFLAG = `if [ \`uname\` = Darwin ] ; then echo -n '-dynamiclib' ; else echo -n '-shared' ; fi`
SHAREDLIBFLAG = -shared

${LIBNAME}: ${OBJECTS}
	${CC} ${LDFLAGS} ${OBJECTS} ${SHAREDLIBFLAG} -o ${LIBFILENAME}

${OBJECTS}: ${SOURCES} ${HEADERS}

example: ${OBJECTS}
	${CC} ${CFLAGS} -Isrc demos/top-down-size-splay-cmm.cpp ${OBJECTS} -o example

test1:  ${OBJECTS}
	g++ ${CPPFLAGS} -Isrc -g demos/test1cmm.cpp src/cmm.cpp -o demos/test1


clean:
	rm -f example; find . -name '*.o' -print|xargs rm -f; rm -f libcmm.so
