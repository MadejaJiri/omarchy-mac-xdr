// SPDX-License-Identifier: GPL-2.0-only
/*
 * apple_xdr_probe - live research hooks for the Apple DCP display driver.
 *
 * The appledrm driver on 13.5 DCP firmware only ever asks the panel for the
 * SDR range (2..510 nits) and only ever submits SDR surfaces.  This module
 * lets us try the rest of the protocol on a running system, without
 * rebuilding the driver or restarting the compositor:
 *
 *  - rewrite the backlight fields of a swap (bl_raw)
 *  - retag RGB surfaces with another transfer function / colour space
 *    (xfer, cspace), e.g. 16/9 for ST 2084 (PQ) in BT.2020
 *  - patch arbitrary bytes of the swap request (poke)
 *  - record what the firmware reports and asks for (props, keys)
 *
 * Every override expires hold_ms after the last parameter write and the
 * panel is put back on the driver's own level, so a blinding or black screen
 * recovers by itself.
 *
 * Offsets come from the running kernel's BTF (see gen-layout.sh), so the
 * module must be rebuilt for each kernel.  Research tool: not for upstream.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/backlight.h>
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/hex.h>
#include <linux/jiffies.h>
#include <linux/kprobes.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "layout.h"

#define XFER_SDR	13	/* what the driver sends for RGB surfaces */
#define BL_POWER_ON	0x40

static char panel[32] = "38bc00000.dcp";
module_param_string(panel, panel, sizeof(panel), 0444);
MODULE_PARM_DESC(panel, "device name of the DCP that drives the built-in panel");

static char backlight[32] = "apple-panel-bl";
module_param_string(backlight, backlight, sizeof(backlight), 0444);

static unsigned int hold_ms = 20000;
module_param(hold_ms, uint, 0664);
MODULE_PARM_DESC(hold_ms, "overrides expire this long after the last write; 0: never");

/* hook addresses, from /proc/kallsyms (load.sh); several firmware templates each */
#define MAX_ADDRS 8
static unsigned long pr_publish[MAX_ADDRS], get_uint[MAX_ADDRS];
static unsigned long sr_set_int[MAX_ADDRS], set_fx[MAX_ADDRS];
static int n_pr_publish, n_get_uint, n_sr_set_int, n_set_fx;
module_param_array(pr_publish, ulong, &n_pr_publish, 0400);
module_param_array(get_uint, ulong, &n_get_uint, 0400);
module_param_array(sr_set_int, ulong, &n_sr_set_int, 0400);
module_param_array(set_fx, ulong, &n_set_fx, 0400);

struct poke {
	u16 off;
	u8 len;
	u8 data[16];
};
#define MAX_POKES 16

static DEFINE_RAW_SPINLOCK(lock);
/* all under lock */
static bool armed;
static u32 bl_raw;
static int bl_every;
static int xfer = -1;
static int cspace = -1;
static unsigned int surf_mask = 0xf;
static struct poke pokes[MAX_POKES];
static int n_pokes;
static char poke_text[256];
static int temp = -1;
static u64 swaps_seen, swaps_patched;
static u32 last_bl_sent, last_bl_driver;

/*
 * Range mapping: stretch the ordinary backlight scale (0..max_brightness in
 * sysfs) over [range_min, range_max], both given as encoded requests.  The
 * encoding is a small float (exponent and mantissa), so interpolating the
 * encoded value linearly gives near-geometric steps in nits: each press of a
 * brightness key changes the level by about the same ratio.  Not subject to
 * hold_ms; this is the mode meant to stay on.
 */
static unsigned int range_on;
static u32 range_min, range_max = 0x7f7d5f50;
static struct backlight_device *range_bd;

static void kick_fn(struct work_struct *work);
static void expire_fn(struct work_struct *work);
static DECLARE_WORK(kick_work, kick_fn);
static DECLARE_DELAYED_WORK(expire_work, expire_fn);

/* Have the driver present once more, so the hook sees a swap now. */
static void kick_fn(struct work_struct *work)
{
	struct backlight_device *bd = backlight_device_get_by_name(backlight);

	if (!bd) {
		pr_warn("no backlight device '%s'\n", backlight);
		return;
	}
	backlight_update_status(bd);
	put_device(&bd->dev);
}

