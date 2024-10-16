/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _UAPI_XE_DRM_H_
#define _UAPI_XE_DRM_H_

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* Please note that modifications to all structs defined here are
 * subject to backwards-compatibility constraints.
 */

/**
 * struct xe_user_extension - Base class for defining a chain of extensions
 *
 * Many interfaces need to grow over time. In most cases we can simply
 * extend the struct and have userspace pass in more data. Another option,
 * as demonstrated by Vulkan's approach to providing extensions for forward
 * and backward compatibility, is to use a list of optional structs to
 * provide those extra details.
 *
 * The key advantage to using an extension chain is that it allows us to
 * redefine the interface more easily than an ever growing struct of
 * increasing complexity, and for large parts of that interface to be
 * entirely optional. The downside is more pointer chasing; chasing across
 * the __user boundary with pointers encapsulated inside u64.
 *
 * Example chaining:
 *
 * .. code-block:: C
 *
 *	struct xe_user_extension ext3 {
 *		.next_extension = 0, // end
 *		.name = ...,
 *	};
 *	struct xe_user_extension ext2 {
 *		.next_extension = (uintptr_t)&ext3,
 *		.name = ...,
 *	};
 *	struct xe_user_extension ext1 {
 *		.next_extension = (uintptr_t)&ext2,
 *		.name = ...,
 *	};
 *
 * Typically the struct xe_user_extension would be embedded in some uAPI
 * struct, and in this case we would feed it the head of the chain(i.e ext1),
 * which would then apply all of the above extensions.
 *
 */
struct xe_user_extension {
	/**
	 * @next_extension:
	 *
	 * Pointer to the next struct xe_user_extension, or zero if the end.
	 */
	__u64 next_extension;

	/**
	 * @name: Name of the extension.
	 *
	 * Note that the name here is just some integer.
	 *
	 * Also note that the name space for this is not global for the whole
	 * driver, but rather its scope/meaning is limited to the specific piece
	 * of uAPI which has embedded the struct xe_user_extension.
	 */
	__u32 name;

	/**
	 * @pad: MBZ
	 *
	 * All undefined bits must be zero.
	 */
	__u32 pad;
};

/*
 * xe specific ioctls.
 *
 * The device specific ioctl range is [DRM_COMMAND_BASE, DRM_COMMAND_END) ie
 * [0x40, 0xa0) (a0 is excluded). The numbers below are defined as offset
 * against DRM_COMMAND_BASE and should be between [0x0, 0x60).
 */
#define DRM_XE_DEVICE_QUERY		0x00
#define DRM_XE_GEM_CREATE		0x01
#define DRM_XE_GEM_MMAP_OFFSET		0x02
#define DRM_XE_VM_CREATE		0x03
#define DRM_XE_VM_DESTROY		0x04
#define DRM_XE_VM_BIND			0x05
#define DRM_XE_ENGINE_CREATE		0x06
#define DRM_XE_ENGINE_DESTROY		0x07
#define DRM_XE_EXEC			0x08
#define DRM_XE_MMIO			0x09
#define DRM_XE_ENGINE_SET_PROPERTY	0x0a
#define DRM_XE_WAIT_USER_FENCE		0x0b
#define DRM_XE_VM_MADVISE		0x0c
#define DRM_XE_ENGINE_GET_PROPERTY	0x0d

