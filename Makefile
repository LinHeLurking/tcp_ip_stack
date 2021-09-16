TARGET = tcp_stack

all: $(TARGET)
	$(shell [ -d server ] || mkdir server)
	$(shell [ -d client ] || mkdir client)
	$(shell cp $(TARGET) server)
	$(shell cp $(TARGET) client)
	$(shell cd client && ../create_randfile.sh)

CC = gcc
LD = gcc

CFLAGS = -g -Wall -Iinclude
LDFLAGS = 

LIBS = -lpthread

HDRS = ./include/*.h

SRCS = arp.c arpcache.c icmp.c ip.c main.c packet.c rtable.c rtable_internal.c \
	   tcp.c tcp_apps.c tcp_in.c tcp_out.c tcp_sock.c tcp_timer.c

OBJS = $(patsubst %.c,%.o,$(SRCS))

$(OBJS) : %.o : %.c include/*.h
	$(CC) -c $(CFLAGS) $< -o $@

$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o $(TARGET) $(LIBS) 

clean:
	rm -f *.o $(TARGET)
	rm -f server/*
	rm -f client/*
