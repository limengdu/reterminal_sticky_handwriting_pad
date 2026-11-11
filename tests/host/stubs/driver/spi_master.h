#pragma once
#include <stddef.h>
#include <stdint.h>
typedef void *spi_device_handle_t;
typedef struct {unsigned flags; size_t length,rxlength; const void *tx_buffer; void *rx_buffer; uint8_t rx_data[4];} spi_transaction_t;
#define SPI_TRANS_USE_RXDATA 1
int spi_device_transmit(spi_device_handle_t,spi_transaction_t *);
