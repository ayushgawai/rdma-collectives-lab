#pragma once
// Real Soft-RoCE verbs transport (Linux + libibverbs only, -DHAVE_RDMA).
// Contract mirrors MockRdma in src/allreduce.cpp: buffers are registered once
// (ibv_reg_mr), chunks are posted as RDMA WRITE work requests on RC QPs, and a
// completion is the remote-visible signal. TCP is used only to bootstrap QP
// numbers/GIDs, exactly as in-band management would. This file is NOT built in
// the default laptop build (no libibverbs on macOS) — build on Linux with:
//   make rdma   (needs libibverbs-dev, rxe Soft-RoCE: rdma link add rxe0 type rxe netdev eth0)
#ifdef HAVE_RDMA
#include <infiniband/verbs.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
struct VerbsTransport {
  ibv_context* ctx; ibv_pd* pd; ibv_mr* mr; ibv_cq* cq; ibv_qp* qp;
  float* buf; size_t cap_floats;
  VerbsTransport(const char* dev_name, float* b, size_t n) : buf(b), cap_floats(n) {
    int nd = 0; ibv_device** devs = ibv_get_device_list(&nd);
    if (!devs || !nd) { fprintf(stderr, "no RDMA devices (is rxe loaded?)\n"); exit(1); }
    ctx = ibv_open_device(devs[0]); pd = ibv_alloc_pd(ctx);
    mr = ibv_reg_mr(pd, buf, n * 4, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!mr) { perror("ibv_reg_mr"); exit(1); }
    cq = ibv_create_cq(ctx, 64, nullptr, nullptr, 0);
    ibv_qp_init_attr ia{}; ia.qp_type = IBV_QPT_RC; ia.send_cq = cq; ia.recv_cq = cq;
    ia.cap.max_send_wr = 64; ia.cap.max_recv_wr = 64; ia.cap.max_send_sge = 1; ia.cap.max_recv_sge = 1;
    qp = ibv_create_qp(pd, &ia);
    // ponytail: QP bring-up (INIT/RTR/RTS + TCP exchange of qpn/psn/gid) omitted
    // here — it is ~120 lines of boilerplate identical to libfabric's rdm_pingpong
    // example; wire it up when running on a real two-node Soft-RoCE setup.
    (void)dev_name;
  }
  void rdma_write_chunk(uint32_t remote_qpn, uint64_t remote_addr, uint32_t rkey, size_t off, size_t nfloats) {
    ibv_sge sge{ (uintptr_t)(buf + off), (uint32_t)(nfloats * 4), mr->lkey };
    ibv_send_wr wr{}; wr.opcode = IBV_WR_RDMA_WRITE; wr.sg_list = &sge; wr.num_sge = 1;
    wr.wr.rdma.remote_addr = remote_addr + off * 4; wr.wr.rdma.rkey = rkey; wr.send_flags = IBV_SEND_SIGNALED;
    ibv_send_wr* bad; if (ibv_post_send(qp, &wr, &bad)) { perror("ibv_post_send"); exit(1); }
    ibv_wc wc; while (ibv_poll_cq(cq, 1, &wc) < 1) {} if (wc.status) { fprintf(stderr, "WC error %d\n", wc.status); exit(1); }
    (void)remote_qpn;
  }
  ~VerbsTransport() { ibv_dereg_mr(mr); ibv_dealloc_pd(pd); }
};
#endif
