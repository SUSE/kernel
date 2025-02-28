// SPDX-License-Identifier: GPL-2.0-only
/*
 * x86 APERF/MPERF KHz calculation for
 * /sys/.../cpufreq/scaling_cur_freq
 *
 * Copyright (C) 2017 Intel Corp.
 * Author: Len Brown <len.brown@intel.com>
 */
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/sched/isolation.h>
#include <linux/sched/topology.h>
#include <linux/smp.h>
#include <linux/syscore_ops.h>

#include <asm/cpu_device_id.h>
#include <asm/intel-family.h>

#include "cpu.h"

struct aperfmperf_sample {
	unsigned int	khz;
	atomic_t	scfpending;
	ktime_t	time;
	u64	aperf;
	u64	mperf;
};

static DEFINE_PER_CPU(struct aperfmperf_sample, samples);

#define APERFMPERF_CACHE_THRESHOLD_MS	10
#define APERFMPERF_REFRESH_DELAY_MS	10
#define APERFMPERF_STALE_THRESHOLD_MS	1000

/*
 * aperfmperf_snapshot_khz()
 * On the current CPU, snapshot APERF, MPERF, and jiffies
 * unless we already did it within 10ms
 * calculate kHz, save snapshot
 */
static void aperfmperf_snapshot_khz(void *dummy)
{
	u64 aperf, aperf_delta;
	u64 mperf, mperf_delta;
	struct aperfmperf_sample *s = this_cpu_ptr(&samples);
	unsigned long flags;

	local_irq_save(flags);
	rdmsrl(MSR_IA32_APERF, aperf);
	rdmsrl(MSR_IA32_MPERF, mperf);
	local_irq_restore(flags);

	aperf_delta = aperf - s->aperf;
	mperf_delta = mperf - s->mperf;

	/*
	 * There is no architectural guarantee that MPERF
	 * increments faster than we can read it.
	 */
	if (mperf_delta == 0)
		return;

	s->time = ktime_get();
	s->aperf = aperf;
	s->mperf = mperf;
	s->khz = div64_u64((cpu_khz * aperf_delta), mperf_delta);
	atomic_set_release(&s->scfpending, 0);
}

static bool aperfmperf_snapshot_cpu(int cpu, ktime_t now, bool wait)
{
	s64 time_delta = ktime_ms_delta(now, per_cpu(samples.time, cpu));
	struct aperfmperf_sample *s = per_cpu_ptr(&samples, cpu);

	/* Don't bother re-computing within the cache threshold time. */
	if (time_delta < APERFMPERF_CACHE_THRESHOLD_MS)
		return true;

	if (!atomic_xchg(&s->scfpending, 1) || wait)
		smp_call_function_single(cpu, aperfmperf_snapshot_khz, NULL, wait);

	/* Return false if the previous iteration was too long ago. */
	return time_delta <= APERFMPERF_STALE_THRESHOLD_MS;
}

unsigned int aperfmperf_get_khz(int cpu)
{
	if (!cpu_khz)
		return 0;

	if (!boot_cpu_has(X86_FEATURE_APERFMPERF))
		return 0;

	if (!housekeeping_cpu(cpu, HK_FLAG_MISC))
		return 0;

	if (rcu_is_idle_cpu(cpu))
		return 0; /* Idle CPUs are completely uninteresting. */

	aperfmperf_snapshot_cpu(cpu, ktime_get(), true);
	return per_cpu(samples.khz, cpu);
}

void arch_freq_prepare_all(void)
{
	ktime_t now = ktime_get();
	bool wait = false;
	int cpu;

	if (!cpu_khz)
		return;

	if (!boot_cpu_has(X86_FEATURE_APERFMPERF))
		return;

	for_each_online_cpu(cpu) {
		if (!housekeeping_cpu(cpu, HK_FLAG_MISC))
			continue;
		if (rcu_is_idle_cpu(cpu))
			continue; /* Idle CPUs are completely uninteresting. */
		if (!aperfmperf_snapshot_cpu(cpu, now, false))
			wait = true;
	}

	if (wait)
		msleep(APERFMPERF_REFRESH_DELAY_MS);
}

