// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU API for QCOM secure IOMMUs.  Somewhat based on arm-smmu.c
 *
 * Copyright (C) 2013 ARM Limited
 * Copyright (C) 2017 Red Hat
 */

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-hi-lo.h>
#include <linux/io-pgtable.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/kconfig.h>
#include <linux/init.h>
#include <linux/interconnect.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "arm-smmu.h"

#define SMMU_INTR_SEL_NS     0x2000

/*
 * QSMMU-v2 (MSM8974 MMSS) global register block, relative to local_base.
 * On secure IOMMUs TrustZone programs these via restore_sec_cfg(); a
 * non-secure IOMMU (no qcom,iommu-secure-id) has no TrustZone path, so the
 * HLOS must set up global control, stream matching and context attribution
 * itself -- otherwise a context-bank access at attach faults an unconfigured
 * SMMU and resets the SoC.
 */
#define QSMMU_GLB_CR0			0x0000
#define QSMMU_GLB_CR2			0x0008
#define QSMMU_GLB_ACR			0x0010
#define QSMMU_GLB_IDR0			0x0020
#define QSMMU_GLB_GFAR			0x0040
#define QSMMU_GLB_SGFSR			0x0048
#define QSMMU_GLB_GFSRRESTORE		0x004c
#define QSMMU_GLB_TLBIALLNSNH		0x0068
#define QSMMU_GLB_SMR(n)		(0x0800 + ((n) << 2))
#define QSMMU_GLB_S2CR(n)		(0x0c00 + ((n) << 2))
#define QSMMU_GLB_CBAR(n)		(0x1000 + ((n) << 2))
#define QSMMU_GLB_MICRO_MMU_CTRL	0x2000

#define QSMMU_MMU_CTRL_HALT_REQ		BIT(2)
#define QSMMU_MMU_CTRL_IDLE		BIT(3)

#define QSMMU_CR0_SMCFCFG		BIT(21)
#define QSMMU_CR0_USFCFG		BIT(10)
#define QSMMU_CR0_STALLD		BIT(8)
#define QSMMU_CR0_GCFGFIE		BIT(5)
#define QSMMU_CR0_GCFGFRE		BIT(4)
#define QSMMU_CR0_GFIE			BIT(2)
#define QSMMU_CR0_GFRE			BIT(1)
#define QSMMU_CR0_CLIENTPD		BIT(0)

#define QSMMU_SMR_VALID			BIT(31)
#define QSMMU_SMR_ID			GENMASK(14, 0)

#define QSMMU_S2CR_CBNDX		GENMASK(7, 0)
#define QSMMU_S2CR_MEMATTR		GENMASK(15, 12)
#define QSMMU_S2CR_TYPE			GENMASK(17, 16)
#define QSMMU_S2CR_NSCFG		GENMASK(19, 18)

#define QSMMU_CBAR_VMID			GENMASK(7, 0)
#define QSMMU_CBAR_BPSHCFG		GENMASK(9, 8)
#define QSMMU_CBAR_MEMATTR		GENMASK(15, 12)
#define QSMMU_CBAR_TYPE			GENMASK(17, 16)
#define QSMMU_CBAR_IRPTNDX		GENMASK(31, 24)

#define QSMMU_MEMATTR_WB		0xa	/* write-back, do not downgrade */
#define QSMMU_HLOS_VMID			3	/* non-secure HLOS VMID */

enum qcom_iommu_clk {
	CLK_IFACE,
	CLK_BUS,
	CLK_TBU,
	/*
	 * Optional MMSS NoC AXI clock. On MSM8974 the multimedia SMMU sits
	 * behind the MMSS NoC; that NoC must be clocked for the SMMU's bus
	 * access to be routed, or the transaction faults and resets the SoC.
	 * Absent (NULL, skipped) on SoCs that don't need it.
	 */
	CLK_MMSSNOC,
	CLK_NUM,
};

struct qcom_iommu_ctx;

struct qcom_iommu_dev {
	/* IOMMU core code handle */
	struct iommu_device	 iommu;
	struct device		*dev;
	struct clk_bulk_data clks[CLK_NUM];
	/*
	 * MMSS<->EBI interconnect path. Present only on SoCs (e.g. MSM8974)
	 * whose IOMMU sits behind the multimedia NoC: the SMMU AXI path must
	 * be voted up before any register access, or the un-arbitrated access
	 * faults the NoC and resets the SoC. NULL when the DT has no
	 * "interconnects" property (all other SoCs -- behaviour unchanged).
	 */
	struct icc_path		*icc_path;
	void __iomem		*local_base;
	u32			 sec_id;
	/*
	 * Non-secure IOMMU (no qcom,iommu-secure-id): the HLOS owns global
	 * programming. local_base is the SMMU global register window.
	 * glb_inited tracks the one-time global (CR0/reset) init.
	 */
	bool			 nonsecure;
	bool			 glb_inited;
	u8			 max_asid;
	struct qcom_iommu_ctx	*ctxs[];   /* indexed by asid */
};

struct qcom_iommu_ctx {
	struct device		*dev;
	void __iomem		*base;
	bool			 secure_init;
	bool			 secured_ctx;
	u8			 asid;      /* asid and ctx bank # are 1:1 */
	struct iommu_domain	*domain;
};

struct qcom_iommu_domain {
	struct io_pgtable_ops	*pgtbl_ops;
	spinlock_t		 pgtbl_lock;
	struct mutex		 init_mutex; /* Protects iommu pointer */
	struct iommu_domain	 domain;
	struct qcom_iommu_dev	*iommu;
	struct iommu_fwspec	*fwspec;
};

static struct qcom_iommu_domain *to_qcom_iommu_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct qcom_iommu_domain, domain);
}

static const struct iommu_ops qcom_iommu_ops;

static struct qcom_iommu_ctx * to_ctx(struct qcom_iommu_domain *d, unsigned asid)
{
	struct qcom_iommu_dev *qcom_iommu = d->iommu;
	if (!qcom_iommu)
		return NULL;
	return qcom_iommu->ctxs[asid];
}

static inline void
iommu_writel(struct qcom_iommu_ctx *ctx, unsigned reg, u32 val)
{
	writel_relaxed(val, ctx->base + reg);
}