/* Must be kept compact -- no holes */
#define DRM_IOCTL_XE_DEVICE_QUERY		DRM_IOWR(DRM_COMMAND_BASE + DRM_XE_DEVICE_QUERY, struct drm_xe_device_query)
#define DRM_IOCTL_XE_GEM_CREATE			DRM_IOWR(DRM_COMMAND_BASE + DRM_XE_GEM_CREATE, struct drm_xe_gem_create)
#define DRM_IOCTL_XE_GEM_MMAP_OFFSET		DRM_IOWR(DRM_COMMAND_BASE + DRM_XE_GEM_MMAP_OFFSET, struct drm_xe_gem_mmap_offset)
#define DRM_IOCTL_XE_VM_CREATE			DRM_IOWR(DRM_COMMAND_BASE + DRM_XE_VM_CREATE, struct drm_xe_vm_create)
#define DRM_IOCTL_XE_VM_DESTROY			 DRM_IOW(DRM_COMMAND_BASE + DRM_XE_VM_DESTROY, struct drm_xe_vm_destroy)
#define DRM_IOCTL_XE_VM_BIND			 DRM_IOW(DRM_COMMAND_BASE + DRM_XE_VM_BIND, struct drm_xe_vm_bind)
#define DRM_IOCTL_XE_ENGINE_CREATE		DRM_IOWR(DRM_COMMAND_BASE + DRM_XE_ENGINE_CREATE, struct drm_xe_engine_create)
#define DRM_IOCTL_XE_ENGINE_GET_PROPERTY	DRM_IOWR(DRM_COMMAND_BASE + DRM_XE_ENGINE_GET_PROPERTY, struct drm_xe_engine_get_property)
#define DRM_IOCTL_XE_ENGINE_DESTROY		 DRM_IOW(DRM_COMMAND_BASE + DRM_XE_ENGINE_DESTROY, struct drm_xe_engine_destroy)
#define DRM_IOCTL_XE_EXEC			 DRM_IOW(DRM_COMMAND_BASE + DRM_XE_EXEC, struct drm_xe_exec)
#define DRM_IOCTL_XE_MMIO			DRM_IOWR(DRM_COMMAND_BASE + DRM_XE_MMIO, struct drm_xe_mmio)
#define DRM_IOCTL_XE_ENGINE_SET_PROPERTY	 DRM_IOW(DRM_COMMAND_BASE + DRM_XE_ENGINE_SET_PROPERTY, struct drm_xe_engine_set_property)
#define DRM_IOCTL_XE_WAIT_USER_FENCE		DRM_IOWR(DRM_COMMAND_BASE + DRM_XE_WAIT_USER_FENCE, struct drm_xe_wait_user_fence)
#define DRM_IOCTL_XE_VM_MADVISE			 DRM_IOW(DRM_COMMAND_BASE + DRM_XE_VM_MADVISE, struct drm_xe_vm_madvise)

/**
 * enum drm_xe_memory_class - Supported memory classes.
 */
enum drm_xe_memory_class {
	/** @XE_MEM_REGION_CLASS_SYSMEM: Represents system memory. */
	XE_MEM_REGION_CLASS_SYSMEM = 0,
	/**
	 * @XE_MEM_REGION_CLASS_VRAM: On discrete platforms, this
	 * represents the memory that is local to the device, which we
	 * call VRAM. Not valid on integrated platforms.
	 */
	XE_MEM_REGION_CLASS_VRAM
};

/**
 * struct drm_xe_query_mem_region - Describes some region as known to
 * the driver.
 */
struct drm_xe_query_mem_region {
	/**
	 * @mem_class: The memory class describing this region.
	 *
	 * See enum drm_xe_memory_class for supported values.
	 */
	__u16 mem_class;
	/**
	 * @instance: The instance for this region.
	 *
	 * The @mem_class and @instance taken together will always give
	 * a unique pair.
	 */
	__u16 instance;
	/** @pad: MBZ */
	__u32 pad;
	/**
	 * @min_page_size: Min page-size in bytes for this region.
	 *
	 * When the kernel allocates memory for this region, the
	 * underlying pages will be at least @min_page_size in size.
	 *
	 * Important note: When userspace allocates a GTT address which
	 * can point to memory allocated from this region, it must also
	 * respect this minimum alignment. This is enforced by the
	 * kernel.
	 */
	__u32 min_page_size;
	/**
	 * @max_page_size: Max page-size in bytes for this region.
	 */
	__u32 max_page_size;
	/**
	 * @total_size: The usable size in bytes for this region.
	 */
	__u64 total_size;
	/**
	 * @used: Estimate of the memory used in bytes for this region.
	 *
	 * Requires CAP_PERFMON or CAP_SYS_ADMIN to get reliable
	 * accounting.  Without this the value here will always equal
	 * zero.
	 */
	__u64 used;
	/**
	 * @cpu_visible_size: How much of this region can be CPU
	 * accessed, in bytes.
	 *
	 * This will always be <= @total_size, and the remainder (if
	 * any) will not be CPU accessible. If the CPU accessible part
	 * is smaller than @total_size then this is referred to as a
	 * small BAR system.
	 *
	 * On systems without small BAR (full BAR), the probed_size will
	 * always equal the @total_size, since all of it will be CPU
	 * accessible.
	 *
	 * Note this is only tracked for XE_MEM_REGION_CLASS_VRAM
	 * regions (for other types the value here will always equal
	 * zero).
	 */
	__u64 cpu_visible_size;
	/**
	 * @cpu_visible_used: Estimate of CPU visible memory used, in
	 * bytes.
	 *
	 * Requires CAP_PERFMON or CAP_SYS_ADMIN to get reliable
	 * accounting. Without this the value here will always equal
	 * zero.  Note this is only currently tracked for
	 * XE_MEM_REGION_CLASS_VRAM regions (for other types the value
	 * here will always be zero).
	 */
	__u64 cpu_visible_used;
	/** @reserved: MBZ */
	__u64 reserved[6];
};

