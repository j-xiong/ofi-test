#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_errno.h>

#define MAX_NUM_CHANNELS    16
#define TEST_MSG	    0
#define TEST_RMA	    1

#define MIN_MSG_SIZE        (1)
#define MAX_MSG_SIZE        (1<<22)
#define ALIGN               (1<<12)
#define ERROR_MSG(name,err) fprintf(stderr,"%s: %s\n", name, strerror(-(err)))
#define MSG_TAG		    (0xFFFF0000FFFF0000ULL)

#define SEND_MSG(ep, buf, len, peer, context)						\
	do {										\
		int err;								\
		if (opt.notag) {							\
			err = fi_send(ep, buf, len, NULL, peer, context);		\
			if (err < 0) {							\
				ERROR_MSG("fi_send", err);				\
				exit(1);						\
			}								\
		}									\
		else {									\
			err = fi_tsend(ep, buf, len, NULL, peer, MSG_TAG, context);	\
			if (err < 0) {							\
				ERROR_MSG("fi_tsend", err);				\
				exit(1);						\
			}								\
		}									\
	} while (0)

#define RECV_MSG(ep, buf, len, peer, context)						\
	do {										\
		int err;								\
		if (opt.notag) {							\
			err = fi_recv(ep, buf, len, NULL, peer, context);		\
			if (err < 0) {							\
				ERROR_MSG("fi_recv", err);				\
				exit(1);						\
			}								\
		}									\
		else {									\
			err = fi_trecv(ep,buf,len,NULL,peer,MSG_TAG,0x0ULL,context);	\
			if (err < 0) {							\
				ERROR_MSG("fi_trecv", err);				\
				exit(1);						\
			}								\
		}									\
	} while (0)

#define WAIT_CQ(cq, n)									\
	do {										\
		struct fi_cq_tagged_entry entry[n];					\
		int ret, completed = 0;							\
		while (completed < n) {							\
			ret = fi_cq_read(cq, entry, n);					\
			if (ret == -FI_EAGAIN)						\
				continue;						\
			if (ret < 0) {							\
				ERROR_MSG("fi_cq_read", ret);				\
				exit(1);						\
			}								\
			completed += ret;						\
		}									\
	} while (0)

static struct {
	int	test_type;
	int	notag;
	int	bidir;
	int	num_ch;
	int	client;
	char	*prov_name;
	char	*server_name;
} opt = { .num_ch = 1 };

struct rma_info {
	uint64_t	sbuf_addr;
	uint64_t	sbuf_key;
	uint64_t	rbuf_addr;
	uint64_t	rbuf_key;
};

static struct fi_info		*fi;
static struct fid_fabric	*fabric;
static struct fid_domain	*domain;
static struct fid_av		*av;

static struct {
	struct fid_ep		*ep;
	struct fid_cq		*cq;
	struct fid_cntr		*cntr;		/* rma only */
	struct fid_mr		*smr;		/* rma only */
	struct fid_mr		*rmr;		/* rma only */
	struct rma_info 	peer_rma_info;	/* rma only */
	fi_addr_t		peer_addr;
	struct fi_context	sctxt;
	struct fi_context	rctxt;
	char			*sbuf;
	char			*rbuf;
} ch[MAX_NUM_CHANNELS];

/****************************
 *	Utility funcitons
 ****************************/

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

static void print_options(void)
{
	printf("test_type = %d (%s)\n", opt.test_type, opt.test_type ? "RMA" : "MSG");
	printf("notag = %d\n", opt.notag);
	printf("bidir = %d\n", opt.bidir);
	printf("num_ch = %d\n", opt.num_ch);
	printf("client = %d\n", opt.client);
	printf("prov_name = %s\n", opt.prov_name);
	printf("server_name = %s\n", opt.server_name);
}

/****************************
 *	Initialization
 ****************************/

static void init_buffer(void)
{
	int i;

	for (i=0; i<opt.num_ch; i++) {
		if (posix_memalign((void *) &ch[i].sbuf, ALIGN, MAX_MSG_SIZE)) {
			fprintf(stderr, "No memory\n");
			exit(1);
		}

		if (posix_memalign((void *) &ch[i].rbuf, ALIGN, MAX_MSG_SIZE)) {
			fprintf(stderr, "No memory\n");
			exit(1);
		}

		memset(ch[i].sbuf, 'a'+i, MAX_MSG_SIZE);
		memset(ch[i].rbuf, 'o'+i, MAX_MSG_SIZE);

		ch[i].sbuf[MAX_MSG_SIZE - 1] = '\0';
		ch[i].rbuf[MAX_MSG_SIZE - 1] = '\0';
	}
}