static inline void
iommu_writeq(struct qcom_iommu_ctx *ctx, unsigned reg, u64 val)
{
	writeq_relaxed(val, ctx->base + reg);
}

static inline u32
iommu_readl(struct qcom_iommu_ctx *ctx, unsigned reg)
{
	return readl_relaxed(ctx->base + reg);
}

static inline u64
iommu_readq(struct qcom_iommu_ctx *ctx, unsigned reg)
{
	return readq_relaxed(ctx->base + reg);
}

static bool qcom_iommu_has_secure_context(struct qcom_iommu_dev *qcom_iommu);

static void qcom_iommu_tlb_sync(void *cookie)
{
	struct qcom_iommu_domain *qcom_domain = cookie;
	struct iommu_fwspec *fwspec = qcom_domain->fwspec;
	unsigned i;

	for (i = 0; i < fwspec->num_ids; i++) {
		struct qcom_iommu_ctx *ctx = to_ctx(qcom_domain, fwspec->ids[i]);
		unsigned int val, ret;

		iommu_writel(ctx, ARM_SMMU_CB_TLBSYNC, 0);

		ret = readl_poll_timeout(ctx->base + ARM_SMMU_CB_TLBSTATUS, val,
					 (val & 0x1) == 0, 0, 5000000);
		if (ret)
			dev_err(ctx->dev, "timeout waiting for TLB SYNC\n");
	}
}

static void qcom_iommu_tlb_inv_context(void *cookie)
{
	struct qcom_iommu_domain *qcom_domain = cookie;
	struct iommu_fwspec *fwspec = qcom_domain->fwspec;
	unsigned i;

	for (i = 0; i < fwspec->num_ids; i++) {
		struct qcom_iommu_ctx *ctx = to_ctx(qcom_domain, fwspec->ids[i]);
		iommu_writel(ctx, ARM_SMMU_CB_S1_TLBIASID, ctx->asid);
	}

	qcom_iommu_tlb_sync(cookie);
}

static void qcom_iommu_tlb_inv_range_nosync(unsigned long iova, size_t size,
					    size_t granule, bool leaf, void *cookie)
{
	struct qcom_iommu_domain *qcom_domain = cookie;
	struct iommu_fwspec *fwspec = qcom_domain->fwspec;
	unsigned i, reg;

	reg = leaf ? ARM_SMMU_CB_S1_TLBIVAL : ARM_SMMU_CB_S1_TLBIVA;

	for (i = 0; i < fwspec->num_ids; i++) {
		struct qcom_iommu_ctx *ctx = to_ctx(qcom_domain, fwspec->ids[i]);
		size_t s = size;

		iova = (iova >> 12) << 12;
		iova |= ctx->asid;
		do {
			iommu_writel(ctx, reg, iova);
			iova += granule;
		} while (s -= granule);
	}
}

static void qcom_iommu_tlb_flush_walk(unsigned long iova, size_t size,
				      size_t granule, void *cookie)
{
	qcom_iommu_tlb_inv_range_nosync(iova, size, granule, false, cookie);
	qcom_iommu_tlb_sync(cookie);
}

static void qcom_iommu_tlb_add_page(struct iommu_iotlb_gather *gather,
				    unsigned long iova, size_t granule,
				    void *cookie)
{
	qcom_iommu_tlb_inv_range_nosync(iova, granule, granule, true, cookie);
}

static const struct iommu_flush_ops qcom_flush_ops = {
	.tlb_flush_all	= qcom_iommu_tlb_inv_context,
	.tlb_flush_walk = qcom_iommu_tlb_flush_walk,
	.tlb_add_page	= qcom_iommu_tlb_add_page,
};

static irqreturn_t qcom_iommu_fault(int irq, void *dev)
{
	struct qcom_iommu_ctx *ctx = dev;
	u32 fsr, fsynr;
	u64 iova;

	fsr = iommu_readl(ctx, ARM_SMMU_CB_FSR);

	if (!(fsr & ARM_SMMU_CB_FSR_FAULT))
		return IRQ_NONE;

	fsynr = iommu_readl(ctx, ARM_SMMU_CB_FSYNR0);
	iova = iommu_readq(ctx, ARM_SMMU_CB_FAR);

	if (!report_iommu_fault(ctx->domain, ctx->dev, iova, 0)) {
		dev_err_ratelimited(ctx->dev,
				    "Unhandled context fault: fsr=0x%x, "
				    "iova=0x%016llx, fsynr=0x%x, cb=%d\n",
				    fsr, iova, fsynr, ctx->asid);
	}

	iommu_writel(ctx, ARM_SMMU_CB_FSR, fsr);
	iommu_writel(ctx, ARM_SMMU_CB_RESUME, ARM_SMMU_RESUME_TERMINATE);

	return IRQ_HANDLED;
}

/*
 * Non-secure global programming (MSM8974 MMSS QSMMU-v2). These touch the
 * SMMU global register window (local_base) and must run with the SMMU
 * clocks enabled and the MMU halted.
 */
