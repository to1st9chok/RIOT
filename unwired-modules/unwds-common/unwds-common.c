/*
 * Copyright (C) 2016 Unwired Devices
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
 * @file		unwds-common.c
 * @brief       Common routines for all UMDK modules
 * @author      Eugene Ponomarev
 * @author      Oleg Artamonov
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "byteorder.h"
#include "nvram.h"
#include "xtimer.h"
#include "board.h"
#include "checksum/fletcher16.h"

#include "unwds-common.h"
#include "unwds-gpio.h"

/* umdk-modules.h is autogenerated during make */
#include "umdk-modules.h"

#define ENABLE_DEBUG (0)
#include "debug.h"

/**
 * EEPROM settings to adjust atomatically to MCUs with different EEPROM size
 */
struct {
    uint32_t eeprom_size;
    uint32_t config_storage_size;
    uint32_t storage_blocks;
    uint32_t min_clean_blocks;
    uint32_t config_storage_addr;
} unwds_eeprom_layout;

/**
 * @brief Bitmap of occupied pins that cannot be used as gpio in-out
 */
static uint32_t non_gpio_pin_map;

/**
 * @brief Bitmap of enabled modules
 */
static uint32_t enabled_bitmap[8];

/**
 * NVRAM config.
 */
static nvram_t *nvram = NULL;
static uint32_t nvram_config_block_size = 0;
static uint32_t nvram_config_base_addr = 0;

static uint8_t storage_used[UNWDS_STORAGE_BLOCKS_MAX] = { 0 };
static uint8_t storage_blocks[UNWDS_STORAGE_BLOCKS_MAX];

void unwds_setup_nvram_config(nvram_t *nvram_ptr, int base_addr, int block_size) {
	nvram = nvram_ptr;
	nvram_config_base_addr = base_addr;
	nvram_config_block_size = block_size;
}

bool unwds_read_nvram_config(unwds_module_id_t module_id, uint8_t *data_out, uint8_t max_size) {
    DEBUG("Reading module config\n");
	/* All configuration blocks has the same size, plus 2 bytes of CRC16 */
	int addr = nvram_config_base_addr + (module_id - 1) * (nvram_config_block_size + 2);

	/* Either max_size bytes or full block */
	int size = (max_size < nvram_config_block_size) ? max_size : nvram_config_block_size;
    
	/* Read NVRAM block */
	if (nvram->read(nvram, data_out, addr, size) < 0) {
        DEBUG("Error reading NVRAM\n");
		return false;
    }
        
    uint16_t crc16 = 0;
    if (nvram->read(nvram, (void *)&crc16, addr + size, 2) < 0) {
        DEBUG("Error reading CRC16\n");
		return false;
    }
    
    if (fletcher16(data_out, size) != crc16) {
        DEBUG("CRC does not match\n");
        return false;
    }
    
    DEBUG("Config read successfully\n");
	return true;
}

bool unwds_write_nvram_config(unwds_module_id_t module_id, uint8_t *data, size_t data_size) {
	if (data_size > nvram_config_block_size)
		return false;

	/* All configuration blocks has the same size, plus 2 bytes of CRC16 */
	int addr = nvram_config_base_addr + (module_id - 1) * (nvram_config_block_size + 2);

    uint16_t crc16 = fletcher16(data, data_size);
    
	/* Write NVRAM block */
	if (nvram->write(nvram, data, addr, data_size) < 0) {
		return false;
    }
    
    if (nvram->write(nvram, (void *)&crc16, addr + data_size, 2) < 0) {
        return false;
    }

	return true;
}

static bool unwds_storage_init(void) {
    bool config_valid = unwds_read_nvram_config(UNWDS_CONFIG_MODULE_ID, storage_blocks, sizeof(storage_blocks));
    
    if (!config_valid) {
        DEBUG("Storage block config invalid\n");
        memset((void*)&storage_blocks, 0, sizeof(storage_blocks));
        return false;
    }
    
    return true;
}

