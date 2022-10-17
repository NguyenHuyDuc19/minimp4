#define MINIMP4_IMPLEMENTATION
#ifdef _WIN32
#include <sys/types.h>
#include <stddef.h>
typedef size_t ssize_t;
#endif

#include <fdk-aac/libAACenc/include/aacenc_lib.h>
#include <fdk-aac/libAACdec/include/aacdecoder_lib.h>
#include "minimp4.h"

#define VIDEO_FPS 24
#define AUDIO_RATE 24000

#define OUTPUT_FILE         "mux_out/output.mp4"
#define VIDEO_INPUT_FILE    "demux_out/output_video.h264"
#define AUDIO_INPUT_FILE    "demux_out/output_audio.aac"

typedef struct
{
    uint8_t *buffer;
    ssize_t size;
} INPUT_BUFFER;

void help(void)
{
    printf("Usage: minimp4 [options]\n"
            "Options:\n"
            "    -s    - enable mux sequential mode (no seek required for writing)\n"
            "    -f    - enable mux fragmentation mode (aka fMP4)\n"
            "    -t    - de-mux tack number\n"
            "Input: %s, %s\n"
            "Output: %s\n", VIDEO_INPUT_FILE, AUDIO_INPUT_FILE, OUTPUT_FILE
            );
}

// read a file to a uint8_t pointer
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

static ssize_t get_nal_size(uint8_t *buf, ssize_t size)
{
    ssize_t pos = 3;
    while ((size - pos) > 3) // size > 3 + pos
    {
        if (buf[pos] == 0 && buf[pos + 1] == 0 && buf[pos + 2] == 1)
            return pos; // return pos if 001
        if (buf[pos] == 0 && buf[pos + 1] == 0 && buf[pos + 2] == 0 && buf[pos + 3] == 1)
            return pos; // return pos if 0001
        pos++;
    }
    return size;
}

static int write_callback(int64_t offset, const void *buffer, size_t size, void *token)
{
    FILE *f = (FILE *)token;
    fseek(f, offset, SEEK_SET);
    return fwrite(buffer, 1, size, f) != size;
}

