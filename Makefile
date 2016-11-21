CC=gcc
CFLAGS=-pthread -I/usr/include/PCSC -c -std=gnu99
LDFLAGS=-lpcsclite
INSTALL=install
 
all: main.o main
 
main: main.o
	$(CC) main.o $(LDFLAGS) -o mcrw
 
main.o: main.c
	$(CC) $(CFLAGS) main.c
 
clean:
	rm -rf *.o mcrw

install:
	$(INSTALL) mcrw /usr/bin/
