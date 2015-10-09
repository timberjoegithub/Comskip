/*
 * mpeg2dec.c
 * Copyright (C) 2000-2003 Michel Lespinasse <walken@zoy.org>
 * Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *
 * This file is part of mpeg2dec, a free MPEG-2 video stream decoder.
 * See http://libmpeg2.sourceforge.net/ for updates.
 *
 * mpeg2dec is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpeg2dec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "platform.h"
#include "vo.h"
#ifdef HAVE_SDL
#include <SDL.h>
#endif

#define SELFTEST

int pass = 0;
double test_pts = 0.0;

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

//#define restrict
//#include <libavcodec/ac3dec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>

#ifdef HARDWARE_DECODE
#include <ffmpeg.h>
const HWAccel hwaccels[] = {
#if HAVE_VDPAU_X11
    { "vdpau", vdpau_init, HWACCEL_VDPAU, AV_PIX_FMT_VDPAU },
#endif
#if HAVE_DXVA2_LIB
    { "dxva2", dxva2_init, HWACCEL_DXVA2, AV_PIX_FMT_DXVA2_VLD },
#endif
#if CONFIG_VDA
    { "vda",   vda_init,   HWACCEL_VDA,   AV_PIX_FMT_VDA },
#endif
    { 0 },
};

static InputStream inputs;
static InputStream *ist = &inputs;
#endif


extern int      hardware_decode;
int av_log_level=AV_LOG_INFO;


#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)
#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0
#define SAMPLE_CORRECTION_PERCENT_MAX 30
#define AUDIO_DIFF_AVG_NB 10
#define FF_ALLOC_EVENT   (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)
#define VIDEO_PICTURE_QUEUE_SIZE 1
#define DEFAULT_AV_SYNC_TYPE AV_SYNC_ADUIO_MASTER

typedef struct VideoPicture
{
    int width, height; /* source height & width */
    int allocated;
    double pts;
} VideoPicture;

typedef struct VideoState
{
    AVFormatContext *pFormatCtx;
    AVCodecContext *dec_ctx;
    int             videoStream, audioStream, subtitleStream;

    int             av_sync_type;
//     double          external_clock; /* external clock base */
//     int64_t         external_clock_time;
    int             seek_req;
    int             seek_by_bytes;
    int             seek_no_flush;
    double           seek_pts;
    int             seek_flags;
    int64_t          seek_pos;
    double          audio_clock;
    AVStream        *audio_st;
    AVStream        *subtitle_st;

    //DECLARE_ALIGNED(16, uint8_t, audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2]);
    unsigned int    audio_buf_size;
    unsigned int    audio_buf_index;
    AVPacket        audio_pkt;
    AVPacket        audio_pkt_temp;
//  uint8_t         *audio_pkt_data;
//  int             audio_pkt_size;
    int             audio_hw_buf_size;
    double          audio_diff_cum; /* used for AV difference average computation */
    double          audio_diff_avg_coef;
    double          audio_diff_threshold;
    int             audio_diff_avg_count;
    double          frame_timer;
    double          frame_last_pts;
    double          frame_last_delay;
    double          video_clock; ///<pts of last decoded frame / predicted pts of next decoded frame
    double          video_clock_submitted;
    double          video_current_pts; ///<current displayed pts (different from video_clock if frame fifos are used)
    int64_t         video_current_pts_time;  ///<time (av_gettime) at which we updated video_current_pts - used to have running video pts
    AVStream        *video_st;
    AVFrame         *pFrame;
    char            filename[1024];
    int             quit;
    AVFrame         *frame;
    double			 duration;
    double			 fps;
} VideoState;

VideoState      *is;

enum
{
    AV_SYNC_AUDIO_MASTER,
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_MASTER,
};


/* Since we only have one decoding thread, the Big Struct
   can be global in case we need it. */
VideoState *global_video_state;
AVPacket flush_pkt;


int64_t pev_best_effort_timestamp = 0;


int video_stream_index = -1;
int audio_stream_index = -1;
int width, height;
int have_frame_rate ;
int stream_index;


int64_t best_effort_timestamp;




#define USE_ASF 1


//#include "mpeg2convert.h"
#include "comskip.h"

extern int coding_type;

void InitComSkip(void);
void BuildCommListAsYouGo(void);
void ReviewResult(void);
int video_packet_process(VideoState *is,AVPacket *packet);



static FILE * in_file;
static FILE * sample_file;
static FILE * timing_file = 0;

extern int lastFrameCommCalculated;

extern int thread_count;
int is_AC3;
int AC3_rate;
int AC3_mode;
int is_h264=0;
int is_AAC=0;
extern unsigned int AC3_sampling_rate; //AC3
extern int AC3_byterate;
int demux_pid=0;
int demux_asf=0;
int last_pid;
#define PIDS	100
#define PID_MASK	0x1fff
int pids[PIDS];
int pid_type[PIDS];
int pid_pcr[PIDS];
int pid_pid[PIDS];
int top_pid_count[PID_MASK+1];
int top_pid_pid;
int pid;
int selected_video_pid=0;
int selected_audio_pid=0;
int selected_subtitle_pid=0;
int selection_restart_count = 0;
int found_pids=0;

int64_t pts;
double initial_pts;
int64_t final_pts;
double pts_offset = 0.0;

int initial_pts_set = 0;
double initial_apts;
double apts_offset = 0.0;
int initial_apts_set = 0;
int do_audio_repair = 1;
extern int timeline_repair;

//int bitrate;
int muxrate,byterate=10000;
//#define PTS_FRAME (double)(1.0 / get_fps())
//#define PTS_FRAME (int) (90000 / get_fps())
//#define SAMPLE_TO_FRAME 2.8125
//#define SAMPLE_TO_FRAME (90000.0/(get_fps() * 1000.0))

//#define BYTERATE	((int)(21400 * 25 / get_fps()))

#define   FSEEK    _fseeki64
#define   FTELL    _ftelli64
// The following two functions are undocumented and not included in any public header,
// so we need to declare them ourselves
extern int  _fseeki64(FILE *, int64_t, int);
extern int64_t _ftelli64(FILE *);

int soft_seeking=0;
extern char	inbasename[];

char pict_type;

char tempstring[512];
//test

#define DUMP_OPEN if (output_timing) { sprintf(tempstring, "%s.timing.csv", inbasename); timing_file = myfopen(tempstring, "w"); DUMP_HEADER }
#define DUMP_HEADER if (timing_file) fprintf(timing_file, "sep=,\ntype   ,real_pts, step        ,pts         ,clock       ,delta       ,offset, repeat\n");
#define DUMP_TIMING(T, D, P, C, O, S) if (timing_file && !csStepping && !csJumping && !csStartJump) fprintf(timing_file, "%7s, %12.3f, %12.3f, %12.3f, %12.3f, %12.3f, %12.3f, %d\n", \
	T, (double) (D), (double) calculated_delay, (double) (P), (double) (C), ((double) (P) - (double) (C)), (O), (S));
#define DUMP_CLOSE if (timing_file) { fclose(timing_file); timing_file = NULL; }


extern int skip_B_frames;
extern int lowres;

static int sigint = 0;

static int verbose = 0;

extern int selftest;
double selftest_target = 0.0;

extern int frame_count;
int	framenum;

fpos_t		filepos;
extern int		standoff;
int64_t			goppos,infopos,packpos,ptspos,headerpos,frompos,SeekPos;

extern int max_repair_size;
extern int variable_bitrate;
int max_internal_repair_size = 40;
int reviewing = 0;
int count=0;
int currentSecond=0;
int cur_hour = 0;
int cur_minute = 0;
int cur_second = 0;

extern char HomeDir[256];
extern int processCC;
int	reorderCC = 0;

extern int live_tv;
int csRestart;
int csStartJump;
int csStepping;
int csJumping;
int csFound;
int	seekIter = 0;
int	seekDirection = 0;

extern FILE * out_file;
extern uint8_t ccData[500];
extern int ccDataLen;

extern int height,width, videowidth;
extern int output_debugwindow;
extern int output_console;
extern int output_timing;
extern int output_srt;
extern int output_smi;

extern unsigned char *frame_ptr;
extern int lastFrameWasSceneChange;
extern int live_tv_retries;
extern int dvrms_live_tv_retries;
int retries;

extern void set_fps(double frame_delay, double dfps, int ticks, double rfps, double afps);
extern void dump_video (char *start, char *end);
extern void dump_audio (char *start, char *end);
extern void	Debug(int level, char* fmt, ...);
extern void dump_video_start(void);
extern void dump_audio_start(void);
void file_open();
int DetectCommercials(int, double);
int BuildMasterCommList(void);
FILE* LoadSettings(int argc, char ** argv);
void ProcessCCData(void);
void dump_data(char *start, int length);
void close_data();

static void signal_handler (int sig)
{
    sigint = 1;
    signal (sig, SIG_DFL);
    //return (RETSIGTYPE)0;
    return;
}



#define AUDIOBUFFER	800000

static double base_apts = 0.0, apts, top_apts = 0.0;
static DECLARE_ALIGNED(16, short, audio_buffer[AUDIOBUFFER]);
static short *audio_buffer_ptr = audio_buffer;
static int audio_samples = 0;
#define ISSAME(T1,T2) (fabs((T1) - (T2)) < 0.001)


static int sound_frame_counter = 0;
extern double get_fps();
extern int get_samplerate();
extern int get_channels();
extern void add_volumes(int *volumes, int nr_frames);
extern void set_frame_volume(uint32_t framenr, int volume);

extern double get_frame_pts(int f);



static int max_volume_found = 0;

int ms_audio_delay = 5;
int tracks_without_sound = 0;
int frames_without_sound = 0;
#define MAX_FRAMES_WITHOUT_SOUND	100
int frames_with_loud_sound = 0;



