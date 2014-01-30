#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_rdma.h>
#include "fi_missing.h"

#define MIN_MSG_SIZE        (1)
#define MAX_MSG_SIZE        (1<<22)
#define ALIGN               (1<<12)
#define ERROR_MSG(name,err) fprintf(stderr,"%s: %s\n", name, strerror(-(err)))

static char			*sbuf, *rbuf;
static char			*server_name = NULL;
static struct fi_info		*fi_info;
static fid_t			epfd, domainfd, ecfd, avfd;
static int			client = 0;
static struct sockaddr_in	bound_addr;
static size_t			bound_addrlen = sizeof(bound_addr);
static void			*direct_addr;
static struct rdma_info {
	uint64_t	sbuf_addr;
	uint64_t	sbuf_key;
	uint64_t	rbuf_addr;
	uint64_t	rbuf_key;
} peer_rdma_info;

static void init_buffer(void)
{
	if (posix_memalign((void *) &sbuf, ALIGN, MAX_MSG_SIZE)) {
		fprintf(stderr, "No memory\n");
		exit(1);
	}

	if (posix_memalign((void *) &rbuf, ALIGN, MAX_MSG_SIZE)) {
		fprintf(stderr, "No memory\n");
		exit(1);
	}

	memset(sbuf, 'a', MAX_MSG_SIZE);
	memset(rbuf, 'b', MAX_MSG_SIZE);
}

static void init_fabric(void)
{
	struct sockaddr_in	addr;
	struct fi_info		hints;
	struct fi_ec_attr	ec_attr;
	struct fi_av_attr	av_attr;
	struct fi_resource	resources[2];
	int 			err;

	memset(&addr, 0, sizeof(addr));
	memset(&hints, 0, sizeof(hints));
	memset(&ec_attr, 0, sizeof(ec_attr));
	memset(&av_attr, 0, sizeof(av_attr));

	addr.sin_port = 0;

	hints.type = FID_RDM;
	hints.protocol = FI_PROTO_UNSPEC;
	hints.protocol_cap = FI_PROTO_CAP_MSG | FI_PROTO_CAP_RDMA;
//	setting these flags would require passing "struct fi_context *" to the messaging calls
//	hints.flags = FI_BUFFERED_RECV | FI_CANCEL;
	hints.flags = 0;
	hints.src_addr = (struct sockaddr *) &addr;
	hints.src_addrlen = sizeof(struct sockaddr_in);

	if (err = fi_getinfo(server_name, NULL, &hints, &fi_info)) {
		ERROR_MSG("fi_getinfo", err);
		exit(1);
	}

	if (err = fi_domain(fi_info, &domainfd, NULL)) {
		ERROR_MSG("fi_domain", err);
		exit(1);
	}

	ec_attr.domain = FI_EC_DOMAIN_COMP;
	ec_attr.type = FI_EC_QUEUE;
	ec_attr.format = FI_EC_FORMAT_TAGGED;
	ec_attr.size = 100;

	if (err = fi_ec_open(domainfd, &ec_attr, &ecfd, NULL)) {
		ERROR_MSG("fi_ec_open", err);
		exit(1);
	}

	av_attr.mask = FI_AV_ATTR_TYPE | FI_AV_ATTR_ADDR_FORMAT;
	av_attr.type = FI_AV_MAP;
	av_attr.addr_format = FI_ADDR;

	if (err = fi_av_open(domainfd, &av_attr, &avfd, NULL)) {
		ERROR_MSG("fi_av_open", err);
		exit(1);
	}

	if (err = fi_endpoint(&fi_info[0], &epfd, NULL)) {
		ERROR_MSG("fi_endpoint", err);
		exit(1);
	}

	resources[0].fid = ecfd;
	resources[0].flags = FI_SEND | FI_RECV;
	resources[1].fid = avfd;
	resources[1].flags = 0;

	if (err = fi_bind(epfd, resources, 2)) {
		ERROR_MSG("fi_bind", err);
		exit(1);
	}

	if (err = fi_getepname(epfd, &bound_addr, &bound_addrlen)) {
		ERROR_MSG("fi_getsockname", err);
		exit(1);
	}
}

