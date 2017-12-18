/**
  ******************************************************************************
  * @file    OTA.c
  * @author  Central LAB
  * @version V3.1.0
  * @date    27-Sept-2017
  * @brief   Over-the-Air Update API implementation
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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#if ((defined USE_STM32L4XX_NUCLEO ) || (defined USE_STM32L475E_IOT01))
  #include <string.h>
#endif /* ((defined USE_STM32L4XX_NUCLEO ) || (defined USE_STM32L475E_IOT01475xx)) */

#include "azure1_config.h"

#ifdef USE_STM32F4XX_NUCLEO
  #include "stm32f4xx_hal.h"
  #ifdef STM32F429xx
    #include "stm32f4xx_nucleo_144.h"
  #else /* STM32F429xx */
    #include "stm32f4xx_nucleo.h"
  #endif /* STM32F429xx */
#endif /* USE_STM32F4XX_NUCLEO */

#ifdef USE_STM32L4XX_NUCLEO
  #include "stm32l4xx_hal.h"
  #include "stm32l4xx_nucleo.h"
#endif /* USE_STM32L4XX_NUCLEO */

#ifdef USE_STM32L475E_IOT01
  #include "stm32l4xx_hal.h"
#endif /* USE_STM32L475E_IOT01 */

#include "OTA.h"

/* Local types ---------------------------------------------------------------*/
typedef struct
{
  uint32_t Version;
  uint32_t MagicNum;
  uint32_t OTAStartAdd;
  uint32_t OTAMaxSize;
  uint32_t ProgStartAdd;
} BootLoaderFeatures_t;

/* Local defines -------------------------------------------------------------*/

/* Compliant BootLoader version */
#define BL_VERSION_MAJOR 1
#define BL_VERSION_MINOR 2
#define BL_VERSION_PATCH 0

/* Board/BlueNRG FW OTA Postion */
#define OTA_ADDRESS_START  0x08080008

/* Board  FW OTA Magic Number Position */
#define OTA_MAGIC_NUM_POS  0x08080000

/* Board  FW OTA Magic Number */
#define OTA_MAGIC_NUM 0xDEADBEEF

/* Uncomment the following define for enabling the PRINTF capability if it's supported */
#define OTA_ENABLE_PRINTF

#ifdef OTA_ENABLE_PRINTF
  /* Each application must declare it's printf implementation if it wants to use it */
  #define OTA_PRINTF AZURE_PRINTF
#else /* OTA_ENABLE_PRINTF */
  #define OTA_PRINTF(...)
#endif /* OTA_ENABLE_PRINTF */

/* Local Macros -------------------------------------------------------------*/
#define OTA_ERROR_FUNCTION() { while(1);}

/* Private variables ---------------------------------------------------------*/
static uint32_t SizeOfUpdateBlueFW=0;
static uint32_t SizeOfUpdateBlueFWCopy=0;
static uint32_t AspecteduwCRCValue=0;
static uint32_t Address = OTA_ADDRESS_START;

static BootLoaderFeatures_t *BootLoaderFeatures = (BootLoaderFeatures_t *)0x08003F00;

#if ((defined USE_STM32L4XX_NUCLEO ) || (defined USE_STM32L475E_IOT01))
/* Local function prototypes --------------------------------------------------*/
static uint32_t GetPage(uint32_t Address);
static uint32_t GetBank(uint32_t Address);
#endif /* ((defined USE_STM32L4XX_NUCLEO ) || (defined USE_STM32L475E_IOT01)) */

/* Exported functions  --------------------------------------------------*/

/**
 * @brief Function for restarting the FOTA procedure
 * @param None
 * @retval None
 */
void CleanBeforeRestart(void)
{
  HAL_FLASH_Lock();
  SizeOfUpdateBlueFW=0;
  SizeOfUpdateBlueFWCopy=0;
  AspecteduwCRCValue=0;
  Address = OTA_ADDRESS_START;
}

/**
 * @brief Function for overriding the Size of firmware image
 * @param uint32_t SizeOfUpdate  size of the firmware image [bytes]
 * @retval None
 */
void ReWriteSizeOfUpdate(uint32_t SizeOfUpdate)
{
  SizeOfUpdateBlueFWCopy = SizeOfUpdateBlueFW = SizeOfUpdate;
}

/**
 * @brief Function for reading the Remaining Size of firmware image
 * @param None
 * @retval uint32_t Remaining size of the firmware image [bytes]
 */