static int qcom_iommu_glb_halt(struct qcom_iommu_dev *qcom_iommu)
{
	void __iomem *base = qcom_iommu->local_base;
	u32 val;

	writel_relaxed(readl_relaxed(base + QSMMU_GLB_MICRO_MMU_CTRL) |
		       QSMMU_MMU_CTRL_HALT_REQ,
		       base + QSMMU_GLB_MICRO_MMU_CTRL);

	/*
	 * Runs in process context (init_domain holds a mutex), so sleep while
	 * polling. Reprogramming a non-idle SMMU corrupts in-flight
	 * translations, so a halt timeout must fail the attach, not continue.
	 */
	if (readl_poll_timeout(base + QSMMU_GLB_MICRO_MMU_CTRL, val,
			       val & QSMMU_MMU_CTRL_IDLE, 10, 1000000)) {
		dev_err(qcom_iommu->dev, "SMMU halt timeout\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static void qcom_iommu_glb_resume(struct qcom_iommu_dev *qcom_iommu)
{
	void __iomem *base = qcom_iommu->local_base;

	writel_relaxed(readl_relaxed(base + QSMMU_GLB_MICRO_MMU_CTRL) &
		       ~QSMMU_MMU_CTRL_HALT_REQ,
		       base + QSMMU_GLB_MICRO_MMU_CTRL);
}

/* One-time global init: reset state and enable the SMMU (CLIENTPD clear). */
static void qcom_iommu_glb_reset(struct qcom_iommu_dev *qcom_iommu)
{
	void __iomem *base = qcom_iommu->local_base;
	u32 numsmr;
	int i;

	writel_relaxed(0, base + QSMMU_GLB_ACR);
	writel_relaxed(0, base + QSMMU_GLB_CR2);
	writel_relaxed(0, base + QSMMU_GLB_GFAR);
	writel_relaxed(0, base + QSMMU_GLB_GFSRRESTORE);
	writel_relaxed(0, base + QSMMU_GLB_TLBIALLNSNH);

	/*
	 * Clear any latched global fault the bootloader left behind (W1C).
	 * Otherwise, once fault reporting is enabled below, the stale bit is
	 * reported immediately.
	 */
	writel_relaxed(readl_relaxed(base + QSMMU_GLB_SGFSR),
		       base + QSMMU_GLB_SGFSR);

	/* Invalidate any stale stream-match entries the bootloader left. */
	numsmr = readl_relaxed(base + QSMMU_GLB_IDR0) & 0xff;
	if (!numsmr || numsmr > 128)
		numsmr = 128;
	for (i = 0; i < numsmr; i++)
		writel_relaxed(0, base + QSMMU_GLB_SMR(i));

	/*
	 * Enable the SMMU (CLIENTPD clear) with fault *reporting* only. Do NOT
	 * enable the global/config fault interrupts (GFIE/GCFGFIE): this driver
	 * has no global-fault IRQ handler, and the global fault shares the
	 * per-context GIC line. An enabled global interrupt with no handler
	 * would never be de-asserted -> interrupt storm -> SoC reset.
	 */
	writel_relaxed(QSMMU_CR0_SMCFCFG | QSMMU_CR0_USFCFG | QSMMU_CR0_STALLD |
		       QSMMU_CR0_GCFGFRE | QSMMU_CR0_GFRE,
		       base + QSMMU_GLB_CR0);   /* CLIENTPD clear -> SMMU enabled */
}

/*
 * Program the stream match (SMR), stream-to-context (S2CR) and context-bank
 * attribution (CBAR) that TrustZone would program for a secure IOMMU. The
 * GPU/Venus stream IDs equal their context-bank index on this SoC, so use
 * the asid as both the SMR slot and the stream ID.
 */
static void qcom_iommu_glb_program_ctx(struct qcom_iommu_dev *qcom_iommu,
				       struct qcom_iommu_ctx *ctx)
{
	void __iomem *base = qcom_iommu->local_base;
	u32 idx = ctx->asid;

	writel_relaxed(FIELD_PREP(QSMMU_CBAR_TYPE, 1) |	/* S1 translate, S2 bypass */
		       FIELD_PREP(QSMMU_CBAR_IRPTNDX, 1) |
		       FIELD_PREP(QSMMU_CBAR_VMID, QSMMU_HLOS_VMID) |
		       FIELD_PREP(QSMMU_CBAR_BPSHCFG, 2) |
		       FIELD_PREP(QSMMU_CBAR_MEMATTR, QSMMU_MEMATTR_WB),
		       base + QSMMU_GLB_CBAR(idx));

	writel_relaxed(QSMMU_SMR_VALID | FIELD_PREP(QSMMU_SMR_ID, idx),
		       base + QSMMU_GLB_SMR(idx));

	writel_relaxed(FIELD_PREP(QSMMU_S2CR_TYPE, 0) |	/* translate via CBNDX */
		       FIELD_PREP(QSMMU_S2CR_CBNDX, idx) |
		       FIELD_PREP(QSMMU_S2CR_MEMATTR, QSMMU_MEMATTR_WB) |
		       FIELD_PREP(QSMMU_S2CR_NSCFG, 3),	/* force non-secure */
		       base + QSMMU_GLB_S2CR(idx));
}

static int qcom_iommu_init_domain(struct iommu_domain *domain,
				  struct qcom_iommu_dev *qcom_iommu,
				  struct device *dev)
{
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct io_pgtable_ops *pgtbl_ops;
	struct io_pgtable_cfg pgtbl_cfg;
	int i, ret = 0;
	u32 reg;

	mutex_lock(&qcom_domain->init_mutex);
	if (qcom_domain->iommu)
		goto out_unlock;


	pgtbl_cfg = (struct io_pgtable_cfg) {
		.pgsize_bitmap	= qcom_iommu_ops.pgsize_bitmap,
		.ias		= 32,
		.oas		= 40,
		.tlb		= &qcom_flush_ops,
		.iommu_dev	= qcom_iommu->dev,
	};

	qcom_domain->iommu = qcom_iommu;
	qcom_domain->fwspec = fwspec;

	pgtbl_ops = alloc_io_pgtable_ops(ARM_32_LPAE_S1, &pgtbl_cfg, qcom_domain);
	if (!pgtbl_ops) {
		dev_err(qcom_iommu->dev, "failed to allocate pagetable ops\n");
		ret = -ENOMEM;
		goto out_clear_iommu;
	}

	/* Update the domain's page sizes to reflect the page table format */
	domain->pgsize_bitmap = pgtbl_cfg.pgsize_bitmap;
	domain->geometry.aperture_end = (1ULL << pgtbl_cfg.ias) - 1;
	domain->geometry.force_aperture = true;

	/*
	 * Non-secure SMMU: TrustZone does not set up the global space, so halt
	 * the MMU and (once) reset + enable it before programming any context.
	 */
	if (qcom_iommu->nonsecure) {
		ret = qcom_iommu_glb_halt(qcom_iommu);
		if (ret)
			goto out_clear_iommu;
		if (!qcom_iommu->glb_inited) {
			qcom_iommu_glb_reset(qcom_iommu);
			qcom_iommu->glb_inited = true;
		}
	}

	for (i = 0; i < fwspec->num_ids; i++) {
		struct qcom_iommu_ctx *ctx = to_ctx(qcom_domain, fwspec->ids[i]);

		if (!ctx->secure_init && qcom_iommu_has_secure_context(qcom_iommu)) {
			ret = qcom_scm_restore_sec_cfg(qcom_iommu->sec_id, ctx->asid);
			if (ret) {
				dev_err(qcom_iommu->dev, "secure init failed: %d\n", ret);
				goto out_clear_iommu;
			}
			ctx->secure_init = true;
		}

		/* Secured QSMMU-500/QSMMU-v2 contexts cannot be programmed */
		if (ctx->secured_ctx) {
			ctx->domain = domain;
			continue;
		}

		/*
		 * Non-secure: program the stream match and context-bank
		 * attribution that TrustZone would own for a secure SMMU.
		 */
		if (qcom_iommu->nonsecure)
			qcom_iommu_glb_program_ctx(qcom_iommu, ctx);

		/* Disable context bank before programming */
		iommu_writel(ctx, ARM_SMMU_CB_SCTLR, 0);

		/* Clear context bank fault address fault status registers */
		iommu_writel(ctx, ARM_SMMU_CB_FAR, 0);
		iommu_writel(ctx, ARM_SMMU_CB_FSR, ARM_SMMU_CB_FSR_FAULT);

		/* TTBRs */
		iommu_writeq(ctx, ARM_SMMU_CB_TTBR0,
				pgtbl_cfg.arm_lpae_s1_cfg.ttbr |
				FIELD_PREP(ARM_SMMU_TTBRn_ASID, ctx->asid));
		iommu_writeq(ctx, ARM_SMMU_CB_TTBR1, 0);

		/*
		 * TCR2 (CB offset 0x10) does not exist on the MSM8974 QSMMU-v2
		 * context bank -- the downstream msm-iommu-v1 driver never writes
		 * it, and writing it here faults and resets the SoC. Skip it on
		 * the non-secure (msm8974) path; the PA-size/SEP it would carry is
		 * conveyed by TCR on this IP.
		 */
		if (!qcom_iommu->nonsecure) {
			iommu_writel(ctx, ARM_SMMU_CB_TCR2,
					arm_smmu_lpae_tcr2(&pgtbl_cfg));
		}
		iommu_writel(ctx, ARM_SMMU_CB_TCR,
			     arm_smmu_lpae_tcr(&pgtbl_cfg) | ARM_SMMU_TCR_EAE);

		/* MAIRs (stage-1 only) */
		iommu_writel(ctx, ARM_SMMU_CB_S1_MAIR0,
				pgtbl_cfg.arm_lpae_s1_cfg.mair);
		iommu_writel(ctx, ARM_SMMU_CB_S1_MAIR1,
				pgtbl_cfg.arm_lpae_s1_cfg.mair >> 32);

		/* SCTLR */
		reg = ARM_SMMU_SCTLR_CFIE | ARM_SMMU_SCTLR_CFRE |
		      ARM_SMMU_SCTLR_TRE |
		      ARM_SMMU_SCTLR_M | ARM_SMMU_SCTLR_S1_ASIDPNE |
		      ARM_SMMU_SCTLR_CFCFG;

		/*
		 * The MSM8974 QSMMU-v2 raises an Access-Flag Fault (FSR.AFF) on
		 * the first stage-1 walk even with SCTLR.AFE cleared -- unlike
		 * the ARM SMMU architectural behaviour, clearing AFE alone does
		 * not disable AF faulting on this IP. Set the Qualcomm-specific
		 * AFFD (Access-Flag-Fault-Disable) bit instead. The secure path
		 * (msm8916 etc.) keeps the architectural AFE and does not touch
		 * the vendor AFFD bit.
		 */
		if (qcom_iommu->nonsecure)
			reg |= ARM_SMMU_SCTLR_AFFD;
		else
			reg |= ARM_SMMU_SCTLR_AFE;

		if (IS_ENABLED(CONFIG_CPU_BIG_ENDIAN))
			reg |= ARM_SMMU_SCTLR_E;

		iommu_writel(ctx, ARM_SMMU_CB_SCTLR, reg);

		ctx->domain = domain;
	}

	if (qcom_iommu->nonsecure) {
		qcom_iommu_glb_resume(qcom_iommu);
	}

	mutex_unlock(&qcom_domain->init_mutex);

	/* Publish page table ops for map/unmap */
	qcom_domain->pgtbl_ops = pgtbl_ops;

	return 0;

out_clear_iommu:
	qcom_domain->iommu = NULL;
out_unlock:
	mutex_unlock(&qcom_domain->init_mutex);
	return ret;
}

static struct iommu_domain *qcom_iommu_domain_alloc_paging(struct device *dev)
{
	struct qcom_iommu_domain *qcom_domain;

	/*
	 * Allocate the domain and initialise some of its data structures.
	 * We can't really do anything meaningful until we've added a
	 * master.
	 */
	qcom_domain = kzalloc(sizeof(*qcom_domain), GFP_KERNEL);
	if (!qcom_domain)
		return NULL;

	mutex_init(&qcom_domain->init_mutex);
	spin_lock_init(&qcom_domain->pgtbl_lock);

	return &qcom_domain->domain;
}

static void qcom_iommu_domain_free(struct iommu_domain *domain)
{
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);

	if (qcom_domain->iommu) {
		/*
		 * NOTE: unmap can be called after client device is powered
		 * off, for example, with GPUs or anything involving dma-buf.
		 * So we cannot rely on the device_link.  Make sure the IOMMU
		 * is on to avoid unclocked accesses in the TLB inv path:
		 */
		pm_runtime_get_sync(qcom_domain->iommu->dev);
		free_io_pgtable_ops(qcom_domain->pgtbl_ops);
		pm_runtime_put_sync(qcom_domain->iommu->dev);
	}

	kfree(qcom_domain);
}

static int qcom_iommu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	struct qcom_iommu_dev *qcom_iommu = dev_iommu_priv_get(dev);
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);
	int ret;

	if (!qcom_iommu) {
		dev_err(dev, "cannot attach to IOMMU, is it on the same bus?\n");
		return -ENXIO;
	}

	/* Ensure that the domain is finalized */
	ret = pm_runtime_resume_and_get(qcom_iommu->dev);
	if (ret < 0)
		return ret;
	ret = qcom_iommu_init_domain(domain, qcom_iommu, dev);
	pm_runtime_put_sync(qcom_iommu->dev);
	if (ret < 0)
		return ret;

	/*
	 * Sanity check the domain. We don't support domains across
	 * different IOMMUs.
	 */
	if (qcom_domain->iommu != qcom_iommu)
		return -EINVAL;

	return 0;
}

