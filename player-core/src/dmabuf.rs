//! Zero-copy 3D frames on Linux (doc 12 §4): the embed backend renders each
//! presented frame into a ring of GBM buffers (linear, ARGB8888) and hands
//! us their dma-buf fds once; we import each one into a Vulkan image with
//! `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`
//! and wrap it as a wgpu texture. Per frame the backend only says which
//! slot is complete; nothing is copied.
//!
//! The device has to be opened with the extensions, so `create_device`
//! replaces `Adapter::request_device` when the adapter has them.

use ash::vk;
use std::ffi::CStr;
use wgpu::hal::api::Vulkan;

const EXTS: [&CStr; 3] = [
    c"VK_KHR_external_memory_fd",
    c"VK_EXT_external_memory_dma_buf",
    c"VK_EXT_image_drm_format_modifier",
];

/// DRM_FORMAT_ARGB8888 ('AR24'): [31:0] A:R:G:B little endian == BGRA8 bytes
pub const DRM_FORMAT_ARGB8888: u32 =
    (b'A' as u32) | ((b'R' as u32) << 8) | ((b'2' as u32) << 16) | ((b'4' as u32) << 24);

/// Open the device with the external-memory extensions when the adapter
/// supports them. Returns `(device, queue, zero_copy_available)`; `None`
/// means "not a Vulkan adapter, use the normal path".
pub fn create_device(
    adapter: &wgpu::Adapter,
    desc: &wgpu::DeviceDescriptor<'_>,
) -> Option<(wgpu::Device, wgpu::Queue, bool)> {
    let open = {
        let hal = unsafe { adapter.as_hal::<Vulkan>() }?;
        let instance = hal.shared_instance().raw_instance();
        let phd = hal.raw_physical_device();
        let available = unsafe { instance.enumerate_device_extension_properties(phd) }.ok()?;
        let has = |name: &CStr| {
            available
                .iter()
                .any(|p| p.extension_name_as_c_str().map(|n| n == name).unwrap_or(false))
        };
        let ok = EXTS.iter().all(|e| has(e));
        if !ok {
            eprintln!("[3d] Vulkan dma-buf import extensions missing; using readback");
        }
        let cb = move |args: wgpu::hal::vulkan::CreateDeviceCallbackArgs<'_, '_, '_>| {
            if ok {
                for e in EXTS {
                    if !args.extensions.contains(&e) {
                        args.extensions.push(e);
                    }
                }
            }
        };
        let open = unsafe {
            hal.open_with_callback(
                desc.required_features,
                &desc.required_limits,
                &desc.memory_hints,
                Some(Box::new(cb)),
            )
        }
        .ok()?;
        (open, ok)
    };
    let (open, ok) = open;
    let (device, queue) = unsafe { adapter.create_device_from_hal::<Vulkan>(open, desc) }.ok()?;
    Some((device, queue, ok))
}

#[allow(clippy::too_many_arguments)]
/// Import one dma-buf as a sampleable wgpu texture. Takes ownership of `fd`
/// (closed on failure; owned by the Vulkan allocation on success).
pub fn import(
    device: &wgpu::Device,
    fd: i32,
    w: u32,
    h: u32,
    stride: u32,
    fourcc: u32,
    modifier: u64,
    srgb: bool,
) -> Result<wgpu::Texture, String> {
    let r = import_inner(device, fd, w, h, stride, fourcc, modifier, srgb);
    if r.is_err() {
        unsafe { libc::close(fd) };
    }
    r
}

