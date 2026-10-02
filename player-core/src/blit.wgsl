// Fullscreen-triangle blit of the guest framebuffer texture.
// Geometry (aspect, integer scale) is handled by the viewport on the CPU
// side; the librashader chain slots in between texture and this pass (M1).

struct VsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> VsOut {
    // covers the viewport with one triangle; uv 0..1 across the visible quad
    let x = f32(i32(i & 1u) * 4 - 1);
    let y = f32(i32(i >> 1u) * 4 - 1);
    var out: VsOut;
    out.pos = vec4<f32>(x, y, 0.0, 1.0);
    out.uv = vec2<f32>((x + 1.0) * 0.5, 1.0 - (y + 1.0) * 0.5);
    return out;
}

@group(0) @binding(0) var fb_tex: texture_2d<f32>;
@group(0) @binding(1) var fb_smp: sampler;

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    return textureSample(fb_tex, fb_smp, in.uv);
}
