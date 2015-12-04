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
static int			opt_notag = 0;
static struct fi_context	sctxt, rctxt;

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
	struct fi_av_attr	av_attr;
	int 			err;
	int			version;

	hints = fi_allocinfo();
	if (!hints) {
		ERROR_MSG("fi_allocinfo", -ENOMEM);
		exit(-1);
	}

	memset(&cq_attr, 0, sizeof(cq_attr));
	memset(&av_attr, 0, sizeof(av_attr));

	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_MSG | FI_TAGGED;
	hints->mode = FI_CONTEXT;

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

		if (opt_notag) {
			if (fi_send(epfd, &bound_addr, bound_addrlen, NULL, direct_addr, &sctxt) < 0) {
				perror("fi_send");
				exit(1);
			}
		}
		else {
			if (fi_tsend(epfd, &bound_addr, bound_addrlen, NULL, direct_addr, MSG_TAG, &sctxt) < 0) {
				perror("fi_tsend");
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
		if (opt_notag) {
			if (fi_recv(epfd, &partner_addr, sizeof(partner_addr), NULL, 0, &rctxt) < 0) {
				perror("fi_recv");
				exit(1);
			}
		}
		else {
			if (fi_trecv(epfd, &partner_addr, sizeof(partner_addr), NULL, 0, MSG_TAG, 0x0ULL, &rctxt) < 0) {
				perror("fi_trecv");
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

	if (opt_notag) {
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

	if (opt_notag) {
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
	int i, n, repeat;
	double t1, t2, t;

	if (argc > 1) {
		if (strcmp(argv[1], "-notag")==0) {
			opt_notag=1;
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

	return 0;
}