/**
 * struct drm_xe_query_mem_usage - describe memory regions and usage
 *
 * If a query is made with a struct drm_xe_device_query where .query
 * is equal to DRM_XE_DEVICE_QUERY_MEM_USAGE, then the reply uses
 * struct drm_xe_query_mem_usage in .data.
 */
struct drm_xe_query_mem_usage {
	/** @num_regions: number of memory regions returned in @regions */
	__u32 num_regions;
	/** @pad: MBZ */
	__u32 pad;
	/** @regions: The returned regions for this device */
	struct drm_xe_query_mem_region regions[];
};

/**
 * struct drm_xe_query_config - describe the device configuration
 *
 * If a query is made with a struct drm_xe_device_query where .query
 * is equal to DRM_XE_DEVICE_QUERY_CONFIG, then the reply uses
 * struct drm_xe_query_config in .data.
 */
struct drm_xe_query_config {
	/** @num_params: number of parameters returned in info */
	__u32 num_params;

	/** @pad: MBZ */
	__u32 pad;

#define XE_QUERY_CONFIG_REV_AND_DEVICE_ID	0
#define XE_QUERY_CONFIG_FLAGS			1
	#define XE_QUERY_CONFIG_FLAGS_HAS_VRAM		(0x1 << 0)
	#define XE_QUERY_CONFIG_FLAGS_USE_GUC		(0x1 << 1)
#define XE_QUERY_CONFIG_MIN_ALIGNEMENT		2
#define XE_QUERY_CONFIG_VA_BITS			3
#define XE_QUERY_CONFIG_GT_COUNT		4
#define XE_QUERY_CONFIG_MEM_REGION_COUNT	5
#define XE_QUERY_CONFIG_MAX_ENGINE_PRIORITY	6
#define XE_QUERY_CONFIG_NUM_PARAM		(XE_QUERY_CONFIG_MAX_ENGINE_PRIORITY + 1)
	/** @info: array of elements containing the config info */
	__u64 info[];
};

/**
 * struct drm_xe_query_gts - describe GTs
 *
 * If a query is made with a struct drm_xe_device_query where .query
 * is equal to DRM_XE_DEVICE_QUERY_GTS, then the reply uses struct
 * drm_xe_query_gts in .data.
 */
struct drm_xe_query_gts {
	/** @num_gt: number of GTs returned in gts */
	__u32 num_gt;

	/** @pad: MBZ */
	__u32 pad;

	/**
	 * @gts: The GTs returned for this device
	 *
	 * TODO: convert drm_xe_query_gt to proper kernel-doc.
	 * TODO: Perhaps info about every mem region relative to this GT? e.g.
	 * bandwidth between this GT and remote region?
	 */
	struct drm_xe_query_gt {
#define XE_QUERY_GT_TYPE_MAIN		0
#define XE_QUERY_GT_TYPE_REMOTE		1
#define XE_QUERY_GT_TYPE_MEDIA		2
		__u16 type;
		__u16 instance;
		__u32 clock_freq;
		__u64 features;
		__u64 native_mem_regions;	/* bit mask of instances from drm_xe_query_mem_usage */
		__u64 slow_mem_regions;		/* bit mask of instances from drm_xe_query_mem_usage */
		__u64 inaccessible_mem_regions;	/* bit mask of instances from drm_xe_query_mem_usage */
		__u64 reserved[8];
	} gts[];
};

/**
 * struct drm_xe_query_topology_mask - describe the topology mask of a GT
 *
 * This is the hardware topology which reflects the internal physical
 * structure of the GPU.
 *
 * If a query is made with a struct drm_xe_device_query where .query
 * is equal to DRM_XE_DEVICE_QUERY_GT_TOPOLOGY, then the reply uses
 * struct drm_xe_query_topology_mask in .data.
 */
struct drm_xe_query_topology_mask {
	/** @gt_id: GT ID the mask is associated with */
	__u16 gt_id;