unsigned int arch_freq_get_on_cpu(int cpu)
{
	struct aperfmperf_sample *s = per_cpu_ptr(&samples, cpu);

	if (!cpu_khz)
		return 0;

	if (!boot_cpu_has(X86_FEATURE_APERFMPERF))
		return 0;

	if (!housekeeping_cpu(cpu, HK_FLAG_MISC))
		return 0;

	if (rcu_is_idle_cpu(cpu))
		return 0;

	if (aperfmperf_snapshot_cpu(cpu, ktime_get(), true))
		return per_cpu(samples.khz, cpu);

	msleep(APERFMPERF_REFRESH_DELAY_MS);
	atomic_set(&s->scfpending, 1);
	smp_mb(); /* ->scfpending before smp_call_function_single(). */
	smp_call_function_single(cpu, aperfmperf_snapshot_khz, NULL, 1);

	return per_cpu(samples.khz, cpu);
}

#if defined(CONFIG_X86_64) && defined(CONFIG_SMP)
/*
 * APERF/MPERF frequency ratio computation.
 *
 * The scheduler wants to do frequency invariant accounting and needs a <1
 * ratio to account for the 'current' frequency, corresponding to
 * freq_curr / freq_max.
 *
 * Since the frequency freq_curr on x86 is controlled by micro-controller and
 * our P-state setting is little more than a request/hint, we need to observe
 * the effective frequency 'BusyMHz', i.e. the average frequency over a time
 * interval after discarding idle time. This is given by:
 *
 *   BusyMHz = delta_APERF / delta_MPERF * freq_base
 *
 * where freq_base is the max non-turbo P-state.
 *
 * The freq_max term has to be set to a somewhat arbitrary value, because we
 * can't know which turbo states will be available at a given point in time:
 * it all depends on the thermal headroom of the entire package. We set it to
 * the turbo level with 4 cores active.
 *
 * Benchmarks show that's a good compromise between the 1C turbo ratio
 * (freq_curr/freq_max would rarely reach 1) and something close to freq_base,
 * which would ignore the entire turbo range (a conspicuous part, making
 * freq_curr/freq_max always maxed out).
 *
 * An exception to the heuristic above is the Atom uarch, where we choose the
 * highest turbo level for freq_max since Atom's are generally oriented towards
 * power efficiency.
 *
 * Setting freq_max to anything less than the 1C turbo ratio makes the ratio
 * freq_curr / freq_max to eventually grow >1, in which case we clip it to 1.
 */

DEFINE_STATIC_KEY_FALSE(arch_scale_freq_key);

static DEFINE_PER_CPU(u64, arch_prev_aperf);
static DEFINE_PER_CPU(u64, arch_prev_mperf);
static u64 arch_turbo_freq_ratio = SCHED_CAPACITY_SCALE;
static u64 arch_max_freq_ratio = SCHED_CAPACITY_SCALE;

void arch_set_max_freq_ratio(bool turbo_disabled)
{
	arch_max_freq_ratio = turbo_disabled ? SCHED_CAPACITY_SCALE :
					arch_turbo_freq_ratio;
}
EXPORT_SYMBOL_GPL(arch_set_max_freq_ratio);

static bool __init turbo_disabled(void)
{
	u64 misc_en;
	int err;

	err = rdmsrl_safe(MSR_IA32_MISC_ENABLE, &misc_en);
	if (err)
		return false;

	return (misc_en & MSR_IA32_MISC_ENABLE_TURBO_DISABLE);
}

static bool __init slv_set_max_freq_ratio(u64 *base_freq, u64 *turbo_freq)
{
	int err;

	err = rdmsrl_safe(MSR_ATOM_CORE_RATIOS, base_freq);
	if (err)
		return false;

	err = rdmsrl_safe(MSR_ATOM_CORE_TURBO_RATIOS, turbo_freq);
	if (err)
		return false;

	*base_freq = (*base_freq >> 16) & 0x3F;     /* max P state */
	*turbo_freq = *turbo_freq & 0x3F;           /* 1C turbo    */

	return true;
}

