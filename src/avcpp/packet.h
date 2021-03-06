/*
A video surveillance software with support of H264 video sources.
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
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef PACKET_H
#define PACKET_H

#include <iostream>
#include <vector>

#include "ffmpeg.h"
#include "rational.h"
//#include "stream.h"

namespace av {



class Stream;
typedef std::shared_ptr<Stream> StreamPtr;
typedef std::weak_ptr<Stream> StreamWPtr;


class Packet;
typedef std::shared_ptr<Packet> PacketPtr;
typedef std::weak_ptr<Packet> PacketWPtr;


class Packet
{
private:
    void ctorInitCommon();
    void ctorInitFromAVPacket(const AVPacket *avpacket);


public:
    /** Makes self to contain data copy of the packet, makes no references.*/
    void deep_copy(const AVPacket* pkt);

    /** Allocates and makes deep data copy, if the buffer is reference-counted
     *  -- the exported will be not.
     * @return packet, user takes care about av_free_packet(); */
    AVPacket deep_copy();

    Packet();
    Packet(const Packet &packet);
    Packet(Packet &&packet);
    explicit Packet(const AVPacket *packet);
    explicit Packet(const std::vector<uint8_t> &data);
    Packet(const uint8_t *data, size_t size, bool doAllign = true);
    ~Packet();

    bool setData(const std::vector<uint8_t> &newData);
    bool setData(const uint8_t *newData, size_t size);

    const uint8_t* getData() const;
    uint8_t*       getData();

    const AVPacket *getAVPacket() const { return &m_pkt; }
    AVPacket       *getAVPacket() {return &m_pkt;}

    int64_t getPts() const;
    int64_t getDts() const;
    int64_t getFakePts() const;
    int32_t getSize() const;

    /**
     * Set packet PTS field. It also set fake pts value, so, if you need fake value, you should
     * store it before and restore later. It useful for audio samples: PTS and DTS values in
     * encoded packets in any cases have AV_NOPTS_VALUE!
     *
     * @param pts new presentation timestamp value
     * @param tsTimeBase  is a time base of setted timestamp, can be omited or sets to Rational(0,0)
     *                    that means that time base equal to packet time base.
     */
    void setPts(int64_t pts,     const Rational &tsTimeBase = Rational(0, 0));
    void setDts(int64_t dts,     const Rational &tsTimeBase = Rational(0, 0));
    void setFakePts(int64_t pts, const Rational &tsTimeBase = Rational(0, 0));

    int     getStreamIndex() const;
    bool    isKeyPacket() const;
    int     getDuration() const;
    bool    isComplete() const;

    void    setStreamIndex(int idx);
    void    setKeyPacket(bool keyPacket);
    void    setDuration(int duration, const Rational &durationTimeBase = Rational(0, 0));
    void    setComplete(bool complete);

    // Flags
    int getFlags();
    void        setFlags(int flags);
    void        addFlags(int flags);
    void        clearFlags(int flags);

//    void        dump(const StreamPtr &st, bool dumpPayload = false) const;

    const Rational& getTimeBase() const { return m_timeBase; }
    void setTimeBase(const Rational &value);

    Packet &operator =(const Packet &rhs);
    Packet &operator= (Packet &&rhs);
    Packet &operator =(const AVPacket &rhs);

    void swap(Packet &other);

private:
    int allocatePayload(int32_t   size);
    int reallocatePayload(int32_t newSize);

private:
    AVPacket         m_pkt;
    bool             m_completeFlag;
    Rational         m_timeBase;
    int64_t          m_fakePts;
};



} // ::av


#endif // PACKET_H