	/*
	 * To query the mask of Dual Sub Slices (DSS) available for geometry
	 * operations. For example a query response containing the following
	 * in mask:
	 *   DSS_GEOMETRY    ff ff ff ff 00 00 00 00
	 * means 32 DSS are available for geometry.
	 */
#define XE_TOPO_DSS_GEOMETRY	(1 << 0)
	/*
	 * To query the mask of Dual Sub Slices (DSS) available for compute
	 * operations. For example a query response containing the following
	 * in mask:
	 *   DSS_COMPUTE    ff ff ff ff 00 00 00 00
	 * means 32 DSS are available for compute.
	 */
#define XE_TOPO_DSS_COMPUTE	(1 << 1)
	/*
	 * To query the mask of Execution Units (EU) available per Dual Sub
	 * Slices (DSS). For example a query response containing the following
	 * in mask:
	 *   EU_PER_DSS    ff ff 00 00 00 00 00 00
	 * means each DSS has 16 EU.
	 */
#define XE_TOPO_EU_PER_DSS	(1 << 2)
	/** @type: type of mask */
	__u16 type;

	/** @num_bytes: number of bytes in requested mask */
	__u32 num_bytes;

	/** @mask: little-endian mask of @num_bytes */
	__u8 mask[];
};

/**
 * struct drm_xe_device_query - main structure to query device information
 *
 * If size is set to 0, the driver fills it with the required size for the
 * requested type of data to query. If size is equal to the required size,
 * the queried information is copied into data.
 *
 * For example the following code snippet allows retrieving and printing
 * information about the device engines with DRM_XE_DEVICE_QUERY_ENGINES:
 *
 * .. code-block:: C
 *
 *	struct drm_xe_engine_class_instance *hwe;
 *	struct drm_xe_device_query query = {
 *		.extensions = 0,
 *		.query = DRM_XE_DEVICE_QUERY_ENGINES,
 *		.size = 0,
 *		.data = 0,
 *	};
 *	ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query);
 *	hwe = malloc(query.size);
 *	query.data = (uintptr_t)hwe;
 *	ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query);
 *	int num_engines = query.size / sizeof(*hwe);
 *	for (int i = 0; i < num_engines; i++) {
 *		printf("Engine %d: %s\n", i,
 *			hwe[i].engine_class == DRM_XE_ENGINE_CLASS_RENDER ? "RENDER":
 *			hwe[i].engine_class == DRM_XE_ENGINE_CLASS_COPY ? "COPY":
 *			hwe[i].engine_class == DRM_XE_ENGINE_CLASS_VIDEO_DECODE ? "VIDEO_DECODE":
 *			hwe[i].engine_class == DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE ? "VIDEO_ENHANCE":
 *			hwe[i].engine_class == DRM_XE_ENGINE_CLASS_COMPUTE ? "COMPUTE":
 *			"UNKNOWN");
 *	}
 *	free(hwe);
 */
struct drm_xe_device_query {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

#define DRM_XE_DEVICE_QUERY_ENGINES	0
#define DRM_XE_DEVICE_QUERY_MEM_USAGE	1
#define DRM_XE_DEVICE_QUERY_CONFIG	2
#define DRM_XE_DEVICE_QUERY_GTS		3
#define DRM_XE_DEVICE_QUERY_HWCONFIG	4
#define DRM_XE_DEVICE_QUERY_GT_TOPOLOGY	5
	/** @query: The type of data to query */
	__u32 query;

	/** @size: Size of the queried data */
	__u32 size;

	/** @data: Queried data is placed here */
	__u64 data;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

struct drm_xe_gem_create {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/**
	 * @size: Requested size for the object
	 *
	 * The (page-aligned) allocated size for the object will be returned.
	 */
	__u64 size;

#define XE_GEM_CREATE_FLAG_DEFER_BACKING	(0x1 << 24)
#define XE_GEM_CREATE_FLAG_SCANOUT		(0x1 << 25)
/*
 * When using VRAM as a possible placement, ensure that the corresponding VRAM
 * allocation will always use the CPU accessible part of VRAM. This is important
 * for small-bar systems (on full-bar systems this gets turned into a noop).
 *
 * Note: System memory can be used as an extra placement if the kernel should
 * spill the allocation to system memory, if space can't be made available in
 * the CPU accessible part of VRAM (giving the same behaviour as the i915
 * interface, see I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS).
 *
 * Note: For clear-color CCS surfaces the kernel needs to read the clear-color
 * value stored in the buffer, and on discrete platforms we need to use VRAM for
 * display surfaces, therefore the kernel requires setting this flag for such
 * objects, otherwise an error is thrown on small-bar systems.
 */
#define XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM	(0x1 << 26)
	/**
	 * @flags: Flags, currently a mask of memory instances of where BO can
	 * be placed
	 */
	__u32 flags;

