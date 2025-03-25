// SPDX-License-Identifier: GPL-2.0
#include <test_progs.h>
#include <network_helpers.h>
#include "test_xdp_context_test_run.skel.h"
#include "test_xdp_meta.skel.h"

#define TX_ADDR "10.0.0.1"
#define RX_ADDR "10.0.0.2"
#define RX_NAME "veth0"
#define TX_NAME "veth1"
#define TX_NETNS "xdp_context_tx"
#define RX_NETNS "xdp_context_rx"

void test_xdp_context_error(int prog_fd, struct bpf_test_run_opts opts,
			    __u32 data_meta, __u32 data, __u32 data_end,
			    __u32 ingress_ifindex, __u32 rx_queue_index,
			    __u32 egress_ifindex)
{
	struct xdp_md ctx = {
		.data = data,
		.data_end = data_end,
		.data_meta = data_meta,
		.ingress_ifindex = ingress_ifindex,
		.rx_queue_index = rx_queue_index,
		.egress_ifindex = egress_ifindex,
	};
	int err;

	opts.ctx_in = &ctx;
	opts.ctx_size_in = sizeof(ctx);
	err = bpf_prog_test_run_opts(prog_fd, &opts);
	ASSERT_EQ(errno, EINVAL, "errno-EINVAL");
	ASSERT_ERR(err, "bpf_prog_test_run");
}

void test_xdp_context_test_run(void)
{
	struct test_xdp_context_test_run *skel = NULL;
	char data[sizeof(pkt_v4) + sizeof(__u32)];
	char bad_ctx[sizeof(struct xdp_md) + 1];
	struct xdp_md ctx_in, ctx_out;
	DECLARE_LIBBPF_OPTS(bpf_test_run_opts, opts,
			    .data_in = &data,
			    .data_size_in = sizeof(data),
			    .ctx_out = &ctx_out,
			    .ctx_size_out = sizeof(ctx_out),
			    .repeat = 1,
		);
	int err, prog_fd;

	skel = test_xdp_context_test_run__open_and_load();
	if (!ASSERT_OK_PTR(skel, "skel"))
		return;
	prog_fd = bpf_program__fd(skel->progs.xdp_context);

	/* Data past the end of the kernel's struct xdp_md must be 0 */
	bad_ctx[sizeof(bad_ctx) - 1] = 1;
	opts.ctx_in = bad_ctx;
	opts.ctx_size_in = sizeof(bad_ctx);
	err = bpf_prog_test_run_opts(prog_fd, &opts);
	ASSERT_EQ(errno, E2BIG, "extradata-errno");
	ASSERT_ERR(err, "bpf_prog_test_run(extradata)");

	*(__u32 *)data = XDP_PASS;
	*(struct ipv4_packet *)(data + sizeof(__u32)) = pkt_v4;
	opts.ctx_in = &ctx_in;
	opts.ctx_size_in = sizeof(ctx_in);
	memset(&ctx_in, 0, sizeof(ctx_in));
	ctx_in.data_meta = 0;
	ctx_in.data = sizeof(__u32);
	ctx_in.data_end = ctx_in.data + sizeof(pkt_v4);
	err = bpf_prog_test_run_opts(prog_fd, &opts);
	ASSERT_OK(err, "bpf_prog_test_run(valid)");
	ASSERT_EQ(opts.retval, XDP_PASS, "valid-retval");
	ASSERT_EQ(opts.data_size_out, sizeof(pkt_v4), "valid-datasize");
	ASSERT_EQ(opts.ctx_size_out, opts.ctx_size_in, "valid-ctxsize");
	ASSERT_EQ(ctx_out.data_meta, 0, "valid-datameta");
	ASSERT_EQ(ctx_out.data, 0, "valid-data");
	ASSERT_EQ(ctx_out.data_end, sizeof(pkt_v4), "valid-dataend");

	/* Meta data's size must be a multiple of 4 */
	test_xdp_context_error(prog_fd, opts, 0, 1, sizeof(data), 0, 0, 0);

	/* data_meta must reference the start of data */
	test_xdp_context_error(prog_fd, opts, 4, sizeof(__u32), sizeof(data),
			       0, 0, 0);

	/* Meta data must be 255 bytes or smaller */
	test_xdp_context_error(prog_fd, opts, 0, 256, sizeof(data), 0, 0, 0);

	/* Total size of data must match data_end - data_meta */
	test_xdp_context_error(prog_fd, opts, 0, sizeof(__u32),
			       sizeof(data) - 1, 0, 0, 0);
	test_xdp_context_error(prog_fd, opts, 0, sizeof(__u32),
			       sizeof(data) + 1, 0, 0, 0);

	/* RX queue cannot be specified without specifying an ingress */
	test_xdp_context_error(prog_fd, opts, 0, sizeof(__u32), sizeof(data),
			       0, 1, 0);

	/* Interface 1 is always the loopback interface which always has only
	 * one RX queue (index 0). This makes index 1 an invalid rx queue index
	 * for interface 1.
	 */
	test_xdp_context_error(prog_fd, opts, 0, sizeof(__u32), sizeof(data),
			       1, 1, 0);

	/* The egress cannot be specified */
	test_xdp_context_error(prog_fd, opts, 0, sizeof(__u32), sizeof(data),
			       0, 0, 1);

	test_xdp_context_test_run__destroy(skel);
}

