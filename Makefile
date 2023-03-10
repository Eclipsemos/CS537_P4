CC     := gcc
CFLAGS := -Wall -Werror 

SRCS   := client.c server.c server-fs.c 

OBJS   := ${SRCS:c=o}
PROGS  := ${SRCS:.c=}

.PHONY: all
all: ${PROGS} compile
export LD_LIBRARY_PATH=.
${PROGS} : % : %.o Makefile.net
	${CC} $< -o $@ udp.c

%.o: %.c Makefile
	${CC} ${CFLAGS} -c $<

compile: libmfs.so
	gcc -o mkfs mkfs.c -Wall
	gcc -o main main.c -Wall -L. -lmfs -g
	./mkfs -f test.img
	ldd main

export:
	LD_LIBRARY_PATH=.

libmfs.so: libmfs.o udp.c
	${CC} ${CFLAGS} -fpic -c udp.c
	gcc -shared -Wl,-soname,libmfs.so -o libmfs.so libmfs.o udp.o -lc	

libmfs.o: libmfs.c
	gcc -fPIC -g -c -Wall libmfs.c

clean:
	rm -r *.o
	rm ./main
	rm ./libmfs.so
	rm ./mkfs
	rm -f ${PROGS} ${OBJS}

visualize: compile
	./mkfs -f test.img -v