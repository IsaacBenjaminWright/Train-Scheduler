CC=gcc
CFLAGS=-Wall -std=c18 -g

all: mts 

mts: mts.c
	$(CC) $(CFLAGS) -o mts mts.c
clean:
	rm -f mts 

.PHONY: all clean