static int qcom_iommu_identity_attach(struct iommu_domain *identity_domain,
				      struct device *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct qcom_iommu_domain *qcom_domain;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct qcom_iommu_dev *qcom_iommu = dev_iommu_priv_get(dev);
	unsigned int i;

	if (domain == identity_domain || !domain)
		return 0;

	qcom_domain = to_qcom_iommu_domain(domain);
	if (WARN_ON(!qcom_domain->iommu))
		return -EINVAL;

	pm_runtime_get_sync(qcom_iommu->dev);
	for (i = 0; i < fwspec->num_ids; i++) {
		struct qcom_iommu_ctx *ctx = to_ctx(qcom_domain, fwspec->ids[i]);

		/* Disable the context bank: */
		iommu_writel(ctx, ARM_SMMU_CB_SCTLR, 0);

		ctx->domain = NULL;
	}
	pm_runtime_put_sync(qcom_iommu->dev);
	return 0;
}

static struct iommu_domain_ops qcom_iommu_identity_ops = {
	.attach_dev = qcom_iommu_identity_attach,
};

static struct iommu_domain qcom_iommu_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	.ops = &qcom_iommu_identity_ops,
};

static int qcom_iommu_map(struct iommu_domain *domain, unsigned long iova,
			  phys_addr_t paddr, size_t pgsize, size_t pgcount,
			  int prot, gfp_t gfp, size_t *mapped)
{
	int ret;
	unsigned long flags;
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);
	struct io_pgtable_ops *ops = qcom_domain->pgtbl_ops;

	if (!ops)
		return -ENODEV;

	spin_lock_irqsave(&qcom_domain->pgtbl_lock, flags);
	ret = ops->map_pages(ops, iova, paddr, pgsize, pgcount, prot, GFP_ATOMIC, mapped);
	spin_unlock_irqrestore(&qcom_domain->pgtbl_lock, flags);
	return ret;
}

