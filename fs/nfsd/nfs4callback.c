/*
 *  Copyright (c) 2001 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Kendrick Smith <kmsmith@umich.edu>
 *  Andy Adamson <andros@umich.edu>
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the University nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 *  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/nfs4.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/xprt.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/slab.h>
#include "nfsd.h"
#include "state.h"
#include "netns.h"
#include "trace.h"
#include "xdr4cb.h"
#include "xdr4.h"
#include "nfs4xdr_gen.h"

#define NFSDDBG_FACILITY                NFSDDBG_PROC

#define NFSPROC4_CB_NULL 0
#define NFSPROC4_CB_COMPOUND 1

/* Index of predefined Linux callback client operations */

struct nfs4_cb_compound_hdr {
	/* args */
	u32		ident;	/* minorversion 0 only */
	u32		nops;
	__be32		*nops_p;
	u32		minorversion;
	/* res */
	int		status;
};

static __be32 *xdr_encode_empty_array(__be32 *p)
{
	*p++ = xdr_zero;
	return p;
}

/*
 * Encode/decode NFSv4 CB basic data types
 *
 * Basic NFSv4 callback data types are defined in section 15 of RFC
 * 3530: "Network File System (NFS) version 4 Protocol" and section
 * 20 of RFC 5661: "Network File System (NFS) Version 4 Minor Version
 * 1 Protocol"
 */

static void encode_uint32(struct xdr_stream *xdr, u32 n)
{
	WARN_ON_ONCE(xdr_stream_encode_u32(xdr, n) < 0);
}

static void encode_bitmap4(struct xdr_stream *xdr, const __u32 *bitmap,
			   size_t len)
{
	xdr_stream_encode_uint32_array(xdr, bitmap, len);
}

static int decode_cb_fattr4(struct xdr_stream *xdr, uint32_t *bitmap,
				struct nfs4_cb_fattr *fattr)
{
	fattr->ncf_cb_change = 0;
	fattr->ncf_cb_fsize = 0;
	fattr->ncf_cb_atime.tv_sec = 0;
	fattr->ncf_cb_atime.tv_nsec = 0;
	fattr->ncf_cb_mtime.tv_sec = 0;
	fattr->ncf_cb_mtime.tv_nsec = 0;

	if (bitmap[0] & FATTR4_WORD0_CHANGE)
		if (xdr_stream_decode_u64(xdr, &fattr->ncf_cb_change) < 0)
			return -EIO;
	if (bitmap[0] & FATTR4_WORD0_SIZE)
		if (xdr_stream_decode_u64(xdr, &fattr->ncf_cb_fsize) < 0)
			return -EIO;
	if (bitmap[2] & FATTR4_WORD2_TIME_DELEG_ACCESS) {
		fattr4_time_deleg_access access;

		if (!xdrgen_decode_fattr4_time_deleg_access(xdr, &access))
			return -EIO;
		fattr->ncf_cb_atime.tv_sec = access.seconds;
		fattr->ncf_cb_atime.tv_nsec = access.nseconds;

	}
	if (bitmap[2] & FATTR4_WORD2_TIME_DELEG_MODIFY) {
		fattr4_time_deleg_modify modify;

		if (!xdrgen_decode_fattr4_time_deleg_modify(xdr, &modify))
			return -EIO;
		fattr->ncf_cb_mtime.tv_sec = modify.seconds;
		fattr->ncf_cb_mtime.tv_nsec = modify.nseconds;

	}
	return 0;
}

static void encode_nfs_cb_opnum4(struct xdr_stream *xdr, enum nfs_cb_opnum4 op)
{
	__be32 *p;

	p = xdr_reserve_space(xdr, 4);
	*p = cpu_to_be32(op);
}

/*
 * nfs_fh4
 *
 *	typedef opaque nfs_fh4<NFS4_FHSIZE>;
 */
static void encode_nfs_fh4(struct xdr_stream *xdr, const struct knfsd_fh *fh)
{
	u32 length = fh->fh_size;
	__be32 *p;

	BUG_ON(length > NFS4_FHSIZE);
	p = xdr_reserve_space(xdr, 4 + length);
	xdr_encode_opaque(p, &fh->fh_raw, length);
}

/*
 * stateid4
 *
 *	struct stateid4 {
 *		uint32_t	seqid;
 *		opaque		other[12];
 *	};
 */
static void encode_stateid4(struct xdr_stream *xdr, const stateid_t *sid)
{
	__be32 *p;

	p = xdr_reserve_space(xdr, NFS4_STATEID_SIZE);
	*p++ = cpu_to_be32(sid->si_generation);
	xdr_encode_opaque_fixed(p, &sid->si_opaque, NFS4_STATEID_OTHER_SIZE);
}

/*
 * sessionid4
 *
 *	typedef opaque sessionid4[NFS4_SESSIONID_SIZE];
 */
static void encode_sessionid4(struct xdr_stream *xdr,
			      const struct nfsd4_session *session)
{
	__be32 *p;

	p = xdr_reserve_space(xdr, NFS4_MAX_SESSIONID_LEN);
	xdr_encode_opaque_fixed(p, session->se_sessionid.data,
					NFS4_MAX_SESSIONID_LEN);
}

/*
 * nfsstat4
 */
static const struct {
	int stat;
	int errno;
} nfs_cb_errtbl[] = {
	{ NFS4_OK,		0		},
	{ NFS4ERR_PERM,		-EPERM		},
	{ NFS4ERR_NOENT,	-ENOENT		},
	{ NFS4ERR_IO,		-EIO		},
	{ NFS4ERR_NXIO,		-ENXIO		},
	{ NFS4ERR_ACCESS,	-EACCES		},
	{ NFS4ERR_EXIST,	-EEXIST		},
	{ NFS4ERR_XDEV,		-EXDEV		},
	{ NFS4ERR_NOTDIR,	-ENOTDIR	},
	{ NFS4ERR_ISDIR,	-EISDIR		},
	{ NFS4ERR_INVAL,	-EINVAL		},
	{ NFS4ERR_FBIG,		-EFBIG		},
	{ NFS4ERR_NOSPC,	-ENOSPC		},
	{ NFS4ERR_ROFS,		-EROFS		},
	{ NFS4ERR_MLINK,	-EMLINK		},
	{ NFS4ERR_NAMETOOLONG,	-ENAMETOOLONG	},
	{ NFS4ERR_NOTEMPTY,	-ENOTEMPTY	},
	{ NFS4ERR_DQUOT,	-EDQUOT		},
	{ NFS4ERR_STALE,	-ESTALE		},
	{ NFS4ERR_BADHANDLE,	-EBADHANDLE	},
	{ NFS4ERR_BAD_COOKIE,	-EBADCOOKIE	},
	{ NFS4ERR_NOTSUPP,	-ENOTSUPP	},
	{ NFS4ERR_TOOSMALL,	-ETOOSMALL	},
	{ NFS4ERR_SERVERFAULT,	-ESERVERFAULT	},
	{ NFS4ERR_BADTYPE,	-EBADTYPE	},
	{ NFS4ERR_LOCKED,	-EAGAIN		},
	{ NFS4ERR_RESOURCE,	-EREMOTEIO	},
	{ NFS4ERR_SYMLINK,	-ELOOP		},
	{ NFS4ERR_OP_ILLEGAL,	-EOPNOTSUPP	},
	{ NFS4ERR_DEADLOCK,	-EDEADLK	},
	{ -1,			-EIO		}
};

/*
 * If we cannot translate the error, the recovery routines should
 * handle it.
 *
 * Note: remaining NFSv4 error codes have values > 10000, so should
 * not conflict with native Linux error codes.
 */
static int nfs_cb_stat_to_errno(int status)
{
	int i;

	for (i = 0; nfs_cb_errtbl[i].stat != -1; i++) {
		if (nfs_cb_errtbl[i].stat == status)
			return nfs_cb_errtbl[i].errno;
	}

	dprintk("NFSD: Unrecognized NFS CB status value: %u\n", status);
	return -status;
}

