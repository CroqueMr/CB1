// SPDX-License-Identifier: GPL-3.0-only
#include "reference_test.h"
#include "dvbridge_render.c"
#include <time.h>

static enum dvbridge_dv_policy_status finish(struct dvbridge_renderer *renderer)
{
    for(unsigned attempt=0;attempt<10000;attempt++){
        const enum dvbridge_dv_policy_status status=dvbridge_render_dv_policy_poll(renderer);
        if(status!=DVBRIDGE_DV_PENDING && status!=DVBRIDGE_DV_BUSY)
            return status;
        nanosleep(&(struct timespec){.tv_nsec=1000000},NULL);
    }
    assert(!"GPU completion timeout");
    return DVBRIDGE_DV_FAILED;
}
int main(void)
{
    const unsigned width=1920,height=1080,level=32;
    struct reference_gpu ctx=reference_gpu_create();
    pl_gpu gpu=ctx.gl->gpu;
    struct dvbridge_retirement_owner *owner=dvbridge_retirement_owner_create(gpu);
    assert(owner);
    struct dvbridge_renderer *r=dvbridge_renderer_create_with_owner(owner);
    assert(r);
    struct pl_frame source={.num_planes=2,.crop={0,0,width,height},
        .repr={.sys=PL_COLOR_SYSTEM_BT_2020_NC,.levels=PL_COLOR_LEVELS_LIMITED,.bits={.sample_depth=16,.color_depth=10,.bit_shift=6}},
        .color={.primaries=PL_COLOR_PRIM_BT_2020,.transfer=PL_COLOR_TRC_PQ,.hdr={.max_luma=1000}}};
    for(unsigned p=0;p<2;p++){
        unsigned w=width>>p,h=height>>p,c=p?2:1; uint16_t *data=calloc(w*h*c,2); assert(data);
        for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)for(unsigned j=0;j<c;j++){
            unsigned v=p?512:700;
            if(x>=w/4 && x<w/2 && y>=h/4 && y<h/2) v=p?512:level;
            data[(y*w+x)*c+j]=v<<6;
        }
        source.planes[p]=(struct pl_plane){.components=c,.component_mapping={p,p+1},.texture=pl_tex_create(gpu,pl_tex_params(.w=w,.h=h,.initial_data=data,.sampleable=true,.format=pl_find_fmt(gpu,PL_FMT_UNORM,c,16,16,PL_FMT_CAP_SAMPLEABLE)))};
        free(data); assert(source.planes[p].texture);
    }
    struct dvbridge_geometry geometry={width,height,0,0,3840,2160};
    struct cb1_ai_result prediction={.id={3,4,7},.pts=0,.raw={.l1_min=0,.l1_max=3080,.l1_avg=1463,.l3_min=2048,.l3_max=2048,.l3_avg=2048}};
    struct dvbridge_policy policy={.revision=7,.mode=DVBRIDGE_MODE_ENHANCED_DV,.tv={2300,DVBRIDGE_PANEL_UNSET,DVBRIDGE_GAMUT_BT2020}};
    bool accepted=dvbridge_render_hdr10_ai_rgb(r,&policy,&prediction,&source,&geometry)!=NULL;
    assert(accepted);
    assert(finish(r)==DVBRIDGE_DV_READY && "Enhanced must accept below-black 1080p input codes");
    const struct dvbridge_dv_policy_output *out=dvbridge_render_dv_policy_output(r);
    assert(!r->ai_enhanced_fused && out && out->value.measured);
    dvbridge_render_dv_policy_cancel(r);
    assert(finish(r)==DVBRIDGE_DV_IDLE);
    dvbridge_renderer_destroy(r);
    assert(dvbridge_retirement_owner_destroy(&owner));
    for(unsigned p=0;p<2;p++)pl_tex_destroy(gpu,&source.planes[p].texture);
    reference_gpu_destroy(&ctx);
    return 0;
}