static void expire_fn(struct work_struct *work)
{
	unsigned long flags;
	bool was;

	raw_spin_lock_irqsave(&lock, flags);
	was = armed;
	armed = false;
	raw_spin_unlock_irqrestore(&lock, flags);
	if (was) {
		pr_info("overrides expired, restoring the driver's level\n");
		schedule_work(&kick_work);
	}
}

static void arm(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&lock, flags);
	armed = true;
	raw_spin_unlock_irqrestore(&lock, flags);
	if (hold_ms)
		mod_delayed_work(system_percpu_wq, &expire_work, msecs_to_jiffies(hold_ms));
	else
		cancel_delayed_work(&expire_work);
	schedule_work(&kick_work);
}

static bool is_panel(void *dcp)
{
	struct device *dev = *(struct device **)(dcp + XDR_DCP_DEV_OFF);

	return dev && !strcmp(dev_name(dev), panel);
}

/* dcp_push(dcp, oob, call, in_len, out_len, data, cb, cookie) */
static int push_pre(struct kprobe *kp, struct pt_regs *regs)
{
	void *dcp = (void *)regs->regs[0];
	const char *tag = (const char *)regs->regs[2] + XDR_METHOD_TAG_OFF;
	u32 in_len = regs->regs[3];
	u8 *req = (u8 *)regs->regs[5];
	unsigned long flags;
	bool patched = false;
	int i;

	if (in_len != XDR_REQ_SIZE || !req || memcmp(tag, "A408", 4) || !is_panel(dcp))
		return 0;

	raw_spin_lock_irqsave(&lock, flags);
	swaps_seen++;
	if (req[XDR_BL_POWER_OFF] == BL_POWER_ON)
		last_bl_driver = get_unaligned_le32(req + XDR_BL_VALUE_OFF);
	if (range_on && range_bd && req[XDR_BL_POWER_OFF] == BL_POWER_ON &&
	    !(armed && bl_raw)) {
		u32 max = range_bd->props.max_brightness;
		u32 b = min_t(u32, range_bd->props.brightness, max);
		u32 lo = min(range_min, range_max);
		u32 v = lo + (max ? div_u64((u64)(range_max - lo) * b, max) : 0);

		put_unaligned_le32(v, req + XDR_BL_VALUE_OFF);
		last_bl_sent = v;
		swaps_patched++;
	}
	if (!armed)
		goto out;

	if (bl_raw && (bl_every || req[XDR_BL_POWER_OFF] == BL_POWER_ON)) {
		put_unaligned_le64(1, req + XDR_BL_UNK_OFF);
		put_unaligned_le32(bl_raw, req + XDR_BL_VALUE_OFF);
		req[XDR_BL_POWER_OFF] = BL_POWER_ON;
		last_bl_sent = bl_raw;
		patched = true;
	}

	for (i = 0; i < XDR_SWAP_SURFACES; i++) {
		u8 *surf = req + XDR_SURF_OFF + i * XDR_SURF_STRIDE;

		if (!(surf_mask & BIT(i)) || req[XDR_SURF_NULL_OFF + i] ||
		    surf[XDR_SURF_XFER_OFF] != XFER_SDR)
			continue;
		if (cspace >= 0) {
			surf[XDR_SURF_CSPACE_OFF] = cspace;
			patched = true;
		}
		if (xfer >= 0) {
			surf[XDR_SURF_XFER_OFF] = xfer;
			patched = true;
		}
	}

	for (i = 0; i < n_pokes; i++) {
		memcpy(req + pokes[i].off, pokes[i].data, pokes[i].len);
		patched = true;
	}
	if (patched)
		swaps_patched++;
out:
	raw_spin_unlock_irqrestore(&lock, flags);
	return 0;
}

static struct kprobe push_kp = {
	.symbol_name = "appledrm:dcp_push",
	.pre_handler = push_pre,
};

/* What the firmware publishes: property id -> last value. */
#define MAX_PROPS 48
static struct {
	u32 id, value, first, count;
} props[MAX_PROPS];
static int n_props;