static bool unwds_storage_cleanup(void) {
    uint32_t clean_blocks = 0;
    uint32_t i = 0;
    uint32_t k = 0;
    
    for (i = 0; i < unwds_eeprom_layout.storage_blocks; i++) {
        if (storage_blocks[i] == 0) {
            clean_blocks++;
        }
    }
    
    if (clean_blocks < unwds_eeprom_layout.min_clean_blocks) {
        for (i = 0; i < unwds_eeprom_layout.storage_blocks; i++) {
            bool block_in_use = false;
            
            for (k = 0; k < unwds_eeprom_layout.storage_blocks; k++) {
                if (storage_blocks[i] == storage_used[k]) {
                    block_in_use = true;
                    DEBUG("Block %lu is in use by module %d\n", i, storage_used[k]);
                    break;
                }
            }
            
            if (!block_in_use) {
                DEBUG("Unused block found, was used by module %d\n", storage_blocks[i]);
                storage_blocks[i] = 0;
                clean_blocks++;
            }
            
            if (clean_blocks >= unwds_eeprom_layout.min_clean_blocks) {
                DEBUG("Cleanup done\n");
                DEBUG("Writing new storage config\n");
                unwds_write_nvram_config(UNWDS_CONFIG_MODULE_ID, storage_blocks, sizeof(storage_blocks));
                return true;
            }
        }
    } else {
        return true;
    }
    
    DEBUG("Done, but only %lu clean blocks\n", clean_blocks);
    DEBUG("Writing new storage config\n");
    unwds_write_nvram_config(UNWDS_CONFIG_MODULE_ID, storage_blocks, sizeof(storage_blocks));
    
    return false;
}

static bool unwds_create_storage_block(unwds_module_id_t module_id) {
    uint32_t i = 0;
    for (i = 0; i < unwds_eeprom_layout.storage_blocks; i++) {
        if (storage_blocks[i] == 0) {
            DEBUG("Found empty storage block\n");
            storage_blocks[i] = module_id;
            
            DEBUG("Erasing EEPROM\n");
            /* storage size plus 2 bytes CRC16 */
            int addr = unwds_eeprom_layout.config_storage_addr + (unwds_eeprom_layout.config_storage_size + 2)*i;
            nvram->clearpart(nvram, addr, unwds_eeprom_layout.config_storage_size + 2);
            
            DEBUG("Writing new storage config\n");
            unwds_write_nvram_config(UNWDS_CONFIG_MODULE_ID, storage_blocks, sizeof(storage_blocks));
            return true;
        }
    }
    
    printf("[unwds-common] Error: no storage block available for module %d\n", module_id);
    return false;
}

bool unwds_read_nvram_storage(unwds_module_id_t module_id, uint8_t *data_out, uint8_t size) {
    
    uint32_t addr = 0;
    uint32_t i = 0;
    
    /* add the module to the list of modules currently using EEPROM storage */
    for (i = 0; i < unwds_eeprom_layout.storage_blocks; i++) {
        if (storage_used[i] == 0) {
            storage_used[i] = module_id;
            break;
        }
    }
    
    /* find if this module already has dedicated storage space */
    for (i = 0; i < unwds_eeprom_layout.storage_blocks; i++) {
        if (storage_blocks[i] == module_id) {
            /* storage size plus 2 bytes CRC16 */
            addr = unwds_eeprom_layout.config_storage_addr + (unwds_eeprom_layout.config_storage_size + 2)*i;
            break;
        }
    }
    
    /* No such space exists, initialize it */
    if (addr == 0) {
        unwds_create_storage_block(module_id);
        return false;
    }

    /* Read NVRAM block */
	if (nvram->read(nvram, data_out, addr, size + 2) < 0)
		return false;
    
    uint16_t crc16;
    memcpy(&crc16, data_out+size, 2);
    
    if (fletcher16(data_out, size) != crc16) {
        return false;
    }
    
    return true;
}