static int decode_cb_op_status(struct xdr_stream *xdr,
			       enum nfs_cb_opnum4 expected, int *status)
{
	__be32 *p;
	u32 op;

	p = xdr_inline_decode(xdr, 4 + 4);
	if (unlikely(p == NULL))
		goto out_overflow;
	op = be32_to_cpup(p++);
	if (unlikely(op != expected))
		goto out_unexpected;
	*status = nfs_cb_stat_to_errno(be32_to_cpup(p));
	return 0;
out_overflow:
	return -EIO;
out_unexpected:
	dprintk("NFSD: Callback server returned operation %d but "
		"we issued a request for %d\n", op, expected);
	return -EIO;
}

/*
 * CB_COMPOUND4args
 *
 *	struct CB_COMPOUND4args {
 *		utf8str_cs	tag;
 *		uint32_t	minorversion;
 *		uint32_t	callback_ident;
 *		nfs_cb_argop4	argarray<>;
 *	};
*/
static void encode_cb_compound4args(struct xdr_stream *xdr,
				    struct nfs4_cb_compound_hdr *hdr)
{
	__be32 * p;

	p = xdr_reserve_space(xdr, 4 + 4 + 4 + 4);
	p = xdr_encode_empty_array(p);		/* empty tag */
	*p++ = cpu_to_be32(hdr->minorversion);
	*p++ = cpu_to_be32(hdr->ident);

	hdr->nops_p = p;
	*p = cpu_to_be32(hdr->nops);		/* argarray element count */
}

/*
 * Update argarray element count
 */
static void encode_cb_nops(struct nfs4_cb_compound_hdr *hdr)
{
	BUG_ON(hdr->nops > NFS4_MAX_BACK_CHANNEL_OPS);
	*hdr->nops_p = cpu_to_be32(hdr->nops);
}

/*
 * CB_COMPOUND4res
 *
 *	struct CB_COMPOUND4res {
 *		nfsstat4	status;
 *		utf8str_cs	tag;
 *		nfs_cb_resop4	resarray<>;
 *	};
 */
static int decode_cb_compound4res(struct xdr_stream *xdr,
				  struct nfs4_cb_compound_hdr *hdr)
{
	u32 length;
	__be32 *p;

	p = xdr_inline_decode(xdr, XDR_UNIT);
	if (unlikely(p == NULL))
		goto out_overflow;
	hdr->status = be32_to_cpup(p);
	/* Ignore the tag */
	if (xdr_stream_decode_u32(xdr, &length) < 0)
		goto out_overflow;
	if (xdr_inline_decode(xdr, length) == NULL)
		goto out_overflow;
	if (xdr_stream_decode_u32(xdr, &hdr->nops) < 0)
		goto out_overflow;
	return 0;
out_overflow:
	return -EIO;
}

/*
 * CB_RECALL4args
 *
 *	struct CB_RECALL4args {
 *		stateid4	stateid;
 *		bool		truncate;
 *		nfs_fh4		fh;
 *	};
 */
static void encode_cb_recall4args(struct xdr_stream *xdr,
				  const struct nfs4_delegation *dp,
				  struct nfs4_cb_compound_hdr *hdr)
{
	__be32 *p;

	encode_nfs_cb_opnum4(xdr, OP_CB_RECALL);
	encode_stateid4(xdr, &dp->dl_stid.sc_stateid);

	p = xdr_reserve_space(xdr, 4);
	*p++ = xdr_zero;			/* truncate */

	encode_nfs_fh4(xdr, &dp->dl_stid.sc_file->fi_fhandle);

	hdr->nops++;
}

/*
 * CB_RECALLANY4args
 *
 *	struct CB_RECALLANY4args {
 *		uint32_t	craa_objects_to_keep;
 *		bitmap4		craa_type_mask;
 *	};
 */
static void
encode_cb_recallany4args(struct xdr_stream *xdr,
	struct nfs4_cb_compound_hdr *hdr, struct nfsd4_cb_recall_any *ra)
{
	encode_nfs_cb_opnum4(xdr, OP_CB_RECALL_ANY);
	encode_uint32(xdr, ra->ra_keep);
	encode_bitmap4(xdr, ra->ra_bmval, ARRAY_SIZE(ra->ra_bmval));
	hdr->nops++;
}

/*
 * CB_GETATTR4args
 *	struct CB_GETATTR4args {
 *	   nfs_fh4 fh;
 *	   bitmap4 attr_request;
 *	};
 *
 * The size and change attributes are the only one
 * guaranteed to be serviced by the client.
 */
static void
encode_cb_getattr4args(struct xdr_stream *xdr, struct nfs4_cb_compound_hdr *hdr,
			struct nfs4_cb_fattr *fattr)
{
	struct nfs4_delegation *dp = container_of(fattr, struct nfs4_delegation, dl_cb_fattr);
	struct knfsd_fh *fh = &dp->dl_stid.sc_file->fi_fhandle;
	struct nfs4_cb_fattr *ncf = &dp->dl_cb_fattr;
	u32 bmap_size = 1;
	u32 bmap[3];

	bmap[0] = FATTR4_WORD0_SIZE;
	if (!ncf->ncf_file_modified)
		bmap[0] |= FATTR4_WORD0_CHANGE;

	if (deleg_attrs_deleg(dp->dl_type)) {
		bmap[1] = 0;
		bmap[2] = FATTR4_WORD2_TIME_DELEG_ACCESS | FATTR4_WORD2_TIME_DELEG_MODIFY;
		bmap_size = 3;
	}
	encode_nfs_cb_opnum4(xdr, OP_CB_GETATTR);
	encode_nfs_fh4(xdr, fh);
	encode_bitmap4(xdr, bmap, bmap_size);
	hdr->nops++;
}

static u32 highest_slotid(struct nfsd4_session *ses)
{
	u32 idx;

	spin_lock(&ses->se_lock);
	idx = fls(~ses->se_cb_slot_avail);
	if (idx > 0)
		--idx;
	idx = max(idx, ses->se_cb_highest_slot);
	spin_unlock(&ses->se_lock);
	return idx;
}

static void
encode_referring_call4(struct xdr_stream *xdr,
		       const struct nfsd4_referring_call *rc)
{
	encode_uint32(xdr, rc->rc_sequenceid);
	encode_uint32(xdr, rc->rc_slotid);
}

static void
encode_referring_call_list4(struct xdr_stream *xdr,
			    const struct nfsd4_referring_call_list *rcl)
{
	struct nfsd4_referring_call *rc;
	__be32 *p;

	p = xdr_reserve_space(xdr, NFS4_MAX_SESSIONID_LEN);
	xdr_encode_opaque_fixed(p, rcl->rcl_sessionid.data,
					NFS4_MAX_SESSIONID_LEN);
	encode_uint32(xdr, rcl->__nr_referring_calls);
	list_for_each_entry(rc, &rcl->rcl_referring_calls, __list)
		encode_referring_call4(xdr, rc);
}

/*
 * CB_SEQUENCE4args
 *
 *	struct CB_SEQUENCE4args {
 *		sessionid4		csa_sessionid;
 *		sequenceid4		csa_sequenceid;
 *		slotid4			csa_slotid;
 *		slotid4			csa_highest_slotid;
 *		bool			csa_cachethis;
 *		referring_call_list4	csa_referring_call_lists<>;
 *	};
 */
static void encode_cb_sequence4args(struct xdr_stream *xdr,
				    const struct nfsd4_callback *cb,
				    struct nfs4_cb_compound_hdr *hdr)
{
	struct nfsd4_session *session = cb->cb_clp->cl_cb_session;
	struct nfsd4_referring_call_list *rcl;
	__be32 *p;

	if (hdr->minorversion == 0)
		return;

	encode_nfs_cb_opnum4(xdr, OP_CB_SEQUENCE);
	encode_sessionid4(xdr, session);