static size_t qcom_iommu_unmap(struct iommu_domain *domain, unsigned long iova,
			       size_t pgsize, size_t pgcount,
			       struct iommu_iotlb_gather *gather)
{
	size_t ret;
	unsigned long flags;
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);
	struct io_pgtable_ops *ops = qcom_domain->pgtbl_ops;

	if (!ops)
		return 0;

	/* NOTE: unmap can be called after client device is powered off,
	 * for example, with GPUs or anything involving dma-buf.  So we
	 * cannot rely on the device_link.  Make sure the IOMMU is on to
	 * avoid unclocked accesses in the TLB inv path:
	 */
	pm_runtime_get_sync(qcom_domain->iommu->dev);
	spin_lock_irqsave(&qcom_domain->pgtbl_lock, flags);
	ret = ops->unmap_pages(ops, iova, pgsize, pgcount, gather);
	spin_unlock_irqrestore(&qcom_domain->pgtbl_lock, flags);
	pm_runtime_put_sync(qcom_domain->iommu->dev);

	return ret;
}

static void qcom_iommu_flush_iotlb_all(struct iommu_domain *domain)
{
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);
	struct io_pgtable *pgtable = container_of(qcom_domain->pgtbl_ops,
						  struct io_pgtable, ops);
	if (!qcom_domain->pgtbl_ops)
		return;

	pm_runtime_get_sync(qcom_domain->iommu->dev);
	qcom_iommu_tlb_sync(pgtable->cookie);
	pm_runtime_put_sync(qcom_domain->iommu->dev);
}

static void qcom_iommu_iotlb_sync(struct iommu_domain *domain,
				  struct iommu_iotlb_gather *gather)
{
	qcom_iommu_flush_iotlb_all(domain);
}

static phys_addr_t qcom_iommu_iova_to_phys(struct iommu_domain *domain,
					   dma_addr_t iova)
{
	phys_addr_t ret;
	unsigned long flags;
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);
	struct io_pgtable_ops *ops = qcom_domain->pgtbl_ops;

	if (!ops)
		return 0;

	spin_lock_irqsave(&qcom_domain->pgtbl_lock, flags);
	ret = ops->iova_to_phys(ops, iova);
	spin_unlock_irqrestore(&qcom_domain->pgtbl_lock, flags);

	return ret;
}

static bool qcom_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		/*
		 * Return true here as the SMMU can always send out coherent
		 * requests.
		 */
		return true;
	case IOMMU_CAP_NOEXEC:
		return true;
	default:
		return false;
	}
}

static struct iommu_device *qcom_iommu_probe_device(struct device *dev)
{
	struct qcom_iommu_dev *qcom_iommu = dev_iommu_priv_get(dev);
	struct device_link *link;

	if (!qcom_iommu)
		return ERR_PTR(-ENODEV);

	/*
	 * Establish the link between iommu and master, so that the
	 * iommu gets runtime enabled/disabled as per the master's
	 * needs.
	 */
	link = device_link_add(dev, qcom_iommu->dev, DL_FLAG_PM_RUNTIME);
	if (!link) {
		dev_err(qcom_iommu->dev, "Unable to create device link between %s and %s\n",
			dev_name(qcom_iommu->dev), dev_name(dev));
		return ERR_PTR(-ENODEV);
	}

	return &qcom_iommu->iommu;
}

