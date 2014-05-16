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

#define MIN_MSG_SIZE        (1)
#define MAX_MSG_SIZE        (1<<22)
#define ALIGN               (1<<12)
#define ERROR_MSG(name,err) fprintf(stderr,"%s: %s\n", name, strerror(-(err)))
#define MSG_TAG		    (0xFFFF0000FFFF0000ULL)

static char			*sbuf, *rbuf;
static char			*server_name = NULL;
static struct fi_info		*fi_info;
static struct fid_fabric	*fabricfd;
static struct fid_domain	*domainfd;
static struct fid_ep		*epfd;
static struct fid_eq		*eqfd;
static struct fid_av		*avfd;
static int			client = 0;
static struct sockaddr_in	bound_addr;
static size_t			bound_addrlen = sizeof(bound_addr);
static void			*direct_addr;
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
	hints.protocol_cap = FI_PROTO_CAP_TAGGED | FI_PROTO_CAP_MSG;
	hints.flags = FI_BUFFERED_RECV | FI_CANCEL;
	hints.src_addr = (struct sockaddr *) &addr;
	hints.src_addrlen = sizeof(struct sockaddr_in);

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

		if (opt_notag) {
			if (fi_sendto(epfd, &bound_addr, bound_addrlen, NULL, direct_addr, &sctxt) < 0) {
				perror("fi_sendto");
				exit(1);
			}
		}
		else {
			if (fi_tsendto(epfd, &bound_addr, bound_addrlen, NULL, direct_addr, MSG_TAG, &sctxt) < 0) {
				perror("fi_tsendto");
				exit(1);
			}
		}

		while (! (completed = fi_eq_readfrom(eqfd, &entry, sizeof(entry), NULL, 0)))
			;

		if (completed < 0) {
			ERROR_MSG("fi_eq_readfrom", completed);
			exit(1);
		}

	} else {
		if (opt_notag) {
			if (fi_recvfrom(epfd, &partner_addr, sizeof(partner_addr), NULL, NULL, &rctxt) < 0) {
				perror("fi_recvfrom");
				exit(1);
			}
		}
		else {
			if (fi_trecvfrom(epfd, &partner_addr, sizeof(partner_addr), NULL, NULL, MSG_TAG, 0x0ULL, &rctxt) < 0) {
				perror("fi_trecvfrom");
				exit(1);
			}
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

static void send_one(int size)
{
	struct fi_eq_tagged_entry	entry;
	void				*src_addr;
	size_t				src_addrlen = sizeof(void *);
	int				completed;
	int				ret;

	if (opt_notag) {
		if ((ret = fi_sendto(epfd, sbuf, size, NULL, direct_addr, &sctxt)) < 0) {
			ERROR_MSG("fi_sendto", ret);
			exit(1);
		}
	}
	else {
		if ((ret = fi_tsendto(epfd, sbuf, size, NULL, direct_addr, MSG_TAG, &sctxt)) < 0) {
			ERROR_MSG("fi_tsendto", ret);
			exit(1);
		}
	}

	while (!(completed = fi_eq_readfrom(eqfd, (void *) &entry, sizeof(entry), &src_addr, &src_addrlen)))
		;

	if (completed < 0) {
		ERROR_MSG("fi_eq_read", completed);
		exit(1);
	}

	assert(entry.op_context == &sctxt);
//	printf("S: %d\n", size);
}

static void recv_one(int size)
{
	struct fi_eq_tagged_entry	entry;
	void				*src_addr;
	size_t				src_addrlen = sizeof(void *);
	int				completed;
	int				ret;

	if (opt_notag) {
		if ((ret = fi_recvfrom (epfd, rbuf, size, NULL, direct_addr, &rctxt)) < 0) {
			ERROR_MSG("fi_recvfrom", ret);
			exit(1);
		}
	}
	else {
		if ((ret = fi_trecvfrom (epfd, rbuf, size, NULL, direct_addr, MSG_TAG, 0x0ULL, &rctxt)) < 0) {
			ERROR_MSG("fi_trecvfrom", ret);
			exit(1);
		}
	}

	while (!(completed = fi_eq_readfrom(eqfd, (void *) &entry, sizeof(entry), &src_addr, &src_addrlen)))
		;

	if (completed < 0) {
		ERROR_MSG("fi_eq_read", completed);
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