	p = xdr_reserve_space(xdr, XDR_UNIT * 4);
	*p++ = cpu_to_be32(session->se_cb_seq_nr[cb->cb_held_slot]);	/* csa_sequenceid */
	*p++ = cpu_to_be32(cb->cb_held_slot);		/* csa_slotid */
	*p++ = cpu_to_be32(highest_slotid(session)); /* csa_highest_slotid */
	*p++ = xdr_zero;			/* csa_cachethis */

	/* csa_referring_call_lists */
	encode_uint32(xdr, cb->cb_nr_referring_call_list);
	list_for_each_entry(rcl, &cb->cb_referring_call_list, __list)
		encode_referring_call_list4(xdr, rcl);

	hdr->nops++;
}

static void update_cb_slot_table(struct nfsd4_session *ses, u32 target)
{
	/* No need to do anything if nothing changed */
	if (likely(target == READ_ONCE(ses->se_cb_highest_slot)))
		return;

	spin_lock(&ses->se_lock);
	if (target > ses->se_cb_highest_slot) {
		int i;

		target = min(target, NFSD_BC_SLOT_TABLE_SIZE - 1);

		/*
		 * Growing the slot table. Reset any new sequences to 1.
		 *
		 * NB: There is some debate about whether the RFC requires this,
		 *     but the Linux client expects it.
		 */
		for (i = ses->se_cb_highest_slot + 1; i <= target; ++i)
			ses->se_cb_seq_nr[i] = 1;
	}
	ses->se_cb_highest_slot = target;
	spin_unlock(&ses->se_lock);
}

/*
 * CB_SEQUENCE4resok
 *
 *	struct CB_SEQUENCE4resok {
 *		sessionid4	csr_sessionid;
 *		sequenceid4	csr_sequenceid;
 *		slotid4		csr_slotid;
 *		slotid4		csr_highest_slotid;
 *		slotid4		csr_target_highest_slotid;
 *	};
 *
 *	union CB_SEQUENCE4res switch (nfsstat4 csr_status) {
 *	case NFS4_OK:
 *		CB_SEQUENCE4resok	csr_resok4;
 *	default:
 *		void;
 *	};
 *
 * Our current back channel implmentation supports a single backchannel
 * with a single slot.
 */
static int decode_cb_sequence4resok(struct xdr_stream *xdr,
				    struct nfsd4_callback *cb)
{
	struct nfsd4_session *session = cb->cb_clp->cl_cb_session;
	int status = -ESERVERFAULT;
	__be32 *p;
	u32 seqid, slotid, target;

	/*
	 * If the server returns different values for sessionID, slotID or
	 * sequence number, the server is looney tunes.
	 */
	p = xdr_inline_decode(xdr, NFS4_MAX_SESSIONID_LEN + 4 + 4 + 4 + 4);
	if (unlikely(p == NULL))
		goto out_overflow;

	if (memcmp(p, session->se_sessionid.data, NFS4_MAX_SESSIONID_LEN)) {
		dprintk("NFS: %s Invalid session id\n", __func__);
		goto out;
	}
	p += XDR_QUADLEN(NFS4_MAX_SESSIONID_LEN);

	seqid = be32_to_cpup(p++);
	if (seqid != session->se_cb_seq_nr[cb->cb_held_slot]) {
		dprintk("NFS: %s Invalid sequence number\n", __func__);
		goto out;
	}

	slotid = be32_to_cpup(p++);
	if (slotid != cb->cb_held_slot) {
		dprintk("NFS: %s Invalid slotid\n", __func__);
		goto out;
	}

	p++; // ignore current highest slot value

	target = be32_to_cpup(p++);
	update_cb_slot_table(session, target);
	status = 0;
out:
	cb->cb_seq_status = status;
	return status;
out_overflow:
	status = -EIO;
	goto out;
}

static int decode_cb_sequence4res(struct xdr_stream *xdr,
				  struct nfsd4_callback *cb)
{
	int status;

	if (cb->cb_clp->cl_minorversion == 0)
		return 0;

	status = decode_cb_op_status(xdr, OP_CB_SEQUENCE, &cb->cb_seq_status);
	if (unlikely(status || cb->cb_seq_status))
		return status;

	return decode_cb_sequence4resok(xdr, cb);
}

/*
 * NFSv4.0 and NFSv4.1 XDR encode functions
 *
 * NFSv4.0 callback argument types are defined in section 15 of RFC
 * 3530: "Network File System (NFS) version 4 Protocol" and section 20
 * of RFC 5661:  "Network File System (NFS) Version 4 Minor Version 1
 * Protocol".
 */

/*
 * NB: Without this zero space reservation, callbacks over krb5p fail
 */
static void nfs4_xdr_enc_cb_null(struct rpc_rqst *req, struct xdr_stream *xdr,
				 const void *__unused)
{
	xdr_reserve_space(xdr, 0);
}

/*
 * 20.1.  Operation 3: CB_GETATTR - Get Attributes
 */
static void nfs4_xdr_enc_cb_getattr(struct rpc_rqst *req,
		struct xdr_stream *xdr, const void *data)
{
	const struct nfsd4_callback *cb = data;
	struct nfs4_cb_fattr *ncf =
		container_of(cb, struct nfs4_cb_fattr, ncf_getattr);
	struct nfs4_cb_compound_hdr hdr = {
		.ident = cb->cb_clp->cl_cb_ident,
		.minorversion = cb->cb_clp->cl_minorversion,
	};

	encode_cb_compound4args(xdr, &hdr);
	encode_cb_sequence4args(xdr, cb, &hdr);
	encode_cb_getattr4args(xdr, &hdr, ncf);
	encode_cb_nops(&hdr);
}

/*
 * 20.2. Operation 4: CB_RECALL - Recall a Delegation
 */
static void nfs4_xdr_enc_cb_recall(struct rpc_rqst *req, struct xdr_stream *xdr,
				   const void *data)
{
	const struct nfsd4_callback *cb = data;
	const struct nfs4_delegation *dp = cb_to_delegation(cb);
	struct nfs4_cb_compound_hdr hdr = {
		.ident = cb->cb_clp->cl_cb_ident,
		.minorversion = cb->cb_clp->cl_minorversion,
	};

	encode_cb_compound4args(xdr, &hdr);
	encode_cb_sequence4args(xdr, cb, &hdr);
	encode_cb_recall4args(xdr, dp, &hdr);
	encode_cb_nops(&hdr);
}

/*
 * 20.6. Operation 8: CB_RECALL_ANY - Keep Any N Recallable Objects
 */
static void
nfs4_xdr_enc_cb_recall_any(struct rpc_rqst *req,
		struct xdr_stream *xdr, const void *data)
{
	const struct nfsd4_callback *cb = data;
	struct nfsd4_cb_recall_any *ra;
	struct nfs4_cb_compound_hdr hdr = {
		.ident = cb->cb_clp->cl_cb_ident,
		.minorversion = cb->cb_clp->cl_minorversion,
	};

	ra = container_of(cb, struct nfsd4_cb_recall_any, ra_cb);
	encode_cb_compound4args(xdr, &hdr);
	encode_cb_sequence4args(xdr, cb, &hdr);
	encode_cb_recallany4args(xdr, &hdr, ra);
	encode_cb_nops(&hdr);
}

/*
 * NFSv4.0 and NFSv4.1 XDR decode functions
 *
 * NFSv4.0 callback result types are defined in section 15 of RFC
 * 3530: "Network File System (NFS) version 4 Protocol" and section 20
 * of RFC 5661:  "Network File System (NFS) Version 4 Minor Version 1
 * Protocol".
 */

static int nfs4_xdr_dec_cb_null(struct rpc_rqst *req, struct xdr_stream *xdr,
				void *__unused)
{
	return 0;
}

/*
 * 20.1.  Operation 3: CB_GETATTR - Get Attributes
 */
