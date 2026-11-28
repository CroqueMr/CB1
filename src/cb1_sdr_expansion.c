/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cb1_sdr_expansion.h"
#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/colorspace.h>
#include <libplacebo/shaders/custom.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* An 8x8 reduction needs at most four passes for 3840x2160. */
struct cb1_sdr_expansion {
    pl_gpu gpu;
    pl_dispatch dispatch;
    pl_tex image, reductions[4];
    pl_buf readback;
    struct dvbridge_identity id;
    struct pl_frame frame;
    struct cb1_ai_result description;
    double pts;
    bool dv, pending;
};

void cb1_sdr_expansion_destroy(struct cb1_sdr_expansion **ptr)
{
    struct cb1_sdr_expansion *s = *ptr;
    if (!s) return;
    pl_buf_destroy(s->gpu, &s->readback);
    for (unsigned i = 0; i < 4; ++i) pl_tex_destroy(s->gpu, &s->reductions[i]);
    pl_tex_destroy(s->gpu, &s->image);
    pl_dispatch_destroy(&s->dispatch);
    free(s);
    *ptr = NULL;
}

static bool valid_source(const struct pl_frame *f)
{
    if (!f || f->rotation || f->num_overlays || f->enhancement_layer || f->repr.dovi ||
        f->icc || f->profile.len || f->lut || f->num_planes < 1 || f->num_planes > 3 ||
        f->crop.x0 != 0 || f->crop.y0 != 0 || !(f->crop.x1 >= 1 && f->crop.x1 <= 3840) ||
        !(f->crop.y1 >= 1 && f->crop.y1 <= 2160) || floorf(f->crop.x1) != f->crop.x1 ||
        floorf(f->crop.y1) != f->crop.y1) return false;
    unsigned channels=0;
    for (int i=0; i<f->num_planes; ++i) {
        const struct pl_plane *plane=&f->planes[i];
        if (!plane->texture || !plane->texture->params.sampleable || plane->components<1 ||
            plane->components>plane->texture->params.format->num_components) return false;
        for (int j=0; j<plane->components; ++j) {
            int c=plane->component_mapping[j];
            if (c < 0) continue;
            if (c > 3 || (channels & (1u<<c))) return false;
            channels |= 1u<<c;
        }
    }
    if ((channels & 7) != 7) return false;
    if (f->planes[0].texture->params.w < f->crop.x1 ||
        f->planes[0].texture->params.h < f->crop.y1) return false;
    const enum pl_color_transfer trc = f->color.transfer;
    return (trc == PL_COLOR_TRC_BT_1886 || trc == PL_COLOR_TRC_SRGB ||
            trc == PL_COLOR_TRC_GAMMA22 || trc == PL_COLOR_TRC_GAMMA24 ||
            trc == PL_COLOR_TRC_GAMMA28) &&
        (f->repr.levels == PL_COLOR_LEVELS_LIMITED || f->repr.levels == PL_COLOR_LEVELS_FULL) &&
        (f->repr.sys == PL_COLOR_SYSTEM_RGB || f->repr.sys == PL_COLOR_SYSTEM_BT_601 ||
         f->repr.sys == PL_COLOR_SYSTEM_BT_709 || f->repr.sys == PL_COLOR_SYSTEM_BT_2020_NC) &&
        (f->color.primaries == PL_COLOR_PRIM_BT_601_525 || f->color.primaries == PL_COLOR_PRIM_BT_601_625 ||
         f->color.primaries == PL_COLOR_PRIM_BT_709 || f->color.primaries == PL_COLOR_PRIM_BT_2020);
}

struct expansion_curve {
    struct pl_color_space source, target;
    float matrix[9];
    bool applied, quantize16;
};

