#define MINIMP4_IMPLEMENTATION
#ifdef _WIN32
#include <sys/types.h>
#include <stddef.h>
typedef size_t ssize_t;
#endif

#include "minimp4.h"
#include <fdk-aac/libAACenc/include/aacenc_lib.h>
#include <fdk-aac/libAACdec/include/aacdecoder_lib.h>

#define VIDEO_FPS 24
#define AUDIO_RATE 24000

#define FILE_INPUT            "mux_out/output.mp4"
#define VIDEO_FILE_OUTPUT     "demux_out/output_video.h264"
#define AUDIO_FILE_OUTPUT     "demux_out/output_audio.aac"
#define USE_SHORT_SYNC 0

typedef struct
{
    uint8_t *buffer;
    ssize_t size;
} INPUT_BUFFER;

static uint8_t *preload(const char *path, ssize_t *data_size)
{
    FILE *file = fopen(path, "rb");
    uint8_t *data;
    *data_size = 0;
    if (!file)
        return 0;
    if (fseek(file, 0, SEEK_END))
        exit(1);
    *data_size = (ssize_t)ftell(file);
    if (*data_size < 0)
        exit(1);
    if (fseek(file, 0, SEEK_SET))
        exit(1);
    data = (unsigned char *)malloc(*data_size);
    if (!data)
        exit(1);
    if ((ssize_t)fread(data, 1, *data_size, file) != *data_size)
        exit(1);
    fclose(file);
    return data;
}

static int read_callback(int64_t offset, void *buffer, size_t size, void *token)
{
    INPUT_BUFFER *buf = (INPUT_BUFFER *)token;
    size_t to_copy = MINIMP4_MIN(size, buf->size - offset - size);
    memcpy(buffer, buf->buffer + offset, to_copy);
    return to_copy != size;
}