bool unwds_write_nvram_storage(unwds_module_id_t module_id, uint8_t *data, size_t data_size) {
    if (data_size > 128)
		return false;
    
    uint32_t addr = 0;
    uint32_t i = 0;
    
    for (i = 0; i < unwds_eeprom_layout.storage_blocks; i++) {
        if (storage_blocks[i] == module_id) {
            /* storage size plus 2 bytes CRC16 */
            addr = unwds_eeprom_layout.config_storage_addr + (unwds_eeprom_layout.config_storage_size + 2)*i;
            break;
        }
    }
    
    if (addr == 0) {
        return false;
    }
    
    uint16_t crc16 = fletcher16(data, data_size);
    
    /* Write NVRAM block */
	if (nvram->write(nvram, data, addr, data_size) < 0) {
		return false;
    }
    
    if (nvram->write(nvram, (void *)&crc16, addr + data_size, 2) < 0) {
        return false;
    }

	return true;
}

bool unwds_erase_nvram_config(unwds_module_id_t module_id) {
	/* All configuration blocks has the same size */
	int addr = nvram_config_base_addr + module_id * nvram_config_block_size;

	/* Write NVRAM block */
	if (nvram->clearpart(nvram, addr, nvram_config_block_size) < 0)
		return false;

	return true;
}

/**
 * Stacks pool.
 */
// static uint8_t stack_pool[UNWDS_STACK_POOL_SIZE_BYTES];
// static uint32_t stack_size_used = 0;

uint8_t *allocate_stack(uint32_t stack_size) {
    uint8_t *address = (uint8_t *)malloc(stack_size);
    
    /* additional check for allocation validity */
    if (address && !cpu_check_address((char *)&address[stack_size - 1])) {
        address = NULL;
    }
    
    return address;
}

void unwds_init_modules(uwnds_cb_t *event_callback)
{
    int i = 0;

    unwds_storage_init();
    
	/* Initialize modules */
    while (modules[i].init_cb != NULL && modules[i].cmd_cb != NULL) {
    	if (enabled_bitmap[modules[i].module_id / 32] & (1 << (modules[i].module_id % 32))) {	/* Module enabled */
    		printf("[unwds] initializing \"%s\" module...\n", modules[i].name);
            modules[i].init_cb(&non_gpio_pin_map, event_callback);
    	}
        i++;
    }
    
    unwds_storage_cleanup();
}

static unwd_module_t *find_module(unwds_module_id_t modid) {
	int i = 0;
    while (modules[i].init_cb != NULL && modules[i].cmd_cb != NULL) {
    	if (modules[i].module_id == modid)
    		return (unwd_module_t *) &modules[i];

    	i++;
    }

    return NULL;
}

void unwds_list_modules(uint32_t *enabled_mods, bool enabled_only) {
	int i = 0;
	int modcount = 0;
    while (modules[i].init_cb != NULL && modules[i].cmd_cb != NULL) {
    	bool enabled = (enabled_mods[modules[i].module_id / 32] & (1 << (modules[i].module_id % 32)));
    	unwds_module_id_t modid = modules[i].module_id;

    	if (enabled_only && !enabled) {
    		i++;
    		continue;
    	}

    	modcount++;
    	printf("[%s] %s (id: %d)\n", (enabled) ? "+" : "-", modules[i].name, modid);

    	i++;
    }

    if (!modcount)
    	puts("<no modules enabled>");
}

void unwds_set_enabled(uint32_t *enabled_mods) {
    memcpy(enabled_bitmap, enabled_mods, sizeof(enabled_bitmap));
}

char *unwds_get_module_name(unwds_module_id_t modid) {
	unwd_module_t *module = find_module(modid);
	if (!module)
		return NULL;

	return module->name;	
}

bool unwds_is_module_exists(unwds_module_id_t modid) {
	return find_module(modid) != NULL;
}

bool unwds_is_module_enabled(unwds_module_id_t modid) {
    if (!find_module(modid)) {
        return false;
    }
    
	return (enabled_bitmap[modid / 32] & (1 << (modid % 32)));
}

bool unwds_send_broadcast(unwds_module_id_t modid, module_data_t *data, module_data_t *reply)
{
	unwd_module_t *module = find_module(modid);
	if (!module)
		return false;

	if (module->cmb_broadcast_cb != NULL)
		return module->cmb_broadcast_cb(data, reply);

	return false;
}