int retreive_frame_volume(double from_pts, double to_pts)
{
    short *buffer;
    int volume = -1;
    VideoState *is = global_video_state;
    int i;
    double calculated_delay;
    int s_per_frame = (to_pts - from_pts) * (double)(is->audio_st->codec->sample_rate+1);


    if (s_per_frame > 1 && base_apts <= from_pts && to_pts < top_apts )
    {
        calculated_delay = 0.0;


 //       Debug(1,"fame=%d, =base=%6.3f, from=%6.3f, samples=%d, to=%6.3f, top==%6.3f\n", -1, base_apts, from_pts, s_per_frame, to_pts, top_apts);
        buffer = & audio_buffer[(int)((from_pts - base_apts) * ((double)is->audio_st->codec->sample_rate+0.5) )];

        volume = 0;
        if (sample_file) fprintf(sample_file, "Frame %i\n", sound_frame_counter);
        for (i = 0; i < s_per_frame; i++)
        {
            if (sample_file) fprintf(sample_file, "%i\n", *buffer);
            volume += (*buffer>0 ? *buffer : - *buffer);
            buffer++;
        }
        volume = volume/s_per_frame;
        DUMP_TIMING("a  read", is->audio_clock, to_pts, from_pts, (double)volume, s_per_frame);

        audio_samples -= (int)((from_pts - base_apts) * (is->audio_st->codec->sample_rate+0.5)); // incomplete frame before complete frame
        audio_samples -= s_per_frame;


        if (volume == 0)
        {
            frames_without_sound++;
        }
        else if (volume > 20000)
        {
            if (volume > 256000)
                volume = 220000;
            frames_with_loud_sound++;
            volume = -1;
        }
        else
        {
            frames_without_sound = 0;
        }

        if (max_volume_found < volume)
            max_volume_found = volume;

        // Remove use samples
        audio_buffer_ptr = audio_buffer;
        if (audio_samples > 0)
        {
            for (i = 0; i < audio_samples; i++)
            {
                *audio_buffer_ptr++ = *buffer++;
            }
        }
        base_apts = to_pts;
        top_apts = base_apts + audio_samples / (double)(is->audio_st->codec->sample_rate);
        sound_frame_counter++;
    }
    return(volume);
}

void backfill_frame_volumes()
{
    int f;
    int volume;
    if (framenum < 3)
        return;
    f = framenum-2;
    while (get_frame_pts(f) + initial_pts> base_apts && f > 1) // Find first frame with samples available, could be incomplete
        f--;
    while (f < framenum-1 && (get_frame_pts(f+1) + initial_pts)<= top_apts && (top_apts - base_apts) > .2 /* && get_frame_pts(f-1) >= base_apts */) {
        volume = retreive_frame_volume(fmax(get_frame_pts(f) + initial_pts, base_apts), get_frame_pts(f+1) + initial_pts);
        if (volume > -1) set_frame_volume(f, volume);
        f++;
    }
}

int ALIGN_AC3_PACKETS=0;



void sound_to_frames(VideoState *is, short **b, int s, int c, int format)
{
    int i,l;
    int volume;
    double old_base_apts;
    static double old_audio_clock=0.0;
    double calculated_delay = 0.0;
    double avg_volume = 0.0;
    float *(fb[16]);
    short *(sb[16]);
    static int old_sample_rate = 0;

    audio_samples = (audio_buffer_ptr - audio_buffer);

    if (old_sample_rate == is->audio_st->codec->sample_rate &&
        ((audio_buffer_ptr - audio_buffer) < 0 || (audio_buffer_ptr - audio_buffer) >= AUDIOBUFFER
        || (top_apts - base_apts) * (is->audio_st->codec->sample_rate+0.5) > AUDIOBUFFER
        || (top_apts < base_apts)
        || !ISSAME(((double)audio_samples /(double)(is->audio_st->codec->sample_rate+0.5))+ base_apts, top_apts)
        || audio_samples < 0
        || audio_samples >= AUDIOBUFFER)) {
       Debug(1, "Panic: Audio buffering corrupt\n");
       audio_buffer_ptr = audio_buffer;
       top_apts = base_apts = 0;
       audio_samples=0;
       return;
    }
    if (old_sample_rate != 0 && old_sample_rate != is->audio_st->codec->sample_rate)
        Debug(1, "Audio samplerate switched from %d to %d\n", old_sample_rate, is->audio_st->codec->sample_rate );
    old_sample_rate = is->audio_st->codec->sample_rate;

    old_base_apts = base_apts;
    if (fabs(base_apts - (is->audio_clock - ((double)audio_samples /(double)(is->audio_st->codec->sample_rate))))> 0.0001)
        base_apts = (is->audio_clock - ((double)audio_samples /(double)(is->audio_st->codec->sample_rate)));
        if (ALIGN_AC3_PACKETS && is->audio_st->codec->codec_id == AV_CODEC_ID_AC3) {
                    if (   ISSAME(base_apts - old_base_apts, 0.032)
                        || ISSAME(base_apts - old_base_apts, -0.032)
                        || ISSAME(base_apts - old_base_apts, 0.064)
                        || ISSAME(base_apts - old_base_apts, -0.064)
                        || ISSAME(base_apts - old_base_apts, -0.096)
                        )
                        old_base_apts = base_apts; // Ignore AC3 packet jitter
            }
    if (old_base_apts != 0.0 && !ISSAME(base_apts, old_base_apts)) {
        Debug(1, "Jump in base apts from %6.5f to %6.5f, delta=%6.5f\n",old_base_apts, base_apts, base_apts -old_base_apts);
    }

    if (s+audio_samples > AUDIOBUFFER ) {
        Debug(1,"Panic: Audio buffer overflow, resetting audio buffer\n");
       audio_buffer_ptr = audio_buffer;
       top_apts = base_apts = 0;
       audio_samples=0;
       return;
    }

    if (s > 0)
    {
        if (format == AV_SAMPLE_FMT_FLTP)
        {
            for (l=0;l < is->audio_st->codec->channels;l++ )
            {
                fb[l] = (float*)b[l];
            }
            for (i = 0; i < s; i++)
            {
                volume = 0;
                for (l=0;l < is->audio_st->codec->channels;l++ ) volume += *((fb[l])++) * 64000;
                *audio_buffer_ptr++ = volume / is->audio_st->codec->channels;
                avg_volume += abs(volume / is->audio_st->codec->channels);
            }
        }
        else
        {
            for (l=0;l < is->audio_st->codec->channels;l++ )
            {
                sb[l] = (short*)b[l];
            }
            for (i = 0; i < s; i++)
            {
                volume = 0;
                for (l=0;l < is->audio_st->codec->channels;l++ ) volume += *((sb[l])++);
                *audio_buffer_ptr++ = volume / is->audio_st->codec->channels;
                avg_volume += abs(volume / is->audio_st->codec->channels);
            }
        }
    }
    avg_volume /= s;
    audio_samples = (audio_buffer_ptr - audio_buffer);
    top_apts = base_apts + audio_samples / (double)(is->audio_st->codec->sample_rate);

    calculated_delay = is->audio_clock - old_audio_clock;
    DUMP_TIMING("a frame", is->audio_clock, top_apts, base_apts, avg_volume,s);
    old_audio_clock = is->audio_clock;

    backfill_frame_volumes();
}


#define AC3_BUFFER_SIZE 100000
static uint8_t ac3_packet[AC3_BUFFER_SIZE];
static int ac3_packet_index = 0;
int data_size;

int ac3_package_misalignment_count = 0;