static void get_peer_address(void)
{
	struct sockaddr_in		partner_addr;
	struct fi_ec_tagged_entry	entry;
	int				completed, err;

	if (client) {
		if (!fi_info[0].dst_addr) {
			fprintf(stderr, "couldn't get server address\n");
			exit(1);
		}
		memcpy(&partner_addr, fi_info[0].dst_addr, fi_info[0].dst_addrlen);

		if (err = fi_av_map(avfd, &partner_addr, 1, &direct_addr, 0)) {
			ERROR_MSG("fi_av_map", err);
			exit(1);
		}

		if (err = fi_av_sync(avfd, 0, NULL)) {
			ERROR_MSG("fi_av_sync", err);
			exit(1);
		}

		if (fi_sendto(epfd, &bound_addr, bound_addrlen, direct_addr, sbuf) < 0) {
			perror("fi_sendto");
			exit(1);
		}

		while (! (completed = fi_ec_readfrom(ecfd, &entry, sizeof(entry), NULL, 0)))
			;

		if (completed < 0) {
			ERROR_MSG("fi_ec_readfrom", completed);
			exit(1);
		}

	} else {
		if (fi_recvfrom(epfd, &partner_addr, sizeof(partner_addr), NULL, rbuf) < 0) {
			perror("fi_recvfrom");
			exit(1);
		}

		while (! (completed = fi_ec_readfrom(ecfd, &entry, sizeof(entry), NULL, 0)))
			;

		if (completed < 0) {
			ERROR_MSG("fi_ec_readfrom", completed);
			exit(1);
		}

		if (err = fi_av_map(avfd, &partner_addr, 1, &direct_addr, 0)) {
			ERROR_MSG("fi_av_map", err);
			exit(1);
		}

		if (err = fi_av_sync(avfd, 0, NULL)) {
			ERROR_MSG("fi_av_sync", err);
			exit(1);
		}
	}
}

static void exchange_info(void)
{
	struct fi_ec_tagged_entry entry;
	struct rdma_info my_rdma_info;
	int completed, ret;

	my_rdma_info.sbuf_addr = (uint64_t)sbuf;
	my_rdma_info.sbuf_key = (uint64_t)1;
	my_rdma_info.rbuf_addr = (uint64_t)rbuf;
	my_rdma_info.rbuf_key = (uint64_t)2;

	printf("my rdma info: saddr=%llx skey=%llx raddr=%llx rkey=%llx\n",
		my_rdma_info.sbuf_addr, my_rdma_info.sbuf_key,
		my_rdma_info.rbuf_addr, my_rdma_info.rbuf_key);

	if (fi_sendto(epfd, &my_rdma_info, sizeof(my_rdma_info), direct_addr, &my_rdma_info) < 0) {
		perror("fi_sendto");
		exit(1);
	}

	if (fi_recvfrom(epfd, &peer_rdma_info, sizeof(peer_rdma_info), NULL, &peer_rdma_info) < 0) {
		perror("fi_recvfrom");
		exit(1);
	}

	completed = 0;
	while (completed < 2) {
		ret = fi_ec_readfrom(ecfd, &entry, sizeof(entry), NULL, 0);
		if (ret < 0) {
			ERROR_MSG("fi_ec_readfrom", ret);
			exit(1);
		}
		completed += ret;
	}

	printf("peer rdma info: saddr=%llx skey=%llx raddr=%llx rkey=%llx\n",
		peer_rdma_info.sbuf_addr, peer_rdma_info.sbuf_key,
		peer_rdma_info.rbuf_addr, peer_rdma_info.rbuf_key);

}

static void sync(void)
{
	struct fi_ec_tagged_entry entry;
	int dummy, dummy2;
	int completed, ret;

	if (fi_sendto(epfd, &dummy, sizeof(dummy), direct_addr, &dummy) < 0) {
		perror("fi_sendto");
		exit(1);
	}

	if (fi_recvfrom(epfd, &dummy2, sizeof(dummy2), NULL, &dummy2) < 0) {
		perror("fi_recvfrom");
		exit(1);
	}

	completed = 0;
	while (completed < 2) {
		ret = fi_ec_readfrom(ecfd, &entry, sizeof(entry), NULL, 0);
		if (ret < 0) {
			ERROR_MSG("fi_ec_readfrom", ret);
			exit(1);
		}
		completed += ret;
	}

	printf("================== sync ==================\n");
}

