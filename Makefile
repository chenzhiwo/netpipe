PROG_NAME=netpipe

CC=gcc
CFLAGS=-O0 -Wall
LDFLAGS=

.PHONY:clean rebuild exec debug

all:$(PROG_NAME)

clean:
	@echo "Cleaning workspace.........."
	-rm ./*.o ./$(PROG_NAME)

rebuild:clean all

exec:$(PROG_NAME)
	./$(PROG_NAME)

debug:CFLAGS+=-g
debug:rebuild all
	gdb ./$(PROG_NAME)

$(PROG_NAME):netpipe.o
	$(CC) $^ -o $@ $(CFLAGS) $(LDFLAGS)

