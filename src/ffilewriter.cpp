/*A video surveillance software with support of H264 video sources.
Copyright (C) 2015 Bogdan Maslowsky, Alexander Sorvilov. 

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.*/


#include "ffilewriter.h"
#include <string>

extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/mathematics.h>
    #include <libavutil/timestamp.h>
}
#include "avcpp/packet.h"
#include <cassert>
#include <atomic>

static ZMBCommon::MByteArray ts_to_string(int64_t ts)
{
    size_t size = 0;
    ZMBCommon::MByteArray buffer;
    buffer.resize(AV_TS_MAX_STRING_SIZE, '\0');

    if (ts == int64_t(AV_NOPTS_VALUE))
        size = snprintf(buffer.unsafe(), AV_TS_MAX_STRING_SIZE, "NOPTS");
    else
        size = snprintf(buffer.unsafe(), AV_TS_MAX_STRING_SIZE, "%" PRIu64, ts);
    return buffer.substr(0, size);
}

static ZMBCommon::MByteArray ts_to_timestring(int64_t ts, AVRational *tb)
{
    size_t size = 0;
    ZMBCommon::MByteArray buffer;
    buffer.resize(AV_TS_MAX_STRING_SIZE, 0x00);

    if (ts == int64_t(AV_NOPTS_VALUE))
        size = snprintf(buffer.unsafe(), AV_TS_MAX_STRING_SIZE, "NOPTS");
    else
        size = snprintf(buffer.unsafe(), AV_TS_MAX_STRING_SIZE, "%.6g", av_q2d(*tb) * ts);
    return buffer.substr(0, size);
}

static ZMBCommon::MByteArray packet_to_string(const ZMBCommon::MByteArray& tag, const AVFormatContext *av_fmt_ctx, const AVPacket *av_pkt)
{
    AVRational *time_base = &av_fmt_ctx->streams[av_pkt->stream_index]->time_base;

    ZMBCommon::MByteArray buffer;
    buffer.resize(255, 0x00);
    size_t len = snprintf(buffer.unsafe(), buffer.size(),
                          "%s: pts: %s pts_time: %s dts: %s dts_time: %s duration: %s duration_time: %s stream_index: %d\n",
           tag.c_str(),
           ts_to_string(av_pkt->pts).c_str(), ts_to_timestring(av_pkt->pts, time_base).c_str(),
           ts_to_string(av_pkt->dts).c_str(), ts_to_timestring(av_pkt->dts, time_base).c_str(),
           ts_to_string(av_pkt->duration).c_str(), ts_to_timestring(av_pkt->duration, time_base).c_str(),
           av_pkt->stream_index);

    return buffer.substr(0, len);
}

static ZMBCommon::MByteArray error_to_string(int errnum)
{
    ZMBCommon::MByteArray buff;
    buff.resize(1024, 0x00);

    if (av_strerror(errnum, &buff[0], buff.size()) == 0)
    {
        buff = buff.substr(0, buff.find_last_not_of(char(0)) + 1);
        return buff;
    }

    return ZMBCommon::MByteArray(std::to_string(errnum));
}
namespace ZMB {

//===============================================================================
FFileWriterBase::FFileWriterBase()
{
    is_open = false;
    pb_file_size.store(0u);
}

FFileWriterBase::~FFileWriterBase()
{

}

std::mutex& FFileWriterBase::mutex()
{
  return mu;
}
std::unique_lock<std::mutex>&& FFileWriterBase::locker()
{
  return std::move(std::unique_lock<std::mutex>(mu));
}

void FFileWriterBase::raise_fatal_error()
{
    throw std::runtime_error("calling method that was not implemented.");
    assert(false);
}

void FFileWriterBase::dumpPocketContent(ZMB::PacketsPocket::seq_key_t lastPacketTs)
{
  pocket.dump_packets(lastPacketTs, (void*)this);
}
//===============================================================================

struct FFileWriter::PV
{
    PV(FFileWriter* parent, const AVFormatContext *av_in_fmt_ctx, const ZConstString& dst)
        : d_parent(parent)
        , outFmtContext(nullptr)
        , inFmtContext(av_in_fmt_ctx)
        , frame_cnt(0)
    {
        parent->dest = std::move(std::string(dst.begin(), dst.size()));
        pts_shift = 0;
        dts_shift = 0;
        if (nullptr != av_in_fmt_ctx)
        {
            try
            {
                open();
            }
            catch(const std::exception& e)
            {
                close();
            }
        }
    }

    ~PV()
    {
        close();
    }