int unwds_send_to_module(unwds_module_id_t modid, module_data_t *data, module_data_t *reply)
{
    if (!unwds_is_module_enabled(modid)) {
        return UNWDS_MODULE_NOT_FOUND;
    }
    
	unwd_module_t *module = find_module(modid);
	return module->cmd_cb(data, reply);
}

bool unwds_is_pin_occupied(uint32_t pin)
{
    return ((non_gpio_pin_map >> pin) & 0x1);
}

uint32_t * unwds_get_enabled(void)
{
    return enabled_bitmap;
}

void unwds_add_shell_command(char *name, char *desc, void* handler) {
    uint32_t i = 0;
    for (i = 0; i < UNWDS_SHELL_COMMANDS_MAX - 1; i++) {
        if (shell_commands[i].name == NULL) {
            shell_commands[i].name = name;
            shell_commands[i].desc = desc;
            shell_commands[i].handler = handler;
            printf("%s shell command added\n", name);
            break;
        }
    }
}

int unwds_modid_by_name(char *name) {
    uint32_t i = 0;
    for (i = 0; i < sizeof(modules)/sizeof(unwd_module_t); i++) {
        if (strcmp(name, modules[i].name) == 0) {
            return modules[i].module_id;
        }
    }
    
    return -1;
}

gpio_t unwds_gpio_pin(int pin)
{
    if (pin < 0 || (uint32_t)pin >= (sizeof(unwds_gpio_map) / sizeof(gpio_t))) {
        return 0;
    }

    return unwds_gpio_map[pin];
}

int unwds_gpio_pins_total(void)
{
    return (sizeof(unwds_gpio_map) / sizeof(gpio_t));
}

void int_to_float_str(char *buf, int decimal, uint8_t precision) {  
    uint32_t i = 0;
    int divider = 1;
    char format[10] = { };
    char digits[3];
    
    buf[0] = 0;
    if (decimal < 0) {
        strcat(format, "-");
    }
    strcat(format, "%d.%0");
    
    for (i = 0; i<precision; i++) {
        divider *= 10;
    }

    snprintf(digits, 3, "%lud", i);
    strcat(format, digits);
    
    snprintf(buf, 50, format, abs(decimal/divider), abs(decimal%divider));
}

void convert_to_be_sam(void *ptr, size_t size) {
    switch (size) {
        case 1: {
            int8_t v = *(int8_t*)ptr;
            *(uint8_t*)ptr = ((v + (v >> 7)) ^ (v >> 7)) | (v & (1 << 7));
            break;
        }
        case 2: {
            int16_t v = *(int16_t*)ptr;
            *(uint16_t*)ptr = ((v + (v >> 15)) ^ (v >> 15)) | (v & (1 << 15));
            break;
        }
        case 4: {
            int32_t v = *(int32_t*)ptr;
            *(uint32_t*)ptr = ((v + (v >> 31)) ^ (v >> 31)) | (v & (1 << 31));
            break;
        }
        case 8: {
            int64_t v = *(int64_t*)ptr;
            *(uint64_t*)ptr = ((v + (v >> 63)) ^ (v >> 63)) | (v & (1ULL << 63));
            break;
        }
        default:
            return;
    }
    
    if (byteorder_is_little_endian()) {
        byteorder_swap(ptr, size);
    }
}

/* determine EEPROM size to work seamlessly with CC and CB-A MCUs */
void unwds_init(void) {
    unwds_eeprom_layout.eeprom_size = get_cpu_eeprom_size();
    
    if (unwds_eeprom_layout.eeprom_size == 8192) {
        unwds_eeprom_layout.config_storage_size = 256;
        unwds_eeprom_layout.storage_blocks = 16;
        unwds_eeprom_layout.min_clean_blocks = 4;    
    } else {
        unwds_eeprom_layout.config_storage_size = 128;
        unwds_eeprom_layout.storage_blocks = 4;
        unwds_eeprom_layout.min_clean_blocks = 1;  
    }
    
    unwds_eeprom_layout.config_storage_addr = unwds_eeprom_layout.eeprom_size -
                                              (unwds_eeprom_layout.storage_blocks*unwds_eeprom_layout.config_storage_size);
}

#ifdef __cplusplus
}
#endif
