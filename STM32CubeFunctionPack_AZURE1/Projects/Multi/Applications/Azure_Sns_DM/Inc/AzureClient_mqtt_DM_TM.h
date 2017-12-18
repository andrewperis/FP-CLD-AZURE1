/**
 ******************************************************************************
 * @file    AzureClient_mqtt_DM_TM.h
 * @author  Central LAB
 * @version V3.1.0
 * @date    27-Sept-2017
 * @brief   Header file for Main MQTT Application
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
#ifndef _AZURE_CLIENT_MQTT_H
#define _AZURE_CLIENT_MQTT_H

#ifdef __cplusplus
extern "C" {
#endif
#include "azure_c_shared_utility/crt_abstractions.h"
#include "iothub_client_version.h"

/* enum values are in lower case per design */
#define FIRMWARE_UPDATE_STATUS_VALUES \
        Running, \
        Rebootting, \
        Downloading, \
        DownloadFailed, \
        DownloadComplete, \
        Applying, \
        ApplyFailed, \
        ApplyComplete, \
        Ended
DEFINE_ENUM(FIRMWARE_UPDATE_STATUS, FIRMWARE_UPDATE_STATUS_VALUES)
#ifdef STM32F429xx
extern void AzureClient_mqtt_DM_TM(void const * argument);
#else /* STM32F429xx */
extern void AzureClient_mqtt_DM_TM(void);
#endif /* STM32F429xx */
extern void ReportState(FIRMWARE_UPDATE_STATUS status);
extern void WaitAllTheMessages(void);

#ifdef __cplusplus
}
#endif

#endif /* _AZURE_CLIENT_MQTT_H */
/******************* (C) COPYRIGHT 2017 STMicroelectronics *****END OF FILE****/
