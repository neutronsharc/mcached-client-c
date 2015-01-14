
#mpi = /home/ouyangx/mvapich2/mvapich2/install-1.6-sock
#mpi = /home/ouyangx/mvapich2/mvapich2/install-1.8-tcp
#CC = ${mpi}/bin/mpicc -g
mpi = mpicc

libevent = /usr
#/home/ouyangx/tools/libevent-2.0.20
libsasl = /usr
#/home/ouyangx/tools/libsasl
libmemcached = /usr
#libmemcached = /home/ouyangx/tools/libmemcached-0.45
#${PWD}/../
#/usr
##libmemcached = /home/ouyangx/memcached/install-libmem-0.45

#INC = -I/usr/include -I/usr/local/include -I${libevent}/include -I${libsasl}/include -I${libmemcached}/include
#INC = -I${libevent}/include -I${libsasl}/include -I${libmemcached}/include
#INC = -I${libevent}/include -I${libsasl}/include -I${libmemcached}/include/libmemcached

#CFLAGS = -c
CFLAGS = -c ${INC}
LDFLAGS = -g
#-D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
#LDFLAGS = -L${libevent}/lib -L${libsasl}/lib -L/usr/lib

LIBS = -lmemcached -lrt -ldl

#mpicc -DMPI sock-tps.c utilities.c -lmemcached -lpthread -o sock-tps

ifdef mpi
	CC = $(mpi) -g
	CFLAGS += -DMPI
else
	CC = gcc -g
endif

exe = sock-tps

objs = sock-tps.o utilities.o

all : ${exe}

sock-tps : sock-tps.o  utilities.o
	${CC} $^ ${LDFLAGS} ${LIBS} -o $@

.c.o:
	${CC} ${CFLAGS} $^

clean :
	rm -f ${exe} *.o