void test_xdp_context_functional(void)
{
	LIBBPF_OPTS(bpf_tc_hook, tc_hook, .attach_point = BPF_TC_INGRESS);
	LIBBPF_OPTS(bpf_tc_opts, tc_opts, .handle = 1, .priority = 1);
	struct netns_obj *rx_ns = NULL, *tx_ns = NULL;
	struct bpf_program *tc_prog, *xdp_prog;
	struct test_xdp_meta *skel = NULL;
	struct nstoken *nstoken = NULL;
	int rx_ifindex;
	int ret;

	tx_ns = netns_new(TX_NETNS, false);
	if (!ASSERT_OK_PTR(tx_ns, "create tx_ns"))
		return;

	rx_ns = netns_new(RX_NETNS, false);
	if (!ASSERT_OK_PTR(rx_ns, "create rx_ns"))
		goto close;

	SYS(close, "ip link add " RX_NAME " netns " RX_NETNS
	    " type veth peer name " TX_NAME " netns " TX_NETNS);

	nstoken = open_netns(RX_NETNS);
	if (!ASSERT_OK_PTR(nstoken, "setns rx_ns"))
		goto close;

	SYS(close, "ip addr add " RX_ADDR "/24 dev " RX_NAME);
	SYS(close, "ip link set dev " RX_NAME " up");

	skel = test_xdp_meta__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open and load skeleton"))
		goto close;

	rx_ifindex = if_nametoindex(RX_NAME);
	if (!ASSERT_GE(rx_ifindex, 0, "if_nametoindex rx"))
		goto close;

	tc_hook.ifindex = rx_ifindex;
	ret = bpf_tc_hook_create(&tc_hook);
	if (!ASSERT_OK(ret, "bpf_tc_hook_create"))
		goto close;

	tc_prog = bpf_object__find_program_by_name(skel->obj, "ing_cls");
	if (!ASSERT_OK_PTR(tc_prog, "open ing_cls prog"))
		goto close;

	tc_opts.prog_fd = bpf_program__fd(tc_prog);
	ret = bpf_tc_attach(&tc_hook, &tc_opts);
	if (!ASSERT_OK(ret, "bpf_tc_attach"))
		goto close;

	xdp_prog = bpf_object__find_program_by_name(skel->obj, "ing_xdp");
	if (!ASSERT_OK_PTR(xdp_prog, "open ing_xdp prog"))
		goto close;

	ret = bpf_xdp_attach(rx_ifindex,
			     bpf_program__fd(xdp_prog),
			     0, NULL);
	if (!ASSERT_GE(ret, 0, "bpf_xdp_attach"))
		goto close;

	close_netns(nstoken);

	nstoken = open_netns(TX_NETNS);
	if (!ASSERT_OK_PTR(nstoken, "setns tx_ns"))
		goto close;

	SYS(close, "ip addr add " TX_ADDR "/24 dev " TX_NAME);
	SYS(close, "ip link set dev " TX_NAME " up");
	ASSERT_OK(SYS_NOFAIL("ping -c 1 " RX_ADDR), "ping");

close:
	close_netns(nstoken);
	test_xdp_meta__destroy(skel);
	netns_free(rx_ns);
	netns_free(tx_ns);
}