static struct pl_hook_res apply_curve(void *priv, const struct pl_hook_params *p)
{
    struct expansion_curve *curve = priv;
    pl_shader_linearize(p->sh, &curve->source);
    float peak = curve->target.hdr.max_luma / PL_COLOR_SDR_WHITE;
    struct pl_shader_var vars[] = {
        {.var=pl_var_mat3("to2020"), .data=curve->matrix},
        {.var=pl_var_float("peak"), .data=&peak},
    };
    /* Fixed diffuse white at 90% BT.1886. Only the tangent-continuous
     * highlight term depends on the target. Keep hue when limiting RGB. */
    bool ok = pl_shader_custom(p->sh, &(struct pl_custom_shader){
        .input=PL_SHADER_SIG_COLOR, .output=PL_SHADER_SIG_COLOR,
        .variables=vars, .num_variables=2,
        .body="vec3 rgb=max(to2020*color.rgb,0.0);"
              "float y=dot(rgb,vec3(.2627,.6780,.0593));"
              "const float white=0.776572528; const float top=1.22976959;"
              "float h=clamp((y-white)/(top-white),0.0,1.0);"
              "float out_y=y/white+(peak-top/white)*h*h*h;"
              "rgb*=y>0.0 ? out_y/y : 0.0;"
              "float largest=max(rgb.r,max(rgb.g,rgb.b));"
              "color.rgb=rgb*(largest>peak ? peak/largest : 1.0);",
    });
    pl_shader_delinearize(p->sh, &curve->target);
    /* ST 2084 maps zero light to a tiny positive code before quantization. */
    ok = ok && pl_shader_custom(p->sh, &(struct pl_custom_shader){
        .input=PL_SHADER_SIG_COLOR, .output=PL_SHADER_SIG_COLOR,
        .body="color.rgb=mix(vec3(0.0),color.rgb,greaterThan(color.rgb,vec3(0.000001)));",
    });
    if (ok && curve->quantize16)
        ok = pl_shader_custom(p->sh, &(struct pl_custom_shader){
            .input=PL_SHADER_SIG_COLOR, .output=PL_SHADER_SIG_COLOR,
            .body="color=roundEven(clamp(color,0.0,1.0)*65535.0)/65535.0;",
        });
    curve->applied = ok;
    return (struct pl_hook_res){.failed=!ok, .output=PL_HOOK_SIG_NONE};
}

static pl_fmt image_format(pl_gpu gpu)
{
    return pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR);
}

static pl_fmt reduction_format(pl_gpu gpu)
{
    return pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_HOST_READABLE);
}

bool cb1_sdr_expansion_supported(pl_gpu gpu, bool dv)
{
    return gpu && image_format(gpu) && (!dv || (gpu->limits.buf_transfer &&
        gpu->limits.max_buf_size >= 16 && gpu->glsl.version >= (gpu->glsl.gles ? 300 : 130) &&
        reduction_format(gpu)));
}

static bool convert(pl_renderer renderer, const struct pl_frame *source, bool dv,
                    pl_tex dst, struct pl_frame *frame)
{
    const int w=dst->params.w, h=dst->params.h;
    struct expansion_curve curve = {
        /* Match the normalized 16-bit intermediate when drawing into the GUI backing. */
        .quantize16=dst->params.format->type==PL_FMT_FLOAT,
        .source={.primaries=source->color.primaries, .transfer=source->color.transfer,
                 .hdr={.max_luma=PL_COLOR_SDR_WHITE, .min_luma=PL_COLOR_HDR_BLACK}},
        .target={.primaries=PL_COLOR_PRIM_BT_2020, .transfer=PL_COLOR_TRC_PQ,
                 .hdr={.max_luma=dv ? 600 : 1000, .min_luma=PL_COLOR_HDR_BLACK}},
    };
    pl_matrix3x3 m = pl_get_color_mapping_matrix(pl_raw_primaries_get(curve.source.primaries),
        pl_raw_primaries_get(curve.target.primaries), PL_INTENT_RELATIVE_COLORIMETRIC);
    for (int i=0;i<3;++i)
        for (int j=0;j<3;++j) curve.matrix[3*j+i]=m.m[i][j];
    const struct pl_hook hook = {.stages=PL_HOOK_OUTPUT, .input=PL_HOOK_SIG_COLOR,
        .priv=&curve, .hook=apply_curve, .signature=UINT64_C(0x4342315344524501)};
    const struct pl_hook *hooks[] = {&hook};
    struct pl_render_params params = pl_render_fast_params;
    params.hooks=hooks;
    params.num_hooks=1;
    params.peak_detect_params=NULL;
    params.dither_params=NULL;
    params.cb1_fast_chroma=true;
    params.min_fbo_precision=16;
    params.skip_caching_single_frame=true;
    struct pl_frame input = *source;
    input.color = curve.source;
    *frame = (struct pl_frame){.num_planes=1, .repr=pl_color_repr_rgb, .color=curve.target,
        .crop={0,0,w,h}, .planes={{.texture=dst, .components=4, .component_mapping={0,1,2,3}}}};
    /* Decode and reconstruct chroma in the original signal space. The final
     * hook owns the only transfer and gamut conversion into the PQ texture. */
    struct pl_frame target = *frame;
    target.color = input.color;
    return pl_render_image(renderer, &input, &target, &params) && curve.applied;
}

