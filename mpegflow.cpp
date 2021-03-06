#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <stdint.h>

extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libavutil/motion_vector.h>
}

// Fix for av_err2str which stops taking the address of a temporary
// https://ffmpeg.org/pipermail/libav-user/2013-January/003458.html
#undef av_err2str
#define av_err2str(errnum) av_make_error_string((char*)__builtin_alloca(AV_ERROR_MAX_STRING_SIZE), AV_ERROR_MAX_STRING_SIZE, errnum)

#include <string>
#include <algorithm>
#include <vector>
#include <stdexcept>
#include <functional>

AVFrame* ffmpeg_pFrame;
AVFormatContext* ffmpeg_pFormatCtx;
AVStream* ffmpeg_pVideoStream;
AVCodecContext* ffmpeg_pVideoCtx;
int ffmpeg_videoStreamIndex;
size_t ffmpeg_frameWidth, ffmpeg_frameHeight;

bool ARG_OUTPUT_RAW_MOTION_VECTORS, ARG_FORCE_GRID_8, ARG_FORCE_GRID_16, ARG_OUTPUT_OCCUPANCY, ARG_QUIET, ARG_HELP;
const char* ARG_VIDEO_PATH;

void ffmpeg_print_error(int err) // copied from cmdutils.c, originally called print_error
{
    char errbuf[128];
    const char *errbuf_ptr = errbuf;

    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));
    av_log(NULL, AV_LOG_ERROR, "ffmpeg_print_error: %s\n", errbuf_ptr);
}

void ffmpeg_log_callback_null(void *ptr, int level, const char *fmt, va_list vl)
{
}

void ffmpeg_init()
{
    av_register_all();

    if(ARG_QUIET)
    {
        av_log_set_level(AV_LOG_ERROR);
        av_log_set_callback(ffmpeg_log_callback_null);
    }

    ffmpeg_pFrame = av_frame_alloc();
    ffmpeg_pFormatCtx = avformat_alloc_context();
    ffmpeg_videoStreamIndex = -1;

    int err = 0;

    if ((err = avformat_open_input(&ffmpeg_pFormatCtx, ARG_VIDEO_PATH, NULL, NULL)) != 0)
    {
        ffmpeg_print_error(err);
        throw std::runtime_error("Couldn't open file. Possibly it doesn't exist.");
    }

    if ((err = avformat_find_stream_info(ffmpeg_pFormatCtx, NULL)) < 0)
    {
        ffmpeg_print_error(err);
        throw std::runtime_error("Stream information not found.");
    }

    // Identify the video stream and get all of the information out into static variables
    for(int i = 0; i < ffmpeg_pFormatCtx->nb_streams; i++)
    {
        // Updated to avoid using ffmpeg_pFormatCtx->streams[i]->codec (deprecated) as from  https://code.mythtv.org/trac/ticket/13186
        auto *stream = ffmpeg_pFormatCtx->streams[i];
        AVCodec *pCodec = avcodec_find_decoder(stream->codecpar->codec_id);
        
        if (AVMEDIA_TYPE_VIDEO == pCodec->type && ffmpeg_videoStreamIndex < 0 )
        {
            ffmpeg_pVideoCtx = avcodec_alloc_context3(pCodec);
            avcodec_parameters_to_context(ffmpeg_pVideoCtx, stream->codecpar);
            // TODO: is this necessary?
            av_codec_set_pkt_timebase(ffmpeg_pVideoCtx, stream->time_base);
            
            AVDictionary *opts = NULL;
            av_dict_set(&opts, "flags2", "+export_mvs", 0);
            if (!pCodec || avcodec_open2(ffmpeg_pVideoCtx, pCodec, &opts) < 0)
                throw std::runtime_error("Codec not found or cannot open codec.");
            
            ffmpeg_videoStreamIndex = i;
            ffmpeg_pVideoStream = ffmpeg_pFormatCtx->streams[i];
            ffmpeg_frameWidth = ffmpeg_pVideoCtx->width;
            ffmpeg_frameHeight = ffmpeg_pVideoCtx->height;
            
            break;
        }
    }

    if(ffmpeg_videoStreamIndex == -1)
        throw std::runtime_error("Video stream not found.");
}