static int nfs4_xdr_dec_cb_getattr(struct rpc_rqst *rqstp,
				  struct xdr_stream *xdr,
				  void *data)
{
	struct nfsd4_callback *cb = data;
	struct nfs4_cb_compound_hdr hdr;
	int status;
	u32 bitmap[3] = {0};
	u32 attrlen, maxlen;
	struct nfs4_cb_fattr *ncf =
		container_of(cb, struct nfs4_cb_fattr, ncf_getattr);

	status = decode_cb_compound4res(xdr, &hdr);
	if (unlikely(status))
		return status;

	status = decode_cb_sequence4res(xdr, cb);
	if (unlikely(status || cb->cb_seq_status))
		return status;

	status = decode_cb_op_status(xdr, OP_CB_GETATTR, &cb->cb_status);
	if (unlikely(status || cb->cb_status))
		return status;
	if (xdr_stream_decode_uint32_array(xdr, bitmap, 3) < 0)
		return -EIO;
	if (xdr_stream_decode_u32(xdr, &attrlen) < 0)
		return -EIO;
	maxlen = sizeof(ncf->ncf_cb_change) + sizeof(ncf->ncf_cb_fsize);
	if (bitmap[2] != 0)
		maxlen += (sizeof(ncf->ncf_cb_mtime.tv_sec) +
			   sizeof(ncf->ncf_cb_mtime.tv_nsec)) * 2;
	if (attrlen > maxlen)
		return -EIO;
	status = decode_cb_fattr4(xdr, bitmap, ncf);
	return status;
}

/*
 * 20.2. Operation 4: CB_RECALL - Recall a Delegation
 */
static int nfs4_xdr_dec_cb_recall(struct rpc_rqst *rqstp,
				  struct xdr_stream *xdr,
				  void *data)
{
	struct nfsd4_callback *cb = data;
	struct nfs4_cb_compound_hdr hdr;
	int status;

	status = decode_cb_compound4res(xdr, &hdr);
	if (unlikely(status))
		return status;

	status = decode_cb_sequence4res(xdr, cb);
	if (unlikely(status || cb->cb_seq_status))
		return status;

	return decode_cb_op_status(xdr, OP_CB_RECALL, &cb->cb_status);
}

/*
 * 20.6. Operation 8: CB_RECALL_ANY - Keep Any N Recallable Objects
 */
static int
nfs4_xdr_dec_cb_recall_any(struct rpc_rqst *rqstp,
				  struct xdr_stream *xdr,
				  void *data)
{
	struct nfsd4_callback *cb = data;
	struct nfs4_cb_compound_hdr hdr;
	int status;

	status = decode_cb_compound4res(xdr, &hdr);
	if (unlikely(status))
		return status;
	status = decode_cb_sequence4res(xdr, cb);
	if (unlikely(status || cb->cb_seq_status))
		return status;
	status =  decode_cb_op_status(xdr, OP_CB_RECALL_ANY, &cb->cb_status);
	return status;
}

#ifdef CONFIG_NFSD_PNFS
/*
 * CB_LAYOUTRECALL4args
 *
 *	struct layoutrecall_file4 {
 *		nfs_fh4         lor_fh;
 *		offset4         lor_offset;
 *		length4         lor_length;
 *		stateid4        lor_stateid;
 *	};
 *
 *	union layoutrecall4 switch(layoutrecall_type4 lor_recalltype) {
 *	case LAYOUTRECALL4_FILE:
 *		layoutrecall_file4 lor_layout;
 *	case LAYOUTRECALL4_FSID:
 *		fsid4              lor_fsid;
 *	case LAYOUTRECALL4_ALL:
 *		void;
 *	};
 *
 *	struct CB_LAYOUTRECALL4args {
 *		layouttype4             clora_type;
 *		layoutiomode4           clora_iomode;
 *		bool                    clora_changed;
 *		layoutrecall4           clora_recall;
 *	};
 */
static void encode_cb_layout4args(struct xdr_stream *xdr,
				  const struct nfs4_layout_stateid *ls,
				  struct nfs4_cb_compound_hdr *hdr)
{
	__be32 *p;

	BUG_ON(hdr->minorversion == 0);

	p = xdr_reserve_space(xdr, 5 * 4);
	*p++ = cpu_to_be32(OP_CB_LAYOUTRECALL);
	*p++ = cpu_to_be32(ls->ls_layout_type);
	*p++ = cpu_to_be32(IOMODE_ANY);
	*p++ = cpu_to_be32(1);
	*p = cpu_to_be32(RETURN_FILE);

	encode_nfs_fh4(xdr, &ls->ls_stid.sc_file->fi_fhandle);

	p = xdr_reserve_space(xdr, 2 * 8);
	p = xdr_encode_hyper(p, 0);
	xdr_encode_hyper(p, NFS4_MAX_UINT64);

	encode_stateid4(xdr, &ls->ls_recall_sid);

	hdr->nops++;
}

static void nfs4_xdr_enc_cb_layout(struct rpc_rqst *req,
				   struct xdr_stream *xdr,
				   const void *data)
{
	const struct nfsd4_callback *cb = data;
	const struct nfs4_layout_stateid *ls =
		container_of(cb, struct nfs4_layout_stateid, ls_recall);
	struct nfs4_cb_compound_hdr hdr = {
		.ident = 0,
		.minorversion = cb->cb_clp->cl_minorversion,
	};

	encode_cb_compound4args(xdr, &hdr);
	encode_cb_sequence4args(xdr, cb, &hdr);
	encode_cb_layout4args(xdr, ls, &hdr);
	encode_cb_nops(&hdr);
}

static int nfs4_xdr_dec_cb_layout(struct rpc_rqst *rqstp,
				  struct xdr_stream *xdr,
				  void *data)
{
	struct nfsd4_callback *cb = data;
	struct nfs4_cb_compound_hdr hdr;
	int status;

	status = decode_cb_compound4res(xdr, &hdr);
	if (unlikely(status))
		return status;

	status = decode_cb_sequence4res(xdr, cb);
	if (unlikely(status || cb->cb_seq_status))
		return status;

	return decode_cb_op_status(xdr, OP_CB_LAYOUTRECALL, &cb->cb_status);
}
#endif /* CONFIG_NFSD_PNFS */

static void encode_stateowner(struct xdr_stream *xdr, struct nfs4_stateowner *so)
{
	__be32	*p;

	p = xdr_reserve_space(xdr, 8 + 4 + so->so_owner.len);
	p = xdr_encode_opaque_fixed(p, &so->so_client->cl_clientid, 8);
	xdr_encode_opaque(p, so->so_owner.data, so->so_owner.len);
}

static void nfs4_xdr_enc_cb_notify_lock(struct rpc_rqst *req,
					struct xdr_stream *xdr,
					const void *data)
{
	const struct nfsd4_callback *cb = data;
	const struct nfsd4_blocked_lock *nbl =
		container_of(cb, struct nfsd4_blocked_lock, nbl_cb);
	struct nfs4_lockowner *lo = (struct nfs4_lockowner *)nbl->nbl_lock.c.flc_owner;
	struct nfs4_cb_compound_hdr hdr = {
		.ident = 0,
		.minorversion = cb->cb_clp->cl_minorversion,
	};

	__be32 *p;

	BUG_ON(hdr.minorversion == 0);

	encode_cb_compound4args(xdr, &hdr);
	encode_cb_sequence4args(xdr, cb, &hdr);

	p = xdr_reserve_space(xdr, 4);
	*p = cpu_to_be32(OP_CB_NOTIFY_LOCK);
	encode_nfs_fh4(xdr, &nbl->nbl_fh);
	encode_stateowner(xdr, &lo->lo_owner);
	hdr.nops++;

	encode_cb_nops(&hdr);
}

static int nfs4_xdr_dec_cb_notify_lock(struct rpc_rqst *rqstp,
					struct xdr_stream *xdr,
					void *data)
{
	struct nfsd4_callback *cb = data;
	struct nfs4_cb_compound_hdr hdr;
	int status;

	status = decode_cb_compound4res(xdr, &hdr);
	if (unlikely(status))
		return status;

	status = decode_cb_sequence4res(xdr, cb);
	if (unlikely(status || cb->cb_seq_status))
		return status;

	return decode_cb_op_status(xdr, OP_CB_NOTIFY_LOCK, &cb->cb_status);
}

