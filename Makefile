CFLAGS=-D_POSIX_SOURCE
OBJ = sim.o worker.o p2.o p3.o p4.o p5.o p6.o
CC=gcc

all:	$(OBJ)
	$(CC) -o sim $(OBJ)

clean:	
	rm -f *.o *.bak sim

sim.o:	common.h protocol.h
worker.o:	common.h protocol.h
p2.o:	protocol.h
p3.o:	protocol.h
p4.o:	protocol.h
p5.o:	protocol.h
p6.o:	protocol.h