    bool open()
    {
        std::string& dst(d_parent->dest);
        int errnum = 0;

        AVOutputFormat *av_out_fmt = av_guess_format(nullptr, /*filename*/dst.c_str(), nullptr);
        if (!av_out_fmt)
        {
            ZMBCommon::MByteArray msg("Can't guess format by passed dest: ");
            msg += dst;
            d_parent->dispatch_error(msg);
            return false;
        }

        outFmtContext = avformat_alloc_context();
        outFmtContext->oformat = av_out_fmt;

        if (!outFmtContext)
        {
            d_parent->dispatch_error("Could not create output context");
            return false;
        }

        for (uint32_t i = 0; i < inFmtContext->nb_streams; ++i)
        {
            if (inFmtContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            {
                AVStream *in_stream = inFmtContext->streams[i];
                AVStream *out_stream = avformat_new_stream(outFmtContext, avcodec_find_encoder(in_stream->codecpar->codec_id));

                if (!out_stream)
                {
                    d_parent->dispatch_error("Failed allocating output stream");
                    goto _on_fail_;
                }

                out_stream->time_base           = in_stream->time_base;
                out_stream->codecpar->codec_id     = in_stream->codecpar->codec_id;
                out_stream->codecpar->sample_aspect_ratio = in_stream->codecpar->sample_aspect_ratio;
            }
        }

        if (!(av_out_fmt->flags & AVFMT_NOFILE))
        {
            if (avio_open(&outFmtContext->pb, dst.c_str(), AVIO_FLAG_WRITE) < 0)
            {
                std::string msg("Could not open output: ");
                msg += dst;
                d_parent->dispatch_error(msg);
                goto _on_fail_;
            }
        }

        errnum = avformat_write_header(outFmtContext, nullptr);
        if(0 != errnum)
          return false;

#ifndef NDEBUG
        av_dump_format(outFmtContext, 0, dst.c_str(), 1);
#endif
        return true;

_on_fail_:
        avformat_free_context(outFmtContext);
        return false;

    }

    void close()
    {
        if (nullptr != outFmtContext)
        {
            if (!outFmtContext->pb->error && frame_cnt > 0)
            {
                av_write_trailer(outFmtContext);
            }

            if ((outFmtContext->pb && !(outFmtContext->oformat->flags & AVFMT_NOFILE)))
            {
                avio_closep(&outFmtContext->pb);
            }            

            avformat_free_context(outFmtContext);
            outFmtContext = nullptr;
        }
    }

    void write(AVPacket* input_avpacket)
    {
        assert (nullptr != inFmtContext);

        if (nullptr == outFmtContext)
        {
            bool ok = false;
            try {
                ok = open();
            }
            catch(const std::exception& e) {
                close();
            }
            assert(ok);
            if (!ok) return;
        }
        int av_stream_idx = input_avpacket->stream_index;

        AVStream *in_stream  = inFmtContext->streams[av_stream_idx];
        AVStream *out_stream = outFmtContext->streams[0];


        if (0 == pts_shift)
        {
            if (input_avpacket->pts != int64_t(AV_NOPTS_VALUE))
            {
                pts_shift = av_rescale_q_rnd(input_avpacket->pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF);
                dts_shift = av_rescale_q_rnd(input_avpacket->dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF);
            }
        }

        int orig_idx = input_avpacket->stream_index;
        int64_t orig_pos = input_avpacket->pos;

        AVPacket* opkt = input_avpacket;
        opkt->stream_index = out_stream->index;
        opkt->pts = input_avpacket->pts  == int64_t(AV_NOPTS_VALUE) ? AV_NOPTS_VALUE
                : av_rescale_q_rnd(input_avpacket->pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF);
        opkt->dts = av_rescale_q_rnd(input_avpacket->dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF);

        opkt->duration = av_rescale_q(input_avpacket->duration, in_stream->time_base, out_stream->time_base);
        opkt->pts -= pts_shift;
        opkt->dts -= dts_shift;
        opkt->pos = -1;

//#ifndef NDEBUG
//        auto mb_log = packet_to_string("in", av_in_fmt_ctx,
//                                       const_cast<AVPacket*>(input_avpacket));
//        LDEBUG(ZCSTR("avpacket"), mb_log.get_const_str());
//#endif

        /** file size indication. Note: always access packet data before *_write_frame()
         *  functions are called, they purge the packet's payload when done.*/
        d_parent->pb_file_size.fetch_add(opkt->size);

        av_interleaved_write_frame(outFmtContext, opkt);

        //restore some values:
        input_avpacket->pts += pts_shift;
        input_avpacket->dts += dts_shift;
        input_avpacket->pos = orig_pos;
        input_avpacket->stream_index = orig_idx;

        ++(frame_cnt);
    }


    FFileWriter* d_parent;

    AVFormatContext *outFmtContext;
    const AVFormatContext *inFmtContext;
    unsigned frame_cnt;

    int64_t pts_shift, dts_shift;
};
//-------------------------------
FFileWriter::FFileWriter() : FFileWriterBase()
{

}

FFileWriter::FFileWriter(const AVFormatContext *av_in_fmt_ctx, const ZConstString& dst)
{
    open(av_in_fmt_ctx, dst);
}

FFileWriter::~FFileWriter()
{
    close();
}

bool FFileWriter::open(const AVFormatContext *av_in_fmt_ctx, const ZConstString& dst)
{
    pv = std::make_shared<PV>(this, av_in_fmt_ctx, dst);
    is_open = pv->open();
    if (!is_open)
        pv.reset();
    return is_open;
}

void FFileWriter::write(AVPacket* input_avpacket)
{
    pv->write(input_avpacket);
}

void FFileWriter::write(std::shared_ptr<av::Packet> pkt)
{
   pv->write(pkt->getAVPacket());
}

void FFileWriter::close()
{
    if (is_open)
        pv->close();
}


}//ZMB

