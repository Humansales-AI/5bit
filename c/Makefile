# 5bit C Engine — Ground Truth for Python & TypeScript
CC = cc
CFLAGS = -O2 -Wall
LDFLAGS = -lssl -lcrypto

all: codec encode lib grid

codec: fivebit_codec.c
	$(CC) $(CFLAGS) -o fivebit_codec fivebit_codec.c

encode: fivebit_encode.c
	$(CC) $(CFLAGS) -o fivebit_encode fivebit_encode.c

lib: fivebit_lib.c fivebit_grid.c
	$(CC) $(CFLAGS) -shared -fPIC -o libfivebit.so fivebit_lib.c fivebit_grid.c $(LDFLAGS)

grid: fivebit_grid.c fivebit_grid.h
	$(CC) $(CFLAGS) -c fivebit_grid.c -o fivebit_grid.o

test: test_grid.c fivebit_grid.c fivebit_grid.h
	$(CC) $(CFLAGS) -o test_grid test_grid.c fivebit_grid.c $(LDFLAGS)

clean:
	rm -f fivebit_codec fivebit_encode libfivebit.so fivebit_grid.o test_grid
