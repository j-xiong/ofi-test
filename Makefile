#######################################################################
# 	Ping-Pong Tests
# 	for
# 	Open Fabric Interface 1.x
#
#	Jianxin Xiong
# 	(jianxin.xiong@intel.com)
# 	2013-2017
#######################################################################

OFI_HOME ?= /usr/local/ofi
CFLAGS    = -I$(OFI_HOME)/include -g
LDFLAGS   = -L$(OFI_HOME)/lib -Xlinker -R$(OFI_HOME)/lib -lfabric

TARGETS=pingpong pingpong-sep pingpong-sep-mt pingpong-self
all: $(TARGETS)

pingpong: pingpong.c Makefile
	cc pingpong.c -o pingpong $(CFLAGS) $(LDFLAGS)

pingpong-sep: pingpong-sep.c Makefile
	cc pingpong-sep.c -o pingpong-sep $(CFLAGS) $(LDFLAGS)

pingpong-sep-mt: pingpong-sep-mt.c Makefile
	cc pingpong-sep-mt.c -o pingpong-sep-mt $(CFLAGS) $(LDFLAGS) -lpthread

pingpong-self: pingpong-self.c Makefile
	cc pingpong-self.c -o pingpong-self $(CFLAGS) $(LDFLAGS)

clean:
	rm $(TARGETS)
