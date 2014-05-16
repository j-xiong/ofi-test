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
#include <rdma/fi_rma.h>

#define MIN_MSG_SIZE        (1)
#define MAX_MSG_SIZE        (1<<22)
#define ALIGN               (1<<12)
#define ERROR_MSG(name,err) fprintf(stderr,"%s: %s\n", name, strerror(-(err)))

static char			*sbuf, *rbuf;
static char			*server_name = NULL;
static struct fi_info		*fi_info;
static struct fid_fabric	*fabricfd;
static struct fid_domain	*domainfd;
static struct fid_ep		*epfd;
static struct fid_eq		*eqfd;
static struct fid_av		*avfd;
static struct fid_mr		*smrfd, *rmrfd;
static int			client = 0;
static struct sockaddr_in	bound_addr;
static size_t			bound_addrlen = sizeof(bound_addr);
static void			*direct_addr;
static int			opt_bidir = 0;
static struct fi_context	sctxt, rctxt;
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
	struct fi_eq_attr	eq_attr;
	struct fi_av_attr	av_attr;
	struct fi_resource	resources[2];
	int 			err;

	memset(&addr, 0, sizeof(addr));
	memset(&hints, 0, sizeof(hints));
	memset(&eq_attr, 0, sizeof(eq_attr));
	memset(&av_attr, 0, sizeof(av_attr));

	addr.sin_port = 0;

	hints.type = FID_RDM;
	hints.protocol = FI_PROTO_UNSPEC;
	hints.protocol_cap = FI_PROTO_CAP_MSG | FI_PROTO_CAP_RMA;
//	setting these flags would require passing "struct fi_context *" to the messaging calls
//	hints.flags = FI_BUFFERED_RECV | FI_CANCEL;
//	hints.src_addr = (struct sockaddr *) &addr;
//	hints.src_addrlen = sizeof(struct sockaddr_in);

	if (err = fi_getinfo(server_name, NULL, &hints, &fi_info)) {
		ERROR_MSG("fi_getinfo", err);
		exit(1);
	}

        if (err = fi_fabric(fi_info->fabric_name, 0, &fabricfd, NULL)) {
                ERROR_MSG("fi_fabric", err);
                exit(1);
        }

	if (err = fi_fdomain(fabricfd, fi_info, &domainfd, NULL)) {
		ERROR_MSG("fi_domain", err);
		exit(1);
	}

	eq_attr.domain = FI_EQ_DOMAIN_COMP;
	eq_attr.format = FI_EQ_FORMAT_TAGGED;
	eq_attr.size = 100;

	if (err = fi_eq_open(domainfd, &eq_attr, &eqfd, NULL)) {
		ERROR_MSG("fi_eq_open", err);
		exit(1);
	}

	av_attr.mask = FI_AV_ATTR_TYPE;
	av_attr.type = FI_AV_MAP;

	if (err = fi_av_open(domainfd, &av_attr, &avfd, NULL)) {
		ERROR_MSG("fi_av_open", err);
		exit(1);
	}

	if (err = fi_endpoint(domainfd, &fi_info[0], &epfd, NULL)) {
		ERROR_MSG("fi_endpoint", err);
		exit(1);
	}

	resources[0].fid = (fid_t)eqfd;
	resources[0].flags = FI_SEND | FI_RECV;
	resources[1].fid = (fid_t)avfd;
	resources[1].flags = 0;

	if (err = fi_bind((fid_t)epfd, resources, 2)) {
		ERROR_MSG("fi_bind", err);
		exit(1);
	}

	if (err = fi_getname((fid_t)epfd, &bound_addr, &bound_addrlen)) {
		ERROR_MSG("fi_getname", err);
		exit(1);
	}
}

