CC=gcc
CFLAGS=-pthread -I/usr/include/PCSC -c -std=c99
LDFLAGS=-lpcsclite
 
all: main.o main
 
main: main.o
	$(CC) main.o $(LDFLAGS) -o mcrw
 
main.o: main.c
	$(CC) $(CFLAGS) main.c
 
clean:
	rm -rf *.o mcrw
