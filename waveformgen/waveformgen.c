/*
 * Copyright (c) 2010 Nicolas George
 * Copyright (c) 2011 Stefano Sabatini
 * Copyright (c) 2014 Andrey Utkin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * API example for demuxing, decoding, filtering, encoding and muxing
 * @example transcoding.c
 */

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavfilter/avfiltergraph.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/bprint.h"
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include "waveformgen.h"

static AVFormatContext *ifmt_ctx;
static AVFormatContext *ofmt_ctx;
typedef struct FilteringContext {
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;
} FilteringContext;
static FilteringContext *filter_ctx;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
unsigned int stream_index;

static int open_input_file(const char *filename)
{
    int ret;
    unsigned int i;
    
    ifmt_ctx = NULL;
    if ((ret = avformat_open_input(&ifmt_ctx, filename, NULL, NULL)) < 0) {
        fprintf(stderr, "Cannot open input file: %s", filename);
        return ret;
    }
    
    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Cannot find stream information");
        return ret;
    }
    
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *stream;
        AVCodecContext *codec_ctx;
        stream = ifmt_ctx->streams[i];
        codec_ctx = stream->codec;
        /* Reencode video & audio and remux subtitles etc. */
        if (codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            stream_index = i;
            /* Open decoder */
            ret = avcodec_open2(codec_ctx,
                                avcodec_find_decoder(codec_ctx->codec_id), NULL);
            if (ret < 0) {
                fprintf(stderr, "Failed to open decoder for stream #%u", i);
                return ret;
            }
        }
    }
    
    av_dump_format(ifmt_ctx, 0, filename, 0);
    return 0;
}

static int open_output_file(const char *filename)
{
    AVStream *out_stream;
    AVStream *in_stream;
    AVCodecContext *dec_ctx, *enc_ctx;
    AVCodec *encoder;
    int ret;
    unsigned int i;
    
    ofmt_ctx = NULL;
    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, filename);
    if (!ofmt_ctx) {
        fprintf(stderr, "Could not create output context");
        return AVERROR_UNKNOWN;
    }
    
    
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        in_stream = ifmt_ctx->streams[i];
        dec_ctx = in_stream->codec;
        if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            out_stream = avformat_new_stream(ofmt_ctx, NULL);
            if (!out_stream) {
                fprintf(stderr, "Failed allocating output stream");
                return AVERROR_UNKNOWN;
            }
            enc_ctx = out_stream->codec;
            
            /* in this example, we choose transcoding to some codec */
            encoder = avcodec_find_encoder(ofmt_ctx->oformat->audio_codec);
            
            /* In this example, we transcode to same properties (picture size,
             * sample rate etc.). These properties can be changed for output
             * streams easily using filters */
            enc_ctx->sample_rate = dec_ctx->sample_rate;
            enc_ctx->channel_layout = av_get_default_channel_layout(2);
            enc_ctx->channels = 2;
            /* take first format from list of supported formats */
            enc_ctx->sample_fmt = encoder->sample_fmts[0];
            enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
            
            /* Third parameter can be used to pass settings to encoder */
            ret = avcodec_open2(enc_ctx, encoder, NULL);
            if (ret < 0) {
                fprintf(stderr, "Cannot open video encoder for stream #%u", i);
                return ret;
            }
        }
    }
    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        enc_ctx->flags |= CODEC_FLAG_GLOBAL_HEADER;
    
    av_dump_format(ofmt_ctx, 0, filename, 1);
    
    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open output file '%s'", filename);
            return ret;
        }
    }
    /* init muxer, write output file header */
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file");
        return ret;
    }
    
    return 0;
}

static int init_filter(FilteringContext* fctx, AVCodecContext *dec_ctx,
                       AVCodecContext *enc_ctx, const char *filter_spec)
{
    char args[512];
    int ret = 0;
    AVFilter *buffersrc = NULL;
    AVFilter *buffersink = NULL;
    AVFilterContext *buffersrc_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterGraph *filter_graph = avfilter_graph_alloc();
    
    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    
    if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        buffersrc = avfilter_get_by_name("abuffer");
        buffersink = avfilter_get_by_name("abuffersink");
        if (!buffersrc || !buffersink) {
            fprintf(stderr, "filtering source or sink element not found");
            ret = AVERROR_UNKNOWN;
            goto end;
        }
        
        if (!dec_ctx->channel_layout)
            dec_ctx->channel_layout =
            av_get_default_channel_layout(dec_ctx->channels);
        snprintf(args, sizeof(args),
                 "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
                 dec_ctx->time_base.num, dec_ctx->time_base.den, dec_ctx->sample_rate,
                 av_get_sample_fmt_name(dec_ctx->sample_fmt),
                 dec_ctx->channel_layout);
        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                           args, NULL, filter_graph);
        if (ret < 0) {
            fprintf(stderr, "Cannot create audio buffer source");
            goto end;
        }
        
        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                           NULL, NULL, filter_graph);
        if (ret < 0) {
            fprintf(stderr, "Cannot create audio buffer sink");
            goto end;
        }
        
        ret = av_opt_set_bin(buffersink_ctx, "sample_fmts",
                             (uint8_t*)&enc_ctx->sample_fmt, sizeof(enc_ctx->sample_fmt),
                             AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            fprintf(stderr, "Cannot set output sample format");
            goto end;
        }
        
        ret = av_opt_set_bin(buffersink_ctx, "channel_layouts",
                             (uint8_t*)&enc_ctx->channel_layout,
                             sizeof(enc_ctx->channel_layout), AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            fprintf(stderr, "Cannot set output channel layout");
            goto end;
        }
        
        ret = av_opt_set_bin(buffersink_ctx, "sample_rates",
                             (uint8_t*)&enc_ctx->sample_rate, sizeof(enc_ctx->sample_rate),
                             AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            fprintf(stderr, "Cannot set output sample rate");
            goto end;
        }
    } else {
        ret = AVERROR_UNKNOWN;
        goto end;
    }
    
    /* Endpoints for the filter graph. */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;
    
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;
    
    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    
    if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_spec,
                                        &inputs, &outputs, NULL)) < 0)
        goto end;
    
    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;
    av_buffersink_set_frame_size(buffersink_ctx,enc_ctx->frame_size);
    /* Fill FilteringContext */
    fctx->buffersrc_ctx = buffersrc_ctx;
    fctx->buffersink_ctx = buffersink_ctx;
    fctx->filter_graph = filter_graph;
    