	/**
	 * @vm_id: Attached VM, if any
	 *
	 * If a VM is specified, this BO must:
	 *
	 *  1. Only ever be bound to that VM.
	 *
	 *  2. Cannot be exported as a PRIME fd.
	 */
	__u32 vm_id;

	/**
	 * @handle: Returned handle for the object.
	 *
	 * Object handles are nonzero.
	 */
	__u32 handle;

	/** @pad: MBZ */
	__u32 pad;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

struct drm_xe_gem_mmap_offset {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @handle: Handle for the object being mapped. */
	__u32 handle;

	/** @flags: Must be zero */
	__u32 flags;

	/** @offset: The fake offset to use for subsequent mmap call */
	__u64 offset;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

/**
 * struct drm_xe_vm_bind_op_error_capture - format of VM bind op error capture
 */
struct drm_xe_vm_bind_op_error_capture {
	/** @error: errno that occured */
	__s32 error;

	/** @op: operation that encounter an error */
	__u32 op;

	/** @addr: address of bind op */
	__u64 addr;

	/** @size: size of bind */
	__u64 size;
};

/** struct drm_xe_ext_vm_set_property - VM set property extension */
struct drm_xe_ext_vm_set_property {
	/** @base: base user extension */
	struct xe_user_extension base;

#define XE_VM_PROPERTY_BIND_OP_ERROR_CAPTURE_ADDRESS		0
	/** @property: property to set */
	__u32 property;

	/** @pad: MBZ */
	__u32 pad;

	/** @value: property value */
	__u64 value;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

struct drm_xe_vm_create {
#define XE_VM_EXTENSION_SET_PROPERTY	0
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

#define DRM_XE_VM_CREATE_SCRATCH_PAGE	(0x1 << 0)
#define DRM_XE_VM_CREATE_COMPUTE_MODE	(0x1 << 1)
#define DRM_XE_VM_CREATE_ASYNC_BIND_OPS	(0x1 << 2)
#define DRM_XE_VM_CREATE_FAULT_MODE	(0x1 << 3)
	/** @flags: Flags */
	__u32 flags;

	/** @vm_id: Returned VM ID */
	__u32 vm_id;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

struct drm_xe_vm_destroy {
	/** @vm_id: VM ID */
	__u32 vm_id;

	/** @pad: MBZ */
	__u32 pad;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

struct drm_xe_vm_bind_op {
	/**
	 * @obj: GEM object to operate on, MBZ for MAP_USERPTR, MBZ for UNMAP
	 */
	__u32 obj;

	/** @pad: MBZ */
	__u32 pad;

	union {
		/**
		 * @obj_offset: Offset into the object, MBZ for CLEAR_RANGE,
		 * ignored for unbind
		 */
		__u64 obj_offset;

		/** @userptr: user pointer to bind on */
		__u64 userptr;
	};

	/**
	 * @range: Number of bytes from the object to bind to addr, MBZ for UNMAP_ALL
	 */
	__u64 range;

	/** @addr: Address to operate on, MBZ for UNMAP_ALL */
	__u64 addr;

	/**
	 * @tile_mask: Mask for which tiles to create binds for, 0 == All tiles,
	 * only applies to creating new VMAs
	 */
	__u64 tile_mask;

#define XE_VM_BIND_OP_MAP		0x0
#define XE_VM_BIND_OP_UNMAP		0x1
#define XE_VM_BIND_OP_MAP_USERPTR	0x2
#define XE_VM_BIND_OP_RESTART		0x3
#define XE_VM_BIND_OP_UNMAP_ALL		0x4
#define XE_VM_BIND_OP_PREFETCH		0x5

#define XE_VM_BIND_FLAG_READONLY	(0x1 << 16)
	/*
	 * A bind ops completions are always async, hence the support for out
	 * sync. This flag indicates the allocation of the memory for new page
	 * tables and the job to program the pages tables is asynchronous
	 * relative to the IOCTL. That part of a bind operation can fail under
	 * memory pressure, the job in practice can't fail unless the system is
	 * totally shot.
	 *
	 * If this flag is clear and the IOCTL doesn't return an error, in
	 * practice the bind op is good and will complete.
	 *
	 * If this flag is set and doesn't return an error, the bind op can
	 * still fail and recovery is needed. If configured, the bind op that
	 * caused the error will be captured in drm_xe_vm_bind_op_error_capture.
	 * Once the user sees the error (via a ufence +
	 * XE_VM_PROPERTY_BIND_OP_ERROR_CAPTURE_ADDRESS), it should free memory
	 * via non-async unbinds, and then restart all queue'd async binds op via
	 * XE_VM_BIND_OP_RESTART. Or alternatively the user should destroy the
	 * VM.
	 *
	 * This flag is only allowed when DRM_XE_VM_CREATE_ASYNC_BIND_OPS is
	 * configured in the VM and must be set if the VM is configured with
	 * DRM_XE_VM_CREATE_ASYNC_BIND_OPS and not in an error state.
	 */
#define XE_VM_BIND_FLAG_ASYNC		(0x1 << 17)
	/*
	 * Valid on a faulting VM only, do the MAP operation immediately rather
	 * than differing the MAP to the page fault handler.
	 */
#define XE_VM_BIND_FLAG_IMMEDIATE	(0x1 << 18)
	/*
	 * When the NULL flag is set, the page tables are setup with a special
	 * bit which indicates writes are dropped and all reads return zero.  In
	 * the future, the NULL flags will only be valid for XE_VM_BIND_OP_MAP
	 * operations, the BO handle MBZ, and the BO offset MBZ. This flag is
	 * intended to implement VK sparse bindings.
	 */
#define XE_VM_BIND_FLAG_NULL		(0x1 << 19)
	/** @op: Operation to perform (lower 16 bits) and flags (upper 16 bits) */
	__u32 op;

