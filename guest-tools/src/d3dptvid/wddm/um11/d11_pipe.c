/*
 * d11_pipe.c: the Direct3D 11 driver's pipeline (track M25): views,
 * states, shaders, input layouts and queries, each an object of this
 * device's context on the host by an id this driver picks; the bindings,
 * the draws and the clears. The DDI's enums for blend, comparison,
 * stencil, fill, cull, filter, address and topology carry the API's
 * values, so they go to the host as they are.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3dpt11.h"

static UINT new_obj(D11_DEV *d, D11_OBJ *o, UINT kind)
{
    ZeroMemory(o, sizeof(*o));
    o->id = ++d->next_id;
    o->kind = kind;
    return o->id;
}

static void destroy_obj(D11_DEV *d, D11_OBJ *o)
{
    d3dpt_d11_ref *t;

    if (!o || !o->id) {
        return;
    }
    t = (d3dpt_d11_ref *)tok(d, D3DPT_D11_DESTROY, sizeof(*t), 0);
    if (t) {
        t->a = o->kind;
        t->b = o->id;
    }
    o->id = 0;
}

static UINT obj_id(const void *p)
{
    return p ? ((const D11_OBJ *)p)->id : 0;
}

static SIZE_T obj_size(void)
{
    return sizeof(D11_OBJ);
}

/* ---------------------------------------------------------------- views */

/* a view keeps what made it, in the device's list */
static void view_keep(D11_DEV *d, D11_OBJ *o, UINT op, const d3dpt_d11_view *t)
{
    if (!o->res || !o->res->vram) {
        /* only a swap chain's buffers and opened surfaces rotate, and they
         * are in VRAM: a game's thousands of texture views walked at every
         * flip were ~5% of its driver time (track M25) */
        o->vop = 0;
        return;
    }
    o->vop = op;
    o->view = *t;
    o->vprev = NULL;
    o->vnext = d->views;
    if (d->views) {
        d->views->vprev = o;
    }
    d->views = o;
}

static void view_drop(D11_DEV *d, D11_OBJ *o)
{
    if (!o->vop) {
        return;
    }
    if (o->vprev) {
        o->vprev->vnext = o->vnext;
    } else if (d->views == o) {
        d->views = o->vnext;
    }
    if (o->vnext) {
        o->vnext->vprev = o->vprev;
    }
    o->vnext = o->vprev = NULL;
    o->vop = 0;
}

/* DXGI rotated these resources' identities (d11_dev.c): each view of one
 * of them is made again under its id on what the resource is now, as the
 * runtime expects of a view (DWM renders through views it made once per
 * buffer; without this it drew into one host buffer while the screen
 * flipped through the others) */
void d11_views_rotated(D11_DEV *d, D11_RES *const *res, UINT n)
{
    D11_OBJ *o;
    UINT i;

    for (o = d->views; o; o = o->vnext) {
        for (i = 0; i < n && o->res != res[i]; i++) {
        }
        if (i < n) {
            d3dpt_d11_view *t = (d3dpt_d11_view *)tok(d, o->vop, sizeof(*t), 1);

            if (t) {
                *t = o->view;
                patch_res(d, &t->res, o->res);
            }
        }
    }
}

static SIZE_T APIENTRY calc_srv(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATESHADERRESOURCEVIEW *c) { D11_TRACE("calc_srv"); (void)h; (void)c; return obj_size(); }
static SIZE_T APIENTRY calc_rtv(D3D10DDI_HDEVICE h, const D3D10DDIARG_CREATERENDERTARGETVIEW *c) { D11_TRACE("calc_rtv"); (void)h; (void)c; return obj_size(); }
static SIZE_T APIENTRY calc_dsv(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *c) { D11_TRACE("calc_dsv"); (void)h; (void)c; return obj_size(); }

/* The API's view dimension. The DDI names the view's own dimension (a
 * Texture2DArray view of a cube is TEXTURE2D) and leaves multisampling to
 * the resource; an array view of one slice that starts at 0 is the plain
 * dimension (the host's DXVK makes either kind of image view for either
 * kind of shader declaration). */
