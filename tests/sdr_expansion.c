/* SPDX-License-Identifier: MIT */
#include "reference_test.h"
#include "cb1_hdr10_ai.h"
#include "dvbridge_gl_frame.h"
#include <GLES2/gl2ext.h>

static float encode_pq(double nits)
{
    double v = pow(nits / 10000.0, 2610.0 / 16384.0);
    return pow((3424.0 / 4096 + 2413.0 / 128 * v) / (1 + 2392.0 / 128 * v),
               2523.0 / 32);
}

static void read_pq(pl_gpu gpu, pl_tex image, int w, int h, uint16_t *wire)
{
    GLuint fbo, tex=pl_opengl_unwrap(gpu,image,NULL,NULL,NULL);
    glGenFramebuffers(1,&fbo);
    glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_SHORT,wire);
    assert(glGetError()==GL_NO_ERROR);
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    glDeleteFramebuffers(1,&fbo);
}

static void test_decoded_planes(pl_gpu gpu, struct dvbridge_renderer *renderer)
{
    const enum dvbridge_gl_layout layouts[]={DVBRIDGE_NV12,DVBRIDGE_YUV420P8,DVBRIDGE_P010};
    const enum pl_color_system matrices[]={PL_COLOR_SYSTEM_BT_601,PL_COLOR_SYSTEM_BT_709,
                                          PL_COLOR_SYSTEM_BT_2020_NC};
    const enum pl_color_primaries primaries[]={PL_COLOR_PRIM_BT_601_625,PL_COLOR_PRIM_BT_709,
                                               PL_COLOR_PRIM_BT_2020};
    struct dvbridge_identity id={19,0,12};
    for (unsigned gamut=0;gamut<3;++gamut) {
        uint16_t limited[4][3];
        for (unsigned layout=0;layout<3;++layout) for (unsigned full=0;full<2;++full) {
            ++id.picture;
            const bool planar=layouts[layout]==DVBRIDGE_YUV420P8;
            const bool ten=layouts[layout]==DVBRIDGE_P010;
            const int count=planar ? 3 : 2;
            const double samples[][3]={{0,0,0},{.5,0,0},{1,0,0},{.55,-.15,.25}};
            struct dvbridge_gl_plane planes[3]={0};
            GLuint textures[3];
            glGenTextures(count,textures);
            glPixelStorei(GL_UNPACK_ALIGNMENT,1);
            for (int c=0;c<count;++c) {
                int w=c ? 16 : 32, h=c ? 4 : 8, n=c && !planar ? 2 : 1;
                uint8_t bytes[256];
                uint16_t words[256];
                for (int y=0;y<h;++y) for (int x=0;x<w;++x) for (int j=0;j<n;++j) {
                    int component=c+j, i=(y*w+x)*n+j;
                    double v=samples[x/(w/4)][component];
                    int code=component ? 128+lround(v*(full ? 255 : 224)) :
                        (full ? 0 : 16)+lround(v*(full ? 255 : 219));
                    bytes[i]=code;
                    int code10=!full ? 4*code : component ? 512+lround((code-128)*1023./255.) :
                                                       lround(code*1023./255.);
                    words[i]=(uint16_t)(code10*64);
                }
                glBindTexture(GL_TEXTURE_2D,textures[c]);
                glTexImage2D(GL_TEXTURE_2D,0,ten ? (n==2 ? GL_RG16_EXT : GL_R16_EXT) :
                    (n==2 ? GL_RG8 : GL_R8),w,h,0,n==2 ? GL_RG : GL_RED,
                    ten ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE,ten ? (void*)words : bytes);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
                planes[c]=(struct dvbridge_gl_plane){textures[c],w,h};
            }
            struct dvbridge_gl_frame imported={0};
            assert(dvbridge_gl_frame_import(&imported,gpu,layouts[layout],32,8,planes));
            imported.frame.repr.sys=matrices[gamut];
            imported.frame.repr.levels=full ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
            imported.frame.color.primaries=primaries[gamut];
            imported.frame.color.transfer=PL_COLOR_TRC_BT_1886;
            struct pl_frame out;
            assert(dvbridge_prepare_sdr_expansion(renderer,&id,&imported.frame,0,false,&out,NULL)==CB1_AI_READY);
            uint16_t pixels[32*8*4];
            read_pq(gpu,out.planes[0].texture,32,8,pixels);
            for (int n=0;n<4;++n) {
                const uint16_t *rgb=pixels+4*(32*3+8*n+4);
                for (int c=0;c<3;++c) {
                    if (!layout && !full) limited[n][c]=rgb[c];
                    const int tolerance=full ? 450 : 40;
                    if (abs((int)rgb[c]-limited[n][c])>=tolerance)
                        fprintf(stderr,"gamut=%u layout=%u full=%u patch=%d channel=%d got=%u reference=%u\n",
                                gamut,layout,full,n,c,rgb[c],limited[n][c]);
                    assert(abs((int)rgb[c]-limited[n][c])<tolerance);
                    if (!n) assert(!rgb[c]);
                }
                if (n==3) assert(rgb[0]>rgb[1] && rgb[0]>rgb[2]);
            }
            dvbridge_gl_frame_release(&imported);
            glDeleteTextures(count,textures);
        }
    }
}

