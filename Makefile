APPNAME = aex

PREFIX = /usr/local

CFLAGS = -g -O0 -std=c99 -Wall -Wno-switch -I core/include
LDLIBS := -lev -pthread

SRCS = $(shell find . -type f -name "*.c")
HEADERS = $(shell find . -type f -name "*.h")
OBJS = ${SRCS:.c=.o}

${APPNAME}: ${OBJS}
	$(CC) $(CFLAGS) -o $(APPNAME) ${OBJS} $(LDLIBS)

all: $(APPNAME)

install: $(APPNAME)
	install -o root -g root -m 755 ${APPNAME} ${PREFIX}/sbin/

clean:
	-rm ${OBJS} ${APPNAME}