/* What the firmware asks for or sets by name. */
#define MAX_KEYS 64
static struct {
	char kind;
	char obj[5];
	char key[65];
	u64 value;
	u32 count;
} keys[MAX_KEYS];
static int n_keys;

static void note_key(char kind, const u8 *in, u64 value)
{
	unsigned long flags;
	char obj[5], key[65];
	int i;

	/* object tags are reversed fourccs */
	obj[0] = in[3]; obj[1] = in[2]; obj[2] = in[1]; obj[3] = in[0]; obj[4] = 0;
	strscpy(key, in + 4, sizeof(key));

	raw_spin_lock_irqsave(&lock, flags);
	for (i = 0; i < n_keys; i++)
		if (keys[i].kind == kind && !strcmp(keys[i].obj, obj) &&
		    !strcmp(keys[i].key, key))
			break;
	if (i == n_keys) {
		if (n_keys == MAX_KEYS)
			goto out;
		n_keys++;
		keys[i].kind = kind;
		strscpy(keys[i].obj, obj, sizeof(keys[i].obj));
		strscpy(keys[i].key, key, sizeof(keys[i].key));
		keys[i].count = 0;
	}
	keys[i].value = value;
	keys[i].count++;
out:
	raw_spin_unlock_irqrestore(&lock, flags);
}

/* All firmware callbacks are bool trampoline(dcp, tag, out, in). */
static int pr_publish_pre(struct kprobe *kp, struct pt_regs *regs)
{
	const u8 *in = (const u8 *)regs->regs[3];
	unsigned long flags;
	u32 id, value;
	int i;

	if (!in || !is_panel((void *)regs->regs[0]))
		return 0;
	id = get_unaligned_le32(in);
	value = get_unaligned_le32(in + 4);

	raw_spin_lock_irqsave(&lock, flags);
	for (i = 0; i < n_props; i++)
		if (props[i].id == id)
			break;
	if (i == n_props) {
		if (n_props == MAX_PROPS)
			goto out;
		n_props++;
		props[i].id = id;
		props[i].first = value;
		props[i].count = 0;
	}
	props[i].value = value;
	props[i].count++;
out:
	raw_spin_unlock_irqrestore(&lock, flags);
	return 0;
}

static int get_uint_pre(struct kprobe *kp, struct pt_regs *regs)
{
	const u8 *in = (const u8 *)regs->regs[3];

	if (in && is_panel((void *)regs->regs[0]))
		note_key('g', in, 0);
	return 0;
}

static int sr_set_int_pre(struct kprobe *kp, struct pt_regs *regs)
{
	const u8 *in = (const u8 *)regs->regs[3];

	if (in && is_panel((void *)regs->regs[0]))
		note_key('s', in, get_unaligned_le64(in + 68));
	return 0;
}

static int set_fx_pre(struct kprobe *kp, struct pt_regs *regs)
{
	const u8 *in = (const u8 *)regs->regs[3];

	if (in && is_panel((void *)regs->regs[0]))
		note_key('f', in, get_unaligned_le32(in + 68));
	return 0;
}

/*
 * The driver answers the firmware's "PMUS"/"Temperature" query with a
 * constant.  temp >= 0 answers with that value instead (same unit, 1/100 C),
 * so a daemon can feed the real sensor.  The response is { u64 value; u8 ret }
 * written by the handler to `out`; we patch it when the trampoline returns.
 */
struct get_uint_ctx {
	void *out;
	bool match;
};

static int get_uint_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct get_uint_ctx *ctx = (void *)ri->data;
	const u8 *in = (const u8 *)regs->regs[3];

	ctx->out = (void *)regs->regs[2];
	ctx->match = in && is_panel((void *)regs->regs[0]) &&
		!memcmp(in, "SUMP", 4) && !strncmp(in + 4, "Temperature", 64);
	return 0;
}

static int get_uint_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct get_uint_ctx *ctx = (void *)ri->data;
	int t = READ_ONCE(temp);

	if (ctx->match && ctx->out && t >= 0)
		put_unaligned_le64(t, ctx->out);
	return 0;
}