	/** @mem_region: Memory region to prefetch VMA to, instance not a mask */
	__u32 region;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

struct drm_xe_vm_bind {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @vm_id: The ID of the VM to bind to */
	__u32 vm_id;

	/**
	 * @engine_id: engine_id, must be of class DRM_XE_ENGINE_CLASS_VM_BIND
	 * and engine must have same vm_id. If zero, the default VM bind engine
	 * is used.
	 */
	__u32 engine_id;

	/** @num_binds: number of binds in this IOCTL */
	__u32 num_binds;

	/** @pad: MBZ */
	__u32 pad;

	union {
		/** @bind: used if num_binds == 1 */
		struct drm_xe_vm_bind_op bind;

		/**
		 * @vector_of_binds: userptr to array of struct
		 * drm_xe_vm_bind_op if num_binds > 1
		 */
		__u64 vector_of_binds;
	};

	/** @num_syncs: amount of syncs to wait on */
	__u32 num_syncs;

	/** @pad2: MBZ */
	__u32 pad2;

	/** @syncs: pointer to struct drm_xe_sync array */
	__u64 syncs;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

/** struct drm_xe_ext_engine_set_property - engine set property extension */
struct drm_xe_ext_engine_set_property {
	/** @base: base user extension */
	struct xe_user_extension base;

	/** @property: property to set */
	__u32 property;

	/** @pad: MBZ */
	__u32 pad;

	/** @value: property value */
	__u64 value;
};

/**
 * struct drm_xe_engine_set_property - engine set property
 *
 * Same namespace for extensions as drm_xe_engine_create
 */
struct drm_xe_engine_set_property {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @engine_id: Engine ID */
	__u32 engine_id;

#define XE_ENGINE_SET_PROPERTY_PRIORITY			0
#define XE_ENGINE_SET_PROPERTY_TIMESLICE		1
#define XE_ENGINE_SET_PROPERTY_PREEMPTION_TIMEOUT	2
	/*
	 * Long running or ULLS engine mode. DMA fences not allowed in this
	 * mode. Must match the value of DRM_XE_VM_CREATE_COMPUTE_MODE, serves
	 * as a sanity check the UMD knows what it is doing. Can only be set at
	 * engine create time.
	 */
#define XE_ENGINE_SET_PROPERTY_COMPUTE_MODE		3
#define XE_ENGINE_SET_PROPERTY_PERSISTENCE		4
#define XE_ENGINE_SET_PROPERTY_JOB_TIMEOUT		5
#define XE_ENGINE_SET_PROPERTY_ACC_TRIGGER		6
#define XE_ENGINE_SET_PROPERTY_ACC_NOTIFY		7
#define XE_ENGINE_SET_PROPERTY_ACC_GRANULARITY		8
	/** @property: property to set */
	__u32 property;

	/** @value: property value */
	__u64 value;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

/** struct drm_xe_engine_class_instance - instance of an engine class */
struct drm_xe_engine_class_instance {
#define DRM_XE_ENGINE_CLASS_RENDER		0
#define DRM_XE_ENGINE_CLASS_COPY		1
#define DRM_XE_ENGINE_CLASS_VIDEO_DECODE	2
#define DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE	3
#define DRM_XE_ENGINE_CLASS_COMPUTE		4
	/*
	 * Kernel only class (not actual hardware engine class). Used for
	 * creating ordered queues of VM bind operations.
	 */
#define DRM_XE_ENGINE_CLASS_VM_BIND		5
	__u16 engine_class;

