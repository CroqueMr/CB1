/* SPDX-License-Identifier: MIT */
#include "reference_test.h"
#include "cb1_hdr10_ingress.h"
#include "cb1_hdr10_ai.h"
#include "dvbridge_gl_frame.h"
#include <GLES2/gl2ext.h>
static double pq(double n){double v=pow(n/10000.,2610./16384.);return pow((3424./4096.+2413./128.*v)/(1+2392./128.*v),2523./32.);}

static void test_hlg8(pl_gpu gpu, struct cb1_hdr10_ingress *ingress)
{
    const uint8_t code[3][4] = {{16,180,120,205}, {128,128,164,92}, {128,128,94,174}};
    enum { SIZE = 16, CHANNELS = 4, SAMPLES = SIZE * SIZE * CHANNELS };
    const int black = (2 * SIZE + 2) * CHANNELS;
    const int white = (2 * SIZE + 13) * CHANNELS;
    const int blue = (13 * SIZE + 2) * CHANNELS;
    const int red = (13 * SIZE + 13) * CHANNELS;
    GLuint tex[3];
    glGenTextures(3,tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    uint16_t reference[SAMPLES], hardware[SAMPLES];
    for(int layout=0;layout<3;++layout){
        const bool high=layout==0, planar=layout!=1;
        struct dvbridge_gl_plane planes[3]={0};
        for(int i=0;i<(planar?3:2);++i){
            const int size = i ? SIZE / 2 : SIZE;
            const bool pair = i && !planar;
            uint8_t samples[SIZE * SIZE];
            uint16_t expanded[SIZE * SIZE];
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const int quadrant = (y >= size / 2) * 2 + (x >= size / 2);
                    const int offset = (y * size + x) * (pair ? 2 : 1);
                    samples[offset] = code[i][quadrant];
                    if (pair) samples[offset + 1] = code[2][quadrant];
                    if (high) expanded[offset] = samples[offset] * 4;
                }
            }
            glBindTexture(GL_TEXTURE_2D,tex[i]);
            glTexImage2D(GL_TEXTURE_2D,0,high?GL_R16_EXT:pair?GL_RG8:GL_R8,size,size,0,
                         pair?GL_RG:GL_RED,high?GL_UNSIGNED_SHORT:GL_UNSIGNED_BYTE,
                         high?(const void*)expanded:samples);
            if(layout==2 && i)
                glTexParameteri(GL_TEXTURE_2D,i==1?GL_TEXTURE_SWIZZLE_G:GL_TEXTURE_SWIZZLE_A,GL_RED);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            planes[i]=(struct dvbridge_gl_plane){tex[i],size,size};
        }
        glBindTexture(GL_TEXTURE_2D,0);
        struct dvbridge_gl_frame imported={0};
        const enum dvbridge_gl_layout format=high?DVBRIDGE_YUV420P10:
            planar?DVBRIDGE_YUV420P8:DVBRIDGE_NV12;
        assert(dvbridge_gl_frame_import(&imported,gpu,format,SIZE,SIZE,planes));
        struct pl_frame *image=&imported.frame;
        image->repr.sys=PL_COLOR_SYSTEM_BT_2020_NC;
        image->repr.levels=PL_COLOR_LEVELS_LIMITED;
        image->color.primaries=PL_COLOR_PRIM_BT_2020;
        image->color.transfer=PL_COLOR_TRC_HLG;
        pl_tex rgb;
        assert(cb1_hdr10_ingress_rgb(ingress,image,&rgb)==CB1_AI_READY);
        uint16_t pixels[SAMPLES];
        assert(pl_tex_download(gpu,pl_tex_transfer_params(.tex=rgb,.ptr=pixels)));
        if(high) {
            memcpy(reference,pixels,sizeof(reference));
        } else {
            // Compare flat patch centers. R8 and R16 can round their
            // sub-pixel chroma interpolation differently at patch edges.
            const int centers[] = {black,white,blue,red};
            for (unsigned i=0;i<sizeof(centers)/sizeof(centers[0]);++i)
                for (int c=0;c<3;++c)
                    assert(abs(pixels[centers[i]+c]-reference[centers[i]+c])<132);
            if(planar)assert(memcmp(pixels,hardware,sizeof(hardware))==0);
            else memcpy(hardware,pixels,sizeof(hardware));
            assert(pixels[black]<64 && pixels[black+1]<64 && pixels[black+2]<64);
            assert(pixels[white]>30000 && abs(pixels[white]-pixels[white+1])<16 &&
                   abs(pixels[white]-pixels[white+2])<16);
            assert(pixels[blue+2]>pixels[blue] && pixels[red]>pixels[red+2]);
            image->repr.levels=PL_COLOR_LEVELS_FULL;
            assert(cb1_hdr10_ingress_validate(image)==CB1_AI_INCOMPATIBLE);
            image->repr.levels=PL_COLOR_LEVELS_LIMITED;
            image->color.transfer=PL_COLOR_TRC_PQ;
            assert(cb1_hdr10_ingress_validate(image)==CB1_AI_INCOMPATIBLE);
        }
        dvbridge_gl_frame_release(&imported);
    }
    glDeleteTextures(3,tex);
}

