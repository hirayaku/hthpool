CC=gcc
CFLAGS=-Wall -std=c99
LFLAGS=-pthread
SRC_DIR=..

hthpool: ${SRC_DIR}/hthpool.c ${SRC_DIR}/common.h
	${CC} ${CFLAGS} -c ${SRC_DIR}/hthpool.c ${LFLAGS}
example: hthpool ${SRC_DIR}/hthpool.h example.c
	${CC} ${CFLAGS} example.c hthpool.o ${LFLAGS} -o example
	@rm *.o

clean:
	@rm *.o -f
