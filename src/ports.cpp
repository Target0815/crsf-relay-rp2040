/*
 * CRSF-Relay for RP2040 - serial ports
 * GPL-3.0-or-later, see crsf.h
 *
 * The STM32 original uses USART + DMA + idle-line interrupts. Here:
 *  - RX: interrupt driven into a ring buffer, frame boundaries are found by the
 *        CRSF parser plus an idle timeout (see main.cpp)
 *  - TX: DMA from a burst buffer
 *  - half duplex: two PIO state machines on one pin; the receiver is switched
 *        off while sending (no echo) and restarted when the burst is complete
 */
#include "ports.h"
#include "config.h"

#include <cstring>

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"

#include "hd_uart.pio.h"

namespace ports {

namespace {

// ------------------------------------------------------------------ input
ByteRing inRing;
volatile uint32_t inLastRx = 0;
volatile uint32_t inFe = 0;
int inDma = -1;
uint8_t inTxBuf[256];

void __not_in_flash_func(uart0Isr)() {
    uart_hw_t* hw = uart_get_hw(uart0);
    bool got = false;
    while (!(hw->fr & UART_UARTFR_RXFE_BITS)) {
        const uint32_t dr = hw->dr;
        if (dr & UART_UARTDR_FE_BITS) {
            ++inFe;
        } else {
            inRing.put(uint8_t(dr));
        }
        got = true;
    }
    if (got) inLastRx = time_us_32();
}

// ------------------------------------------------------------------ relay
ByteRing relayRing;
volatile uint32_t relayLastRx = 0;
volatile uint32_t relayFe = 0;
uint32_t relayFeTaken = 0;
int relayDma = -1;
uint8_t relayTxBuf[256];
uint32_t baudRate = 400'000;
bool half = false;
bool hdTxActive = false;

PIO const hdPio = pio0;
constexpr uint smTx = 0;
constexpr uint smRx = 1;
uint offTx = 0;
uint offRx = 0;

void __not_in_flash_func(uart1Isr)() {
    uart_hw_t* hw = uart_get_hw(uart1);
    bool got = false;
    while (!(hw->fr & UART_UARTFR_RXFE_BITS)) {
        const uint32_t dr = hw->dr;
        if (dr & UART_UARTDR_FE_BITS) {
            ++relayFe;
        } else {
            relayRing.put(uint8_t(dr));
        }
        got = true;
    }
    if (got) relayLastRx = time_us_32();
}

void __not_in_flash_func(pio0Isr)() {
    bool got = false;
    while (!pio_sm_is_rx_fifo_empty(hdPio, smRx)) {
        relayRing.put(uint8_t(hdPio->rxf[smRx] >> 24));
        got = true;
    }
    if (got) relayLastRx = time_us_32();
}

void uartSetup(uart_inst_t* u, uint txPin, uint rxPin, uint32_t baud, irq_handler_t isr, int irqNum) {
    uart_init(u, baud);
    uart_set_format(u, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(u, false, false);
    uart_set_fifo_enabled(u, true);
    gpio_set_function(txPin, GPIO_FUNC_UART);
    gpio_set_function(rxPin, GPIO_FUNC_UART);
    gpio_pull_up(rxPin);
    gpio_pull_up(txPin);
    irq_set_exclusive_handler(irqNum, isr);
    irq_set_priority(irqNum, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(irqNum, true);
    uart_set_irq_enables(u, true, false);
}

void dmaSend(int ch, volatile void* dst, uint dreq, const uint8_t* src, size_t n) {
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, dreq);
    dma_channel_configure(ch, &c, dst, src, n, true);
}

// 1.0.8: polarity of the relay pins. gpio_set_function()/pio_gpio_init() reset the overrides,
// so this is applied after every (re)configuration. Idle level is held by the pull resistor:
// pull-up for normal, pull-down for inverted signals.
bool relayInverted = false;

void applyPolarity(uint pin) {
    const uint ov = relayInverted ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL;
    gpio_set_outover(pin, ov);
    gpio_set_inover(pin, ov);
    if (relayInverted) gpio_pull_down(pin); else gpio_pull_up(pin);
}

void hdRxStart() {
    while (!pio_sm_is_rx_fifo_empty(hdPio, smRx)) (void)hdPio->rxf[smRx];
    pio_sm_restart(hdPio, smRx);
    pio_sm_exec(hdPio, smRx, pio_encode_jmp(offRx));
    pio_interrupt_clear(hdPio, 4);
    pio_sm_set_enabled(hdPio, smRx, true);
}

void enterHalfDuplex() {
    uart_set_irq_enables(uart1, false, false);
    uart_deinit(uart1);
    gpio_init(PIN_RELAY_RX); // unused in this mode
    gpio_set_dir(PIN_RELAY_RX, GPIO_IN);
    gpio_pull_up(PIN_RELAY_RX);

    pio_sm_set_enabled(hdPio, smTx, false);
    pio_sm_set_enabled(hdPio, smRx, false);
    hd_tx_program_init(hdPio, smTx, offTx, PIN_RELAY_TX, baudRate);
    hd_rx_program_init(hdPio, smRx, offRx, PIN_RELAY_TX, baudRate);
    pio_gpio_init(hdPio, PIN_RELAY_TX);
    applyPolarity(PIN_RELAY_TX);
    pio_sm_set_enabled(hdPio, smTx, true);
    hdTxActive = false;
    hdRxStart();
    relayRing.clear();
}

void enterFullDuplex() {
    pio_sm_set_enabled(hdPio, smTx, false);
    pio_sm_set_enabled(hdPio, smRx, false);
    hdTxActive = false;
    uartSetup(uart1, PIN_RELAY_TX, PIN_RELAY_RX, baudRate, uart1Isr, UART1_IRQ);
    applyPolarity(PIN_RELAY_TX);
    applyPolarity(PIN_RELAY_RX);
    relayRing.clear();
}

bool hdTxDone() {
    if (dma_channel_is_busy(relayDma)) return false;
    if (!pio_sm_is_tx_fifo_empty(hdPio, smTx)) return false;
    return pio_sm_get_pc(hdPio, smTx) == offTx + 1; // stalled at "pull block" after release
}

} // namespace

// ------------------------------------------------------------------ input API
void inputInit(uint32_t baud) {
    inDma = dma_claim_unused_channel(true);
    uartSetup(uart0, PIN_IN_TX, PIN_IN_RX, baud, uart0Isr, UART0_IRQ);
}
bool inputRead(uint8_t& c) { return inRing.get(c); }
uint32_t inputIdleUs() { return time_us_32() - inLastRx; }
bool inputBusy() { return dma_channel_is_busy(inDma); }
void inputSend(const uint8_t* p, size_t n) {
    if (n > sizeof(inTxBuf)) n = sizeof(inTxBuf);
    std::memcpy(inTxBuf, p, n);
    dmaSend(inDma, &uart_get_hw(uart0)->dr, uart_get_dreq(uart0, true), inTxBuf, n);
}
uint32_t inputFrameErrors() { return inFe; }
uint32_t inputOverflows() { return inRing.overflows; }

// ------------------------------------------------------------------ relay API
void relayInit(uint32_t baud) {
    baudRate = baud;
    relayDma = dma_claim_unused_channel(true);
    pio_sm_claim(hdPio, smTx);
    pio_sm_claim(hdPio, smRx);
    offTx = pio_add_program(hdPio, &hd_tx_program);
    offRx = pio_add_program(hdPio, &hd_rx_program);
    irq_set_exclusive_handler(PIO0_IRQ_0, pio0Isr);
    irq_set_priority(PIO0_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
    pio_set_irq0_source_enabled(hdPio, pis_sm1_rx_fifo_not_empty, true);
    irq_set_enabled(PIO0_IRQ_0, true);
    half = false;
    enterFullDuplex();
}

void relaySetBaud(uint32_t baud) {
    baudRate = baud;
    if (half) {
        pio_sm_set_clkdiv(hdPio, smTx, hd_uart_clkdiv(baud));
        pio_sm_set_clkdiv(hdPio, smRx, hd_uart_clkdiv(baud));
    } else {
        uart_set_baudrate(uart1, baud);
    }
}
uint32_t relayBaud() { return baudRate; }

void relaySetHalfDuplex(bool on) {
    if (on == half) return;
    if (relayDma >= 0) dma_channel_abort(relayDma);
    half = on;
    if (half) {
        enterHalfDuplex();
    } else {
        enterFullDuplex();
    }
}
bool relayHalfDuplex() { return half; }

void relaySetInverted(bool on) {
    relayInverted = on;
    applyPolarity(PIN_RELAY_TX);
    if (!half) applyPolarity(PIN_RELAY_RX);
    relayRing.clear();
}

void relayPoll() {
    if (!half) return;
    if (pio_interrupt_get(hdPio, 4)) {
        pio_interrupt_clear(hdPio, 4);
        ++relayFe;
    }
    if (hdTxActive && hdTxDone()) {
        hdTxActive = false;
        hdRxStart();
    }
}

bool relayRead(uint8_t& c) { return relayRing.get(c); }
uint32_t relayIdleUs() { return time_us_32() - relayLastRx; }

bool relayBusy() {
    if (half) return hdTxActive;
    return dma_channel_is_busy(relayDma);
}

void relaySend(const uint8_t* p, size_t n) {
    if (n > sizeof(relayTxBuf)) n = sizeof(relayTxBuf);
    std::memcpy(relayTxBuf, p, n);
    if (half) {
        pio_sm_set_enabled(hdPio, smRx, false); // no echo of our own bytes
        hdTxActive = true;
        dmaSend(relayDma, &hdPio->txf[smTx], pio_get_dreq(hdPio, smTx, true), relayTxBuf, n);
    } else {
        dmaSend(relayDma, &uart_get_hw(uart1)->dr, uart_get_dreq(uart1, true), relayTxBuf, n);
    }
}

uint32_t relayFrameErrorsTakeNew() {
    const uint32_t fe = relayFe;
    const uint32_t d = fe - relayFeTaken;
    relayFeTaken = fe;
    return d;
}
uint32_t relayFrameErrorsTotal() { return relayFe; }
uint32_t relayOverflows() { return relayRing.overflows; }

}
