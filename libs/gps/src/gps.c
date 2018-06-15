/*
 * @File  gps.c
 * @Brief 
 * 
 * @Author: Neucrack 
 * @Date: 2018-06-14 18:13:36 
 * @Last Modified by: Neucrack
 * @Last Modified time: 2018-06-15 18:56:01
 */

#include "gps.h"
#include "api_hal_uart.h"
#include "assert.h"
#include "buffer.h"
#include "api_debug.h"
#include "gps_parse.h"



#define GPS_CMD_HEADER "$PGKC"
const unsigned char Hex_Str[16]={'0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F'};


static bool isCmdSendTimeOut = false;
static HANDLE semCmdSending = NULL;
static uint8_t tmp[GPS_NMEA_FRAME_BUFFER_LENGTH+1];
static Buffer_t gpsNmeaBuffer;
static uint8_t  gpsDataBuffer[GPS_DATA_BUFFER_MAX_LENGTH];


void GPS_Init()
{
    //Initialize buffer to cache nmea message
    Buffer_Init(&gpsNmeaBuffer,gpsDataBuffer,GPS_DATA_BUFFER_MAX_LENGTH);
}

void GPS_Send(char* cmd,uint8_t len)
{
    UART_Write(UART2,cmd,len);
}

void GPS_Update(uint8_t* data,uint32_t length)
{
    int32_t index;
    int32_t index2;
    
    Buffer_Puts(&gpsNmeaBuffer,data,length);
    GPS_DEBUG_I("semCmdSending0:%d",semCmdSending);
    if(semCmdSending == NULL)
    {
        index = Buffer_Query(&gpsNmeaBuffer,"$GNVTG",strlen("$GNVTG"),Buffer_StartPostion(&gpsNmeaBuffer));
        if(index >= 0)
        {
            // GPS_DEBUG_I("find $GNVTG");
            index = Buffer_Query(&gpsNmeaBuffer,"\r\n",strlen("\r\n"),index);
            if(index >= 0)
            {
                // GPS_DEBUG_I("find complete GPS frame");
                
                memset(tmp,0,sizeof(tmp));
                uint32_t len = Buffer_Size2(&gpsNmeaBuffer,index)+1;
                // GPS_DEBUG_I("frame len:%d",len);
                if(!Buffer_Gets(&gpsNmeaBuffer,tmp,len>GPS_NMEA_FRAME_BUFFER_LENGTH?GPS_NMEA_FRAME_BUFFER_LENGTH:len))
                {
                    GPS_DEBUG_I("get data from buffer fail");
                    return;
                }
                GPS_DEBUG_I("parse nmea frame");
                GPS_Parse(tmp);
            }
        }
    }
    else//sending command
    {
        index = Buffer_Query(&gpsNmeaBuffer,GPS_CMD_HEADER,strlen(GPS_CMD_HEADER),Buffer_StartPostion(&gpsNmeaBuffer));
        if(index >= 0)
        {
            index2 = Buffer_Query(&gpsNmeaBuffer,"\r\n",strlen("\r\n"),index);
            if(index2 >= 0)
            {
                GPS_DEBUG_I("GPS find ack message");
                memset(tmp,0,sizeof(tmp));
                uint32_t len = Buffer_Size2(&gpsNmeaBuffer,index2)+1;
                if(!Buffer_Gets(&gpsNmeaBuffer,tmp,len>GPS_NMEA_FRAME_BUFFER_LENGTH?GPS_NMEA_FRAME_BUFFER_LENGTH:len))
                {
                    GPS_DEBUG_I("get data from buffer fail");
                    return;
                }
                if(semCmdSending)
                {
                    OS_ReleaseSemaphore(semCmdSending);
                }
            }
        }
    }
}

/**
 * 
 * @param str: string to check parity, must end with '\0'
 * 
 */
bool GPS_CheckParity(char* str)
{
    // char str[] = "$GPGGA,085014.955,2839.2050,n,11549.5721,E,104,03.6,76.6,M,-6.2,M,,*4C";
    int i;
    if(str == NULL)
        return false;
    if(strstr(str,"*") == NULL)
        return false;

    char result = *(str+1);
    for (i = 2;*(str+i) !='*'; i++)
    {
        result ^= *(str+i);
    }
    if(Hex_Str[result>>4] == str[i+1] && Hex_Str[result&0X0F] == str[i+2])
        return true;
    return false;
}

