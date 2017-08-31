CC=gcc
CFLAGS=-Wall -std=c99
LFLAGS=-pthread

worklist: worklist.c common.h
	${CC} ${CFLAGS} -c worklist.c ${LFLAGS}
hthpool: worklist hthpool.c worklist.h common.h
	${CC} ${CFLAGS} -c hthpool.c ${LFLAGS}

clean:
	@rm *.o *.so -f
