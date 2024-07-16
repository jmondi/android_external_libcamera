/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2021, Google Inc.
 *
 * Allocate FrameBuffer using gralloc API
 */

#include <memory>
#include <vector>
#include <unistd.h>

#include <libcamera/base/log.h>
#include <libcamera/base/shared_fd.h>

#include "libcamera/internal/formats.h"
#include "libcamera/internal/framebuffer.h"

#include <hardware/camera3.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra-semi"
#include <ui/GraphicBufferAllocator.h>
#pragma GCC diagnostic pop
#include <utils/Errors.h>
#include "cros_gralloc_handle.h"

#include "../camera_device.h"
#include "../frame_buffer_allocator.h"
#include "../hal_framebuffer.h"

using namespace libcamera;

LOG_DECLARE_CATEGORY(HAL)

namespace {
class GenericFrameBufferData : public FrameBuffer::Private
{
	LIBCAMERA_DECLARE_PUBLIC(FrameBuffer)

public:
	GenericFrameBufferData(android::GraphicBufferAllocator &allocDevice,
			       buffer_handle_t handle,
			       const std::vector<FrameBuffer::Plane> &planes)
		: FrameBuffer::Private(planes), allocDevice_(allocDevice),
		  handle_(handle)
	{
		ASSERT(handle_);
	}

	~GenericFrameBufferData() override
	{
		/*
		 * allocDevice_ is used to destroy handle_. allocDevice_ is
		 * owned by PlatformFrameBufferAllocator::Private.
		 * GenericFrameBufferData must be destroyed before it is
		 * destroyed.
		 *
		 * \todo Consider managing alloc_device_t with std::shared_ptr
		 * if this is difficult to maintain.
		 *
		 * \todo Thread safety against alloc_device_t is not documented.
		 * Is it no problem to call alloc/free in parallel?
		 */
		android::status_t status = allocDevice_.free(handle_);
		if (status != android::NO_ERROR)
			LOG(HAL, Error) << "Error freeing framebuffer: " << status;
	}

private:
	android::GraphicBufferAllocator &allocDevice_;
	const buffer_handle_t handle_;
};
} /* namespace */

class PlatformFrameBufferAllocator::Private : public Extensible::Private
{
	LIBCAMERA_DECLARE_PUBLIC(PlatformFrameBufferAllocator)

public:
	Private(CameraDevice *const cameraDevice)
		: cameraDevice_(cameraDevice),
		  allocDevice_(android::GraphicBufferAllocator::get())
	{
	}

	~Private() = default;

	std::unique_ptr<HALFrameBuffer>
	allocate(int halPixelFormat, const libcamera::Size &size, uint32_t usage);

private:
	const CameraDevice *const cameraDevice_;
	android::GraphicBufferAllocator &allocDevice_;
};

std::unique_ptr<HALFrameBuffer>
PlatformFrameBufferAllocator::Private::allocate(int halPixelFormat,
						const libcamera::Size &size,
						uint32_t usage)
{
	uint32_t stride = 0;
	buffer_handle_t handle = nullptr;

	LOG(HAL, Debug) << "Private::allocate: pixelFormat=" << halPixelFormat << " size=" << size << " usage=" << usage;

	android::status_t status = allocDevice_.allocate(size.width, size.height, halPixelFormat,
							 1 /*layerCount*/, usage, &handle, &stride,
							 "libcameraHAL");

	if (status != android::NO_ERROR) {
		LOG(HAL, Error) << "failed buffer allocation: " << status;
		return nullptr;
	}


	if (!handle) {
		LOG(HAL, Fatal) << "invalid buffer_handle_t";
		return nullptr;
	}							 

	/* This code assumes the planes are mapped consecutively. */
	const libcamera::PixelFormat pixelFormat =
		cameraDevice_->capabilities()->toPixelFormat(halPixelFormat);
	const auto &info = PixelFormatInfo::info(pixelFormat);
	

	auto cros_handle = reinterpret_cast<cros_gralloc_handle_t>(handle);
	std::vector<FrameBuffer::Plane> planes(cros_handle->num_planes);

	SharedFD fd{ handle->data[0] };
	const size_t maxDmaLength = lseek(fd.get(), 0, SEEK_END);
	
	LOG(HAL, Debug) << "Private::allocate: created fd=" << fd.get() 
					<< " pixelFormat=" << info.name 
					<< " width=" << cros_handle->width
					<< " height=" << cros_handle->height
					<< " req stride=" << stride
					<< " numPlanes=" << cros_handle->num_planes
					<< " numFds=" << handle->numFds
					<< " numInts=" << handle->numInts
					<< " dmaLength=" << maxDmaLength;

	for(int i=0; i<cros_handle->numFds; i++) {
		int fdd = cros_handle->fds[i];
		const size_t len = lseek(fdd, 0, SEEK_END);
		LOG(HAL, Debug) << "Private::allocate: fd info fd=" << fdd 
						<< " len=" << len;
	}

	for(int i=0; i<cros_handle->num_planes; i++) {
		LOG(HAL, Debug) << "Private::allocate: PLANE DATA Index=" << i
						<< " size=" << cros_handle->sizes[i]
						<< " offset=" << cros_handle->offsets[i]
						<< " stride=" << cros_handle->strides[i];
	}
	
	size_t offset = 0;
	for (auto [i, plane] : utils::enumerate(planes)) {
		//SharedFD fdd{ handle->data[i] };
		//size_t planeSize = info.planeSize(size.height, i, stride);
		size_t planeSize = cros_handle->sizes[i];

		// if(planeSize > maxDmaLength) {
		// 	planeSize = maxDmaLength;
		// }

		LOG(HAL, Debug) << "Private::allocate: planeInfo i=" << i << " offset=" << offset << " size=" << planeSize;

		plane.fd = fd;
		plane.offset = cros_handle->offsets[i];
		plane.length = cros_handle->sizes[i];
		offset += planeSize;
	}

	return std::make_unique<HALFrameBuffer>(
		std::make_unique<GenericFrameBufferData>(
			allocDevice_, handle, planes),
		handle);
}

PUBLIC_FRAME_BUFFER_ALLOCATOR_IMPLEMENTATION
