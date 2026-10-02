//! Zero-copy 3D frames on macOS (doc 12 §4): the embed backend renders each
//! presented frame into a ring of IOSurfaces (BGRA8, top-down) and hands us
//! each surface once; we wrap it in a Metal texture through wgpu-hal. Per
//! frame the backend only says which slot is complete; nothing is copied.
//!
//! The backend keeps the IOSurface alive while it is offered; Metal holds
//! its own reference for the texture's lifetime.

use objc2_io_surface::IOSurfaceRef;
use objc2_metal::{MTLDevice, MTLPixelFormat, MTLTextureDescriptor, MTLTextureType, MTLTextureUsage};
use wgpu::hal::api::Metal;

/// Wrap `iosurface` (an `IOSurfaceRef` from the backend) as a wgpu texture.
pub fn import(
    device: &wgpu::Device,
    iosurface: *mut std::ffi::c_void,
    w: u32,
    h: u32,
    srgb: bool,
) -> Result<wgpu::Texture, String> {
    if iosurface.is_null() {
        return Err("null IOSurface".into());
    }
    let hal = unsafe { device.as_hal::<Metal>() }.ok_or("not a Metal device")?;
    let (mtl_format, wgpu_format) = if srgb {
        (MTLPixelFormat::BGRA8Unorm_sRGB, wgpu::TextureFormat::Bgra8UnormSrgb)
    } else {
        (MTLPixelFormat::BGRA8Unorm, wgpu::TextureFormat::Bgra8Unorm)
    };
    let desc = unsafe {
        MTLTextureDescriptor::texture2DDescriptorWithPixelFormat_width_height_mipmapped(
            mtl_format, w as usize, h as usize, false,
        )
    };
    desc.setUsage(MTLTextureUsage::ShaderRead);
    // SAFETY: the backend guarantees a live IOSurfaceRef for as long as the
    // slot is offered; Metal retains it for the texture.
    let surface: &IOSurfaceRef = unsafe { &*(iosurface as *const IOSurfaceRef) };
    let raw = hal
        .raw_device()
        .newTextureWithDescriptor_iosurface_plane(&desc, surface, 0)
        .ok_or("newTextureWithDescriptor:iosurface:plane: returned nil")?;
    // an associated function on the Metal backend (no receiver)
    let hal_tex = unsafe {
        wgpu::hal::metal::Device::texture_from_raw(
            raw,
            wgpu_format,
            MTLTextureType::Type2D,
            1,
            1,
            wgpu::hal::CopyExtent {
                width: w,
                height: h,
                depth: 1,
            },
            None,
        )
    };
    drop(hal);
    let size = wgpu::Extent3d {
        width: w,
        height: h,
        depth_or_array_layers: 1,
    };
    let tex = unsafe {
        device.create_texture_from_hal::<Metal>(
            hal_tex,
            &wgpu::TextureDescriptor {
                label: Some("3d IOSurface"),
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
