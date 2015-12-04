#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
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
#include <rdma/fi_tagged.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_errno.h>

#define MIN_MSG_SIZE        (1)
#define MAX_MSG_SIZE        (1<<22)
#define ALIGN               (1<<12)
#define ERROR_MSG(name,err) fprintf(stderr,"%s: %s\n", name, strerror(-(err)))
#define MSG_TAG		    (0xFFFF0000FFFF0000ULL)

static char			*sbuf, *rbuf;
static char			*server_name = NULL;
static struct fi_info		*fi;
static struct fid_fabric	*fabricfd;
static struct fid_domain	*domainfd;
static struct fid_ep		*epfd;
static struct fid_cq		*cqfd;
static struct fid_av		*avfd;
static int			client = 0;
static struct sockaddr_in	bound_addr;
static size_t			bound_addrlen = sizeof(bound_addr);
static fi_addr_t		direct_addr;
static struct fi_context	sctxt, rctxt;

/* RMA */
static struct fid_cntr		*mrcntrfd;
static struct fid_mr		*smrfd, *rmrfd;
static struct rma_info {
	uint64_t	sbuf_addr;
	uint64_t	sbuf_key;
	uint64_t	rbuf_addr;
	uint64_t	rbuf_key;
} peer_rma_info;

#define TEST_MSG	0
#define TEST_RMA	1

static struct {
	int test_type;
	int notag;
	int bidir;
} opt = {
	.test_type = TEST_MSG,
	.notag = 0,
	.bidir = 0,
};

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

	sbuf[MAX_MSG_SIZE - 1] = '\0';
	rbuf[MAX_MSG_SIZE - 1] = '\0';
}