static int qcom_iommu_of_xlate(struct device *dev,
			       const struct of_phandle_args *args)
{
	struct qcom_iommu_dev *qcom_iommu;
	struct platform_device *iommu_pdev;
	unsigned asid = args->args[0];

	if (args->args_count != 1) {
		dev_err(dev, "incorrect number of iommu params found for %s "
			"(found %d, expected 1)\n",
			args->np->full_name, args->args_count);
		return -EINVAL;
	}

	iommu_pdev = of_find_device_by_node(args->np);
	if (WARN_ON(!iommu_pdev))
		return -EINVAL;

	qcom_iommu = platform_get_drvdata(iommu_pdev);

	/* make sure the asid specified in dt is valid, so we don't have
	 * to sanity check this elsewhere:
	 */
	if (WARN_ON(asid > qcom_iommu->max_asid) ||
	    WARN_ON(qcom_iommu->ctxs[asid] == NULL)) {
		put_device(&iommu_pdev->dev);
		return -EINVAL;
	}

	if (!dev_iommu_priv_get(dev)) {
		dev_iommu_priv_set(dev, qcom_iommu);
	} else {
		/* make sure devices iommus dt node isn't referring to
		 * multiple different iommu devices.  Multiple context
		 * banks are ok, but multiple devices are not:
		 */
		if (WARN_ON(qcom_iommu != dev_iommu_priv_get(dev))) {
			put_device(&iommu_pdev->dev);
			return -EINVAL;
		}
	}

	return iommu_fwspec_add_ids(dev, &asid, 1);
}

static const struct iommu_ops qcom_iommu_ops = {
	.identity_domain = &qcom_iommu_identity_domain,
	.capable	= qcom_iommu_capable,
	.domain_alloc_paging = qcom_iommu_domain_alloc_paging,
	.probe_device	= qcom_iommu_probe_device,
	.device_group	= generic_device_group,
	.of_xlate	= qcom_iommu_of_xlate,
	.pgsize_bitmap	= SZ_4K | SZ_64K | SZ_1M | SZ_16M,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= qcom_iommu_attach_dev,
		.map_pages	= qcom_iommu_map,
		.unmap_pages	= qcom_iommu_unmap,
		.flush_iotlb_all = qcom_iommu_flush_iotlb_all,
		.iotlb_sync	= qcom_iommu_iotlb_sync,
		.iova_to_phys	= qcom_iommu_iova_to_phys,
		.free		= qcom_iommu_domain_free,
	}
};

static int qcom_iommu_sec_ptbl_init(struct device *dev)
{
	size_t psize = 0;
	unsigned int spare = 0;
	void *cpu_addr;
	dma_addr_t paddr;
	unsigned long attrs;
	static bool allocated = false;
	int ret;

	if (allocated)
		return 0;

	ret = qcom_scm_iommu_secure_ptbl_size(spare, &psize);
	if (ret) {
		dev_err(dev, "failed to get iommu secure pgtable size (%d)\n",
			ret);
		return ret;
	}

	dev_info(dev, "iommu sec: pgtable size: %zu\n", psize);

	attrs = DMA_ATTR_NO_KERNEL_MAPPING;

	cpu_addr = dma_alloc_attrs(dev, psize, &paddr, GFP_KERNEL, attrs);
	if (!cpu_addr) {
		dev_err(dev, "failed to allocate %zu bytes for pgtable\n",
			psize);
		return -ENOMEM;
	}

	ret = qcom_scm_iommu_secure_ptbl_init(paddr, psize, spare);
	if (ret) {
		dev_err(dev, "failed to init iommu pgtable (%d)\n", ret);
		goto free_mem;
	}

	allocated = true;
	return 0;

free_mem:
	dma_free_attrs(dev, psize, cpu_addr, paddr, attrs);
	return ret;
}

static int get_asid(const struct device_node *np)
{
	u32 reg, val;
	int asid;

	/* read the "reg" property directly to get the relative address
	 * of the context bank, and calculate the asid from that:
	 */
	if (of_property_read_u32_index(np, "reg", 0, &reg))
		return -ENODEV;

	/*
	 * Context banks are 0x1000 apart but, in some cases, the ASID
	 * number doesn't match to this logic and needs to be passed
	 * from the DT configuration explicitly.
	 */
	if (!of_property_read_u32(np, "qcom,ctx-asid", &val))
		asid = val;
	else
		asid = reg / 0x1000;

	return asid;
}

static int qcom_iommu_ctx_probe(struct platform_device *pdev)
{
	struct qcom_iommu_ctx *ctx;
	struct device *dev = &pdev->dev;
	struct qcom_iommu_dev *qcom_iommu = dev_get_drvdata(dev->parent);
	int ret, irq;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = dev;
	platform_set_drvdata(pdev, ctx);

	ctx->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctx->base))
		return PTR_ERR(ctx->base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	if (of_device_is_compatible(dev->of_node, "qcom,msm-iommu-v2-sec"))
		ctx->secured_ctx = true;

	/* clear IRQs before registering fault handler, just in case the
	 * boot-loader left us a surprise.  Enable clocks via the parent
	 * device's PM runtime to avoid unclocked MMSS register accesses
	 * which cause a bus error on MSM8974.
	 */
	if (!ctx->secured_ctx) {
		ret = pm_runtime_resume_and_get(dev->parent);
		if (ret < 0)
			return ret;
		iommu_writel(ctx, ARM_SMMU_CB_FSR, iommu_readl(ctx, ARM_SMMU_CB_FSR));
		pm_runtime_put_sync(dev->parent);
	}

	ret = devm_request_irq(dev, irq,
			       qcom_iommu_fault,
			       IRQF_SHARED,
			       "qcom-iommu-fault",
			       ctx);
	if (ret) {
		dev_err(dev, "failed to request IRQ %u\n", irq);
		return ret;
	}

	ret = get_asid(dev->of_node);
	if (ret < 0) {
		dev_err(dev, "missing reg property\n");
		return ret;
	}

	ctx->asid = ret;

	dev_dbg(dev, "found asid %u\n", ctx->asid);

	qcom_iommu->ctxs[ctx->asid] = ctx;

	return 0;
}

static void qcom_iommu_ctx_remove(struct platform_device *pdev)
{
	struct qcom_iommu_dev *qcom_iommu = dev_get_drvdata(pdev->dev.parent);
	struct qcom_iommu_ctx *ctx = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);

	qcom_iommu->ctxs[ctx->asid] = NULL;
}