void audio_packet_process(VideoState *is, AVPacket *pkt)
{
    int prev_codec_id = -1;
    int len1, data_size;
    uint8_t *pp;
    double prev_audio_clock;
//    AC3DecodeContext *s = is->audio_st->codec->priv_data;
    int      rps,ps;
    AVPacket *pkt_temp = &is->audio_pkt_temp;

    int got_frame;
    if (!reviewing)
    {
        dump_audio_start();
        dump_audio((char *)pkt->data,(char *) (pkt->data + pkt->size));
    }


    pkt_temp->data = pkt->data;
    pkt_temp->size = pkt->size;

    if ( !ALIGN_AC3_PACKETS && is->audio_st->codec->codec_id == AV_CODEC_ID_AC3
        && ((pkt_temp->data[0] != 0x0b || pkt_temp->data[1] != 0x77)))
    {
//        Debug(1, "AC3 packet misaligned, audio decoding will fail\n");
        ac3_package_misalignment_count++;
    } else {
        ac3_package_misalignment_count = 0;
    }
    if (!ALIGN_AC3_PACKETS && ac3_package_misalignment_count > 4) {
        Debug(1, "AC3 packets misaligned, enabling AC3 re-alignment\n");
        ALIGN_AC3_PACKETS = 1;
    }

    if (ALIGN_AC3_PACKETS && is->audio_st->codec->codec_id == AV_CODEC_ID_AC3) {
        if (ac3_packet_index + pkt_temp->size >= AC3_BUFFER_SIZE )
        {
            Debug(1,"AC3 sync error\n");
            return;
        }
        memcpy(&ac3_packet[ac3_packet_index], pkt_temp->data, pkt_temp->size);
        pkt_temp->data = ac3_packet;
        pkt_temp->size += ac3_packet_index;
        ac3_packet_index = pkt_temp->size;
        ps = 0;
        while (pkt_temp->size >= 2 && (pkt_temp->data[0] != 0x0b || pkt_temp->data[1] != 0x77) ) {
            pkt_temp->data++;
            pkt_temp->size--;
            ps++;
        }
        if (pkt_temp->size < 2)
            return; // No packet start found
        if (ps>0)
            Debug(1,"Skipped %d of added %d bytes in audio input stream around frame %d\n", ps, pkt->size, framenum);
        pp = pkt_temp->data;
        rps = pkt_temp->size-2;
        while (rps > 1 && (pp[rps] != 0x0b || pp[rps+1] != 0x77) ) {
            rps--;
        }
        if (rps >= 2)
        {
            pkt_temp->size = rps;
        }
        else
        {
            // No complete packet found;
            rps = pkt_temp->size;
            pp = &pkt_temp->data[0];
            pkt_temp->size = 0;
            return;
        }
        if ( (pkt_temp->size % 768 ) != 0)
            Debug(1,"Strange packet size of %d bytes in audio input stream around frame %d\n", rps, framenum);

    }

    /*  Try to align on packet boundary as some demuxers don't do that, in particular dvr-ms */




    if (pkt->pts != AV_NOPTS_VALUE)
    {
        prev_audio_clock = is->audio_clock;
        is->audio_clock = av_q2d(is->audio_st->time_base)*( pkt->pts -  (is->video_st->start_time != AV_NOPTS_VALUE ? is->video_st->start_time : 0)) - apts_offset;
            if (ALIGN_AC3_PACKETS && is->audio_st->codec->codec_id == AV_CODEC_ID_AC3) {
                    if (   ISSAME(is->audio_clock - prev_audio_clock, 0.032)
                        || ISSAME(is->audio_clock - prev_audio_clock, -0.032)
                        || ISSAME(is->audio_clock - prev_audio_clock, 0.064)
                        || ISSAME(is->audio_clock - prev_audio_clock, -0.064)
                        || ISSAME(is->audio_clock - prev_audio_clock, -0.096)
                        )
                        prev_audio_clock = is->audio_clock; // Ignore AC3 packet jitter
            }

        if ( initial_apts_set && is->audio_clock != 0.0 && fabs( is->audio_clock - prev_audio_clock) > 0.02) {
            if (do_audio_repair && fabs( is->audio_clock - prev_audio_clock) < 1) {
                 is->audio_clock = prev_audio_clock; //Ignore small jitter
            }
            else {
                Debug(1 ,"Strange audio pts step of %6.5f instead of %6.5f at frame %d\n", (is->audio_clock - prev_audio_clock)+0.0005, 0.0 , framenum);
                if (do_audio_repair) {
//                    apts_offset += is->audio_clock - prev_audio_clock ;
//                    is->audio_clock = prev_audio_clock;
                }
            }
        }
    }

    initial_apts_set = 1;

    //		fprintf(stderr, "sac = %f\n", is->audio_clock);
    while(pkt_temp->size > 0)
    {
 //       data_size = STORAGE_SIZE;

//        if (!is->frame)
//        {
            if (!(is->frame = av_frame_alloc()))
                return;
 //       }
 //       else
 //           avcodec_get_frame_defaults(is->frame);

        len1 = avcodec_decode_audio4(is->audio_st->codec, is->frame, &got_frame, pkt_temp);

        if (prev_codec_id != -1 && (unsigned int)prev_codec_id != is->audio_st->codec->codec_id)
        {
            Debug(2 ,"Audio format change\n");
        }
        prev_codec_id = is->audio_st->codec->codec_id;
        if (len1 < 0  && !ALIGN_AC3_PACKETS)
        {
            /* if error, we skip the frame */
            pkt_temp->size = 0;
            if (is->audio_st->codec->codec_id == AV_CODEC_ID_AC3) ac3_packet_index = 0;

            break;
        }
        if (len1 < 0  && ALIGN_AC3_PACKETS)
        {
            len1 = 2; // Skip over packet start
            pkt_temp->data += len1;
            pkt_temp->size -= len1;
            break;
        }
        pkt_temp->data += len1;
        pkt_temp->size -= len1;
        if (!got_frame)
        {
            /* stop sending empty packets if the decoder is finished */
            continue;
        }



        data_size = av_samples_get_buffer_size(NULL, is->frame->channels,
                                               is->frame->nb_samples,
                                               is->frame->format, 1);
        if (data_size > 0)
        {
            sound_to_frames(is, (short **)is->frame->data, is->frame->nb_samples ,is->frame->channels, is->frame->format);
        }
        is->audio_clock += (double)data_size /
                           (is->frame->channels * is->frame->sample_rate * av_get_bytes_per_sample(is->frame->format));
        av_frame_free(&(is->frame));
    }

    if (ALIGN_AC3_PACKETS && is->audio_st->codec->codec_id == AV_CODEC_ID_AC3) {
        ps = 0;
        rps = (pkt_temp->data - ac3_packet);
        while (0 < ac3_packet_index - rps)
        {
            ac3_packet[ps] = ac3_packet[rps];
            ps++;
            rps++;
        }
        ac3_packet_index = ps;
    }
}

static double print_fps (int final)
{
    static uint32_t frame_counter = 0;
    static struct timeval tv_beg, tv_start;
    static int total_elapsed;
    static int last_count = 0;
    struct timeval tv_end;
    double fps, tfps;
    int frames, elapsed;
    char cur_pos[100] = "0:00:00";

    if (verbose)
        return 0.0;

    if(csStepping)
        return 0.0;

    if(final < 0)
    {
        frame_counter = 0;
        last_count = 0;
        return 0.0;
    }
#ifdef DONATOR
#else
#ifndef DEBUG
again:
#endif
#endif
    gettimeofday (&tv_end, NULL);

    if (!frame_counter)
    {
        tv_start = tv_beg = tv_end;
        signal (SIGINT, signal_handler);
    }

    elapsed = (tv_end.tv_sec - tv_beg.tv_sec) * 100 + (tv_end.tv_usec - tv_beg.tv_usec) / 10000;
    total_elapsed = (tv_end.tv_sec - tv_start.tv_sec) * 100 + (tv_end.tv_usec - tv_start.tv_usec) / 10000;

    if (final)
    {
        if (total_elapsed)
            tfps = frame_counter * 100.0 / total_elapsed;
        else
            tfps = 0;

        fprintf (stderr,"\n%d frames decoded in %.2f seconds (%.2f fps)\n",
                 frame_counter, total_elapsed / 100.0, tfps);
        fflush(stderr);
        return tfps;
    }

    frame_counter++;

    frames = frame_counter - last_count;

#ifdef DONATOR
#else
#ifndef DEBUG
    if (is_h264 && frames > 15 &&  elapsed < 100)
    {
        Sleep(100L);
        goto again;
    }
#endif
#endif

    if (elapsed < 100)	/* only display every 1.00 seconds */
        return 0.0;

    tv_beg = tv_end;

//    cur_second = (int)(get_frame_pts(framenum));
    cur_second = (int)((framenum)/get_fps());
    cur_hour = cur_second / (60 * 60);
    cur_second -= cur_hour * 60 * 60;
    cur_minute = cur_second / 60;
    cur_second -= cur_minute * 60;


    sprintf(cur_pos, "%2i:%.2i:%.2i", cur_hour, cur_minute, cur_second);

    fps = frames * 100.0 / elapsed;
    tfps = frame_counter * 100.0 / total_elapsed;

    fprintf (stderr, "%s - %d frames in %.2f sec(%.2f fps), "
             "%.2f sec(%.2f fps), %d%%\r", cur_pos, frame_counter,
//             total_elapsed / 100.0, tfps, elapsed / 100.0, fps, (int) (100.0 * get_frame_pts(framenum) / global_video_state->duration));
             total_elapsed / 100.0, tfps, elapsed / 100.0, fps, (int) (100.0 * (framenum)/get_fps() / global_video_state->duration));
    fflush(stderr);
    last_count = frame_counter;
    return tfps;
}

#ifdef PROCESS_CC
void CEW_reinit();
long process_block (unsigned char *data, long length);
#endif


int SubmitFrame(AVStream        *video_st, AVFrame         *pFrame , double pts)
{
    int res=0;
    int changed = 0;

//	bitrate = pFrame->bit_rate;
    if (pFrame->linesize[0] > 2000 || pFrame->height > 1200 || pFrame->linesize[0] < 100 || pFrame->height < 100)
    {
        //				printf("Panic: illegal height, width or frame period\n");
        frame_ptr = NULL;
        return(0);
    }
    if (height != pFrame->height && pFrame->height > 100 && pFrame->height < 2000)
    {
        height= pFrame->height;
        changed = 1;
    }
    if (width != pFrame->linesize[0] && pFrame->linesize[0] > 100 && pFrame->linesize[0]  < 2000)
    {
        width= pFrame->linesize[0];
        changed = 1;
    }
    if (videowidth != pFrame->width && pFrame->width > 100 && pFrame->width < 2000)
    {
        videowidth= pFrame->width;
        changed = 1;
    }
    if (changed) Debug(2, "Format changed to [%d : %d]\n", videowidth, height);
    infopos = headerpos;
    frame_ptr = pFrame->data[0];
    if (frame_ptr == NULL)
    {
        return(0);; // return; // exit(2);
    }

    if (pFrame->pict_type == AV_PICTURE_TYPE_B)
        pict_type = 'B';
    else if (pFrame->pict_type == AV_PICTURE_TYPE_I)
        pict_type = 'I';
    else
        pict_type = 'P';

    if (selftest == 2 && framenum == 0 && pass == 0 && test_pts == 0.0)
        test_pts = pts;
    if (selftest == 2 && pass > 0)
    {
        if (test_pts != pts)
        {
               sample_file = fopen("seektest.log", "a+");
                fprintf(sample_file, "Reset file Failed, initial pts = %6.3f, seek pts = %6.3f, pass = %d, \"%s\"\n", test_pts, pts, pass+1, is->filename);
                fclose(sample_file);
        }
        exit(1);
    }

    if (!reviewing)
    {

        print_fps (0);
        res = DetectCommercials((int)framenum, pts);
        framenum++;
#ifdef SELFTEST
    if (selftest == 2 && pass == 0 && framenum > 20)
    {
        res = true;
        pass++;
    }
#endif
        if (res) {
            framenum = 0;
            sound_frame_counter = 0;
            is->seek_req = 1;
            is->seek_pos = 0;
            is->seek_pts = 0.0;
        }
    }
    return (res);
}

