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

#ifdef _FI_DOMAIN_H_
static inline int fi_progress(fid_t fid)
{
        struct fid_domain *domain = container_of(fid, struct fid_domain, fid);
        FI_ASSERT_CLASS(fid, FID_CLASS_DOMAIN);
        FI_ASSERT_OPS(fid, struct fid_domain, ops);
        FI_ASSERT_OP(domain->ops, struct fi_ops_domain, av_open);
        return domain->ops->progress(fid);
}
#endif

