#ifdef _FI_ENDPOINT_H_
static inline ssize_t fi_recvfrom(fid_t fid, void *buf, size_t len,
		const void *src_addr, void *context) 
{
	struct fid_ep *ep = container_of(fid, struct fid_ep, fid);
	FI_ASSERT_CLASS(fid, FID_CLASS_EP);
	FI_ASSERT_OPS(fid, struct fid_ep, msg);
	FI_ASSERT_OP(ep->msg, struct fi_ops_msg, recvfrom);
	return ep->msg->recvfrom(fid, buf, len, src_addr, context); 
}
#endif

#ifdef _FI_RDMA_H_
static inline int fi_rdma_writeto(fid_t fid, const void *buf, size_t len, 
                              const void *dst_addr, uint64_t addr, be64_t key,
                              void *context)
{
        struct fid_ep *ep = container_of(fid, struct fid_ep, fid);
        FI_ASSERT_CLASS(fid, FID_CLASS_EP);
        FI_ASSERT_OPS(fid, struct fid_ep, rdma);
        FI_ASSERT_OP(ep->rdma, struct fi_ops_rdma, writeto);
        return ep->rdma->writeto(fid, buf, len, 
                        dst_addr, addr, key, context);
}
 
static inline int fi_rdma_readfrom(fid_t fid, void *buf, size_t len,
                const void *src_addr, uint64_t addr, be64_t key, void *context)
{
        struct fid_ep *ep = container_of(fid, struct fid_ep, fid);
        FI_ASSERT_CLASS(fid, FID_CLASS_EP);
        FI_ASSERT_OPS(fid, struct fid_ep, rdma);
        FI_ASSERT_OP(ep->rdma, struct fi_ops_rdma, readfrom);
        return ep->rdma->readfrom(fid, buf, len, 
                        src_addr, addr,key, context);
}
#endif