static void init_fabric(void)
{
	struct fi_info		*hints;
	struct fi_cq_attr	cq_attr;
	struct fi_cntr_attr	cntr_attr;
	struct fi_av_attr	av_attr;
	int 			err;
	int			version;

	hints = fi_allocinfo();
	if (!hints) {
		ERROR_MSG("fi_allocinfo", -ENOMEM);
		exit(-1);
	}

	memset(&cq_attr, 0, sizeof(cq_attr));
	memset(&cntr_attr, 0, sizeof(cntr_attr));
	memset(&av_attr, 0, sizeof(av_attr));

	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_MSG;
	hints->mode = FI_CONTEXT;

	if (opt.test_type == TEST_RMA)
		hints->caps |= FI_RMA;
	else if (!opt.notag)
		hints->caps |= FI_TAGGED;

	version = FI_VERSION(1, 0);
	if (err = fi_getinfo(version, server_name, NULL, 0, hints, &fi)) {
		ERROR_MSG("fi_getinfo", err);
		exit(1);
	}

	fi_freeinfo(hints);

	printf("Using OFI device: %s\n", fi->fabric_attr->name);

	if (err = fi_fabric(fi->fabric_attr, &fabricfd, NULL)) {
		ERROR_MSG("fi_fabric", err);
		exit(1);
	}

	if (err = fi_domain(fabricfd, fi, &domainfd, NULL)) {
		ERROR_MSG("fi_domain", err);
		exit(1);
	}

	cq_attr.format = FI_CQ_FORMAT_TAGGED;
	cq_attr.size = 100;

	if (err = fi_cq_open(domainfd, &cq_attr, &cqfd, NULL)) {
		ERROR_MSG("fi_cq_open", err);
		exit(1);
	}

	if (opt.test_type == TEST_RMA) {
		if (err = fi_cntr_open(domainfd, &cntr_attr, &mrcntrfd, NULL)) {
			ERROR_MSG("fi_cntr_open", err);
			exit(1);
		}
	}

	av_attr.type = FI_AV_MAP;

	if (err = fi_av_open(domainfd, &av_attr, &avfd, NULL)) {
		ERROR_MSG("fi_av_open", err);
		exit(1);
	}

	if (err = fi_endpoint(domainfd, fi, &epfd, NULL)) {
		ERROR_MSG("fi_endpoint", err);
		exit(1);
	}

	if (err = fi_ep_bind(epfd, (fid_t)cqfd, FI_SEND|FI_RECV)) {
		ERROR_MSG("fi_ep_bind cq", err);
		exit(1);
	}

	if (err = fi_ep_bind(epfd, (fid_t)avfd, 0)) {
		ERROR_MSG("fi_ep_bind av", err);
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
	struct fi_cq_tagged_entry	entry;
	int				completed, err;

	if (client) {
		if (!fi->dest_addr) {
			fprintf(stderr, "couldn't get server address\n");
			exit(1);
		}
		memcpy(&partner_addr, fi->dest_addr, fi->dest_addrlen);

		if ((err = fi_av_insert(avfd, &partner_addr, 1, &direct_addr, 0, NULL)) != 1) {
			ERROR_MSG("fi_av_insert", err);
			exit(1);
		}

		if (opt.notag) {
			err = fi_send(epfd, &bound_addr, bound_addrlen, NULL, direct_addr, &sctxt);
			if (err < 0) {
				ERROR_MSG("fi_send", err);
				exit(1);
			}
		}
		else {
			err = fi_tsend(epfd, &bound_addr, bound_addrlen, NULL, direct_addr, MSG_TAG, &sctxt);
			if (err < 0) {
				ERROR_MSG("fi_tsend", err);
				exit(1);
			}
		}

		while ((completed = fi_cq_read(cqfd, &entry, 1)) == -FI_EAGAIN)
			;

		if (completed < 0) {
			ERROR_MSG("fi_cq_read", completed);
			exit(1);
		}

	} else {
		if (opt.notag) {
			err = fi_recv(epfd, &partner_addr, sizeof(partner_addr), NULL, 0, &rctxt);
			if (err < 0) {
				ERROR_MSG("fi_recv", err);
				exit(1);
			}
		}
		else {
			err = fi_trecv(epfd, &partner_addr, sizeof(partner_addr), NULL, 0, MSG_TAG, 0x0ULL, &rctxt);
			if (err < 0) {
				ERROR_MSG("fi_trecv", err);
				exit(1);
			}
		}

		while ((completed = fi_cq_read(cqfd, &entry, 1)) == -FI_EAGAIN)
			;

		if (completed < 0) {
			ERROR_MSG("fi_cq_read", completed);
			exit(1);
		}

		if ((err = fi_av_insert(avfd, &partner_addr, 1, &direct_addr, 0, NULL)) != 1) {
			ERROR_MSG("fi_av_insert", err);
			exit(1);
		}
	}
}

static void send_one(int size)
{
	struct fi_cq_tagged_entry	entry;
	void				*src_addr;
	size_t				src_addrlen = sizeof(void *);
	int				completed;
	int				ret;

	if (opt.notag) {
		if ((ret = fi_send(epfd, sbuf, size, NULL, direct_addr, &sctxt)) < 0) {
			ERROR_MSG("fi_send", ret);
			exit(1);
		}
	}
	else {
		if ((ret = fi_tsend(epfd, sbuf, size, NULL, direct_addr, MSG_TAG, &sctxt)) < 0) {
			ERROR_MSG("fi_tsend", ret);
			exit(1);
		}
	}

	while ((completed = fi_cq_read(cqfd, (void *) &entry, 1)) == -FI_EAGAIN)
		;

	if (completed < 0) {
		ERROR_MSG("fi_cq_read", completed);
		exit(1);
	}

	assert(entry.op_context == &sctxt);
//	printf("S: %d\n", size);
}

static void recv_one(int size)
{
	struct fi_cq_tagged_entry	entry;
	void				*src_addr;
	size_t				src_addrlen = sizeof(void *);
	int				completed;
	int				ret;

	if (opt.notag) {
		if ((ret = fi_recv(epfd, rbuf, size, NULL, direct_addr, &rctxt)) < 0) {
			ERROR_MSG("fi_recv", ret);
			exit(1);
		}
	}
	else {
		if ((ret = fi_trecv(epfd, rbuf, size, NULL, direct_addr, MSG_TAG, 0x0ULL, &rctxt)) < 0) {
			ERROR_MSG("fi_trecv", ret);
			exit(1);
		}
	}

	while ((completed = fi_cq_read(cqfd, (void *) &entry, 1)) == -FI_EAGAIN)
		;

	if (completed < 0) {
		ERROR_MSG("fi_cq_read", completed);
		exit(1);
	}

	assert(entry.op_context == &rctxt);
	assert(entry.len == size);
//	printf("R: %d\n", size);
}

static void run_msg_test(void)
{
	int size;
	int i, n, repeat;
	double t1, t2, t;

	for (size = MIN_MSG_SIZE; size <= MAX_MSG_SIZE; size = size << 1) {
		repeat = 1000;
		n = size >> 16;
		while (n) {
			repeat >>= 1;
			n >>= 1;
		}

		printf("send/recv %-8d (x %4d): ", size, repeat);
		fflush(stdout);
		t1 = when();
		for (i=0; i<repeat; i++) {
			if (client) {
				recv_one(size);
				send_one(size);
			}
			else {
				send_one(size);
				recv_one(size);
			}
		}
		t2 = when();
		t = (t2 - t1) / repeat / 2;
		printf("%8.2lf us, %8.2lf MB/s\n", t, size/t);
	}
}

/****************************
 *	RMA Test
 ****************************/
static void exchange_rma_info(void)
{
	struct fi_cq_tagged_entry entry[2];
	struct rma_info my_rma_info;
	int completed, ret;
	int err;

	err = fi_mr_reg(domainfd, sbuf, MAX_MSG_SIZE, FI_REMOTE_READ, 0, 1, 0, &smrfd, NULL);
	if (err) {
		ERROR_MSG("fi_mr_reg", err);
		exit(1);
	}

	err = fi_mr_reg(domainfd, rbuf, MAX_MSG_SIZE, FI_REMOTE_WRITE, 0, 2, 0, &rmrfd, NULL);
	if (err) {
		ERROR_MSG("fi_mr_reg", err);
		exit(1);
	}

	err = fi_mr_bind(smrfd, (fid_t)mrcntrfd, 0);
	if (err) {
		ERROR_MSG("fi_mr_bind", err);
		exit(1);
	}

	err = fi_mr_bind(rmrfd, (fid_t)mrcntrfd, 0);
	if (err) {
		ERROR_MSG("fi_mr_bind", err);
		exit(1);
	}

	my_rma_info.sbuf_addr = (uint64_t)sbuf;
	my_rma_info.sbuf_key = fi_mr_key(smrfd);
	my_rma_info.rbuf_addr = (uint64_t)rbuf;
	my_rma_info.rbuf_key = fi_mr_key(rmrfd);

	if (fi->domain_attr->mr_mode == FI_MR_SCALABLE) {
		my_rma_info.sbuf_addr = 0ULL;
		my_rma_info.rbuf_addr = 0ULL;
	}

	printf("my rma info: saddr=%llx skey=%llx raddr=%llx rkey=%llx\n",
		my_rma_info.sbuf_addr, my_rma_info.sbuf_key,
		my_rma_info.rbuf_addr, my_rma_info.rbuf_key);

	err = fi_send(epfd, &my_rma_info, sizeof(my_rma_info), NULL, direct_addr, &sctxt);
	if (err < 0) {
		ERROR_MSG("fi_send", err);
		exit(1);
	}

	err = fi_recv(epfd, &peer_rma_info, sizeof(peer_rma_info), NULL, 0, &rctxt);
	if (err < 0) {
		ERROR_MSG("fi_recv", err);
		exit(1);
	}

	completed = 0;
	while (completed < 2) {
		ret = fi_cq_read(cqfd, entry, 2);
		if (ret == -FI_EAGAIN)
			continue;
		if (ret < 0) {
			ERROR_MSG("fi_cq_read", ret);
			exit(1);
		}
		completed += ret;
	}

	printf("peer rma info: saddr=%llx skey=%llx raddr=%llx rkey=%llx\n",
		peer_rma_info.sbuf_addr, peer_rma_info.sbuf_key,
		peer_rma_info.rbuf_addr, peer_rma_info.rbuf_key);
}

static void sync(void)
{
	struct fi_cq_tagged_entry entry[2];
	int dummy, dummy2;
	int completed, ret;
	int err;

	err = fi_send(epfd, &dummy, sizeof(dummy), NULL, direct_addr, &sctxt);
	if (err < 0) {
		ERROR_MSG("fi_send", err);
		exit(1);
	}

	err = fi_recv(epfd, &dummy2, sizeof(dummy2), NULL, 0, &rctxt);
	if (err < 0) {
		ERROR_MSG("fi_recv", err);
		exit(1);
	}

	completed = 0;
	while (completed < 2) {
		ret = fi_cq_read(cqfd, entry, 2);
		if (ret == -FI_EAGAIN)
			continue;
		if (ret < 0) {
			ERROR_MSG("fi_cq_read", ret);
			exit(1);
		}
		completed += ret;
	}

	printf("====================== sync =======================\n");
}

static void write_one(int size)
{
	struct fi_cq_tagged_entry	entry;
	int				completed;
	int				ret;

	if ((ret = fi_write(epfd, sbuf, size, NULL, direct_addr,
					peer_rma_info.rbuf_addr,
					peer_rma_info.rbuf_key, 
					&sctxt)) < 0) {
		ERROR_MSG("fi_write", ret);
		exit(1);
	}

	while ((completed = fi_cq_read(cqfd, (void *) &entry, 1)) == -FI_EAGAIN)
		;

	if (completed < 0) {
		ERROR_MSG("fi_cq_read", completed);
		exit(1);
	}

	assert(entry.op_context == &sctxt);
	//printf("W: %d\n", size);
}

static void read_one(int size)
{
	struct fi_cq_tagged_entry	entry;
	int				completed;
	int				ret;

	if ((ret = fi_read(epfd, rbuf, size, NULL, direct_addr,
					peer_rma_info.sbuf_addr,
					peer_rma_info.sbuf_key,
					&rctxt)) < 0) {
		ERROR_MSG("fi_readfrom", ret);
		exit(1);
	}

	while ((completed = fi_cq_read(cqfd, (void *) &entry, 1)) == -FI_EAGAIN)
		;

	if (completed < 0) {
		ERROR_MSG("fi_cq_read", completed);
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
		fi_cq_read(cqfd, NULL, 0);
	//printf("P: %d\n", size);
}

static inline void reset_one(int size)
{
	rbuf[size-1] = 'b';
}

static inline wait_one(void)
{
	static uint64_t completed = 0;
	uint64_t counter;

	while (1) {
		counter = fi_cntr_read(mrcntrfd);
		if (counter > completed)
			break;
	}

	completed++;
}

static void run_rma_test(void)
{
	int size;
	double t1, t2, t;
	int repeat, i, n;

	exchange_rma_info();

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
				if (opt.bidir)
					wait_one();
			}
			else {
				wait_one();
				 if (opt.bidir) {
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
	if (client || opt.bidir) {
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
				//reset_one(size);
				read_one(size);
				//poll_one(size);
			}
			t2 = when();
			t = (t2 - t1) / repeat;
			printf("%8.2lf us, %8.2lf MB/s\n", t, size/t);
		}
	}
	
	sync();

	sleep(1);
}

int main(int argc, char *argv[])
{
	while (argc > 1) {
		if (argv[1][0] != '-')
			break;

		if (strcmp(argv[1], "-msg")==0) {
			opt.test_type = TEST_MSG;
		}
		else if (strcmp(argv[1], "-rma")==0) {
			opt.test_type = TEST_RMA;
			opt.notag = 1;
		}
		else if (strcmp(argv[1], "-notag")==0) {
			opt.notag = 1;
		}
		else if (strcmp(argv[1], "-bidir")==0) {
			opt.bidir = 1;
		}

		argc--;
		argv++;
	}

	if (argc > 1) {
		client = 1;
		server_name = strdup(argv[1]);
	}

	init_buffer();
	init_fabric();
	get_peer_address();

	switch (opt.test_type) {
	case TEST_MSG:
		run_msg_test();
		break;
	case TEST_RMA:
		run_rma_test();
		break;
	}

	return 0;
}