static void init_fabric(void)
{
	struct fi_info		*hints;
	struct fi_cq_attr	cq_attr;
	struct fi_cntr_attr	cntr_attr;
	struct fi_av_attr	av_attr;
	int 			err;
	int			version;
	int			i;

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
	hints->fabric_attr->prov_name = opt.prov_name;

	if (opt.test_type == TEST_RMA)
		hints->caps |= FI_RMA;
	else if (!opt.notag)
		hints->caps |= FI_TAGGED;

	version = FI_VERSION(1, 0);
	if (err = fi_getinfo(version, opt.server_name, "12345", 
				(opt.client ? 0 : FI_SOURCE), hints, &fi)) {
		ERROR_MSG("fi_getinfo", err);
		exit(1);
	}

	fi_freeinfo(hints);

	printf("Using OFI device: %s\n", fi->fabric_attr->name);

	if (err = fi_fabric(fi->fabric_attr, &fabric, NULL)) {
		ERROR_MSG("fi_fabric", err);
		exit(1);
	}

	if (err = fi_domain(fabric, fi, &domain, NULL)) {
		ERROR_MSG("fi_domain", err);
		exit(1);
	}

	av_attr.type = FI_AV_MAP;

	if (err = fi_av_open(domain, &av_attr, &av, NULL)) {
		ERROR_MSG("fi_av_open", err);
		exit(1);
	}

	for (i=0; i<opt.num_ch; i++) {
		cq_attr.format = FI_CQ_FORMAT_TAGGED;
		cq_attr.size = 100;

		if (err = fi_cq_open(domain, &cq_attr, &ch[i].cq, NULL)) {
			ERROR_MSG("fi_cq_open", err);
			exit(1);
		}

		if (err = fi_endpoint(domain, fi, &ch[i].ep, NULL)) {
			ERROR_MSG("fi_endpoint", err);
			exit(1);
		}

		if (err = fi_ep_bind(ch[i].ep, (fid_t)ch[i].cq, FI_SEND|FI_RECV)) {
			ERROR_MSG("fi_ep_bind cq", err);
			exit(1);
		}

		if (err = fi_ep_bind(ch[i].ep, (fid_t)av, 0)) {
			ERROR_MSG("fi_ep_bind av", err);
			exit(1);
		}

		if (err = fi_enable(ch[i].ep)) {
			ERROR_MSG("fi_enable", err);
			exit(1);
		}

		if (opt.test_type != TEST_RMA)
			continue;

		if (err = fi_mr_reg(domain, ch[i].sbuf, MAX_MSG_SIZE, FI_REMOTE_READ,
				0, i+i+1, 0, &ch[i].smr, NULL)) {
			ERROR_MSG("fi_mr_reg", err);
			exit(1);
		}

		if (err = fi_mr_reg(domain, ch[i].rbuf, MAX_MSG_SIZE, FI_REMOTE_WRITE,
				0, i+i+2, 0, &ch[i].rmr, NULL)) {
			ERROR_MSG("fi_mr_reg", err);
			exit(1);
		}

		if (err = fi_cntr_open(domain, &cntr_attr, &ch[i].cntr, NULL)) {
			ERROR_MSG("fi_cntr_open", err);
			exit(1);
		}

		if (err = fi_mr_bind(ch[i].rmr, (fid_t)ch[i].cntr, FI_REMOTE_WRITE)) {
			ERROR_MSG("fi_mr_bind", err);
			exit(1);
		}
	}
}

static finalize_fabric(void)
{
	int i;

	for (i=0; i<opt.num_ch; i++) {
		if (opt.test_type == TEST_RMA) {
			fi_close((fid_t)ch[i].cntr);
			fi_close((fid_t)ch[i].rmr);
			fi_close((fid_t)ch[i].smr);
		}

		fi_close((fid_t)ch[i].ep);
		fi_close((fid_t)ch[i].cq);
	}

	fi_close((fid_t)av);
	fi_close((fid_t)domain);
	fi_close((fid_t)fabric);
	fi_freeinfo(fi);
}

