#include "common-defines.h"
#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/memorymap.h>
#include "core/uart.h"
#include "core/system.h"
#include "comms.h"
#include "bl-flash.h"
#include "core/simple-timer.h"

#define BOOTLOADER_SIZE (0x8000U)
#define MAIN_APP_START_ADDRESS (FLASH_BASE + BOOTLOADER_SIZE)
#define FLASH_END_ADDRESS (FLASH_BASE + (256U * 1024U))
#define FLASH_PAGE_SIZE (2048U)
#define SRAM_BASE_ADDRESS (0x20000000U)
#define SRAM_END_ADDRESS (SRAM_BASE_ADDRESS + (64U * 1024U))
#define ENABLE_FLASH_WRITE_TEST (0U)
#define ENABLE_UART_DIAGNOSTICS (0U)
#define MAX_FW_LENGTH (FLASH_END_ADDRESS - MAIN_APP_START_ADDRESS)
#define DEVICE_ID (0x42)
#define SYNC_SEQ_0 (0xc4)
#define SYNC_SEQ_1 (0x55)
#define SYNC_SEQ_2 (0x7e)
#define SYNC_SEQ_3 (0x10)
#define DEFAULT_TIMEOUT (30000)

typedef enum bl_state_t{
	BL_State_Sync,
	BL_State_WaitForUpdateReq,
	BL_State_DeviceIDReq,
	BL_State_DeviceIDRes,
	BL_State_FWLengthReq,
	BL_State_FWLengthRes,
	BL_State_EraseApplication,
	BL_State_ReceiveFirmware,
	BL_State_Done

} bl_state_t;

static bl_state_t state= BL_State_Sync;
static uint32_t fw_length=0;
static uint32_t bytes_written=0;
static bool update_successful = false;
static uint8_t sync_seq[4]={0};


static 	simple_timer_t timer;
static comms_packet_t packet;


static void jump_to_main(void) __attribute__((noreturn));
static bool application_is_valid(void);
static void prepare_to_jump(void);
#if ENABLE_FLASH_WRITE_TEST
static void run_flash_write_test(void);
#endif

static bool application_is_valid(void)
{
	const uint32_t *app_vector_table = (const uint32_t *)MAIN_APP_START_ADDRESS;
	const uint32_t app_stack = app_vector_table[0];
	const uint32_t app_reset = app_vector_table[1];
	const uint32_t app_reset_address = app_reset & ~1U;

	return ((app_stack >= SRAM_BASE_ADDRESS) && (app_stack <= SRAM_END_ADDRESS) &&
		((app_stack & 0x7U) == 0U) && ((app_reset & 1U) != 0U) &&
		(app_reset_address >= MAIN_APP_START_ADDRESS) &&
		(app_reset_address < FLASH_END_ADDRESS));
}

#if ENABLE_UART_DIAGNOSTICS
/* Plain-text diagnostics for viewing in a raw serial terminal (e.g. PuTTY).
 * Keep disabled when using the fw-updater packet protocol: text bytes share
 * the same UART and will desync the host's fixed-length packet parser. */
static void uart_write_string(const char *s)
{
	while (*s) {
		uart_write_byte((uint8_t)*s++);
	}
}
#endif

static void bootloading_fail(void){
	comms_create_single_byte_packet(&packet,BL_PACKET_NACK_DATA0);
					    comms_write(&packet);
						state=BL_State_Done;

}
static void check_for_timeout(void){
if(simple_timer_has_elapsed(&timer)){
						bootloading_fail();
}
}
static bool is_device_id_packet(const comms_packet_t* rx_packet){
	if (rx_packet->length != 2 || rx_packet->data[0] != BL_PACKET_DEVICE_ID_RES_DATA0) {
        return false;
    }

    for (uint8_t i = 2; i < PACKET_DATA_LENGTH; i++) {
        if (rx_packet->data[i] != 0xff) {
            return false;
        }
    }
    return true;

}
static bool is_fw_length_packet(const comms_packet_t* rx_packet){
	if (rx_packet->length != 5 || rx_packet->data[0] != BL_PACKET_FW_LENGTH_RES_DATA0) {
        return false;
    }

    for (uint8_t i = 5; i < PACKET_DATA_LENGTH; i++) {
        if (rx_packet->data[i] != 0xff) {
            return false;
        }
    }
    return true;

}

