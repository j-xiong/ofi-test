OFI_HOME=/home/jxiong/install/ofi-ext
CFLAGS=-I$(OFI_HOME)/include -g
LDFLAGS=-L$(OFI_HOME)/lib -Xlinker -R$(OFI_HOME)/lib -lfabric -lrdmacm

TARGETS=pingpong pingpong-sep pingpong-sep-mt
all: $(TARGETS)

pingpong: pingpong.c Makefile
	cc pingpong.c -o pingpong $(CFLAGS) $(LDFLAGS)

pingpong-sep: pingpong-sep.c Makefile
	cc pingpong-sep.c -o pingpong-sep $(CFLAGS) $(LDFLAGS)

pingpong-sep-mt: pingpong-sep-mt.c Makefile
	cc pingpong-sep-mt.c -o pingpong-sep-mt $(CFLAGS) $(LDFLAGS) -lpthread

clean:
	rm $(TARGETS)