uint32_t ReadRemSizeOfUpdate(void)
{
  return SizeOfUpdateBlueFW;
}

/**
 * @brief Function for Testing the BootLoader Compliance
 * @param None
 * @retval int8_t Return value for checking purpouse (0/-1 == Ok/Error)
 */
int8_t CheckBootLoaderCompliance(void)
{ 
  OTA_PRINTF("Testing BootLoaderCompliance:\r\n");
  OTA_PRINTF("\tVersion  %ld.%ld.%ld\r\n",
              BootLoaderFeatures->Version>>16     ,
             (BootLoaderFeatures->Version>>8)&0xFF,
              BootLoaderFeatures->Version    &0xFF);

  if((( BootLoaderFeatures->Version>>16      )!=BL_VERSION_MAJOR) |
     (((BootLoaderFeatures->Version>>8 )&0xFF)!=BL_VERSION_MINOR) |
      ((BootLoaderFeatures->Version     &0xFF)!=BL_VERSION_PATCH)) {
    OTA_PRINTF("\tBL Version  Ko\r\n");
    return 0;
  } else {
    OTA_PRINTF("\tBL Version  Ok\r\n");
  }

  if(BootLoaderFeatures->MagicNum==OTA_MAGIC_NUM) {
    OTA_PRINTF("\tMagicNum    Ok\r\n");
  } else {
    OTA_PRINTF("\tMagicNum    Ko\r\n");
    return 0;
  }

  OTA_PRINTF("\tMaxSize  0x%lx\r\n",BootLoaderFeatures->OTAMaxSize);

  if(BootLoaderFeatures->OTAStartAdd==(OTA_ADDRESS_START-8)) {
    OTA_PRINTF("\tOTAStartAdd Ok\r\n");
  } else {
    OTA_PRINTF("\tOTAStartAdd Ko\r\n");
    return 0;
  }
  
  return 1;
}

#ifdef USE_STM32F4XX_NUCLEO
/**
 * @brief Function for Updating the Firmware
 * @param uint32_t *SizeOfUpdate Remaining size of the firmware image [bytes].
 * @param uint8_t *att_data attribute data
 * @param int32_t data_length length of the data
 * @param uint8_t WriteMagicNum 1/0 for writing or not the magic number
 * @retval int8_t Return value for checking purpouse (1/-1 == Ok/Error)
 */
int8_t UpdateFWBlueMS(uint32_t *SizeOfUpdate,uint8_t * att_data, int32_t data_length,uint8_t WriteMagicNum)
{
  int8_t ReturnValue=0;
  /* Save the Packed received */
  //OTA_PRINTF("What UpdateFWBlueMS receives InSizeOfUpdate=%d lenght=%d\r\n",*SizeOfUpdate,data_length);
  if(data_length>(SizeOfUpdateBlueFW)){
    /* Too many bytes...Something wrong... necessity to send it again... */
    OTA_PRINTF("OTA something wrong data_length=%ld RemSizeOfUpdate=%ld....\r\nPlease Try again\r\n",data_length,(SizeOfUpdateBlueFW));
    ReturnValue = -1;
    /* Reset for Restarting again */
    *SizeOfUpdate=0;
  } else {
    int32_t Counter;
    /* Save the received OTA packed ad save it to flash */
    /* Unlock the Flash to enable the flash control register access *************/
    HAL_FLASH_Unlock();
    
    for(Counter=0;Counter<data_length;Counter++) {
      if(HAL_FLASH_Program(TYPEPROGRAM_BYTE, Address,att_data[Counter])==HAL_OK) {
      //printf("%c",att_data[Counter]);
       Address++;
      } else {
        /* Error occurred while writing data in Flash memory.
           User can add here some code to deal with this error
           FLASH_ErrorTypeDef errorcode = HAL_FLASH_GetError(); */
        OTA_ERROR_FUNCTION();
      }
    }
    /* Reduce the remaing bytes for OTA completition */
    *SizeOfUpdate -= data_length;
    SizeOfUpdateBlueFW-=data_length;

    if(SizeOfUpdateBlueFW==0) {
      /* We had received the whole firmware and we have saved it in Flash */

      if(WriteMagicNum) {
        uint32_t uwCRCValue = 0;
        if(AspecteduwCRCValue) {
          /* Make the CRC integrety check */         

          /* CRC handler declaration */
          CRC_HandleTypeDef   CrcHandle;

          /* Init CRC for OTA-integrity check */
          CrcHandle.Instance = CRC;

          if(HAL_CRC_GetState(&CrcHandle) != HAL_CRC_STATE_RESET) {
            HAL_CRC_DeInit(&CrcHandle);
          }

          if (HAL_CRC_Init(&CrcHandle) != HAL_OK) {
            /* Initialization Error */
            OTA_ERROR_FUNCTION();
          } else {
            OTA_PRINTF("CRC  Initialized\n\r");
          }
          /* Compute the CRC */
          uwCRCValue = HAL_CRC_Calculate(&CrcHandle, (uint32_t *)OTA_ADDRESS_START, SizeOfUpdateBlueFWCopy>>2);

          if(uwCRCValue==AspecteduwCRCValue) {
            ReturnValue=1;
            OTA_PRINTF("OTA CRC-checked\r\n");
          }
        } else {
          ReturnValue=1;
        }

        if(ReturnValue==1) {
          /* We write the Magic number for making the OTA at the next Board reset */
          Address = OTA_MAGIC_NUM_POS;

          if(HAL_FLASH_Program(TYPEPROGRAM_WORD, Address,OTA_MAGIC_NUM)!=HAL_OK) {
          /* Error occurred while writing data in Flash memory.
             User can add here some code to deal with this error
             FLASH_ErrorTypeDef errorcode = HAL_FLASH_GetError(); */
          OTA_ERROR_FUNCTION();
          } else {
            OTA_PRINTF("OTA will be installed at next board reset\r\n");
          }
        } else {
          ReturnValue=-1;
          if(AspecteduwCRCValue) {
            OTA_PRINTF("Wrong CRC! Computed=%lx  aspected=%lx ... Try again\r\n",uwCRCValue,AspecteduwCRCValue);
          }
        }
      }
    }

    /* Lock the Flash to disable the flash control register access (recommended
     to protect the FLASH memory against possible unwanted operation) *********/
    HAL_FLASH_Lock();
  }
  return ReturnValue;
}