static void APIENTRY create_srv(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATESHADERRESOURCEVIEW *c, D3D10DDI_HSHADERRESOURCEVIEW hv,
                                D3D10DDI_HRTSHADERRESOURCEVIEW rt)
{ D11_TRACE("create_srv");
    D11_DEV *d = DEV(h);
    D11_OBJ *o = OBJ(hv);
    D11_RES *r = RES(c->hDrvResource);
    d3dpt_d11_view *t;

    (void)rt;
    new_obj(d, o, D3DPT_D11K_SRV);
    o->res = r;
    t = (d3dpt_d11_view *)tok(d, D3DPT_D11_SRV_CREATE, sizeof(*t), 1);
    if (!t) {
        return;
    }
    t->id = o->id;
    patch_res(d, &t->res, r);
    t->format = (UINT)c->Format;
    switch (c->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
        t->dim = 1;                                                  /* BUFFER */
        t->u[0] = c->Buffer.FirstElement;
        t->u[1] = c->Buffer.NumElements;
        break;
    case D3D11DDIRESOURCE_BUFFEREX:
        t->dim = 11;                                                 /* BUFFEREX */
        t->u[0] = c->BufferEx.FirstElement;
        t->u[1] = c->BufferEx.NumElements;
        t->u[2] = c->BufferEx.Flags;
        break;
    case D3D10DDIRESOURCE_TEXTURE1D:
        t->u[0] = c->Tex1D.MostDetailedMip;
        t->u[1] = c->Tex1D.MipLevels;
        if (c->Tex1D.ArraySize > 1 || c->Tex1D.FirstArraySlice || r->r.array > 1) {
            t->dim = 3;                                              /* TEXTURE1DARRAY */
            t->u[2] = c->Tex1D.FirstArraySlice;
            t->u[3] = c->Tex1D.ArraySize;
        } else {
            t->dim = 2;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        if (r->r.samples > 1) {
            if (c->Tex2D.ArraySize > 1 || c->Tex2D.FirstArraySlice || r->r.array > 1) {
                t->dim = 7;                                          /* TEXTURE2DMSARRAY */
                t->u[0] = c->Tex2D.FirstArraySlice;
                t->u[1] = c->Tex2D.ArraySize;
            } else {
                t->dim = 6;
            }
            break;
        }
        t->u[0] = c->Tex2D.MostDetailedMip;
        t->u[1] = c->Tex2D.MipLevels;
        if (c->Tex2D.ArraySize > 1 || c->Tex2D.FirstArraySlice || r->r.array > 1) {
            t->dim = 5;                                              /* TEXTURE2DARRAY */
            t->u[2] = c->Tex2D.FirstArraySlice;
            t->u[3] = c->Tex2D.ArraySize;
        } else {
            t->dim = 4;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE3D:
        t->dim = 8;
        t->u[0] = c->Tex3D.MostDetailedMip;
        t->u[1] = c->Tex3D.MipLevels;
        break;
    case D3D10DDIRESOURCE_TEXTURECUBE:
        t->u[0] = c->TexCube.MostDetailedMip;
        t->u[1] = c->TexCube.MipLevels;
        if (c->TexCube.NumCubes > 1 || c->TexCube.First2DArrayFace) {
            t->dim = 10;                                             /* TEXTURECUBEARRAY */
            t->u[2] = c->TexCube.First2DArrayFace;
            t->u[3] = c->TexCube.NumCubes;
        } else {
            t->dim = 9;
        }
        break;
    default:
        umd_log("d3d11: a shader resource view of dimension %u", c->ResourceDimension);
        break;
    }
    view_keep(d, o, D3DPT_D11_SRV_CREATE, t);
}

/* render target and depth views: a cube's faces are a 2D array */
static void tex_view(UINT *dim, UINT u[4], const D11_RES *r, UINT ddi_dim, UINT mip, UINT first, UINT size, BOOL dsv)
{
    BOOL arr = size > 1 || first || r->r.array > 1;

    if (ddi_dim == D3D10DDIRESOURCE_TEXTURE1D) {
        *dim = dsv ? (arr ? 2 : 1) : (arr ? 3 : 2);
        u[0] = mip;
        u[1] = first;
        u[2] = size;
        return;
    }
    if (r->r.samples > 1) {
        *dim = dsv ? (arr ? 6 : 5) : (arr ? 7 : 6);
        u[0] = first;
        u[1] = size;
        return;
    }
    *dim = dsv ? (arr ? 4 : 3) : (arr ? 5 : 4);
    u[0] = mip;
    u[1] = first;
    u[2] = size;
}

static void APIENTRY create_rtv(D3D10DDI_HDEVICE h, const D3D10DDIARG_CREATERENDERTARGETVIEW *c, D3D10DDI_HRENDERTARGETVIEW hv,
                                D3D10DDI_HRTRENDERTARGETVIEW rt)
{ D11_TRACE("create_rtv");
    D11_DEV *d = DEV(h);
    D11_OBJ *o = OBJ(hv);
    D11_RES *r = RES(c->hDrvResource);
    d3dpt_d11_view *t;

    (void)rt;
    new_obj(d, o, D3DPT_D11K_RTV);
    o->res = r;
    t = (d3dpt_d11_view *)tok(d, D3DPT_D11_RTV_CREATE, sizeof(*t), 1);
    if (!t) {
        return;
    }
    t->id = o->id;
    patch_res(d, &t->res, r);
    t->format = (UINT)c->Format;
    switch (c->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
        t->dim = 1;
        t->u[0] = c->Buffer.FirstElement;
        t->u[1] = c->Buffer.NumElements;
        break;
    case D3D10DDIRESOURCE_TEXTURE1D:
        tex_view(&t->dim, t->u, r, D3D10DDIRESOURCE_TEXTURE1D, c->Tex1D.MipSlice, c->Tex1D.FirstArraySlice, c->Tex1D.ArraySize, FALSE);
        break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        tex_view(&t->dim, t->u, r, D3D10DDIRESOURCE_TEXTURE2D, c->Tex2D.MipSlice, c->Tex2D.FirstArraySlice, c->Tex2D.ArraySize, FALSE);
        break;
    case D3D10DDIRESOURCE_TEXTURECUBE:
        tex_view(&t->dim, t->u, r, D3D10DDIRESOURCE_TEXTURE2D, c->TexCube.MipSlice, c->TexCube.FirstArraySlice, c->TexCube.ArraySize, FALSE);
        t->dim = r->r.samples > 1 ? 7 : 5;      /* always an array of faces */
        break;
    case D3D10DDIRESOURCE_TEXTURE3D:
        t->dim = 8;
        t->u[0] = c->Tex3D.MipSlice;
        t->u[1] = c->Tex3D.FirstW;
        t->u[2] = c->Tex3D.WSize;
        break;
    default:
        umd_log("d3d11: a render target view of dimension %u", c->ResourceDimension);
        break;
    }
    view_keep(d, o, D3DPT_D11_RTV_CREATE, t);
}

static void APIENTRY create_dsv(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *c, D3D10DDI_HDEPTHSTENCILVIEW hv,
                                D3D10DDI_HRTDEPTHSTENCILVIEW rt)
{ D11_TRACE("create_dsv");
    D11_DEV *d = DEV(h);
    D11_OBJ *o = OBJ(hv);
    D11_RES *r = RES(c->hDrvResource);
    d3dpt_d11_view *t;

    (void)rt;
    new_obj(d, o, D3DPT_D11K_DSV);
    o->res = r;
    t = (d3dpt_d11_view *)tok(d, D3DPT_D11_DSV_CREATE, sizeof(*t), 1);
    if (!t) {
        return;
    }
    t->id = o->id;
    patch_res(d, &t->res, r);
    t->format = (UINT)c->Format;
    t->flags = c->Flags & D3D11_DDI_CREATE_DSV_FLAG_MASK;
    switch (c->ResourceDimension) {
    case D3D10DDIRESOURCE_TEXTURE1D:
        tex_view(&t->dim, t->u, r, D3D10DDIRESOURCE_TEXTURE1D, c->Tex1D.MipSlice, c->Tex1D.FirstArraySlice, c->Tex1D.ArraySize, TRUE);
        break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        tex_view(&t->dim, t->u, r, D3D10DDIRESOURCE_TEXTURE2D, c->Tex2D.MipSlice, c->Tex2D.FirstArraySlice, c->Tex2D.ArraySize, TRUE);
        break;
    case D3D10DDIRESOURCE_TEXTURECUBE:
        tex_view(&t->dim, t->u, r, D3D10DDIRESOURCE_TEXTURE2D, c->TexCube.MipSlice, c->TexCube.FirstArraySlice, c->TexCube.ArraySize, TRUE);
        t->dim = r->r.samples > 1 ? 6 : 4;
        break;
    default:
        umd_log("d3d11: a depth stencil view of dimension %u", c->ResourceDimension);
        break;
    }
    view_keep(d, o, D3DPT_D11_DSV_CREATE, t);
}

static void APIENTRY destroy_srv(D3D10DDI_HDEVICE h, D3D10DDI_HSHADERRESOURCEVIEW v) { D11_TRACE("destroy_srv"); view_drop(DEV(h), OBJ(v)); destroy_obj(DEV(h), OBJ(v)); }
static void APIENTRY destroy_rtv(D3D10DDI_HDEVICE h, D3D10DDI_HRENDERTARGETVIEW v) { D11_TRACE("destroy_rtv"); view_drop(DEV(h), OBJ(v)); destroy_obj(DEV(h), OBJ(v)); }
static void APIENTRY destroy_dsv(D3D10DDI_HDEVICE h, D3D10DDI_HDEPTHSTENCILVIEW v) { D11_TRACE("destroy_dsv"); view_drop(DEV(h), OBJ(v)); destroy_obj(DEV(h), OBJ(v)); }

static void APIENTRY gen_mips(D3D10DDI_HDEVICE h, D3D10DDI_HSHADERRESOURCEVIEW v)
{ D11_TRACE("gen_mips");
    D11_DEV *d = DEV(h);
    UINT *t = (UINT *)tok(d, D3DPT_D11_GEN_MIPS, 4, 0);

    if (t) {
        t[0] = OBJ(v)->id;
    }
}

/* --------------------------------------------------------------- states */

static SIZE_T APIENTRY calc_blend(D3D10DDI_HDEVICE h, const D3D10_1_DDI_BLEND_DESC *c) { D11_TRACE("calc_blend"); (void)h; (void)c; return obj_size(); }
static SIZE_T APIENTRY calc_depth(D3D10DDI_HDEVICE h, const D3D10_DDI_DEPTH_STENCIL_DESC *c) { D11_TRACE("calc_depth"); (void)h; (void)c; return obj_size(); }
static SIZE_T APIENTRY calc_raster(D3D10DDI_HDEVICE h, const D3D10_DDI_RASTERIZER_DESC *c) { D11_TRACE("calc_raster"); (void)h; (void)c; return obj_size(); }
static SIZE_T APIENTRY calc_sampler(D3D10DDI_HDEVICE h, const D3D10_DDI_SAMPLER_DESC *c) { D11_TRACE("calc_sampler"); (void)h; (void)c; return obj_size(); }

static void APIENTRY create_blend(D3D10DDI_HDEVICE h, const D3D10_1_DDI_BLEND_DESC *c, D3D10DDI_HBLENDSTATE hs, D3D10DDI_HRTBLENDSTATE rt)
{ D11_TRACE("create_blend");
    D11_DEV *d = DEV(h);
    d3dpt_d11_blend *t;
    UINT i;

    (void)rt;
    new_obj(d, OBJ(hs), D3DPT_D11K_BLEND);
    t = (d3dpt_d11_blend *)tok(d, D3DPT_D11_BLEND_CREATE, sizeof(*t), 0);
    if (!t) {
        return;
    }
    t->id = OBJ(hs)->id;
    t->alpha_to_coverage = c->AlphaToCoverageEnable;
    t->independent = c->IndependentBlendEnable;
    for (i = 0; i < 8; i++) {
        const D3D10_DDI_RENDER_TARGET_BLEND_DESC1 *b = &c->RenderTarget[c->IndependentBlendEnable ? i : 0];

        t->rt[i].enable = b->BlendEnable;
        t->rt[i].src = b->SrcBlend;
        t->rt[i].dst = b->DestBlend;
        t->rt[i].op = b->BlendOp;
        t->rt[i].src_a = b->SrcBlendAlpha;
        t->rt[i].dst_a = b->DestBlendAlpha;
        t->rt[i].op_a = b->BlendOpAlpha;
        t->rt[i].mask = b->RenderTargetWriteMask;
    }
}

static void APIENTRY create_depth(D3D10DDI_HDEVICE h, const D3D10_DDI_DEPTH_STENCIL_DESC *c, D3D10DDI_HDEPTHSTENCILSTATE hs,
                                  D3D10DDI_HRTDEPTHSTENCILSTATE rt)
{ D11_TRACE("create_depth");
    D11_DEV *d = DEV(h);
    d3dpt_d11_depth *t;

    (void)rt;
    new_obj(d, OBJ(hs), D3DPT_D11K_DEPTH);
    t = (d3dpt_d11_depth *)tok(d, D3DPT_D11_DEPTH_CREATE, sizeof(*t), 0);
    if (!t) {
        return;
    }
    t->id = OBJ(hs)->id;
    t->depth_enable = c->DepthEnable;
    t->write_mask = c->DepthWriteMask;
    t->func = c->DepthFunc;
    t->stencil_enable = c->StencilEnable;
    t->read_mask = c->StencilReadMask;
    t->write_mask8 = c->StencilWriteMask;
    t->front[0] = c->FrontFace.StencilFailOp;
    t->front[1] = c->FrontFace.StencilDepthFailOp;
    t->front[2] = c->FrontFace.StencilPassOp;
    t->front[3] = c->FrontFace.StencilFunc;
    t->back[0] = c->BackFace.StencilFailOp;
    t->back[1] = c->BackFace.StencilDepthFailOp;
    t->back[2] = c->BackFace.StencilPassOp;
    t->back[3] = c->BackFace.StencilFunc;
}

static void APIENTRY create_raster(D3D10DDI_HDEVICE h, const D3D10_DDI_RASTERIZER_DESC *c, D3D10DDI_HRASTERIZERSTATE hs,
                                   D3D10DDI_HRTRASTERIZERSTATE rt)
{ D11_TRACE("create_raster");
    D11_DEV *d = DEV(h);
    d3dpt_d11_raster *t;

    (void)rt;
    new_obj(d, OBJ(hs), D3DPT_D11K_RASTER);
    t = (d3dpt_d11_raster *)tok(d, D3DPT_D11_RASTER_CREATE, sizeof(*t), 0);
    if (!t) {
        return;
    }
    t->id = OBJ(hs)->id;
    t->fill = c->FillMode;
    t->cull = c->CullMode;
    t->front_ccw = c->FrontCounterClockwise;
    t->bias = c->DepthBias;
    t->bias_clamp = c->DepthBiasClamp;
    t->slope_bias = c->SlopeScaledDepthBias;
    t->depth_clip = c->DepthClipEnable;
    t->scissor = c->ScissorEnable;
    t->multisample = c->MultisampleEnable;
    t->aa_lines = c->AntialiasedLineEnable;
}

static void APIENTRY create_sampler(D3D10DDI_HDEVICE h, const D3D10_DDI_SAMPLER_DESC *c, D3D10DDI_HSAMPLER hs, D3D10DDI_HRTSAMPLER rt)
{ D11_TRACE("create_sampler");
    D11_DEV *d = DEV(h);
    d3dpt_d11_sampler *t;

    (void)rt;
    new_obj(d, OBJ(hs), D3DPT_D11K_SAMPLER);
    t = (d3dpt_d11_sampler *)tok(d, D3DPT_D11_SAMPLER_CREATE, sizeof(*t), 0);
    if (!t) {
        return;
    }
    t->id = OBJ(hs)->id;
    t->filter = (UINT)c->Filter & 0x1ffu;         /* not Direct3D 10's mono 1-bit text filter (bit 31) */
    t->address[0] = c->AddressU;
    t->address[1] = c->AddressV;
    t->address[2] = c->AddressW;
    t->lod_bias = c->MipLODBias;
    t->max_aniso = c->MaxAnisotropy;
    t->func = c->ComparisonFunc;
    CopyMemory(t->border, c->BorderColor, sizeof(t->border));
    t->min_lod = c->MinLOD;
    t->max_lod = c->MaxLOD;
}

static void APIENTRY destroy_blend(D3D10DDI_HDEVICE h, D3D10DDI_HBLENDSTATE s) { D11_TRACE("destroy_blend"); destroy_obj(DEV(h), OBJ(s)); }
static void APIENTRY destroy_depth(D3D10DDI_HDEVICE h, D3D10DDI_HDEPTHSTENCILSTATE s) { D11_TRACE("destroy_depth"); destroy_obj(DEV(h), OBJ(s)); }
static void APIENTRY destroy_raster(D3D10DDI_HDEVICE h, D3D10DDI_HRASTERIZERSTATE s) { D11_TRACE("destroy_raster"); destroy_obj(DEV(h), OBJ(s)); }
static void APIENTRY destroy_sampler(D3D10DDI_HDEVICE h, D3D10DDI_HSAMPLER s) { D11_TRACE("destroy_sampler"); destroy_obj(DEV(h), OBJ(s)); }

/* -------------------------------------------------------------- shaders */

static SIZE_T APIENTRY calc_shader(D3D10DDI_HDEVICE h, const UINT *code, const D3D10DDIARG_STAGE_IO_SIGNATURES *s)
{ D11_TRACE("calc_shader");
    (void)h; (void)code; (void)s;
    return obj_size();
}

static SIZE_T APIENTRY calc_gs_so(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *c,
                                  const D3D10DDIARG_STAGE_IO_SIGNATURES *s)
{ D11_TRACE("calc_gs_so");
    (void)h; (void)c; (void)s;
    return obj_size();
}

static SIZE_T APIENTRY calc_tess(D3D10DDI_HDEVICE h, const UINT *code, const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *s)
{ D11_TRACE("calc_tess");
    (void)h; (void)code; (void)s;
    return obj_size();
}

/* a stage's signatures as either interface gives them: Windows 7's
 * entries, or 11.1's, which add the stream and the component type */
typedef struct SIGS {
    const void *in, *out;
    UINT nin, nout;
    BOOL v11_1;
    const void *pc;                               /* v35: a hull or domain shader's patch constants */
    UINT npc;
} SIGS;

static SIGS sigs_tess(const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *s)
{
    SIGS g = { 0 };

    if (s) {
        g.in = s->pInputSignature;
        g.out = s->pOutputSignature;
        g.pc = s->pPatchConstantSignature;
        g.nin = s->NumInputSignatureEntries;
        g.nout = s->NumOutputSignatureEntries;
        g.npc = s->NumPatchConstantSignatureEntries;
    }
    return g;
}

static SIGS sigs_tess1(const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *s)
{
    SIGS g = { 0 };

    if (s) {
        g.in = s->pInputSignature;
        g.out = s->pOutputSignature;
        g.pc = s->pPatchConstantSignature;
        g.nin = s->NumInputSignatureEntries;
        g.nout = s->NumOutputSignatureEntries;
        g.npc = s->NumPatchConstantSignatureEntries;
        g.v11_1 = TRUE;
    }
    return g;
}

static SIGS sigs10(const D3D10DDIARG_STAGE_IO_SIGNATURES *s)
{
    SIGS g = { 0 };

    if (s) {
        g.in = s->pInputSignature;
        g.out = s->pOutputSignature;
        g.nin = s->NumInputSignatureEntries;
        g.nout = s->NumOutputSignatureEntries;
    }
    return g;
}

static SIGS sigs11_1(const D3D11_1DDIARG_STAGE_IO_SIGNATURES *s)
{
    SIGS g = { 0 };

    if (s) {
        g.in = s->pInputSignature;
        g.out = s->pOutputSignature;
        g.nin = s->NumInputSignatureEntries;
        g.nout = s->NumOutputSignatureEntries;
        g.v11_1 = TRUE;
    }
    return g;
}

static void sig_put(d3dpt_d11_sig *g, const SIGS *s, const void *list, UINT i)
{
    ZeroMemory(g, sizeof(*g));
    if (s->v11_1) {
        const D3D11_1DDIARG_SIGNATURE_ENTRY2 *e = (const D3D11_1DDIARG_SIGNATURE_ENTRY2 *)list + i;

        g->sv = e->SystemValue;
        g->reg = e->Register;
        g->mask = e->Mask;
        g->stream = e->Stream;
        g->type = e->RegisterComponentType;
    } else {
        const D3D10DDIARG_SIGNATURE_ENTRY *e = (const D3D10DDIARG_SIGNATURE_ENTRY *)list + i;

        g->sv = e->SystemValue;
        g->reg = e->Register;
        g->mask = e->Mask;
    }
}

/* SHADER_CREATE: the tokens (dword 1 is the program's length in dwords),
 * the signatures and a geometry shader's stream output */
static void shader(D11_DEV *d, D11_OBJ *o, UINT stage, const UINT *code, SIGS sg,
                   const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *so)
{
    const SIGS *s = &sg;
    UINT dw = code[1], nin = s->nin, nout = s->nout;
    UINT nso = so ? so->NumEntries : 0, i;
    /* v35: a hull or domain shader's patch-constant signature, counted in
     * nso, as d3dpt_d11_sig entries where stream output would be */
    BOOL tess = stage == D3DPT_D11S_HS || stage == D3DPT_D11S_DS;
    UINT npc = tess ? s->npc : 0;
    d3dpt_d11_shader *t;
    d3dpt_d11_sig *g;
    d3dpt_d11_so *e;

    new_obj(d, o, D3DPT_D11K_SHADER);
    o->extra = stage;
    t = (d3dpt_d11_shader *)tok(d, D3DPT_D11_SHADER_CREATE,
                                sizeof(*t) + dw * 4 + (nin + nout + npc) * sizeof(*g) + nso * sizeof(*e), 0);
    if (!t) {
        umd_log("d3d11: a shader of %u dwords dropped", dw);
        return;
    }
    t->id = o->id;
    t->stage = stage;
    t->code_dwords = dw;
    t->nin = nin;
    t->nout = nout;
    t->nso = nso;
    t->so_rast = so ? so->RasterizedStream : 0;
    for (i = 0; so && i < so->NumStrides && i < 4; i++) {
        t->so_strides[i] = so->BufferStridesInBytes[i];
    }
    CopyMemory(t + 1, code, dw * 4);
    g = (d3dpt_d11_sig *)((BYTE *)(t + 1) + dw * 4);
    for (i = 0; i < nin; i++, g++) {
        sig_put(g, s, s->in, i);
    }
    for (i = 0; i < nout; i++, g++) {
        sig_put(g, s, s->out, i);
    }
    if (tess) {
        t->nso = npc;
        for (i = 0; i < npc; i++, g++) {
            sig_put(g, s, s->pc, i);
        }
        return;
    }
    e = (d3dpt_d11_so *)g;
    for (i = 0; i < nso; i++, e++) {
        e->stream = so->pOutputStreamDecl[i].Stream;
        e->slot = so->pOutputStreamDecl[i].OutputSlot;
        e->reg = so->pOutputStreamDecl[i].RegisterIndex;
        e->mask = so->pOutputStreamDecl[i].RegisterMask;
    }
}

static void APIENTRY create_vs(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER rt,
                               const D3D10DDIARG_STAGE_IO_SIGNATURES *s)
{ D11_TRACE("create_vs");
    (void)rt;
    shader(DEV(h), OBJ(hs), D3DPT_D11S_VS, code, sigs10(s), NULL);
}

static void APIENTRY create_ps(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER rt,
                               const D3D10DDIARG_STAGE_IO_SIGNATURES *s)
{ D11_TRACE("create_ps");
    (void)rt;
    shader(DEV(h), OBJ(hs), D3DPT_D11S_PS, code, sigs10(s), NULL);
}

static void APIENTRY create_gs(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER rt,
                               const D3D10DDIARG_STAGE_IO_SIGNATURES *s)
{ D11_TRACE("create_gs");
    (void)rt;
    shader(DEV(h), OBJ(hs), D3DPT_D11S_GS, code, sigs10(s), NULL);
}

static void APIENTRY create_gs_so(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *c, D3D10DDI_HSHADER hs,
                                  D3D10DDI_HRTSHADER rt, const D3D10DDIARG_STAGE_IO_SIGNATURES *s)
{ D11_TRACE("create_gs_so");
    (void)rt;
    shader(DEV(h), OBJ(hs), D3DPT_D11S_GS, c->pShaderCode, sigs10(s), c);
}

/* hull and domain shaders (feature level 11_0, protocol v35): their
 * control-point signatures and the patch constants between them */
static void APIENTRY create_hs(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER rt,
                               const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *s)
{ D11_TRACE("create_hs");
    (void)rt;
    shader(DEV(h), OBJ(hs), D3DPT_D11S_HS, code, sigs_tess(s), NULL);
}

static void APIENTRY create_ds(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER rt,
                               const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *s)
{ D11_TRACE("create_ds");
    (void)rt;
    shader(DEV(h), OBJ(hs), D3DPT_D11S_DS, code, sigs_tess(s), NULL);
}

/* v34: no signatures */
static void APIENTRY create_cs(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER rt)
{ D11_TRACE("create_cs");
    SIGS none;

    (void)rt;
    ZeroMemory(&none, sizeof(none));
    shader(DEV(h), OBJ(hs), D3DPT_D11S_CS, code, none, NULL);
}

static void APIENTRY destroy_shader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s) { D11_TRACE("destroy_shader"); destroy_obj(DEV(h), OBJ(s)); }

static SIZE_T APIENTRY calc_layout(D3D10DDI_HDEVICE h, const D3D10DDIARG_CREATEELEMENTLAYOUT *c) { D11_TRACE("calc_layout"); (void)h; (void)c; return obj_size(); }

static void APIENTRY create_layout(D3D10DDI_HDEVICE h, const D3D10DDIARG_CREATEELEMENTLAYOUT *c, D3D10DDI_HELEMENTLAYOUT hl,
                                   D3D10DDI_HRTELEMENTLAYOUT rt)
{ D11_TRACE("create_layout");
    D11_DEV *d = DEV(h);
    d3dpt_d11_list *t;
    d3dpt_d11_element *e;
    UINT i;

    (void)rt;
    new_obj(d, OBJ(hl), D3DPT_D11K_LAYOUT);
    t = (d3dpt_d11_list *)tok(d, D3DPT_D11_LAYOUT_CREATE, sizeof(*t) + c->NumElements * sizeof(*e), 0);
    if (!t) {
        return;
    }
    t->id = OBJ(hl)->id;
    t->n = c->NumElements;
    e = (d3dpt_d11_element *)(t + 1);
    for (i = 0; i < c->NumElements; i++) {
        const D3D10DDIARG_INPUT_ELEMENT_DESC *s = &c->pVertexElements[i];

        e[i].slot = s->InputSlot;
        e[i].offset = s->AlignedByteOffset;
        e[i].format = (UINT)s->Format;
        e[i].per_instance = s->InputSlotClass == D3D10_DDI_INPUT_PER_INSTANCE_DATA;
        e[i].step = s->InstanceDataStepRate;
        e[i].reg = s->InputRegister;
    }
}

static void APIENTRY destroy_layout(D3D10DDI_HDEVICE h, D3D10DDI_HELEMENTLAYOUT l) { D11_TRACE("destroy_layout"); destroy_obj(DEV(h), OBJ(l)); }

/* -------------------------------------------------------------- queries */

/* the most a query's answer takes (Direct3D 11's pipeline statistics,
 * 88 bytes), fetched whatever size GetData names */
#define D11_QUERY_BYTES 96

typedef struct D11_QUERY {
    D11_OBJ o;
    UINT type;                                    /* the API's D3D11_QUERY */
    /* GetData without waiting (track M25): the host's answer goes into
     * this query's own result page; a lock that does not wait reads it
     * once the submission that fetched it has run */
    D11_RES *res;                                 /* the result page, made at the first GetData */
    UINT seq;                                     /* Ends so far */
    UINT get_seq;                                 /* the End the fetch in flight answers */
    BOOL pending;                                 /* a fetch in flight */
    BOOL have;                                    /* data holds the answer to End seq */
    BYTE data[D11_QUERY_BYTES];
} D11_QUERY;

static SIZE_T APIENTRY calc_query(D3D10DDI_HDEVICE h, const D3D10DDIARG_CREATEQUERY *c) { D11_TRACE("calc_query"); (void)h; (void)c; return sizeof(D11_QUERY); }

/* the DDI's query to the API's: Direct3D 10's pipeline statistics are
 * the API's (a smaller result), Direct3D 11's per-stream ones follow */
static UINT query_api(UINT q)
{
    if (q <= D3D10DDI_QUERY_STREAMOVERFLOWPREDICATE) return q;
    if (q == D3D11DDI_QUERY_PIPELINESTATS) return 4;
    if (q >= D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM0 && q <= D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM3) {
        return 8 + (q - D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM0);
    }
    return ~0u;
}

static void APIENTRY create_query(D3D10DDI_HDEVICE h, const D3D10DDIARG_CREATEQUERY *c, D3D10DDI_HQUERY hq, D3D10DDI_HRTQUERY rt)
{ D11_TRACE("create_query");
    D11_DEV *d = DEV(h);
    D11_QUERY *q = (D11_QUERY *)hq.pDrvPrivate;
    d3dpt_d11_query *t;

    (void)rt;
    ZeroMemory(q, sizeof(*q));                    /* the runtime's storage comes as it was left */
    new_obj(d, &q->o, D3DPT_D11K_QUERY);
    q->type = query_api((UINT)c->Query);
    if (q->type == ~0u) {
        umd_log("d3d11: CreateQuery %u (a counter): refused", c->Query);
        set_error(d, E_INVALIDARG);
        return;
    }
    if (g_d11_verbose) {
        umd_log("d3d11: CreateQuery %u (API %u) misc 0x%x -> id %u", c->Query, q->type, c->MiscFlags, q->o.id);
    }
    t = (d3dpt_d11_query *)tok(d, D3DPT_D11_QUERY_CREATE, sizeof(*t), 0);
    if (t) {
        t->id = q->o.id;
        t->type = q->type;
        t->misc = c->MiscFlags;
    }
}

static void APIENTRY destroy_query(D3D10DDI_HDEVICE h, D3D10DDI_HQUERY hq)
{ D11_TRACE("destroy_query");
    D11_DEV *d = DEV(h);
    D11_QUERY *q = (D11_QUERY *)hq.pDrvPrivate;

    destroy_obj(d, &q->o);
    if (q->res) {
        D3DDDICB_DEALLOCATE da;

        cmd_flush(d);                             /* nothing queued may name its page once it is gone */
        ZeroMemory(&da, sizeof(da));
        da.NumAllocations = 1;
        da.HandleList = &q->res->kmt;
        d->kt->pfnDeallocateCb(d->rt.handle, &da);
        HeapFree(GetProcessHeap(), 0, q->res);
        q->res = NULL;
    }
}

static void query_tok(D11_DEV *d, UINT op, D3D10DDI_HQUERY q)
{
    UINT *t = (UINT *)tok(d, op, 4, 0);

    if (t) {
        t[0] = OBJ(q)->id;
    }
}

static void APIENTRY query_begin(D3D10DDI_HDEVICE h, D3D10DDI_HQUERY q) { D11_TRACE("query_begin"); query_tok(DEV(h), D3DPT_D11_QUERY_BEGIN, q); }
static void APIENTRY query_end(D3D10DDI_HDEVICE h, D3D10DDI_HQUERY q)
{ D11_TRACE("query_end");
    D11_QUERY *x = (D11_QUERY *)q.pDrvPrivate;

    x->seq++;                                     /* an answer fetched before is no longer the one */
    x->have = FALSE;
    query_tok(DEV(h), D3DPT_D11_QUERY_END, q);
}

#define D11_RESULT_BYTES 4096

/* a result page: the kernel driver copies a record's result into it
 * (D3DPT_UMD_OP_RETURN), the lock reads it */
static D11_RES *result_res(D11_DEV *d)
{
    D3DDDICB_ALLOCATE al;
    D3DDDI_ALLOCATIONINFO ai;
    D11_RES *r;

    r = (D11_RES *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->a.magic = D3DPT_ALLOC_MAGIC;
    r->a.kind = D3DPT_ALLOC_D3D;
    r->a.size = r->a.w = r->a.pitch = D11_RESULT_BYTES;
    r->a.h = 1;
    r->a.caps = 0;                                /* no host handle: the kernel driver writes it */
    ZeroMemory(&al, sizeof(al));
    ZeroMemory(&ai, sizeof(ai));
    ai.pPrivateDriverData = &r->a;
    ai.PrivateDriverDataSize = sizeof(r->a);
    al.NumAllocations = 1;
    al.pAllocationInfo = &ai;
    if (FAILED(d->kt->pfnAllocateCb(d->rt.handle, &al))) {
        HeapFree(GetProcessHeap(), 0, r);
        return NULL;
    }
    r->kmt = ai.hAllocation;
    return r;
}

/* the fetch: a RETURN record, then the GET token alone in its D11 record
 * (the kernel driver runs it by itself and copies its answer to the page) */
static BOOL query_fetch(D11_DEV *d, D11_QUERY *q)
{
    d3dpt_u32x2 *ret;
    d3dpt_d11_ref *t;
    D3DDDI_PATCHLOCATIONLIST *pl;
    UINT i;

    /* the RETURN and its record in one buffer, which need not be a
     * fresh one (a flush per fetch was half of Heaven's submissions) */
    cmd_reserve(d, D3DPT_ALIGN8((UINT)sizeof(d3dpt_cmd) + (UINT)sizeof(*ret)) + (UINT)sizeof(d3dpt_cmd) + (UINT)sizeof(d3dpt_d11) +
                       (UINT)sizeof(d3dpt_d11_tok) + (UINT)sizeof(*t) + 16, 1);
    ret = (d3dpt_u32x2 *)cmd_rec(d, D3DPT_UMD_OP_RETURN, sizeof(*ret), 1);
    /* the result page's VRAM offset, as the RETURN record wants it */
    i = al_slot(d, q->res->kmt);
    d->al[i].WriteOperation = 1;
    pl = &d->pl[d->pl_n++];
    ZeroMemory(pl, sizeof(*pl));
    pl->AllocationIndex = i;
    pl->DriverId = D3DPT_PATCH_OFFSET;
    pl->PatchOffset = (UINT)((BYTE *)&ret->a - d->buf);
    ret->b = D11_QUERY_BYTES;
    t = (d3dpt_d11_ref *)tok(d, D3DPT_D11_QUERY_GET, sizeof(*t), 0);
    if (!t) {
        return FALSE;
    }
    t->a = q->o.id;
    t->b = D11_QUERY_BYTES;
    rec_close(d);
    q->pending = TRUE;
    q->get_seq = q->seq;
    return TRUE;
}

/* GetData, never waiting (S_FALSE: not yet, the runtime's
 * WASSTILLDRAWING). It waited before, for the submission that fetched the
 * answer and so for everything queued ahead of it: Unigine Heaven reads
 * two pipeline-statistics queries ~3 times a frame, and each read drained
 * the queue, the game and the GPU taking turns (track M25). Now a fetch
 * goes out, later calls look whether its submission has run (a lock that
 * does not wait), and the answer is kept until the next End. */
static void APIENTRY query_get(D3D10DDI_HDEVICE h, D3D10DDI_HQUERY hq, VOID *data, UINT size, UINT flags)
{ D11_TRACE("query_get");
    D11_DEV *d = DEV(h);
    D11_QUERY *q = (D11_QUERY *)hq.pDrvPrivate;
    HRESULT hr = S_FALSE;
    ULONG64 t0 = D11_PROF_T0();

    if (size > D11_QUERY_BYTES) {
        set_error(d, E_INVALIDARG);
        return;
    }
    if (!q->res) {
        q->res = result_res(d);
        if (!q->res) {
            set_error(d, E_OUTOFMEMORY);
            return;
        }
    }
    if (q->pending) {
        if (al_find(d, q->res->kmt) < d->al_n) {
            /* the fetch is still in the unsubmitted buffer (a DONOTFLUSH
             * call queued it): a lock now would read the page as it was */
            if (!(flags & D3D10_DDI_GET_DATA_DO_NOT_FLUSH)) {
                cmd_flush(d);
            }
        } else {
            HRESULT lk;
            BYTE *mem = res_lock(d, q->res, TRUE, &lk);

            if (mem) {
                const d3dpt_ret *r = (const d3dpt_ret *)mem;
                HRESULT got = (HRESULT)r->hr;

                if (got == S_OK && q->get_seq == q->seq) {
                    CopyMemory(q->data, r + 1, D11_QUERY_BYTES);
                    q->have = TRUE;
                } else if (FAILED(got)) {
                    static ULONG logged;

                    if (logged++ < 8) {
                        umd_log("d3d11: a query's GetData: the host answered 0x%08x", got);
                    }
                }
                res_unlock(d, q->res);
                q->pending = FALSE;
            } else if (lk != D3DDDIERR_WASSTILLDRAWING) {
                q->pending = FALSE;               /* fetched again below */
            }
        }
    }
    if (q->have) {
        hr = S_OK;
        if (data && size) {
            CopyMemory(data, q->data, size);
        }
    } else if (!q->pending) {
        /* not answered yet (or the host said not yet): ask, in the buffer
         * as it is; the frame's own flush sends it, or the next poll does
         * (above), so a caller that spins still gets its answer */
        query_fetch(d, q);
    }
    if (g_d11_verbose) {
        umd_log("d3d11: GetData id %u type %u size %u flags 0x%x -> 0x%08x%s", q->o.id, q->type, size, flags, hr,
                q->pending ? " (fetching)" : "");
    }
    D11_PROF_ADD(D11P_QUERY, t0, 0);
    if (hr != S_OK) {
        set_error(d, DXGI_DDI_ERR_WASSTILLDRAWING_);
    }
}

static void APIENTRY set_predication(D3D10DDI_HDEVICE h, D3D10DDI_HQUERY q, BOOL value)
{ D11_TRACE("set_predication");
    D11_DEV *d = DEV(h);
    d3dpt_d11_ref *t = (d3dpt_d11_ref *)tok(d, D3DPT_D11_SET_PREDICATION, sizeof(*t), 0);

    if (t) {
        t->a = obj_id(q.pDrvPrivate);
        t->b = value;
    }
}

void d11_pipe_destroy(D11_DEV *d)
{
    (void)d;                                      /* each query frees its own result page */
}

/* ------------------------------------------------------------- bindings */

static void set_shader(D11_DEV *d, UINT stage, D3D10DDI_HSHADER s)
{
    d3dpt_d11_ref *t = (d3dpt_d11_ref *)tok(d, D3DPT_D11_SET_SHADER, sizeof(*t), 0);

    if (t) {
        t->a = stage;
        t->b = obj_id(s.pDrvPrivate);
    }
}

static void APIENTRY vs_set_shader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s) { D11_TRACE("vs_set_shader"); set_shader(DEV(h), D3DPT_D11S_VS, s); }
static void APIENTRY ps_set_shader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s) { D11_TRACE("ps_set_shader"); set_shader(DEV(h), D3DPT_D11S_PS, s); }
static void APIENTRY gs_set_shader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s) { D11_TRACE("gs_set_shader"); set_shader(DEV(h), D3DPT_D11S_GS, s); }
static void APIENTRY hs_set_shader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s) { D11_TRACE("hs_set_shader"); set_shader(DEV(h), D3DPT_D11S_HS, s); }
static void APIENTRY ds_set_shader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s) { D11_TRACE("ds_set_shader"); set_shader(DEV(h), D3DPT_D11S_DS, s); }
static void APIENTRY cs_set_shader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s) { D11_TRACE("cs_set_shader"); set_shader(DEV(h), D3DPT_D11S_CS, s); }

