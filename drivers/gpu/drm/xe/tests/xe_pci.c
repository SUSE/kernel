// SPDX-License-Identifier: GPL-2.0 AND MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "tests/xe_pci_test.h"

#include "tests/xe_test.h"

#include <kunit/test.h>
#include <kunit/visibility.h>

struct kunit_test_data {
	int ndevs;
	xe_device_fn xe_fn;
};

static int dev_to_xe_device_fn(struct device *dev, void *__data)

{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct kunit_test_data *data = __data;
	int ret = 0;
	int idx;

	data->ndevs++;

	if (drm_dev_enter(drm, &idx))
		ret = data->xe_fn(to_xe_device(dev_get_drvdata(dev)));
	drm_dev_exit(idx);

	return ret;
}

/**
 * xe_call_for_each_device - Iterate over all devices this driver binds to
 * @xe_fn: Function to call for each device.
 *
 * This function iterated over all devices this driver binds to, and calls
 * @xe_fn: for each one of them. If the called function returns anything else
 * than 0, iteration is stopped and the return value is returned by this
 * function. Across each function call, drm_dev_enter() / drm_dev_exit() is
 * called for the corresponding drm device.
 *
 * Return: Zero or the error code of a call to @xe_fn returning an error
 * code.
 */
int xe_call_for_each_device(xe_device_fn xe_fn)
{
	int ret;
	struct kunit_test_data data = {
	    .xe_fn = xe_fn,
	    .ndevs = 0,
	};

	ret = driver_for_each_device(&xe_pci_driver.driver, NULL,
				     &data, dev_to_xe_device_fn);

	if (!data.ndevs)
		kunit_skip(current->kunit_test, "test runs only on hardware\n");

	return ret;
}

int xe_pci_fake_device_init(struct xe_device *xe, enum xe_platform platform,
			    enum xe_subplatform subplatform)
{
	const struct pci_device_id *ent = pciidlist;
	const struct xe_device_desc *desc;
	const struct xe_subplatform_desc *subplatform_desc;

	if (platform == XE_TEST_PLATFORM_ANY) {
		desc = (const void *)ent->driver_data;
		subplatform_desc = NULL;
		goto done;
	}

	for (ent = pciidlist; ent->device; ent++) {
		desc = (const void *)ent->driver_data;
		if (desc->platform == platform)
			break;
	}

	if (!ent->device)
		return -ENODEV;

	if (subplatform == XE_TEST_SUBPLATFORM_ANY) {
		subplatform_desc = desc->subplatforms;
		goto done;
	}

	for (subplatform_desc = desc->subplatforms;
	     subplatform_desc && subplatform_desc->subplatform;
	     subplatform_desc++)
		if (subplatform_desc->subplatform == subplatform)
			break;

	if (subplatform == XE_SUBPLATFORM_NONE && subplatform_desc)
		return -ENODEV;

	if (subplatform != XE_SUBPLATFORM_NONE && !subplatform_desc)
		return -ENODEV;

done:
	xe_info_init(xe, desc, subplatform_desc);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(xe_pci_fake_device_init);