static const struct of_device_id ctx_of_match[] = {
	{ .compatible = "qcom,msm-iommu-v1-ns" },
	{ .compatible = "qcom,msm-iommu-v1-sec" },
	{ .compatible = "qcom,msm-iommu-v2-ns" },
	{ .compatible = "qcom,msm-iommu-v2-sec" },
	{ /* sentinel */ }
};

static struct platform_driver qcom_iommu_ctx_driver = {
	.driver	= {
		.name		= "qcom-iommu-ctx",
		.of_match_table	= ctx_of_match,
	},
	.probe	= qcom_iommu_ctx_probe,
	.remove = qcom_iommu_ctx_remove,
};

static bool qcom_iommu_has_secure_context(struct qcom_iommu_dev *qcom_iommu)
{
	struct device_node *child;

	for_each_child_of_node(qcom_iommu->dev->of_node, child) {
		if (of_device_is_compatible(child, "qcom,msm-iommu-v1-sec") ||
		    of_device_is_compatible(child, "qcom,msm-iommu-v2-sec")) {
			of_node_put(child);
			return true;
		}
	}

	return false;
}

static int qcom_iommu_device_probe(struct platform_device *pdev)
{
	struct device_node *child;
	struct qcom_iommu_dev *qcom_iommu;
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct clk *clk;
	int ret, max_asid = 0;

	/* find the max asid (which is 1:1 to ctx bank idx), so we know how
	 * many child ctx devices we have:
	 */
	for_each_child_of_node(dev->of_node, child)
		max_asid = max(max_asid, get_asid(child));

	qcom_iommu = devm_kzalloc(dev, struct_size(qcom_iommu, ctxs, max_asid + 1),
				  GFP_KERNEL);
	if (!qcom_iommu)
		return -ENOMEM;
	qcom_iommu->max_asid = max_asid;
	qcom_iommu->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res) {
		qcom_iommu->local_base = devm_ioremap_resource(dev, res);
		if (IS_ERR(qcom_iommu->local_base))
			return PTR_ERR(qcom_iommu->local_base);
	}

	clk = devm_clk_get(dev, "iface");
	if (IS_ERR(clk)) {
		dev_err(dev, "failed to get iface clock\n");
		return PTR_ERR(clk);
	}
	qcom_iommu->clks[CLK_IFACE].clk = clk;

	clk = devm_clk_get(dev, "bus");
	if (IS_ERR(clk)) {
		dev_err(dev, "failed to get bus clock\n");
		return PTR_ERR(clk);
	}
	qcom_iommu->clks[CLK_BUS].clk = clk;

	clk = devm_clk_get_optional(dev, "tbu");
	if (IS_ERR(clk)) {
		dev_err(dev, "failed to get tbu clock\n");
		return PTR_ERR(clk);
	}
	qcom_iommu->clks[CLK_TBU].clk = clk;

	clk = devm_clk_get_optional(dev, "mmssnoc");
	if (IS_ERR(clk)) {
		dev_err(dev, "failed to get mmssnoc clock\n");
		return PTR_ERR(clk);
	}
	qcom_iommu->clks[CLK_MMSSNOC].clk = clk;

	/*
	 * Optional MMSS<->EBI interconnect. of_icc_get() returns NULL when the
	 * node has no "interconnects" property, so this is a no-op everywhere
	 * except SoCs that need the bus vote (MSM8974).
	 */
	qcom_iommu->icc_path = devm_of_icc_get(dev, NULL);
	if (IS_ERR(qcom_iommu->icc_path))
		return dev_err_probe(dev, PTR_ERR(qcom_iommu->icc_path),
				     "failed to get interconnect path\n");

	if (of_property_read_u32(dev->of_node, "qcom,iommu-secure-id",
				 &qcom_iommu->sec_id)) {
		/*
		 * A non-secure IOMMU (e.g. the MSM8974 GPU/Venus SMMUs) has no
		 * TrustZone-owned stream mapping: HLOS programs it directly and
		 * no restore_sec_cfg()/secure page-table pool is needed. All
		 * secure-only paths below are gated by
		 * qcom_iommu_has_secure_context(), so sec_id stays unused here.
		 */
		qcom_iommu->sec_id = -1;
		qcom_iommu->nonsecure = true;
	}

	if (qcom_iommu_has_secure_context(qcom_iommu)) {
		ret = qcom_iommu_sec_ptbl_init(dev);
		if (ret) {
			dev_err(dev, "cannot init secure pg table(%d)\n", ret);
			return ret;
		}
	}

	platform_set_drvdata(pdev, qcom_iommu);

	pm_runtime_enable(dev);

	/* register context bank devices, which are child nodes: */
	ret = devm_of_platform_populate(dev);
	if (ret) {
		dev_err(dev, "Failed to populate iommu contexts\n");
		goto err_pm_disable;
	}

	ret = iommu_device_sysfs_add(&qcom_iommu->iommu, dev, NULL,
				     dev_name(dev));
	if (ret) {
		dev_err(dev, "Failed to register iommu in sysfs\n");
		goto err_pm_disable;
	}

	ret = iommu_device_register(&qcom_iommu->iommu, &qcom_iommu_ops, dev);
	if (ret) {
		dev_err(dev, "Failed to register iommu\n");
		goto err_pm_disable;
	}

	if (qcom_iommu->local_base && qcom_iommu_has_secure_context(qcom_iommu)) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0)
			goto err_pm_disable;
		writel_relaxed(0xffffffff, qcom_iommu->local_base + SMMU_INTR_SEL_NS);
		pm_runtime_put_sync(dev);
	}

	return 0;

err_pm_disable:
	pm_runtime_disable(dev);
	return ret;
}

static void qcom_iommu_device_remove(struct platform_device *pdev)
{
	struct qcom_iommu_dev *qcom_iommu = platform_get_drvdata(pdev);

	pm_runtime_force_suspend(&pdev->dev);
	platform_set_drvdata(pdev, NULL);
	iommu_device_sysfs_remove(&qcom_iommu->iommu);
	iommu_device_unregister(&qcom_iommu->iommu);
}