static void get_peer_address(void)
{
	struct sockaddr_in		partner_addr;
	struct fi_eq_tagged_entry	entry;
	int				completed, err;

	if (client) {
		if (!fi_info[0].dest_addr) {
			fprintf(stderr, "couldn't get server address\n");
			exit(1);
		}
		memcpy(&partner_addr, fi_info[0].dest_addr, fi_info[0].dest_addrlen);

		if (err = fi_av_map(avfd, &partner_addr, 1, &direct_addr, 0)) {
			ERROR_MSG("fi_av_map", err);
			exit(1);
		}

		if (err = fi_av_sync(avfd, 0, NULL)) {
			ERROR_MSG("fi_av_sync", err);
			exit(1);
		}

		if (fi_sendto(epfd, &bound_addr, bound_addrlen, NULL, direct_addr, &sctxt) < 0) {
			perror("fi_sendto");
			exit(1);
		}

		while (! (completed = fi_eq_readfrom(eqfd, &entry, sizeof(entry), NULL, 0)))
			;

		if (completed < 0) {
			ERROR_MSG("fi_eq_readfrom", completed);
			exit(1);
		}

	} else {
		if (fi_recvfrom(epfd, &partner_addr, sizeof(partner_addr), NULL, NULL, &rctxt) < 0) {
			perror("fi_recvfrom");
			exit(1);
		}

		while (! (completed = fi_eq_readfrom(eqfd, &entry, sizeof(entry), NULL, 0)))
			;

		if (completed < 0) {
			ERROR_MSG("fi_eq_readfrom", completed);
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
	struct fi_eq_tagged_entry entry;
	struct rdma_info my_rdma_info;
	int completed, ret;
	struct fi_resource fids;

	if (fi_mr_reg(domainfd, sbuf, MAX_MSG_SIZE, -1, 0, 0, &smrfd, NULL)) {
		perror("fi_mr_reg");
		exit(1);
	}

	if (fi_mr_reg(domainfd, rbuf, MAX_MSG_SIZE, -1, 0, 0, &rmrfd, NULL)) {
		perror("fi_mr_reg");
		exit(1);
	}

	fids.fid = (fid_t)eqfd;
	fids.flags = 0;
	if (fi_bind((fid_t)smrfd, &fids, 1)) {
		perror("fi_mr_bind");
		exit(1);
	}

	if (fi_bind((fid_t)rmrfd, &fids, 1)) {
		perror("fi_mr_bind");
		exit(1);
	}

	my_rdma_info.sbuf_addr = (uint64_t)sbuf;
	my_rdma_info.sbuf_key = (uint64_t)smrfd; /* FIXME: get the MR key */
	my_rdma_info.rbuf_addr = (uint64_t)rbuf;
	my_rdma_info.rbuf_key = (uint64_t)rmrfd; /* FIXME: get the MR key */

	printf("my rdma info: saddr=%llx skey=%llx raddr=%llx rkey=%llx\n",
		my_rdma_info.sbuf_addr, my_rdma_info.sbuf_key,
		my_rdma_info.rbuf_addr, my_rdma_info.rbuf_key);

	if (fi_sendto(epfd, &my_rdma_info, sizeof(my_rdma_info), NULL, direct_addr, &sctxt) < 0) {
		perror("fi_sendto");
		exit(1);
	}

	if (fi_recvfrom(epfd, &peer_rdma_info, sizeof(peer_rdma_info), NULL, NULL, &rctxt) < 0) {
		perror("fi_recvfrom");
		exit(1);
	}

	completed = 0;
	while (completed < 2) {
		ret = fi_eq_readfrom(eqfd, &entry, sizeof(entry), NULL, 0);
		if (ret < 0) {
			ERROR_MSG("fi_eq_readfrom", ret);
			exit(1);
		}
		completed += ret / sizeof(entry);
	}

	printf("peer rdma info: saddr=%llx skey=%llx raddr=%llx rkey=%llx\n",
		peer_rdma_info.sbuf_addr, peer_rdma_info.sbuf_key,
		peer_rdma_info.rbuf_addr, peer_rdma_info.rbuf_key);

}

static void sync(void)
{
	struct fi_eq_tagged_entry entry;
	int dummy, dummy2;
	int completed, ret;

	if (fi_sendto(epfd, &dummy, sizeof(dummy), NULL, direct_addr, &sctxt) < 0) {
		perror("fi_sendto");
		exit(1);
	}

	if (fi_recvfrom(epfd, &dummy2, sizeof(dummy2), NULL, NULL, &rctxt) < 0) {
		perror("fi_recvfrom");
		exit(1);
	}

	completed = 0;
	while (completed < 2) {
		ret = fi_eq_readfrom(eqfd, &entry, sizeof(entry), NULL, 0);
		if (ret < 0) {
			ERROR_MSG("fi_eq_readfrom", ret);
			exit(1);
		}
		completed += ret / sizeof(entry);
	}

	printf("====================== sync =======================\n");
}

static void write_one(int size)
{
	struct fi_eq_tagged_entry	entry;
	void				*src_addr;
	size_t				src_addrlen = sizeof(void *);
	int				completed;
	int				ret;

	if ((ret = fi_rdma_writeto(epfd, sbuf, size, NULL, direct_addr,
					peer_rdma_info.rbuf_addr,
					peer_rdma_info.rbuf_key, 
					&sctxt)) < 0) {
		ERROR_MSG("fi_writeto", ret);
		exit(1);
	}

	while (!(completed = fi_eq_readfrom(eqfd, (void *) &entry, sizeof(entry), &src_addr, &src_addrlen)))
		;

	if (completed < 0) {
		ERROR_MSG("fi_eq_read", completed);
		exit(1);
	}

	assert(entry.op_context == &sctxt);
	//printf("W: %d\n", size);
}

static void read_one(int size)
{
	struct fi_eq_tagged_entry	entry;
	void				*src_addr;
	size_t				src_addrlen = sizeof(void *);
	int				completed;
	int				ret;

	if ((ret = fi_rdma_readfrom (epfd, rbuf, size, NULL, direct_addr,
					peer_rdma_info.sbuf_addr,
					peer_rdma_info.sbuf_key,
					&rctxt)) < 0) {
		ERROR_MSG("fi_readfrom", ret);
		exit(1);
	}

	while (!(completed = fi_eq_readfrom(eqfd, (void *) &entry, sizeof(entry), &src_addr, &src_addrlen)))
		;

	if (completed < 0) {
		ERROR_MSG("fi_eq_read", completed);
		exit(1);
	}

	assert(entry.op_context == &rctxt);
	assert(entry.len == size);
	//printf("R: %d\n", size);
}

static inline void poll_one(int size)
{
	volatile char *p = rbuf + size - 1;
	while (*p != 'a')
		fi_progress(domainfd);
	//printf("P: %d\n", size);
}

static inline void reset_one(int size)
{
	rbuf[size-1] = 'b';
}

static inline wait_one(void)
{
	struct fi_eq_tagged_entry	entry;
	int completed;

	while (!(completed = fi_eq_readfrom(eqfd, (void *) &entry, sizeof(entry), NULL, 0)))
		;

	if (completed < 0) {
		ERROR_MSG("fi_eq_read", completed);
		exit(1);
	}

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
	int repeat, i, n;

	if (argc > 1) {
		if (strcmp(argv[1], "-bidir")==0) {
			opt_bidir=1;
			argc--;
			argv++;
		}
	}

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
		n = size >> 16;
		while (n) {
			repeat >>= 1;
			n >>= 1;
		}

		printf("write %-8d (x %4d): ", size, repeat);
		fflush(stdout);
		t1 = when();
		for (i=0; i<repeat; i++) {
			if (client) {
				write_one(size);
				//poll_one(size);
				//reset_one(size);
				if (opt_bidir)
					wait_one();
			}
			else {
				wait_one();
				 if (opt_bidir) {
					//poll_one(size);
					//reset_one(size);
					write_one(size);
				}
			}
		}
		t2 = when();
		t = (t2 - t1) / repeat;
		printf("%8.2lf us, %8.2lf MB/s\n", t, size/t);
	}

	sync();

rrr:
	for (size = MIN_MSG_SIZE; size <= MAX_MSG_SIZE; size = size << 1) {
		repeat = 1000;
		n = size >> 16;
		while (n) {
			repeat >>= 1;
			n >>= 1;
		}

		printf("read  %-8d (x %4d): ", size, repeat);
		fflush(stdout);
		t1 = when();
		for (i=0; i<repeat; i++) {
			if (client || opt_bidir) {
				//reset_one(size);
				read_one(size);
				//poll_one(size);
			}
		}
		t2 = when();
		t = (t2 - t1) / repeat;
		printf("%8.2lf us, %8.2lf MB/s\n", t, size/t);
	}
	
	sync();

	sleep(1);

	return 0;
}