int for_each_frame(std::function<void(int64_t, char)> callback, std::vector<AVMotionVector>& motionVectors) {
    AVPacket pkt;
    AVFrame* frame = av_frame_alloc();
    int ret;

    while (true) {
        ret = av_read_frame(ffmpeg_pFormatCtx, &pkt);
        if (ret != 0)
            return ret;

        if (pkt.stream_index == ffmpeg_videoStreamIndex) {
            // We know this is a video packet, now extract the frames

            // Send the packet to the video decoder
            ret = avcodec_send_packet(ffmpeg_pVideoCtx, &pkt);
            if (ret != 0)
                return ret;

            // One packet can contain multiple frames, so extract each one in turn
            while (ret >= 0) {
                // Get a frame
                ret = avcodec_receive_frame(ffmpeg_pVideoCtx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    // Either the packet has ended (AVERROR_EOF) or it needs more packets to fully extract the data (EAGAIN).
                    // Break out and send it another video packet, then try again.
                    break;
                } else if (ret < 0) {
                    // An unexpected error occurred
                    return ret;
                }

                // We have a valid frame, process it
                int64_t pts = av_frame_get_best_effort_timestamp(frame);
                char pictType = av_get_picture_type_char(ffmpeg_pFrame->pict_type);

                AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
                if(sd != NULL)
                {
                    // reading motion vectors, see ff_print_debug_info2 in ffmpeg's libavcodec/mpegvideo.c for reference and a fresh doc/examples/extract_mvs.c
                    AVMotionVector* mvs = (AVMotionVector*)sd->data;
                    int mvcount = sd->size / sizeof(AVMotionVector);
                    motionVectors = std::vector<AVMotionVector>(mvs, mvs + mvcount);
                }
                else
                {
                    motionVectors = std::vector<AVMotionVector>();
                }

                // Call the callback
                callback(pts, pictType);

                // No need to unref the frame here, avcodec_receive_frame does that automatically
            }
        }

        av_frame_unref(frame);
        av_packet_unref(&pkt);
    }
}

struct FrameInfo
{
    const static size_t MAX_GRID_SIZE = 512;

    size_t GridStep;
    std::pair<size_t, size_t> Shape;

    int dx[MAX_GRID_SIZE][MAX_GRID_SIZE];
    int dy[MAX_GRID_SIZE][MAX_GRID_SIZE];
    uint8_t occupancy[MAX_GRID_SIZE][MAX_GRID_SIZE];
    int64_t Pts;
    int FrameIndex;
    char PictType;
    const char* Origin;
    bool Empty;
    bool Printed;

    FrameInfo()
    {
        memset(dx, 0, sizeof(dx));
        memset(dy, 0, sizeof(dy));
        memset(occupancy, 0, sizeof(occupancy));
        Empty = true;
        Printed = false;
        PictType = '?';
        FrameIndex = -1;
        Pts = -1;
        Origin = "";
    }

    void InterpolateFlow(FrameInfo& prev, FrameInfo& next)
    {
        for(int i = 0; i < Shape.first; i++)
        {
            for(int j = 0; j < Shape.second; j++)
            {
                dx[i][j] = (prev.dx[i][j] + next.dx[i][j]) / 2;
                dy[i][j] = (prev.dy[i][j] + next.dy[i][j]) / 2;
            }
        }
        Empty = false;
        Origin = "interpolated";
    }