static struct kprobe cb_kps[4 * MAX_ADDRS];
static struct kretprobe temp_krps[MAX_ADDRS];
static int n_cb_kps, n_temp_krps;

static void add_cb_probes(unsigned long *addrs, int n, kprobe_pre_handler_t pre)
{
	int i, ret;

	for (i = 0; i < n; i++) {
		struct kprobe *kp = &cb_kps[n_cb_kps];

		kp->addr = (kprobe_opcode_t *)addrs[i];
		kp->pre_handler = pre;
		ret = register_kprobe(kp);
		if (ret)
			pr_warn("kprobe at %lx failed: %d\n", addrs[i], ret);
		else
			n_cb_kps++;
	}
}

/* ---- parameters ---- */

static int set_and_arm(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (!ret)
		arm();
	return ret;
}

static const struct kernel_param_ops arm_int_ops = {
	.set = set_and_arm,
	.get = param_get_int,
};

static int set_bl_raw(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);

	if (!ret)
		arm();
	return ret;
}

static const struct kernel_param_ops bl_raw_ops = {
	.set = set_bl_raw,
	.get = param_get_uint,
};

static int set_range(const char *val, const struct kernel_param *kp)
{
	struct backlight_device *bd;
	int ret = param_set_uint(val, kp);

	if (ret)
		return ret;
	if (!READ_ONCE(range_bd)) {
		bd = backlight_device_get_by_name(backlight);
		if (bd && cmpxchg(&range_bd, NULL, bd))
			put_device(&bd->dev);
	}
	schedule_work(&kick_work);
	return 0;
}

static const struct kernel_param_ops range_ops = {
	.set = set_range,
	.get = param_get_uint,
};
module_param_cb(range_on, &range_ops, &range_on, 0664);
MODULE_PARM_DESC(range_on, "1: map the backlight scale onto [range_min, range_max]");
module_param_cb(range_min, &range_ops, &range_min, 0664);
module_param_cb(range_max, &range_ops, &range_max, 0664);
MODULE_PARM_DESC(range_max, "encoded request sent at 100% brightness");

module_param_cb(bl_raw, &bl_raw_ops, &bl_raw, 0664);
MODULE_PARM_DESC(bl_raw, "send this bl_value instead of the driver's; 0: off");
module_param_cb(bl_every, &arm_int_ops, &bl_every, 0664);
MODULE_PARM_DESC(bl_every, "1: attach bl_raw to every swap, not only brightness updates");
module_param_cb(xfer, &arm_int_ops, &xfer, 0664);
MODULE_PARM_DESC(xfer, "transfer function for RGB surfaces (13 sRGB, 16 PQ, 18 HLG); -1: off");
module_param_cb(cspace, &arm_int_ops, &cspace, 0664);
MODULE_PARM_DESC(cspace, "colour space for RGB surfaces (9 BT.2020, 12 native); -1: off");
module_param_cb(temp, &arm_int_ops, &temp, 0664);
MODULE_PARM_DESC(temp, "answer the firmware's panel temperature query with this; -1: driver default");
module_param(surf_mask, uint, 0664);

/* poke: "off=hexbytes[,off=hexbytes...]" into the swap request; "" clears */
static int set_poke(const char *val, const struct kernel_param *kp)
{
	struct poke tmp[MAX_POKES];
	char buf[sizeof(poke_text)], *s, *tok;
	unsigned long flags;
	int n = 0;

	if (strscpy(buf, val, sizeof(buf)) < 0)
		return -E2BIG;
	s = strim(buf);
	while ((tok = strsep(&s, ",")) != NULL) {
		char *eq = strchr(tok, '=');
		unsigned int off;
		size_t hexlen;

		if (!*tok)
			continue;
		if (!eq || n == MAX_POKES)
			return -EINVAL;
		*eq++ = 0;
		if (kstrtouint(tok, 0, &off))
			return -EINVAL;
		hexlen = strlen(eq);
		if (!hexlen || hexlen % 2 || hexlen / 2 > sizeof(tmp[n].data) ||
		    off + hexlen / 2 > XDR_REQ_SIZE)
			return -EINVAL;
		if (hex2bin(tmp[n].data, eq, hexlen / 2))
			return -EINVAL;
		tmp[n].off = off;
		tmp[n].len = hexlen / 2;
		n++;
	}

	raw_spin_lock_irqsave(&lock, flags);
	memcpy(pokes, tmp, sizeof(pokes));
	n_pokes = n;
	strscpy(poke_text, val, sizeof(poke_text));
	strim(poke_text);
	raw_spin_unlock_irqrestore(&lock, flags);
	arm();
	return 0;
}