void Set_seek(VideoState *is, double pts, double length)
{
    AVFormatContext *ic = is->pFormatCtx;

    is->seek_flags = AVSEEK_FLAG_ANY;
    is->seek_flags = AVSEEK_FLAG_BACKWARD;
    is->seek_req = true;
    is->seek_pts = pts;
    pts = fmax(0,pts+initial_pts);
#ifdef DEBUG
    printf("Seek to %8.2f\n", pts);
#endif // DEBUG

#define MAX_GOP_SIZE 2.0
    pts = fmax(0.0,pts-MAX_GOP_SIZE);

    if (is->seek_by_bytes)
    {
//                            pos = avio_tell(is->pFormatCtx->pb);
      uint64_t size =  avio_size(ic->pb);
        is->seek_pos = size*fmax(0,pts-4.0)/length;
        is->seek_flags |= AVSEEK_FLAG_BYTE;
    } else {
        is->seek_pos = pts / av_q2d(is->video_st->time_base);
        if (is->video_st->start_time != AV_NOPTS_VALUE)
        {
//            is->seek_pos += is->video_st->start_time;
        }
    }
}

void DoSeekRequest(VideoState *is)
{
    int ret;
again:
//           ret = avformat_seek_file(is->pFormatCtx, is->videoStream, INT64_MIN, is->seek_pos, INT64_MAX, is->seek_flags);
    ret = av_seek_frame(is->pFormatCtx, is->videoStream,  is->seek_pos,  is->seek_flags);
//            ret = av_seek_frame(is->pFormatCtx, -1,  is->seek_pos,  is->seek_flags);
    pev_best_effort_timestamp = 0;
    best_effort_timestamp = 0;
    is->video_clock = 0.0;
    is->audio_clock = 0.0;
    if(ret < 0)
    {
        char *error_text;
        if (is->pFormatCtx->iformat->read_seek)
        {
            error_text = "Format specific";
        }
        else if(is->pFormatCtx->iformat->read_timestamp)
        {
            error_text = "Frame binary";
        }
        else
        {
            error_text = "Generic";
        }

        fprintf(stderr, "%s error while seeking. target=%6.3f, \"%s\"\n", error_text,is->seek_pts, is->pFormatCtx->filename);

        if (!is->seek_by_bytes)
        {
            is->seek_by_bytes = 1; // Fall back to byte seek
            Set_seek(is, is->seek_pts, is->duration);
            goto again;
        }

        if (selftest)
        {
            sample_file = fopen("seektest.log", "a+");
            fprintf(sample_file, "%s error while seeking, target=%6.3f, \"%s\"\n", error_text,is->seek_pts, is->pFormatCtx->filename);
            fclose(sample_file);
        }
    }
    if (!is->seek_no_flush)
    {
        if(is->audioStream >= 0)
        {
            avcodec_flush_buffers(is->audio_st->codec);
        }
        if(is->videoStream >= 0)
        {
            avcodec_flush_buffers(is->video_st->codec);
        }
    }
    is->seek_no_flush = 0;
}

void DecodeOnePicture(FILE * f, double pts, double length)
{
    VideoState *is = global_video_state;
    AVPacket *packet;
//    int ret;

//    int64_t pack_pts=0, comp_pts=0, pack_duration=0;

    file_open();
    is = global_video_state;

    reviewing = 1;
    Set_seek(is, pts, length);

    pev_best_effort_timestamp = 0;
    best_effort_timestamp = 0;
    pts_offset = 0.0;

//     Debug ( 5,  "Seek to %f\n", pts);
    frame_ptr = NULL;
    packet = &(is->audio_pkt);

    for(;;)
    {
        if(is->quit)
        {
            break;
        }
        // seek stuff goes here
        if(is->seek_req)
        {
again:      DoSeekRequest(is);
        }
nextpacket:
        if(av_read_frame(is->pFormatCtx, packet) < 0)
        {
            break;
        }
        if (is->seek_req) {
                double packet_time = (packet->pts - (is->video_st->start_time != AV_NOPTS_VALUE ? is->video_st->start_time : 0)) * av_q2d(is->video_st->time_base);
            if (packet->pts==AV_NOPTS_VALUE) {
                av_free_packet(packet);
                goto nextpacket;
            }
            if (is->seek_req < 6 && (is->seek_flags & AVSEEK_FLAG_BYTE) &&  fabs(packet_time - (is->seek_pts - 2.5) ) < is->duration / (10 * is->seek_req)) {
                is->seek_pos += ((is->seek_pts - 2.5 - packet_time) / is->duration ) * avio_size(is->pFormatCtx->pb) * 1.1;
                is->seek_req++;
                goto again;
            }
            is->seek_req = 0;
        }
        is->seek_req = 0;

        if(packet->stream_index == is->videoStream)
        {
/*
            if (packet->pts != AV_NOPTS_VALUE)
                comp_pts = packet->pts;
            pack_pts = comp_pts; // av_rescale_q(comp_pts, is->video_st->time_base, AV_TIME_BASE_Q);
            pack_duration = packet->duration; //av_rescale_q(packet->duration, is->video_st->time_base, AV_TIME_BASE_Q);
            comp_pts += packet->duration;
 */
 //           pass = 0;
            retries = 1; // once a frame has been decoded this will be set to zero
            if (video_packet_process(is, packet) )
            {

                if (retries == 0) // A frame has been decoded so stop reading packets.
                {
#ifdef DEBUG
    printf("Seek landed at %8.2f\n", is->video_clock);
#endif // DEBUG

                    av_free_packet(packet);
                    break;
                }
/*
                double frame_delay = av_q2d(is->video_st->codec->time_base)* is->video_st->codec->ticks_per_frame;         // <------------------------ frame delay is the time in seconds till the next frame
                if (is->video_clock - is->seek_pts > -frame_delay / 2.0)
                {
                    av_free_packet(packet);
                    break;
                }
                if (is->video_clock + (pack_duration * av_q2d(is->video_st->time_base)) >= is->seek_pts)
                {
                    av_free_packet(packet);
                    break;
                }
 */
            }
        }
        else if(packet->stream_index == is->audioStream)
        {
            // audio_packet_process(is, packet);
        }
        else
        {
            // Do nothing
        }
        av_free_packet(packet);
    }
    reviewing = 0;
}

void raise_exception(void)
{
#ifdef _WIN32
    *(int *)0 = 0;
#elif defined(__amd64__) || defined(__i386__)
    __asm__("int3");
#endif
}



int filter(void)
{
    printf("Exception raised, Comskip is terminating\n");
    exit(99);
}

extern char					mpegfilename[];



