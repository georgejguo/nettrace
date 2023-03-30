#define KBUILD_MODNAME ""
#include <kheaders.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_tracing.h>

#include "shared.h"
#include <skb_parse.h>

#include "kprobe_trace.h"
#include "kprobe.h"

#ifdef KERN_VER
__u32 kern_ver SEC("version") = KERN_VER;
#endif

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(int));
	__uint(value_size, sizeof(int));
	__uint(max_entries, TRACE_MAX);
} m_ret SEC(".maps");

#ifdef STACK_TRACE
struct {
	__uint(type, BPF_MAP_TYPE_STACK_TRACE);
	__uint(max_entries, 16384);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, sizeof(stack_trace_t));
} m_stack SEC(".maps");
#endif

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 102400);
	__uint(key_size, sizeof(u64));
	__uint(value_size, sizeof(u8));
} m_matched SEC(".maps");

static try_inline void get_ret(int func)
{
	int *ref = bpf_map_lookup_elem(&m_ret, &func);
	if (!ref)
		return;
	(*ref)++;
}

static try_inline int put_ret(int func)
{
	int *ref = bpf_map_lookup_elem(&m_ret, &func);
	if (!ref || *ref <= 0)
		return 1;
	(*ref)--;
	return 0;
}

#ifdef STACK_TRACE
static try_inline void try_trace_stack(context_t *ctx)
{
	int i = 0, key;
	u16 *funcs;

	if (!ctx->args->stack)
		return;

	funcs = ctx->args->stack_funs;

#pragma unroll
	for (; i < MAX_FUNC_STACK; i++) {
		if (!funcs[i])
			break;
		if (funcs[i] == ctx->func)
			goto do_stack;
	}
	return;

do_stack:
	key = bpf_get_stackid(ctx->regs, &m_stack, 0);
	ctx->e->stack_id = key;
}
#else
static try_inline void try_trace_stack(context_t *ctx) { }
#endif

static try_inline int handle_entry(context_t *ctx)
{
	bpf_args_t *args = (void *)ctx->args;
	struct sk_buff *skb = ctx->skb;
	bool *matched, skip_life;
	event_t *e = ctx->e;
	packet_t *pkt;
	u32 pid;

	if (!args->ready)
		return -1;

	pr_debug_skb("begin to handle, func=%d", ctx->func);
	skip_life = args->trace_mode & MODE_SKIP_LIFE_MASK;
	pid = (u32)bpf_get_current_pid_tgid();
	pkt = &e->pkt;
	if (skip_life) {
		if (!probe_parse_skb(skb, ctx->sk, pkt))
			goto skip_life;
		return -1;
	}

	matched = bpf_map_lookup_elem(&m_matched, &skb);
	if (matched && *matched) {
		probe_parse_skb_always(skb, ctx->sk, pkt);
	} else if (!ARGS_CHECK(args, pid, pid) &&
		   !probe_parse_skb(skb, ctx->sk, pkt)) {
		bool _matched = true;
		bpf_map_update_elem(&m_matched, &skb, &_matched, 0);
	} else {
		return -1;
	}

skip_life:
	if (!args->detail)
		goto out;

	/* store more (detail) information about net or task. */
	struct net_device *dev = _C(skb, dev);
	detail_event_t *detail = (void *)e;

	bpf_get_current_comm(detail->task, sizeof(detail->task));
	detail->pid = pid;
	if (dev) {
		bpf_probe_read_str(detail->ifname, sizeof(detail->ifname) - 1,
				   dev->name);
		detail->ifindex = _C(dev, ifindex);
	} else {
		detail->ifindex = _C(skb, skb_iif);
	}

out:
	pr_debug_skb("pkt matched");
	try_trace_stack(ctx);
	pkt->ts = bpf_ktime_get_ns();
	e->key = (u64)(void *)skb;
	e->func = ctx->func;

	if (ctx->size)
		EVENT_OUTPUT_PTR(ctx->regs, ctx->e, ctx->size);

	if (!skip_life)
		get_ret(ctx->func);
	return 0;
}

static try_inline int handle_destroy(context_t *ctx)
{
	if (!(ctx->args->trace_mode & MODE_SKIP_LIFE_MASK))
		bpf_map_delete_elem(&m_matched, &ctx->skb);
	return 0;
}

static try_inline int default_handle_entry(context_t *ctx)
{
#ifdef COMPAT_MODE
	if (ctx->args->detail) {
		detail_event_t e = { };
		ctx_event(ctx, e);
		handle_entry(ctx);
	} else {
		event_t e = { };
		ctx_event(ctx, e);
		handle_entry(ctx);
	}
#else
	DECLARE_EVENT(event_t, e)
	handle_entry(ctx);
#endif

	switch (ctx->func) {
	case INDEX_consume_skb:
	case INDEX___kfree_skb:
		handle_destroy(ctx);
		break;
	default:
		break;
	}

	return 0;
}

