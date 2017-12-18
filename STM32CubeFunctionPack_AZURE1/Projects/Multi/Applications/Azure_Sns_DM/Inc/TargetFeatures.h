/**
  ******************************************************************************
  * @file    TargetFeatures.h 
  * @author  Central LAB
  * @version V3.1.0
  * @date    27-Sept-2017
  * @brief   Specification of the HW Features for each target platform
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; COPYRIGHT(c) 2017 STMicroelectronics</center></h2>
  *
  * Licensed under MCD-ST Liberty SW License Agreement V2, (the "License");
  * You may not use this file except in compliance with the License.
  * You may obtain a copy of the License at:
  *
  *        http://www.st.com/software_license_agreement_liberty_v2
  *
  * Redistribution and use in source and binary forms, with or without modification,
  * are permitted provided that the following conditions are met:
  *   1. Redistributions of source code must retain the above copyright notice,
  *      this list of conditions and the following disclaimer.
  *   2. Redistributions in binary form must reproduce the above copyright notice,
  *      this list of conditions and the following disclaimer in the documentation
  *      and/or other materials provided with the distribution.
  *   3. Neither the name of STMicroelectronics nor the names of its contributors
  *      may be used to endorse or promote products derived from this software
  *      without specific prior written permission.
  *
  * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  *
  ******************************************************************************
  */
  
/* Define to prevent recursive inclusion -------------------------------------*/  
#ifndef _TARGET_FEATURES_H_
#define _TARGET_FEATURES_H_

#ifdef __cplusplus
 extern "C" {
#endif 

/* Includes ------------------------------------------------------------------*/
#include <stdlib.h>
#ifdef USE_STM32F4XX_NUCLEO
  #include "stm32f4xx_hal.h"
  #ifndef STM32F429xx
    #include "stm32f4xx_nucleo.h"
  #else /* STM32F429xx */
    #include "stm32f4xx_nucleo_144.h"
  #endif /* STM32F429xx */
  #include "stm32f4xx_hal_conf.h"
  #include "stm32f4xx_UART.h"
  #include "stm32f4xx_I2C.h"
  #include "stm32f4xx_periph_conf.h"

  #include "x_nucleo_iks01a2.h"
  #include "x_nucleo_iks01a2_accelero.h"
  #include "x_nucleo_iks01a2_gyro.h"
  #include "x_nucleo_iks01a2_magneto.h"
  #include "x_nucleo_iks01a2_humidity.h"
  #include "x_nucleo_iks01a2_temperature.h"
  #include "x_nucleo_iks01a2_pressure.h"
  #ifndef IKS01A2
    #include "x_nucleo_iks01a1.h"
    #include "x_nucleo_iks01a1_accelero.h"
    #include "x_nucleo_iks01a1_gyro.h"
    #include "x_nucleo_iks01a1_magneto.h"
    #include "x_nucleo_iks01a1_humidity.h"
    #include "x_nucleo_iks01a1_temperature.h"
    #include "x_nucleo_iks01a1_pressure.h"
  #else /* IKS01A2 */
   #include "SensorMappFunc.h"
  #endif /* IKS01A2 */
#endif /* USE_STM32F4XX_NUCLEO */

#ifdef USE_STM32L4XX_NUCLEO
  #include "stm32l4xx_hal.h"
  #include "stm32l4xx_nucleo.h"
  #include "stm32l4xx_hal_conf.h"
  #include "stm32l4xx_UART.h"
  #include "stm32l4xx_I2C.h"
  #include "stm32l4xx_periph_conf.h"

  #include "x_nucleo_iks01a2.h"
  #include "x_nucleo_iks01a2_accelero.h"
  #include "x_nucleo_iks01a2_gyro.h"
  #include "x_nucleo_iks01a2_magneto.h"
  #include "x_nucleo_iks01a2_humidity.h"
  #include "x_nucleo_iks01a2_temperature.h"
  #include "x_nucleo_iks01a2_pressure.h"

  #include "x_nucleo_iks01a1.h"
  #include "x_nucleo_iks01a1_accelero.h"
  #include "x_nucleo_iks01a1_gyro.h"
  #include "x_nucleo_iks01a1_magneto.h"
  #include "x_nucleo_iks01a1_humidity.h"
  #include "x_nucleo_iks01a1_temperature.h"
  #include "x_nucleo_iks01a1_pressure.h"
#endif /* USE_STM32L4XX_NUCLEO */

#ifdef USE_STM32L475E_IOT01
#include "stm32l4xx_hal.h"
#include "stm32l4xx_hal_conf.h"
#include "stm32l475e_iot01.h"
#include "stm32l475e_iot01_accelero.h"
#include "stm32l475e_iot01_psensor.h"
#include "stm32l475e_iot01_gyro.h"
#include "stm32l475e_iot01_hsensor.h"
#include "stm32l475e_iot01_tsensor.h"
#include "stm32l475e_iot01_magneto.h"
#endif /* USE_STM32L475E_IOT01 */

#include "azure1_config.h"
#ifdef USE_STM32L475E_IOT01
#include "m24sr_wrapper.h"
#else /* USE_STM32L475E_IOT01 */
#include "drv_I2C_M24SR.h"
#endif /* USE_STM32L475E_IOT01 */
#include "lib_TagType4.h"
#ifdef USE_STM32L475E_IOT01
#include "wifi.h"
#endif /* USE_STM32L475E_IOT01 */

/* Exported defines ------------------------------------------------------- */
#ifndef USE_STM32L475E_IOT01
#define MAX_TEMP_SENSORS 2
#ifndef STM32F429xx
#define AZURE_SOCKET_TELEMETRY 0
#define AZURE_SOCKET_FOTA  1
#endif /* STM32F429xx */
#endif /* USE_STM32L475E_IOT01 */
/* Exported types ------------------------------------------------------- */

/**
 * @brief  Target's Features data structure definition
 */
typedef struct
{
#ifndef USE_STM32L475E_IOT01
  int32_t NumTempSensors;
  void *HandleTempSensors[MAX_TEMP_SENSORS];

  void *HandlePressSensor;
  void *HandleHumSensor;

  uint32_t HWAdvanceFeatures;
  void *HandleAccSensor;
  void *HandleGyroSensor;
  void *HandleMagSensor;

  uint8_t SnsAltFunc;
  uint8_t EmulateSns;
#endif /* USE_STM32L475E_IOT01 */
  RTC_HandleTypeDef RtcHandle;
  uint32_t TIM_CC1_Pulse;
  uint32_t NFCInitialized;

#if defined(STM32F429xx) || defined(STM32L476xx) || (defined USE_STM32L475E_IOT01)
  RNG_HandleTypeDef  RngHandle;
#endif
} TargetFeatures_t;

/* Exported variables ------------------------------------------------------- */
extern TargetFeatures_t TargetBoardFeatures;
#ifdef USE_STM32L475E_IOT01
extern UART_HandleTypeDef UartHandle;
#endif /* USE_STM32L475E_IOT01 */

#ifdef __cplusplus
}
#endif

#endif /* _TARGET_FEATURES_H_ */

/******************* (C) COPYRIGHT 2017 STMicroelectronics *****END OF FILE****/

