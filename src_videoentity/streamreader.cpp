#include "streamreader.h"
#include "src/avcpp/packet.h"

namespace ZMBEntities {

StreamReader::StreamReader(const std::string& name) : Poco::Task(name)
{
    memset(&pkt, 0, sizeof(pkt));
    av_init_packet(&pkt);
    should_run = true;
}

bool StreamReader::open(ZConstString url)
{
    int res = ff.open(url);
    assert(0 == res);
    res = ff.get_video_stream_and_codecs();
    assert(0 == res);
    ff.read_play();
    should_run = true;
    return true;
}

StreamReader::~StreamReader()
{
    av_free_packet(&pkt);
    av_init_packet(&pkt);
}

void StreamReader::stop()
{
    should_run = false;
}

void StreamReader::runTask()
{
    while (should_run)
    {
        int res = ff.read_frame(&pkt);
        av::Rational tb(*ff.get_timebase());
        if (res >= 0)
        {
            /*copy contents*/
            auto avpkt = std::make_shared<av::Packet> ();
            avpkt->deep_copy(&pkt);
            avpkt->setTimeBase(tb);
            assert(avpkt->getSize() > 0);
            file_pkt_q->pkt_chan << avpkt;
        }
    }
}

}//ZMBEntities