static try_inline int handle_exit(struct pt_regs *regs, int func)
{
	retevent_t event = {
		.ts = bpf_ktime_get_ns(),
		.func = func,
		.val = PT_REGS_RC(regs),
	};

	if (!ARGS_GET_CONFIG(ready) || put_ret(func))
		return 0;

	if (func == INDEX_skb_clone) {
		bool matched = true;
		bpf_map_update_elem(&m_matched, &event.val, &matched, 0);
	}

	EVENT_OUTPUT(regs, event);
	return 0;
}


/**********************************************************************
 * 
 * Following is the definntion of all kind of BPF program.
 * 
 * DEFINE_ALL_PROBES() will define all the default implement of BPF
 * program, and the customize handle of kernel function or tracepoint
 * is defined following.
 * 
 **********************************************************************/

DEFINE_ALL_PROBES(KPROBE_DEFAULT, TP_DEFAULT, FNC)

struct kfree_skb_args {
	u64 pad;
	void *skb;
	void *location;
	unsigned short protocol;
	int reason;
};

DEFINE_TP(kfree_skb, skb, kfree_skb, 8)
{
	struct kfree_skb_args *args = ctx->regs;
	DECLARE_EVENT(drop_event_t, e)

	e->location = (unsigned long)args->location;
	if (ARGS_GET_CONFIG(drop_reason))
		e->reason = _(args->reason);

	handle_entry(ctx);
	handle_destroy(ctx);
	return 0;
}

DEFINE_KPROBE_INIT(__netif_receive_skb_core_pskb,
		   __netif_receive_skb_core,
		   .skb = _(*(void **)(nt_regs(regs, 1))))
{
	return default_handle_entry(ctx);
}

static try_inline int bpf_ipt_do_table(context_t *ctx, struct xt_table *table,
				       struct nf_hook_state *state)
{
	DECLARE_EVENT(nf_event_t, e, .hook = _C(state, hook))

	bpf_probe_read(e->table, sizeof(e->table) - 1, _C(table, name));
	return handle_entry(ctx);
}

DEFINE_KPROBE_SKB_TARGET(ipt_do_table_legacy, ipt_do_table, 1)
{
	struct nf_hook_state *state = nt_regs_ctx(ctx, 2);
	struct xt_table *table = nt_regs_ctx(ctx, 3);

	return bpf_ipt_do_table(ctx, table, state);
}

DEFINE_KPROBE_SKB(ipt_do_table, 2)
{
	struct nf_hook_state *state = nt_regs_ctx(ctx, 3);
	struct xt_table *table = nt_regs_ctx(ctx, 1);

	return bpf_ipt_do_table(ctx, table, state);
}

DEFINE_KPROBE_SKB(nf_hook_slow, 1)
{
	struct nf_hook_state *state;
	size_t size;
	int num;

	state = nt_regs_ctx(ctx, 2);
	if (ctx->args->hooks)
		goto on_hooks;

	DECLARE_EVENT(nf_event_t, e)

	size = ctx->size;
	ctx->size = 0;
	if (handle_entry(ctx))
		return 0;

	e->hook = _C(state, hook);
	e->pf = _C(state, pf);
	EVENT_OUTPUT_PTR(ctx->regs, ctx->e, size);
	return 0;

on_hooks:;
	struct nf_hook_entries *entries = nt_regs_ctx(ctx, 3);
	__DECLARE_EVENT(hooks, nf_hooks_event_t, hooks_event)

	size = ctx->size;
	ctx->size = 0;
	if (handle_entry(ctx))
		return 0;

	hooks_event->hook = _C(state, hook);
	hooks_event->pf = _C(state, pf);
	num = _(entries->num_hook_entries);

#define COPY_HOOK(i) do {					\
	if (i >= num) goto out;					\
	hooks_event->hooks[i] = (u64)_(entries->hooks[i].hook);	\
} while (0)

	COPY_HOOK(0);
	COPY_HOOK(1);
	COPY_HOOK(2);
	COPY_HOOK(3);
	COPY_HOOK(4);
	COPY_HOOK(5);

	/* following code can't unroll, don't know why......:
	 * 
	 * #pragma clang loop unroll(full)
	 * 	for (i = 0; i < 8; i++)
	 * 		COPY_HOOK(i);
	 */
out:
	EVENT_OUTPUT_PTR(ctx->regs, ctx->e, size);
	return 0;
}

#ifndef NT_DISABLE_NFT
#define NFT_LEGACY 1
#include "kprobe_nft.c"
#undef NFT_LEGACY
#include "kprobe_nft.c"
#endif

#define QDISC_LEGACY 1
#include "kprobe_qdisc.c"
#undef QDISC_LEGACY
#include "kprobe_qdisc.c"

char _license[] SEC("license") = "GPL";
