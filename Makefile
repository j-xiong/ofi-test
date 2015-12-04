OFI_HOME=/home/jxiong/install/ofi-ext
CFLAGS=-I$(OFI_HOME)/include -g
LDFLAGS=-L$(OFI_HOME)/lib -Xlinker -R$(OFI_HOME)/lib -lfabric -lrdmacm

TARGETS=pingpong
all: $(TARGETS)

pingpong: pingpong.c Makefile
	cc pingpong.c -o pingpong $(CFLAGS) $(LDFLAGS)

clean:
	rm $(TARGETS)