static void write_one(int size)
{
	struct fi_ec_tagged_entry	entry;
	void				*src_addr;
	size_t				src_addrlen = sizeof(void *);
	int				completed;
	int				ret;

	if ((ret = fi_rdma_writeto(epfd, sbuf, size, direct_addr,
					peer_rdma_info.rbuf_addr,
					peer_rdma_info.rbuf_key, 
					sbuf)) < 0) {
		ERROR_MSG("fi_writeto", ret);
		exit(1);
	}

	while (!(completed = fi_ec_readfrom(ecfd, (void *) &entry, sizeof(entry), &src_addr, &src_addrlen)))
		;

	if (completed < 0) {
		ERROR_MSG("fi_ec_read", completed);
		exit(1);
	}

	assert(entry.op_context == sbuf);
	//printf("W: %d\n", size);
}

static void read_one(int size)
{
	struct fi_ec_tagged_entry	entry;
	void				*src_addr;
	size_t				src_addrlen = sizeof(void *);
	int				completed;
	int				ret;

	if ((ret = fi_rdma_readfrom (epfd, rbuf, size, direct_addr,
					peer_rdma_info.sbuf_addr,
					peer_rdma_info.sbuf_key,
					rbuf)) < 0) {
		ERROR_MSG("fi_readfrom", ret);
		exit(1);
	}

	while (!(completed = fi_ec_readfrom(ecfd, (void *) &entry, sizeof(entry), &src_addr, &src_addrlen)))
		;

	if (completed < 0) {
		ERROR_MSG("fi_ec_read", completed);
		exit(1);
	}

	assert(entry.op_context == rbuf);
	assert(entry.len == size);
	//printf("R: %d\n", size);
}

static inline void poll_one(int size)
{
	volatile char *p = rbuf + size - 1;
	while (*p != 'a')
		;
	//printf("P: %d\n", size);
}

static inline void reset_one(int size)
{
	rbuf[size-1] = 'b';
}

static double when(void)
{
	struct timeval tv;
	static struct timeval tv0;
	static int first = 1;
	int err;

	err = gettimeofday(&tv, NULL);
	if (err) {
		perror("gettimeofday");
		return 0;
	}

	if (first) {
		tv0 = tv;
		first = 0;
	}
	return (double)(tv.tv_sec - tv0.tv_sec) * 1.0e6 + (double)(tv.tv_usec - tv0.tv_usec); 
//	return (double)tv.tv_sec * 1.0e6 + (double)tv.tv_usec; 
}

int main(int argc, char *argv[])
{
	int size;
	double t1, t2, t;
	int repeat, i;

	if (argc > 1) {
		client = 1;
		server_name = strdup(argv[1]);
	}

	init_buffer();

	init_fabric();

	get_peer_address();

	exchange_info();

	sync();

	for (size = MIN_MSG_SIZE; size <= MAX_MSG_SIZE; size = size << 1) {
		repeat = 1000;
		printf("write %-8d: ", size);
		fflush(stdout);
		t1 = when();
		for (i=0; i<repeat; i++) {
			if (client) {
				write_one(size);
				//poll_one(size);
				//reset_one(size);
			}
			else {
				//poll_one(size);
				//reset_one(size);
				write_one(size);
			}
		}
		t2 = when();
		t = (t2 - t1) / repeat;
		printf("%8.2lf us, %8.2lf MB/s\n", t, size/t);
	}

	sync();

	for (size = MIN_MSG_SIZE; size <= MAX_MSG_SIZE; size = size << 1) {
		repeat = 1000;
		printf("read  %-8d: ", size);
		fflush(stdout);
		t1 = when();
		for (i=0; i<repeat; i++) {
			//reset_one(size);
			read_one(size);
			//poll_one(size);
		}
		t2 = when();
		t = (t2 - t1) / repeat;
		printf("%8.2lf us, %8.2lf MB/s\n", t, size/t);
	}
	
	sync();

	return 0;
}