/* class linkage: none at these levels; the shader alone */
static void APIENTRY ps_set_shader_if(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s, UINT n, const UINT *i, const D3D11DDIARG_POINTERDATA *p) { D11_TRACE("ps_set_shader_if"); (void)n; (void)i; (void)p; set_shader(DEV(h), D3DPT_D11S_PS, s); }
static void APIENTRY vs_set_shader_if(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s, UINT n, const UINT *i, const D3D11DDIARG_POINTERDATA *p) { D11_TRACE("vs_set_shader_if"); (void)n; (void)i; (void)p; set_shader(DEV(h), D3DPT_D11S_VS, s); }
static void APIENTRY gs_set_shader_if(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s, UINT n, const UINT *i, const D3D11DDIARG_POINTERDATA *p) { D11_TRACE("gs_set_shader_if"); (void)n; (void)i; (void)p; set_shader(DEV(h), D3DPT_D11S_GS, s); }
static void APIENTRY hs_set_shader_if(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s, UINT n, const UINT *i, const D3D11DDIARG_POINTERDATA *p) { D11_TRACE("hs_set_shader_if"); (void)n; (void)i; (void)p; set_shader(DEV(h), D3DPT_D11S_HS, s); }
static void APIENTRY ds_set_shader_if(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s, UINT n, const UINT *i, const D3D11DDIARG_POINTERDATA *p) { D11_TRACE("ds_set_shader_if"); (void)n; (void)i; (void)p; set_shader(DEV(h), D3DPT_D11S_DS, s); }
static void APIENTRY cs_set_shader_if(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s, UINT n, const UINT *i, const D3D11DDIARG_POINTERDATA *p) { D11_TRACE("cs_set_shader_if"); (void)n; (void)i; (void)p; set_shader(DEV(h), D3DPT_D11S_CS, s); }

