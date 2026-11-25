package io.github.abdoeid.phonecam.capture

import org.junit.Assert.assertEquals
import org.junit.Test

class PhoneOrientationTest {
  @Test
  fun staysPutNearTheCurrentPosition() {
    for (raw in listOf(0, 10, 59, 301, 350, 359)) assertEquals(0, nextQuarterTurn(0, raw))
    for (raw in listOf(31, 90, 149)) assertEquals(90, nextQuarterTurn(90, raw))
    for (raw in listOf(211, 270, 329)) assertEquals(270, nextQuarterTurn(270, raw))
  }

  @Test
  fun switchesOnceClearlyTurned() {
    assertEquals(90, nextQuarterTurn(0, 60))
    assertEquals(90, nextQuarterTurn(0, 95))
    assertEquals(270, nextQuarterTurn(0, 300))
    assertEquals(180, nextQuarterTurn(0, 180))
    assertEquals(0, nextQuarterTurn(90, 20))
    assertEquals(0, nextQuarterTurn(270, 340))
    assertEquals(0, nextQuarterTurn(180, 359))
  }

  @Test
  fun restingOnTheDiagonalDoesNotFlip() {
    var turn = 0
    repeat(20) { step ->
      turn = nextQuarterTurn(turn, if (step % 2 == 0) 44 else 47)
      assertEquals(0, turn)
    }
  }

  @Test
  fun lyingFlatKeepsTheLastPosition() {
    assertEquals(270, nextQuarterTurn(270, -1))
  }

  @Test
  fun everyResultIsAQuarterTurn() {
    for (current in listOf(0, 90, 180, 270)) {
      for (raw in 0..359) assertEquals(0, nextQuarterTurn(current, raw) % 90)
    }
  }

  @Test
  fun rotationForOrdinaryLenses() {
    // Upright: the turn the PC always used for a back lens, and the one a front lens needs.
    assertEquals(90, uprightRotation(90, frontFacing = false, quarterTurn = 0))
    assertEquals(270, uprightRotation(270, frontFacing = true, quarterTurn = 0))
    // Sideways either way: a back lens needs none or half a turn, a front lens too.
    assertEquals(180, uprightRotation(90, frontFacing = false, quarterTurn = 90))
    assertEquals(0, uprightRotation(90, frontFacing = false, quarterTurn = 270))
    assertEquals(180, uprightRotation(270, frontFacing = true, quarterTurn = 90))
    assertEquals(0, uprightRotation(270, frontFacing = true, quarterTurn = 270))
  }
}
