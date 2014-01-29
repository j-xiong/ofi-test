SFI=/home/jxiong/sfi
CFLAGS=-I$(SFI)/include
LDFLAGS=-L$(SFI)/lib -Xlinker -R$(SFI)/lib -lfabric

all: pingpong rdma

pingpong: pingpong.c Makefile fi_missing.h
	cc pingpong.c -o pingpong $(CFLAGS) $(LDFLAGS)

rdma: rdma.c Makefile fi_missing.h
	cc rdma.c -o rdma $(CFLAGS) $(LDFLAGS)