int main(int argc, char **argv)
{
    if(argc <= 1 || argv[1] == "help" || argv[1] == "-help" || argv[1] == "-h")
    {
        help();
        return 0;
    }

    // check switches
    int sequential_mode = 0;
    int fragmentation_mode = 0;
    int track = 0;
    int i;

    switch (argv[1][1])
    {
        case 's':
            sequential_mode = 1;
            break;
        case 'f':
            fragmentation_mode = 1;
            break;
        case 't':
            i++;
            if (i < argc)
                track = atoi(argv[i]);
            break;
        default:
            printf("error: unrecognized option\n");
            return 1;
    }
    
    ssize_t h264_size;
    uint8_t *alloc_buf;
    uint8_t *buf_h264 = alloc_buf = preload(VIDEO_INPUT_FILE, &h264_size);

    FILE *fout = fopen(OUTPUT_FILE, "wb");

    /* strstr return NULL if string not exit or pointer points to fist position of string */
    int is_hevc = (0 != strstr(VIDEO_INPUT_FILE, "265")) || (0 != strstr(VIDEO_INPUT_FILE, "hevc")); // if using h264, is_hevc will be zero

    MP4E_mux_t *mux;
    mp4_h26x_writer_t mp4wr;
    mux = MP4E_open(sequential_mode, fragmentation_mode, fout, write_callback);
    if (MP4E_STATUS_OK != mp4_h26x_write_init(&mp4wr, mux, 352, 288, is_hevc))
    {
        printf("error: mp4_h26x_write_init failed\n");
        exit(1);
    }

    /*----------------------------- mux audio -----------------------------*/
    ssize_t pcm_size;
    int16_t *alloc_pcm;
    int16_t *buf_pcm = alloc_pcm = (int16_t *)preload(AUDIO_INPUT_FILE, &pcm_size);
    if (!buf_pcm)
    {
        printf("error: can't open pcm file\n");
        exit(1);
    }
    uint32_t sample = 0, total_samples = pcm_size / 2;
    uint64_t ts = 0, ats = 0;
    HANDLE_AACENCODER aacenc;
    AACENC_InfoStruct info;
    aacEncOpen(&aacenc, 0, 0);
    aacEncoder_SetParam(aacenc, AACENC_TRANSMUX, 0);
    aacEncoder_SetParam(aacenc, AACENC_AFTERBURNER, 1);
    aacEncoder_SetParam(aacenc, AACENC_BITRATE, 64000);
    aacEncoder_SetParam(aacenc, AACENC_SAMPLERATE, AUDIO_RATE);
    aacEncoder_SetParam(aacenc, AACENC_CHANNELMODE, 1);
    aacEncEncode(aacenc, NULL, NULL, NULL, NULL);
    aacEncInfo(aacenc, &info);

    MP4E_track_t tr;
    tr.track_media_kind = e_audio;
    tr.language[0] = 'u';
    tr.language[1] = 'n';
    tr.language[2] = 'd';
    tr.language[3] = 0;
    tr.object_type_indication = MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;
    tr.time_scale = 90000;
    tr.default_duration = 0;
    tr.u.a.channelcount = 1;
    int audio_track_id = MP4E_add_track(mux, &tr);
    MP4E_set_dsi(mux, audio_track_id, info.confBuf, info.confSize);

    /*----------------------------- mux video -----------------------------*/
    while (h264_size > 0)
    {
        ssize_t nal_size = get_nal_size(buf_h264, h264_size);
        if (nal_size < 4)
        {
            buf_h264 += 1;
            h264_size -= 1;
            continue;
        }

        if (MP4E_STATUS_OK != mp4_h26x_write_nal(&mp4wr, buf_h264, nal_size, 90000 / VIDEO_FPS))
        {
            printf("error: mp4_h26x_write_nal failed\n");
            exit(1);
        }
        buf_h264 += nal_size;
        h264_size -= nal_size;

        /*----------------------------- mux audio -----------------------------*/
        if (fragmentation_mode && !mux->fragments_count)
            continue; /* make sure mp4_h26x_write_nal writes sps/pps, because in fragmentation mode first MP4E_put_sample writes moov with track information and dsi.
                         all tracks dsi must be set (MP4E_set_dsi) before first MP4E_put_sample. */
        ts += 90000 / VIDEO_FPS;
        while (ats < ts)
        {
            AACENC_BufDesc in_buf, out_buf;
            AACENC_InArgs in_args;
            AACENC_OutArgs out_args;
            uint8_t buf[2048];
            if (total_samples < 1024)
            {
                buf_pcm = alloc_pcm;
                total_samples = pcm_size / 2;
            }
            in_args.numInSamples = 1024;
            void *in_ptr = buf_pcm, *out_ptr = buf;
            int in_size = 2 * in_args.numInSamples;
            int in_element_size = 2;
            int in_identifier = IN_AUDIO_DATA;
            int out_size = sizeof(buf);
            int out_identifier = OUT_BITSTREAM_DATA;
            int out_element_size = 1;

            in_buf.numBufs = 1;
            in_buf.bufs = &in_ptr;
            in_buf.bufferIdentifiers = &in_identifier;
            in_buf.bufSizes = &in_size;
            in_buf.bufElSizes = &in_element_size;
            out_buf.numBufs = 1;
            out_buf.bufs = &out_ptr;
            out_buf.bufferIdentifiers = &out_identifier;
            out_buf.bufSizes = &out_size;
            out_buf.bufElSizes = &out_element_size;

            if (AACENC_OK != aacEncEncode(aacenc, &in_buf, &out_buf, &in_args, &out_args))
            {
                printf("error: aac encode fail\n");
                exit(1);
            }
            sample += in_args.numInSamples;
            buf_pcm += in_args.numInSamples;
            total_samples -= in_args.numInSamples;
            ats = (uint64_t)sample * 90000 / AUDIO_RATE;

            if (MP4E_STATUS_OK != MP4E_put_sample(mux, audio_track_id, buf, out_args.numOutBytes, 1024 * 90000 / AUDIO_RATE, MP4E_SAMPLE_RANDOM_ACCESS))
            {
                printf("error: MP4E_put_sample failed\n");
                exit(1);
            }
        }
    }
    // unallocate for audio
    if (alloc_pcm)
        free(alloc_pcm);
    aacEncClose(&aacenc);

    // unallocate for video
    if (alloc_buf)
        free(alloc_buf);
    MP4E_close(mux);
    mp4_h26x_write_close(&mp4wr);
    if (fout)
        fclose(fout);
}