end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    
    return ret;
}

static int init_filters(const char *filter_config)
{
    const char *filter_spec;
    int ret;
    filter_ctx = av_malloc(sizeof(*filter_ctx));
    if (!filter_ctx)
        return AVERROR(ENOMEM);
    
    filter_ctx->buffersrc_ctx  = NULL;
    filter_ctx->buffersink_ctx = NULL;
    filter_ctx->filter_graph   = NULL;
    
    
    filter_spec = filter_config; /* passthrough (dummy) filter for audio */
    ret = init_filter(filter_ctx, ifmt_ctx->streams[stream_index]->codec,
                      ofmt_ctx->streams[0]->codec, filter_spec);
    if (ret)
        return ret;
    
    return 0;
}

static int encode_write_frame(AVFrame *filt_frame, unsigned int stream_index, int *got_frame) {
    int ret;
    int got_frame_local;
    AVPacket enc_pkt;
    
    if (!got_frame)
        got_frame = &got_frame_local;
    
    /* encode filtered frame */
    enc_pkt.data = NULL;
    enc_pkt.size = 0;
    av_init_packet(&enc_pkt);
    ret = avcodec_encode_audio2(ofmt_ctx->streams[stream_index]->codec, &enc_pkt,
                                filt_frame, got_frame);
    av_frame_free(&filt_frame);
    if (ret < 0)
        return ret;
    if (!(*got_frame))
        return 0;
    
    /* prepare packet for muxing */
    enc_pkt.stream_index = stream_index;
    av_packet_rescale_ts(&enc_pkt,
                         ofmt_ctx->streams[stream_index]->codec->time_base,
                         ofmt_ctx->streams[stream_index]->time_base);
    
    /* mux encoded frame */
    ret = av_interleaved_write_frame(ofmt_ctx, &enc_pkt);
    return ret;
}

static int filter_encode_write_frame(AVFrame *frame)
{
    int ret;
    AVFrame *filt_frame;
    
    /* push the decoded frame into the filtergraph */
    ret = av_buffersrc_add_frame_flags(filter_ctx->buffersrc_ctx,
                                       frame, 0);
    if (ret < 0) {
        fprintf(stderr, "Error while feeding the filtergraph");
        return ret;
    }
    
    /* pull filtered frames from the filtergraph */
    while (1) {
        filt_frame = av_frame_alloc();
        if (!filt_frame) {
            ret = AVERROR(ENOMEM);
            break;
        }
        
        ret = av_buffersink_get_frame(filter_ctx->buffersink_ctx,
                                      filt_frame);
        if (ret < 0) {
            /* if no more frames for output - returns AVERROR(EAGAIN)
             * if flushed and no more frames for output - returns AVERROR_EOF
             * rewrite retcode to 0 to show it as normal procedure completion
             */
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                ret = 0;
            av_frame_free(&filt_frame);
            break;
        }
        
        filt_frame->pict_type = AV_PICTURE_TYPE_NONE;
        ret = encode_write_frame(filt_frame, 0, NULL);
        if (ret < 0)
            break;
    }
    
    return ret;
}

static int flush_encoder(unsigned int stream_index)
{
    int ret;
    int got_frame;
    
    if (!(ofmt_ctx->streams[stream_index]->codec->codec->capabilities &
          CODEC_CAP_DELAY))
        return 0;
    
    while (1) {
        ret = encode_write_frame(NULL, stream_index, &got_frame);
        if (ret < 0)
            break;
        if (!got_frame)
            return 0;
    }
    return ret;
}

void log_callback(void* ptr, int level, const char* fmt, va_list vl);

AVBPrint *buffer;
char jsonFileNameTmpl[28];

