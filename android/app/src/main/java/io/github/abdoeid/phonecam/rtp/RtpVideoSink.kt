package io.github.abdoeid.phonecam.rtp

import android.util.Log
import io.github.abdoeid.phonecam.transport.VideoPacketSink
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.random.Random

/**
 * Sends the encoder's Annex-B packets to the PC as RTP/H.264 (RFC 6184 packetization) over UDP.
 *
 * Why this exists alongside the framed-TCP transports: UDP lets the PC side apply live-video loss
 * policy -- for a live camera, a late frame is worth less than a dropped one, and bounded latency
 * requires the *option* to drop instead of stalling behind a retransmit. Packetization is
 * standard, so nothing on the wire is PhoneCam-proprietary.
 *
 * Packetization choices, RFC 6184:
 *  - payload type 96 (RTP dynamic), 90 kHz clock, random 16-bit sequence / 32-bit SSRC starts
 *    (RFC 3550 wants those unpredictable, not zero);
 *  - NALs that fit are sent as single NAL unit packets;
 *  - larger NALs are FU-A fragmentation units, with S/E per fragment and the original NAL header
 *    byte reconstructed by the receiver from indicator+FU header;
 *  - the RTP marker bit is set on the last packet of the whole [writePacket] call -- each encoder
 *    output buffer *is* one access unit (docs/wire-protocol.md: multi-NALU frames ship as one
 *    packet), so call boundaries are AU boundaries by construction and the receiver never has to
 *    guess where a frame ends.
 *
 * Lifecycle: one instance = one capture session (CaptureController passes it to H264Encoder),
 * matching the encoder's own one-cycle contract; [close] releases the UDP socket after the
 * session ends.
 */
