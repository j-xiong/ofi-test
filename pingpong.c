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

static char			*sbuf, *rbuf;
static char			*server_name = NULL;
static struct fi_info		*fi_info;
static fid_t			epfd, domainfd, ecfd, avfd;
static int			client = 0;
static struct sockaddr_in	bound_addr;
static size_t			bound_addrlen = sizeof(bound_addr);
static void			*direct_addr;

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
	hints.protocol_cap = FI_PROTO_CAP_TAGGED;
	hints.flags = FI_BUFFERED_RECV | FI_CANCEL;
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

		if (fi_tsendto(epfd, &bound_addr, bound_addrlen, direct_addr, 0x0ULL, sbuf) < 0) {
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
		if (fi_trecvfrom(epfd, &partner_addr, sizeof(partner_addr), NULL, 0x0ULL, 0x0ULL, rbuf) < 0) {
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

static void send_one(int size)
{
	struct fi_ec_tagged_entry	entry;
	void				*src_addr;
	size_t				src_addrlen = sizeof(void *);
	int				completed;
	int				ret;

	if ((ret = fi_tsendto(epfd, sbuf, size, direct_addr, 0x0ULL, sbuf)) < 0) {
		ERROR_MSG("fi_sendto", ret);
		exit(1);
	}

	while (!(completed = fi_ec_readfrom(ecfd, (void *) &entry, sizeof(entry), &src_addr, &src_addrlen)))
		;

	if (completed < 0) {
		ERROR_MSG("fi_ec_read", completed);
		exit(1);
	}

	assert(entry.op_context == sbuf);
	printf("S: %d\n", size);
}

static void recv_one(int size)
{
	struct fi_ec_tagged_entry	entry;
	void				*src_addr;
	size_t				src_addrlen = sizeof(void *);
	int				completed;
	int				ret;

	if ((ret = fi_trecvfrom (epfd, rbuf, size, direct_addr, 0x0ULL, 0x0ULL, rbuf)) < 0) {
		ERROR_MSG("fi_recvfrom", ret);
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
	printf("R: %d\n", size);
}

int main(int argc, char *argv[])
{
	int size;

	if (argc > 1) {
		client = 1;
		server_name = strdup(argv[1]);
	}

	init_buffer();

	init_fabric();

	get_peer_address();

	if (client) {
		for (size = MIN_MSG_SIZE; size <= MAX_MSG_SIZE; size = size << 1) {
			recv_one(size);
			send_one(size);
		}
	} else {
		for (size = MIN_MSG_SIZE; size <= MAX_MSG_SIZE; size = size << 1) {
			send_one(size);
			recv_one(size);
		}
	}

	return 0;
}