int video_packet_process(VideoState *is,AVPacket *packet)
{
    double frame_delay;
    int len1, frameFinished;
    int repeat;
    double pts;
//    double dts;
    double real_pts;
static int find_29fps = 0;
static int force_29fps = 0;
static int find_25fps = 0;
static int force_25fps = 0;
static int find_24fps = 0;
static int force_24fps = 0;
static double prev_pts = 0.0;
static double prev_real_pts = 0.0;
static double prev_strange_step = 0.0;
static int    prev_strange_framenum = 0;
//static double prev_frame_delay = 0.0;

    double calculated_delay;

    if (!reviewing)
    {
        dump_video_start();
        dump_video((char *)packet->data,(char *) (packet->data + packet->size));
    }
    real_pts = 0.0;
    pts = 0;
    //        is->video_st->codec->flags |= CODEC_FLAG_GRAY;
    // Decode video frame
    len1 = avcodec_decode_video2(is->video_st->codec, is->pFrame, &frameFinished,
                                 packet);

    if (len1<0)
    {
        if (len1 == -1 && thread_count > 1)
        {
            InitComSkip();
            thread_count = 1;
            is->seek_req = 1;
            is->seek_pos = 0;
            is->seek_pts = 0.0;
            pev_best_effort_timestamp = 0;
            best_effort_timestamp = 0;
            framenum = 1;
            Debug(1 ,"Restarting processing in single thread mode because frame size is changing \n");
            goto quit;
        }
    }

    // Did we get a video frame?
    if(frameFinished)
    {


        frame_delay = av_q2d(is->video_st->codec->time_base) * is->video_st->codec->ticks_per_frame ;
        repeat = av_stream_get_parser(is->video_st) ? av_stream_get_parser(is->video_st)->repeat_pict: 4;

 //       if (prev_frame_delay != 0.0 && frame_delay != prev_frame_delay)
 //           Debug(1, "Changing fps from %6.3f to %6.3f", 1.0/prev_frame_delay, 1.0/frame_delay);
        pev_best_effort_timestamp = best_effort_timestamp;
        best_effort_timestamp = av_frame_get_best_effort_timestamp(is->pFrame);
        calculated_delay = (best_effort_timestamp - pev_best_effort_timestamp) * av_q2d(is->video_st->time_base);

        if (best_effort_timestamp == AV_NOPTS_VALUE)
            real_pts = 0;
        else
        {
            headerpos = avio_tell(is->pFormatCtx->pb);
            if (initial_pts_set < 3 && !reviewing)
            {
                if (!ISSAME(initial_pts, av_q2d(is->video_st->time_base)* (best_effort_timestamp - (frame_delay * framenum) / av_q2d(is->video_st->time_base) - (is->video_st->start_time != AV_NOPTS_VALUE ? is->video_st->start_time : 0)))) {
                    initial_pts = (best_effort_timestamp  - (is->video_st->start_time != AV_NOPTS_VALUE ? is->video_st->start_time : 0)) * av_q2d(is->video_st->time_base) - (frame_delay * framenum);
                    Debug( 10,"\nInitial pts = %10.3f\n", initial_pts);
                    if (timeline_repair<2)
                        initial_pts = 0.0;
                }
                initial_pts_set++;
                final_pts = 0;
                pts_offset = 0.0;

            }
            real_pts = av_q2d(is->video_st->time_base)* ( best_effort_timestamp - (is->video_st->start_time != AV_NOPTS_VALUE ? is->video_st->start_time : 0))  - initial_pts;
            final_pts = best_effort_timestamp -  (is->video_st->start_time != AV_NOPTS_VALUE ? is->video_st->start_time : 0);
        }

//        dts =  av_q2d(is->video_st->time_base)* ( is->pFrame->pkt_dts - (is->video_st->start_time != AV_NOPTS_VALUE ? is->video_st->start_time : 0)) ;


        calculated_delay = real_pts - prev_real_pts;

        if (framenum < 500)
        {
            if ( (!(fabs(frame_delay - 0.03336666) < 0.001 )) &&
            ((fabs(calculated_delay - 0.0333333) < 0.0001) || (fabs(calculated_delay - 0.033) < 0.0001) || (fabs(calculated_delay - 0.034) < 0.0001) ||
            (fabs(calculated_delay - 0.067) < 0.0001)     || (fabs(calculated_delay - 0.066) < 0.0001) || (fabs(calculated_delay - 0.06673332) < 0.0001)  ) )
            {
                find_29fps++;
            }
            else
                find_29fps = 0;
            if (force_29fps == 0 && find_29fps == 5)
            {
                force_29fps = 1;
                force_25fps = 0;
                force_24fps = 0;
                Debug(1 ,"Framerate forced to 29.97fps at frame %d\n", frame_count);
            }

            if ( (!(fabs(frame_delay - 0.040) < 0.001 )) &&
                    (((fabs(calculated_delay - 0.04) < 0.0001)) || (fabs(calculated_delay - 0.039) < 0.0001) || (fabs(calculated_delay - 0.041) < 0.0001)) )
            {
                find_25fps++;
            }
            else
                find_25fps = 0;
            if (force_25fps == 0 && find_25fps == 5)
            {
                force_29fps = 0;
                force_25fps = 1;
                force_24fps = 0;
                Debug(1 ,"Framerate forced to 25.00fps at frame %d\n", frame_count);
            }

            if ( ((find_24fps & 1 ) == 0 && (fabs(calculated_delay - 0.050) < 0.001 )) ||
                    ((find_24fps & 1 ) == 1 && (fabs(calculated_delay - 0.033) < 0.001 ))
               )
            {
                find_24fps++;
            }
            else
                find_24fps = 0;
            if (force_24fps == 0 && find_24fps == 5)
            {
                force_29fps = 0;
                force_25fps = 0;
                force_24fps = 1;
                Debug(1 ,"Framerate forced to 24.00fps at frame %d\n", frame_count);
            }
        }

        if (force_29fps)
        {
            frame_delay=0.033366666666666669;
        }
        if (force_25fps)
        {
            frame_delay=0.04;
        }
        if (force_24fps)
        {
            frame_delay=0.0416666666666667;
        }

//#define SHOW_VIDEO_TIMING
#ifdef SHOW_VIDEO_TIMING
        if (framenum==0)
            Debug(1,"Video timing ---------------------------------------------------\n", frame_delay/is->video_st->codec->ticks_per_frame, is->video_st->codec->ticks_per_frame, repeat, real_pts,calculated_delay);
        else if (framenum<20)
            Debug(1,"Video timing fr=%6.5f, tick=%d, repeat=%d, pts=%6.3f, step=%6.5f\n", frame_delay/is->video_st->codec->ticks_per_frame, is->video_st->codec->ticks_per_frame, repeat, real_pts,calculated_delay);
#endif // SHOW_VIDEO_TIMING


        pts_offset *= 0.9;
        if (!reviewing && timeline_repair) {
            if (framenum > 1 && fabs(calculated_delay - pts_offset - frame_delay) < 1.0) { // Allow max 0.5 second timeline jitter to be compensated
                if (!ISSAME(3*frame_delay/ is->video_st->codec->ticks_per_frame, calculated_delay))
                    if (!ISSAME(1*frame_delay/ is->video_st->codec->ticks_per_frame, calculated_delay))
                        pts_offset = pts_offset + frame_delay - calculated_delay;
            }
        }
        else
            do_audio_repair = 0;

//		Debug(0 ,"pst[%3d] = %12.3f, inter = %d, ticks = %d\n", framenum, pts/frame_delay, is->pFrame->interlaced_frame, is->video_st->codec->ticks_per_frame);

        pts = real_pts + pts_offset;

        calculated_delay = pts - prev_pts;

        if (!reviewing
            && framenum > 1 && fabs(calculated_delay - frame_delay) > 0.01
            && !ISSAME(3*frame_delay/ is->video_st->codec->ticks_per_frame, calculated_delay)
            && !ISSAME(1*frame_delay/ is->video_st->codec->ticks_per_frame, calculated_delay)
            ){
            if ( (prev_strange_framenum + 1 != framenum) &&( prev_strange_step < fabs(calculated_delay - frame_delay))) {
                Debug(1 ,"Strange video pts step of %6.5f instead of %6.5f at frame %d\n", calculated_delay+0.0005, frame_delay+0.0005, framenum); // Unknown strange step
                if (calculated_delay < -0.5)
                    do_audio_repair = 0;        // Disable audio repair with messed up video timeline
            }
            prev_strange_framenum = framenum;
            prev_strange_step = fabs(calculated_delay - frame_delay);
        }

        set_fps(frame_delay, is->fps, repeat, av_q2d(is->video_st->r_frame_rate),  av_q2d(is->video_st->avg_frame_rate));

        if(pts != 0)
        {
            is->video_clock = pts;
            DUMP_TIMING("v   set", real_pts, pts, is->video_clock, pts_offset, repeat);
        }
        else
        {
            /* if we aren't given a pts, set it to the clock */
            DUMP_TIMING("v clock", real_pts, pts, is->video_clock, pts_offset, repeat);
            pts = is->video_clock;
        }
        is->video_clock_submitted = is->video_clock;

#ifdef HARDWARE_DECODE
        if (ist->hwaccel_retrieve_data && is->pFrame->format == ist->hwaccel_pix_fmt) {
            if (ist->hwaccel_retrieve_data(ist->dec_ctx, is->pFrame) < 0)
                goto quit;
        }
        ist->hwaccel_retrieved_pix_fmt = is->pFrame->format;
#endif




        if (retries == 0)
        {
            if (is->video_clock - is->seek_pts > -frame_delay / 2.0)
            {

#ifdef SELFTEST
                if (selftest == 1 && pass == 1 /*&& framenum > 501 && is->video_clock > 0 */)
                {
                   if (is->video_clock < selftest_target - 0.05 || is->video_clock > selftest_target + 0.05)
                   {
                    sample_file = fopen("seektest.log", "a+");
                    fprintf(sample_file, "Seek error: target=%8.1f, result=%8.1f, error=%6.3f, size=%8.1f, mode=%s, \"%s\"\n",
                            is->seek_pts,
                            is->video_clock,
                            is->video_clock - is->seek_pts,
                            is->duration,
                            (is->seek_by_bytes ? "byteseek": "timeseek" ),
                            is->filename);
                    fclose(sample_file);
                   }
                    /*
                                    if (tries ==  0 && fabs((double) av_q2d(is->video_st->time_base)* ((double)(packet->pts - is->video_st->start_time - is->seek_pos ))) > 2.0) {
                     				   is->seek_req=1;
                    				   is->seek_pos = 20.0 / av_q2d(is->video_st->time_base);
                    				   is->seek_flags = AVSEEK_FLAG_BYTE;
                    				   tries++;
                                   } else
                     */
                    selftest = 3;
                    pass = 0;
//                    exit(1);
                }
#endif
                if (SubmitFrame (is->video_st, is->pFrame, is->video_clock))
                {
                    goto quit;
                }
            }
        }
        else {
            if (is->video_clock - is->seek_pts > -frame_delay / 2.0)
            {
                if (selftest == 3)
                {
                    if (is->video_clock < selftest_target - 0.05 || is->video_clock > selftest_target + 0.05)
                    {
                        sample_file = fopen("seektest.log", "a+");
                        fprintf(sample_file, "Reopen error: target=%8.1f, result=%8.1f, error=%6.3f, size=%8.1f, mode=%s, \"%s\"\n",
                            is->seek_pts,
                            is->video_clock,
                            is->video_clock - is->seek_pts,
                            is->duration,
                            (is->seek_by_bytes ? "byteseek": "timeseek" ),
                            is->filename);
                        fclose(sample_file);
                    }
                    exit(1);
                }
                retries = 0;
                if (SubmitFrame (is->video_st, is->pFrame, is->video_clock))
                {
                    goto quit;
                }
            } else {
                if (fabs(is->seek_pts - is->video_clock) > 8 ) {
                    Debug(1,"Positioning file failing with pts=%6.2f\n", is->video_clock );
                    if (selftest == 1 || selftest == 3)
                    {
                        sample_file = fopen("seektest.log", "a+");
                        fprintf(sample_file, "To early error: target=%8.1f, result=%8.1f, error=%6.3f, size=%8.1f, mode=%s, \"%s\"\n",
                            is->seek_pts,
                            is->video_clock,
                            is->video_clock - is->seek_pts,
                            is->duration,
                            (is->seek_by_bytes ? "byteseek": "timeseek" ),
                            is->filename);
                        fclose(sample_file);
                        exit(1);
                    }
                    retries = 0;
                }
            }
//            if (selftest == 4) exit(1);
        }
        /* update the video clock */
        is->video_clock += frame_delay;
        prev_pts = pts;
        prev_real_pts = real_pts;
//        prev_frame_delay = frame_delay;

#ifdef PROCESS_CC
        if (is->pFrame->nb_side_data) {
            int i;
            for (i = 0; i < is->pFrame->nb_side_data; i++) {
                AVFrameSideData *sd = is->pFrame->side_data[i];
                if (sd->type != AV_FRAME_DATA_A53_CC) continue;
                ccDataLen = sd->size + 7;
                ccData[0] = 'G';
                ccData[1] = 'A';
                ccData[2] = '9';
                ccData[3] = '4';
                ccData[4] = 3;
                ccData[5] = sd->size / 3 + 64;
                for (i=0; i<sd->size; i++) {
                  ccData[i+7] = sd->data[i];
                }
                dump_data((char *)ccData, (int)ccDataLen);
                if (processCC) ProcessCCData();
                if (output_srt) process_block(ccData, (int)ccDataLen);
            }
        }
#endif

        return 1;
    }
quit:
    return 0;
}