bool cb1_sdr_expansion_hdr10_into(pl_renderer renderer, const struct pl_frame *source,
    pl_tex target, struct pl_frame *frame)
{
    if (!valid_source(source) || target->params.w!=source->crop.x1 ||
        target->params.h!=source->crop.y1) return false;
    for (int i=0; i<source->num_planes; ++i)
        if (source->planes[i].texture==target) return false;
    return convert(renderer,source,false,target,frame);
}

/* Min, max and arithmetic mean of maximum PQ channel, over every video pixel.
 * A final 16-byte record is the only download, with no read of a live film texture. */
static bool measure(struct cb1_sdr_expansion *s)
{
    if (!s->dispatch) s->dispatch = pl_dispatch_create(NULL, s->gpu);
    if (!s->readback) s->readback = pl_buf_create(s->gpu, pl_buf_params(.size=16, .host_readable=true));
    pl_fmt fmt = reduction_format(s->gpu);
    if (!s->dispatch || !s->readback || !fmt) return false;
    pl_tex input=s->image;
    pl_dispatch_reset_frame(s->dispatch);
    for (unsigned i=0; i<4; ++i) {
        int w=(input->params.w+7)/8, h=(input->params.h+7)/8;
        pl_tex *dst=&s->reductions[i];
        if (!pl_tex_recreate(s->gpu, dst, pl_tex_params(.w=w, .h=h, .format=fmt,
            .sampleable=true, .renderable=true, .host_readable=w==1 && h==1))) return false;
        struct pl_shader_desc sampler={.desc={.name="pixels", .type=PL_DESC_SAMPLED_TEX},
                                      .binding={.object=input, .sample_mode=PL_TEX_SAMPLE_NEAREST}};
        pl_shader sh=pl_dispatch_begin(s->dispatch);
        bool ok=pl_shader_custom(sh, &(struct pl_custom_shader){
            .output=PL_SHADER_SIG_COLOR, .descriptors=&sampler, .num_descriptors=1,
            .prelude=i ? "#define REDUCTION_INPUT 1\n" : "",
            .body="ivec2 start=8*ivec2(gl_FragCoord.xy), size=textureSize(pixels,0);"
                  "color=vec4(1.0,0.0,0.0,0.0);"
                  "for(int y=0;y<8 && start.y+y<size.y;++y)"
                  "for(int x=0;x<8 && start.x+x<size.x;++x){"
                  "vec4 v=texelFetch(pixels,start+ivec2(x,y),0);"
                  "\n#ifndef REDUCTION_INPUT\nfloat q=max(v.r,max(v.g,v.b));v=vec4(q,q,q,1);\n#endif\n"
                  "color=vec4(min(color.r,v.r),max(color.g,v.g),color.ba+v.ba);}",
        });
        if (ok) ok=pl_dispatch_finish(s->dispatch, pl_dispatch_params(.shader=&sh,.target=*dst));
        pl_dispatch_abort(s->dispatch,&sh);
        if (!ok) return false;
        input=*dst;
        if (w==1 && h==1)
            return pl_tex_download(s->gpu, pl_tex_transfer_params(.tex=input,.buf=s->readback));
    }
    return false;
}

