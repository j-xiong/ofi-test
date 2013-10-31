SFI=/home/jxiong/sfi
CFLAGS=-I$(SFI)/include
LDFLAGS=-L$(SFI)/lib -Xlinker -R$(SFI)/lib -lfabric

pingpong: pingpong.c Makefile
	cc pingpong.c -o pingpong $(CFLAGS) $(LDFLAGS)