#if ENABLE_UART_DIAGNOSTICS
static void uart_write_hex32(uint32_t value)
{
	static const char hex_digits[] = "0123456789ABCDEF";
	uart_write_string("0x");
	for (int shift = 28; shift >= 0; shift -= 4) {
		uart_write_byte((uint8_t)hex_digits[(value >> shift) & 0xFU]);
	}
}

static void log_boot_diagnostics(void)
{
	const uint32_t *app_vector_table = (const uint32_t *)MAIN_APP_START_ADDRESS;
	const uint32_t app_stack = app_vector_table[0];
	const uint32_t app_reset = app_vector_table[1];

	uart_write_string("\r\n[bootloader] app_stack=");
	uart_write_hex32(app_stack);
	uart_write_string(" app_reset=");
	uart_write_hex32(app_reset);
	uart_write_string(application_is_valid() ? " valid=YES\r\n" : " valid=NO\r\n");
}
#endif

static void prepare_to_jump(void)
{
	uart_teardown();
	systick_teardown();
}

#if ENABLE_FLASH_WRITE_TEST
static void run_flash_write_test(void)
{
	uint8_t data[1024] = {0};

	for (uint16_t i = 0; i < 1024; i++) {
		data[i] = (uint8_t)(i & 0xffU);
	}

	bl_flash_erase_main_application();
	bl_flash_write(MAIN_APP_START_ADDRESS + (0U * FLASH_PAGE_SIZE), data, 1024);
	bl_flash_write(MAIN_APP_START_ADDRESS + (1U * FLASH_PAGE_SIZE), data, 1024);
	bl_flash_write(MAIN_APP_START_ADDRESS + (2U * FLASH_PAGE_SIZE), data, 1024);
	bl_flash_write(MAIN_APP_START_ADDRESS + (3U * FLASH_PAGE_SIZE), data, 1024);
}
#endif

static void jump_to_main(void){
	typedef void (*void_fn)(void);
	uint32_t *app_vector_table = (uint32_t *)MAIN_APP_START_ADDRESS;
	uint32_t app_stack = app_vector_table[0];
	uint32_t app_reset = app_vector_table[1];
	void_fn jump_fn = (void_fn)app_reset;

	prepare_to_jump();
	SCB_VTOR = MAIN_APP_START_ADDRESS;
	__asm volatile (
		"msr msp, %0    \n"
		"bx  %1         \n"
		:
		: "r" (app_stack), "r" (jump_fn)
		: "memory"
	);
	__builtin_unreachable();
}

