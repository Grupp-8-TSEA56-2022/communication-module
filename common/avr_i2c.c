#include <avr/interrupt.h>
#include <avr/io.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>

#include "avr_i2c.h"

// Slave Receive mode (Master writes, slave reads)

#define I2C_SR_START 0x60  // Own SLA+W has been received; ACK has been returned
#define I2C_SR_DATA 0x80  // Previously addressed with own SLA+W; data has been received; ACK has been returned
#define I2C_SR_DATA_NACK 0x88  // Previously addressed with own SLA+W; data has been received; NOT ACK has been returned
#define I2C_SR_STOP 0xA0  // A STOP condition or repeated START condition has been received while still addressed as Slave

// Slave Transmit mode (Master reads, slave writes)

#define I2C_ST_START 0xA8  // Own SLA+R has been received; ACK has been returned
#define I2C_ST_WROTE 0xB8  // Data byte in TWDR has been transmitted; ACK has been recieved
#define I2C_ST_WROTE_NACK 0xC0  // Data byte in TWDR has been transmitted; NOT ACK has been recieved

volatile uint8_t i2c_in_buffer[32];
volatile int i2c_in_ptr = 0;

volatile uint8_t i2c_out_buffer[32];
volatile int i2c_out_ptr = 0;
volatile int i2c_out_len = 0;
volatile bool i2c_out_data_ready = false;
volatile bool i2c_N_bytes_sent = false;

volatile uint8_t debug = 0;

void I2C_init(uint8_t slave_address) {
    TWAR = slave_address << 1;  // Set slave address
    // Start Slave Listening: Clear TWINT Flag, Enable ACK, Enable TWI, TWI Interrupt Enable
    TWCR = (1<<TWINT) | (1<<TWEA) | (1<<TWEN) | (1<<TWIE);

    // Default initial values
    TWDR = 0x00;
    i2c_new_data = false;
}

void I2C_pack(uint16_t *message_names, uint16_t *messages, int len) {
    cli();
	if (!i2c_out_data_ready) {
		i2c_out_len = 0;
		for (int i=0; i<len; ++i) {
			uint8_t name0 = (uint8_t)((message_names[i] & 0xff00) >> 8);
			uint8_t name1 = (uint8_t)(message_names[i] & 0x00ff);
			uint8_t msg0 = (uint8_t)((messages[i] & 0xff00) >> 8);
			uint8_t msg1 = (uint8_t)(messages[i] & 0x00ff);
			i2c_out_buffer[4*i + 0] = name0;
			i2c_out_buffer[4*i + 1] = name1;
			i2c_out_buffer[4*i + 2] = msg0;
			i2c_out_buffer[4*i + 3] = msg1;
			i2c_out_len += 4;
		}
		i2c_out_data_ready = true;
	}
    sei();
}

int I2C_unpack(uint16_t *message_names, uint16_t *messages) {
    // The arrays must be long enough for all possible message types
    // Use length 16 to be safe
	
	i2c_new_data = false;

    int msg_idx = 0;

    // Translate the 8 bit packages to 16 bit data, and put them in the place for
    for (int i=0; i<i2c_in_ptr/2; i++) {
        uint16_t packet = (uint16_t)(i2c_in_buffer[2*i] << 8) | i2c_in_buffer[2*i+1];
        printf(packet);
        if ((packet & 0xfff0) == 0xfff0) {
            // Message name
            message_names[msg_idx] = packet;
        } else {
            // Message
            messages[msg_idx++] = packet;
        }
    }
    i2c_in_ptr = 0;
    return msg_idx;
}

ISR(TWI_vect) {
    cli();
    uint8_t status_code;
    status_code = TWSR & 0xf8;

    switch (status_code) {

        // Slave Receive Mode
        case I2C_SR_START:
            // SLA+W has been received (slave is now reading)
            TWCR |= (1<<TWINT) | (1<<TWEA) | (1<<TWIE);
            break;
        case I2C_SR_DATA:
            // Data package received. Add to buffer
            i2c_in_buffer[i2c_in_ptr++] = TWDR;
            TWCR |= (1<<TWINT) | (1<<TWEA) | (1<<TWIE);
            break;
        case I2C_SR_STOP:
            // Last byte received
            i2c_new_data = true;
            TWCR |= (1<<TWINT) | (1<<TWIE);
            break;

        // Slave Transmit mode
        case I2C_ST_START:
            if (!i2c_N_bytes_sent) {
                if (i2c_out_data_ready) {
                    // Transmit number of bytes
                    TWDR = i2c_out_len;
                    TWCR |= (1<<TWINT) | (1<<TWIE);
                } else {
                    TWDR = -2;
                    TWCR |= (1<<TWINT) | (1<<TWIE);
                }
            } else {
                // Transmit first byte
                i2c_out_ptr = 0;
                TWDR = i2c_out_buffer[i2c_out_ptr++];
                TWCR |= (1<<TWINT) | (1<<TWEA) | (1<<TWIE);
            }
            break;
        case I2C_ST_WROTE:
            // Byte written, ACK received
            // Write another byte
            if (i2c_out_ptr < i2c_out_len - 1) {
                TWDR = i2c_out_buffer[i2c_out_ptr++];
                TWCR |= (1<<TWINT) | (1<<TWEA) | (1<<TWIE);
            } else if (i2c_out_ptr == i2c_out_len - 1) {
                // Last byte
                TWDR = i2c_out_buffer[i2c_out_ptr++];
                TWCR |= (1<<TWINT) | (1<<TWIE);
                //i2c_N_bytes_sent = false;
            } else {
                // This is unexpected
				debug |= 0x02;
            }
            break;
        case I2C_ST_WROTE_NACK:
            // Byte written, NACK received
            if (!i2c_N_bytes_sent) {
                i2c_N_bytes_sent = true;
                TWCR |= (1<<TWINT) | (1<<TWEA) | (1<<TWIE);
            } else if (i2c_out_ptr == i2c_out_len) {
				i2c_N_bytes_sent = false;
                i2c_out_data_ready = false;
                TWCR |= (1<<TWINT) | (1<<TWEA) | (1<<TWIE);
            } else {
				debug |= 0x04;
            }
            break;

        default:
			// This should not happen, try to reset
			debug |= 0x1;
			i2c_out_data_ready = false;
			i2c_N_bytes_sent = false;
			TWCR |= (1<<TWINT);
			TWCR |= (1<<TWINT) | (1<<TWEA) | (1<<TWEN) | (1<<TWIE);
			TWDR = 0x00;
            break;
    }

    sei();
}