int main(int argc,char **argv)
{
    assert(argc==2);
    struct reference_gpu gpu=reference_gpu_create();
    struct cb1_hdr10_ingress *ingress=cb1_hdr10_ingress_create(gpu.gl->gpu);assert(ingress);
    struct pl_frame source={.num_planes=3,.crop={0,0,4,4},
        .repr={.sys=PL_COLOR_SYSTEM_BT_2020_NC,.levels=PL_COLOR_LEVELS_LIMITED,
            .bits={.sample_depth=16,.color_depth=10}},
        .color={.primaries=PL_COLOR_PRIM_BT_2020,.transfer=PL_COLOR_TRC_HLG}};
    uint16_t data[16];
    for(int c=0;c<3;c++){
        for(int i=0;i<16;i++)data[i]=c?512:64+657; /* 75% neutral HLG */
        pl_fmt f=pl_find_fmt(gpu.gl->gpu,PL_FMT_UNORM,1,16,16,PL_FMT_CAP_SAMPLEABLE);
        source.planes[c]=(struct pl_plane){.components=1,.component_mapping={c},
            .texture=pl_tex_create(gpu.gl->gpu,pl_tex_params(.w=4,.h=4,.format=f,.sampleable=true,.initial_data=data))};
        assert(source.planes[c].texture);
    }
    assert(cb1_hdr10_ingress_validate(&source)==CB1_AI_READY);
    pl_tex rgb;assert(cb1_hdr10_ingress_rgb(ingress,&source,&rgb)==CB1_AI_READY);
    uint16_t pixels[64];assert(pl_tex_download(gpu.gl->gpu,pl_tex_transfer_params(.tex=rgb,.ptr=pixels)));
    const double scene=(exp((.75-.55991073)/.17883277)+.28466892)/12.;
    const double expected=pq(1000*pow(scene,1.2));
    for(unsigned i=0;i<16;i++)for(unsigned c=0;c<3;c++)assert(fabs(pixels[i*4+c]/65535.-expected)<.001);
    pl_tex small;assert(cb1_hdr10_ingress_preview_u16(ingress,&source,&small)==CB1_AI_READY);
    float preview[4];reference_read(gpu.gl->gpu,small,60,30,1,1,preview);
    assert(fabs(preview[0]-expected)<.001);
    float reused[4];
    assert(cb1_hdr10_ingress_hlg_preview(ingress,rgb,&small)==CB1_AI_READY);
    reference_read(gpu.gl->gpu,small,60,30,1,1,reused);
    assert(memcmp(preview,reused,sizeof(preview))==0);
    struct pl_frame stripes=source;stripes.crop=(pl_rect2df){0,0,1024,576};
    uint16_t *pattern=malloc(1024*576*sizeof(*pattern));assert(pattern);
    for(unsigned c=0;c<3;c++){
        for(unsigned y=0;y<576;y++)for(unsigned x=0;x<1024;x++)pattern[y*1024+x]=c?512:64+((x%8)>=1 && (x%8)<=4?438:657);
        stripes.planes[c].texture=pl_tex_create(gpu.gl->gpu,pl_tex_params(.w=1024,.h=576,
            .format=source.planes[c].texture->params.format,.sampleable=true,.initial_data=pattern));
        assert(stripes.planes[c].texture);
    }
    assert(cb1_hdr10_ingress_preview_u16(ingress,&stripes,&small)==CB1_AI_READY);
    reference_read(gpu.gl->gpu,small,60,30,1,1,preview);
    assert(fabs(preview[0]-(expected+pq(1000*pow(1./12.,1.2)))*.5)<.002);
    for(unsigned c=0;c<3;c++)pl_tex_destroy(gpu.gl->gpu,&stripes.planes[c].texture);
    free(pattern);
    struct cb1_hdr10_ai *ai=NULL;
    assert(cb1_hdr10_ai_create(gpu.gl->gpu,argv[1],&ai)==CB1_AI_READY);
    cb1_hdr10_ai_reset(ai,3,7);
    for(unsigned picture=1;picture<=25;picture++){
        struct cb1_ai_identity id={3,picture,7};
        enum cb1_ai_status status=cb1_hdr10_ai_submit(ai,&source,id,(picture-1)*.04);
        assert(status==CB1_AI_PENDING);glFinish();
        for(unsigned tries=0;status==CB1_AI_PENDING && tries<10000;tries++)
            status=cb1_hdr10_ai_submit(ai,NULL,id,(picture-1)*.04);
        assert(status==CB1_AI_READY);
    }
    assert(cb1_hdr10_ai_advance(ai,1.)==CB1_AI_READY);
    struct cb1_ai_result inferred;
    assert(cb1_hdr10_ai_poll(ai,(struct cb1_ai_identity){3,1,7},&inferred)==CB1_AI_READY);
    assert(inferred.model && inferred.id.picture==1 && inferred.pts==0);
    assert(inferred.base[0]<=inferred.base[1]);
    for(unsigned i=1;i<3;i++)assert(fabs(inferred.base[i]-expected*4095)<3);
    cb1_hdr10_ai_destroy(&ai);
    /* Movie and model input must have the same reference PQ domain. */
    struct dvbridge_retirement_owner *owner=dvbridge_retirement_owner_create(gpu.gl->gpu);
    struct dvbridge_renderer *r=dvbridge_renderer_create_with_owner(owner);assert(r);
    struct dvbridge_policy policy={.revision=7,.mode=DVBRIDGE_MODE_STANDARD};
    struct cb1_ai_result prediction={.id={3,4,7},.pts=0,
        .raw={.l1_min=0,.l1_max=3128,.l1_avg=1412,.l3_min=2048,.l3_max=2048,.l3_avg=2048}};
    struct dvbridge_geometry g={4,4,0,0,4,4,4,4};
    pl_tex result=dvbridge_render_hdr10_ai_rgb(r,&policy,&prediction,&source,&g);assert(result);
    float film[4];reference_read(gpu.gl->gpu,result,2,2,1,1,film);
    assert(fabs(film[0]-expected)<.001);
    const struct dvbridge_dv_policy_output *out=dvbridge_render_dv_policy_output(r);assert(out);
    assert(out->value.container.transfer==PL_COLOR_TRC_PQ && out->value.nominal_max_nits==1000);
    const AVDOVIMetadata *m=out->output_metadata;assert(!av_dovi_find_level(m,2) && !av_dovi_find_level(m,8));
    struct pl_frame normalized=reference_frame(result,4,4);normalized.repr=pl_color_repr_rgb;
    normalized.color=out->value.container;
    struct dvbridge_policy sdr={.revision=7,.mode=DVBRIDGE_MODE_SDR};
    const struct dvbridge_identity sdr_id={3,4,7};
    /* A mode switch cannot overwrite the retiring DV picture's GPU storage. */
    assert(!dvbridge_render_sdr_policy_rgb(r,&sdr,&sdr_id,&normalized,NULL,0,0,0,g));
    assert(!dvbridge_render_policy_output(r));
    dvbridge_render_dv_policy_cancel(r);glFinish();
    while(dvbridge_render_dv_policy_poll(r)==DVBRIDGE_DV_PENDING){}
    policy.mode=DVBRIDGE_MODE_ENHANCED_DV;policy.tv=(struct dvbridge_tv_profile){1000,DVBRIDGE_PANEL_OLED,DVBRIDGE_GAMUT_BT2020};
    assert(!dvbridge_render_hdr10_ai_rgb(r,&policy,&prediction,&source,&g));
    source.repr.dovi=(void*)1;assert(cb1_hdr10_ingress_validate(&source)==CB1_AI_INCOMPATIBLE);
    dvbridge_renderer_destroy(r);assert(dvbridge_retirement_owner_destroy(&owner));
    test_hlg8(gpu.gl->gpu,ingress);
    cb1_hdr10_ingress_destroy(&ingress);
    for(unsigned c=0;c<3;c++)pl_tex_destroy(gpu.gl->gpu,&source.planes[c].texture);
    reference_gpu_destroy(&gpu);return 0;
}