#define X86_MATCH(model)					\
	X86_MATCH_VENDOR_FAM_MODEL_FEATURE(INTEL, 6,		\
		INTEL_FAM6_##model, X86_FEATURE_APERFMPERF, NULL)

static const struct x86_cpu_id has_knl_turbo_ratio_limits[] __initconst = {
	X86_MATCH(XEON_PHI_KNL),
	X86_MATCH(XEON_PHI_KNM),
	{}
};

static const struct x86_cpu_id has_skx_turbo_ratio_limits[] __initconst = {
	X86_MATCH(SKYLAKE_X),
	{}
};

static const struct x86_cpu_id has_glm_turbo_ratio_limits[] __initconst = {
	X86_MATCH(ATOM_GOLDMONT),
	X86_MATCH(ATOM_GOLDMONT_D),
	X86_MATCH(ATOM_GOLDMONT_PLUS),
	{}
};

static bool __init knl_set_max_freq_ratio(u64 *base_freq, u64 *turbo_freq,
					  int num_delta_fratio)
{
	int fratio, delta_fratio, found;
	int err, i;
	u64 msr;

	err = rdmsrl_safe(MSR_PLATFORM_INFO, base_freq);
	if (err)
		return false;

	*base_freq = (*base_freq >> 8) & 0xFF;	    /* max P state */

	err = rdmsrl_safe(MSR_TURBO_RATIO_LIMIT, &msr);
	if (err)
		return false;

	fratio = (msr >> 8) & 0xFF;
	i = 16;
	found = 0;
	do {
		if (found >= num_delta_fratio) {
			*turbo_freq = fratio;
			return true;
		}

		delta_fratio = (msr >> (i + 5)) & 0x7;

		if (delta_fratio) {
			found += 1;
			fratio -= delta_fratio;
		}

		i += 8;
	} while (i < 64);

	return true;
}

static bool __init skx_set_max_freq_ratio(u64 *base_freq, u64 *turbo_freq, int size)
{
	u64 ratios, counts;
	u32 group_size;
	int err, i;

	err = rdmsrl_safe(MSR_PLATFORM_INFO, base_freq);
	if (err)
		return false;

	*base_freq = (*base_freq >> 8) & 0xFF;      /* max P state */

	err = rdmsrl_safe(MSR_TURBO_RATIO_LIMIT, &ratios);
	if (err)
		return false;

	err = rdmsrl_safe(MSR_TURBO_RATIO_LIMIT1, &counts);
	if (err)
		return false;

	for (i = 0; i < 64; i += 8) {
		group_size = (counts >> i) & 0xFF;
		if (group_size >= size) {
			*turbo_freq = (ratios >> i) & 0xFF;
			return true;
		}
	}

	return false;
}

static bool __init core_set_max_freq_ratio(u64 *base_freq, u64 *turbo_freq)
{
	u64 msr;
	int err;

	err = rdmsrl_safe(MSR_PLATFORM_INFO, base_freq);
	if (err)
		return false;

	err = rdmsrl_safe(MSR_TURBO_RATIO_LIMIT, &msr);
	if (err)
		return false;

	*base_freq = (*base_freq >> 8) & 0xFF;    /* max P state */
	*turbo_freq = (msr >> 24) & 0xFF;         /* 4C turbo    */

	/* The CPU may have less than 4 cores */
	if (!*turbo_freq)
		*turbo_freq = msr & 0xFF;         /* 1C turbo    */

	return true;
}

static bool __init intel_set_max_freq_ratio(void)
{
	u64 base_freq, turbo_freq;
	u64 turbo_ratio;

	if (slv_set_max_freq_ratio(&base_freq, &turbo_freq))
		goto out;

	if (x86_match_cpu(has_glm_turbo_ratio_limits) &&
	    skx_set_max_freq_ratio(&base_freq, &turbo_freq, 1))
		goto out;

	if (x86_match_cpu(has_knl_turbo_ratio_limits) &&
	    knl_set_max_freq_ratio(&base_freq, &turbo_freq, 1))
		goto out;

	if (x86_match_cpu(has_skx_turbo_ratio_limits) &&
	    skx_set_max_freq_ratio(&base_freq, &turbo_freq, 4))
		goto out;

	if (core_set_max_freq_ratio(&base_freq, &turbo_freq))
		goto out;

	return false;

out:
	/*
	 * Some hypervisors advertise X86_FEATURE_APERFMPERF
	 * but then fill all MSR's with zeroes.
	 * Some CPUs have turbo boost but don't declare any turbo ratio
	 * in MSR_TURBO_RATIO_LIMIT.
	 */
	if (!base_freq || !turbo_freq) {
		pr_debug("Couldn't determine cpu base or turbo frequency, necessary for scale-invariant accounting.\n");
		return false;
	}

	turbo_ratio = div_u64(turbo_freq * SCHED_CAPACITY_SCALE, base_freq);
	if (!turbo_ratio) {
		pr_debug("Non-zero turbo and base frequencies led to a 0 ratio.\n");
		return false;
	}

	arch_turbo_freq_ratio = turbo_ratio;
	arch_set_max_freq_ratio(turbo_disabled());

	return true;
}

static void init_counter_refs(void)
{
	u64 aperf, mperf;

	rdmsrl(MSR_IA32_APERF, aperf);
	rdmsrl(MSR_IA32_MPERF, mperf);

	this_cpu_write(arch_prev_aperf, aperf);
	this_cpu_write(arch_prev_mperf, mperf);
}

#ifdef CONFIG_PM_SLEEP
static struct syscore_ops freq_invariance_syscore_ops = {
	.resume = init_counter_refs,
};

static void register_freq_invariance_syscore_ops(void)
{
	register_syscore_ops(&freq_invariance_syscore_ops);
}
#else
static inline void register_freq_invariance_syscore_ops(void) {}
#endif

static void freq_invariance_enable(void)
{
	if (static_branch_unlikely(&arch_scale_freq_key)) {
		WARN_ON_ONCE(1);
		return;
	}
	static_branch_enable(&arch_scale_freq_key);
	register_freq_invariance_syscore_ops();
	pr_info("Estimated ratio of average max frequency by base frequency (times 1024): %llu\n", arch_max_freq_ratio);
}

void freq_invariance_set_perf_ratio(u64 ratio, bool turbo_disabled)
{
	arch_turbo_freq_ratio = ratio;
	arch_set_max_freq_ratio(turbo_disabled);
	freq_invariance_enable();
}

void __init bp_init_freq_invariance(void)
{
	if (!cpu_feature_enabled(X86_FEATURE_APERFMPERF))
		return;

	init_counter_refs();

	if (boot_cpu_data.x86_vendor != X86_VENDOR_INTEL)
		return;

	if (intel_set_max_freq_ratio())
		freq_invariance_enable();
}

void ap_init_freq_invariance(void)
{
	if (cpu_feature_enabled(X86_FEATURE_APERFMPERF))
		init_counter_refs();
}

static void disable_freq_invariance_workfn(struct work_struct *work)
{
	static_branch_disable(&arch_scale_freq_key);
}

static DECLARE_WORK(disable_freq_invariance_work,
		    disable_freq_invariance_workfn);

DEFINE_PER_CPU(unsigned long, arch_freq_scale) = SCHED_CAPACITY_SCALE;

void arch_scale_freq_tick(void)
{
	u64 freq_scale;
	u64 aperf, mperf;
	u64 acnt, mcnt;

	if (!arch_scale_freq_invariant())
		return;

	rdmsrl(MSR_IA32_APERF, aperf);
	rdmsrl(MSR_IA32_MPERF, mperf);

	acnt = aperf - this_cpu_read(arch_prev_aperf);
	mcnt = mperf - this_cpu_read(arch_prev_mperf);

	this_cpu_write(arch_prev_aperf, aperf);
	this_cpu_write(arch_prev_mperf, mperf);

	if (check_shl_overflow(acnt, 2*SCHED_CAPACITY_SHIFT, &acnt))
		goto error;

	if (check_mul_overflow(mcnt, arch_max_freq_ratio, &mcnt) || !mcnt)
		goto error;

	freq_scale = div64_u64(acnt, mcnt);
	if (!freq_scale)
		goto error;

	if (freq_scale > SCHED_CAPACITY_SCALE)
		freq_scale = SCHED_CAPACITY_SCALE;

	this_cpu_write(arch_freq_scale, freq_scale);
	return;

error:
	pr_warn("Scheduler frequency invariance went wobbly, disabling!\n");
	schedule_work(&disable_freq_invariance_work);
}
#endif /* CONFIG_X86_64 && CONFIG_SMP */