static void get_peer_address(void)
{
	struct { char raw[16]; }	bound_addr, partner_addr;
	size_t				bound_addrlen;
	int				err;
	int				i;

	if (opt.client) {
		/* get the address of peer channel 0 */
		if (!fi->dest_addr) {
			fprintf(stderr, "couldn't get server address\n");
			exit(1);
		}
		memcpy(&partner_addr, fi->dest_addr, fi->dest_addrlen);

		if ((err = fi_av_insert(av, &partner_addr, 1, &ch[0].peer_addr,
					0, NULL)) != 1) {
			ERROR_MSG("fi_av_insert", err);
			exit(1);
		}

		/* send all my local addresses to peer channel 0 */
		for (i=0; i<opt.num_ch; i++) {
			bound_addrlen = sizeof(bound_addr);
			if (err = fi_getname((fid_t)ch[i].ep, &bound_addr,
						&bound_addrlen)) {
				ERROR_MSG("fi_getname", err);
				exit(1);
			}

			SEND_MSG(ch[0].ep, &bound_addr, bound_addrlen,
					ch[0].peer_addr, &ch[0].sctxt);

			WAIT_CQ(ch[0].cq, 1);
		}

		/* receive peer addresses except channel 0 */
		for (i=1; i<opt.num_ch; i++) {
			RECV_MSG(ch[i].ep, &partner_addr, sizeof(partner_addr),
				 0, &ch[i].rctxt);

			WAIT_CQ(ch[i].cq, 1);

			if ((err = fi_av_insert(av, &partner_addr, 1, &ch[i].peer_addr,
						0, NULL)) != 1) {
				ERROR_MSG("fi_av_insert", err);
				exit(1);
			}
		}
	} else {
		/* receive all peer addresses from channel 0 */
		for (i=0; i<opt.num_ch; i++) {
			RECV_MSG(ch[0].ep, &partner_addr, sizeof(partner_addr),
				 0, &ch[0].rctxt);

			WAIT_CQ(ch[0].cq, 1);

			if ((err = fi_av_insert(av, &partner_addr, 1, &ch[i].peer_addr,
						0, NULL)) != 1) {
				ERROR_MSG("fi_av_insert", err);
				exit(1);
			}
		}

		/* send all my local addresses (except channle 0) to peer */
		for (i=1; i<opt.num_ch; i++) {
			bound_addrlen = sizeof(bound_addr);
			if (err = fi_getname((fid_t)ch[i].ep, &bound_addr,
						&bound_addrlen)) {
				ERROR_MSG("fi_getname", err);
				exit(1);
			}

			SEND_MSG(ch[i].ep, &bound_addr, bound_addrlen,
					ch[i].peer_addr, &ch[i].sctxt);

			WAIT_CQ(ch[i].cq, 1);
		}
	}
}

/****************************
 *	MSG Test
 ****************************/

static void send_one(int size)
{
	int i;

	for (i=0; i<opt.num_ch; i++)
		SEND_MSG(ch[i].ep, ch[i].sbuf, size, ch[i].peer_addr, &ch[i].sctxt);

	for (i=0; i<opt.num_ch; i++)
		WAIT_CQ(ch[i].cq, 1);
}