//extern int dxva2_init(AVCodecContext *s);

#ifdef HARDWARE_DECODE
static const HWAccel *get_hwaccel(enum AVPixelFormat pix_fmt)
{
    int i;
    for (i = 0; hwaccels[i].name; i++)
        if (hwaccels[i].pix_fmt == pix_fmt)
            return &hwaccels[i];
    return NULL;
}

static enum AVPixelFormat get_format(AVCodecContext *s, const enum AVPixelFormat *pix_fmts)
{
    InputStream *ist = s->opaque;
    const enum AVPixelFormat *p;
    int ret;

    for (p = pix_fmts; *p != -1; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        const HWAccel *hwaccel;

        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            break;

        hwaccel = get_hwaccel(*p);
        if (!hwaccel ||
            (ist->active_hwaccel_id && ist->active_hwaccel_id != hwaccel->id) ||
            (ist->hwaccel_id != HWACCEL_AUTO && ist->hwaccel_id != hwaccel->id))
            continue;

        ret = hwaccel->init(s);
        if (ret < 0) {
            if (ist->hwaccel_id == hwaccel->id) {
                av_log(NULL, AV_LOG_FATAL,
                       "%s hwaccel requested for input stream #%d:%d, "
                       "but cannot be initialized.\n", hwaccel->name,
                       ist->file_index, ist->st->index);
                return AV_PIX_FMT_NONE;
            }
            continue;
        }
        ist->active_hwaccel_id = hwaccel->id;
        ist->hwaccel_pix_fmt   = *p;
        break;
    }

    return *p;
}

static int get_buffer(AVCodecContext *s, AVFrame *frame, int flags)
{
    InputStream *ist = s->opaque;

    if (ist->hwaccel_get_buffer && frame->format == ist->hwaccel_pix_fmt)
        return ist->hwaccel_get_buffer(s, frame, flags);

    return avcodec_default_get_buffer2(s, frame, flags);
}
#endif