/*
 * struct write_response4 {
 *	stateid4	wr_callback_id<1>;
 *	length4		wr_count;
 *	stable_how4	wr_committed;
 *	verifier4	wr_writeverf;
 * };
 * union offload_info4 switch (nfsstat4 coa_status) {
 *	case NFS4_OK:
 *		write_response4	coa_resok4;
 *	default:
 *		length4		coa_bytes_copied;
 * };
 * struct CB_OFFLOAD4args {
 *	nfs_fh4		coa_fh;
 *	stateid4	coa_stateid;
 *	offload_info4	coa_offload_info;
 * };
 */
static void encode_offload_info4(struct xdr_stream *xdr,
				 const struct nfsd4_cb_offload *cbo)
{
	__be32 *p;

	p = xdr_reserve_space(xdr, 4);
	*p = cbo->co_nfserr;
	switch (cbo->co_nfserr) {
	case nfs_ok:
		p = xdr_reserve_space(xdr, 4 + 8 + 4 + NFS4_VERIFIER_SIZE);
		p = xdr_encode_empty_array(p);
		p = xdr_encode_hyper(p, cbo->co_res.wr_bytes_written);
		*p++ = cpu_to_be32(cbo->co_res.wr_stable_how);
		p = xdr_encode_opaque_fixed(p, cbo->co_res.wr_verifier.data,
					    NFS4_VERIFIER_SIZE);
		break;
	default:
		p = xdr_reserve_space(xdr, 8);
		/* We always return success if bytes were written */
		p = xdr_encode_hyper(p, 0);
	}
}

static void encode_cb_offload4args(struct xdr_stream *xdr,
				   const struct nfsd4_cb_offload *cbo,
				   struct nfs4_cb_compound_hdr *hdr)
{
	__be32 *p;

	p = xdr_reserve_space(xdr, 4);
	*p = cpu_to_be32(OP_CB_OFFLOAD);
	encode_nfs_fh4(xdr, &cbo->co_fh);
	encode_stateid4(xdr, &cbo->co_res.cb_stateid);
	encode_offload_info4(xdr, cbo);

	hdr->nops++;
}

static void nfs4_xdr_enc_cb_offload(struct rpc_rqst *req,
				    struct xdr_stream *xdr,
				    const void *data)
{
	const struct nfsd4_callback *cb = data;
	const struct nfsd4_cb_offload *cbo =
		container_of(cb, struct nfsd4_cb_offload, co_cb);
	struct nfs4_cb_compound_hdr hdr = {
		.ident = 0,
		.minorversion = cb->cb_clp->cl_minorversion,
	};

	encode_cb_compound4args(xdr, &hdr);
	encode_cb_sequence4args(xdr, cb, &hdr);
	encode_cb_offload4args(xdr, cbo, &hdr);
	encode_cb_nops(&hdr);
}

static int nfs4_xdr_dec_cb_offload(struct rpc_rqst *rqstp,
				   struct xdr_stream *xdr,
				   void *data)
{
	struct nfsd4_callback *cb = data;
	struct nfs4_cb_compound_hdr hdr;
	int status;

	status = decode_cb_compound4res(xdr, &hdr);
	if (unlikely(status))
		return status;

	status = decode_cb_sequence4res(xdr, cb);
	if (unlikely(status || cb->cb_seq_status))
		return status;

	return decode_cb_op_status(xdr, OP_CB_OFFLOAD, &cb->cb_status);
}
/*
 * RPC procedure tables
 */
#define PROC(proc, call, argtype, restype)				\
[NFSPROC4_CLNT_##proc] = {						\
	.p_proc    = NFSPROC4_CB_##call,				\
	.p_encode  = nfs4_xdr_enc_##argtype,		\
	.p_decode  = nfs4_xdr_dec_##restype,				\
	.p_arglen  = NFS4_enc_##argtype##_sz,				\
	.p_replen  = NFS4_dec_##restype##_sz,				\
	.p_statidx = NFSPROC4_CB_##call,				\
	.p_name    = #proc,						\
}

static const struct rpc_procinfo nfs4_cb_procedures[] = {
	PROC(CB_NULL,	NULL,		cb_null,	cb_null),
	PROC(CB_RECALL,	COMPOUND,	cb_recall,	cb_recall),
#ifdef CONFIG_NFSD_PNFS
	PROC(CB_LAYOUT,	COMPOUND,	cb_layout,	cb_layout),
#endif
	PROC(CB_NOTIFY_LOCK,	COMPOUND,	cb_notify_lock,	cb_notify_lock),
	PROC(CB_OFFLOAD,	COMPOUND,	cb_offload,	cb_offload),
	PROC(CB_RECALL_ANY,	COMPOUND,	cb_recall_any,	cb_recall_any),
	PROC(CB_GETATTR,	COMPOUND,	cb_getattr,	cb_getattr),
};

static unsigned int nfs4_cb_counts[ARRAY_SIZE(nfs4_cb_procedures)];
static const struct rpc_version nfs_cb_version4 = {
/*
 * Note on the callback rpc program version number: despite language in rfc
 * 5661 section 18.36.3 requiring servers to use 4 in this field, the
 * official xdr descriptions for both 4.0 and 4.1 specify version 1, and
 * in practice that appears to be what implementations use.  The section
 * 18.36.3 language is expected to be fixed in an erratum.
 */
	.number			= 1,
	.nrprocs		= ARRAY_SIZE(nfs4_cb_procedures),
	.procs			= nfs4_cb_procedures,
	.counts			= nfs4_cb_counts,
};

static const struct rpc_version *nfs_cb_version[2] = {
	[1] = &nfs_cb_version4,
};

static const struct rpc_program cb_program;

static struct rpc_stat cb_stats = {
	.program		= &cb_program
};

#define NFS4_CALLBACK 0x40000000
static const struct rpc_program cb_program = {
	.name			= "nfs4_cb",
	.number			= NFS4_CALLBACK,
	.nrvers			= ARRAY_SIZE(nfs_cb_version),
	.version		= nfs_cb_version,
	.stats			= &cb_stats,
	.pipe_dir_name		= "nfsd4_cb",
};

static int max_cb_time(struct net *net)
{
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	/*
	 * nfsd4_lease is set to at most one hour in __nfsd4_write_time,
	 * so we can use 32-bit math on it. Warn if that assumption
	 * ever stops being true.
	 */
	if (WARN_ON_ONCE(nn->nfsd4_lease > 3600))
		return 360 * HZ;

	return max(((u32)nn->nfsd4_lease)/10, 1u) * HZ;
}

static bool nfsd4_queue_cb(struct nfsd4_callback *cb)
{
	struct nfs4_client *clp = cb->cb_clp;

	trace_nfsd_cb_queue(clp, cb);
	return queue_work(clp->cl_callback_wq, &cb->cb_work);
}

static void nfsd4_requeue_cb(struct rpc_task *task, struct nfsd4_callback *cb)
{
	struct nfs4_client *clp = cb->cb_clp;

	if (!test_bit(NFSD4_CLIENT_CB_KILL, &clp->cl_flags)) {
		trace_nfsd_cb_restart(clp, cb);
		task->tk_status = 0;
		set_bit(NFSD4_CALLBACK_REQUEUE, &cb->cb_flags);
	}
}

static void nfsd41_cb_inflight_begin(struct nfs4_client *clp)
{
	atomic_inc(&clp->cl_cb_inflight);
}

static void nfsd41_cb_inflight_end(struct nfs4_client *clp)
{

	atomic_dec_and_wake_up(&clp->cl_cb_inflight);
}

static void nfsd41_cb_inflight_wait_complete(struct nfs4_client *clp)
{
	wait_var_event(&clp->cl_cb_inflight,
			!atomic_read(&clp->cl_cb_inflight));
}