static int get_poke(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%s\n", poke_text);
}

static const struct kernel_param_ops poke_ops = {
	.set = set_poke,
	.get = get_poke,
};
module_param_cb(poke, &poke_ops, NULL, 0664);
MODULE_PARM_DESC(poke, "off=hexbytes[,...] patched into each swap request");

static int get_status(char *buf, const struct kernel_param *kp)
{
	unsigned long flags;
	int len;

	raw_spin_lock_irqsave(&lock, flags);
	len = sysfs_emit(buf,
			 "armed=%d swaps_seen=%llu swaps_patched=%llu bl_driver=0x%08x bl_sent=0x%08x\n",
			 armed, swaps_seen, swaps_patched, last_bl_driver, last_bl_sent);
	raw_spin_unlock_irqrestore(&lock, flags);
	return len;
}

static const struct kernel_param_ops status_ops = { .get = get_status };
module_param_cb(status, &status_ops, NULL, 0444);

static int get_props(char *buf, const struct kernel_param *kp)
{
	unsigned long flags;
	int i, len = 0;

	raw_spin_lock_irqsave(&lock, flags);
	for (i = 0; i < n_props; i++)
		len += sysfs_emit_at(buf, len, "prop %u = %u (0x%x) first=%u n=%u\n",
				     props[i].id, props[i].value, props[i].value,
				     props[i].first, props[i].count);
	raw_spin_unlock_irqrestore(&lock, flags);
	return len;
}

static const struct kernel_param_ops props_ops = { .get = get_props };
module_param_cb(props, &props_ops, NULL, 0444);

static int get_keys(char *buf, const struct kernel_param *kp)
{
	unsigned long flags;
	int i, len = 0;

	raw_spin_lock_irqsave(&lock, flags);
	for (i = 0; i < n_keys; i++)
		len += sysfs_emit_at(buf, len, "%c %s/%s = %llu (0x%llx) n=%u\n",
				     keys[i].kind, keys[i].obj, keys[i].key,
				     keys[i].value, keys[i].value, keys[i].count);
	raw_spin_unlock_irqrestore(&lock, flags);
	return len;
}

static const struct kernel_param_ops keys_ops = { .get = get_keys };
module_param_cb(keys, &keys_ops, NULL, 0444);

/*
 * The firmware logs the level it actually reached ("mNits" events on the
 * system endpoint).  The driver clamps that to the device tree maximum before
 * showing it as actual_brightness; keep the raw values.
 * parse_system_log_mnits(handle, struct dcp_system_ev_mnits *entry)
 */
#define MAX_MNITS 12
static struct {
	u32 millinits, idac;
	unsigned long when;
} mnits_log[MAX_MNITS];
static u64 n_mnits;

static int mnits_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	*(void **)ri->data = (void *)regs->regs[1];
	return 0;
}

static int mnits_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	const u32 *ev = *(void **)ri->data;
	unsigned long flags;
	int i;

	if (regs_return_value(regs) || !ev)
		return 0;
	raw_spin_lock_irqsave(&lock, flags);
	i = n_mnits++ % MAX_MNITS;
	mnits_log[i].millinits = ev[XDR_MNITS_MILLINITS_OFF / 4];
	mnits_log[i].idac = ev[XDR_MNITS_IDAC_OFF / 4];
	mnits_log[i].when = jiffies;
	raw_spin_unlock_irqrestore(&lock, flags);
	return 0;
}

static struct kretprobe mnits_krp = {
	.kp.symbol_name = "appledrm:parse_system_log_mnits",
	.entry_handler = mnits_entry,
	.handler = mnits_ret,
	.data_size = sizeof(void *),
	.maxactive = 4,
};
static bool mnits_hooked;