class RtpVideoSink(
  private val address: InetAddress,
  private val port: Int,
  // Max NAL/fragment bytes per RTP packet. 1400 keeps every datagram's total
  // (12 RTP + 0/2 overhead + IP/UDP headers) under the ~1500-byte MTU, i.e. no
  // IP fragmentation -- a fragmented datagram is lost entirely when any single
  // fragment is lost, which defeats the whole point of small RTP packets.
  private val maxRtpPayload: Int = 1400,
) : VideoPacketSink {
  private val socket = DatagramSocket()
  private val closed = AtomicBoolean(false)

  private var rtpSeq = Random.nextInt(0, 0x10000)
  private val ssrc = Random.nextInt()

  // Reused per packet (one frame can expand into dozens of RTP packets; the
  // largest is exactly RTP header + 2 FU bytes + maxRtpPayload).
  private val scratch = ByteArray(RTP_HEADER + 2 + maxRtpPayload)

  @Throws(IOException::class)
  override fun writePacket(
    type: Byte,
    keyframe: Boolean,
    seq: Int,
    ptsUs: Long,
    payload: ByteArray,
    payloadOffset: Int,
    payloadLength: Int,
  ) {
    if (closed.get()) throw IOException("RtpVideoSink: already closed")

    // 90 kHz RTP clock (RFC 3551 H.264): ptsUs * 90000 / 1e6, integer math.
    // ptsUs saturates well below Long overflow even for multiday sessions.
    val timestamp = ptsUs * 9 / 100

    val spans = splitNalus(payload, payloadOffset, payloadLength)
    if (spans.isEmpty()) return // header-only packet (payloadLen 0) carries nothing on RTP

    val lastSpan = spans.size - 1
    for (index in 0..lastSpan) {
      val nalStart = spans[index][0]
      val nalEnd = spans[index][1]
      val nalLen = nalEnd - nalStart
      val isFinalSpan = index == lastSpan
      val nalHeader = payload[nalStart].toInt() and 0xFF

      if (nalLen <= maxRtpPayload) {
        // Single NAL unit packet: the NAL is shipped verbatim.
        sendRtp(timestamp, marker = isFinalSpan, extra = null, payload, nalStart, nalLen)
      } else {
        // FU-A: original NAL header splits into indicator (F/NRI preserved,
        // type becomes 28) + per-fragment FU header carrying the real type.
        val indicator = (nalHeader and 0xE0) or FU_TYPE
        val realType = nalHeader and 0x1F
        val chunkSize = maxRtpPayload - 2 // indicator + FU header
        val chunks = (nalLen + chunkSize - 1) / chunkSize
        var offset = nalStart + 1 // skip the NAL header byte, folded into extra bytes
        for (fragment in 0 until chunks) {
          val len = minOf(chunkSize, nalEnd - offset)
          val isFirst = fragment == 0
          val isLast = fragment == chunks - 1
          val fuHeader = (if (isFirst) FU_S else 0) or (if (isLast) FU_E else 0) or realType
          sendRtp(
            timestamp, marker = isLast && isFinalSpan,
            extra = byteArrayOf(indicator.toByte(), fuHeader.toByte()),
            payload, offset, len,
          )
          offset += len
        }
      }
    }
  }

  override fun close() {
    if (!closed.compareAndSet(false, true)) return
    socket.close()
  }

  /** Stamps a complete RTP packet into [scratch] and sends it. Single drain thread only. */
  @Throws(IOException::class)
  private fun sendRtp(
    timestamp: Long,
    marker: Boolean,
    extra: ByteArray?,
    payload: ByteArray,
    payloadOffset: Int,
    payloadLength: Int,
  ) {
    rtpSeq = (rtpSeq + 1) and 0xFFFF
    val extraLen = extra?.size ?: 0
    val packetLen = RTP_HEADER + extraLen + payloadLength

    // Fixed header, freshly stamped every packet (still marked bit on the
    // previous packet must never leak into this one).
    // The version lives in the TOP two bits of byte 0 (0b10xxxxxx = 0x80) --
    // not in the low bits, which carry P/X/CC.
    scratch[0] = (RTP_VERSION shl 6).toByte()
    scratch[1] = ((if (marker) MARKER_BIT else 0) or PAYLOAD_TYPE).toByte()
    scratch[2] = ((rtpSeq ushr 8) and 0xFF).toByte()
    scratch[3] = (rtpSeq and 0xFF).toByte()
    scratch[4] = ((timestamp ushr 24) and 0xFF).toByte()
    scratch[5] = ((timestamp ushr 16) and 0xFF).toByte()
    scratch[6] = ((timestamp ushr 8) and 0xFF).toByte()
    scratch[7] = (timestamp and 0xFF).toByte()
    scratch[8] = ((ssrc ushr 24) and 0xFF).toByte()
    scratch[9] = ((ssrc ushr 16) and 0xFF).toByte()
    scratch[10] = ((ssrc ushr 8) and 0xFF).toByte()
    scratch[11] = (ssrc and 0xFF).toByte()
    if (extra != null) System.arraycopy(extra, 0, scratch, RTP_HEADER, extraLen)
    System.arraycopy(payload, payloadOffset, scratch, RTP_HEADER + extraLen, payloadLength)

    socket.send(DatagramPacket(scratch, packetLen, address, port))
  }

  private companion object {
    const val TAG = "RtpVideoSink"
    const val RTP_VERSION = 2
    const val RTP_HEADER = 12
    const val MARKER_BIT = 0x80
    const val PAYLOAD_TYPE = 96 // dynamic, unassigned in RFC 3551
    const val FU_TYPE = 28 // FU-A per RFC 6184
    const val FU_S = 0x80
    const val FU_E = 0x40
  }

  /**
   * Locates Annex-B start codes (3-byte and 4-byte) in payload[off, off+len), returning per-NAL
   * spans as (payloadStart, payloadEndExclusive) pairs indexed into [payload]. The start-code
   * bytes themselves are not included in the span.
   */
  private fun splitNalus(payload: ByteArray, offset: Int, length: Int): List<IntArray> {
    val spans = ArrayList<IntArray>(4)

    // Absorb preceding zeros into the start code: a 4-byte 00 00 00 01 shows up as a 3-byte
    // match whose preceding byte is also 00, and that zero belongs to the delimiter, not the
    // previous NAL. (Encoders emit 3- or 4-byte start codes; this covers both.)
    fun matchStart(pos: Int): Int {
      var lead = pos
      while (lead > offset && payload[lead - 1].toInt() == 0) lead--
      return lead
    }

    var index = offset
    var currentNalStart = -1
    while (index + 2 < offset + length) {
      if (payload[index].toInt() == 0 && payload[index + 1].toInt() == 0 && payload[index + 2].toInt() == 1) {
        if (currentNalStart >= 0) {
          spans.add(intArrayOf(currentNalStart, matchStart(index)))
        }
        currentNalStart = index + 3
        index += 3
      } else {
        index++
      }
    }
    if (currentNalStart >= 0) {
      spans.add(intArrayOf(currentNalStart, offset + length))
    }

    if (spans.isEmpty() && length > 0) {
      // No start codes found: MediaCodec's surface output is Annex-B by contract, so this is a
      // genuinely unexpected shape. Fail loudly rather than ship a NAL that will misassemble on
      // the receiver -- a corrupted frame there costs a keyframe request anyway.
      Log.e(TAG, "payload without Annex-B start codes ($length bytes)")
      throw IOException("RtpVideoSink: payload has no Annex-B start codes")
    }
    return spans
  }
}
