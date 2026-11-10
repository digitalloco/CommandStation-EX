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

#include "RailcomUart.h"
#if defined(RAILCOM_UART) && defined(ARDUINO_ARCH_ESP32)
#include "driver/uart.h"
#include "hal/uart_ll.h"
#include "DIAG.h"
#include "DCCRMT.h"   // RMTChannel::railcomOnMain
#include "StringFormatter.h"

#ifndef RAILCOM_UART_INVERT
#define RAILCOM_UART_INVERT 1
#endif

// Ring size in records: 28 bytes each with MAX_PACKET_SIZE 5, so about
// 14 kB of RAM, and about 4 s of cutouts at the usual packet rate.
#define RAILCOM_UART_RING 512
// Reply cap in bytes. EXNetwork::loop() copies each outbound message
// into a variable-length array on the loop task's 8 kB stack before
// handing it to lwIP, and a reply of about 4 kB crashed the station.
// Capped by bytes, since a record with a long packet is twice the
// length of one with a speed packet.
#define RAILCOM_UART_REPLY_BYTES 3000
// "<jRC seq pkt ch1 ch2 flags>": 10 digits, the packet in hex, 8 and 16
// hex digits for the channels, the flags, spaces and brackets.
#define RAILCOM_UART_LINE (48 + 2 * (MAX_PACKET_SIZE + 1))

// Concurrency: the writer, capture(), runs in the MCPWM interrupt, and
// every reader runs in loop(), both on core 1, so a reader can be
// interrupted by the writer but not the other way round. get() copies
// a record with interrupts off, which is enough on one core. The packet
// pipeline below has the same single writer per variable.

// The packet in flight. notePending() copies what RMTfillData() was
// given into pending. packetEnded(), at the end of each packet, moves
// loaded (the packet that just finished, loaded one interrupt earlier)
// into current and pending into loaded. capture() at the end of the
// cutout then pairs current with the UART bytes. A repeat leaves
// loaded as it is; an idle clears it.
static byte pending[MAX_PACKET_SIZE + 1], pendingLen = 0;
static byte loaded[MAX_PACKET_SIZE + 1], loadedLen = 0;
static byte current[MAX_PACKET_SIZE + 1], currentLen = 0;

// Slot seq % RAILCOM_UART_RING holds record seq until record
// seq + RAILCOM_UART_RING overwrites it.
static volatile uint32_t written = 0;
static RailcomUartRecord ring[RAILCOM_UART_RING];
bool RailcomUart::sendEmpty = false;

uint32_t RailcomUart::count() { return written; }

uint32_t RailcomUart::first() {
  uint32_t c = written;
  return c > RAILCOM_UART_RING ? c - RAILCOM_UART_RING : 0;
}

bool RailcomUart::get(uint32_t seq, RailcomUartRecord &r) {
  bool ok = false;
  noInterrupts();
  if (seq < written && seq >= first()) {
    r = ring[seq % RAILCOM_UART_RING];
    ok = true;
  }
  interrupts();
  return ok;
}

void RailcomUart::notePending(const byte buffer[], byte byteCount) {
  // Called with interrupts off, beside dataReady being set.
  pendingLen = byteCount > sizeof(pending) ? sizeof(pending) : byteCount;
  memcpy(pending, buffer, pendingLen);
}

void IRAM_ATTR RailcomUart::packetEnded(bool idleNext, bool newNext) {
  currentLen = loadedLen;
  memcpy(current, loaded, sizeof(current));
  if (idleNext) {
    loadedLen = 0;
  } else if (newNext) {
    loadedLen = pendingLen;
    memcpy(loaded, pending, sizeof(loaded));
  }
}

// Reads up to max bytes from a UART's receive FIFO into buf, and
// empties the rest if there was more. Returns the number read; *over
// is set if bytes were dropped here or by the UART itself.
static byte IRAM_ATTR drain(uart_dev_t *uart, byte *buf, byte max, bool *over) {
  uint32_t n = uart_ll_get_rxfifo_len(uart);
  byte take = n > max ? max : n;
  uart_ll_read_rxfifo(uart, buf, take);
  if (n > take) uart_ll_rxfifo_rst(uart);
  *over = n > take || uart->int_raw.rxfifo_ovf;
  return take;
}

static void IRAM_ATTR clearFlags(uart_dev_t *uart) {
  uart->int_clr.frm_err = 1;
  uart->int_clr.rxfifo_ovf = 1;
}

// Empties both FIFOs and clears their flags.
static void IRAM_ATTR discard() {
#ifdef RAILCOM_UART_CH1
  uart_ll_rxfifo_rst(&UART2);
  clearFlags(&UART2);
#endif
#ifdef RAILCOM_UART_CH2
  uart_ll_rxfifo_rst(&UART1);
  clearFlags(&UART1);
#endif
}

// At the cutout start, before any decoder can answer (the earliest is
// 80 us after the end bit, and this runs about 28 us after it), throw
// away whatever an ungated detector has seen of the driven packet, so
// that a record holds only what arrived during the cutout. A gated
// detector has nothing to throw away. If this interrupt runs late the
// head of Channel 1 is lost with the junk; the cutout itself has the
// same dependence on this interrupt being on time.
void IRAM_ATTR RailcomUart::cutoutStarted() {
  discard();
}