int demux(uint8_t *input_buf, ssize_t input_size, FILE *f_video, FILE *f_audio, int ntrack)
{
    int /*ntrack, */ i, spspps_bytes;
    const void *spspps;
    INPUT_BUFFER buf = {input_buf, input_size};
    MP4D_demux_t mp4 = {0, };
    MP4D_open(&mp4, read_callback, &buf, input_size);

    for (ntrack = 0; ntrack < mp4.track_count; ntrack++)
    {
        MP4D_track_t *tr = mp4.track + ntrack;
        unsigned sum_duration = 0;
        i = 0;
        if (tr->handler_type == MP4D_HANDLER_TYPE_VIDE)
        { // assume h264
printf("Go to MP4D_HANDLER_TYPE_VIDE!\n");
            char sync[4] = {0, 0, 0, 1};
            while (spspps = MP4D_read_sps(&mp4, ntrack, i, &spspps_bytes))
            {
                fwrite(sync + USE_SHORT_SYNC, 1, 4 - USE_SHORT_SYNC, f_video);
                fwrite(spspps, 1, spspps_bytes, f_video);
                i++;
            }
            i = 0;
            while (spspps = MP4D_read_pps(&mp4, ntrack, i, &spspps_bytes))
            {
                fwrite(sync + USE_SHORT_SYNC, 1, 4 - USE_SHORT_SYNC, f_video);
                fwrite(spspps, 1, spspps_bytes, f_video);
                i++;
            }
            for (i = 0; i < mp4.track[ntrack].sample_count; i++)
            {
                unsigned frame_bytes, timestamp, duration;
                MP4D_file_offset_t ofs = MP4D_frame_offset(&mp4, ntrack, i, &frame_bytes, &timestamp, &duration);
                uint8_t *mem = input_buf + ofs;
                sum_duration += duration;
                while (frame_bytes)
                {
                    uint32_t size = ((uint32_t)mem[0] << 24) | ((uint32_t)mem[1] << 16) | ((uint32_t)mem[2] << 8) | mem[3];
                    size += 4;
                    mem[0] = 0;
                    mem[1] = 0;
                    mem[2] = 0;
                    mem[3] = 1;
                    fwrite(mem + USE_SHORT_SYNC, 1, size - USE_SHORT_SYNC, f_video);
                    if (frame_bytes < size)
                    {
                        printf("error: demux sample failed\n");
                        exit(1);
                    }
                    frame_bytes -= size;
                    mem += size;
                }
            }
        }
        else if (tr->handler_type == MP4D_HANDLER_TYPE_SOUN)
        { // assume aac
printf("Go to MP4D_HANDLER_TYPE_SOUN!\n");
            HANDLE_AACDECODER dec = aacDecoder_Open(TT_MP4_RAW, 1);
            UCHAR *dsi = (UCHAR *)tr->dsi;
            UINT dsi_size = tr->dsi_bytes;
            if (AAC_DEC_OK != aacDecoder_ConfigRaw(dec, &dsi, &dsi_size))
            {
                printf("error: aac config fail\n");
                exit(1);
            }
            for (i = 0; i < mp4.track[ntrack].sample_count; i++)
            {
                unsigned frame_bytes, timestamp, duration;
                MP4D_file_offset_t ofs = MP4D_frame_offset(&mp4, ntrack, i, &frame_bytes, &timestamp, &duration);
                // printf("ofs=%d frame_bytes=%d timestamp=%d duration=%d\n", (unsigned)ofs, frame_bytes, timestamp, duration);
                UCHAR *frame = (UCHAR *)(input_buf + ofs);
                UINT frame_size = frame_bytes;
                UINT valid = frame_size;
                if (AAC_DEC_OK != aacDecoder_Fill(dec, &frame, &frame_size, &valid))
                {
                    printf("error: aac decode fail\n");
                    exit(1);
                }
                INT_PCM pcm[2048 * 8];
                int err = aacDecoder_DecodeFrame(dec, pcm, sizeof(pcm), 0);
                if (AAC_DEC_OK != err)
                {
                    printf("error: aac decode fail %d\n", err);
                    exit(1);
                }
                CStreamInfo *info = aacDecoder_GetStreamInfo(dec);
                if (!info)
                {
                    printf("error: aac decode fail\n");
                    exit(1);
                }
                fwrite(pcm, sizeof(INT_PCM) * info->frameSize * info->numChannels, 1, f_audio);
            }
        }
    }

    MP4D_close(&mp4);
    if (input_buf)
        free(input_buf);
    return 0;
}

int main(int argc, char **argv)
{
    // check switches
    int track = 0;
    int i;
    
    for (i = 1; i < argc; i++)
    {
        if (argv[i][0] != '-')
            break;
        switch (argv[i][1])
        {
            case 't':
                i++;
                if (i < argc)
                    track = atoi(argv[i]);
                break;
            default:
                printf("error: unrecognized option\n");
                return 1;
        }
    }
    // if (argc <= (i + 1))
    // {
    //     printf("Usage: minimp4 [command] [options] input output\n"
    //            "Commands:\n"
    //            "    -m    - do muxing (default); input is h264 elementary stream, output is mp4 file\n"
    //            "    -d    - do de-muxing; input is mp4 file, output is h264 elementary stream\n"
    //            "Options:\n"
    //            "    -s    - enable mux sequential mode (no seek required for writing)\n"
    //            "    -f    - enable mux fragmentation mode (aka fMP4)\n"
    //            "    -t    - de-mux tack number\n");
    //     return 0;
    // }

    ssize_t h264_size;
    uint8_t *alloc_buf = preload(FILE_INPUT, &h264_size);;
    if (!alloc_buf)
    {
        printf("error: can't open h264 file\n");
        exit(1);
    }

    FILE *f_video = fopen(VIDEO_FILE_OUTPUT, "wb");
    if (!f_video)
    {
        printf("error: can't open output file\n");
        exit(1);
    }

    FILE *f_audio = fopen(AUDIO_FILE_OUTPUT, "wb");
    if (!f_audio)
    {
        printf("error: can't open output file\n");
        exit(1);
    }


 track = 1;
    printf("Track: %d\n", track);
    return demux(alloc_buf, h264_size, f_video, f_audio, track);
}