static void set_cb(D11_DEV *d, UINT stage, UINT start, UINT n, const D3D10DDI_HRESOURCE *b)
{
    d3dpt_d11_bind *t = (d3dpt_d11_bind *)tok(d, D3DPT_D11_SET_CB, sizeof(*t) + n * 4, n);
    UINT i, *ids;

    if (!t) {
        return;
    }
    t->stage = stage;
    t->start = start;
    t->n = n;
    ids = (UINT *)(t + 1);
    for (i = 0; i < n; i++) {
        patch_res(d, &ids[i], RES(b[i]));
    }
}

static void APIENTRY vs_set_cb(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HRESOURCE *b) { D11_TRACE("vs_set_cb"); set_cb(DEV(h), D3DPT_D11S_VS, s, n, b); }
static void APIENTRY ps_set_cb(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HRESOURCE *b) { D11_TRACE("ps_set_cb"); set_cb(DEV(h), D3DPT_D11S_PS, s, n, b); }
static void APIENTRY gs_set_cb(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HRESOURCE *b) { D11_TRACE("gs_set_cb"); set_cb(DEV(h), D3DPT_D11S_GS, s, n, b); }
static void APIENTRY hs_set_cb(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HRESOURCE *b) { D11_TRACE("hs_set_cb"); set_cb(DEV(h), D3DPT_D11S_HS, s, n, b); }
static void APIENTRY ds_set_cb(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HRESOURCE *b) { D11_TRACE("ds_set_cb"); set_cb(DEV(h), D3DPT_D11S_DS, s, n, b); }
static void APIENTRY cs_set_cb(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HRESOURCE *b) { D11_TRACE("cs_set_cb"); set_cb(DEV(h), D3DPT_D11S_CS, s, n, b); }