int wfg_generateImage(char *infile, char *outfile)
{
    int ret, samplesPerLine, samplesPerSmallLine, sampleRate, duration = 0;
    buffer = av_malloc(sizeof(AVBPrint));
    av_bprint_init(buffer, width*8+1, width*8+1);
    AVPacket packet = { .data = NULL, .size = 0 };
    AVFrame *frame = NULL;
    long samples;
    long readedSamples = 0;
    int got_frame;
    sprintf(jsonFileNameTmpl, "%s_%%s.json", infile);
    
    av_log_set_callback(&log_callback);
    
    
    av_register_all();
    avfilter_register_all();
    
    if ((ret = open_input_file(infile)) < 0)
        goto end;
    if ((ret = open_output_file(outfile)) < 0)
        goto end;
    duration = ifmt_ctx->duration/1000;
    fflush(stdout);
    sampleRate = ifmt_ctx->streams[stream_index]->codec->sample_rate;
    samples = ifmt_ctx->duration*sampleRate/AV_TIME_BASE;
    samplesPerLine = samples/width;
    samplesPerSmallLine = samples/widthSmall;
    char filter_descr[48];
    sprintf(filter_descr, "wf=n=%d:w=%d:h=%d,wf=n=%d:w=%d:h=%d",
            samplesPerSmallLine, widthSmall, height, samplesPerLine, width, height);
    if ((ret = init_filters(filter_descr)) < 0)
        goto end;
    
    time_t ts, oldTs;
    ts = oldTs = time(NULL);
    
    /* read all packets */
    while (1) {
        ts = time(NULL);
        if((ts - oldTs) > 1) {
            oldTs = ts;
            printf("%ld\n", readedSamples*100/samples);
            fflush(stdout);
        }
        
        if ((ret = av_read_frame(ifmt_ctx, &packet)) < 0)
            break;
        if (stream_index != packet.stream_index)
            continue;
        
        if (filter_ctx->filter_graph) {
            frame = av_frame_alloc();
            if (!frame) {
                ret = AVERROR(ENOMEM);
                break;
            }
            av_packet_rescale_ts(&packet,
                                 ifmt_ctx->streams[stream_index]->time_base,
                                 ifmt_ctx->streams[stream_index]->codec->time_base);
            ret = avcodec_decode_audio4(ifmt_ctx->streams[stream_index]->codec, frame,
                                        &got_frame, &packet);
            if (ret < 0) {
                av_frame_free(&frame);
                break;
            }
            
            if (got_frame) {
                readedSamples += frame->nb_samples;
                frame->pts = av_frame_get_best_effort_timestamp(frame);
                ret = filter_encode_write_frame(frame);
                av_frame_free(&frame);
                if (ret < 0)
                    goto end;
            } else {
                av_frame_free(&frame);
            }
        } else {
            /* remux this frame without reencoding */
            av_packet_rescale_ts(&packet,
                                 ifmt_ctx->streams[stream_index]->time_base,
                                 ofmt_ctx->streams[0]->time_base);
            
            ret = av_interleaved_write_frame(ofmt_ctx, &packet);
            if (ret < 0)
                goto end;
        }
        av_free_packet(&packet);
    }
    
    /* flush filters and encoders */
    /* flush filter */
    ret = filter_encode_write_frame(NULL);
    if (ret < 0) {
        fprintf(stderr, "Flushing filter failed");
        goto end;
    }
    
    /* flush encoder */
    ret = flush_encoder(0);
    if (ret < 0) {
        fprintf(stderr, "Flushing encoder failed");
        goto end;
    }
    
    av_write_trailer(ofmt_ctx);
end:
    printf("%d\n", duration);
    av_free_packet(&packet);
    av_frame_free(&frame);
    
    if (ifmt_ctx && ifmt_ctx->streams[stream_index])
        avcodec_close(ifmt_ctx->streams[stream_index]->codec);
    if (filter_ctx && filter_ctx->filter_graph)
        avfilter_graph_free(&filter_ctx->filter_graph);
    if(ofmt_ctx && ofmt_ctx->streams[0])
        avcodec_close(ofmt_ctx->streams[0]->codec);
    av_free(filter_ctx);
    avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_close(ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);
    
    if (ret < 0)
        fprintf(stderr, "Error occurred: %s", av_err2str(ret));
    
    return ret ? 1 : 0;
}

int flag = 1;

void log_callback(void* ptr, int level, const char* fmt, va_list vl)
{
    if (level == 49) {
        pthread_mutex_lock(&mutex);
        av_vbprintf (buffer, fmt, vl);
        FILE *jsonFile;
        char jsonFileName[32];
        sprintf(jsonFileName, jsonFileNameTmpl, flag ? "m" : "s");
        jsonFile = fopen(jsonFileName, "w");
        fprintf(jsonFile, "{\"width\":%d,\"height\":%d,\"samples\":[%s]}",
                flag ? width : widthSmall, height, buffer->str);
        fflush(jsonFile);
        if(flag){
            av_bprint_init(buffer, widthSmall*8+1, widthSmall*8+1);
            flag = 0;
        }
        pthread_mutex_unlock(&mutex);
    }
}