static unsigned pq_code(float q)
{
    return (unsigned)lroundf(fminf(1, fmaxf(0, q))*4095);
}

enum cb1_ai_status cb1_sdr_expansion_prepare(struct cb1_sdr_expansion **ptr,
    pl_gpu gpu, pl_renderer renderer, const struct dvbridge_identity *id,
    const struct pl_frame *source, double pts, bool dv, struct pl_frame *pq,
    struct cb1_ai_result *description)
{
    if (!id || !id->stream || !id->picture || !id->revision || !pq || (dv && !description) ||
        !isfinite(pts)) return CB1_AI_INVALID;
    struct cb1_sdr_expansion *s=*ptr;
    if (!s || !dvbridge_identity_equal(&s->id,id)) {
        if (!source) return CB1_AI_INVALID;
        if (!valid_source(source) || ((!s || s->dv!=dv) && !cb1_sdr_expansion_supported(gpu,dv)))
            return CB1_AI_INCOMPATIBLE;
        if (!s) {
            *ptr=s=calloc(1,sizeof(*s));
            if (!s) return CB1_AI_INCOMPATIBLE;
            s->gpu=gpu;
        }
        /* Only a new DV reduction overwrites the asynchronous transfer buffer. */
        if (dv && s->readback && pl_buf_poll(gpu,s->readback,0)) return CB1_AI_PENDING;
        s->id=(struct dvbridge_identity){0};
        s->pending=false;
        pl_fmt fmt=image_format(gpu);
        if (!fmt || !pl_tex_recreate(gpu,&s->image,pl_tex_params(
                .w=source->crop.x1,.h=source->crop.y1,.format=fmt,.renderable=true,.sampleable=true)) ||
            !convert(renderer,source,dv,s->image,&s->frame) || (dv && !measure(s)))
            return CB1_AI_INCOMPATIBLE;
        s->id=*id;
        s->pts=pts;
        s->dv=dv;
        s->pending=dv;
    }
    if (pts!=s->pts || dv!=s->dv) return CB1_AI_INVALID;
    if (s->pending) {
        if (pl_buf_poll(gpu,s->readback,0)) return CB1_AI_PENDING;
        float record[4];
        if (!pl_buf_read(gpu,s->readback,0,record,sizeof(record)) ||
            record[3]!=s->image->params.w*s->image->params.h ||
            !isfinite(record[0]) || !isfinite(record[1]) || !isfinite(record[2]))
            return CB1_AI_INVALID;
        s->description=(struct cb1_ai_result){.id={id->stream,id->picture,id->revision},
            .pts=pts,.source_flags=CB1_SOURCE_SDR_EXPANDED | CB1_SOURCE_STATISTICS,
            .raw={.l1_min=pq_code(record[0]), .l1_max=pq_code(record[1]),
                  .l1_avg=pq_code(fminf(record[1], fmaxf(record[0],record[2]/record[3]))),
                  .l3_min=2048, .l3_avg=2048, .l3_max=2048}};
        s->pending=false;
    }
    *pq=s->frame;
    if (dv) *description=s->description;
    return CB1_AI_READY;
}

bool cb1_sdr_expansion_matches_dv(const struct cb1_sdr_expansion *s,
    const struct cb1_ai_result *description, const struct pl_frame *source)
{
    return s && s->dv && !s->pending && source && source->num_planes==1 &&
        source->planes[0].texture==s->image && description->id.stream==s->id.stream &&
        description->id.picture==s->id.picture && description->id.revision==s->id.revision &&
        description->pts==s->pts && description->source_flags==s->description.source_flags &&
        !memcmp(&description->raw,&s->description.raw,sizeof(description->raw));
}

const struct pl_frame *cb1_sdr_expansion_hdr10(const struct cb1_sdr_expansion *s,
    const struct dvbridge_identity *id)
{
    return s && id && !s->dv && s->id.stream && dvbridge_identity_equal(&s->id,id) ? &s->frame : NULL;
}
