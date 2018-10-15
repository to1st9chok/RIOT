/*
 * Copyright (C) 2018 Unwired Devices [info@unwds.com]
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @defgroup
 * @ingroup
 * @brief
 * @{
 * @file        umdk-st95.c
 * @brief       umdk-st95 module implementation
 * @author      Mikhail Perkov

 */

#ifdef __cplusplus
extern "C" {
#endif

/* define is autogenerated, do not change */
#undef _UMDK_MID_
#define _UMDK_MID_ UNWDS_ST95_MODULE_ID

/* define is autogenerated, do not change */
#undef _UMDK_NAME_
#define _UMDK_NAME_ "st95"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

#include "periph/gpio.h"
#include "periph/spi.h"
#include "periph/uart.h"
#include "board.h"

#include "umdk-ids.h"
#include "unwds-common.h"
#include "include/umdk-st95.h"

#include "thread.h"
#include "xtimer.h"
#include "rtctimers-millis.h"

#define ENABLE_DEBUG (1)
#include "debug.h"

static msg_t msg_rx;

static umdk_st95_config_t umdk_st95_config = { ST95_IFACE_UART, ST95_MODE_READER, ISO14443A_SELECT };

static st95_params_t st95_params = { .spi = UMDK_ST95_SPI_DEV, .cs_spi = UMDK_ST95_SPI_CS, \
                                     .uart = UMDK_ST95_UART_DEV, \
                                     .irq_in = UMDK_ST95_IRQ_IN, .irq_out = UMDK_ST95_IRQ_OUT, \
                                     .ssi_0 = UMDK_ST95_SSI_0, .ssi_1 = UMDK_ST95_SSI_1 };
static kernel_pid_t radio_pid;
// static uwnds_cb_t *callback;
static rtctimers_millis_t detect_timer;

static xtimer_t rx_timer;

static uint8_t rxbuf[30];
static uint8_t txbuf[30];

static uint8_t dac_data_h = 0x00;
static uint8_t dac_data_l = 0x00;


static volatile uint8_t num_bytes_rx = 0;
static volatile uint8_t uart_rx = 0;

static uint8_t uid_full[15];
static uint8_t uid_size = 0;

static uint8_t current_cmd = 0;

static uint8_t (*st95_send)(uint8_t) = NULL;
// static uint8_t (*st95_handler)(uint8_t) = NULL;

static uint8_t protocol = 0;

static volatile st95_pack_state_t current_state = UMDK_ST95_PACK_ERROR;
static volatile st95_rx_state_t flag_rx = UMDK_ST95_NOT_RECIEVED;

// uint8_t send_data[10] = {0x80, 0x08, 0x90, 0xAB, 0x85, 0xD7, 0x69, 0x28, 0x00, 0x00 };

static void _check_respose(void);

static uint8_t st95_select_iface(uint8_t iface);

static uint8_t send_pack(uint8_t len);
static uint8_t st95_send_uart(uint8_t length);
static uint8_t st95_send_spi(uint8_t length);

// static uint32_t t1 = 0;
// static uint32_t t2 = 0;


// static void st95_reset_spi(void);
static uint8_t st95_cmd_echo(void);
static uint8_t st95_cmd_idn(void);
static uint8_t st95_cmd_idle(void);
static uint8_t st95_calibration(void);

static uint8_t st95_cmd_select_protocol(void);
static uint8_t st95_select_iso15693(void);
static uint8_t st95_select_field_off(void);
static uint8_t st95_select_iso14443a(void);
static uint8_t st95_select_iso14443b(void);
static uint8_t st95_select_iso18092(void);

static void st95_cmd_send_receive(uint8_t *data, uint8_t size, uint8_t topaz, uint8_t splitFrame, uint8_t crc, uint8_t sigBits);
static void st95_send_irqin_negative_pulse(void);

static bool st95_check_pack(uint8_t length);

static void _send_reqa(void);
static uint8_t _get_uidsize(uint8_t uid_byte);

static void _anticollision_1(void);
static void _anticollision_2(void);
static void _anticollision_3(void);
static void _select_1(uint8_t num, uint8_t * uid_sel);
static void _select_2(uint8_t num, uint8_t * uid_sel);
static void _select_3(uint8_t num, uint8_t * uid_sel);

static bool _check_bcc(uint8_t length, uint8_t * data, uint8_t bcc);
// static bool _check_crc(uint8_t length, uint8_t * buf);


#if ENABLE_DEBUG
    #define PRINTBUFF _printbuff
    static void _printbuff(uint8_t *buff, unsigned len)
    {
        while (len) {
            len--;
            printf("%02X ", *buff++);
        }
        printf("\n");
    }
#else
    #define PRINTBUFF(...)
#endif

static bool _check_bcc(uint8_t length, uint8_t * data, uint8_t bcc)
{
    uint8_t bcc_tmp = 0;

	for (uint8_t i = 0; i < length; i++) {
        bcc_tmp	^= data[i];
    }
	
	if((bcc_tmp ^ bcc) == 0) {
		return true;
    }
	else {
		return false;
    }
}

static void detect_handler(void *arg)
{
    (void) arg;

    // t1 = xtimer_now_usec();

    memset(rxbuf, 0x00, sizeof(rxbuf));
    current_cmd = ST95_CMD_IDLE;

    flag_rx = UMDK_ST95_NOT_RECIEVED;
    current_state = UMDK_ST95_PACK_ERROR;

    st95_cmd_idle();
}

static uint8_t st95_cmd_select_protocol(void)
{
    msg_rx.type = UMDK_ST95_MSG_PROTOCOL;
    current_cmd = ST95_CMD_PROTOCOL;

    if(protocol & ISO14443A_SELECT) {
        // puts("        [ ISO 14443 A ]");
        st95_select_iso14443a();
    }
    if(protocol & ISO14443B_SELECT) {
        puts("        [ ISO 14443 B ]");
        // st95_select_iso14443b();
    }
    if(protocol & ISO15693_SELECT) {
        puts("        [ ISO 15693 ]");
        // st95_select_iso15693();
    }
    if(protocol & ISO18092_SELECT) {
        puts("        [ ISO 18092 ]");
        st95_select_iso18092();
    }

    if(protocol == NO_SELECT_PROTOCOL) {
        puts("No selected protocol");
        return 0;
    }

    return 1;
}

static void _send_reqa(void)
{
	uint8_t data = 0x26;
    
    msg_rx.type = UMDK_ST95_MSG_GET_UID;
    msg_rx.content.value = ISO14443A_REQA_MSG;
    
	/* 1 byte data, Not used topaz format, not SplitFrame, Not aapend CRC, 7 significant bits in last byte */
	st95_cmd_send_receive(&data, 1, 0, 0, 0, 7);
}

static void _anticollision_1(void)
{
	uint8_t data[2] = { ISO14443A_SELECT_LVL1, 0x20 };
    
    msg_rx.type = UMDK_ST95_MSG_GET_UID;
    msg_rx.content.value = ISO14443A_ANTICOL_1_MSG;
    
	/* 2 byte data, Not used topaz format, not SplitFrame, Not aapend CRC, 8 significant bits in last byte */
	st95_cmd_send_receive(data, 2, 0, 0, 0, 8);
}

static void _anticollision_2(void)
{
	uint8_t data[2] = { ISO14443A_SELECT_LVL2, 0x20 };
    
    msg_rx.type = UMDK_ST95_MSG_GET_UID;
    msg_rx.content.value = ISO14443A_ANTICOL_2_MSG;
    
	/* 2 byte data, Not used topaz format, not SplitFrame, Not aapend CRC, 8 significant bits in last byte */
	st95_cmd_send_receive(data, 2, 0, 0, 0, 8);
}

static void _anticollision_3(void)
{
	uint8_t data[2] = { ISO14443A_SELECT_LVL3, 0x20 };
    
    msg_rx.type = UMDK_ST95_MSG_GET_UID;
    msg_rx.content.value = ISO14443A_ANTICOL_3_MSG;
    
	/* 2 byte data, Not used topaz format, not SplitFrame, Not aapend CRC, 8 significant bits in last byte */
	st95_cmd_send_receive(data, 2, 0, 0, 0, 8);
}

static void _select_1(uint8_t num, uint8_t * uid_sel)
{
	uint8_t data[16] = { 0 };
    
    msg_rx.type = UMDK_ST95_MSG_GET_UID;
    msg_rx.content.value = ISO14443A_SELECT_1_MSG;
	
	data[0] = ISO14443A_SELECT_LVL1;
	data[1] = 0x70;
	memcpy(data + 2, uid_sel, num);
	
	/* 2 byte data, Not used topaz format, not SplitFrame, Append CRC, 8 significant bits in last byte */
	st95_cmd_send_receive(data, num + 2, 0, 0, 1, 8);
}

static void _select_2(uint8_t num, uint8_t * uid_sel)
{
	uint8_t data[16] = { 0 };
    
    msg_rx.type = UMDK_ST95_MSG_GET_UID;
    msg_rx.content.value = ISO14443A_SELECT_2_MSG;
	
	data[0] = ISO14443A_SELECT_LVL2;
	data[1] = 0x70;
	memcpy(data + 2, uid_sel, num);
	
	/* 2 byte data, Not used topaz format, not SplitFrame, Append CRC, 8 significant bits in last byte */
	st95_cmd_send_receive(data, num + 2, 0, 0, 1, 8);
}

static void _select_3(uint8_t num, uint8_t * uid_sel)
{
	uint8_t data[16] = { 0 };
    
    msg_rx.type = UMDK_ST95_MSG_GET_UID;
    msg_rx.content.value = ISO14443A_SELECT_3_MSG;
	
	data[0] = ISO14443A_SELECT_LVL3;
	data[1] = 0x70;
	memcpy(data + 2, uid_sel, num);
	
	/* 2 byte data, Not used topaz format, not SplitFrame, Append CRC, 8 significant bits in last byte */
	st95_cmd_send_receive(data, num + 2, 0, 0, 1, 8);
}

static uint8_t _get_uidsize(uint8_t uid_byte)
{
	uint8_t size = 0;
	
	size = (uid_byte >> 6) & 0x03;
	return size;
}

static uint8_t _is_uid_complete(uint8_t sak_byte)
{
	if ((sak_byte & 0x04) == 0x04)
		return 0;
	else 
		return 1; 
}


static void st95_cmd_send_receive(uint8_t *data, uint8_t size, uint8_t topaz, uint8_t split_frame, uint8_t crc, uint8_t sign_bits) 
{
	uint8_t length = 0;
    current_cmd = ST95_CMD_SEND_RECV;

    txbuf[0] = ST95_CMD_SEND_RECV;
    txbuf[1] = size + 1;
	
	memcpy(txbuf + 2, data, size);
	length = size + 2;
	
	txbuf[length] = (topaz << 7) | (split_frame << 6) | (crc << 5) | sign_bits;
	length++;
    
		// DEBUG("		Send: ");
		// PRINTBUFF(txbuf, length);
	
	send_pack(length);
}

static uint8_t st95_cmd_idle(void)
{
    msg_rx.type = UMDK_ST95_MSG_IDLE;

    txbuf[0] = ST95_CMD_IDLE;    // Command
    txbuf[1] = 14;                // Data Length
    /* Idle params */
        /* Wake Up Source */
    txbuf[2] = 0x02;            // Tag Detection
        /* Enter Control (the resources when entering WFE mode)*/
    txbuf[3] = 0x21;            // Tag Detection
    txbuf[4] = 0x00;            //
        /* Wake Up Control (the wake-up resources) */
    txbuf[5] = 0x79;            // Tag Detection
    txbuf[6] = 0x01;            //
        /* Leave Control (the resources when returning to Ready state)*/
    txbuf[7] = 0x18;            // Tag Detection
    txbuf[8] = 0x00;            //
        /* Wake Up Period (the time allowed between two tag detections) */
    txbuf[9] = 0x20;            // Typical value 0x20
        /* Osc Start (the delay for HFO stabilization) */
    txbuf[10] = 0x60;            // Recommendeded value 0x60
        /* DAC Start (the delay for DAC stabilization) */
    txbuf[11] = 0x60;            // Recommendeded value 0x60
        /* DAC Data */
    txbuf[12] = dac_data_l;     //0x42;            // DacDataL
    txbuf[13] = dac_data_h;     //0xFC;            // DacDataH
        /* Swing Count */
    txbuf[14] = 0x3F;            // Recommendeded value 0x3F
        /* Max Sleep */
    txbuf[15] = 0x08;            // Typical value 0x28

    send_pack(16);

    return 1;
}


// static void st95_reset_spi(void)
// {
    // spi_acquire(SPI_DEV(st95_params.spi), st95_params.cs_spi, SPI_MODE_0, SPI_CLK_1MHZ);
        /*Send Reset*/
    // tx_spi = 0x01;
    // spi_transfer_bytes(SPI_DEV(st95_params.spi), st95_params.cs_spi, false, &tx_spi, &rx_reset, 1);

    // spi_release(SPI_DEV(st95_params.spi));

    // rtctimers_millis_sleep(20);
    // st95_send_irqin_negative_pulse();
// }

void st95_send_irqin_negative_pulse(void)
{
    // gpio_init(UMDK_ST95_IRQ_IN, GPIO_OUT);
    gpio_set(UMDK_ST95_IRQ_IN);
    xtimer_usleep(1000);
    gpio_clear(UMDK_ST95_IRQ_IN);
    xtimer_usleep(1000);
    gpio_set(UMDK_ST95_IRQ_IN);
    // gpio_init_af(UMDK_ST95_IRQ_IN, GPIO_AF7);
}

static uint8_t st95_cmd_idn(void)
{
    current_cmd = ST95_CMD_IDN;
    msg_rx.type = UMDK_ST95_MSG_IDN;

    txbuf[0] = ST95_CMD_IDN;
    txbuf[1] = 0x00;

    send_pack(2);
	
    return 1;
}

static uint8_t st95_select_field_off(void)
{
    msg_rx.type = UMDK_ST95_MSG_RF_OFF;

    txbuf[0] = ST95_CMD_PROTOCOL;
    txbuf[1] = 2;
    txbuf[2] = FIELD_OFF;
    txbuf[3] = 0x00;

    send_pack(4);

    return 1;
}

static uint8_t st95_select_iso18092(void)
{
    uint8_t length  = 0;

    txbuf[0] = ST95_CMD_PROTOCOL;
    txbuf[1] = 2;    // Data Length

    txbuf[2] = ISO_18092;

    // txbuf[3] = 0x00 | (ST95_TX_RATE_14443A << 6) | (ST95_RX_RATE_14443A << 4);

    txbuf[3] = 0x51;
    length = 4;

    txbuf[4] = 0x00;    // PP (Optioanal)                                                             // 00
    txbuf[5] = 0x00;    // MM (Optioanal)                                                            // 01
    txbuf[6] = 0x00;    // DD (Optioanal)                                                            // 80
    txbuf[7] = 0x00;    // ST Reserved (Optioanal)
    txbuf[8] = 0x00;    // ST Reserved (Optioanal)

    send_pack(length);

    return 4;

    return 0;
}

static uint8_t st95_select_iso15693(void)
{
    txbuf[0] = 0x00;

    txbuf[1] = ST95_CMD_PROTOCOL;
    txbuf[2] = 2;// Length

    txbuf[3] = ISO_15693;
    txbuf[4] = 0x05;

    return 5;
}

static uint8_t st95_select_iso14443a(void)
{
    uint8_t length  = 0;

    txbuf[0] = ST95_CMD_PROTOCOL;
    txbuf[1] = 2;    // Data Length
    txbuf[2] = ISO_14443A;
    txbuf[3] = 0x00 | (ST95_TX_RATE_14443A << 6) | (ST95_RX_RATE_14443A << 4);

    length = 4;

    txbuf[4] = 0x00;    // PP (Optioanal)                                                             // 00
    txbuf[5] = 0x00;    // MM (Optioanal)                                                            // 01
    txbuf[6] = 0x00;    // DD (Optioanal)                                                            // 80
    txbuf[7] = 0x00;    // ST Reserved (Optioanal)
    txbuf[8] = 0x00;    // ST Reserved (Optioanal)

    send_pack(length);

    return 4;
}

static uint8_t st95_select_iso14443b(void)
{
    txbuf[0] = 0x00;

    txbuf[1] = ST95_CMD_PROTOCOL;
    txbuf[2] = 2;// Length

    txbuf[3] = ISO_14443B;
    // txbuf[4] = 0x00 | (ST95_TX_RATE_106 << 6) | (ST95_RX_RATE_106 << 4);    // (TX_RATE << 6) | (RX_RATE << 4) // 02
    txbuf[4] = 0x01;


    txbuf[5] = 0x00;    // PP (Optioanal)                                                             // 00
    txbuf[6] = 0x00;    // MM (Optioanal)                                                            // 01
    txbuf[7] = 0x00;    // DD (Optioanal)                                                            // 80
    txbuf[8] = 0x00;    // ST Reserved (Optioanal)
    txbuf[9] = 0x00;    // ST Reserved (Optioanal)

    return 5;
}

static uint8_t send_pack(uint8_t len)
{
	
	// if(msg_rx.type != UMDK_ST95_MSG_CALIBR) {
		// DEBUG("TX: ");
		// PRINTBUFF(txbuf, len);
	// }

    current_state = UMDK_ST95_PACK_ERROR;
    flag_rx = UMDK_ST95_NOT_RECIEVED;
    memset(rxbuf, 0x00, sizeof(rxbuf));
    num_bytes_rx = 0;

    return ((*st95_send)(len));
}

static void _check_respose(void)
{
	uint32_t time_begin = rtctimers_millis_now();
    uint32_t time_end = 0;
    uint32_t time_delta = 0;
	
	while(flag_rx != UMDK_ST95_RECIEVED) {
		time_end = rtctimers_millis_now();
		time_delta = time_end - time_begin;
		if(time_delta > UMDK_ST95_NO_RESPONSE_TIME_MS) {
			current_state = UMDK_ST95_PACK_ERROR;
			puts("No response");
			// msg_send(&msg_rx, radio_pid);
			// break;
			return;
		}
	}
}

static uint8_t st95_cmd_echo(void)
{
    current_cmd = ST95_CMD_ECHO;
    flag_rx = UMDK_ST95_NOT_RECIEVED;
    current_state = UMDK_ST95_PACK_ERROR;

    txbuf[0] = ST95_CMD_ECHO;

    msg_rx.type = UMDK_ST95_MSG_ECHO;
    send_pack(1);

    // rtctimers_millis_sleep(ST95_ECHO_WAIT_TIME_MS);

	_check_respose();
	
    return (uint8_t)current_state;
}

static void *radio_send(void *arg)
{
    (void) arg;
    msg_t msg;
    msg_t msg_queue[16];
    msg_init_queue(msg_queue, 16);

    while (1) {
        msg_receive(&msg);

        st95_msg_t  msg_type = (st95_msg_t)msg.type;

		// if(msg_rx.type != UMDK_ST95_MSG_CALIBR) {
			// DEBUG("RX data ");
			// PRINTBUFF(rxbuf, num_bytes_rx);
		// }
		
        switch(msg_type) {
			case UMDK_ST95_MSG_GET_UID: {
                   iso14443a_msg_t state = msg.content.value;
				   switch(state) {
					   case ISO14443A_REQA_MSG: {
                           // UIDsize : (2 bits) value: 0 for single, 1 for double,  2 for triple and 3 for RFU
                            uid_size = _get_uidsize(rxbuf[2]);
                           
                           // === Select cascade level 1 ===
                            _anticollision_1();
                           
                           break;
                       }
                       case ISO14443A_ANTICOL_1_MSG: {                         
                           //  Check BCC
                           if(!_check_bcc(4, rxbuf + 2, rxbuf[2 + 4])) {
                               break;
                           }
                            // copy UID from CR95Hf response
                            if (uid_size == ISO14443A_ATQA_SINGLE) {
                                memcpy(uid_full,&rxbuf[2],ISO14443A_UID_SINGLE );
                            }
                            else {
                                memcpy(uid_full,&rxbuf[2 + 1],ISO14443A_UID_SINGLE - 1 );
                            }
                            
                            _select_1(5, &rxbuf[2]);
                           break;
                       }
                        case ISO14443A_SELECT_1_MSG: {
                            if(_is_uid_complete(rxbuf[2]) == 1) {
                                printf("UID Completed -> Single SAK: %02X\n", rxbuf[2]);
                                DEBUG("UID:  ");
                                PRINTBUFF(uid_full, ISO14443A_UID_SINGLE);
                            }
                            else {
                                 // === Select cascade level 2 ===
                                _anticollision_2();
                            }
                           break;
                       }
                       case ISO14443A_ANTICOL_2_MSG: {
                            //  Check BCC
                           if(!_check_bcc(4, rxbuf + 2, rxbuf[2 + 4])) {
                               break;
                           }
	
                            // copy UID from CR95Hf response
                            if (uid_size == ISO14443A_ATQA_DOUBLE)
                                memcpy(&uid_full[ISO14443A_UID_SINGLE - 1], &rxbuf[2], ISO14443A_UID_SINGLE );
                            else 
                                memcpy(&uid_full[ISO14443A_UID_SINGLE - 1], &rxbuf[2], ISO14443A_UID_SINGLE - 1);
                            
                            //Send Select command	
                            _select_2(5, &rxbuf[2]);
                           break;
                       }
                       case ISO14443A_SELECT_2_MSG: {
                           if(_is_uid_complete(rxbuf[2]) == 1) {
                                printf("UID Completed -> Double SAK: %02X\n", rxbuf[2]);
                                DEBUG("UID:  ");
                                PRINTBUFF(uid_full, ISO14443A_UID_DOUBLE);
                            }
                            else {
                                 // === Select cascade level 2 ===
                                _anticollision_3();
                            }
                           break;
                       }
                       case ISO14443A_ANTICOL_3_MSG: {
                           //  Check BCC
                           if(!_check_bcc(4, rxbuf + 2, rxbuf[2 + 4])) {
                               break;
                           }
	
                            // copy UID from CR95Hf response
                            if (uid_size == ISO14443A_ATQA_TRIPLE)
                                memcpy(&uid_full[ISO14443A_UID_DOUBLE - 1], &rxbuf[2], ISO14443A_UID_DOUBLE );
                            
                            //Send Select command	
                            _select_3(5, &rxbuf[2]);
                           break;
                       }
                       case ISO14443A_SELECT_3_MSG: {
                           
                           if(_is_uid_complete(rxbuf[2]) == 1) {
                                printf("UID Completed -> Triple SAK: %02X\n", rxbuf[2]);
                                DEBUG("UID:  ");
                                PRINTBUFF(uid_full, ISO14443A_UID_DOUBLE);
                            }
                            else {
                                puts("RFU - not support");
                            }
                           break;
                       }
                       
						default:
							break;
				   }
				   
                break;
            }
			
            case UMDK_ST95_MSG_RADIO: {
                    // puts("\n    [ MSG RADIO ]\n");
                break;
            }

            case UMDK_ST95_MSG_ECHO: {
                current_cmd = 0;
				flag_rx = UMDK_ST95_RECIEVED;
                if(rxbuf[0] == ST95_CMD_ECHO) {
                    current_state = UMDK_ST95_PACK_OK;
                    st95_cmd_idn();
                }
                else {
                    current_state = UMDK_ST95_PACK_ERROR;
                    puts("[umdk-" _UMDK_NAME_ "] Device: not found");
                }

                rxbuf[0] = 0x00;
                rxbuf[1] = 0x00;
                num_bytes_rx = 0;
                break;
            }

            case UMDK_ST95_MSG_IDLE: {
                if(st95_check_pack(num_bytes_rx)) {
                    if(rxbuf[2] == 0x02) {
                        current_state = UMDK_ST95_PACK_OK;
                        flag_rx = UMDK_ST95_RECIEVED;

                        st95_cmd_select_protocol();

                    }
                    else {
                        current_state = UMDK_ST95_PACK_ERROR;
                    }
                }
                else {
                    current_state = UMDK_ST95_PACK_ERROR;
                }

                num_bytes_rx = 0;
                flag_rx = UMDK_ST95_RECIEVED;
                rtctimers_millis_set(&detect_timer, UMDK_ST95_DETECT_MS);
                // t2 = xtimer_now_usec();
                // printf("TIME: %ld\n", t2-t1);
                break;
            }
            
            case UMDK_ST95_MSG_RF_OFF: {
                current_cmd = 0x0;
                 if(st95_check_pack(num_bytes_rx)) {
                      if((rxbuf[0] == 0x00) && (rxbuf[1] == 0x00)) {
                           current_state = UMDK_ST95_PACK_OK;
                            dac_data_h = 0x00;
                            dac_data_l = 0x00;
                            st95_calibration();
                      }
                      else {
                        current_state = UMDK_ST95_PACK_ERROR; 
                    }
                 }
                 else {
                    current_state = UMDK_ST95_PACK_ERROR;
                }
                num_bytes_rx = 0;
                flag_rx = UMDK_ST95_RECIEVED;
                break;
            }

            case UMDK_ST95_MSG_PROTOCOL: {
                current_cmd = 0x0;
                if(st95_check_pack(num_bytes_rx)) {
                    if((rxbuf[0] == 0x00) && (rxbuf[1] == 0x00)) {
                        current_state = UMDK_ST95_PACK_OK;
                        if(umdk_st95_config.mode == ST95_MODE_READER) {
							_send_reqa();
                        }
                        else if(umdk_st95_config.mode == ST95_MODE_WRITER) {
                            // st95_cmd_send_receive(send_data, 10);
                        }
                    }
                    else {
                        current_state = UMDK_ST95_PACK_ERROR;
                        // puts("    ERROR");
                    }
                }
                else {
                    current_state = UMDK_ST95_PACK_ERROR;
                }
                num_bytes_rx = 0;
                flag_rx = UMDK_ST95_RECIEVED;

                break;
            }

            case UMDK_ST95_MSG_ANTICOL: {
                if(rxbuf[0] == 0x80) {
                    uint32_t uid =  (rxbuf[5] << 24) + (rxbuf[4] << 16) + (rxbuf[3] << 8) + rxbuf[2];
                    printf("    -> UID: ");
                        printf(" %02X %02X %02X %02X", rxbuf[2], rxbuf[3], rxbuf[4], rxbuf[5]);
                    printf(" ->  %"PRIu32"\n", uid);
					
					
                }
                else {
                    puts("[ NOT VALID ANTICOL DATA ]");
                    current_state = UMDK_ST95_PACK_ERROR;
                }

                num_bytes_rx = 0;
                flag_rx = UMDK_ST95_RECIEVED;
                break;
            }

            case UMDK_ST95_MSG_UID: {
                num_bytes_rx = 0;
                if(rxbuf[0] == 0x80) {
                    current_state = UMDK_ST95_PACK_OK;
                    msg_rx.type = UMDK_ST95_MSG_ANTICOL;
					_anticollision_1();
                }
                else {
                    puts("[ NOT VALID UID DATA ]");
                    current_state = UMDK_ST95_PACK_ERROR;
                }
                flag_rx = UMDK_ST95_RECIEVED;

                break;
            }

            case UMDK_ST95_MSG_IDN: {
                if(st95_check_pack(num_bytes_rx)) {
                    if(rxbuf[0] == 0x00) {
                        current_state = UMDK_ST95_PACK_OK;
                        flag_rx = UMDK_ST95_RECIEVED;

                        printf("[umdk-" _UMDK_NAME_ "] Device: ");
                        for(uint32_t i = 2; i < 14; i++ ) {
                            printf("%c", rxbuf[i]);
                        }
                        printf("\n");

                        dac_data_h = 0x00;
                        dac_data_l = 0x00;
                        st95_select_field_off();
                        // st95_calibration();
                    }
                    else {
                        current_state = UMDK_ST95_PACK_ERROR;
                    }
                }
                else {
                    puts("NOT IDN");
                    current_state = UMDK_ST95_PACK_ERROR;
                }

                num_bytes_rx = 0;
                flag_rx = UMDK_ST95_RECIEVED;

                break;
            }

            case UMDK_ST95_MSG_CALIBR: {
                if(st95_check_pack(num_bytes_rx)) {
                    current_state = UMDK_ST95_PACK_OK;
                    if(dac_data_h == 0x00) {
                        if(rxbuf[2] == 0x02) {
                            dac_data_h = 0xFC;
                            st95_calibration();
                        }
                        else {
                            puts("[umdk-" _UMDK_NAME_ "] Error first calibration tag detection");
                        }
                    }
                    else if(dac_data_h > 0x02) {    /* Calibration */
                        if(rxbuf[2] == 0x02) {
                            dac_data_l = dac_data_h;
                            dac_data_h = 0xFC;
                            puts("[umdk-" _UMDK_NAME_ "] Calibration done");
                            // printf("\n            FINISH:  %02X  / %02X\n", dac_data_l, dac_data_h);

                            if(umdk_st95_config.mode == ST95_MODE_READER) {
                                rtctimers_millis_set(&detect_timer, UMDK_ST95_DETECT_MS);
                            }
                            break;
                        }
                        else if(rxbuf[2] == 0x01) {
                            dac_data_h -= 0x04;
                            st95_calibration();
                        }
                        else {
                            puts("[umdk-" _UMDK_NAME_ "] Error calibration tag detection");
                        }
                    }
                    else {
                        puts("ERROR");
                    }
                }
                else {
                    current_state = UMDK_ST95_PACK_ERROR;
                }

                flag_rx = UMDK_ST95_RECIEVED;
                num_bytes_rx = 0;
                break;
            }

            default:
                break;
        }

    }
    return NULL;
}

void st95_uart_rx(void *arg, uint8_t data)
{
    (void) arg;

    if(num_bytes_rx >= 30) {
        puts("OVERFLOW");
        return;
    }

    if(uart_rx == 0) {
        puts("RX UART NOT ALLOW");
        return;
    }

    rxbuf[num_bytes_rx] = data;
    num_bytes_rx++;

    xtimer_set_msg(&rx_timer, UMDK_ST95_UART_TIME_RX_USEC, &msg_rx, radio_pid);

    return;
}

static bool st95_check_pack(uint8_t length)
{
    if((length - 2) != rxbuf[1]) {
        puts("Wrong pack length");
           printf("Current msg: %02X\n", msg_rx.type);
        return false;
    }

    return true;
}

static uint8_t st95_send_uart(uint8_t length)
{
    uart_write(UART_DEV(st95_params.uart), txbuf, length);

    return 1;
}



static void st95_spi_rx(void* arg)
{
    (void) arg;
    gpio_irq_disable(st95_params.irq_out);

    uint8_t tx_rx = 0x02;

    spi_transfer_bytes(SPI_DEV(st95_params.spi), st95_params.cs_spi, true, &tx_rx, NULL, 1);
    spi_transfer_bytes(SPI_DEV(st95_params.spi), st95_params.cs_spi, false, NULL, rxbuf, sizeof(rxbuf));

    spi_release(SPI_DEV(st95_params.spi));

    if(current_cmd == ST95_CMD_ECHO) {
        num_bytes_rx = 1;
    }
    else {
        num_bytes_rx = rxbuf[1] + 2;
    }

    msg_send(&msg_rx, radio_pid);

    return;
}

static uint8_t st95_send_spi(uint8_t length)
{
    uint8_t tx_spi = 0x00;

    spi_acquire(SPI_DEV(st95_params.spi), st95_params.cs_spi, SPI_MODE_0, UMDK_ST95_SPI_CLK);

    /*Send command*/
    spi_transfer_bytes(SPI_DEV(st95_params.spi), st95_params.cs_spi, true, &tx_spi, NULL, 1);
    spi_transfer_bytes(SPI_DEV(st95_params.spi), st95_params.cs_spi, false, txbuf, NULL, length);

    gpio_irq_enable(st95_params.irq_out);

    return 1;
}


static uint8_t st95_select_iface(uint8_t iface)
{
    gpio_init(st95_params.ssi_0, GPIO_OUT);
    gpio_init(st95_params.irq_in, GPIO_OUT);
    // TODO: Set "power on" ST95HF

    if(iface == ST95_IFACE_UART) {
            /* Initialize the UART params*/
        uart_params_t params;
        params.baudrate = UMDK_ST95_UART_BAUD_DEF;
        params.databits = UART_DATABITS_8;
        params.parity = UART_PARITY_NOPARITY;
        params.stopbits = UART_STOPBITS_10;

                /* Select UART iface */
        gpio_clear(st95_params.ssi_0);
            /* Set low level IRQ_IN/RX */
        gpio_set(st95_params.irq_in);
        rtctimers_millis_sleep(ST95_RAMP_UP_TIME_MS);
        gpio_clear(st95_params.irq_in);
        rtctimers_millis_sleep(ST95_RAMP_UP_TIME_MS);

        /* Initialize UART */
        gpio_init_af(st95_params.irq_in, GPIO_AF7);
        if (uart_init_ext(UART_DEV(st95_params.uart), &params, st95_uart_rx, NULL)) {
            printf("[umdk-" _UMDK_NAME_ "] Error init UART interface: %02d \n", st95_params.uart);
            return 0;
        }
        else {
            printf("[umdk-" _UMDK_NAME_ "] Init UART interface: %02d \n", st95_params.uart);
        }

        st95_send = &st95_send_uart;

        uart_rx = 1;
    }
    else if(iface == ST95_IFACE_SPI) {
        /* Select SPI iface */
        gpio_set(st95_params.ssi_0);
            /* Set low level IRQ_IN/RX */
        gpio_set(st95_params.irq_in);
        rtctimers_millis_sleep(ST95_RAMP_UP_TIME_MS);
        gpio_clear(st95_params.irq_in);
        rtctimers_millis_sleep(ST95_RAMP_UP_TIME_MS);

             /* Initialize SPI */
        spi_init(SPI_DEV(st95_params.spi));
         /* Initialize CS SPI */
        if(spi_init_cs(SPI_DEV(st95_params.spi), st95_params.cs_spi) == SPI_OK) {
            printf("[umdk-" _UMDK_NAME_ "] Init SPI interface: %02d\n", st95_params.spi);
        }
        else {
            printf("[umdk-" _UMDK_NAME_ "] Error init SPI interface: %02d\n", st95_params.spi);
            return 0;
        }

        gpio_init_int(st95_params.irq_out, GPIO_IN_PU, GPIO_FALLING, st95_spi_rx, NULL);
        gpio_irq_disable(st95_params.irq_out);

        // gpio_set(UMDK_ST95_SSI_0);
        st95_send = &st95_send_spi;

        uart_rx = 0;
    }
    else {
        puts("[umdk-" _UMDK_NAME_ "] Error selecting interface");
        return 0;
    }

    rtctimers_millis_sleep(ST95_HFO_SETUP_TIME_MS);

    return 1;
}

static uint8_t st95_calibration(void)
{
    msg_rx.type = UMDK_ST95_MSG_CALIBR;

    txbuf[0] = ST95_CMD_IDLE;    // Command
    txbuf[1] = 14;                // Data Length
    /* Idle params */
        /* Wake Up Source */
    txbuf[2] = 0x03;            // Tag Detection + Time out
        /* Enter Control (the resources when entering WFE mode)*/
    txbuf[3] = 0xA1;            // Tag Detector Calibration
    txbuf[4] = 0x00;            //
        /* Wake Up Control (the wake-up resources) */
    txbuf[5] = 0xF8;            // Tag Detector Calibration
    txbuf[6] = 0x01;            //
        /* Leave Control (the resources when returning to Ready state)*/
    txbuf[7] = 0x18;            // Tag Detection
    txbuf[8] = 0x00;
        /* Wake Up Period (the time allowed between two tag detections) */
    txbuf[9] = 0x02;            //
        /* Osc Start (the delay for HFO stabilization) */
    txbuf[10] = 0x60;            // Recommendeded value 0x60
        /* DAC Start (the delay for DAC stabilization) */
    txbuf[11] = 0x60;            // Recommendeded value 0x60


        /* DAC Data */
    txbuf[12] = dac_data_l;            // DacDataL
    txbuf[13] = dac_data_h;            // DacDataH


        /* Swing Count */
    txbuf[14] = 0x3F;            // Recommendeded value 0x3F
        /* Max Sleep */
    txbuf[15] = 0x01;            // This value must be set to 0x01 during tag detection calibration

    send_pack(16);

    return  1;
}

static void reset_config(void) {
    umdk_st95_config.iface = ST95_IFACE_SPI;
    umdk_st95_config.mode = ST95_MODE_READER;
    umdk_st95_config.protocol = ISO14443A_SELECT;
}

static inline void save_config(void) {
    unwds_write_nvram_config(_UMDK_MID_, (uint8_t *) &umdk_st95_config, sizeof(umdk_st95_config));
}

static void init_config(void) {
    reset_config();

    if (!unwds_read_nvram_config(_UMDK_MID_, (uint8_t *) &umdk_st95_config, sizeof(umdk_st95_config))) {
        reset_config();
        return;
    }

    if (umdk_st95_config.iface > ST95_IFACE_SPI) {
        reset_config();
        return;
    }

    if (umdk_st95_config.mode > ST95_MODE_READER) {
        reset_config();
        return;
    }

    if (umdk_st95_config.protocol > SELECT_ALL_PROTOCOL) {
        reset_config();
        return;
    }
}


void umdk_st95_init(uwnds_cb_t *event_callback)
{
    (void)event_callback;
    // callback = event_callback;

    init_config();

    if(!st95_select_iface(umdk_st95_config.iface)) {
        return;
    }

    if(umdk_st95_config.mode == ST95_MODE_WRITER) {
        puts("[umdk-" _UMDK_NAME_ "]: Writer mode");
    }
    else if(umdk_st95_config.mode == ST95_MODE_READER) {
        puts("[umdk-" _UMDK_NAME_ "]: Reader mode");
    }

     /* Create handler thread */
    char *stack = (char *) allocate_stack(UMDK_ST95_STACK_SIZE);
    if (!stack) {
        return;
    }

    radio_pid = thread_create(stack, UMDK_ST95_STACK_SIZE, THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST, radio_send, NULL, "st95 thread");

    memset(txbuf, 0x00, 30);
    memset(rxbuf, 0x00, 30);

    st95_cmd_echo();
	
    protocol = umdk_st95_config.protocol;

     /* Configure periodic wakeup */
    detect_timer.callback = &detect_handler;
}


// static inline void reply_code(module_data_t *reply, st95_reply_t code)
// {
    // reply->as_ack = true;
    // reply->length = 2;
    // reply->data[0] = _UMDK_MID_;
    // reply->data[1] = code;
// }


bool umdk_st95_cmd(module_data_t *cmd, module_data_t *reply)
{
    memset(txbuf, 0x00, 30);
    memset(rxbuf, 0x00, 30);

    printf("Data: ");
    for(uint32_t i = 0; i < cmd->length; i++) {
        printf(" %02X", cmd->data[i]);
    }
    printf("\n");
    if(cmd->data[0] == 0x00) {    // Send data

        st95_cmd_select_protocol();
        // memcpy(txbuf, cmd->data + 1, cmd->length);


    }
    else if(cmd->data[0] == 0x01) {    // IFACE
        if(cmd->data[1] == ST95_IFACE_UART) {
            umdk_st95_config.iface = ST95_IFACE_UART;
            puts("UART iface");
        }
        else if(cmd->data[1] == ST95_IFACE_SPI) {
            umdk_st95_config.iface = ST95_IFACE_SPI;
            puts("SPI iface");
        }
        save_config();
    }
    else if(cmd->data[0] == 0x02) {    // MODE
        if(cmd->data[1] == ST95_MODE_WRITER) {
            umdk_st95_config.mode = ST95_MODE_WRITER;
            puts("WRITER mode");
        }
        else if(cmd->data[1] == ST95_MODE_READER) {
            umdk_st95_config.iface = ST95_MODE_READER;
            puts("READER mode");
        }
                save_config();
    }
    else if(cmd->data[0] == 0x03) {    // PROTOCOL

    }

    return false;


    st95_cmd_idn();
    st95_select_field_off();
// _get_uid();
    st95_select_iso14443a();
    st95_select_iso14443b();
    st95_select_iso15693();
    rtctimers_millis_set(&detect_timer, UMDK_ST95_DETECT_MS);

st95_send_irqin_negative_pulse();

    reply->as_ack = true;
    reply->length = 1;
    reply->data[0] = _UMDK_MID_;

    return true; /* Allow reply */
}


#ifdef __cplusplus
}
#endif
