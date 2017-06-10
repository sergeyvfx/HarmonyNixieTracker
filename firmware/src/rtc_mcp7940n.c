// Copyright (c) 2017, Sergey Sharybin
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//
// Author: Sergey Sharybin (sergey.vfx@gmail.com)

#include "rtc_mcp7940n.h"

#include "system_definitions.h"

////////////////////////////////////////////////////////////////////////////////
// Handy compatibility/abstraction macros and types.

#define RTC_MCP7940N_LOG_PREFIX "MCP7940N: "

#define DEBUG_LEVEL 0

#define ERROR_MESSAGE(message) \
    SYS_CONSOLE_MESSAGE(RTC_MCP7940N_LOG_PREFIX message)

#define DEBUG_MESSAGE(message) \
    SYS_DEBUG_MESSAGE(DEBUG_LEVEL, "[DEBUG] " RTC_MCP7940N_LOG_PREFIX message)

#define DEBUG_PRINT(format, ...) \
    SYS_DEBUG_PRINT(DEBUG_LEVEL, "[DEBUG] " RTC_MCP7940N_LOG_PREFIX format, ##__VA_ARGS__)

////////////////////////////////////////////////////////////////////////////////
// Internal routines.

static uint8_t convertToBCD(uint8_t decimal) {
  return (decimal / 10) << 4 | (decimal % 10);
}

static uint8_t convertFromBCD(uint8_t bcd) {
  uint8_t byte_msb = 0;
  uint8_t byte_lsb = 0;
  byte_msb = (bcd & 0b11110000) >> 4;
  byte_lsb = (bcd & 0b00001111);
  return ((byte_msb*10) + byte_lsb);
}

static DRV_I2C_BUFFER_EVENT getI2CTransferStatus(RTC_MCP7940N* rtc) {
  return DRV_I2C_TransferStatusGet(rtc->i2c_handle, rtc->i2c_buffer_handle);
}

static bool checkI2CBufferReadyForTransmit(RTC_MCP7940N* rtc) {
  return (rtc->i2c_buffer_handle == (DRV_I2C_BUFFER_HANDLE) NULL) ||
         (getI2CTransferStatus(rtc) == DRV_I2C_BUFFER_EVENT_COMPLETE) ||
         (getI2CTransferStatus(rtc) == DRV_I2C_BUFFER_EVENT_ERROR);
}

static bool performI2CTransmit(RTC_MCP7940N* rtc,
                               uint8_t* transmit_buffer,
                               size_t num_bytes) {
  if (!checkI2CBufferReadyForTransmit(rtc)) {
    ERROR_MESSAGE("Unable to perform I2C transmittance.\r\n");
    rtc->state = RTC_MCP7940N_STATE_ERROR;
    return false;
  }
  DEBUG_PRINT("Performing transmittance of %d bytes.\r\n", num_bytes);
  // TODO(sergey): For some reason we need to shift address here...
  rtc->i2c_buffer_handle = DRV_I2C_Transmit(rtc->i2c_handle,
                                            (MCP7940N_I2C_ADDRESS << 1),
                                            transmit_buffer,
                                            num_bytes,
                                            NULL);
  if (rtc->i2c_buffer_handle == NULL) {
    ERROR_MESSAGE("I2C transmit returned invalid handle.\r\n");
    rtc->state = RTC_MCP7940N_STATE_ERROR;
    return false;
  }
  rtc->state = RTC_MCP7940N_STATE_I2C_STAUS_CHECK;
  return true;
}

static bool performI2CTransmitThenReceive(RTC_MCP7940N* rtc,
                                          uint8_t* transmit_buffer,
                                          size_t num_bytes_transmit,
                                          uint8_t* receive_buffer,
                                          size_t num_bytes_receive) {
  if (!checkI2CBufferReadyForTransmit(rtc)) {
    ERROR_MESSAGE("Unable to perform I2C transmittance.\r\n");
    rtc->state = RTC_MCP7940N_STATE_ERROR;
    return false;
  }
  DEBUG_PRINT("Performing transmittance of %d bytes, "
              "followed with receiving %d bytes\r\n",
               num_bytes_transmit, num_bytes_receive);
  // TODO(sergey): For some reason we need to shift address here...
  rtc->i2c_buffer_handle = DRV_I2C_TransmitThenReceive(
      rtc->i2c_handle,
      (MCP7940N_I2C_ADDRESS << 1),
      transmit_buffer, num_bytes_transmit,
      receive_buffer, num_bytes_receive,
      NULL);
  if (rtc->i2c_buffer_handle == NULL) {
    ERROR_MESSAGE("I2C transmit+receive returned invalid handle.\r\n");
    rtc->state = RTC_MCP7940N_STATE_ERROR;
    return false;
  }
  rtc->state = RTC_MCP7940N_STATE_I2C_STAUS_CHECK;
  return true;
}

static void oscillatorUpdateBits(RTC_MCP7940N* rtc) {
  DEBUG_PRINT("Register value before updating oscillator: 0x%02x.\r\n",
              rtc->_private.register_storage[0]);
  rtc->transmit_buffer[0] = MCP7940N_REG_ADDR_SECONDS;
  if (rtc->next_task == RTC_MCP7940N_TASK_OSCILLATOR_UPDATE_ENABLE) {
    rtc->transmit_buffer[1] = rtc->_private.register_storage[0] |
                              MCP7940N_FLAG_START_OSCILLATOR;
  } else {
    rtc->transmit_buffer[1] = rtc->_private.register_storage[0] &
                              (~MCP7940N_FLAG_START_OSCILLATOR);
  }
  performI2CTransmit(rtc, rtc->transmit_buffer, 2);
}

static void oscillatorUpdateStatus(RTC_MCP7940N* rtc) {
  DEBUG_PRINT("Fetched register value before updating: 0x%02x.\r\n",
              rtc->_private.register_storage[0]);
  const bool enabled = (rtc->_private.register_storage[0] & MCP7940N_FLAG_START_OSCILLATOR);
  *rtc->_private.return_status_ptr = enabled;
}

static void batteryUpdateBits(RTC_MCP7940N* rtc) {
  DEBUG_PRINT("Register value before updating battery: 0x%02x.\r\n",
              rtc->_private.register_storage[0]);
  rtc->transmit_buffer[0] = MCP7940N_REG_ADDR_DAY_OF_WEEK;
  if (rtc->next_task == RTC_MCP7940N_TASK_BATTERY_UPDATE_ENABLE) {
    rtc->transmit_buffer[1] = rtc->_private.register_storage[0] |
                              MCP7940N_FLAG_BATTERY_ENABLE;
  } else {
    rtc->transmit_buffer[1] = rtc->_private.register_storage[0] &
                              (~MCP7940N_FLAG_BATTERY_ENABLE);
  }
  performI2CTransmit(rtc, rtc->transmit_buffer, 2);
}

static void batteryUpdateStatus(RTC_MCP7940N* rtc) {
  DEBUG_PRINT("Fetched register value before updating: 0x%02x.\r\n",
              rtc->_private.register_storage[0]);
  const bool enabled = (rtc->_private.register_storage[0] & MCP7940N_FLAG_BATTERY_ENABLE);
  *rtc->_private.return_status_ptr = enabled;
}

static void dateTimeConvertToBCDAndTransmit(RTC_MCP7940N* rtc) {
  const uint8_t num_registers = sizeof(RTC_MCP7940N_DateTime);
  const RTC_MCP7940N_DateTime *date_time = &rtc->_private.date_time;
  uint8_t *reg = rtc->_private.register_storage;
#ifdef SYS_CMD_REMAP_SYS_DEBUG_MESSAGE
  {
    uint8_t i;
    DEBUG_MESSAGE("Fetched register values before updating: ");
    for (i = 0; i < num_registers; ++i) {
      if (i != 0) {
        SYS_DEBUG_MESSAGE(DEBUG_LEVEL, " ");
      }
      SYS_DEBUG_PRINT(DEBUG_LEVEL, "0x%02x", reg[i]);
    }
    SYS_DEBUG_MESSAGE(DEBUG_LEVEL, "\r\n");
  }
#endif
  reg[0] = (reg[0] & ~0x7f) | convertToBCD(date_time->seconds);
  reg[1] = (reg[1] & ~0x7f) | convertToBCD(date_time->minutes);
  reg[2] = (reg[2] & ~0x3f) | convertToBCD(date_time->hours);
  reg[3] = (reg[3] & ~0x7) | convertToBCD(date_time->day_of_week);
  reg[4] = (reg[4] & ~0x3f) | convertToBCD(date_time->day);
  reg[5] = (reg[5] & ~0xf) | convertToBCD(date_time->month);
  reg[6] = (reg[6] & ~0xff) | convertToBCD(date_time->year);
  RTC_MCP7940N_WriteNumRegisters(rtc, reg, num_registers);
}

static void dateTimeConvertFromBCD(RTC_MCP7940N* rtc) {
  RTC_MCP7940N_DateTime* date_time = rtc->_private.date_time_ptr;
  date_time->seconds = convertFromBCD(date_time->seconds & 0x7f);
  date_time->minutes = convertFromBCD(date_time->minutes & 0x7f);
  date_time->hours = convertFromBCD(date_time->hours & 0x3f);
  date_time->day_of_week = convertFromBCD(date_time->day_of_week & 0x7);
  date_time->day = convertFromBCD(date_time->day & 0x3f);
  date_time->month = convertFromBCD(date_time->month & 0xf);
  date_time->year = convertFromBCD(date_time->year & 0xff);
}

static void checkI2CStatus(RTC_MCP7940N* rtc) {
  DRV_I2C_BUFFER_EVENT status = getI2CTransferStatus(rtc);
  switch (status) {
    case DRV_I2C_BUFFER_EVENT_COMPLETE:
      DEBUG_MESSAGE("I2C transaction finished.\r\n");
      switch (rtc->next_task) {
        case RTC_MCP7940N_TASK_NONE:
          rtc->state = RTC_MCP7940N_STATE_NONE;
          break;
        case RTC_MCP7940N_TASK_OSCILLATOR_UPDATE_ENABLE:
        case RTC_MCP7940N_TASK_OSCILLATOR_UPDATE_DISABLE:
          // NOTE: Update next_task AFTER the function.
          oscillatorUpdateBits(rtc);
          rtc->next_task = RTC_MCP7940N_TASK_NONE;
          break;
        case RTC_MCP7940N_TASK_OSCILLATOR_UPDATE_STATUS:
          rtc->next_task = RTC_MCP7940N_TASK_NONE;
          oscillatorUpdateStatus(rtc);
          break;
        case RTC_MCP7940N_TASK_BATTERY_UPDATE_ENABLE:
        case RTC_MCP7940N_TASK_BATTERY_UPDATE_DISABLE:
          // NOTE: Update next_task AFTER the function.
          batteryUpdateBits(rtc);
          rtc->next_task = RTC_MCP7940N_TASK_NONE;
          break;
        case RTC_MCP7940N_TASK_BATTERY_UPDATE_STATUS:
          rtc->next_task = RTC_MCP7940N_TASK_NONE;
          batteryUpdateStatus(rtc);
          break;
        case RTC_MCP7940N_TASK_DATE_TIME_CONVERT_BCD:
          rtc->next_task = RTC_MCP7940N_TASK_NONE;
          rtc->state = RTC_MCP7940N_STATE_NONE;
          dateTimeConvertFromBCD(rtc);
          break;
        case RTC_MCP7940N_TASK_DATE_TIME_UPDATE_AND_TRANSMIT:
          rtc->next_task = RTC_MCP7940N_TASK_NONE;
          dateTimeConvertToBCDAndTransmit(rtc);
          break;
      }
      break;
    case DRV_I2C_BUFFER_EVENT_ERROR:
      ERROR_MESSAGE("Error detected during I2C transaction.\r\n");
      rtc->state = RTC_MCP7940N_STATE_ERROR;
      break;
    default:
      // Nothing to do.
      break;
  }
}

////////////////////////////////////////////////////////////////////////////////
// Public API.

bool RTC_MCP7940N_Initialize(RTC_MCP7940N* rtc,
                             SYS_MODULE_INDEX i2c_module_index) {
  // TODO(sergey): Ignore for release builds to save CPU ticks?
  memset(rtc, 0, sizeof(*rtc));
  rtc->i2c_handle = DRV_I2C_Open(i2c_module_index, DRV_IO_INTENT_READWRITE);
  if (rtc->i2c_handle == DRV_HANDLE_INVALID) {
    ERROR_MESSAGE("Error opening I2C bus handle.\r\n");
    rtc->state = RTC_MCP7940N_STATE_ERROR;
    return false;
  }
  rtc->state = RTC_MCP7940N_STATE_NONE;
  rtc->next_task = RTC_MCP7940N_TASK_NONE;
  rtc->i2c_buffer_handle = NULL;
  DEBUG_MESSAGE("New RTC handle is initialized.\r\n");
  return true;
}

void RTC_MCP7940N_Tasks(RTC_MCP7940N* rtc) {
  switch (rtc->state) {
    case RTC_MCP7940N_STATE_NONE:
      // Nothing to do, pass.
      break;
    case RTC_MCP7940N_STATE_ERROR:
      // TODO(sergey): Report some extra error message?
      break;
    case RTC_MCP7940N_STATE_I2C_STAUS_CHECK:
      checkI2CStatus(rtc);
      break;
  }
}

bool RTC_MCP7940N_IsBusy(RTC_MCP7940N* rtc) {
  return !(rtc->state == RTC_MCP7940N_STATE_NONE ||
           rtc->state == RTC_MCP7940N_STATE_ERROR);
}

void RTC_MCP7940N_WriteDateAndTime(RTC_MCP7940N* rtc,
                                   const RTC_MCP7940N_DateTime* date_time) {
  DEBUG_MESSAGE("Begin transmitting date and time to RTC.\r\n");
  // We should preserve non-date-time bits intact, so we first read old state
  // and then write an updated one.
  rtc->next_task = RTC_MCP7940N_TASK_DATE_TIME_UPDATE_AND_TRANSMIT;
  memcpy(&rtc->_private.date_time, date_time, sizeof(rtc->_private.date_time));
  RTC_MCP7940N_ReadNumRegisters(rtc, rtc->_private.register_storage,
                                sizeof(rtc->_private.date_time));
}

void RTC_MCP7940N_ReadDateAndTime(RTC_MCP7940N* rtc,
                                  RTC_MCP7940N_DateTime* date_time) {
  DEBUG_PRINT("Begin sequence to read current date and time\r\n");
  // Prepare transmittance buffer.
  rtc->transmit_buffer[0] = MCP7940N_REG_ADDR_SECONDS;
  // Schedule receive.
  rtc->next_task = RTC_MCP7940N_TASK_DATE_TIME_CONVERT_BCD;
  rtc->_private.date_time_ptr = date_time;
  performI2CTransmitThenReceive(rtc,
                                rtc->transmit_buffer, 1,
                                (uint8_t*)date_time, sizeof(*date_time));
}

void RTC_MCP7940N_ReadRegister(RTC_MCP7940N* rtc,
                               uint8_t register_address,
                               uint8_t* register_value) {
  DEBUG_PRINT("Begin receiving register %x from RTC.\r\n", register_address);
  // Prepare transmittance buffer.
  rtc->transmit_buffer[0] = register_address;
  // Schedule receive.
  performI2CTransmitThenReceive(rtc,
                                rtc->transmit_buffer, 1,
                                register_value, sizeof(*register_value));
}

void RTC_MCP7940N_WriteRegister(RTC_MCP7940N* rtc,
                                uint8_t register_address,
                                uint8_t register_value) {
  DEBUG_PRINT("Begin writing register %x to RTC with value %d.\r\n", 
              register_address, register_value);
  // Prepare transmittance buffer.
  rtc->transmit_buffer[0] = register_address;
  rtc->transmit_buffer[1] = register_value;
  // Schedule transmit.
  performI2CTransmit(rtc, rtc->transmit_buffer, 2 * sizeof(uint8_t));
}

void RTC_MCP7940N_ReadNumRegisters(RTC_MCP7940N* rtc,
                                   uint8_t* register_storage,
                                   uint8_t num_registers) {
  // TODO(sergey): Check that we are not reading more register than exists
  // on the chip.
  DEBUG_PRINT("Begin reading all registers.\r\n");
  rtc->transmit_buffer[0] = MCP7940N_REG_ADDR_SECONDS;
  // Schedule receive.
  performI2CTransmitThenReceive(rtc,
                                rtc->transmit_buffer, 1,
                                register_storage,
                                sizeof(*register_storage) * num_registers);
}

void RTC_MCP7940N_WriteNumRegisters(RTC_MCP7940N* rtc,
                                    const uint8_t* register_storage,
                                    uint8_t num_registers) {
  // TODO(sergey): Check that we are not writing more register than exists
  // on the chip.
  DEBUG_PRINT("Begin writing all registers.\r\n");
  rtc->transmit_buffer[0] = MCP7940N_REG_ADDR_SECONDS;
  memcpy(&rtc->transmit_buffer[1], register_storage,
         sizeof(*register_storage) * num_registers);
  performI2CTransmit(rtc,
                     rtc->transmit_buffer,
                     (num_registers + 1) * sizeof(*register_storage));
}

void RTC_MCP7940N_EnableOscillator(RTC_MCP7940N* rtc, bool enable) {
  DEBUG_PRINT("Begin sequence to set oscillator status to %s.\r\n",
              enable ? "ENABLED" : "DISABLED");
  // Prepare transmittance buffer.
  rtc->transmit_buffer[0] = MCP7940N_REG_ADDR_SECONDS;
  // Schedule receive.
  if (enable) {
    rtc->next_task = RTC_MCP7940N_TASK_OSCILLATOR_UPDATE_ENABLE;
  } else {
    rtc->next_task = RTC_MCP7940N_TASK_OSCILLATOR_UPDATE_DISABLE;
  }
  performI2CTransmitThenReceive(
      rtc,
      rtc->transmit_buffer, 1,
      &rtc->_private.register_storage[0],
      sizeof(rtc->_private.register_storage[0]));
}

void RTC_MCP7940N_OscillatorStatus(RTC_MCP7940N* rtc, bool* enabled) {
  DEBUG_MESSAGE("Begin sequence to check whether oscillator is enabled.\r\n");
  rtc->transmit_buffer[0] = MCP7940N_REG_ADDR_SECONDS;
  // Schedule receive.
  rtc->next_task = RTC_MCP7940N_TASK_OSCILLATOR_UPDATE_STATUS;
  rtc->_private.return_status_ptr = enabled;
  performI2CTransmitThenReceive(
      rtc,
      rtc->transmit_buffer, 1,
      &rtc->_private.register_storage[0],
      sizeof(rtc->_private.register_storage[0]));
}

void RTC_MCP7940N_EnableBatteryBackup(RTC_MCP7940N* rtc, bool enable) {
  DEBUG_PRINT("Begin sequence to set battery backup to %s.\r\n",
              enable ? "ENABLED" : "DISABLED");
  // Prepare transmittance buffer.
  rtc->transmit_buffer[0] = MCP7940N_REG_ADDR_DAY_OF_WEEK;
  // Schedule receive.
  if (enable) {
    rtc->next_task = RTC_MCP7940N_TASK_BATTERY_UPDATE_ENABLE;
  } else {
    rtc->next_task = RTC_MCP7940N_TASK_BATTERY_UPDATE_DISABLE;
  }
  performI2CTransmitThenReceive(
      rtc,
      rtc->transmit_buffer, 1,
      &rtc->_private.register_storage[0], sizeof(rtc->_private.register_storage[0]));
}

void RTC_MCP7940N_BatteryBackupStatus(RTC_MCP7940N* rtc, bool* enabled) {
  DEBUG_MESSAGE("Begin sequence to check whether battery backup "
                "is enabled.\r\n");
  rtc->transmit_buffer[0] = MCP7940N_REG_ADDR_DAY_OF_WEEK;
  // Schedule receive.
  rtc->next_task = RTC_MCP7940N_TASK_BATTERY_UPDATE_STATUS;
  rtc->_private.return_status_ptr = enabled;
  performI2CTransmitThenReceive(
      rtc,
      rtc->transmit_buffer, 1,
      &rtc->_private.register_storage[0], sizeof(rtc->_private.register_storage[0]));
}