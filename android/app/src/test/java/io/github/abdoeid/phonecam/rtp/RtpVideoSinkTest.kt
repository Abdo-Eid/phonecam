package io.github.abdoeid.phonecam.rtp

import io.github.abdoeid.phonecam.transport.WireFraming
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import org.junit.Assert.*
import org.junit.Test

class RtpVideoSinkTest {
  @Test fun repeatsConfigWithEveryKeyframeAtThePictureTimestamp() {
    DatagramSocket(0).use { receiver ->
      receiver.soTimeout = 1000
      val sink = RtpVideoSink(InetAddress.getLoopbackAddress(), receiver.localPort)
      try {
        val config = byteArrayOf(0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0x12)
        val frame = byteArrayOf(0, 0, 0, 1, 0x65, 0x80.toByte())
        sink.writePacket(WireFraming.TYPE_CONFIG, true, 0, 0, config, 0, config.size)
        repeat(2) { index ->
          val pts = 44_000_000_000L + index * 1_000_000L
          sink.writePacket(WireFraming.TYPE_FRAME, true, index + 1, pts, frame, 0, frame.size)
          for (nal in listOf(7, 8, 5)) {
            val packet = DatagramPacket(ByteArray(2048), 2048)
            receiver.receive(packet)
            val data = packet.data
            assertEquals(nal, data[12].toInt() and 0x1f)
            assertEquals(nal == 5, data[1].toInt() and 0x80 != 0)
            var timestamp = 0L
            for (i in 4..7) timestamp = (timestamp shl 8) or (data[i].toLong() and 255)
            assertEquals((pts * 9 / 100) and 0xffffffffL, timestamp)
          }
        }
      } finally { sink.close() }
    }
  }

  @Test fun exactFragmentBoundaryDoesNotSendAnEmptyFragment() {
    DatagramSocket(0).use { receiver ->
      receiver.soTimeout = 1000
      val sink = RtpVideoSink(InetAddress.getLoopbackAddress(), receiver.localPort)
      try {
        val frame = ByteArray(4 + 1 + 1398 * 2) { 0x55 }
        byteArrayOf(0, 0, 0, 1, 0x65).copyInto(frame)
        sink.writePacket(WireFraming.TYPE_FRAME, true, 0, 1000, frame, 0, frame.size)
        repeat(2) { index ->
          val packet = DatagramPacket(ByteArray(2048), 2048)
          receiver.receive(packet)
          assertEquals(1412, packet.length)
          assertEquals(index == 1, packet.data[1].toInt() and 0x80 != 0)
          assertEquals(index == 1, packet.data[13].toInt() and 0x40 != 0)
        }
      } finally { sink.close() }
    }
  }
}
