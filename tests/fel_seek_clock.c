/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include "synthetic_idr.h"

static AVRational base_clock = {1, 1000};
static AVRational enhancement_clock = {1, 90000};
static int64_t anchor, enhancement_offset;
static unsigned reads, uneven;

static int test_seek(AVFormatContext *format, int stream, int64_t position, int flags)
{
    (void)format;
    (void)stream;
    (void)flags;
    anchor = position;
    reads = 0;
    return 0;
}

static int test_read(AVFormatContext *format, AVPacket *packet)
{
    (void)format;
    if (reads >= (uneven ? 3u : 2u))
        return AVERROR_EOF;
    const unsigned packet_index = reads++;
    const bool enhancement = uneven == 1 ? packet_index == 2 :
                             uneven == 2 ? packet_index != 2 : packet_index == 1;
    const int64_t source = uneven ? anchor - 200 + (packet_index == 1 ? 100 : 0) : anchor;
    assert(av_new_packet(packet, sizeof(synthetic_paired)) == 0);
    memcpy(packet->data, synthetic_paired, sizeof(synthetic_paired));
    packet->stream_index = enhancement;
    packet->pts = packet->dts = enhancement
        ? av_rescale_q(source, base_clock, enhancement_clock) + enhancement_offset : source;
    packet->pos = enhancement ? 100 : 200;
    return 0;
}

static int test_parse(AVCodecParserContext *parser, AVCodecContext *codec,
                      uint8_t **output, int *size, const uint8_t *data,
                      int bytes, int64_t pts, int64_t dts, int64_t position)
{
    (void)codec;
    (void)pts;
    (void)dts;
    (void)position;
    parser->key_frame = 1;
    *output = (uint8_t *)data;
    *size = bytes;
    return bytes;
}

#define av_seek_frame test_seek
#define av_read_frame test_read
#define av_parser_parse2 test_parse
#include "dvbridge_fel_seek.h"

int main(void)
{
    AVFormatContext *format = avformat_alloc_context();
    assert(format);
    AVStream *base = avformat_new_stream(format, NULL);
    AVStream *enhancement = avformat_new_stream(format, NULL);
    assert(base && enhancement);
    base->start_time = 0;
    base->time_base = base_clock;
    enhancement->time_base = enhancement_clock;
    base->codecpar->codec_type = enhancement->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    base->codecpar->codec_id = enhancement->codecpar->codec_id = AV_CODEC_ID_HEVC;

    AVStreamGroup *group = avformat_stream_group_create(format, AV_STREAM_GROUP_PARAMS_DOLBY_VISION, NULL);
    assert(group);
    group->params.layered_video->el_index = 1;
    assert(avformat_stream_group_add_stream(group, base) == 0);
    assert(avformat_stream_group_add_stream(group, enhancement) == 0);

    int64_t first_position = -1;
    assert(dvbridge_fel_seek_point_at(format, 0, 1000, &first_position, NULL) == 1000);
    assert(first_position == 100);
    for (enhancement_offset = -1; enhancement_offset <= 1; enhancement_offset += 2) {
        assert(dvbridge_fel_seek_point_at(format, 0, 1000, &first_position, NULL) == AV_NOPTS_VALUE);
        assert(first_position == -1);
    }
    enhancement_offset = 0;
    for (uneven = 1; uneven <= 2; ++uneven) {
        assert(dvbridge_fel_seek_point_at(format, 0, 1000, &first_position, NULL) == 800);
        assert(first_position == 100);
    }
    avformat_free_context(format);
}
