CC=gcc
CFLAGS=-Wall -std=c99
LFLAGS=-pthread

hthpool: hthpool.c common.h
	${CC} ${CFLAGS} -c hthpool.c ${LFLAGS}
example: hthpool hthpool.h example.c
	${CC} ${CFLAGS} example.c hthpool.o ${LFLAGS}

clean:
	@rm *.o *.so -f
