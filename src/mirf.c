#include "mirf.h"
#include "nRF24L01.h"
#include "swspi.h"
#include <avr/io.h>
#include <avr/interrupt.h>

// tmp
#include "usart.h"


// maintain a copy of the config register
volatile uint8_t config;

// Initialize all the default settings of the RF module and I/O pins of AVR
void mirf_init() {

    // Set CSN and CE to default
    CHIP_ENABLE_LO();
    CHIP_SELECT_HI();

    // Set IRQ as interrupt
    //PCICR |= _BV(PCIE0);  
    //PCMSK1 |= _BV(PCINT0);
    
    // Enable auto-acknowledgement
    mirf_st(EN_AA, 0x01);
    
    // Configure the retry wait time and number of attempts
    mirf_st(SETUP_RETR, 0x3F); // 3 -> 250 + 250 * X us, F -> 15 attempts
    
    // Choose number of enabled data pipes
    mirf_st(EN_RXADDR, 0x01);
    
    // RF address width setup
    mirf_st(SETUP_AW, 0x03); // 0b0000 0011 -> 5 bytes RF address
    
    // RF channel setup
    mirf_st(RF_CH, 0x03); // 0b0000 0001 -> 2,400-2,527GHz
    
    // Power mode and data speed
    mirf_st(RF_SETUP, 0x06); // 0b0010 0110 -> 250kbps 0dBm (last bit means nothing)
    
    // Set receiver address (same as TX_ADDR since EN_AA is set)
    uint8_t rx_addr[5] = mirf_RX_ADDR;
    mirf_stm(RX_ADDR_P0, rx_addr, 5); // on P0 to match EN_RXADDR setting
    
    // Set length of payload 
    mirf_st(RX_PW_P0, 8);
    
    // Clear all status flags
    mirf_st(STATUS, _BV(RX_DR));
    mirf_st(STATUS, _BV(TX_DS));
    mirf_st(STATUS, _BV(MAX_RT));

    // initialize config
    config = 0x00;
    
    // Enable CRC (forced if EN_AA is enabled)
    config |= _BV(EN_CRC);
    
    // Double byte CRC encoding scheme
    config |= _BV(CRCO);
    
    // Receiver mode by default
    config |= _BV(PRIM_RX);
    
    // Start up the module
    mirf_st(CONFIG, config | _BV(PWR_UP));
}

// Load value from the specified register
uint8_t mirf_ld(uint8_t reg) {
    uint8_t value;
    CHIP_SELECT_LO();
    spi_transfer(R_REGISTER | (REGISTER_MASK & reg));
    _delay_us(10);
    value = spi_transfer(NOP);
    CHIP_SELECT_HI();
    return value;
}

// Stores the given value to a specified register
void mirf_st(uint8_t reg, uint8_t value) {
    CHIP_SELECT_LO();
    spi_transfer(W_REGISTER | (REGISTER_MASK & reg));
    _delay_us(10);
    spi_transfer(value);
    CHIP_SELECT_HI();
}

// Loads multiple values from the given start position in the specified register
void mirf_ldm(uint8_t reg, uint8_t * value, uint8_t len) {
    CHIP_SELECT_LO();
    spi_transfer(R_REGISTER | (REGISTER_MASK & reg));
    _delay_us(10);
    spi_ntransfer(value, value, len);
    CHIP_SELECT_HI();
}

// Stores multiple values at the given start position of the specified register
void mirf_stm(uint8_t reg, uint8_t * value, uint8_t len) {
    CHIP_SELECT_LO();
    spi_transfer(W_REGISTER | (REGISTER_MASK & reg));
    _delay_us(10);
    spi_ntransfer(value, value, len);
    CHIP_SELECT_HI();
}

// Returns true if there is data waiting at incoming queue
uint8_t mirf_ready() {
    uint8_t status;
    CHIP_SELECT_LO();
    status = spi_transfer(NOP);
    CHIP_SELECT_HI();
    return status & _BV(RX_DR);
}

// Reads len bytes from incoming queue into data array
void mirf_read(uint8_t * data, uint8_t len) {

    // Read packet from FIFO queue
    CHIP_SELECT_LO();
    spi_transfer(R_RX_PAYLOAD);
    _delay_us(10);
    spi_ntransfer(data, data, len);
    CHIP_SELECT_HI();
    
    // Clear the incoming package flag
    mirf_st(STATUS, _BV(RX_DR));
    
    // Flush the FIFO queue
    CHIP_SELECT_LO();
    spi_transfer(FLUSH_TX);
    CHIP_SELECT_HI();
}

// Returns true if the maximum retries have been reached for the current transmission
uint8_t mirf_retry_max() {
    uint8_t status;
    CHIP_SELECT_LO();
    status = spi_transfer(NOP);
    CHIP_SELECT_HI();
    return status & _BV(MAX_RT);
}

// Send data from a buffer to the pre-configured receiver address
uint8_t mirf_write(uint8_t* value, uint8_t len) {

    // Flush data from TX queue
    CHIP_SELECT_LO();
    spi_transfer(FLUSH_TX);
    CHIP_SELECT_HI();

    // Flush data from RX queue
    CHIP_SELECT_LO();
    spi_transfer(FLUSH_RX);
    CHIP_SELECT_HI();
    
    // Clear flags from any previous transmission
    mirf_st(STATUS, _BV(TX_DS) | _BV(MAX_RT));
    
    // Enable auto-acknowledgement
    mirf_st(EN_AA, 0x01);
    
    // Set transmitter address (where we are transmitting to)
    uint8_t tx_addr[5] = mirf_TX_ADDR;
    mirf_stm(TX_ADDR, tx_addr, 5);

    // Set receiver address (same as TX_ADDR since EN_AA is set)
    uint8_t rx_addr[5] = mirf_RX_ADDR;
    mirf_stm(RX_ADDR_P0, rx_addr, 5);
    
    // Send payload to RF module
    CHIP_SELECT_LO();
    spi_transfer(W_TX_PAYLOAD);
    _delay_us(10);
    spi_ntransfer(value, value, len);
    CHIP_SELECT_HI();

    // Begin transmission
    TRANSMIT_MODE_ON();
        
    // Set ready flag
    CHIP_ENABLE_HI();
    
    // Wait for transmission to finish
    int timeout = 500;
    uint8_t status, ret;
    while (timeout > 0) {
        status = mirf_ld(STATUS);
        if (status & _BV(MAX_RT)) {
            mirf_st(STATUS, _BV(MAX_RT));
            ret = RESULT_FAILED;
            break;
        } else if (status & _BV(TX_DS)) {
            mirf_st(STATUS, _BV(TX_DS));
            ret = RESULT_SUCCESS;
            break;
        } else if (timeout < 0) {
            ret = RESULT_TIMEOUT;
            break;
        }
        
        timeout -= 10;
        _delay_ms(10);
    }

    // Finish transmission
    CHIP_ENABLE_LO();
    TRANSMIT_MODE_OFF();
    return ret;
}