void IRAM_ATTR RailcomUart::capture() {
  // Not DCCWaveform::isRailcom(), which is always true on ESP32: see
  // RMTChannel::railcomOnMain in DCCRMT.h.
  if (!RMTChannel::railcomOnMain) {
    discard();                              // no cutout: nothing to read
    return;
  }
  uint32_t seq = written;
  RailcomUartRecord &r = ring[seq % RAILCOM_UART_RING];
  r.seq = seq;
  r.pktLen = currentLen;
  memcpy(r.pkt, current, sizeof(r.pkt));
  r.n1 = 0;
  r.n2 = 0;
  r.flags = 0;
  bool over;
#ifdef RAILCOM_UART_CH1
  r.n1 = drain(&UART2, r.ch1, sizeof(r.ch1), &over);
  if (UART2.int_raw.frm_err) r.flags |= 1;
  if (over) r.flags |= 4;
  clearFlags(&UART2);
#endif
#ifdef RAILCOM_UART_CH2
  r.n2 = drain(&UART1, r.ch2, sizeof(r.ch2), &over);
  if (UART1.int_raw.frm_err) r.flags |= 2;
  if (over) r.flags |= 8;
  clearFlags(&UART1);
#endif
  written = seq + 1;
}

uint16_t RailcomUart::address(const RailcomUartRecord &r) {
  if (r.pktLen == 0) return 255;            // the station's own idle
  byte b = r.pkt[0];
  if (b >= 192 && b <= 231 && r.pktLen > 1) return ((b & 0x3F) << 8) | r.pkt[1];
  return b;                                 // short, accessory, 253, 254, 255, 0
}

static char *toHex(char *p, const byte *b, byte n) {
  static const char digits[] = "0123456789ABCDEF";
  if (n == 0) {
    *p++ = '-';
    return p;
  }
  for (byte i = 0; i < n; i++) {
    *p++ = digits[b[i] >> 4];
    *p++ = digits[b[i] & 15];
  }
  return p;
}

// "<jRC seq pkt ch1 ch2 flags>" into buf, RAILCOM_UART_LINE bytes.
static void format(const RailcomUartRecord &r, char *buf) {
  char *p = buf + sprintf(buf, "<jRC %lu ", (unsigned long)r.seq);
  p = toHex(p, r.pkt, r.pktLen);
  *p++ = ' ';
  p = toHex(p, r.ch1, r.n1);
  *p++ = ' ';
  p = toHex(p, r.ch2, r.n2);
  sprintf(p, " %d>", r.flags);
}

// The parser hands n as a signed 16-bit value, so n is taken modulo
// 32768 and resolved to the newest record number not after the count
// that agrees with it. A client more than 32768 cutouts behind has lost
// far more than the ring holds anyway.
void RailcomUart::poll(Print *stream, int16_t n, uint16_t addr, bool fromStart) {
  uint32_t total = count();
  uint32_t oldest = first();
  uint32_t from = total;
  if (fromStart) {
    uint32_t want = (uint16_t)n & 0x7FFF;
    from = total - ((total - want) & 0x7FFF);
    if (from > total) from = total;         // want beyond the count: nothing yet
  }
  uint32_t dropped = from < oldest ? oldest - from : 0;
  if (from < oldest) from = oldest;
  uint32_t sent = 0, empty = 0, last = from;
  char line[RAILCOM_UART_LINE];
  RailcomUartRecord r;
  for (uint32_t s = from; s < total && sent < RAILCOM_UART_REPLY_BYTES; s++) {
    if (!get(s, r)) {
      dropped++;
      continue;
    }
    last = s + 1;
    if (addr && address(r) != addr) continue;
    if (r.n1 == 0 && r.n2 == 0) {
      empty++;
      if (!sendEmpty) continue;
    }
    format(r, line);
    StringFormatter::send(stream, F("%s\n"), line);
    sent += strlen(line) + 1;
  }
  // first: oldest record still held; last: the next number to ask for;
  // empty: cutouts in the range with no bytes; dropped: asked for but gone.
  StringFormatter::send(stream, F("<jRC* %L %L %L %L>\n"),
                        (unsigned long)oldest, (unsigned long)last,
                        (unsigned long)empty, (unsigned long)dropped);
}

// Receive only, 250 kbit/s 8N1, no driver installed, so nothing empties
// the FIFOs but capture().
static void uartSetup(uart_port_t port, int pin) {
  uart_config_t cfg = {};
  cfg.baud_rate = 250000;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_APB;
  ESP_ERROR_CHECK(uart_param_config(port, &cfg));
  ESP_ERROR_CHECK(uart_set_pin(port, UART_PIN_NO_CHANGE, pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  if (RAILCOM_UART_INVERT)
    ESP_ERROR_CHECK(uart_set_line_inverse(port, UART_SIGNAL_RXD_INV));
}

void RailcomUart::begin() {
#ifdef RAILCOM_UART_CH1
  uartSetup(UART_NUM_2, RAILCOM_UART_CH1);
  uart_ll_rxfifo_rst(&UART2);
  clearFlags(&UART2);
  DIAG(F("RailCom Channel 1 on UART2 from GPIO %d%s"), RAILCOM_UART_CH1,
       RAILCOM_UART_INVERT ? " inverted" : "");
#endif
#ifdef RAILCOM_UART_CH2
  uartSetup(UART_NUM_1, RAILCOM_UART_CH2);
  uart_ll_rxfifo_rst(&UART1);
  clearFlags(&UART1);
  DIAG(F("RailCom Channel 2 on UART1 from GPIO %d%s"), RAILCOM_UART_CH2,
       RAILCOM_UART_INVERT ? " inverted" : "");
#endif
}

#endif