/* SRVs and samplers: the ids of objects, n of them from start */
static void set_ids(D11_DEV *d, UINT op, UINT stage, UINT start, UINT n, const void *handles)
{
    d3dpt_d11_bind *t = (d3dpt_d11_bind *)tok(d, op, sizeof(*t) + n * 4, 0);
    const D3D10DDI_HSAMPLER *hs = (const D3D10DDI_HSAMPLER *)handles;   /* every handle is one pointer */
    UINT i, *ids;

    if (!t) {
        return;
    }
    t->stage = stage;
    t->start = start;
    t->n = n;
    ids = (UINT *)(t + 1);
    for (i = 0; i < n; i++) {
        ids[i] = obj_id(hs[i].pDrvPrivate);
    }
}

static void APIENTRY vs_set_srv(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HSHADERRESOURCEVIEW *v) { D11_TRACE("vs_set_srv"); set_ids(DEV(h), D3DPT_D11_SET_SRV, D3DPT_D11S_VS, s, n, v); }
static void APIENTRY ps_set_srv(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HSHADERRESOURCEVIEW *v) { D11_TRACE("ps_set_srv"); set_ids(DEV(h), D3DPT_D11_SET_SRV, D3DPT_D11S_PS, s, n, v); }
static void APIENTRY gs_set_srv(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HSHADERRESOURCEVIEW *v) { D11_TRACE("gs_set_srv"); set_ids(DEV(h), D3DPT_D11_SET_SRV, D3DPT_D11S_GS, s, n, v); }
static void APIENTRY hs_set_srv(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HSHADERRESOURCEVIEW *v) { D11_TRACE("hs_set_srv"); set_ids(DEV(h), D3DPT_D11_SET_SRV, D3DPT_D11S_HS, s, n, v); }
static void APIENTRY ds_set_srv(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HSHADERRESOURCEVIEW *v) { D11_TRACE("ds_set_srv"); set_ids(DEV(h), D3DPT_D11_SET_SRV, D3DPT_D11S_DS, s, n, v); }
static void APIENTRY cs_set_srv(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HSHADERRESOURCEVIEW *v) { D11_TRACE("cs_set_srv"); set_ids(DEV(h), D3DPT_D11_SET_SRV, D3DPT_D11S_CS, s, n, v); }

static void APIENTRY vs_set_samp(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HSAMPLER *v) { D11_TRACE("vs_set_samp"); set_ids(DEV(h), D3DPT_D11_SET_SAMPLERS, D3DPT_D11S_VS, s, n, v); }
static void APIENTRY ps_set_samp(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HSAMPLER *v) { D11_TRACE("ps_set_samp"); set_ids(DEV(h), D3DPT_D11_SET_SAMPLERS, D3DPT_D11S_PS, s, n, v); }
static void APIENTRY gs_set_samp(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HSAMPLER *v) { D11_TRACE("gs_set_samp"); set_ids(DEV(h), D3DPT_D11_SET_SAMPLERS, D3DPT_D11S_GS, s, n, v); }
static void APIENTRY hs_set_samp(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HSAMPLER *v) { D11_TRACE("hs_set_samp"); set_ids(DEV(h), D3DPT_D11_SET_SAMPLERS, D3DPT_D11S_HS, s, n, v); }
static void APIENTRY ds_set_samp(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HSAMPLER *v) { D11_TRACE("ds_set_samp"); set_ids(DEV(h), D3DPT_D11_SET_SAMPLERS, D3DPT_D11S_DS, s, n, v); }
static void APIENTRY cs_set_samp(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HSAMPLER *v) { D11_TRACE("cs_set_samp"); set_ids(DEV(h), D3DPT_D11_SET_SAMPLERS, D3DPT_D11S_CS, s, n, v); }

static void APIENTRY ia_set_layout(D3D10DDI_HDEVICE h, D3D10DDI_HELEMENTLAYOUT l)
{ D11_TRACE("ia_set_layout");
    UINT *t = (UINT *)tok(DEV(h), D3DPT_D11_SET_LAYOUT, 4, 0);

    if (t) {
        t[0] = obj_id(l.pDrvPrivate);
    }
}

static void APIENTRY ia_set_vb(D3D10DDI_HDEVICE h, UINT start, UINT n, const D3D10DDI_HRESOURCE *b, const UINT *strides, const UINT *offsets)
{ D11_TRACE("ia_set_vb");
    D11_DEV *d = DEV(h);
    d3dpt_d11_bind *t = (d3dpt_d11_bind *)tok(d, D3DPT_D11_SET_VB, sizeof(*t) + n * sizeof(d3dpt_d11_vb), n);
    d3dpt_d11_vb *v;
    UINT i;

    if (!t) {
        return;
    }
    t->start = start;
    t->n = n;
    v = (d3dpt_d11_vb *)(t + 1);
    for (i = 0; i < n; i++) {
        patch_res(d, &v[i].res, RES(b[i]));
        v[i].stride = strides[i];
        v[i].offset = offsets[i];
    }
}

static void APIENTRY ia_set_ib(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE b, DXGI_FORMAT fmt, UINT offset)
{ D11_TRACE("ia_set_ib");
    D11_DEV *d = DEV(h);
    d3dpt_d11_vb *t = (d3dpt_d11_vb *)tok(d, D3DPT_D11_SET_IB, sizeof(*t), 1);

    if (t) {
        patch_res(d, &t->res, RES(b));
        t->stride = (UINT)fmt;
        t->offset = offset;
    }
}

static void APIENTRY ia_set_topology(D3D10DDI_HDEVICE h, D3D10_DDI_PRIMITIVE_TOPOLOGY p)
{ D11_TRACE("ia_set_topology");
    UINT *t = (UINT *)tok(DEV(h), D3DPT_D11_SET_TOPOLOGY, 4, 0);

    if (t) {
        t[0] = (UINT)p;
    }
}