    void FillInSomeMissingVectorsInGrid8()
    {
        for(int k = 0; k < 2; k++)
        {
            for(int i = 1; i < Shape.first - 1; i++)
            {
                for(int j = 1; j < Shape.second - 1; j++)
                {
                    if(occupancy[i][j] == 0)
                    {
                        if(occupancy[i][j - 1] != 0 && occupancy[i][j + 1] != 0)
                        {
                            dx[i][j] = (dx[i][j -1] + dx[i][j + 1]) / 2;
                            dy[i][j] = (dy[i][j -1] + dy[i][j + 1]) / 2;
                            occupancy[i][j] = 2;
                        }
                        else if(occupancy[i - 1][j] != 0 && occupancy[i + 1][j] != 0)
                        {
                            dx[i][j] = (dx[i - 1][j] + dx[i + 1][j]) / 2;
                            dy[i][j] = (dy[i - 1][j] + dy[i + 1][j]) / 2;
                            occupancy[i][j] = 2;
                        }
                    }
                }
            }
        }
    }

    void PrintIfNotPrinted()
    {
        static int64_t FirstPts = -1;

        if(Printed)
            return;

        if(FirstPts == -1)
            FirstPts = Pts;
        
        printf("# pts=%lld frame_index=%d pict_type=%c output_type=arranged shape=%zux%zu origin=%s\n", (long long) Pts - FirstPts, FrameIndex, PictType, (ARG_OUTPUT_OCCUPANCY ? 3 : 2) * Shape.first, Shape.second, Origin);
        for(int i = 0; i < Shape.first; i++)
        {
            for(int j = 0; j < Shape.second; j++)
            {
                printf("%4d", dx[i][j]);
            }
            printf("\n");
        }
        for(int i = 0; i < Shape.first; i++)
        {
            for(int j = 0; j < Shape.second; j++)
            {
                printf("%4d", dy[i][j]);
            }
            printf("\n");
        }

        if(ARG_OUTPUT_OCCUPANCY)
        {
            for(int i = 0; i < Shape.first; i++)
            {
                for(int j = 0; j < Shape.second; j++)
                {
                    printf("%4d", occupancy[i][j]);
                }
                printf("\n");
            }
        }

        Printed = true;
    }
};

const size_t FrameInfo::MAX_GRID_SIZE;

void output_vectors_raw(int frameIndex, int64_t pts, char pictType, std::vector<AVMotionVector>& motionVectors)
{
    printf("# pts=%lld frame_index=%d pict_type=%c output_type=raw shape=%zux4\n", (long long) pts, frameIndex, pictType, motionVectors.size());
    for(int i = 0; i < motionVectors.size(); i++)
    {
        AVMotionVector& mv = motionVectors[i];
        int mvdx = mv.dst_x - mv.src_x;
        int mvdy = mv.dst_y - mv.src_y;

        if (mvdx != 0 || mvdy != 0)
            printf("%d\t%d\t%d\t%d\n", mv.dst_x, mv.dst_y, mvdx, mvdy);
        /*printf("%d,\t%2d,\t%2d,\t%2d,\t%4d,\t%4d,\t%4d\n",
                        mv->source,
                        mv->w, mv->h, mv->src_x, mv->src_y,
                        mv->dst_x, mv->dst_y);*/
    }
}

