/*
 *  © 2026 Phil Claridge
 *  All rights reserved.
 *
 *  This file is part of DCC-EX
 *
 *  This is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  It is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with CommandStation.  If not, see <https://www.gnu.org/licenses/>.
 */

// Optional RailCom reader for ESP32: the raw bytes of each cutout, read
// from the receive FIFOs of UART1 (Channel 2) and UART2 (Channel 1) at
// the end of the cutout, kept with the packet the cutout followed in a
// ring indexed by cutout number, and served by number over the command
// port with <J RC n>. No decoding is done here: the bytes are passed on
// raw for a client to decode.
//
// The hardware is anything that turns the track's RailCom current into
// a 250 kbit/s serial line on a GPIO, for example a comparator behind a
// current sense resistor. If the line carries data only during the
// RailCom windows (a gated detector, or one only connected during the
// cutout), the UART sees nothing else. Two UARTs are used because one
// FIFO cannot tell the channels apart: the gap between the windows is
// shorter than a character.
//
// Build switches (config.h), all off by default:
//   #define RAILCOM_UART_CH1 39   // GPIO for Channel 1, read by UART2
//   #define RAILCOM_UART_CH2 36   // GPIO for Channel 2, read by UART1
//   #define RAILCOM_UART_INVERT 0 // RX line not inverted (default 1)
// Either channel may be used alone.

#ifndef RailcomUart_h
#define RailcomUart_h
#include "defines.h"

#if defined(RAILCOM_UART_CH1) || defined(RAILCOM_UART_CH2)
#define RAILCOM_UART
#ifndef ARDUINO_ARCH_ESP32
#error RAILCOM_UART_CH1 and RAILCOM_UART_CH2 need an ESP32
#endif
// An RX pin is any ESP32 GPIO that exists: 0 to 39 less 20, 24 and 28
// to 31. 34 to 39 are input only, which is fine for RX.
#define RAILCOM_UART_PIN_OK(p) ((p) >= 0 && (p) <= 39 && (p) != 20 && (p) != 24 && ((p) < 28 || (p) > 31))
#if defined(RAILCOM_UART_CH1) && !RAILCOM_UART_PIN_OK(RAILCOM_UART_CH1)
#error RAILCOM_UART_CH1 is not an ESP32 GPIO
#endif
#if defined(RAILCOM_UART_CH2) && !RAILCOM_UART_PIN_OK(RAILCOM_UART_CH2)
#error RAILCOM_UART_CH2 is not an ESP32 GPIO
#endif
#endif

#if defined(RAILCOM_UART) && defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include "DCCWaveform.h" // for MAX_PACKET_SIZE

// One record per cutout: the packet whose cutout it was, and the bytes
// the two UARTs held when the cutout ended.
struct RailcomUartRecord {
  uint32_t seq;                    // cutout number since boot
  byte pktLen;                     // 0: the station was sending its own idle
  byte pkt[MAX_PACKET_SIZE + 1];   // packet bytes including the checksum
  byte n1, n2;                     // bytes read from Channel 1 and Channel 2
  byte ch1[4];                     // Channel 1 holds 2 bytes; room for noise
  byte ch2[8];                     // Channel 2 holds 6 bytes; room for noise
  // Bit 0/1: framing error on Channel 1/2 in this cutout. Bit 2/3:
  // Channel 1/2 had more bytes than the record holds, or the UART's own
  // FIFO overflowed, so what is stored is the head of a longer run.
  byte flags;
};

class RailcomUart {
 public:
  // Records written so far. The newest is count()-1, the oldest still
  // held is first(). get() copies one record by number and returns false
  // if it has been overwritten or not yet written.
  static uint32_t count();
  static uint32_t first();
  static bool get(uint32_t seq, RailcomUartRecord &r);
  // The loco or special address the record's packet was sent to: 1-127
  // short, long addresses as a number, 253 XDCC, 254 DCC-A, 255 idle,
  // 0 broadcast; the first byte for anything else. Long addresses 253
  // to 255 are therefore not told apart from the special addresses, and
  // broadcast cannot be selected by <J RC n a>, because a = 0 means any.
  static uint16_t address(const RailcomUartRecord &r);
  // <J RC>: trailer only. <J RC n>, <J RC n a>: records from n, then
  // the trailer. addr 0 means any address.
  static void poll(Print *stream, int16_t n, uint16_t addr, bool fromStart);
  // Send records with no bytes as well; off at boot.
  static bool sendEmpty;

  // Called from DCCRMT.cpp only: set-up before the cutout interrupt is
  // enabled; the packet pipeline; the two cutout interrupts.
  static void begin();
  static void notePending(const byte buffer[], byte byteCount);
  static void IRAM_ATTR packetEnded(bool idleNext, bool newNext);
  static void IRAM_ATTR cutoutStarted();
  static void IRAM_ATTR capture();
};

#endif
#endif