/*
 * Re-apply the global space and every attached context bank after the SMMU
 * has lost power. On MSM8974 the non-secure GPU SMMU shares the GPU power
 * domain, so when the GPU runtime-suspends the SMMU is power-collapsed and
 * all of its registers reset. The secure path is restored by TrustZone
 * (qcom_scm_restore_sec_cfg); the non-secure path must re-establish the
 * global control/stream mapping and each context bank itself, or the next
 * GPU access silently stalls with translation disabled (no fault, just a
 * hang) -- the classic "second submit after idle wedges the GPU".
 */
static void qcom_iommu_nonsecure_restore(struct qcom_iommu_dev *qcom_iommu)
{
	int i;

	if (!qcom_iommu->nonsecure || !qcom_iommu->glb_inited)
		return;

	qcom_iommu_glb_halt(qcom_iommu);
	qcom_iommu_glb_reset(qcom_iommu);

	for (i = 0; i <= qcom_iommu->max_asid; i++) {
		struct qcom_iommu_ctx *ctx = qcom_iommu->ctxs[i];
		struct qcom_iommu_domain *qcom_domain;
		struct io_pgtable_cfg *cfg;
		u32 reg;

		if (!ctx || !ctx->domain)
			continue;
		qcom_domain = to_qcom_iommu_domain(ctx->domain);
		if (!qcom_domain->pgtbl_ops)
			continue;
		cfg = &io_pgtable_ops_to_pgtable(qcom_domain->pgtbl_ops)->cfg;

		qcom_iommu_glb_program_ctx(qcom_iommu, ctx);

		iommu_writel(ctx, ARM_SMMU_CB_SCTLR, 0);
		iommu_writel(ctx, ARM_SMMU_CB_FAR, 0);
		iommu_writel(ctx, ARM_SMMU_CB_FSR, ARM_SMMU_CB_FSR_FAULT);
		iommu_writeq(ctx, ARM_SMMU_CB_TTBR0,
			     cfg->arm_lpae_s1_cfg.ttbr |
			     FIELD_PREP(ARM_SMMU_TTBRn_ASID, ctx->asid));
		iommu_writeq(ctx, ARM_SMMU_CB_TTBR1, 0);
		iommu_writel(ctx, ARM_SMMU_CB_TCR,
			     arm_smmu_lpae_tcr(cfg) | ARM_SMMU_TCR_EAE);
		iommu_writel(ctx, ARM_SMMU_CB_S1_MAIR0,
			     cfg->arm_lpae_s1_cfg.mair);
		iommu_writel(ctx, ARM_SMMU_CB_S1_MAIR1,
			     cfg->arm_lpae_s1_cfg.mair >> 32);

		reg = ARM_SMMU_SCTLR_CFIE | ARM_SMMU_SCTLR_CFRE |
		      ARM_SMMU_SCTLR_TRE | ARM_SMMU_SCTLR_M |
		      ARM_SMMU_SCTLR_S1_ASIDPNE | ARM_SMMU_SCTLR_CFCFG |
		      ARM_SMMU_SCTLR_AFFD;
		if (IS_ENABLED(CONFIG_CPU_BIG_ENDIAN))
			reg |= ARM_SMMU_SCTLR_E;
		iommu_writel(ctx, ARM_SMMU_CB_SCTLR, reg);
	}

	qcom_iommu_glb_resume(qcom_iommu);
}

static int __maybe_unused qcom_iommu_resume(struct device *dev)
{
	struct qcom_iommu_dev *qcom_iommu = dev_get_drvdata(dev);
	int ret;

	/*
	 * Vote the MMSS<->EBI interconnect up before enabling clocks or
	 * touching any SMMU register. On MSM8974 the SMMU's AXI master is
	 * otherwise un-arbitrated on the multimedia NoC and the first
	 * register access raises a NoC/xPU error that resets the SoC. No-op
	 * where icc_path is NULL (SoCs without an "interconnects" property).
	 */
	if (qcom_iommu->icc_path) {
		ret = icc_set_bw(qcom_iommu->icc_path, 0, MBps_to_icc(150));
		if (ret)
			return ret;
	}

	ret = clk_bulk_prepare_enable(CLK_NUM, qcom_iommu->clks);
	if (ret < 0)
		goto err_icc;

	if (dev->pm_domain && qcom_iommu_has_secure_context(qcom_iommu)) {
		ret = qcom_scm_restore_sec_cfg(qcom_iommu->sec_id, 0);
		if (ret)
			goto err_clk;
	}

	/* Non-secure: re-establish SMMU state lost to power collapse. */
	qcom_iommu_nonsecure_restore(qcom_iommu);

	return 0;

err_clk:
	clk_bulk_disable_unprepare(CLK_NUM, qcom_iommu->clks);
err_icc:
	if (qcom_iommu->icc_path)
		icc_set_bw(qcom_iommu->icc_path, 0, 0);
	return ret;
}

static int __maybe_unused qcom_iommu_suspend(struct device *dev)
{
	struct qcom_iommu_dev *qcom_iommu = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(CLK_NUM, qcom_iommu->clks);
	if (qcom_iommu->icc_path)
		icc_set_bw(qcom_iommu->icc_path, 0, 0);

	return 0;
}

static const struct dev_pm_ops qcom_iommu_pm_ops = {
	SET_RUNTIME_PM_OPS(qcom_iommu_suspend, qcom_iommu_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

static const struct of_device_id qcom_iommu_of_match[] = {
	{ .compatible = "qcom,msm-iommu-v1" },
	{ .compatible = "qcom,msm-iommu-v2" },
	{ /* sentinel */ }
};

static struct platform_driver qcom_iommu_driver = {
	.driver	= {
		.name		= "qcom-iommu",
		.of_match_table	= qcom_iommu_of_match,
		.pm		= &qcom_iommu_pm_ops,
	},
	.probe	= qcom_iommu_device_probe,
	.remove = qcom_iommu_device_remove,
};

static int __init qcom_iommu_init(void)
{
	int ret;

	ret = platform_driver_register(&qcom_iommu_ctx_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&qcom_iommu_driver);
	if (ret)
		platform_driver_unregister(&qcom_iommu_ctx_driver);

	return ret;
}
device_initcall(qcom_iommu_init);