static void APIENTRY set_targets(D3D10DDI_HDEVICE h, const D3D10DDI_HRENDERTARGETVIEW *rtv, UINT n, UINT clear, D3D10DDI_HDEPTHSTENCILVIEW dsv,
                                 const D3D11DDI_HUNORDEREDACCESSVIEW *uav, const UINT *counts, UINT uav_start, UINT nuav,
                                 UINT uav_first, UINT uav_size)
{ D11_TRACE("set_targets");
    D11_DEV *d = DEV(h);
    d3dpt_d11_list *t;
    UINT i, *ids;

    (void)clear;
    if (nuav || uav_size || d->om_uav_any) {
        /* v34: the slots the runtime names (UAVRangeStart / Size) are set
         * from its array (UAVStartSlot on) or cleared; the host's API takes
         * every bound slot in one call, so the shadow's whole bound range
         * goes, with the initial counts the runtime gave for its own
         * (~0, kept, for the others) */
        UINT first = 64, last = 0, newc[64], any = 0;
        d3dpt_d11_targets *u;
        UINT *rids, *uids, *cnt;

        for (i = 0; i < 64; i++) {
            newc[i] = ~0u;
        }
        for (i = uav_first; i < uav_first + uav_size && i < 64; i++) {
            d->om_uav[i] = 0;
        }
        for (i = 0; i < nuav && uav_start + i < 64; i++) {
            d->om_uav[uav_start + i] = obj_id(uav[i].pDrvPrivate);
            newc[uav_start + i] = counts ? counts[i] : ~0u;
        }
        for (i = 0; i < 64; i++) {
            if (d->om_uav[i]) {
                first = first < i ? first : i;
                last = i + 1;
                any = 1;
            }
        }
        d->om_uav_any = any;
        if (!any) {
            first = last = n;                       /* none: every slot unbound */
        }
        if (first < n) {
            first = n;                              /* the API's rule: UAVs after the targets */
        }
        if (last < first) {
            last = first;
        }
        u = (d3dpt_d11_targets *)tok(d, D3DPT_D11_SET_TARGETS_UAV, sizeof(*u) + n * 4 + (last - first) * 8, 0);
        if (!u) {
            return;
        }
        u->dsv = obj_id(dsv.pDrvPrivate);
        u->nrtv = n;
        u->uav_start = first;
        u->nuav = last - first;
        rids = (UINT *)(u + 1);
        uids = rids + n;
        cnt = uids + (last - first);
        for (i = 0; i < n; i++) {
            rids[i] = obj_id(rtv[i].pDrvPrivate);
        }
        for (i = first; i < last; i++) {
            uids[i - first] = d->om_uav[i];
            cnt[i - first] = newc[i];
        }
        return;
    }
    t = (d3dpt_d11_list *)tok(d, D3DPT_D11_SET_TARGETS, sizeof(*t) + n * 4, 0);
    if (!t) {
        return;
    }
    t->id = obj_id(dsv.pDrvPrivate);
    t->n = n;
    ids = (UINT *)(t + 1);
    for (i = 0; i < n; i++) {
        ids[i] = obj_id(rtv[i].pDrvPrivate);
    }
}

static void APIENTRY set_blend(D3D10DDI_HDEVICE h, D3D10DDI_HBLENDSTATE s, const FLOAT factor[4], UINT mask)
{ D11_TRACE("set_blend");
    d3dpt_d11_set_blend *t = (d3dpt_d11_set_blend *)tok(DEV(h), D3DPT_D11_SET_BLEND, sizeof(*t), 0);

    if (t) {
        t->id = obj_id(s.pDrvPrivate);
        t->mask = mask;
        CopyMemory(t->factor, factor, sizeof(t->factor));
    }
}

static void APIENTRY set_depth(D3D10DDI_HDEVICE h, D3D10DDI_HDEPTHSTENCILSTATE s, UINT ref)
{ D11_TRACE("set_depth");
    d3dpt_d11_ref *t = (d3dpt_d11_ref *)tok(DEV(h), D3DPT_D11_SET_DEPTH, sizeof(*t), 0);

    if (t) {
        t->a = obj_id(s.pDrvPrivate);
        t->b = ref;
    }
}

static void APIENTRY set_raster(D3D10DDI_HDEVICE h, D3D10DDI_HRASTERIZERSTATE s)
{ D11_TRACE("set_raster");
    UINT *t = (UINT *)tok(DEV(h), D3DPT_D11_SET_RASTER, 4, 0);

    if (t) {
        t[0] = obj_id(s.pDrvPrivate);
    }
}

static void APIENTRY set_viewports(D3D10DDI_HDEVICE h, UINT n, UINT clear, const D3D10_DDI_VIEWPORT *v)
{ D11_TRACE("set_viewports");
    UINT *t = (UINT *)tok(DEV(h), D3DPT_D11_SET_VIEWPORTS, 4 + n * sizeof(*v), 0);

    (void)clear;
    if (t) {
        t[0] = n;
        CopyMemory(t + 1, v, n * sizeof(*v));
    }
}

static void APIENTRY set_scissors(D3D10DDI_HDEVICE h, UINT n, UINT clear, const D3D10_DDI_RECT *r)
{ D11_TRACE("set_scissors");
    UINT *t = (UINT *)tok(DEV(h), D3DPT_D11_SET_SCISSORS, 4 + n * 16, 0);

    (void)clear;
    if (t) {
        t[0] = n;
        CopyMemory(t + 1, r, n * 16);
    }
}

static void APIENTRY so_set_targets(D3D10DDI_HDEVICE h, UINT n, UINT clear, const D3D10DDI_HRESOURCE *b, const UINT *offsets)
{ D11_TRACE("so_set_targets");
    D11_DEV *d = DEV(h);
    d3dpt_d11_bind *t = (d3dpt_d11_bind *)tok(d, D3DPT_D11_SET_SO, sizeof(*t) + n * sizeof(d3dpt_d11_ref), n);
    d3dpt_d11_ref *e;
    UINT i;

    (void)clear;
    if (!t) {
        return;
    }
    t->n = n;
    e = (d3dpt_d11_ref *)(t + 1);
    for (i = 0; i < n; i++) {
        patch_res(d, &e[i].a, RES(b[i]));
        e[i].b = offsets ? offsets[i] : 0;
    }
}

/* ----------------------------------------------------------------- work */

static void draw_tok(D11_DEV *d, UINT op, UINT n, const UINT *v)
{
    UINT *t = (UINT *)tok(d, op, n * 4, 0);

    if (t) {
        CopyMemory(t, v, n * 4);
    }
}

static void APIENTRY draw(D3D10DDI_HDEVICE h, UINT count, UINT first)
{ D11_TRACE("draw");
    UINT v[2] = { count, first };

    draw_tok(DEV(h), D3DPT_D11_DRAW, 2, v);
}

static void APIENTRY draw_indexed(D3D10DDI_HDEVICE h, UINT count, UINT first, INT base)
{ D11_TRACE("draw_indexed");
    UINT v[3] = { count, first, (UINT)base };

    draw_tok(DEV(h), D3DPT_D11_DRAW_INDEXED, 3, v);
}

static void APIENTRY draw_instanced(D3D10DDI_HDEVICE h, UINT count, UINT inst, UINT first, UINT first_inst)
{ D11_TRACE("draw_instanced");
    UINT v[4] = { count, inst, first, first_inst };

    draw_tok(DEV(h), D3DPT_D11_DRAW_INSTANCED, 4, v);
}

static void APIENTRY draw_indexed_instanced(D3D10DDI_HDEVICE h, UINT count, UINT inst, UINT first, INT base, UINT first_inst)
{ D11_TRACE("draw_indexed_instanced");
    UINT v[5] = { count, inst, first, (UINT)base, first_inst };

    draw_tok(DEV(h), D3DPT_D11_DRAW_INDEXED_INSTANCED, 5, v);
}

static void APIENTRY draw_auto(D3D10DDI_HDEVICE h)
{ D11_TRACE("draw_auto");
    tok(DEV(h), D3DPT_D11_DRAW_AUTO, 0, 0);
}

static void APIENTRY clear_rtv(D3D10DDI_HDEVICE h, D3D10DDI_HRENDERTARGETVIEW v, FLOAT c[4])
{ D11_TRACE("clear_rtv");
    UINT *t = (UINT *)tok(DEV(h), D3DPT_D11_CLEAR_RTV, 20, 0);

    if (t) {
        t[0] = obj_id(v.pDrvPrivate);
        CopyMemory(t + 1, c, 16);
    }
}

static void APIENTRY clear_dsv(D3D10DDI_HDEVICE h, D3D10DDI_HDEPTHSTENCILVIEW v, UINT flags, FLOAT depth, UINT8 stencil)
{ D11_TRACE("clear_dsv");
    UINT *t = (UINT *)tok(DEV(h), D3DPT_D11_CLEAR_DSV, 16, 0);

    if (t) {
        t[0] = obj_id(v.pDrvPrivate);
        t[1] = flags;
        CopyMemory(&t[2], &depth, 4);
        t[3] = stencil;
    }
}

/* ------------------------------------- the compute pipeline, unordered access
 * (feature level 11_0, protocol v34) */

static SIZE_T APIENTRY calc_uav(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *c) { D11_TRACE("calc_uav"); (void)h; (void)c; return obj_size(); }

/* the API's UAV dimension and union: a buffer's first element, count and
 * flags (the DDI's flags are the API's), a texture's mip and slices */