static int get_mnits(char *buf, const struct kernel_param *kp)
{
	unsigned long flags;
	int k, len = 0;
	u64 n;

	raw_spin_lock_irqsave(&lock, flags);
	n = n_mnits;
	for (k = 0; k < MAX_MNITS && k < n; k++) {
		int i = (n - 1 - k) % MAX_MNITS;

		len += sysfs_emit_at(buf, len, "%u.%03u nits idac=%u age_ms=%u\n",
				     mnits_log[i].millinits / 1000,
				     mnits_log[i].millinits % 1000, mnits_log[i].idac,
				     jiffies_to_msecs(jiffies - mnits_log[i].when));
	}
	raw_spin_unlock_irqrestore(&lock, flags);
	return len;
}

static const struct kernel_param_ops mnits_ops = { .get = get_mnits };
module_param_cb(mnits, &mnits_ops, NULL, 0444);
MODULE_PARM_DESC(mnits, "levels the firmware reported, newest first, unclamped");

/* Writing anything drops every override at once. */
static int set_reset(const char *val, const struct kernel_param *kp)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&lock, flags);
	armed = false;
	bl_raw = 0;
	bl_every = 0;
	xfer = -1;
	cspace = -1;
	n_pokes = 0;
	poke_text[0] = 0;
	raw_spin_unlock_irqrestore(&lock, flags);
	WRITE_ONCE(temp, -1);
	cancel_delayed_work(&expire_work);
	schedule_work(&kick_work);
	return 0;
}

static const struct kernel_param_ops reset_ops = { .set = set_reset };
module_param_cb(reset, &reset_ops, NULL, 0220);

static int __init xdr_probe_init(void)
{
	int i, ret;

	ret = register_kprobe(&push_kp);
	if (ret) {
		pr_err("cannot hook dcp_push: %d\n", ret);
		return ret;
	}

	add_cb_probes(pr_publish, n_pr_publish, pr_publish_pre);
	add_cb_probes(sr_set_int, n_sr_set_int, sr_set_int_pre);
	add_cb_probes(set_fx, n_set_fx, set_fx_pre);
	add_cb_probes(get_uint, n_get_uint, get_uint_pre);

	for (i = 0; i < n_get_uint; i++) {
		struct kretprobe *rp = &temp_krps[n_temp_krps];

		rp->kp.addr = (kprobe_opcode_t *)get_uint[i];
		rp->entry_handler = get_uint_entry;
		rp->handler = get_uint_ret;
		rp->data_size = sizeof(struct get_uint_ctx);
		rp->maxactive = 4;
		ret = register_kretprobe(rp);
		if (ret)
			pr_warn("kretprobe at %lx failed: %d\n", get_uint[i], ret);
		else
			n_temp_krps++;
	}

	ret = register_kretprobe(&mnits_krp);
	if (ret)
		pr_warn("cannot hook parse_system_log_mnits: %d\n", ret);
	else
		mnits_hooked = true;

	pr_info("hooked dcp_push and %d callbacks for %s (swap request %u bytes)\n",
		n_cb_kps, panel, XDR_REQ_SIZE);
	return 0;
}

static void __exit xdr_probe_exit(void)
{
	unsigned long flags;
	int i;

	cancel_delayed_work_sync(&expire_work);
	raw_spin_lock_irqsave(&lock, flags);
	armed = false;
	range_on = 0;
	raw_spin_unlock_irqrestore(&lock, flags);
	schedule_work(&kick_work);
	flush_work(&kick_work);
	if (range_bd)
		put_device(&range_bd->dev);

	if (mnits_hooked)
		unregister_kretprobe(&mnits_krp);
	for (i = 0; i < n_temp_krps; i++)
		unregister_kretprobe(&temp_krps[i]);
	for (i = 0; i < n_cb_kps; i++)
		unregister_kprobe(&cb_kps[i]);
	unregister_kprobe(&push_kp);
}

module_init(xdr_probe_init);
module_exit(xdr_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Live research hooks for Apple DCP brightness and HDR");