/**
 * @brief Start Function for Updating the Firmware
 * @param uint32_t SizeOfUpdate  size of the firmware image [bytes]
 * @param uint32_t uwCRCValue aspected CRV value
 * @retval None
 */
void StartUpdateFWBlueMS(uint32_t SizeOfUpdate, uint32_t uwCRCValue)
{
  FLASH_EraseInitTypeDef EraseInitStruct;
  uint32_t SectorError = 0;
  OTA_PRINTF("Start FLASH Erase\r\n");

  SizeOfUpdateBlueFWCopy = SizeOfUpdateBlueFW = SizeOfUpdate;
  AspecteduwCRCValue = uwCRCValue;
  Address = OTA_ADDRESS_START;

  EraseInitStruct.TypeErase = TYPEERASE_SECTORS;
  EraseInitStruct.VoltageRange = VOLTAGE_RANGE_3;
  EraseInitStruct.Sector = FLASH_SECTOR_8;

  if(SizeOfUpdate>(0x40000-8)) {
    /* We need 3 sectors of 128KB */
    EraseInitStruct.NbSectors = 3;
  } else if(SizeOfUpdate>(0x20000-8)) {
    /* We need 2 sectors of 128KB */
    EraseInitStruct.NbSectors = 2;
  } else {
    /* One sector of 128KB is enough */
    EraseInitStruct.NbSectors = 1;
  }

  /* Unlock the Flash to enable the flash control register access *************/
  HAL_FLASH_Unlock();

  if(HAL_FLASHEx_Erase(&EraseInitStruct, &SectorError) != HAL_OK){
    /* Error occurred while sector erase. 
      User can add here some code to deal with this error. 
      SectorError will contain the faulty sector and then to know the code error on this sector,
      user can call function 'HAL_FLASH_GetError()'
      FLASH_ErrorTypeDef errorcode = HAL_FLASH_GetError(); */
    OTA_ERROR_FUNCTION();
  } else {    
    OTA_PRINTF("End FLASH Erase %ld Sectors of 128KB\r\n",EraseInitStruct.NbSectors);
  }

  /* Lock the Flash to disable the flash control register access (recommended
  to protect the FLASH memory against possible unwanted operation) *********/
  HAL_FLASH_Lock();
}
#endif /* USE_STM32F4XX_NUCLEO */

#if ((defined USE_STM32L4XX_NUCLEO ) || (defined USE_STM32L475E_IOT01))
/**
 * @brief Function for Updating the Firmware
 * @param uint32_t *SizeOfUpdate Remaining size of the firmware image [bytes]
 * @param uint8_t *att_data attribute data
 * @param int32_t data_length length of the data
 * @param uint8_t WriteMagicNum 1/0 for writing or not the magic number
 * @retval int8_t Return value for checking purpouse (1/-1 == Ok/Error)
 */
