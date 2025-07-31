CFLAGS = -g -Wall -lm #-lglib-2.0
INCLUDE = -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include/
OBJS = \
	   d_htable.o \
	   lru_list.o \
	   skiplist.o \
	   valueset.o \
	   sha256.o \
	   pqueue.o \
	   random/rand.o \
	   random/zipf.o \
	   murmurhash.o \
	   rte_ring/rte_ring.o

.PHONY: all

all: $(OBJS)

%.o: %.c
	$(CC) -c -o $@ $< $(INCLUDE) $(CFLAGS)

d_htable.o: d_htable.c d_htable.h ../dftl_utils.h

lru_list.o: lru_list.c lru_list.h

pqueue.o: pqueue.c pqueue.h

sha256.o: sha256.c sha256.h

skiplist.o: skiplist.c skiplist.h valueset.h ../dftl_utils.h ../dftl_types.h

valueset.o: valueset.c valueset.h ../dftl_types.h

clean:
	rm -f *.o **/*.o *.out **/*.out
