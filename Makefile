PROG_NAME=netpipe

CC=gcc
CFLAGS=-O0 -Wall
LDFLAGS=
#CFLAGS+=-g

.PHONY:clean rebuild exec

all:$(PROG_NAME)

clean:
	@echo "Cleaning workspace.........."
	-rm ./*.o ./$(PROG_NAME)

rebuild:clean all


exec:$(PROG_NAME)
	./$(PROG_NAME)

$(PROG_NAME):netpipe.o
	$(CC) $^ -o $@ $(CFLAGS) $(LDFLAGS)