int8_t UpdateFWBlueMS(uint32_t *SizeOfUpdate,uint8_t * att_data, int32_t data_length,uint8_t WriteMagicNum)
{
  int8_t ReturnValue=0;
  /* Save the Packed received */
  //OTA_PRINTF("What UpdateFWBlueMS receives SizeOfUpdateBlueFW=%d InSizeOfUpdate=%d lenght=%d\r\n",SizeOfUpdateBlueFW,*SizeOfUpdate,data_length);
  if(data_length>(SizeOfUpdateBlueFW)){
    /* Too many bytes...Something wrong... necessity to send it again... */
    OTA_PRINTF("OTA something wrong data_length=%ld RemSizeOfUpdate=%ld....\r\nPlease Try again\r\n",data_length,(SizeOfUpdateBlueFW));
    ReturnValue = -1;
    /* Reset for Restarting again */
    *SizeOfUpdate=0;
  } else {
    uint64_t ValueToWrite;
    int32_t Counter;
    /* Save the received OTA packed ad save it to flash */
    /* Unlock the Flash to enable the flash control register access *************/
    HAL_FLASH_Unlock();
    //printf("\r\n%ld 0x%X\r\n",Address,Address);
    
    for(Counter=0;Counter<data_length;Counter+=8) {
      memcpy((uint8_t*) &ValueToWrite,att_data+Counter,data_length-Counter+1);

      if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, Address,ValueToWrite)==HAL_OK) {
       Address+=8;
      } else {
        /* Error occurred while writing data in Flash memory.
           User can add here some code to deal with this error
           FLASH_ErrorTypeDef errorcode = HAL_FLASH_GetError(); */
        OTA_ERROR_FUNCTION();
      }
    }
    /* Reduce the remaing bytes for OTA completition */
    *SizeOfUpdate -= data_length;
    SizeOfUpdateBlueFW-=data_length;

    if(SizeOfUpdateBlueFW==0) {
      /* We had received the whole firmware and we have saved it in Flash */
      OTA_PRINTF("OTA Update saved\r\n");

      if(WriteMagicNum) {
        uint32_t uwCRCValue = 0;
        if(AspecteduwCRCValue) {
          /* Make the CRC integrety check */          
          /* CRC handler declaration */
          CRC_HandleTypeDef   CrcHandle;

          /* Init CRC for OTA-integrity check */
          CrcHandle.Instance = CRC;
          /* The default polynomial is used */
          CrcHandle.Init.DefaultPolynomialUse    = DEFAULT_POLYNOMIAL_ENABLE;

          /* The default init value is used */
          CrcHandle.Init.DefaultInitValueUse     = DEFAULT_INIT_VALUE_ENABLE;

          /* The input data are not inverted */
          CrcHandle.Init.InputDataInversionMode  = CRC_INPUTDATA_INVERSION_NONE;

          /* The output data are not inverted */
          CrcHandle.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;

          /* The input data are 32-bit long words */
          CrcHandle.InputDataFormat              = CRC_INPUTDATA_FORMAT_WORDS;

          if(HAL_CRC_GetState(&CrcHandle) != HAL_CRC_STATE_RESET) {
            HAL_CRC_DeInit(&CrcHandle);
          }

          if (HAL_CRC_Init(&CrcHandle) != HAL_OK) {
            /* Initialization Error */
            OTA_ERROR_FUNCTION();
          } else {
            OTA_PRINTF("CRC  Initialized\n\r");
          }
          /* Compute the CRC */
          uwCRCValue = HAL_CRC_Calculate(&CrcHandle, (uint32_t *)OTA_ADDRESS_START, SizeOfUpdateBlueFWCopy>>2);

          if(uwCRCValue==AspecteduwCRCValue) {
            ReturnValue=1;
            OTA_PRINTF("OTA CRC-checked\r\n");
          }
        } else {
          ReturnValue=1;
        }
        if(ReturnValue==1) {
          /* We write the Magic number for making the OTA at the next Board reset */
          Address = OTA_MAGIC_NUM_POS;
          ValueToWrite=OTA_MAGIC_NUM;

          if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, Address,ValueToWrite)!=HAL_OK) {
            /* Error occurred while writing data in Flash memory.
               User can add here some code to deal with this error
               FLASH_ErrorTypeDef errorcode = HAL_FLASH_GetError(); */
            OTA_ERROR_FUNCTION();
          } else {
            OTA_PRINTF("OTA will be installed at next board reset\r\n");
          }
        } else {
          ReturnValue=-1;
          if(AspecteduwCRCValue) {
            OTA_PRINTF("Wrong CRC! Computed=%lx  aspected=%lx ... Try again\r\n",uwCRCValue,AspecteduwCRCValue);
          }
        }
      }
    }

    /* Lock the Flash to disable the flash control register access (recommended
     to protect the FLASH memory against possible unwanted operation) *********/
    HAL_FLASH_Lock();
  }
  return ReturnValue;
}