int main(void)
{
	system_setup();
	uart_setup();
#if ENABLE_FLASH_WRITE_TEST
	run_flash_write_test();
#endif
#if ENABLE_UART_DIAGNOSTICS
	log_boot_diagnostics();
	uart_write_string("[bootloader] application invalid, waiting for updater\r\n");
#endif
	comms_setup();


	

    simple_timer_setup(&timer,DEFAULT_TIMEOUT,false);
	
    

	while (state!=BL_State_Done) {

		if (state==BL_State_Sync){

			if(uart_data_available()){
				sync_seq[0]=sync_seq[1];
				sync_seq[1]=sync_seq[2];
				sync_seq[2]=sync_seq[3];
				sync_seq[3]=uart_read_byte();

				bool is_match = sync_seq[0]==SYNC_SEQ_0;
				is_match = is_match && (sync_seq[1]==SYNC_SEQ_1);
				is_match = is_match && (sync_seq[2] == SYNC_SEQ_2);
				is_match = is_match && (sync_seq[3] == SYNC_SEQ_3);
				if(is_match){

					uart_flush_rx();
					comms_setup();
					comms_create_single_byte_packet(&packet,BL_PACKET_SYNC_OBSERVED_DATA0);
					comms_write(&packet);
					simple_timer_reset(&timer);
					state=BL_State_WaitForUpdateReq;

				}
				else{
					check_for_timeout();
				}
			}else{
				check_for_timeout();

			}
			continue;

		}
		comms_update();

		switch(state){
			case BL_State_WaitForUpdateReq:{
				if (comms_packets_available()){

					comms_read(&packet);

					if(comms_is_single_byte_packet(&packet,BL_PACKET_FW_UPDATE_REQ_DATA0)){
						  simple_timer_reset(&timer);
                          comms_create_single_byte_packet(&packet,BL_PACKET_FW_UPDATE_RES_DATA0);
						  comms_write(&packet);
						  state=BL_State_DeviceIDReq;
					}
					else{
                        bootloading_fail();
					}

				}else{
                   check_for_timeout();
				}

			}break;
			case BL_State_DeviceIDReq:{
				 simple_timer_reset(&timer);
				comms_create_single_byte_packet(&packet,BL_PACKET_DEVICE_ID_REQ_DATA0);
				comms_write(&packet);
				state=BL_State_DeviceIDRes;

			} break;
			case BL_State_DeviceIDRes :{
					if (comms_packets_available()){

					comms_read(&packet);

					if(is_device_id_packet(&packet) && (packet.data[1]==DEVICE_ID)){
                          simple_timer_reset(&timer);
						  state=BL_State_FWLengthReq;
					}
					else{
                        bootloading_fail();
					}

				}else{
                   check_for_timeout();
				}

			} break;
			case BL_State_FWLengthReq:{
				simple_timer_reset(&timer);
				comms_create_single_byte_packet(&packet,BL_PACKET_FW_LENGTH_REQ_DATA0);
				comms_write(&packet);
				state=BL_State_FWLengthRes;

			} break;
			case BL_State_FWLengthRes:{
					if (comms_packets_available()){

					comms_read(&packet);
					fw_length=(
						((uint32_t)packet.data[1]) |
						((uint32_t)packet.data[2] << 8) |
						((uint32_t)packet.data[3] << 16) |
						((uint32_t)packet.data[4] << 24)
					);

					if(is_fw_length_packet(&packet) && (fw_length > 0U) && (fw_length <= MAX_FW_LENGTH) ){
                          simple_timer_reset(&timer);
						  bytes_written = 0;
						  state=BL_State_EraseApplication;
					}
					else{
                        bootloading_fail();
					}

				}else{
                   check_for_timeout();
				}

			} break;
			case BL_State_EraseApplication:{

				bl_flash_erase_main_application();
				simple_timer_reset(&timer);
				comms_create_single_byte_packet(&packet,BL_PACKET_READY_FOR_DATA_DATA0);
				comms_write(&packet);
				state=BL_State_ReceiveFirmware;


			} break;
			case BL_State_ReceiveFirmware:{
				if(comms_packets_available()){
					comms_read(&packet);
					const uint8_t packet_length = packet.length;
					if ((packet_length == 0U) || ((bytes_written + packet_length) > fw_length)) {
						bootloading_fail();
						break;
					}

					bl_flash_write(MAIN_APP_START_ADDRESS+ bytes_written, packet.data, packet_length );
					bytes_written += packet_length;
					simple_timer_reset(&timer);
					if(bytes_written >= fw_length){
						if (application_is_valid()) {
							update_successful = true;
							comms_create_single_byte_packet(&packet,BL_PACKET_UPDATE_SUCCESSFUL_DATA0);
							comms_write(&packet);
							state=BL_State_Done;
						} else {
							bootloading_fail();
						}
					} else {
						comms_create_single_byte_packet(&packet,BL_PACKET_READY_FOR_DATA_DATA0);
						comms_write(&packet);

					}
				}
				else {
					check_for_timeout();
				}

			} break;
			default: {
				state = BL_State_Sync;
			}

		}
		
	}
	if (update_successful && application_is_valid()) {
		system_delay(150);
		jump_to_main();
	}

	while (1) {
		__asm__("wfi");
	}
}
