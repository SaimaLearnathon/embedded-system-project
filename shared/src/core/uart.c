#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/cm3/nvic.h>
#include <stddef.h>

#include "core/uart.h"  

static volatile uint8_t data_buffer = 0U;
static volatile bool data_available = false;

void uart_setup(void)
{
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_USART1);

	/* BluePill USART1: PA9 = TX, PA10 = RX. */
	gpio_set_mode(GPIO_BANK_USART1_TX, GPIO_MODE_OUTPUT_50_MHZ,
		      GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART1_TX);
	gpio_set_mode(GPIO_BANK_USART1_RX, GPIO_MODE_INPUT,
		      GPIO_CNF_INPUT_FLOAT, GPIO_USART1_RX);

	usart_set_baudrate(USART1, 115200);
	usart_set_databits(USART1, 8);
	usart_set_stopbits(USART1, USART_STOPBITS_1);
	usart_set_mode(USART1, USART_MODE_TX_RX);
	usart_set_parity(USART1, USART_PARITY_NONE);
	usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);
	usart_enable_rx_interrupt(USART1);
	nvic_enable_irq(NVIC_USART1_IRQ);
	usart_enable(USART1);
}

void usart1_isr(void)
{
	const bool overrun_occurred = usart_get_flag(USART1, USART_FLAG_ORE) != 0;
	const bool received_data = usart_get_flag(USART1, USART_FLAG_RXNE) != 0;

	if (received_data || overrun_occurred) {
		data_buffer = (uint8_t)usart_recv(USART1);
		data_available = true;
	}
}

void uart_write(uint8_t *data, const uint32_t length)
{
	if ((data == NULL) || (length == 0U)) {
		return;
	}

	for (uint32_t i = 0; i < length; ++i) {
		uart_write_byte(data[i]);
	}
}

void uart_write_byte(uint8_t data)
{
	usart_send_blocking(USART1, data);
}

uint32_t uart_read(uint8_t *data, const uint32_t length)
{
	if ((data == NULL) || (length == 0U) || !data_available) {
		return 0U;
	}

	*data = data_buffer;
	data_available = false;
	return 1U;
}

uint8_t uart_read_byte(void)
{
	data_available = false;
	return data_buffer;
}

bool uart_data_available(void)
{
	return data_available;
}