int main(void)
{
    struct reference_gpu gpu = reference_gpu_create();
    struct dvbridge_retirement_owner *owner = dvbridge_retirement_owner_create(gpu.gl->gpu);
    struct dvbridge_renderer *renderer = dvbridge_renderer_create_with_owner(owner);
    assert(renderer);
    assert(dvbridge_renderer_supports_sdr_expansion(renderer,false) &&
           dvbridge_renderer_supports_sdr_expansion(renderer,true) &&
           !dvbridge_renderer_supports_sdr_expansion(NULL,false));
    const int w = 20, h = 4;
    float data[20 * 4 * 4];
    for (int i = 0; i < w * h; ++i) {
        float level = (i % w) / (float)(w - 1);
        for (int c = 0; c < 3; ++c)
            data[4*i+c] = i / w == h - 1 && c ? 0 : level;
        if (i / w == 1 && i % w < 2)
            for (int c = 0; c < 3; ++c) data[4*i+c] = i % w ? 1.09f : .90f;
        data[4*i+3] = 1;
    }
    pl_tex input = reference_texture(gpu.gl->gpu, w, h, data);
    struct pl_frame source = reference_frame(input, w, h), output = {0};
    source.repr = pl_color_repr_rgb;
    source.repr.levels = PL_COLOR_LEVELS_FULL;
    source.color.primaries = PL_COLOR_PRIM_BT_709;
    source.color.transfer = PL_COLOR_TRC_GAMMA24;
    struct dvbridge_identity id = {3, 7, 2};
    float mids[2];
    for (unsigned dv = 0; dv < 2; ++dv) {
        const unsigned peak = dv ? 600 : 1000;
        ++id.revision;
        struct cb1_ai_result description = {0};
        enum cb1_ai_status status = dvbridge_prepare_sdr_expansion(renderer, &id, &source,
            0, dv, &output, &description);
        glFinish();
        if (status == CB1_AI_PENDING)
            status = dvbridge_prepare_sdr_expansion(renderer, &id, NULL, 0, dv, &output, &description);
        assert(status == CB1_AI_READY && output.num_planes == 1);
        assert(output.color.primaries == PL_COLOR_PRIM_BT_2020 && output.color.transfer == PL_COLOR_TRC_PQ);
        assert(output.color.hdr.max_luma == peak && output.planes[0].texture->params.w == w);
        float pixels[20 * 4 * 4];
        uint16_t wire[20 * 4 * 4];
        read_pq(gpu.gl->gpu,output.planes[0].texture,w,h,wire);
        for (int i=0;i<4*w*h;++i) pixels[i]=wire[i]/65535.f;
        assert(pixels[0] == 0 && pixels[1] == 0 && pixels[2] == 0);
        for (int x = 1; x < w; ++x) {
            assert(pixels[4*x] > pixels[4*(x-1)]);
            assert(fabs(pixels[4*x]-pixels[4*x+1]) < .0003);
        }
        assert(fabs(pixels[4*w] - encode_pq(203)) < .0005);
        assert(fabs(pixels[4*(w+1)] - encode_pq(peak)) < .0005);
        mids[dv] = pixels[4*(w/2)];
        assert(pixels[4*(w-1)] > encode_pq(203) && pixels[4*(w-1)] <= encode_pq(peak)+.001);
        const float *red = pixels + 4*(w*h-1);
        assert(red[0] > red[1] && red[1] > red[2] && red[2] > 0);
        if (!dv) {
            struct dvbridge_policy policy={.revision=id.revision,.mode=DVBRIDGE_MODE_HDR10_BASIC};
            struct dvbridge_geometry g={w,h,0,0,w,h,w,h};
            assert(dvbridge_render_expanded_hdr10(renderer,&policy,&id,g));
            pl_tex screen=reference_texture(gpu.gl->gpu,w,h,NULL);
            assert(dvbridge_render_hdr10_policy_resolve(renderer,screen,false,false,10));
            float presented[20*4*4];
            reference_read(gpu.gl->gpu,screen,0,0,w,h,presented);
            for (int i=0;i<w*h;++i) for (int c=0;c<3;++c)
                assert(fabs(presented[4*i+c] - pixels[4*i+c]) <= 2./1023.);
            assert(dvbridge_render_policy_commit(renderer,&id,screen));
            struct dvbridge_hdr10_policy_output signal;
            assert(dvbridge_render_policy_committed(renderer,&signal));
            assert(signal.hdr10.max_luminance==1000 && !signal.hdr10.max_cll && !signal.hdr10.max_fall);
            pl_tex_destroy(gpu.gl->gpu,&screen);
        } else {
            assert(description.id.stream==id.stream && description.id.picture==id.picture &&
                   description.id.revision==id.revision && !description.model);
            double sum=0, maximum=0;
            for (int i=0;i<w*h;i++) {
                double q=fmax(pixels[4*i],fmax(pixels[4*i+1],pixels[4*i+2]));
                sum+=q; maximum=fmax(maximum,q);
            }
            assert(description.raw.l1_min==0 && abs((int)description.raw.l1_max-lround(maximum*4095))<=1);
            assert(abs((int)description.raw.l1_avg-lround(sum/(w*h)*4095))<=1);
            assert(description.raw.l3_min==2048 && description.raw.l3_avg==2048 && description.raw.l3_max==2048);
            struct dvbridge_policy policy={.revision=id.revision,.mode=DVBRIDGE_MODE_STANDARD};
            struct dvbridge_geometry g={w,h,0,0,w,h,w,h};
            struct cb1_ai_result mismatched=description;
            ++mismatched.id.picture;
            assert(!dvbridge_render_hdr10_ai_rgb(renderer,&policy,&mismatched,&output,&g));
            assert(dvbridge_render_hdr10_ai_rgb(renderer,&policy,&description,&output,&g));
            const struct dvbridge_dv_policy_output *prepared=dvbridge_render_dv_policy_output(renderer);
            assert(prepared && prepared->bytes && prepared->value.identity.picture==id.picture);
            const AVDOVIMetadata *m=prepared->output_metadata;
            assert(av_dovi_find_level(m,1)->l1.max_pq==description.raw.l1_max &&
                   av_dovi_find_level(m,3)->l3.max_pq_offset==2048 &&
                   !av_dovi_find_level(m,2) && !av_dovi_find_level(m,8));
            dvbridge_render_dv_policy_cancel(renderer);
            glFinish();
            while(dvbridge_render_dv_policy_poll(renderer)==DVBRIDGE_DV_PENDING) {}
        }
    }
    assert(fabs(mids[0]-mids[1]) < .0005);
    ++id.picture;
    float black[20*4*4]={0};
    pl_tex black_input=reference_texture(gpu.gl->gpu,w,h,black);
    source.planes[0].texture=black_input;
    struct cb1_ai_result dark;
    enum cb1_ai_status status=dvbridge_prepare_sdr_expansion(renderer,&id,&source,-0.5,true,&output,&dark);
    glFinish();
    if (status==CB1_AI_PENDING)
        status=dvbridge_prepare_sdr_expansion(renderer,&id,NULL,-0.5,true,&output,&dark);
    assert(status==CB1_AI_READY && !dark.raw.l1_min && !dark.raw.l1_max && !dark.raw.l1_avg);
    struct dvbridge_policy standard={.revision=id.revision,.mode=DVBRIDGE_MODE_STANDARD};
    struct dvbridge_geometry geometry={w,h,0,0,w,h,w,h};
    assert(dvbridge_render_hdr10_ai_rgb(renderer,&standard,&dark,&output,&geometry));
    const struct dvbridge_dv_policy_output *packet=dvbridge_render_dv_policy_output(renderer);
    assert(packet && !av_dovi_find_level(packet->output_metadata,1)->l1.max_pq);
    dvbridge_render_dv_policy_cancel(renderer);
    glFinish();
    while(dvbridge_render_dv_policy_poll(renderer)==DVBRIDGE_DV_PENDING) {}
    if (pl_opengl_has_ext(gpu.gl,"GL_KHR_shader_subgroup")) {
        GLint stages=0,features=0;
        glGetIntegerv(0x9533,&stages);
        glGetIntegerv(0x9534,&features);
        if ((stages & GL_FRAGMENT_SHADER_BIT) && (features & 0x81)==0x81) {
            struct dvbridge_geometry fill={w,h,0,0,320,64,320,64};
            pl_tex screen=pl_tex_create(gpu.gl->gpu,pl_tex_params(.w=320,.h=64,
                .format=pl_find_named_fmt(gpu.gl->gpu,"rgba8"),.renderable=true));
            unsigned fbo;
            assert(screen && pl_opengl_unwrap(gpu.gl->gpu,screen,NULL,NULL,&fbo));
            assert(dvbridge_render_hdr10_ai_packed_overlay(renderer,&standard,&dark,
                &output,&fill,screen,fbo,false,NULL));
            assert(dvbridge_render_dv_policy_resolve(renderer,screen,fbo,false));
            packet=dvbridge_render_dv_policy_output(renderer);
            assert(packet && dvbridge_render_dv_policy_commit(renderer,&id,screen,packet->resolve_serial));
            dvbridge_render_dv_policy_cancel(renderer);
            glFinish();
            while (dvbridge_render_dv_policy_poll(renderer)==DVBRIDGE_DV_PENDING) {}
            pl_tex_destroy(gpu.gl->gpu,&screen);
            puts("Packed DV frame committed");
        }
    }
    pl_tex_destroy(gpu.gl->gpu,&black_input);
    ++id.picture;
    source.planes[0].texture=input;
    source.color.transfer=PL_COLOR_TRC_PQ;
    assert(dvbridge_prepare_sdr_expansion(renderer,&id,&source,1,true,&output,
                                         &(struct cb1_ai_result){0}) == CB1_AI_INCOMPATIBLE);
    test_decoded_planes(gpu.gl->gpu,renderer);
    pl_tex_destroy(gpu.gl->gpu,&input);
    dvbridge_renderer_destroy(renderer);
    assert(dvbridge_retirement_owner_destroy(&owner));
    reference_gpu_destroy(&gpu);
    return 0;
}