/**
 * @brief Start Function for Updating the Firmware
 * @param uint32_t SizeOfUpdate  size of the firmware image [bytes]
 * @param uint32_t uwCRCValue aspected CRV value
 * @retval None
 */
void StartUpdateFWBlueMS(uint32_t SizeOfUpdate, uint32_t uwCRCValue)
{
  FLASH_EraseInitTypeDef EraseInitStruct;
  uint32_t SectorError = 0;
  OTA_PRINTF("Start FLASH Erase\r\n");

  SizeOfUpdateBlueFWCopy = SizeOfUpdateBlueFW = SizeOfUpdate;
  AspecteduwCRCValue = uwCRCValue;
  Address = OTA_ADDRESS_START;

  EraseInitStruct.TypeErase   = FLASH_TYPEERASE_PAGES;
  EraseInitStruct.Banks       = GetBank(OTA_MAGIC_NUM_POS);
  EraseInitStruct.Page        = GetPage(OTA_MAGIC_NUM_POS);
  EraseInitStruct.NbPages     = (SizeOfUpdate+8+FLASH_PAGE_SIZE-1)/FLASH_PAGE_SIZE;
    
  /* Unlock the Flash to enable the flash control register access *************/
  HAL_FLASH_Unlock();

  if(HAL_FLASHEx_Erase(&EraseInitStruct, &SectorError) != HAL_OK){
    /* Error occurred while sector erase. 
      User can add here some code to deal with this error. 
      SectorError will contain the faulty sector and then to know the code error on this sector,
      user can call function 'HAL_FLASH_GetError()'
      FLASH_ErrorTypeDef errorcode = HAL_FLASH_GetError(); */
    OTA_ERROR_FUNCTION();
  } else {    
    OTA_PRINTF("End FLASH Erase %ld Pages of 2KB\r\n",EraseInitStruct.NbPages);
  }

  /* Lock the Flash to disable the flash control register access (recommended
  to protect the FLASH memory against possible unwanted operation) *********/
  HAL_FLASH_Lock();
}

/* Local functions  --------------------------------------------------*/
/**
  * @brief  Gets the page of a given address
  * @param  Addr: Address of the FLASH Memory
  * @retval The page of a given address
  */
static uint32_t GetPage(uint32_t Addr)
{
  uint32_t page = 0;
  
  if (Addr < (FLASH_BASE + FLASH_BANK_SIZE)) {
    /* Bank 1 */
    page = (Addr - FLASH_BASE) / FLASH_PAGE_SIZE;
  } else {
    /* Bank 2 */
    page = (Addr - (FLASH_BASE + FLASH_BANK_SIZE)) / FLASH_PAGE_SIZE;
  }
  
  return page;
}

/**
  * @brief  Gets the bank of a given address
  * @param  Addr: Address of the FLASH Memory
  * @retval The bank of a given address
  */
static uint32_t GetBank(uint32_t Addr)
{
  uint32_t bank = 0;
  
  if (READ_BIT(SYSCFG->MEMRMP, SYSCFG_MEMRMP_FB_MODE) == 0) {
    /* No Bank swap */
    if (Addr < (FLASH_BASE + FLASH_BANK_SIZE)) {
      bank = FLASH_BANK_1;
    } else {
      bank = FLASH_BANK_2;
    }
  } else {
    /* Bank swap */
    if (Addr < (FLASH_BASE + FLASH_BANK_SIZE)){
      bank = FLASH_BANK_2;
    } else {
      bank = FLASH_BANK_1;
    }
  }
  
  return bank;
}
#endif /* ((defined USE_STM32L4XX_NUCLEO ) || (defined USE_STM32L475E_IOT01)) */

/******************* (C) COPYRIGHT 2017 STMicroelectronics *****END OF FILE****/