static const struct cred *get_backchannel_cred(struct nfs4_client *clp, struct rpc_clnt *client, struct nfsd4_session *ses)
{
	if (clp->cl_minorversion == 0) {
		client->cl_principal = clp->cl_cred.cr_targ_princ ?
			clp->cl_cred.cr_targ_princ : "nfs";

		return get_cred(rpc_machine_cred());
	} else {
		struct cred *kcred;

		kcred = prepare_kernel_cred(&init_task);
		if (!kcred)
			return NULL;

		kcred->fsuid = ses->se_cb_sec.uid;
		kcred->fsgid = ses->se_cb_sec.gid;
		return kcred;
	}
}

static int setup_callback_client(struct nfs4_client *clp, struct nfs4_cb_conn *conn, struct nfsd4_session *ses)
{
	int maxtime = max_cb_time(clp->net);
	struct rpc_timeout	timeparms = {
		.to_initval	= maxtime,
		.to_retries	= 0,
		.to_maxval	= maxtime,
	};
	struct rpc_create_args args = {
		.net		= clp->net,
		.address	= (struct sockaddr *) &conn->cb_addr,
		.addrsize	= conn->cb_addrlen,
		.saddress	= (struct sockaddr *) &conn->cb_saddr,
		.timeout	= &timeparms,
		.program	= &cb_program,
		.version	= 1,
		.flags		= (RPC_CLNT_CREATE_NOPING | RPC_CLNT_CREATE_QUIET),
		.cred		= current_cred(),
	};
	struct rpc_clnt *client;
	const struct cred *cred;

	if (clp->cl_minorversion == 0) {
		if (!clp->cl_cred.cr_principal &&
		    (clp->cl_cred.cr_flavor >= RPC_AUTH_GSS_KRB5)) {
			trace_nfsd_cb_setup_err(clp, -EINVAL);
			return -EINVAL;
		}
		args.client_name = clp->cl_cred.cr_principal;
		args.prognumber	= conn->cb_prog;
		args.protocol = XPRT_TRANSPORT_TCP;
		args.authflavor = clp->cl_cred.cr_flavor;
		clp->cl_cb_ident = conn->cb_ident;
	} else {
		if (!conn->cb_xprt || !ses)
			return -EINVAL;
		clp->cl_cb_session = ses;
		args.bc_xprt = conn->cb_xprt;
		args.prognumber = clp->cl_cb_session->se_cb_prog;
		args.protocol = conn->cb_xprt->xpt_class->xcl_ident |
				XPRT_TRANSPORT_BC;
		args.authflavor = ses->se_cb_sec.flavor;
	}
	/* Create RPC client */
	client = rpc_create(&args);
	if (IS_ERR(client)) {
		trace_nfsd_cb_setup_err(clp, PTR_ERR(client));
		return PTR_ERR(client);
	}
	cred = get_backchannel_cred(clp, client, ses);
	if (!cred) {
		trace_nfsd_cb_setup_err(clp, -ENOMEM);
		rpc_shutdown_client(client);
		return -ENOMEM;
	}

	if (clp->cl_minorversion != 0)
		clp->cl_cb_conn.cb_xprt = conn->cb_xprt;
	clp->cl_cb_client = client;
	clp->cl_cb_cred = cred;
	rcu_read_lock();
	trace_nfsd_cb_setup(clp, rpc_peeraddr2str(client, RPC_DISPLAY_NETID),
			    args.authflavor);
	rcu_read_unlock();
	return 0;
}

static void nfsd4_mark_cb_state(struct nfs4_client *clp, int newstate)
{
	if (clp->cl_cb_state != newstate) {
		clp->cl_cb_state = newstate;
		trace_nfsd_cb_new_state(clp);
	}
}

static void nfsd4_mark_cb_down(struct nfs4_client *clp)
{
	if (test_bit(NFSD4_CLIENT_CB_UPDATE, &clp->cl_flags))
		return;
	nfsd4_mark_cb_state(clp, NFSD4_CB_DOWN);
}

static void nfsd4_mark_cb_fault(struct nfs4_client *clp)
{
	if (test_bit(NFSD4_CLIENT_CB_UPDATE, &clp->cl_flags))
		return;
	nfsd4_mark_cb_state(clp, NFSD4_CB_FAULT);
}

static void nfsd4_cb_probe_done(struct rpc_task *task, void *calldata)
{
	struct nfs4_client *clp = container_of(calldata, struct nfs4_client, cl_cb_null);

	if (task->tk_status)
		nfsd4_mark_cb_down(clp);
	else
		nfsd4_mark_cb_state(clp, NFSD4_CB_UP);
}

static void nfsd4_cb_probe_release(void *calldata)
{
	struct nfs4_client *clp = container_of(calldata, struct nfs4_client, cl_cb_null);

	nfsd41_cb_inflight_end(clp);

}

static const struct rpc_call_ops nfsd4_cb_probe_ops = {
	/* XXX: release method to ensure we set the cb channel down if
	 * necessary on early failure? */
	.rpc_call_done = nfsd4_cb_probe_done,
	.rpc_release = nfsd4_cb_probe_release,
};

/*
 * Poke the callback thread to process any updates to the callback
 * parameters, and send a null probe.
 */
void nfsd4_probe_callback(struct nfs4_client *clp)
{
	trace_nfsd_cb_probe(clp);
	nfsd4_mark_cb_state(clp, NFSD4_CB_UNKNOWN);
	set_bit(NFSD4_CLIENT_CB_UPDATE, &clp->cl_flags);
	nfsd4_run_cb(&clp->cl_cb_null);
}

void nfsd4_probe_callback_sync(struct nfs4_client *clp)
{
	nfsd4_probe_callback(clp);
	flush_workqueue(clp->cl_callback_wq);
}

void nfsd4_change_callback(struct nfs4_client *clp, struct nfs4_cb_conn *conn)
{
	nfsd4_mark_cb_state(clp, NFSD4_CB_UNKNOWN);
	spin_lock(&clp->cl_lock);
	memcpy(&clp->cl_cb_conn, conn, sizeof(struct nfs4_cb_conn));
	spin_unlock(&clp->cl_lock);
}

static int grab_slot(struct nfsd4_session *ses)
{
	int idx;

	spin_lock(&ses->se_lock);
	idx = ffs(ses->se_cb_slot_avail) - 1;
	if (idx < 0 || idx > ses->se_cb_highest_slot) {
		spin_unlock(&ses->se_lock);
		return -1;
	}
	/* clear the bit for the slot */
	ses->se_cb_slot_avail &= ~BIT(idx);
	spin_unlock(&ses->se_lock);
	return idx;
}

/*
 * There's currently a single callback channel slot.
 * If the slot is available, then mark it busy.  Otherwise, set the
 * thread for sleeping on the callback RPC wait queue.
 */
static bool nfsd41_cb_get_slot(struct nfsd4_callback *cb, struct rpc_task *task)
{
	struct nfs4_client *clp = cb->cb_clp;
	struct nfsd4_session *ses = clp->cl_cb_session;

	if (cb->cb_held_slot >= 0)
		return true;
	cb->cb_held_slot = grab_slot(ses);
	if (cb->cb_held_slot < 0) {
		rpc_sleep_on(&clp->cl_cb_waitq, task, NULL);
		/* Race breaker */
		cb->cb_held_slot = grab_slot(ses);
		if (cb->cb_held_slot < 0)
			return false;
		rpc_wake_up_queued_task(&clp->cl_cb_waitq, task);
	}
	return true;
}

static void nfsd41_cb_release_slot(struct nfsd4_callback *cb)
{
	struct nfs4_client *clp = cb->cb_clp;
	struct nfsd4_session *ses = clp->cl_cb_session;

	if (cb->cb_held_slot >= 0) {
		spin_lock(&ses->se_lock);
		ses->se_cb_slot_avail |= BIT(cb->cb_held_slot);
		spin_unlock(&ses->se_lock);
		cb->cb_held_slot = -1;
		rpc_wake_up_next(&clp->cl_cb_waitq);
	}
}