	__u16 engine_instance;
	__u16 gt_id;
};

struct drm_xe_engine_create {
#define XE_ENGINE_EXTENSION_SET_PROPERTY               0
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @width: submission width (number BB per exec) for this engine */
	__u16 width;

	/** @num_placements: number of valid placements for this engine */
	__u16 num_placements;

	/** @vm_id: VM to use for this engine */
	__u32 vm_id;

	/** @flags: MBZ */
	__u32 flags;

	/** @engine_id: Returned engine ID */
	__u32 engine_id;

	/**
	 * @instances: user pointer to a 2-d array of struct
	 * drm_xe_engine_class_instance
	 *
	 * length = width (i) * num_placements (j)
	 * index = j + i * width
	 */
	__u64 instances;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

struct drm_xe_engine_get_property {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @engine_id: Engine ID */
	__u32 engine_id;

#define XE_ENGINE_GET_PROPERTY_BAN			0
	/** @property: property to get */
	__u32 property;

	/** @value: property value */
	__u64 value;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

struct drm_xe_engine_destroy {
	/** @engine_id: Engine ID */
	__u32 engine_id;

	/** @pad: MBZ */
	__u32 pad;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

struct drm_xe_sync {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

#define DRM_XE_SYNC_SYNCOBJ		0x0
#define DRM_XE_SYNC_TIMELINE_SYNCOBJ	0x1
#define DRM_XE_SYNC_DMA_BUF		0x2
#define DRM_XE_SYNC_USER_FENCE		0x3
#define DRM_XE_SYNC_SIGNAL		0x10
	__u32 flags;

	/** @pad: MBZ */
	__u32 pad;

	union {
		__u32 handle;

		/**
		 * @addr: Address of user fence. When sync passed in via exec
		 * IOCTL this a GPU address in the VM. When sync passed in via
		 * VM bind IOCTL this is a user pointer. In either case, it is
		 * the users responsibility that this address is present and
		 * mapped when the user fence is signalled. Must be qword
		 * aligned.
		 */
		__u64 addr;
	};

	__u64 timeline_value;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

struct drm_xe_exec {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @engine_id: Engine ID for the batch buffer */
	__u32 engine_id;

	/** @num_syncs: Amount of struct drm_xe_sync in array. */
	__u32 num_syncs;

	/** @syncs: Pointer to struct drm_xe_sync array. */
	__u64 syncs;

	/**
	 * @address: address of batch buffer if num_batch_buffer == 1 or an
	 * array of batch buffer addresses
	 */
	__u64 address;

	/**
	 * @num_batch_buffer: number of batch buffer in this exec, must match
	 * the width of the engine
	 */
	__u16 num_batch_buffer;

	/** @pad: MBZ */
	__u16 pad[3];

	/** @reserved: Reserved */
	__u64 reserved[2];
};

struct drm_xe_mmio {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	__u32 addr;

#define DRM_XE_MMIO_8BIT	0x0
#define DRM_XE_MMIO_16BIT	0x1
#define DRM_XE_MMIO_32BIT	0x2
#define DRM_XE_MMIO_64BIT	0x3
#define DRM_XE_MMIO_BITS_MASK	0x3
#define DRM_XE_MMIO_READ	0x4
#define DRM_XE_MMIO_WRITE	0x8
	__u32 flags;

	__u64 value;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

/**
 * struct drm_xe_wait_user_fence - wait user fence
 *
 * Wait on user fence, XE will wakeup on every HW engine interrupt in the
 * instances list and check if user fence is complete::
 *
 *	(*addr & MASK) OP (VALUE & MASK)
 *
 * Returns to user on user fence completion or timeout.
 */
struct drm_xe_wait_user_fence {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	union {
		/**
		 * @addr: user pointer address to wait on, must qword aligned
		 */
		__u64 addr;

