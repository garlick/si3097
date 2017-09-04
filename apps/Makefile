ALL  = si-dump si-test si-image

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-2.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-2.0)
CFLAGS=-g -Wall -Werror -I../driver $(GTK_CFLAGS)
CC=gcc

all: $(ALL)

clean:
	rm -f *.o $(ALL)

si-test: lib.o si-test.o demux.o
	$(CC) -g -o $@ $^

si-image: si-image.o uart.o lib.o demux.o
	$(CC) -g -o $@ $^  $(GTK_LIBS) -lpthread -lm