void output_vectors_std(int frameIndex, int64_t pts, char pictType, std::vector<AVMotionVector>& motionVectors)
{
    static std::vector<FrameInfo> prev;

    size_t gridStep = ARG_FORCE_GRID_8 ? 8 : 16;
    std::pair<size_t, size_t> shape = std::make_pair(std::min(ffmpeg_frameHeight / gridStep, FrameInfo::MAX_GRID_SIZE), std::min(ffmpeg_frameWidth / gridStep, FrameInfo::MAX_GRID_SIZE));

    /*if(!prev.empty() && pts != prev.back().Pts + 1)
    {
        for(int64_t dummy_pts = prev.back().Pts + 1; dummy_pts < pts; dummy_pts++)
        {
            FrameInfo dummy;
            dummy.FrameIndex = -1;
            dummy.Pts = dummy_pts;
            dummy.Origin = "dummy";
            dummy.PictType = '?';
            dummy.GridStep = gridStep;
            dummy.Shape = shape;
            prev.push_back(dummy);
        }
    }*/

    FrameInfo cur;
    cur.FrameIndex = frameIndex;
    cur.Pts = pts;
    cur.Origin = "video";
    cur.PictType = pictType;
    cur.GridStep = gridStep;
    cur.Shape = shape;

    for(int i = 0; i < motionVectors.size(); i++)
    {
        AVMotionVector& mv = motionVectors[i];
        int mvdx = mv.dst_x - mv.src_x;
        int mvdy = mv.dst_y - mv.src_y;

        size_t i_clipped = std::max(size_t(0), std::min(mv.dst_y / cur.GridStep, cur.Shape.first - 1)); 
        size_t j_clipped = std::max(size_t(0), std::min(mv.dst_x / cur.GridStep, cur.Shape.second - 1));

        cur.Empty = false;
        cur.dx[i_clipped][j_clipped] = mvdx;
        cur.dy[i_clipped][j_clipped] = mvdy;
        cur.occupancy[i_clipped][j_clipped] = true;
    }

    if(cur.GridStep == 8)
        cur.FillInSomeMissingVectorsInGrid8();
    
    if(frameIndex == -1)
    {
        for(int i = 0; i < prev.size(); i++)
            prev[i].PrintIfNotPrinted();
    }
    else if(!motionVectors.empty())
    {
        if(prev.size() == 2 && prev.front().Empty == false)
        {
            prev.back().InterpolateFlow(prev.front(), cur);
            prev.back().PrintIfNotPrinted();
        }
        else
        {
            for(int i = 0; i < prev.size(); i++)
                prev[i].PrintIfNotPrinted();
        }
        prev.clear();
        cur.PrintIfNotPrinted();
    }

    prev.push_back(cur);
}

void parse_options(int argc, const char* argv[])
{
    for(int i = 1; i < argc; i++)
    {
        if(strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            ARG_HELP = true;
        else if(strcmp(argv[i], "--raw") == 0)
            ARG_OUTPUT_RAW_MOTION_VECTORS = true;
        else if(strcmp(argv[i], "--grid8x8") == 0)
            ARG_FORCE_GRID_8 = true;
        else if(strcmp(argv[i], "--occupancy") == 0)
            ARG_OUTPUT_OCCUPANCY = true;
        else if(strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0)
            ARG_QUIET = true;
        else
            ARG_VIDEO_PATH = argv[i];
    }
    if(ARG_HELP || ARG_VIDEO_PATH == NULL)
    {
        fprintf(stderr, "Usage: mpegflow [--raw | [[--grid8x8] [--occupancy]]] videoPath\n  --help and -h will output this help message.\n  --raw will prevent motion vectors from being arranged in matrices.\n  --grid8x8 will force fine 8x8 grid.\n  --occupancy will append occupancy matrix after motion vector matrices.\n  --quiet will suppress debug output.\n");
        exit(1);
    }
}

int main(int argc, const char* argv[])
{
    parse_options(argc, argv);
    ffmpeg_init();
        
    int64_t pts, prev_pts = -1;
    int frameIndex = 0;
    char pictType;
    std::vector<AVMotionVector> motionVectors;

    auto frameCallback = [&](int64_t newPts, char newPictType) {
        frameIndex++;
        pts = newPts;
        pictType = newPictType;

        if(pts <= prev_pts && prev_pts != -1)
        {
            if(!ARG_QUIET)
                fprintf(stderr, "Skipping frame %d (frame with pts %d already processed).\n", int(frameIndex), int(pts));
            return;
        }

        if(ARG_OUTPUT_RAW_MOTION_VECTORS)
            output_vectors_raw(frameIndex, pts, pictType, motionVectors);
        else
            output_vectors_std(frameIndex, pts, pictType, motionVectors);

        prev_pts = pts;
    };
    int ret = for_each_frame(frameCallback, motionVectors);

    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        exit(1);
    }

    if(ARG_OUTPUT_RAW_MOTION_VECTORS == false)
        output_vectors_std(-1, pts, pictType, motionVectors);
}
