#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/cm3/nvic.h>
#include <stddef.h>

#include "core/uart.h"  
#include "core/ring-buffer.h"
#define RING_BUFFER_SIZE (64)
static volatile uint8_t data_buffer[RING_BUFFER_SIZE] = {0U};

static ring_buffer_t rb = {0U};
static bool uart_is_setup = false;

void uart_setup(void)
{
	if (!ring_buffer_setup(&rb, data_buffer, RING_BUFFER_SIZE)) {
		return;
	}
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
	uart_is_setup = true;
}

void uart_teardown(void)
{
	if (!uart_is_setup) {
		return;
	}

	while (usart_get_flag(USART1, USART_FLAG_TC) == 0) {
		/* Wait until the last byte has fully left the shift register. */
	}

	usart_disable_rx_interrupt(USART1);
	nvic_disable_irq(NVIC_USART1_IRQ);
	nvic_clear_pending_irq(NVIC_USART1_IRQ);

	usart_disable(USART1);
	gpio_set_mode(GPIO_BANK_USART1_TX, GPIO_MODE_INPUT,
		      GPIO_CNF_INPUT_FLOAT, GPIO_USART1_TX);
	gpio_set_mode(GPIO_BANK_USART1_RX, GPIO_MODE_INPUT,
		      GPIO_CNF_INPUT_FLOAT, GPIO_USART1_RX);
	rcc_periph_clock_disable(RCC_USART1);

	rb.read_index = 0U;
	rb.write_index = 0U;
	uart_is_setup = false;
}

void uart_flush_rx(void)
{
	if (!uart_is_setup) {
		return;
	}

	usart_disable_rx_interrupt(USART1);
	while (usart_get_flag(USART1, USART_FLAG_RXNE) != 0) {
		(void)usart_recv(USART1);
	}
	rb.read_index = rb.write_index;
	usart_enable_rx_interrupt(USART1);
}

void usart1_isr(void)
{
	const bool overrun_occurred = usart_get_flag(USART1, USART_FLAG_ORE) != 0;
	const bool received_data = usart_get_flag(USART1, USART_FLAG_RXNE) != 0;

	if (received_data || overrun_occurred) {

		if(!ring_buffer_write(&rb , (uint8_t) usart_recv(USART1))){
			//handle failure 
		}
		
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
	if ((data == NULL) || (length == 0U)) {
		return 0U;
	}
	for(uint32_t i =0 ;i<length;i++){
		if(!ring_buffer_read(&rb, &data[i])){
        return i;
		}
	}
	return length;

}

uint8_t uart_read_byte(void)
{
	uint8_t byte = 0;
	(void)uart_read(&byte,1);
	return byte ; 

}

bool uart_data_available(void)
{
	return !ring_buffer_empty(&rb);
}