static void APIENTRY create_uav(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *c, D3D11DDI_HUNORDEREDACCESSVIEW hv,
                                D3D11DDI_HRTUNORDEREDACCESSVIEW rt)
{ D11_TRACE("create_uav");
    D11_DEV *d = DEV(h);
    D11_OBJ *o = OBJ(hv);
    D11_RES *r = RES(c->hDrvResource);
    d3dpt_d11_view *t;

    (void)rt;
    new_obj(d, o, D3DPT_D11K_UAV);
    o->res = r;
    t = (d3dpt_d11_view *)tok(d, D3DPT_D11_UAV_CREATE, sizeof(*t), 1);
    if (!t) {
        return;
    }
    t->id = o->id;
    patch_res(d, &t->res, r);
    t->format = (UINT)c->Format;
    switch (c->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
    case D3D11DDIRESOURCE_BUFFEREX:
        t->dim = 1;                                                  /* BUFFER */
        t->u[0] = c->Buffer.FirstElement;
        t->u[1] = c->Buffer.NumElements;
        t->u[2] = c->Buffer.Flags;
        break;
    case D3D10DDIRESOURCE_TEXTURE1D:
        t->u[0] = c->Tex1D.MipSlice;
        if (c->Tex1D.ArraySize > 1 || c->Tex1D.FirstArraySlice || r->r.array > 1) {
            t->dim = 3;                                              /* TEXTURE1DARRAY */
            t->u[1] = c->Tex1D.FirstArraySlice;
            t->u[2] = c->Tex1D.ArraySize;
        } else {
            t->dim = 2;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        t->u[0] = c->Tex2D.MipSlice;
        if (c->Tex2D.ArraySize > 1 || c->Tex2D.FirstArraySlice || r->r.array > 1) {
            t->dim = 5;                                              /* TEXTURE2DARRAY */
            t->u[1] = c->Tex2D.FirstArraySlice;
            t->u[2] = c->Tex2D.ArraySize;
        } else {
            t->dim = 4;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE3D:
        t->dim = 8;                                                  /* TEXTURE3D */
        t->u[0] = c->Tex3D.MipSlice;
        t->u[1] = c->Tex3D.FirstW;
        t->u[2] = c->Tex3D.WSize;
        break;
    default:
        umd_log("d3d11: an unordered access view of dimension %u", c->ResourceDimension);
        break;
    }
    view_keep(d, o, D3DPT_D11_UAV_CREATE, t);
}

static void APIENTRY destroy_uav(D3D10DDI_HDEVICE h, D3D11DDI_HUNORDEREDACCESSVIEW v)
{ D11_TRACE("destroy_uav");
    D11_DEV *d = DEV(h);
    UINT i;

    for (i = 0; i < 64; i++) {
        if (OBJ(v)->id && d->om_uav[i] == OBJ(v)->id) {
            d->om_uav[i] = 0;
        }
    }
    view_drop(d, OBJ(v));
    destroy_obj(d, OBJ(v));
}

static void clear_uav(D11_DEV *d, D3D11DDI_HUNORDEREDACCESSVIEW v, const void *c, UINT is_float)
{
    d3dpt_d11_clear_uav *t = (d3dpt_d11_clear_uav *)tok(d, D3DPT_D11_CLEAR_UAV, sizeof(*t), 0);

    if (t) {
        t->id = obj_id(v.pDrvPrivate);
        t->is_float = is_float;
        CopyMemory(t->v, c, 16);
    }
}

static void APIENTRY clear_uav_uint(D3D10DDI_HDEVICE h, D3D11DDI_HUNORDEREDACCESSVIEW v, const UINT c[4]) { D11_TRACE("clear_uav_uint"); clear_uav(DEV(h), v, c, 0); }
static void APIENTRY clear_uav_float(D3D10DDI_HDEVICE h, D3D11DDI_HUNORDEREDACCESSVIEW v, const FLOAT c[4]) { D11_TRACE("clear_uav_float"); clear_uav(DEV(h), v, c, 1); }

/* the compute stage's UAVs and their initial counts (~0: kept) */
static void APIENTRY cs_set_uav(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D11DDI_HUNORDEREDACCESSVIEW *v, const UINT *c)
{ D11_TRACE("cs_set_uav");
    d3dpt_d11_bind *t = (d3dpt_d11_bind *)tok(DEV(h), D3DPT_D11_SET_UAV, sizeof(*t) + n * 8, 0);
    UINT i, *ids;

    if (!t) {
        return;
    }
    t->stage = D3DPT_D11S_CS;
    t->start = s;
    t->n = n;
    ids = (UINT *)(t + 1);
    for (i = 0; i < n; i++) {
        ids[i] = obj_id(v[i].pDrvPrivate);
        ids[n + i] = c ? c[i] : ~0u;
    }
}

static void APIENTRY dispatch(D3D10DDI_HDEVICE h, UINT x, UINT y, UINT z)
{ D11_TRACE("dispatch");
    UINT *t = (UINT *)tok(DEV(h), D3DPT_D11_DISPATCH, 12, 0);

    if (t) {
        t[0] = x;
        t[1] = y;
        t[2] = z;
    }
}

static void APIENTRY dispatch_indirect(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE b, UINT o)
{ D11_TRACE("dispatch_indirect");
    D11_DEV *d = DEV(h);
    d3dpt_d11_ref *t = (d3dpt_d11_ref *)tok(d, D3DPT_D11_DISPATCH_INDIRECT, sizeof(*t), 1);

    if (t) {
        patch_res(d, &t->a, RES(b));
        t->b = o;
    }
}

static void indirect(D11_DEV *d, D3D10DDI_HRESOURCE b, UINT o, UINT indexed)
{
    UINT *t = (UINT *)tok(d, D3DPT_D11_DRAW_INDIRECT, 12, 1);

    if (t) {
        patch_res(d, &t[0], RES(b));
        t[1] = o;
        t[2] = indexed;
    }
}

static void APIENTRY draw_indirect(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE b, UINT offset) { D11_TRACE("draw_indirect"); indirect(DEV(h), b, offset, 0); }
static void APIENTRY draw_indexed_indirect(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE b, UINT offset) { D11_TRACE("draw_indexed_indirect"); indirect(DEV(h), b, offset, 1); }

static void APIENTRY copy_structure_count(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE b, UINT o, D3D11DDI_HUNORDEREDACCESSVIEW v)
{ D11_TRACE("copy_structure_count");
    D11_DEV *d = DEV(h);
    UINT *t = (UINT *)tok(d, D3DPT_D11_COPY_STRUCTURE_COUNT, 12, 1);

    if (t) {
        patch_res(d, &t[0], RES(b));
        t[1] = o;
        t[2] = obj_id(v.pDrvPrivate);
    }
}

void d11_pipe_funcs(D3D11DDI_DEVICEFUNCS *f)
{
    f->pfnCalcPrivateShaderResourceViewSize = calc_srv;
    f->pfnCreateShaderResourceView = create_srv;
    f->pfnDestroyShaderResourceView = destroy_srv;
    f->pfnCalcPrivateRenderTargetViewSize = calc_rtv;
    f->pfnCreateRenderTargetView = create_rtv;
    f->pfnDestroyRenderTargetView = destroy_rtv;
    f->pfnCalcPrivateDepthStencilViewSize = calc_dsv;
    f->pfnCreateDepthStencilView = create_dsv;
    f->pfnDestroyDepthStencilView = destroy_dsv;
    f->pfnGenMips = gen_mips;

    f->pfnCalcPrivateBlendStateSize = calc_blend;
    f->pfnCreateBlendState = create_blend;
    f->pfnDestroyBlendState = destroy_blend;
    f->pfnCalcPrivateDepthStencilStateSize = calc_depth;
    f->pfnCreateDepthStencilState = create_depth;
    f->pfnDestroyDepthStencilState = destroy_depth;
    f->pfnCalcPrivateRasterizerStateSize = calc_raster;
    f->pfnCreateRasterizerState = create_raster;
    f->pfnDestroyRasterizerState = destroy_raster;
    f->pfnCalcPrivateSamplerSize = calc_sampler;
    f->pfnCreateSampler = create_sampler;
    f->pfnDestroySampler = destroy_sampler;

    f->pfnCalcPrivateShaderSize = calc_shader;
    f->pfnCreateVertexShader = create_vs;
    f->pfnCreatePixelShader = create_ps;
    f->pfnCreateGeometryShader = create_gs;
    f->pfnCalcPrivateGeometryShaderWithStreamOutput = calc_gs_so;
    f->pfnCreateGeometryShaderWithStreamOutput = create_gs_so;
    f->pfnCalcPrivateTessellationShaderSize = calc_tess;
    f->pfnCreateHullShader = create_hs;
    f->pfnCreateDomainShader = create_ds;
    f->pfnCreateComputeShader = create_cs;
    f->pfnDestroyShader = destroy_shader;
    f->pfnCalcPrivateElementLayoutSize = calc_layout;
    f->pfnCreateElementLayout = create_layout;
    f->pfnDestroyElementLayout = destroy_layout;

    f->pfnCalcPrivateQuerySize = calc_query;
    f->pfnCreateQuery = create_query;
    f->pfnDestroyQuery = destroy_query;
    f->pfnQueryBegin = query_begin;
    f->pfnQueryEnd = query_end;
    f->pfnQueryGetData = query_get;
    f->pfnSetPredication = set_predication;

    f->pfnVsSetShader = vs_set_shader;
    f->pfnPsSetShader = ps_set_shader;
    f->pfnGsSetShader = gs_set_shader;
    f->pfnHsSetShader = hs_set_shader;
    f->pfnDsSetShader = ds_set_shader;
    f->pfnCsSetShader = cs_set_shader;
    f->pfnPsSetShaderWithIfaces = ps_set_shader_if;
    f->pfnVsSetShaderWithIfaces = vs_set_shader_if;
    f->pfnGsSetShaderWithIfaces = gs_set_shader_if;
    f->pfnHsSetShaderWithIfaces = hs_set_shader_if;
    f->pfnDsSetShaderWithIfaces = ds_set_shader_if;
    f->pfnCsSetShaderWithIfaces = cs_set_shader_if;
    f->pfnVsSetConstantBuffers = vs_set_cb;
    f->pfnPsSetConstantBuffers = ps_set_cb;
    f->pfnGsSetConstantBuffers = gs_set_cb;
    f->pfnHsSetConstantBuffers = hs_set_cb;
    f->pfnDsSetConstantBuffers = ds_set_cb;
    f->pfnCsSetConstantBuffers = cs_set_cb;
    f->pfnVsSetShaderResources = vs_set_srv;
    f->pfnPsSetShaderResources = ps_set_srv;
    f->pfnGsSetShaderResources = gs_set_srv;
    f->pfnHsSetShaderResources = hs_set_srv;
    f->pfnDsSetShaderResources = ds_set_srv;
    f->pfnCsSetShaderResources = cs_set_srv;
    f->pfnVsSetSamplers = vs_set_samp;
    f->pfnPsSetSamplers = ps_set_samp;
    f->pfnGsSetSamplers = gs_set_samp;
    f->pfnHsSetSamplers = hs_set_samp;
    f->pfnDsSetSamplers = ds_set_samp;
    f->pfnCsSetSamplers = cs_set_samp;
    f->pfnIaSetInputLayout = ia_set_layout;
    f->pfnIaSetVertexBuffers = ia_set_vb;
    f->pfnIaSetIndexBuffer = ia_set_ib;
    f->pfnIaSetTopology = ia_set_topology;
    f->pfnSetRenderTargets = set_targets;
    f->pfnSetBlendState = set_blend;
    f->pfnSetDepthStencilState = set_depth;
    f->pfnSetRasterizerState = set_raster;
    f->pfnSetViewports = set_viewports;
    f->pfnSetScissorRects = set_scissors;
    f->pfnSoSetTargets = so_set_targets;

    f->pfnDraw = draw;
    f->pfnDrawIndexed = draw_indexed;
    f->pfnDrawInstanced = draw_instanced;
    f->pfnDrawIndexedInstanced = draw_indexed_instanced;
    f->pfnDrawAuto = draw_auto;
    f->pfnDrawInstancedIndirect = draw_indirect;
    f->pfnDrawIndexedInstancedIndirect = draw_indexed_indirect;
    f->pfnClearRenderTargetView = clear_rtv;
    f->pfnClearDepthStencilView = clear_dsv;

    f->pfnCalcPrivateUnorderedAccessViewSize = calc_uav;
    f->pfnCreateUnorderedAccessView = create_uav;
    f->pfnDestroyUnorderedAccessView = destroy_uav;
    f->pfnClearUnorderedAccessViewUint = clear_uav_uint;
    f->pfnClearUnorderedAccessViewFloat = clear_uav_float;
    f->pfnCsSetUnorderedAccessViews = cs_set_uav;
    f->pfnDispatch = dispatch;
    f->pfnDispatchIndirect = dispatch_indirect;
    f->pfnCopyStructureCount = copy_structure_count;
}

/* ---------------------------------------------------- the 11.1 interface */

static SIZE_T APIENTRY calc_blend1(D3D10DDI_HDEVICE h, const D3D11_1_DDI_BLEND_DESC *c) { D11_TRACE("calc_blend1"); (void)h; (void)c; return obj_size(); }
static SIZE_T APIENTRY calc_raster1(D3D10DDI_HDEVICE h, const D3D11_1_DDI_RASTERIZER_DESC *c) { D11_TRACE("calc_raster1"); (void)h; (void)c; return obj_size(); }

/* 11.1's blend state: a logic op per target (the options say none, so it
 * never comes enabled) */
static void APIENTRY create_blend1(D3D10DDI_HDEVICE h, const D3D11_1_DDI_BLEND_DESC *c, D3D10DDI_HBLENDSTATE hs, D3D10DDI_HRTBLENDSTATE rt)
{ D11_TRACE("create_blend1");
    D3D10_1_DDI_BLEND_DESC b;
    UINT i;

    ZeroMemory(&b, sizeof(b));
    b.AlphaToCoverageEnable = c->AlphaToCoverageEnable;
    b.IndependentBlendEnable = c->IndependentBlendEnable;
    for (i = 0; i < 8; i++) {
        const D3D11_1_DDI_RENDER_TARGET_BLEND_DESC *s = &c->RenderTarget[i];

        if (s->LogicOpEnable) {
            D11_TODO("a blend state's logic op");
        }
        b.RenderTarget[i].BlendEnable = s->BlendEnable;
        b.RenderTarget[i].SrcBlend = s->SrcBlend;
        b.RenderTarget[i].DestBlend = s->DestBlend;
        b.RenderTarget[i].BlendOp = s->BlendOp;
        b.RenderTarget[i].SrcBlendAlpha = s->SrcBlendAlpha;
        b.RenderTarget[i].DestBlendAlpha = s->DestBlendAlpha;
        b.RenderTarget[i].BlendOpAlpha = s->BlendOpAlpha;
        b.RenderTarget[i].RenderTargetWriteMask = s->RenderTargetWriteMask;
    }
    create_blend(h, &b, hs, rt);
}

/* 11.1's rasterizer state: a forced sample count (target-independent
 * rasterization, not at these levels) */
static void APIENTRY create_raster1(D3D10DDI_HDEVICE h, const D3D11_1_DDI_RASTERIZER_DESC *c, D3D10DDI_HRASTERIZERSTATE hs,
                                    D3D10DDI_HRTRASTERIZERSTATE rt)
{ D11_TRACE("create_raster1");
    D3D10_DDI_RASTERIZER_DESC r;

    if (c->ForcedSampleCount) {
        D11_TODO("a rasterizer state's forced sample count");
    }
    r.FillMode = c->FillMode;
    r.CullMode = c->CullMode;
    r.FrontCounterClockwise = c->FrontCounterClockwise;
    r.DepthBias = c->DepthBias;
    r.DepthBiasClamp = c->DepthBiasClamp;
    r.SlopeScaledDepthBias = c->SlopeScaledDepthBias;
    r.DepthClipEnable = c->DepthClipEnable;
    r.ScissorEnable = c->ScissorEnable;
    r.MultisampleEnable = c->MultisampleEnable;
    r.AntialiasedLineEnable = c->AntialiasedLineEnable;
    create_raster(h, &r, hs, rt);
}

static SIZE_T APIENTRY calc_shader1(D3D10DDI_HDEVICE h, const UINT *code, const D3D11_1DDIARG_STAGE_IO_SIGNATURES *s)
{ D11_TRACE("calc_shader1");
    (void)h; (void)code; (void)s;
    return obj_size();
}

static SIZE_T APIENTRY calc_gs_so1(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *c,
                                   const D3D11_1DDIARG_STAGE_IO_SIGNATURES *s)
{ D11_TRACE("calc_gs_so1");
    (void)h; (void)c; (void)s;
    return obj_size();
}

static SIZE_T APIENTRY calc_tess1(D3D10DDI_HDEVICE h, const UINT *code, const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *s)
{ D11_TRACE("calc_tess1");
    (void)h; (void)code; (void)s;
    return obj_size();
}

static void APIENTRY create_vs1(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER rt,
                                const D3D11_1DDIARG_STAGE_IO_SIGNATURES *s)
{ D11_TRACE("create_vs1");
    (void)rt;
    shader(DEV(h), OBJ(hs), D3DPT_D11S_VS, code, sigs11_1(s), NULL);
}

static void APIENTRY create_ps1(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER rt,
                                const D3D11_1DDIARG_STAGE_IO_SIGNATURES *s)
{ D11_TRACE("create_ps1");
    (void)rt;
    shader(DEV(h), OBJ(hs), D3DPT_D11S_PS, code, sigs11_1(s), NULL);
}

static void APIENTRY create_gs1(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER rt,
                                const D3D11_1DDIARG_STAGE_IO_SIGNATURES *s)
{ D11_TRACE("create_gs1");
    (void)rt;
    shader(DEV(h), OBJ(hs), D3DPT_D11S_GS, code, sigs11_1(s), NULL);
}

static void APIENTRY create_gs_so1(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *c, D3D10DDI_HSHADER hs,
                                   D3D10DDI_HRTSHADER rt, const D3D11_1DDIARG_STAGE_IO_SIGNATURES *s)
{ D11_TRACE("create_gs_so1");
    (void)rt;
    shader(DEV(h), OBJ(hs), D3DPT_D11S_GS, c->pShaderCode, sigs11_1(s), c);
}

static void APIENTRY create_hs1(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER rt,
                                const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *s)
{ D11_TRACE("create_hs1");
    (void)rt;
    shader(DEV(h), OBJ(hs), D3DPT_D11S_HS, code, sigs_tess1(s), NULL);
}

static void APIENTRY create_ds1(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER rt,
                                const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *s)
{ D11_TRACE("create_ds1");
    (void)rt;
    shader(DEV(h), OBJ(hs), D3DPT_D11S_DS, code, sigs_tess1(s), NULL);
}

/* constant buffers with a window into each (first constant and count, in
 * 16-byte constants); none given: the 11.0 binding */
static void set_cb1(D11_DEV *d, UINT stage, UINT start, UINT n, const D3D10DDI_HRESOURCE *b, const UINT *first, const UINT *num)
{
    d3dpt_d11_bind *t;
    d3dpt_d11_cb1 *e;
    UINT i;

    if (!first && !num) {
        set_cb(d, stage, start, n, b);
        return;
    }
    t = (d3dpt_d11_bind *)tok(d, D3DPT_D11_SET_CB1, sizeof(*t) + n * sizeof(*e), n);
    if (!t) {
        return;
    }
    t->stage = stage;
    t->start = start;
    t->n = n;
    e = (d3dpt_d11_cb1 *)(t + 1);
    for (i = 0; i < n; i++) {
        const D11_RES *r = RES(b[i]);

        patch_res(d, &e[i].res, r);
        e[i].first = first ? first[i] : 0;
        e[i].num = num ? num[i] : (r ? r->r.width / 16 : 0);
    }
}

static void APIENTRY vs_set_cb1(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HRESOURCE *b, const UINT *f, const UINT *c) { D11_TRACE("vs_set_cb1"); set_cb1(DEV(h), D3DPT_D11S_VS, s, n, b, f, c); }
static void APIENTRY ps_set_cb1(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HRESOURCE *b, const UINT *f, const UINT *c) { D11_TRACE("ps_set_cb1"); set_cb1(DEV(h), D3DPT_D11S_PS, s, n, b, f, c); }
static void APIENTRY gs_set_cb1(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HRESOURCE *b, const UINT *f, const UINT *c) { D11_TRACE("gs_set_cb1"); set_cb1(DEV(h), D3DPT_D11S_GS, s, n, b, f, c); }
static void APIENTRY hs_set_cb1(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HRESOURCE *b, const UINT *f, const UINT *c) { D11_TRACE("hs_set_cb1"); set_cb1(DEV(h), D3DPT_D11S_HS, s, n, b, f, c); }
static void APIENTRY ds_set_cb1(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HRESOURCE *b, const UINT *f, const UINT *c) { D11_TRACE("ds_set_cb1"); set_cb1(DEV(h), D3DPT_D11S_DS, s, n, b, f, c); }
static void APIENTRY cs_set_cb1(D3D10DDI_HDEVICE h, UINT s, UINT n, const D3D10DDI_HRESOURCE *b, const UINT *f, const UINT *c) { D11_TRACE("cs_set_cb1"); set_cb1(DEV(h), D3DPT_D11S_CS, s, n, b, f, c); }

/* Discard: a hint the host's device has no use for */
static void APIENTRY discard(D3D10DDI_HDEVICE h, D3D11DDI_HANDLETYPE type, VOID *p, const D3D10_DDI_RECT *r, UINT n)
{ D11_TRACE("discard");
    (void)h; (void)type; (void)p; (void)r; (void)n;
}

static void APIENTRY assign_debug_binary(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s, UINT n, const VOID *b)
{ D11_TRACE("assign_debug_binary");
    (void)h; (void)s; (void)n; (void)b;
}

/* a direct flip of an application's buffer onto the screen: never, DWM
 * composes it */
static void APIENTRY check_direct_flip(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE a, D3D10DDI_HRESOURCE b, UINT flags, BOOL *ok)
{ D11_TRACE("check_direct_flip");
    (void)h; (void)a; (void)b; (void)flags;
    *ok = FALSE;
}

/* ClearView of a render target view, with or without rectangles */
static void APIENTRY clear_view(D3D10DDI_HDEVICE h, D3D11DDI_HANDLETYPE type, VOID *v, const FLOAT c[4], const D3D10_DDI_RECT *r, UINT n)
{ D11_TRACE("clear_view");
    D11_DEV *d = DEV(h);
    d3dpt_d11_clear_view *t;

    if (type != D3D10DDI_HT_RENDERTARGETVIEW) {
        D11_TODO("ClearView of a view that is not a render target's");
        return;
    }
    t = (d3dpt_d11_clear_view *)tok(d, D3DPT_D11_CLEAR_VIEW, sizeof(*t) + n * sizeof(*r), 0);
    if (!t) {
        return;
    }
    t->kind = D3DPT_D11K_RTV;
    t->id = obj_id(v);
    CopyMemory(t->color, c, sizeof(t->color));
    t->n = n;
    if (n) {
        CopyMemory(t + 1, r, n * sizeof(*r));
    }
}

/* after d11_pipe_funcs on the same table (the 11.1 table starts as the
 * 11.0 one, entry for entry): the entries 11.1 changed or added */
void d11_pipe_funcs11_1(D3D11_1DDI_DEVICEFUNCS *f)
{
    f->pfnCalcPrivateBlendStateSize = calc_blend1;
    f->pfnCreateBlendState = create_blend1;
    f->pfnCalcPrivateRasterizerStateSize = calc_raster1;
    f->pfnCreateRasterizerState = create_raster1;
    f->pfnCalcPrivateShaderSize = calc_shader1;
    f->pfnCreateVertexShader = create_vs1;
    f->pfnCreateGeometryShader = create_gs1;
    f->pfnCreatePixelShader = create_ps1;
    f->pfnCalcPrivateGeometryShaderWithStreamOutput = calc_gs_so1;
    f->pfnCreateGeometryShaderWithStreamOutput = create_gs_so1;
    f->pfnCalcPrivateTessellationShaderSize = calc_tess1;
    f->pfnCreateHullShader = create_hs1;
    f->pfnCreateDomainShader = create_ds1;
    f->pfnVsSetConstantBuffers = vs_set_cb1;
    f->pfnPsSetConstantBuffers = ps_set_cb1;
    f->pfnGsSetConstantBuffers = gs_set_cb1;
    f->pfnHsSetConstantBuffers = hs_set_cb1;
    f->pfnDsSetConstantBuffers = ds_set_cb1;
    f->pfnCsSetConstantBuffers = cs_set_cb1;
    f->pfnDiscard = discard;
    f->pfnAssignDebugBinary = assign_debug_binary;
    f->pfnCheckDirectFlipSupport = check_direct_flip;
    f->pfnClearView = clear_view;
}