static void nfsd41_destroy_cb(struct nfsd4_callback *cb)
{
	struct nfs4_client *clp = cb->cb_clp;

	trace_nfsd_cb_destroy(clp, cb);
	nfsd41_cb_release_slot(cb);
	if (test_bit(NFSD4_CALLBACK_WAKE, &cb->cb_flags))
		clear_and_wake_up_bit(NFSD4_CALLBACK_RUNNING, &cb->cb_flags);
	else
		clear_bit(NFSD4_CALLBACK_RUNNING, &cb->cb_flags);

	if (cb->cb_ops && cb->cb_ops->release)
		cb->cb_ops->release(cb);
	nfsd41_cb_inflight_end(clp);
}

/**
 * nfsd41_cb_referring_call - add a referring call to a callback operation
 * @cb: context of callback to add the rc to
 * @sessionid: referring call's session ID
 * @slotid: referring call's session slot index
 * @seqno: referring call's slot sequence number
 *
 * Caller serializes access to @cb.
 *
 * NB: If memory allocation fails, the referring call is not added.
 */
void nfsd41_cb_referring_call(struct nfsd4_callback *cb,
			      struct nfs4_sessionid *sessionid,
			      u32 slotid, u32 seqno)
{
	struct nfsd4_referring_call_list *rcl;
	struct nfsd4_referring_call *rc;
	bool found;

	might_sleep();

	found = false;
	list_for_each_entry(rcl, &cb->cb_referring_call_list, __list) {
		if (!memcmp(rcl->rcl_sessionid.data, sessionid->data,
			   NFS4_MAX_SESSIONID_LEN)) {
			found = true;
			break;
		}
	}
	if (!found) {
		rcl = kmalloc(sizeof(*rcl), GFP_KERNEL);
		if (!rcl)
			return;
		memcpy(rcl->rcl_sessionid.data, sessionid->data,
		       NFS4_MAX_SESSIONID_LEN);
		rcl->__nr_referring_calls = 0;
		INIT_LIST_HEAD(&rcl->rcl_referring_calls);
		list_add(&rcl->__list, &cb->cb_referring_call_list);
		cb->cb_nr_referring_call_list++;
	}

	found = false;
	list_for_each_entry(rc, &rcl->rcl_referring_calls, __list) {
		if (rc->rc_sequenceid == seqno && rc->rc_slotid == slotid) {
			found = true;
			break;
		}
	}
	if (!found) {
		rc = kmalloc(sizeof(*rc), GFP_KERNEL);
		if (!rc)
			goto out;
		rc->rc_sequenceid = seqno;
		rc->rc_slotid = slotid;
		rcl->__nr_referring_calls++;
		list_add(&rc->__list, &rcl->rcl_referring_calls);
	}

out:
	if (!rcl->__nr_referring_calls) {
		cb->cb_nr_referring_call_list--;
		list_del(&rcl->__list);
		kfree(rcl);
	}
}

/**
 * nfsd41_cb_destroy_referring_call_list - release referring call info
 * @cb: context of a callback that has completed
 *
 * Callers who allocate referring calls using nfsd41_cb_referring_call() must
 * release those resources by calling nfsd41_cb_destroy_referring_call_list.
 *
 * Caller serializes access to @cb.
 */
void nfsd41_cb_destroy_referring_call_list(struct nfsd4_callback *cb)
{
	struct nfsd4_referring_call_list *rcl;
	struct nfsd4_referring_call *rc;

	while (!list_empty(&cb->cb_referring_call_list)) {
		rcl = list_first_entry(&cb->cb_referring_call_list,
				       struct nfsd4_referring_call_list,
				       __list);

		while (!list_empty(&rcl->rcl_referring_calls)) {
			rc = list_first_entry(&rcl->rcl_referring_calls,
					      struct nfsd4_referring_call,
					      __list);
			list_del(&rc->__list);
			kfree(rc);
		}
		list_del(&rcl->__list);
		kfree(rcl);
	}
}

static void nfsd4_cb_prepare(struct rpc_task *task, void *calldata)
{
	struct nfsd4_callback *cb = calldata;
	struct nfs4_client *clp = cb->cb_clp;
	u32 minorversion = clp->cl_minorversion;

	/*
	 * cb_seq_status is only set in decode_cb_sequence4res,
	 * and so will remain 1 if an rpc level failure occurs.
	 */
	trace_nfsd_cb_rpc_prepare(clp);
	cb->cb_seq_status = 1;
	cb->cb_status = 0;
	if (minorversion && !nfsd41_cb_get_slot(cb, task))
		return;
	rpc_call_start(task);
}

/* Returns true if CB_COMPOUND processing should continue */
static bool nfsd4_cb_sequence_done(struct rpc_task *task, struct nfsd4_callback *cb)
{
	struct nfsd4_session *session = cb->cb_clp->cl_cb_session;
	bool ret = false;

	if (cb->cb_held_slot < 0)
		goto requeue;

	/* This is the operation status code for CB_SEQUENCE */
	trace_nfsd_cb_seq_status(task, cb);
	switch (cb->cb_seq_status) {
	case 0:
		/*
		 * No need for lock, access serialized in nfsd4_cb_prepare
		 *
		 * RFC5661 20.9.3
		 * If CB_SEQUENCE returns an error, then the state of the slot
		 * (sequence ID, cached reply) MUST NOT change.
		 */
		++session->se_cb_seq_nr[cb->cb_held_slot];
		ret = true;
		break;
	case -ESERVERFAULT:
		/*
		 * Call succeeded, but the session, slot index, or slot
		 * sequence number in the response do not match the same
		 * in the server's call. The sequence information is thus
		 * untrustworthy.
		 */
		nfsd4_mark_cb_fault(cb->cb_clp);
		break;
	case 1:
		/*
		 * cb_seq_status remains 1 if an RPC Reply was never
		 * received. NFSD can't know if the client processed
		 * the CB_SEQUENCE operation. Ask the client to send a
		 * DESTROY_SESSION to recover.
		 */
		fallthrough;
	case -NFS4ERR_BADSESSION:
		nfsd4_mark_cb_fault(cb->cb_clp);
		goto requeue;
	case -NFS4ERR_DELAY:
		cb->cb_seq_status = 1;
		if (RPC_SIGNALLED(task) || !rpc_restart_call(task))
			goto requeue;
		rpc_delay(task, 2 * HZ);
		return false;
	case -NFS4ERR_SEQ_MISORDERED:
	case -NFS4ERR_BADSLOT:
		/*
		 * A SEQ_MISORDERED or BADSLOT error means that the client and
		 * server are out of sync as to the backchannel parameters. Mark
		 * the backchannel faulty and restart the RPC, but leak the slot
		 * so that it's no longer used.
		 */
		nfsd4_mark_cb_fault(cb->cb_clp);
		cb->cb_held_slot = -1;
		goto retry_nowait;
	default:
		nfsd4_mark_cb_fault(cb->cb_clp);
	}
	trace_nfsd_cb_free_slot(task, cb);
	nfsd41_cb_release_slot(cb);
	return ret;
retry_nowait:
	/*
	 * RPC_SIGNALLED() means that the rpc_client is being torn down and
	 * (possibly) recreated. Requeue the call in that case.
	 */
	if (!RPC_SIGNALLED(task)) {
		if (rpc_restart_call_prepare(task))
			return false;
	}
requeue:
	nfsd41_cb_release_slot(cb);
	nfsd4_requeue_cb(task, cb);
	return false;
}