		/**
		 * @vm_id: The ID of the VM which encounter an error used with
		 * DRM_XE_UFENCE_WAIT_VM_ERROR. Upper 32 bits must be clear.
		 */
		__u64 vm_id;
	};

#define DRM_XE_UFENCE_WAIT_EQ	0
#define DRM_XE_UFENCE_WAIT_NEQ	1
#define DRM_XE_UFENCE_WAIT_GT	2
#define DRM_XE_UFENCE_WAIT_GTE	3
#define DRM_XE_UFENCE_WAIT_LT	4
#define DRM_XE_UFENCE_WAIT_LTE	5
	/** @op: wait operation (type of comparison) */
	__u16 op;

#define DRM_XE_UFENCE_WAIT_SOFT_OP	(1 << 0)	/* e.g. Wait on VM bind */
#define DRM_XE_UFENCE_WAIT_ABSTIME	(1 << 1)
#define DRM_XE_UFENCE_WAIT_VM_ERROR	(1 << 2)
	/** @flags: wait flags */
	__u16 flags;

	/** @pad: MBZ */
	__u32 pad;

	/** @value: compare value */
	__u64 value;

#define DRM_XE_UFENCE_WAIT_U8		0xffu
#define DRM_XE_UFENCE_WAIT_U16		0xffffu
#define DRM_XE_UFENCE_WAIT_U32		0xffffffffu
#define DRM_XE_UFENCE_WAIT_U64		0xffffffffffffffffu
	/** @mask: comparison mask */
	__u64 mask;
	/**
	 * @timeout: how long to wait before bailing, value in nanoseconds.
	 * Without DRM_XE_UFENCE_WAIT_ABSTIME flag set (relative timeout)
	 * it contains timeout expressed in nanoseconds to wait (fence will
	 * expire at now() + timeout).
	 * When DRM_XE_UFENCE_WAIT_ABSTIME flat is set (absolute timeout) wait
	 * will end at timeout (uses system MONOTONIC_CLOCK).
	 * Passing negative timeout leads to neverending wait.
	 *
	 * On relative timeout this value is updated with timeout left
	 * (for restarting the call in case of signal delivery).
	 * On absolute timeout this value stays intact (restarted call still
	 * expire at the same point of time).
	 */
	__s64 timeout;

	/**
	 * @num_engines: number of engine instances to wait on, must be zero
	 * when DRM_XE_UFENCE_WAIT_SOFT_OP set
	 */
	__u64 num_engines;

	/**
	 * @instances: user pointer to array of drm_xe_engine_class_instance to
	 * wait on, must be NULL when DRM_XE_UFENCE_WAIT_SOFT_OP set
	 */
	__u64 instances;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

struct drm_xe_vm_madvise {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @vm_id: The ID VM in which the VMA exists */
	__u32 vm_id;

	/** @pad: MBZ */
	__u32 pad;

	/** @range: Number of bytes in the VMA */
	__u64 range;

	/** @addr: Address of the VMA to operation on */
	__u64 addr;

	/*
	 * Setting the preferred location will trigger a migrate of the VMA
	 * backing store to new location if the backing store is already
	 * allocated.
	 *
	 * For DRM_XE_VM_MADVISE_PREFERRED_MEM_CLASS usage, see enum
	 * drm_xe_memory_class.
	 */
#define DRM_XE_VM_MADVISE_PREFERRED_MEM_CLASS	0
#define DRM_XE_VM_MADVISE_PREFERRED_GT		1
	/*
	 * In this case lower 32 bits are mem class, upper 32 are GT.
	 * Combination provides a single IOCTL plus migrate VMA to preferred
	 * location.
	 */
#define DRM_XE_VM_MADVISE_PREFERRED_MEM_CLASS_GT	2
	/*
	 * The CPU will do atomic memory operations to this VMA. Must be set on
	 * some devices for atomics to behave correctly.
	 */
#define DRM_XE_VM_MADVISE_CPU_ATOMIC		3
	/*
	 * The device will do atomic memory operations to this VMA. Must be set
	 * on some devices for atomics to behave correctly.
	 */
#define DRM_XE_VM_MADVISE_DEVICE_ATOMIC		4
	/*
	 * Priority WRT to eviction (moving from preferred memory location due
	 * to memory pressure). The lower the priority, the more likely to be
	 * evicted.
	 */
#define DRM_XE_VM_MADVISE_PRIORITY		5
#define		DRM_XE_VMA_PRIORITY_LOW		0
#define		DRM_XE_VMA_PRIORITY_NORMAL	1	/* Default */
#define		DRM_XE_VMA_PRIORITY_HIGH	2	/* Must be elevated user */
	/* Pin the VMA in memory, must be elevated user */
#define DRM_XE_VM_MADVISE_PIN			6
	/** @property: property to set */
	__u32 property;

	/** @pad2: MBZ */
	__u32 pad2;

	/** @value: property value */
	__u64 value;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

#if defined(__cplusplus)
}
#endif

#endif /* _UAPI_XE_DRM_H_ */
