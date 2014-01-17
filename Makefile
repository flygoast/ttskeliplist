CC = gcc
CFLAGS  = -fPIC -Wall -Werror -pipe  -g -D_GNU_SOURCE -D_REENTRANT
LDFLAGS = -Wl,--rpath=. -Wl,-e,__ttskeliplist_main
BIN     = /usr/local/bin/
LIB 	= -ltokyocabinet -lpthread -rdynamic
INC     =
OO	= ttskeliplist.o
TARGETS = ttskeliplist.so
all: $(TARGETS)

$(TARGETS): $(OO)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OO) -shared -o $@ $(LIBDIR) $(LIB)

install:
	install $(TARGETS) $(BIN)

.c.o:
	$(CC) $(CFLAGS) $< -c -o $@ $(INC)

clean:
	rm -f *.o
	rm -f $(TARGETS)
