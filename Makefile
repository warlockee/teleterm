UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    CC = clang
    CFLAGS = -Wall -O2 -mmacosx-version-min=14.0
    FRAMEWORKS = -framework CoreGraphics -framework CoreFoundation \
                 -framework CoreServices -framework ApplicationServices
    BACKEND = backend_macos.o
else
    CC = gcc
    CFLAGS = -Wall -O2
    FRAMEWORKS =
    BACKEND = backend_tmux.o
endif

LIBS = -lcurl -lsqlite3
OBJS = bot_common.o $(BACKEND) botlib.o sds.o cJSON.o sqlite_wrap.o json_wrap.o qrcodegen.o sha1.o

all: teleterm

teleterm: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(FRAMEWORKS) $(LIBS)

bot_common.o: bot_common.c botlib.h sds.h backend.h
	$(CC) $(CFLAGS) -c bot_common.c

backend_macos.o: backend_macos.c backend.h sds.h botlib.h
	$(CC) $(CFLAGS) -c backend_macos.c

backend_tmux.o: backend_tmux.c backend.h sds.h
	$(CC) $(CFLAGS) -c backend_tmux.c

botlib.o: botlib.c botlib.h sds.h cJSON.h sqlite_wrap.h
	$(CC) $(CFLAGS) -c botlib.c

sds.o: sds.c sds.h sdsalloc.h
	$(CC) $(CFLAGS) -c sds.c

cJSON.o: cJSON.c cJSON.h
	$(CC) $(CFLAGS) -c cJSON.c

sqlite_wrap.o: sqlite_wrap.c sqlite_wrap.h
	$(CC) $(CFLAGS) -c sqlite_wrap.c

json_wrap.o: json_wrap.c cJSON.h
	$(CC) $(CFLAGS) -c json_wrap.c

qrcodegen.o: qrcodegen.c qrcodegen.h
	$(CC) $(CFLAGS) -c qrcodegen.c

sha1.o: sha1.c sha1.h
	$(CC) $(CFLAGS) -c sha1.c

clean:
	rm -f teleterm *.o

.PHONY: all clean