static void recv_one(int size)
{
	int i;

	for (i=0; i<opt.num_ch; i++)
		RECV_MSG(ch[i].ep, ch[i].rbuf, size, ch[i].peer_addr, &ch[i].rctxt);

	for (i=0; i<opt.num_ch; i++)
		WAIT_CQ(ch[i].cq, 1);
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
			if (opt.client) {
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
	struct rma_info my_rma_info;
	int i;

	for (i=0; i<opt.num_ch; i++) {

		if (fi->domain_attr->mr_mode == FI_MR_SCALABLE) {
			ch[i].peer_rma_info.sbuf_addr = 0ULL;
			ch[i].peer_rma_info.sbuf_key = (uint64_t)(i+i+1);
			ch[i].peer_rma_info.rbuf_addr = 0ULL;
			ch[i].peer_rma_info.rbuf_key = (uint64_t)(i+i+2);
			continue;
		}

		my_rma_info.sbuf_addr = (uint64_t)ch[i].sbuf;
		my_rma_info.sbuf_key = fi_mr_key(ch[i].smr);
		my_rma_info.rbuf_addr = (uint64_t)ch[i].rbuf;
		my_rma_info.rbuf_key = fi_mr_key(ch[i].rmr);

		printf("my rma info: saddr=%llx skey=%llx raddr=%llx rkey=%llx\n",
			my_rma_info.sbuf_addr, my_rma_info.sbuf_key,
			my_rma_info.rbuf_addr, my_rma_info.rbuf_key);

		SEND_MSG(ch[i].ep, &my_rma_info, sizeof(my_rma_info),
				ch[i].peer_addr, &ch[i].sctxt);

		RECV_MSG(ch[i].ep, &ch[i].peer_rma_info, sizeof(ch[i].peer_rma_info),
				0, &ch[i].rctxt);

		WAIT_CQ(ch[i].cq, 2);

		printf("peer rma info [%d]: saddr=%llx skey=%llx raddr=%llx rkey=%llx\n", i,
			ch[i].peer_rma_info.sbuf_addr, ch[i].peer_rma_info.sbuf_key,
			ch[i].peer_rma_info.rbuf_addr, ch[i].peer_rma_info.rbuf_key);
	}
}

static void synchronize(void)
{
	int dummy, dummy2;
	int i;

	for (i=0; i<opt.num_ch; i++) {
		SEND_MSG(ch[i].ep, &dummy, sizeof(dummy), ch[i].peer_addr, &ch[i].sctxt);
		RECV_MSG(ch[i].ep, &dummy2, sizeof(dummy2), 0, &ch[i].rctxt);
		WAIT_CQ(ch[i].cq, 2);
	}

	printf("====================== sync =======================\n");
}

static void write_one(int size)
{
	int ret;
	int i;

	for (i=0; i<opt.num_ch; i++) {
		if ((ret = fi_write(ch[i].ep, ch[i].sbuf, size, NULL, ch[i].peer_addr,
						ch[i].peer_rma_info.rbuf_addr,
						ch[i].peer_rma_info.rbuf_key, 
						&ch[i].sctxt)) < 0) {
			ERROR_MSG("fi_write", ret);
			exit(1);
		}

		WAIT_CQ(ch[i].cq, 1);
	}
}

static void read_one(int size)
{
	int ret;
	int i;

	for (i=0; i<opt.num_ch; i++) {
		if ((ret = fi_read(ch[i].ep, ch[i].rbuf, size, NULL, ch[i].peer_addr,
						ch[i].peer_rma_info.sbuf_addr,
						ch[i].peer_rma_info.sbuf_key,
						&ch[i].rctxt)) < 0) {
			ERROR_MSG("fi_readfrom", ret);
			exit(1);
		}

		WAIT_CQ(ch[i].cq, 1);
	}
}

static inline void poll_one(int size)
{
	int i;

	for (i=0; i<opt.num_ch; i++) {
		volatile char *p = ch[i].rbuf + size - 1;
		while (*p != ('a'+i))
			fi_cq_read(ch[i].cq, NULL, 0);
		//printf("P: %d\n", size);
	}
}

static inline void reset_one(int size)
{
	int i;

	for (i=0; i<opt.num_ch; i++)
		ch[i].rbuf[size-1] = 'o' + i;
}

static inline wait_one(void)
{
	static uint64_t completed[MAX_NUM_CHANNELS];
	uint64_t counter;
	int i;

	for (i=0; i<opt.num_ch; i++) {
		while (1) {
			counter = fi_cntr_read(ch[i].cntr);
			if (counter > completed[i])
				break;
		}
		completed[i]++;
	}
}

static void run_rma_test(void)
{
	int size;
	double t1, t2, t;
	int repeat, i, n;

	exchange_rma_info();

	synchronize();

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
			if (opt.client) {
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

	synchronize();

rrr:
	if (opt.client || opt.bidir) {
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
	
	synchronize();
}

/****************************
 *	Main
 ****************************/

void print_usage(void)
{
	printf("Usage: pingpong [-b][-c <num_channels>][-f <provider>][-t <test_type>]"
		" [server_name]\n"); 
	printf("Options:\n");
	printf("\t-b\t\t\tbidirectional test (RMA test only)\n");
	printf("\t-c <num_channels>\ttest over multiple channels concurrently\n");
	printf("\t-f <provider>\t\tuse the specific provider\n");
	printf("\t-t <test_type>\t\tperform the spcified test, <test_type> can be:\n");
	printf("\t\t\t\tmsg ------- non-tagged send/receive\n");
	printf("\t\t\t\ttagged ---- tagged send/receive\n");
	printf("\t\t\t\trma ------- RMA read/write\n");
}

int main(int argc, char *argv[])
{
	int c;

	while ((c = getopt(argc, argv, "bc:f:t:")) != -1) {
		switch (c) {
		case 'b':
			opt.bidir = 1;
			break;

		case 'c':
			opt.num_ch = atoi(optarg);
			break;

		case 'f':
			opt.prov_name = strdup(optarg);
			break;

		case 't':
			if (strcmp(optarg, "msg") == 0) {
				opt.test_type = TEST_MSG;
				opt.notag = 1;
			}
			else if (strcmp(optarg, "tagged") == 0) {
				opt.test_type = TEST_MSG;
				opt.notag = 0;
			}
			else if (strcmp(optarg, "rma") == 0) {
				opt.test_type = TEST_RMA;
				opt.notag = 1;
			}
			else {
				print_usage();
				exit(1);
			}
			break;

		default:
			print_usage();
			exit(1);
			break;
		}
	}

	if (argc > optind) {
		opt.client = 1;
		opt.server_name = strdup(argv[optind]);
	}

	print_options();
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

	finalize_fabric();

	return 0;
}