#[allow(clippy::too_many_arguments)]
fn import_inner(
    device: &wgpu::Device,
    fd: i32,
    w: u32,
    h: u32,
    stride: u32,
    fourcc: u32,
    modifier: u64,
    srgb: bool,
) -> Result<wgpu::Texture, String> {
    if fourcc != DRM_FORMAT_ARGB8888 {
        return Err(format!("unsupported fourcc 0x{fourcc:08x}"));
    }
    let hal = unsafe { device.as_hal::<Vulkan>() }.ok_or("not a Vulkan device")?;
    let raw = hal.raw_device().clone();
    let instance = hal.shared_instance().raw_instance().clone();
    let phd = hal.raw_physical_device();
    let (vk_format, wgpu_format) = if srgb {
        (vk::Format::B8G8R8A8_SRGB, wgpu::TextureFormat::Bgra8UnormSrgb)
    } else {
        (vk::Format::B8G8R8A8_UNORM, wgpu::TextureFormat::Bgra8Unorm)
    };

    let mut ext_mem = vk::ExternalMemoryImageCreateInfo::default()
        .handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);
    let layouts = [vk::SubresourceLayout {
        offset: 0,
        size: 0,
        row_pitch: stride as u64,
        array_pitch: 0,
        depth_pitch: 0,
    }];
    let mut mod_info = vk::ImageDrmFormatModifierExplicitCreateInfoEXT::default()
        .drm_format_modifier(modifier)
        .plane_layouts(&layouts);
    let info = vk::ImageCreateInfo::default()
        .image_type(vk::ImageType::TYPE_2D)
        .format(vk_format)
        .extent(vk::Extent3D {
            width: w,
            height: h,
            depth: 1,
        })
        .mip_levels(1)
        .array_layers(1)
        .samples(vk::SampleCountFlags::TYPE_1)
        .tiling(vk::ImageTiling::DRM_FORMAT_MODIFIER_EXT)
        .usage(vk::ImageUsageFlags::SAMPLED | vk::ImageUsageFlags::TRANSFER_SRC)
        .sharing_mode(vk::SharingMode::EXCLUSIVE)
        .initial_layout(vk::ImageLayout::UNDEFINED)
        .push_next(&mut ext_mem)
        .push_next(&mut mod_info);
    let image = unsafe { raw.create_image(&info, None) }.map_err(|e| format!("vkCreateImage: {e}"))?;

    let req = unsafe { raw.get_image_memory_requirements(image) };
    let fd_loader = ash::khr::external_memory_fd::Device::new(&instance, &raw);
    let mut fd_props = vk::MemoryFdPropertiesKHR::default();
    if let Err(e) = unsafe {
        fd_loader.get_memory_fd_properties(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT, fd, &mut fd_props)
    } {
        unsafe { raw.destroy_image(image, None) };
        return Err(format!("vkGetMemoryFdProperties: {e}"));
    }
    let mem_props = unsafe { instance.get_physical_device_memory_properties(phd) };
    let type_bits = req.memory_type_bits & fd_props.memory_type_bits;
    let Some(mem_type) = (0..mem_props.memory_type_count).find(|i| type_bits & (1 << i) != 0) else {
        unsafe { raw.destroy_image(image, None) };
        return Err("no memory type accepts the dma-buf".into());
    };
    let mut import_info = vk::ImportMemoryFdInfoKHR::default()
        .handle_type(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT)
        .fd(fd);
    let mut dedicated = vk::MemoryDedicatedAllocateInfo::default().image(image);
    let alloc = vk::MemoryAllocateInfo::default()
        .allocation_size(req.size)
        .memory_type_index(mem_type)
        .push_next(&mut import_info)
        .push_next(&mut dedicated);
    let memory = match unsafe { raw.allocate_memory(&alloc, None) } {
        Ok(m) => m,
        Err(e) => {
            unsafe { raw.destroy_image(image, None) };
            return Err(format!("vkAllocateMemory(import): {e}"));
        }
    };
    if let Err(e) = unsafe { raw.bind_image_memory(image, memory, 0) } {
        unsafe {
            raw.destroy_image(image, None);
            raw.free_memory(memory, None);
        }
        return Err(format!("vkBindImageMemory: {e}"));
    }

    let size = wgpu::Extent3d {
        width: w,
        height: h,
        depth_or_array_layers: 1,
    };
    let hal_desc = wgpu::hal::TextureDescriptor {
        label: Some("3d dma-buf"),
        size,
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu_format,
        usage: wgpu::TextureUses::RESOURCE | wgpu::TextureUses::COPY_SRC,
        memory_flags: wgpu::hal::MemoryFlags::empty(),
        view_formats: vec![],
    };
    let raw2 = raw.clone();
    let hal_tex = unsafe {
        hal.texture_from_raw(
            image,
            &hal_desc,
            Some(Box::new(move || {
                raw2.destroy_image(image, None);
                raw2.free_memory(memory, None);
            })),
            wgpu::hal::vulkan::TextureMemory::External,
        )
    };
    drop(hal);
    let tex = unsafe {
        device.create_texture_from_hal::<Vulkan>(
            hal_tex,
            &wgpu::TextureDescriptor {
                label: Some("3d dma-buf"),
                size,
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu_format,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_SRC,
                view_formats: &[],
            },
            wgpu::TextureUses::UNINITIALIZED,
        )
    };
    Ok(tex)
}