bool GPS_IsCMDValid(uint16_t cmd)
{
    if( cmd == GPS_CMD_ACK || 
        cmd == GPS_CMD_NMEA_OUTPUT_INTERVAL 
        )
        return true;
    return false;
}

bool GPS_GetAckParam(char* str, GPS_CMD_t* cmd, GPS_CMD_Ack_t* result)
{
    Assert(!(str == NULL || cmd == NULL ||result == NULL),"param error");
    char* index = strstr(str,GPS_CMD_HEADER);
    if(!index)
        return false;
    if(!GPS_CheckParity(index))
    {
        GPS_DEBUG_I("check parity fail");
        return false;
    }
    uint16_t ackCmd0 = (index[5]-'0')*100+(index[6]-'0')*10+(index[7]-'0');
    if(ackCmd0 == GPS_CMD_ACK)
    {
        uint16_t ackCmd = (index[9]-'0')*100+(index[10]-'0')*10+(index[11]-'0');
        if(!GPS_IsCMDValid(ackCmd))
        {
            GPS_DEBUG_I("ack check fail,cmd:%d",ackCmd);
            return false;
        }
        *cmd = ackCmd;
        *result = (index[13]-'0');
    }
    else
    {
        //not support now
        return false;
    }
    return true;
}

/**
 * Add parity tail and send to gps
 */
void GPS_CMDSend(char* str)
{
    // char a[] = "$GPGGA,085014.955,2839.2050,n,11549.5721,E,104,03.6,76.6,M,-6.2,M,,*4C";
	int i;
    char result = *(str+1);
    for (i = 2;*(str+i) != 0; i++)
    {
        result ^= *(str+i);
    }
    *(str+i++) = '*';
    *(str+i++) = Hex_Str[result>>4];
	*(str+i++) = Hex_Str[result&0X0F];
	*(str+i++) = '\r';
	*(str+i++) = '\n';
    *(str+i) = 0;
    GPS_DEBUG_I("gps send cmd:%s,len:%d",str,strlen(str));
    Buffer_Clear(&gpsNmeaBuffer);
	GPS_Send(str,strlen(str));
}

void OnCmdAckFail(void* param)
{
    isCmdSendTimeOut = true;
    OS_ReleaseSemaphore(semCmdSending);    
    GPS_DEBUG_I("gps ack time out");
}

bool GPS_SendCMDWaitAck(GPS_CMD_t cmdSend, char* cmdStr, uint16_t timeout)
{
    isCmdSendTimeOut = false;
    semCmdSending = OS_CreateSemaphore(0);
    GPS_CMDSend(cmdStr);
    OS_StartCallbackTimer(OS_GetUserMainHandle(),timeout,OnCmdAckFail,NULL);
    OS_WaitForSemaphore(semCmdSending,OS_TIME_OUT_WAIT_FOREVER);
    OS_DeleteSemaphore(semCmdSending);
    semCmdSending = NULL;
    if(isCmdSendTimeOut)
    {
        GPS_DEBUG_I("GPS exec command fail");
        return false;
    }
    else
    {
        OS_StopCallbackTimer(OS_GetUserMainHandle(),OnCmdAckFail,NULL);
        char* ack = strstr(tmp,GPS_CMD_HEADER);
        GPS_CMD_t cmd;
        GPS_CMD_Ack_t result;

        if(!GPS_GetAckParam(ack,&cmd,&result))
        {
            GPS_DEBUG_I("check ack fail");
            GPS_DEBUG_I("GPS ack:%s",ack);
            return false;
        }
        if(cmd != cmdSend || result != GPS_CMD_ACK_EXEC_SUCCESS)
        {
            GPS_DEBUG_I("result error,cmd:%d,result:%d",cmdSend,result);
            return false;
        }
    }
    GPS_DEBUG_I("GPS exec command:%d success",cmd);
    return true;
}

bool GPS_SetOutputInterval(uint16_t intervalMs)
{
    char temp[GPS_BUFFER_MAX_LENGTH+6];
    snprintf(temp,GPS_BUFFER_MAX_LENGTH,"%s%03d,%d",GPS_CMD_HEADER,GPS_CMD_NMEA_OUTPUT_INTERVAL,intervalMs);
    return GPS_SendCMDWaitAck(GPS_CMD_NMEA_OUTPUT_INTERVAL,temp,GPS_TIME_OUT_CMD);
}