int stream_component_open(VideoState *is, int stream_index)
{

    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecContext *codecCtx;
    AVCodec *codec;


    if(stream_index < 0 || (unsigned int)stream_index >= pFormatCtx->nb_streams)
    {
        return -1;
    }

    if (strcmp(pFormatCtx->iformat->name, "mpegts")==0)
        demux_pid = 1;

    // Get a pointer to the codec context for the video stream

    codecCtx = pFormatCtx->streams[stream_index]->codec;
    avcodec_close(codecCtx);

    if (codecCtx->codec_type == AVMEDIA_TYPE_VIDEO)
    {
        if (!hardware_decode)
            codecCtx->flags |= CODEC_FLAG_GRAY;
        is->dec_ctx = codecCtx;
#ifdef HARDWARE_DECODE
        ist->dec_ctx = codecCtx;
        ist->dec_ctx->opaque = ist;
        ist->dec_ctx->get_format            = get_format;
        ist->dec_ctx->get_buffer2           = get_buffer;
//        ist->dec_ctx->thread_safe_callbacks = 1;
        ist->hwaccel_id = -1; //HWACCEL_AUTO;
        if (hardware_decode) {
#ifdef DONATOR
            ist->hwaccel_id = HWACCEL_AUTO;
#else
            Debug(0, "Hardware accelerated video decoding is only available in the Donator version\n");
#endif
        }
#endif

        codecCtx->flags2 |= CODEC_FLAG2_FAST /* | AV_CODEC_FLAG2_SHOW_ALL */ ;
        if (codecCtx->codec_id != CODEC_ID_MPEG1VIDEO)
#ifdef DONATOR
            codecCtx->thread_count= thread_count;
#else
            codecCtx->thread_count= 1;
#endif


        if (codecCtx->codec_id == CODEC_ID_H264) {
            is_h264 = 1;
#ifdef DONATOR
#else
            Debug(0, "h.264 video can only be processed at full speed by the Donator version\n");
#endif
        }
        else
        {
#ifdef DONATOR
            int w;
            if (lowres == 10) {
                w = codecCtx->width;
                lowres = 0;
                while (w > 600) {
                    w = w >> 1;
                    lowres++;
                }
            }
 //           codecCtx->lowres = lowres;
#endif
//            /* if(lowres) */ codecCtx->flags |= CODEC_FLAG_EMU_EDGE;
        }
//        codecCtx->flags2 |= CODEC_FLAG2_FAST;
        if (codecCtx->codec_id != CODEC_ID_MPEG1VIDEO)
#ifdef DONATOR
            codecCtx->thread_count= thread_count;
#else
            codecCtx->thread_count= 1;
#endif
    }


    codec = avcodec_find_decoder(codecCtx->codec_id);

    if(!codec || (avcodec_open2(codecCtx, codec, NULL) < 0))
    {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    switch(codecCtx->codec_type)
    {
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitleStream = stream_index;
        is->subtitle_st = pFormatCtx->streams[stream_index];
        if (demux_pid)
            selected_subtitle_pid = is->subtitle_st->id;
        break;
    case AVMEDIA_TYPE_AUDIO:
        is->audioStream = stream_index;
        is->audio_st = pFormatCtx->streams[stream_index];
//          is->audio_buf_size = 0;
//          is->audio_buf_index = 0;

        /* averaging filter for audio sync */
//          is->audio_diff_avg_coef = exp(log(0.01 / AUDIO_DIFF_AVG_NB));
//          is->audio_diff_avg_count = 0;
        /* Correct audio only if larger error than this */
//          is->audio_diff_threshold = 2.0 * SDL_AUDIO_BUFFER_SIZE / codecCtx->sample_rate;
        if (demux_pid)
            selected_audio_pid = is->audio_st->id;


 //       memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->videoStream = stream_index;
        is->video_st = pFormatCtx->streams[stream_index];

//          is->frame_timer = (double)av_gettime() / 1000000.0;
//          is->frame_last_delay = 40e-3;
//          is->video_current_pts_time = av_gettime();

        is->pFrame = av_frame_alloc();
        if (!hardware_decode)
            codecCtx->flags |= CODEC_FLAG_GRAY;
        codecCtx->lowres = min(av_codec_get_max_lowres(codecCtx->codec),lowres);
        if (codecCtx->codec_id == CODEC_ID_H264)
        {
            is_h264 = 1;
#ifdef DONATOR
#else
            Debug(0, "h.264 video can only be processed at full speed by the Donator version\n");
#endif
        }

        //        codecCtx->flags2 |= CODEC_FLAG2_FAST;
        if (codecCtx->codec_id != CODEC_ID_MPEG1VIDEO)
#ifdef DONATOR
            codecCtx->thread_count= thread_count;
#else
            codecCtx->thread_count= 1;
#endif
        if (codecCtx->codec_id == CODEC_ID_MPEG1VIDEO)
            is->video_st->codec->ticks_per_frame = 1;
        if (demux_pid)
            selected_video_pid = is->video_st->id;
        /*
        MPEG
                        if(  (codecCtx->skip_frame >= AVDISCARD_NONREF && s2->pict_type==FF_B_TYPE)
                            ||(codecCtx->skip_frame >= AVDISCARD_NONKEY && s2->pict_type!=FF_I_TYPE)
                            || codecCtx->skip_frame >= AVDISCARD_ALL)


                        if(  (s->avctx->skip_idct >= AVDISCARD_NONREF && s->pict_type == FF_B_TYPE)
                           ||(codecCtx->skip_idct >= AVDISCARD_NONKEY && s->pict_type != FF_I_TYPE)
                           || s->avctx->skip_idct >= AVDISCARD_ALL)
        h.264
            if(   s->codecCtx->skip_loop_filter >= AVDISCARD_ALL
               ||(s->codecCtx->skip_loop_filter >= AVDISCARD_NONKEY && h->slice_type_nos != FF_I_TYPE)
               ||(s->codecCtx->skip_loop_filter >= AVDISCARD_BIDIR  && h->slice_type_nos == FF_B_TYPE)
               ||(s->codecCtx->skip_loop_filter >= AVDISCARD_NONREF && h->nal_ref_idc == 0))

        Both
                        if(  (codecCtx->skip_frame >= AVDISCARD_NONREF && s2->pict_type==FF_B_TYPE)
                            ||(codecCtx->skip_frame >= AVDISCARD_NONKEY && s2->pict_type!=FF_I_TYPE)
                            || codecCtx->skip_frame >= AVDISCARD_ALL)
                            break;

        */
        if (skip_B_frames)
            codecCtx->skip_frame = AVDISCARD_NONREF;
        //          codecCtx->skip_loop_filter = AVDISCARD_NONKEY;
//           codecCtx->skip_idct = AVDISCARD_NONKEY;

        break;
    default:
        break;
    }

    return(0);
}

static void log_callback_report(void *ptr, int level, const char *fmt, va_list vl)
{
    va_list vl2;
//    int l;
    char line[1024];
    static int print_prefix = 1;

    if (reviewing)
        return;
    av_log_get_level();
    if (level > av_log_level)
        return;
    va_copy(vl2, vl);
 //   av_log_default_callback(ptr, level, fmt, vl);
    av_log_format_line(ptr, level, fmt, vl2, line, sizeof(line), &print_prefix);
    va_end(vl2);
        Debug(10, line);

 //   fputs(line, report_file);
 //   fflush(report_file);
}



void file_open()
{
    VideoState *is;
    int subtitle_index= -1, audio_index= -1, video_index = -1;
    int openretries = 0;

    if (global_video_state == NULL)
    {
        is = av_mallocz(sizeof(VideoState));
        memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
        strcpy(is->filename, mpegfilename);
        // Register all formats and codecs
        av_log_level=AV_LOG_INFO;
        av_log_set_callback(log_callback_report);

        av_log_set_flags(AV_LOG_SKIP_REPEATED);
        avcodec_register_all();
//    avfilter_register_all();
        av_register_all();
        avformat_network_init();
        global_video_state = is;
        is->videoStream=-1;
        is->audioStream=-1;
        is->subtitleStream = -1;
        is->pFormatCtx = NULL;
    }
    else
        is = global_video_state;
    // Open video file
    if ( is->pFormatCtx == NULL)
    {
        is->pFormatCtx = avformat_alloc_context();
        is->pFormatCtx->max_analyze_duration2 *= 4;
//        pFormatCtx->probesize = 400000;
again:
        if(avformat_open_input(&is->pFormatCtx, is->filename, NULL, NULL)!=0)
        {
            fprintf(stderr, "%s: Can not open file\n", is->filename);
            if (openretries++ < live_tv_retries)
            {
                Sleep(1000L);
                goto again;
            }
            exit(-1);

        }
        is->seek_by_bytes = !!(is->pFormatCtx->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", is->pFormatCtx->iformat->name);
// #if def _DEBUG
//        if (is->duration < 5*60 && retries++ < live_tv_retries)
//        {
//            Sleep(4000L);
//            goto again;
//        }
// #en dif
//     is->pFormatCtx->max_analyze_duration = 320000000;
//    is->pFormatCtx->thread_count= 2;

        // Retrieve stream information
        if(avformat_find_stream_info(is->pFormatCtx, 0L )<0)
        {
            fprintf(stderr, "%s: Can not find stream info\n", is->filename);
            exit(-1);
        }
        // Dump information about file onto standard error
        if (retries == 0) av_dump_format(is->pFormatCtx, 0, is->filename, 0);
    }
#ifndef DONATOR

    if (strcmp(is->pFormatCtx->iformat->name,"wtv")==0)
    {
        Debug(0, "WTV files can only be processed by the Donator version\n");
        exit(-1);
    }
#endif

    if ( is->videoStream == -1)
    {
        video_index = av_find_best_stream(is->pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if(video_index >= 0)
        {
            stream_component_open(is, video_index);
        }
        if(is->videoStream < 0)
        {
            Debug(0, "Could not open video codec\n");
            fprintf(stderr, "%s: could not open video codec\n", is->filename);
            exit(-1);
        }

        if ( is->video_st->duration == AV_NOPTS_VALUE ||  is->video_st->duration < 0)
            is->duration =  ((float)is->pFormatCtx->duration) / AV_TIME_BASE;
        else
            is->duration =  av_q2d(is->video_st->time_base)* is->video_st->duration;


        /* Calc FPS */
        if(is->video_st->r_frame_rate.den && is->video_st->r_frame_rate.num)
        {
            is->fps = av_q2d(is->video_st->r_frame_rate);
        }
        else
        {
            is->fps = 1/av_q2d(is->video_st->codec->time_base);
        }
//        Debug(1, "Stream frame rate is %5.3f f/s\n", is->fps);


    }

    if (is->audioStream== -1 && video_index>=0)
    {

        audio_index = av_find_best_stream(is->pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, video_index, NULL, 0);
        if(audio_index >= 0)
        {
            stream_component_open(is, audio_index);

            if (is->audioStream < 0)
            {
                Debug(1,"Could not open audio decoder or no audio present\n");
            }
        }

    }

    if (is->subtitleStream == -1 && video_index>=0)
    {
        subtitle_index = av_find_best_stream(is->pFormatCtx, AVMEDIA_TYPE_SUBTITLE, -1, video_index, NULL, 0);
        if(subtitle_index >= 0)
        {
            is->subtitle_st = is->pFormatCtx->streams[subtitle_index];
            if (demux_pid)
                selected_subtitle_pid = is->subtitle_st->id;
        }

    }
    av_log_level=AV_LOG_ERROR;


                    is->seek_req = 0;
//                    framenum = 0;
                    pts_offset = 0.0;
                    is->video_clock = 0.0;
                    is->audio_clock = 0.0;
//                    sound_frame_counter = 0;
//                    initial_pts = 0.0;
//                    initial_pts_set = 0;
//                    initial_apts_set = 0;
                    initial_apts = 0;
                    apts_offset = 0.0;
                    base_apts = 0.0;
                    top_apts = 0.0;
                    apts = 0.0;
                    audio_buffer_ptr = audio_buffer;
                    audio_samples = 0;
//                    DUMP_CLOSE
//                    DUMP_OPEN
//                    DUMP_HEADER
//                    close_data();
#ifdef PROCESS_CC
//                    if (output_srt || output_smi) CEW_reinit();
#endif

}



void file_close()
{
    is = global_video_state;

//    av_freep(&ist->hwaccel_device);


    if (is->videoStream != -1) avcodec_close(is->pFormatCtx->streams[is->videoStream]->codec);
    is->videoStream = -1;
//    avcodec_free_context(&is->pFormatCtx->streams[is->videoStream]->codec);

    if (is->audioStream != -1) avcodec_close(is->pFormatCtx->streams[is->audioStream]->codec);
    is->audioStream = -1;
    if (is->subtitleStream != -1)  avcodec_close(is->pFormatCtx->streams[is->subtitleStream]->codec);
    is->subtitleStream = -1;
//    is->pFormatCtx = NULL;


    avformat_close_input(&is->pFormatCtx);

    av_frame_free(&is->frame);

#ifdef HARDWARE_DECODE
    ist->hwaccel_ctx = NULL;
#endif

    avformat_network_deinit();
//  global_video_state = NULL;
};


// copied & modified from mingw-runtime-3.13's init.c
typedef struct
{
    int newmode;
} _startupinfo;
extern void __wgetmainargs (int *, wchar_t ***, wchar_t ***, int, _startupinfo
                            *);


int main (int argc, char ** argv)
{
    AVPacket pkt1, *packet = &pkt1;
    int result = 0;
    int ret;
    double tfps;
    double old_clock = 0.0;
                    int empty_packet_count = 0;

    int64_t last_packet_pos = 0;
    int64_t last_packet_pts = 0;
    double retry_target = 0.0;

#ifdef SELFTEST
    //int tries = 0;
#endif
    retries = 0;

    char *ptr;
    size_t len;

#ifdef __MSVCRT_VERSION__

    int i;
    int _argc = 0;
    wchar_t **_argv = 0;
    wchar_t **dummy_environ = 0;
    _startupinfo start_info;
    start_info.newmode = 0;
    __wgetmainargs(&_argc, &_argv, &dummy_environ, -1, &start_info);



    char *aargv[20];
    char (argt[20])[1000];

    argv = aargv;
    argc = _argc;

    for (i= 0; i< argc; i++)
    {
        argv[i] = &(argt[i][0]);
        WideCharToMultiByte(CP_UTF8, 0,_argv[i],-1, argv[i],  1000, NULL, NULL );
    }

#endif

#ifndef _DEBUG
//	__tr y
    {
        //      raise_ exception();
#endif

//		output_debugwindow = 1;

        if (strstr(argv[0],"comskipGUI"))
            output_debugwindow = 1;
        else
        {
#ifdef _WIN32
            //added windows specific
            SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
        }
        //get path to executable
        ptr = argv[0];
        if (*ptr == '\"')
        {
            ptr++; //strip off quotation marks
            len = (size_t)(strchr(ptr,'\"') - ptr);
        }
        else
        {
            len = strlen(ptr);
        }
        strncpy(HomeDir, ptr, len);

        len = (size_t)max(0,strrchr(HomeDir,'\\') - HomeDir);
        if (len==0)
        {
            HomeDir[0] = '.';
            HomeDir[1] = '\0';
        }
        else
        {
            HomeDir[len] = '\0';
        }

        fprintf (stderr, "%s, made using ffmpeg\n", PACKAGE_STRING);

#ifndef DONATOR
        fprintf (stderr, "Public build\n");
#else
        fprintf (stderr, "Donator build\n");
#endif

#ifdef _WIN32
#ifdef HAVE_IO_H
//		_setmode (_fileno (stdin), O_BINARY);
//		_setmode (_fileno (stdout), O_BINARY);
#endif
#endif


#ifdef _WIN32
        //added windows specific
//		if (!live_tv) SetThreadPriority(GetCurrentThread(), /* THREAD_MODE_BACKGROUND_BEGIN */ 0x00010000); // This will fail in XP but who cares

#endif
        /*
        #define ES_AWAYMODE_REQUIRED    0x00000040
        #define ES_CONTINUOUS           0x80000000
        #define ES_SYSTEM_REQUIRED      0x00000001
        */

#if (_WIN32_WINNT >= 0x0500 || _WIN32_WINDOWS >= 0x0410)

        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
#endif

//
// Wait until recording is complete...
//

//        av_log_set_level(AV_LOG_WARNING);
//        av_log_set_flags(AV_LOG_SKIP_REPEATED);
//        av_log_set_callback(log_callback_report);
//        av_log_set_level(AV_LOG_WARNING);
        in_file = LoadSettings(argc, argv);

        file_open();



        csRestart = 0;
        framenum = 0;

//        DUMP_OPEN

        if (output_timing)
        {
            sprintf(tempstring, "%s.timing.csv", inbasename);
            timing_file = myfopen(tempstring, "w");
            DUMP_HEADER
        }

        av_log_set_level(AV_LOG_INFO);
//        av_log_set_flags(AV_LOG_SKIP_REPEATED);
//        av_log_set_callback(log_callback_report);
        is = global_video_state;
        packet = &(is->audio_pkt);

        // main decode loop
again:
        for(;;)
        {
            if(is->quit)
            {
                break;
            }
            // seek stuff goes here
            if(is->seek_req)
            {
                if (is->seek_pts > 0.0)
                {
                    DoSeekRequest(is);
                }
                else
                {
                    is->seek_req = 0;
                    file_close();
                    file_open();

                    DUMP_CLOSE
                    DUMP_OPEN
                    DUMP_HEADER
                    close_data();
#ifdef PROCESS_CC
                    if (output_srt || output_smi) CEW_reinit();
#endif
                }
            }
nextpacket:
            ret=av_read_frame(is->pFormatCtx, packet);

            if (ret>=0 && is->seek_req)
            {
                double packet_time = (packet->pts - (is->video_st->start_time != AV_NOPTS_VALUE ? is->video_st->start_time : 0)) * av_q2d(is->video_st->time_base);
                if (packet->pts==AV_NOPTS_VALUE || packet->pts == 0 )
                {
                    av_free_packet(packet);
                    goto nextpacket;
                }
                if (is->seek_req < 6 && (is->seek_flags & AVSEEK_FLAG_BYTE) &&  fabs(packet_time - (is->seek_pts - 2.5) ) < is->duration / (10 * is->seek_req))
                {
                    is->seek_pos += ((is->seek_pts - 2.5 - packet_time) / is->duration ) * avio_size(is->pFormatCtx->pb) * 0.9;
                    is->seek_req++;
                    goto again;
                }
                if (retries) Debug( 9,"Retry t_pos=%" PRId64 ", l_pos=%" PRId64 ", t_pts=%" PRId64 ", l_pts=%" PRId64 "\n", last_packet_pos, packet->pos, last_packet_pts, packet->pts);
                is->seek_req = 0;
            }
            /*
                    if (ret < 0 && is->seek_req && !is->seek_by_bytes) {
                        is->seek_by_bytes = 1;
                        Set_seek(is, is->seek_pts, is->duration);
                        goto again;
                    }
            */
            is->seek_req = 0;



#define REOPEN_TIME 500.0

            if ((selftest == 3 && retries==0 && is->video_clock >=REOPEN_TIME))
            {
                ret=AVERROR_EOF;  // Simulate EOF
                live_tv = 1;
            }
            if ((selftest == 4 && retries==0 && framenum > 0 && (framenum % 500) == 0))
            {
                ret=AVERROR_EOF;  // Simulate EOF
                live_tv = 1;
            }
            if(ret < 0 )
            {
                if (ret == AVERROR_EOF || is->pFormatCtx->pb->eof_reached)
                {
                    if (selftest == 3)   // Either simulated EOF or real EOF before REOPEN_TIME
                    {
                        if (retries > 0)
                        {
                            if (is->video_clock < selftest_target - 0.05 || is->video_clock > selftest_target + 0.05)
                            {
//                                sample_file = fopen("seektest.log", "a+");
//                                fprintf(sample_file, "\"%s\": reopen file failed, size=%8.1f, pts=%6.2f\n", is->filename, is->duration, is->video_clock );
//                                fclose(sample_file);
//                                exit(1);
                            }
                        }
                        else
                        {
                            if (is->video_clock < REOPEN_TIME)
                            {
                                selftest_target = is->video_clock - 2.0;
                            }
                            else
                            {
                                selftest_target = REOPEN_TIME;
                            }
                            selftest_target = fmax(selftest_target,0.5);
                            live_tv = 1;
                        }
                    }

                    if ((live_tv && retries < live_tv_retries) /* || (selftest == 3 && retries == 0) */)
                    {
//                    uint64_t retry_target;
                        if (retries == 0)
                        {
                            if (selftest == 3)
                                retry_target = selftest_target;
//                        retry_target = avio_tell(is->pFormatCtx->pb);
                            else
                                retry_target = is->video_clock;
                        }
                        file_close();
                        Debug( 1,"\nRetry=%d at frame=%d, time=%8.2f seconds\n", retries, framenum, retry_target);
                        Debug( 9,"Retry target pos=%" PRId64 ", pts=%" PRId64 "\n", last_packet_pos, last_packet_pts);

                        if (selftest == 0) Sleep(4000L);
                        file_open();
                        Set_seek(is, retry_target, is->duration);

                        retries++;
                        goto again;
                    }

                    break;
                }


            }

            if (packet->pts != AV_NOPTS_VALUE && packet->pts != 0 )
            {
                last_packet_pts = packet->pts;
            }
            if (packet->pos != 0 && packet->pos != -1)
            {
                last_packet_pos = packet->pos;
            }

            if(packet->stream_index == is->videoStream)
            {
                video_packet_process(is, packet);
            }
            else if(packet->stream_index == is->audioStream)
            {
                audio_packet_process(is, packet);
            }
            else
            {
                /*
                			  ccDataLen = (int)packet->size;
                			  for (i=0; i<ccDataLen; i++) {
                				  ccData[i] = packet->data[i];
                			  }
                			  dump_data((char *)ccData, (int)ccDataLen);
                					if (output_srt)
                						process_block(ccData, (int)ccDataLen);
                					if (processCC) ProcessCCData();
                */
            }
            av_free_packet(packet);
            if (is->video_clock == old_clock)
            {
                empty_packet_count++;
                if (empty_packet_count > 1000)
                    Debug(0, "Empty input\n");
            }
            else
            {
                old_clock = is->video_clock;
                empty_packet_count = 0;
            }
#ifdef SELFTEST
            if (selftest == 1 && pass == 0 && is->seek_req == 0 && framenum == 50)
            {
                selftest_target = fmin(450.0, is->duration - 2);
                Set_seek(is, selftest_target, is->duration);
                pass = 1;
                framenum++;
            }
#endif
        }

        if (selftest == 1 && pass == 1 /*&& framenum > 501 && is->video_clock > 0 */)
        {
            if (is->video_clock < selftest_target - 0.08 || is->video_clock > selftest_target + 0.08)
            {
                sample_file = fopen("seektest.log", "a+");
                fprintf(sample_file, "Seek error: target=%8.1f, result=%8.1f, error=%6.3f, size=%8.1f, mode=%s\"%s\"\n",
                        is->seek_pts,
                        is->video_clock,
                        is->video_clock - is->seek_pts,
                        is->duration,
                        (is->seek_by_bytes ? "byteseek": "timeseek" ),
                        is->filename);
                fclose(sample_file);
            }
            /*
                            if (tries ==  0 && fabs((double) av_q2d(is->video_st->time_base)* ((double)(packet->pts - is->video_st->start_time - is->seek_pos ))) > 2.0) {
             				   is->seek_req=1;
            				   is->seek_pos = 20.0 / av_q2d(is->video_st->time_base);
            				   is->seek_flags = AVSEEK_FLAG_BYTE;
            				   tries++;
                           } else
             */
            selftest = 3;
            pass = 0;
            //exit(1);
        }



        if (live_tv)
        {
            lastFrameCommCalculated = 0;
            BuildCommListAsYouGo();
        }

        tfps = print_fps (1);

        Debug( 10,"\nParsed %d video frames and %d audio frames at %8.2f fps\n", framenum, sound_frame_counter, tfps);
        Debug( 10,"\nMaximum Volume found is %d\n", max_volume_found);


        in_file = 0;
        if (framenum>0)
        {
            if(BuildMasterCommList())
            {
                result = 1;
                printf("Commercials were found.\n");
            }
            else
            {
                result = 0;
                printf("Commercials were not found.\n");
            }
            if (output_debugwindow)
            {
                processCC = 0;
                printf("Close window when done\n");

                DUMP_CLOSE
                if (output_timing)
                {
                    output_timing = 0;
                }

#ifdef _WIN32
                while(1)
                {
                    ReviewResult();
                    vo_refresh();
                    Sleep(100L);
                }
#endif
                //		printf(" Press Enter to close debug window\n");
                //		gets(HomeDir);
            }
        }

#ifndef _DEBUG
    }
//	__exc ept(filter()) /* Stage 3 */
//	{
//      printf("Exception raised, terminating\n");/* Stage 5 of terminating exception */
//		exit(result);
//	}
#endif

//
// Clear EXECUTION_STATE flags to disable away mode and allow the system to idle to sleep normally.
//
#if (_WIN32_WINNT >= 0x0500 || _WIN32_WINDOWS >= 0x0410)
    SetThreadExecutionState(ES_CONTINUOUS);
#endif

#ifdef _WIN32
    exit (result);
#else
    exit (!result);
#endif
}