static void nfsd4_cb_done(struct rpc_task *task, void *calldata)
{
	struct nfsd4_callback *cb = calldata;
	struct nfs4_client *clp = cb->cb_clp;

	trace_nfsd_cb_rpc_done(clp);

	if (!clp->cl_minorversion) {
		/*
		 * If the backchannel connection was shut down while this
		 * task was queued, we need to resubmit it after setting up
		 * a new backchannel connection.
		 *
		 * Note that if we lost our callback connection permanently
		 * the submission code will error out, so we don't need to
		 * handle that case here.
		 */
		if (RPC_SIGNALLED(task))
			nfsd4_requeue_cb(task, cb);
	} else if (!nfsd4_cb_sequence_done(task, cb)) {
		return;
	}

	if (cb->cb_status) {
		WARN_ONCE(task->tk_status,
			  "cb_status=%d tk_status=%d cb_opcode=%d",
			  cb->cb_status, task->tk_status, cb->cb_ops->opcode);
		task->tk_status = cb->cb_status;
	}

	switch (cb->cb_ops->done(cb, task)) {
	case 0:
		task->tk_status = 0;
		rpc_restart_call_prepare(task);
		return;
	case 1:
		switch (task->tk_status) {
		case -EIO:
		case -ETIMEDOUT:
		case -EACCES:
			nfsd4_mark_cb_down(clp);
		}
		break;
	default:
		BUG();
	}
}

static void nfsd4_cb_release(void *calldata)
{
	struct nfsd4_callback *cb = calldata;

	trace_nfsd_cb_rpc_release(cb->cb_clp);

	if (test_bit(NFSD4_CALLBACK_REQUEUE, &cb->cb_flags))
		nfsd4_queue_cb(cb);
	else
		nfsd41_destroy_cb(cb);

}

static const struct rpc_call_ops nfsd4_cb_ops = {
	.rpc_call_prepare = nfsd4_cb_prepare,
	.rpc_call_done = nfsd4_cb_done,
	.rpc_release = nfsd4_cb_release,
};

/* must be called under the state lock */
void nfsd4_shutdown_callback(struct nfs4_client *clp)
{
	if (clp->cl_cb_state != NFSD4_CB_UNKNOWN)
		trace_nfsd_cb_shutdown(clp);

	set_bit(NFSD4_CLIENT_CB_KILL, &clp->cl_flags);
	/*
	 * Note this won't actually result in a null callback;
	 * instead, nfsd4_run_cb_null() will detect the killed
	 * client, destroy the rpc client, and stop:
	 */
	nfsd4_run_cb(&clp->cl_cb_null);
	flush_workqueue(clp->cl_callback_wq);
	nfsd41_cb_inflight_wait_complete(clp);
}

static struct nfsd4_conn * __nfsd4_find_backchannel(struct nfs4_client *clp)
{
	struct nfsd4_session *s;
	struct nfsd4_conn *c;

	lockdep_assert_held(&clp->cl_lock);

	list_for_each_entry(s, &clp->cl_sessions, se_perclnt) {
		list_for_each_entry(c, &s->se_conns, cn_persession) {
			if (c->cn_flags & NFS4_CDFC4_BACK)
				return c;
		}
	}
	return NULL;
}

/*
 * Note there isn't a lot of locking in this code; instead we depend on
 * the fact that it is run from clp->cl_callback_wq, which won't run two
 * work items at once.  So, for example, clp->cl_callback_wq handles all
 * access of cl_cb_client and all calls to rpc_create or rpc_shutdown_client.
 */
static void nfsd4_process_cb_update(struct nfsd4_callback *cb)
{
	struct nfs4_cb_conn conn;
	struct nfs4_client *clp = cb->cb_clp;
	struct nfsd4_session *ses = NULL;
	struct nfsd4_conn *c;
	int err;

	trace_nfsd_cb_bc_update(clp, cb);

	/*
	 * This is either an update, or the client dying; in either case,
	 * kill the old client:
	 */
	if (clp->cl_cb_client) {
		trace_nfsd_cb_bc_shutdown(clp, cb);
		rpc_shutdown_client(clp->cl_cb_client);
		clp->cl_cb_client = NULL;
		put_cred(clp->cl_cb_cred);
		clp->cl_cb_cred = NULL;
	}
	if (clp->cl_cb_conn.cb_xprt) {
		svc_xprt_put(clp->cl_cb_conn.cb_xprt);
		clp->cl_cb_conn.cb_xprt = NULL;
	}
	if (test_bit(NFSD4_CLIENT_CB_KILL, &clp->cl_flags))
		return;

	spin_lock(&clp->cl_lock);
	/*
	 * Only serialized callback code is allowed to clear these
	 * flags; main nfsd code can only set them:
	 */
	WARN_ON(!(clp->cl_flags & NFSD4_CLIENT_CB_FLAG_MASK));
	clear_bit(NFSD4_CLIENT_CB_UPDATE, &clp->cl_flags);

	memcpy(&conn, &cb->cb_clp->cl_cb_conn, sizeof(struct nfs4_cb_conn));
	c = __nfsd4_find_backchannel(clp);
	if (c) {
		svc_xprt_get(c->cn_xprt);
		conn.cb_xprt = c->cn_xprt;
		ses = c->cn_session;
	}
	spin_unlock(&clp->cl_lock);

	err = setup_callback_client(clp, &conn, ses);
	if (err) {
		nfsd4_mark_cb_down(clp);
		if (c)
			svc_xprt_put(c->cn_xprt);
		return;
	}
}

static void
nfsd4_run_cb_work(struct work_struct *work)
{
	struct nfsd4_callback *cb =
		container_of(work, struct nfsd4_callback, cb_work);
	struct nfs4_client *clp = cb->cb_clp;
	struct rpc_clnt *clnt;
	int flags, ret;

	trace_nfsd_cb_start(clp);

	if (clp->cl_flags & NFSD4_CLIENT_CB_FLAG_MASK)
		nfsd4_process_cb_update(cb);

	clnt = clp->cl_cb_client;
	if (!clnt || clp->cl_state == NFSD4_COURTESY) {
		/*
		 * Callback channel broken, client killed or
		 * nfs4_client in courtesy state; give up.
		 */
		nfsd41_destroy_cb(cb);
		return;
	}

	/*
	 * Don't send probe messages for 4.1 or later.
	 */
	if (!cb->cb_ops && clp->cl_minorversion) {
		nfsd4_mark_cb_state(clp, NFSD4_CB_UP);
		nfsd41_destroy_cb(cb);
		return;
	}

	if (!test_and_clear_bit(NFSD4_CALLBACK_REQUEUE, &cb->cb_flags)) {
		if (cb->cb_ops && cb->cb_ops->prepare)
			cb->cb_ops->prepare(cb);
	}

	cb->cb_msg.rpc_cred = clp->cl_cb_cred;
	flags = clp->cl_minorversion ? RPC_TASK_NOCONNECT : RPC_TASK_SOFTCONN;
	ret = rpc_call_async(clnt, &cb->cb_msg, RPC_TASK_SOFT | flags,
			     cb->cb_ops ? &nfsd4_cb_ops : &nfsd4_cb_probe_ops, cb);
	if (ret != 0) {
		set_bit(NFSD4_CALLBACK_REQUEUE, &cb->cb_flags);
		nfsd4_queue_cb(cb);
	}
}

void nfsd4_init_cb(struct nfsd4_callback *cb, struct nfs4_client *clp,
		const struct nfsd4_callback_ops *ops, enum nfsd4_cb_op op)
{
	cb->cb_clp = clp;
	cb->cb_msg.rpc_proc = &nfs4_cb_procedures[op];
	cb->cb_msg.rpc_argp = cb;
	cb->cb_msg.rpc_resp = cb;
	cb->cb_flags = 0;
	cb->cb_ops = ops;
	INIT_WORK(&cb->cb_work, nfsd4_run_cb_work);
	cb->cb_status = 0;
	cb->cb_held_slot = -1;
	cb->cb_nr_referring_call_list = 0;
	INIT_LIST_HEAD(&cb->cb_referring_call_list);
}

/**
 * nfsd4_run_cb - queue up a callback job to run
 * @cb: callback to queue
 *
 * Kick off a callback to do its thing. Returns false if it was already
 * on a queue, true otherwise.
 */
bool nfsd4_run_cb(struct nfsd4_callback *cb)
{
	struct nfs4_client *clp = cb->cb_clp;
	bool queued;

	nfsd41_cb_inflight_begin(clp);
	queued = nfsd4_queue_cb(cb);
	if (!queued)
		nfsd41_cb_inflight_end(clp);
	return queued;
}
