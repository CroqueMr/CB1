/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reference_test.h"

/* Inverse of the pinned HPE LMS-to-BT.2020 matrix. */
static void input_pixels(float pixels[32],double peak)
{
    pl_matrix3x3 rgb2lms={{{3.06441879,-2.16597676,.10155818},
        {-.65612108,1.78554118,-.12943749},{.01736321,-.04725154,1.03004253}}};
    pl_matrix3x3_invert(&rgb2lms);
    const double rgb[8][3]={{0,0,0},{1,1,1},{1,0,0},{0,1,0},{0,0,1},
        {1,1,0},{0,1,1},{1,0,1}};
    for(int i=0;i<8;i++) {
        for(int c=0;c<3;c++) {
            double lms=0;for(int j=0;j<3;j++)lms+=rgb2lms.m[c][j]*rgb[i][j]*peak;
            pixels[4*i+c]=pl_hdr_rescale(PL_HDR_NITS,PL_HDR_PQ,lms);
        }
        pixels[4*i+3]=1;
    }
}

/* The saturated P8.1 chart in issue #339 has P3 mastering metadata but
 * contains distinct BT.2020 samples up to 10000 nits. */
static void check_colour_volume(struct dvbridge_renderer *r, pl_gpu gpu)
{
    const unsigned yuv[3][4][3]={
        {{294,387,960},{282,393,937},{271,400,915},{259,406,892}},
        {{116,959,476},{114,935,478},{111,915,479},{108,892,481}},
        {{658,189,100},{628,206,121},{598,221,141},{569,237,161}},
    };
    float pixels[4*4*4];
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){
        for(unsigned c=0;c<3;c++)pixels[(4*y+x)*4+c]=yuv[y%3][x][c]*64.0f/65535.0f;
        pixels[(4*y+x)*4+3]=1;
    }
    size_t bytes;AVDOVIMetadata *m=task3_fixture(&bytes);
    AVDOVIColorMetadata *c=av_dovi_get_color(m);
    const int ycc[9]={9574,0,13802,9574,-1540,-5348,9574,17610,0};
    const int lms[9]={7222,8771,390,2654,12430,1300,0,422,15962};
    for(unsigned i=0;i<9;i++){
        c->ycc_to_rgb_matrix[i]=(AVRational){ycc[i],8192};
        c->rgb_to_lms_matrix[i]=(AVRational){lms[i],16384};
    }
    c->ycc_to_rgb_offset[0]=(AVRational){1,16};
    c->ycc_to_rgb_offset[1]=c->ycc_to_rgb_offset[2]=(AVRational){1,2};
    c->source_min_pq=62;c->source_max_pq=3696;c->signal_full_range_flag=1;
    *av_dovi_get_ext(m,0)=(AVDOVIDmData){.level=1,.l1={0,4087,2048}};
    *av_dovi_get_ext(m,1)=(AVDOVIDmData){.level=2,.l2={3079,2013,2050,1936,2048,2048,2048}};
    *av_dovi_get_ext(m,2)=(AVDOVIDmData){.level=6,.l6={.max_luminance=4000,.min_luminance=50,
        .max_cll=10000,.max_fall=9953}};
    *av_dovi_get_ext(m,3)=(AVDOVIDmData){.level=9,.dvbridge_raw_magic=0x41424456,
        .dvbridge_original_length=1};
    m->num_ext_blocks=4;
    pl_tex input=reference_texture(gpu,4,4,pixels);
    struct pl_frame frame=reference_frame(input,4,4);
    frame.planes[0].components=3;frame.repr.alpha=PL_ALPHA_NONE;
    frame.repr.bits=(struct pl_bit_encoding){.sample_depth=16,.color_depth=10,.bit_shift=6};
    struct dvbridge_policy p=reference_policy();
    p.tv.peak_nits=2600;p.tv.gamut=DVBRIDGE_GAMUT_BT2020;p.expert_output_bits=10;
    struct dvbridge_identity id={10,1,1};
    assert(dvbridge_render_hdr10_policy_rgb(r,&p,&id,&frame,m,bytes,0,0,
        (struct dvbridge_geometry){4,4,0,0,4,4}));
    float out[64];reference_read(gpu,dvbridge_render_texture(r),0,0,4,4,out);
    const unsigned cap=floor(pl_hdr_rescale(PL_HDR_NITS,PL_HDR_PQ,p.tv.peak_nits)*1023);
    for(unsigned y=0;y<3;y++){
        long previous[3]={0};
        for(unsigned x=0;x<4;x++){
            long rgb[3];unsigned separation=0;
            for(unsigned k=0;k<3;k++){
                rgb[k]=lround(out[(4*y+x)*4+k]*1023);
                assert(rgb[k]>=0 && rgb[k]<=cap);
                separation+=labs(rgb[k]-previous[k]);
                previous[k]=rgb[k];
            }
            fprintf(stderr,"Expert volume row=%u patch=%u RGB10=%ld,%ld,%ld separation=%u\n",
                y,x,rgb[0],rgb[1],rgb[2],separation);
            if(x)assert(separation>=2);
        }
    }
    av_free(m);pl_tex_destroy(gpu,&input);
}
int main(void)
{
    struct reference_gpu g=reference_gpu_create();pl_gpu gpu=g.gl->gpu;
    struct dvbridge_renderer *r=dvbridge_renderer_create(gpu);assert(r);
    check_colour_volume(r,gpu);
    const double targets[]={600,1000,2000,10000,600};unsigned count=0;
    for(int master=1000;master<=4000;master+=3000) {
        float pixels[32];input_pixels(pixels,master);
        pl_tex input=reference_texture(gpu,8,1,pixels);
        struct pl_frame frame=reference_frame(input,8,1);
        size_t bytes;AVDOVIMetadata *m=task3_fixture(&bytes);m->num_ext_blocks=2;
        unsigned peak=lround(pl_hdr_rescale(PL_HDR_NITS,PL_HDR_PQ,master)*4095);
        av_dovi_get_color(m)->source_max_pq=peak;
        *av_dovi_get_ext(m,0)=(AVDOVIDmData){.level=1,.l1={.max_pq=peak,.avg_pq=2081}};
        *av_dovi_get_ext(m,1)=(AVDOVIDmData){.level=6,.l6={.max_luminance=master}};
        void *copy=av_memdup(m,bytes);assert(copy);
        for(int gamut=DVBRIDGE_GAMUT_P3_D65;gamut<=DVBRIDGE_GAMUT_BT2020;gamut++)
            for(unsigned k=0;k<sizeof(targets)/sizeof(*targets);k++) {
                struct dvbridge_policy policy=reference_policy();
                policy.tv.peak_nits=targets[k];policy.tv.gamut=gamut;
                struct dvbridge_identity id={1,++count,1};
                assert(dvbridge_render_hdr10_policy_rgb(r,&policy,&id,&frame,m,bytes,0,0,
                    (struct dvbridge_geometry){8,1,0,0,8,1}));
                float out[32];reference_read(gpu,dvbridge_render_texture(r),0,0,8,1,out);
                double maximum=0,white=0;
                for(int i=0;i<8;i++) {
                    double y=0;const double weights[]={.2627,.678,.0593};
                    for(int c=0;c<3;c++) {
                        assert(isfinite(out[4*i+c]) && out[4*i+c]>=0 && out[4*i+c]<=1);
                        double n=pl_hdr_rescale(PL_HDR_PQ,PL_HDR_NITS,out[4*i+c]);
                        y+=n*weights[c];
                        if(i==0)assert(out[c]==0);
                    }
                    if(y>maximum)maximum=y;
                    if(i==1)white=y;
                }
                fprintf(stderr,"Expert master=%d target=%.0f gamut=%d maxY=%.6f white=%.6f\n",
                    master,targets[k],gamut,maximum,white);
                assert(maximum<=targets[k]+1);
                if(targets[k]>master)assert(fabs(white-master)<2);
                assert(!memcmp(copy,m,bytes));
            }
        av_free(copy);av_free(m);pl_tex_destroy(gpu,&input);
    }
    /* Exercise the direct 10-bit scanout path with the saturating yellow case. */
    float patches[32];input_pixels(patches,4000);
    float *full=malloc(3840u*2160*4*sizeof(float));assert(full);
    for(unsigned i=0;i<3840u*2160;i++)memcpy(full+4*i,patches+5*4,4*sizeof(float));
    pl_tex input=reference_texture(gpu,3840,2160,full);free(full);
    pl_tex output=reference_texture(gpu,3840,2160,NULL);
    struct pl_frame frame=reference_frame(input,3840,2160);
    size_t bytes;AVDOVIMetadata *m=task3_fixture(&bytes);m->num_ext_blocks=2;
    av_dovi_get_color(m)->source_max_pq=3696;
    *av_dovi_get_ext(m,0)=(AVDOVIDmData){.level=1,.l1={.max_pq=3696,.avg_pq=2081}};
    *av_dovi_get_ext(m,1)=(AVDOVIDmData){.level=6,.l6={.max_luminance=4000}};
    for(unsigned k=0;k<3;k++) {
        struct dvbridge_policy p=reference_policy();p.tv.peak_nits=targets[k];p.tv.gamut=DVBRIDGE_GAMUT_BT2020;
        struct dvbridge_identity id={1,++count,1};
        assert(dvbridge_render_hdr10_policy_direct(r,&p,&id,&frame,m,bytes,0,0,
            (struct dvbridge_geometry){3840,2160,0,0,3840,2160},output,false,true,10));
        float sample[4];reference_read(gpu,output,1920,1080,1,1,sample);
        double cap=ceil(pl_hdr_rescale(PL_HDR_NITS,PL_HDR_PQ,targets[k])*876+64);
        for(int c=0;c<3;c++) {
            double code=round(sample[c]*1023);
            assert(isfinite(sample[c]) && code>=64 && code<=cap);
        }
    }
    av_free(m);pl_tex_destroy(gpu,&input);pl_tex_destroy(gpu,&output);
    dvbridge_renderer_destroy(r);reference_gpu_destroy(&g);
    printf("PASS %u Expert peak cases, unchanged metadata and no inverse expansion\n",count);
}
