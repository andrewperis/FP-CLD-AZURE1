/**
 ******************************************************************************
 * @file    tcpsocketSTM32Cube.c
 * @author  Central LAB
 * @version V3.1.0
 * @date    27-Sept-2017
 * @brief   Adapter to tcp Socket
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

#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "azure_c_shared_utility/socketio.h"
//#include <sys/types.h>
//#include <sys/socket.h>
//#include <sys/select.h>

#include "azure1_config.h"

/*
#ifdef TIZENRT
#include <net/lwip/tcp.h>
#else
#include <netinet/tcp.h>
#endif
*/
#include "lwip/tcp.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

//#include <netdb.h>
//#include <unistd.h>
//#include <fcntl.h>
//#include <errno.h>
#include "azure_c_shared_utility/singlylinkedlist.h"
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/xlogging.h"

#define SOCKET_SUCCESS          0
#define INVALID_SOCKET          -1

// connect timeout in seconds
#define CONNECT_TIMEOUT         10

//#define DEBUG_SOCKETIO

typedef enum IO_STATE_TAG
{
    IO_STATE_CLOSED,
    IO_STATE_OPENING,
    IO_STATE_OPEN,
    IO_STATE_CLOSING,
    IO_STATE_ERROR
} IO_STATE;

typedef struct PENDING_SOCKET_IO_TAG
{
    unsigned char* bytes;
    size_t size;
    ON_SEND_COMPLETE on_send_complete;
    void* callback_context;
    SINGLYLINKEDLIST_HANDLE pending_io_list;
} PENDING_SOCKET_IO;

typedef struct SOCKET_IO_INSTANCE_TAG
{
    int socket;
    ON_BYTES_RECEIVED on_bytes_received;
    ON_IO_ERROR on_io_error;
    void* on_bytes_received_context;
    void* on_io_error_context;
    char* hostname;
    int port;
    IO_STATE io_state;
    SINGLYLINKEDLIST_HANDLE pending_io_list;
} SOCKET_IO_INSTANCE;

/**
 * @brief This function will clone an option given by name and value
 * @param const char* name Option name
 * @param const void* value Option value
 * @retval NULL
 */
static void* socketio_CloneOption(const char* name, const void* value)
{
#ifdef DEBUG_SOCKETIO
  AZURE_PRINTF("socketio_CloneOption\r\n");
#endif /* DEBUG_SOCKETIO */
    (void)name;
    (void)value;
    return NULL;
}

/**
 * @brief This function destroys an option previously created
 * @param const char* name Option name
 * @param const void* value Option value
 */
static void socketio_DestroyOption(const char* name, const void* value)
{
#ifdef DEBUG_SOCKETIO
  AZURE_PRINTF("socketio_DestroyOption\r\n");
#endif /* DEBUG_SOCKETIO */
    (void)name;
    (void)value;
}

/**
 * @brief Retrieve socket options
 * @param CONCRETE_IO_HANDLE handle socket handle
 * @retval OPTIONHANDLER_HANDLE option result
 */
static OPTIONHANDLER_HANDLE socketio_retrieveoptions(CONCRETE_IO_HANDLE handle)
{
    OPTIONHANDLER_HANDLE result;
    (void)handle;
#ifdef DEBUG_SOCKETIO
  AZURE_PRINTF("socketio_retrieveoptions\r\n");
#endif /* DEBUG_SOCKETIO */
    result = OptionHandler_Create(socketio_CloneOption, socketio_DestroyOption, socketio_setoption);
    if (result == NULL)
    {
        LogError("unable to OptionHandler_Create");
        /*return as is*/
    }
    else
    {
        /*insert here work to add the options to "result" handle*/
    }
    return result;
}

static const IO_INTERFACE_DESCRIPTION socket_io_interface_description = 
{
    socketio_retrieveoptions,
    socketio_create,
    socketio_destroy,
    socketio_open,
    socketio_close,
    socketio_send,
    socketio_dowork,
    socketio_setoption
};

/**
 * @brief Indicate error
 * @param SOCKET_IO_INSTANCE* socket_io_instance
 */
static void indicate_error(SOCKET_IO_INSTANCE* socket_io_instance)
{
#ifdef DEBUG_SOCKETIO
  AZURE_PRINTF("indicate_error\r\n");
#endif /* DEBUG_SOCKETIO */
    if (socket_io_instance->on_io_error != NULL)
    {
        socket_io_instance->on_io_error(socket_io_instance->on_io_error_context);
    }
}

/**
 * @brief Add pending IO
 * @param SOCKET_IO_INSTANCE* socket_io_instance
 * @param const unsigned char* buffer
 * @param size_t size
 * @param ON_SEND_COMPLETE on_send_complete
 * @param void* callback_context
 * @retval int line error
 */
static int add_pending_io(SOCKET_IO_INSTANCE* socket_io_instance, const unsigned char* buffer, size_t size, ON_SEND_COMPLETE on_send_complete, void* callback_context)
{
    int result;
    PENDING_SOCKET_IO* pending_socket_io = (PENDING_SOCKET_IO*)malloc(sizeof(PENDING_SOCKET_IO));
#ifdef DEBUG_SOCKETIO
  AZURE_PRINTF("add_pending_io\r\n");
#endif /* DEBUG_SOCKETIO */
    if (pending_socket_io == NULL)
    {
        result = __LINE__;
    }
    else
    {
        pending_socket_io->bytes = (unsigned char*)malloc(size);
        if (pending_socket_io->bytes == NULL)
        {
            LogError("Allocation Failure: Unable to allocate pending list.");
            free(pending_socket_io);
            result = __LINE__;
        }
        else
        {
            pending_socket_io->size = size;
            pending_socket_io->on_send_complete = on_send_complete;
            pending_socket_io->callback_context = callback_context;
            pending_socket_io->pending_io_list = socket_io_instance->pending_io_list;
            (void)memcpy(pending_socket_io->bytes, buffer, size);

            if (singlylinkedlist_add(socket_io_instance->pending_io_list, pending_socket_io) == NULL)
            {
                LogError("Failure: Unable to add socket to pending list.");
                free(pending_socket_io->bytes);
                free(pending_socket_io);
                result = __LINE__;
            }
            else
            {
                result = 0;
            }
        }
    }

    return result;
}

/**
 * @brief Add pending IO
 * @param void* io_create_parameters
 * @retval CONCRETE_IO_HANDLE socket handle
 */
CONCRETE_IO_HANDLE socketio_create(void* io_create_parameters)
{
    SOCKETIO_CONFIG* socket_io_config = io_create_parameters;
    SOCKET_IO_INSTANCE* result;
#ifdef DEBUG_SOCKETIO
  AZURE_PRINTF("socketio_create\r\n");
#endif /* DEBUG_SOCKETIO */

    if (socket_io_config == NULL)
    {
        LogError("Invalid argument: socket_io_config is NULL");
        result = NULL;
    }
    else
    {
        result = malloc(sizeof(SOCKET_IO_INSTANCE));
        if (result != NULL)
        {
            result->pending_io_list = singlylinkedlist_create();
            if (result->pending_io_list == NULL)
            {
                LogError("Failure: singlylinkedlist_create unable to create pending list.");
                free(result);
                result = NULL;
            }
            else
            {
                if (socket_io_config->hostname != NULL)
                {
                    result->hostname = (char*)malloc(strlen(socket_io_config->hostname) + 1);
                    if (result->hostname != NULL)
                    {
                        (void)strcpy(result->hostname, socket_io_config->hostname);
                    }

                    result->socket = INVALID_SOCKET;
                }
                else
                {
                    result->hostname = NULL;
                    result->socket = *((int*)socket_io_config->accepted_socket);
                }

                if ((result->hostname == NULL) && (result->socket == INVALID_SOCKET))
                {
                    LogError("Failure: hostname == NULL and socket is invalid.");
                    singlylinkedlist_destroy(result->pending_io_list);
                    free(result);
                    result = NULL;
                }
                else
                {
                    result->port = socket_io_config->port;
                    result->on_bytes_received = NULL;
                    result->on_io_error = NULL;
                    result->on_bytes_received_context = NULL;
                    result->on_io_error_context = NULL;
                    result->io_state = IO_STATE_CLOSED;
                }
            }
        }
        else
        {
            LogError("Allocation Failure: SOCKET_IO_INSTANCE");
        }
    }

    return result;
}

/**
 * @brief Socket IO destroy
 * @param CONCRETE_IO_HANDLE socket_io
 */
void socketio_destroy(CONCRETE_IO_HANDLE socket_io)
{
#ifdef DEBUG_SOCKETIO
  AZURE_PRINTF("socketio_destroy\r\n");
#endif /* DEBUG_SOCKETIO */
    if (socket_io != NULL)
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
        /* we cannot do much if the close fails, so just ignore the result */
        if (socket_io_instance->socket != INVALID_SOCKET)
        {
            close(socket_io_instance->socket);
        }

        /* clear allpending IOs */
        LIST_ITEM_HANDLE first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
        while (first_pending_io != NULL)
        {
            PENDING_SOCKET_IO* pending_socket_io = (PENDING_SOCKET_IO*)singlylinkedlist_item_get_value(first_pending_io);
            if (pending_socket_io != NULL)
            {
                free(pending_socket_io->bytes);
                free(pending_socket_io);
            }

            (void)singlylinkedlist_remove(socket_io_instance->pending_io_list, first_pending_io);
            first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
        }

        singlylinkedlist_destroy(socket_io_instance->pending_io_list);
        free(socket_io_instance->hostname);
        free(socket_io);
    }
}

/**
 * @brief Socket IO open
 * @param CONCRETE_IO_HANDLE socket_io
 * @param ON_IO_OPEN_COMPLETE on_io_open_complete
 * @param void* on_io_open_complete_context
 * @param ON_BYTES_RECEIVED on_bytes_received
 * @param void* on_bytes_received_context
 * @param ON_IO_ERROR on_io_error
 * @param void* on_io_error_context
 * @retval line error
 */
int socketio_open(CONCRETE_IO_HANDLE socket_io, ON_IO_OPEN_COMPLETE on_io_open_complete, void* on_io_open_complete_context, ON_BYTES_RECEIVED on_bytes_received, void* on_bytes_received_context, ON_IO_ERROR on_io_error, void* on_io_error_context)
{
    int result;
	int retval = -1;
	int select_errno = 0;

    SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
#ifdef DEBUG_SOCKETIO
  AZURE_PRINTF("socketio_open\r\n");
#endif /* DEBUG_SOCKETIO */
    if (socket_io == NULL)
    {
        LogError("Invalid argument: SOCKET_IO_INSTANCE is NULL");
        result = __LINE__;
    }
    else
    {
        if (socket_io_instance->io_state != IO_STATE_CLOSED)
        {
            LogError("Failure: socket state is not closed.");
            result = __LINE__;
        }
        else if (socket_io_instance->socket != INVALID_SOCKET)
        {
            // Opening an accepted socket
            socket_io_instance->on_bytes_received_context = on_bytes_received_context;
            socket_io_instance->on_bytes_received = on_bytes_received;
            socket_io_instance->on_io_error = on_io_error;
            socket_io_instance->on_io_error_context = on_io_error_context;

            socket_io_instance->io_state = IO_STATE_OPEN;

            result = 0;
        }
        else
        {
            struct addrinfo* addrInfo;
            char portString[16];

            socket_io_instance->socket = socket(AF_INET, SOCK_STREAM, 0);
            if (socket_io_instance->socket < SOCKET_SUCCESS)
            {
                LogError("Failure: socket create failure %d.", socket_io_instance->socket);
                result = __LINE__;
            }
            else
            {
                struct addrinfo addrHint = { 0 };
                addrHint.ai_family = AF_INET;
                addrHint.ai_socktype = SOCK_STREAM;
                addrHint.ai_protocol = 0;

                sprintf(portString, "%u", socket_io_instance->port);
                int err = lwip_getaddrinfo(socket_io_instance->hostname, portString, &addrHint, &addrInfo);
                if (err != 0)
                {
                    LogError("Failure: getaddrinfo failure %d.", err);
                    close(socket_io_instance->socket);
                    socket_io_instance->socket = INVALID_SOCKET;
                    result = __LINE__;
                }
                else
                {
                    int flags;
                    if ((-1 == (flags = fcntl(socket_io_instance->socket, F_GETFL, 0))) ||
                        (fcntl(socket_io_instance->socket, F_SETFL, flags | O_NONBLOCK) == -1))
                    {
                        LogError("Failure: fcntl failure.");
                        close(socket_io_instance->socket);
                        socket_io_instance->socket = INVALID_SOCKET;
                        result = __LINE__;
                    }
                    else
                    {
                        err = connect(socket_io_instance->socket, addrInfo->ai_addr, sizeof(*addrInfo->ai_addr));
                        
                        if ((err != 0) && (errno != EINPROGRESS))
                        {
                            LogError("Failure: connect failure %d.", errno);
                            close(socket_io_instance->socket);
                            socket_io_instance->socket = INVALID_SOCKET;
                            result = __LINE__;
                            
                            AZURE_PRINTF("Connect failure\r\n");
                            while(1);
                        }
                        else
                        {
                            if (err != 0)
                            {
                                fd_set fdset;
                                struct timeval tv;

                                FD_ZERO(&fdset);
                                FD_SET(socket_io_instance->socket, &fdset);
                                tv.tv_sec = CONNECT_TIMEOUT;
                                tv.tv_usec = 0;

                                do
                                {
                                    retval = select(socket_io_instance->socket + 1, NULL, &fdset, NULL, &tv);
                                  
                                    if (retval < 0)
                                    {
                                        select_errno = errno;
                                    }
                                } while (retval < 0 && select_errno == EINTR);
                                
                                if (retval != 1)
                                {
                                    LogError("Failure: select failure.");
                                    close(socket_io_instance->socket);
                                    socket_io_instance->socket = INVALID_SOCKET;
                                    result = __LINE__;

                                    AZURE_PRINTF("Socket select failure %d\r\n",retval);
                                    while(1);
                                }
                                else
                                {
                                    int so_error = 0;
                                    socklen_t len = sizeof(so_error);
                                    err = getsockopt(socket_io_instance->socket, SOL_SOCKET, SO_ERROR, &so_error, &len);
                                    if (err != 0)
                                    {
                                        LogError("Failure: getsockopt failure %d.", errno);
                                        close(socket_io_instance->socket);
                                        socket_io_instance->socket = INVALID_SOCKET;
                                        result = __LINE__;
                                    }
                                    else if (so_error != 0)
                                    {
                                        err = so_error;
                                        LogError("Failure: connect failure %d.", so_error);
                                        close(socket_io_instance->socket);
                                        socket_io_instance->socket = INVALID_SOCKET;
                                        result = __LINE__;

                                        AZURE_PRINTF("Socket connect failure %d\r\n",so_error);
                                        while(1);
                                    }
                                }
                            }
                            if (err == 0)
                            {
                                socket_io_instance->on_bytes_received = on_bytes_received;
                                socket_io_instance->on_bytes_received_context = on_bytes_received_context;

                                socket_io_instance->on_io_error = on_io_error;
                                socket_io_instance->on_io_error_context = on_io_error_context;

                                socket_io_instance->io_state = IO_STATE_OPEN;

                                result = 0;
                            }
                        }
                    }
                    freeaddrinfo(addrInfo);
                }
            }
        }
    }

    if (on_io_open_complete != NULL)
    {
        on_io_open_complete(on_io_open_complete_context, result == 0 ? IO_OPEN_OK : IO_OPEN_ERROR);
    }

    return result;
}

/**
 * @brief Socket IO close
 * @param CONCRETE_IO_HANDLE socket_io
 * @param ON_IO_CLOSE_COMPLETE on_io_close_complete
 * @param void* callback_context
 * @retval line error
 */
int socketio_close(CONCRETE_IO_HANDLE socket_io, ON_IO_CLOSE_COMPLETE on_io_close_complete, void* callback_context)
{
    int result = 0;
#ifdef DEBUG_SOCKETIO
  AZURE_PRINTF("socketio_close\r\n");
#endif /* DEBUG_SOCKETIO */

    if (socket_io == NULL)
    {
        result = __LINE__;
    }
    else
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
        if ((socket_io_instance->io_state != IO_STATE_CLOSED) && (socket_io_instance->io_state != IO_STATE_CLOSING))
        {
            // Only close if the socket isn't already in the closed or closing state
            (void)shutdown(socket_io_instance->socket, SHUT_RDWR);
            close(socket_io_instance->socket);
            socket_io_instance->socket = INVALID_SOCKET;
            socket_io_instance->io_state = IO_STATE_CLOSED;
        }

        if (on_io_close_complete != NULL)
        {
            on_io_close_complete(callback_context);
        }

        result = 0;
    }

    return result;
}

/**
 * @brief Socket IO send
 * @param CONCRETE_IO_HANDLE socket_io
 * @param const void* buffer
 * @param size_t size
 * @param ON_SEND_COMPLETE on_send_complete
 * @param void* callback_context
 * @retval int line error
 */
int socketio_send(CONCRETE_IO_HANDLE socket_io, const void* buffer, size_t size, ON_SEND_COMPLETE on_send_complete, void* callback_context)
{
    int result;

#ifdef DEBUG_SOCKETIO
  AZURE_PRINTF("socketio_send\r\n");
#endif /* DEBUG_SOCKETIO */
    if ((socket_io == NULL) ||
        (buffer == NULL) ||
        (size == 0))
    {
        /* Invalid arguments */
        LogError("Invalid argument: send given invalid parameter");
        result = __LINE__;
    }
    else
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
        if (socket_io_instance->io_state != IO_STATE_OPEN)
        {
            LogError("Failure: socket state is not opened.");
            result = __LINE__;
        }
        else
        {
            LIST_ITEM_HANDLE first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
            if (first_pending_io != NULL)
            {
                if (add_pending_io(socket_io_instance, buffer, size, on_send_complete, callback_context) != 0)
                {
                    LogError("Failure: add_pending_io failed.");
                    result = __LINE__;
                }
                else
                {
                    result = 0;
                }
            }
            else
            {
                int send_result = send(socket_io_instance->socket, buffer, size, 0);
                if (send_result != size)
                {
                    if (send_result == INVALID_SOCKET)
                    {
                        if (errno == EAGAIN) /*send says "come back later" with EAGAIN - likely the socket buffer cannot accept more data*/
                        {
                            /*do nothing*/
                            result = 0;
                        }
                        else
                        {
                            indicate_error(socket_io_instance);
                            LogError("Failure: sending socket failed. errno=%d (%s).", errno, strerror(errno));
                            result = __LINE__;
                        }
                    }
                    else
                    {
                        /* queue data */
                        if (add_pending_io(socket_io_instance, (unsigned char const*)buffer + send_result, size - send_result, on_send_complete, callback_context) != 0)
                        {
                            LogError("Failure: add_pending_io failed.");
                            result = __LINE__;
                        }
                        else
                        {
                            result = 0;
                        }
                    }
                }
                else
                {
                    if (on_send_complete != NULL)
                    {
                        on_send_complete(callback_context, IO_SEND_OK);
                    }

                    result = 0;
                }
            }
        }
    }

    return result;
}

/**
 * @brief Socket do work
 * @param CONCRETE_IO_HANDLE socket_io
 */
void socketio_dowork(CONCRETE_IO_HANDLE socket_io)
{
#ifdef DEBUG_SOCKETIO
  //AZURE_PRINTF("socketio_dowork\r\n");
#endif /* DEBUG_SOCKETIO */
    if (socket_io != NULL)
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
        if (socket_io_instance->io_state == IO_STATE_OPEN)
        {
            int received = 1;

            LIST_ITEM_HANDLE first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
            while (first_pending_io != NULL)
            {
                PENDING_SOCKET_IO* pending_socket_io = (PENDING_SOCKET_IO*)singlylinkedlist_item_get_value(first_pending_io);
                if (pending_socket_io == NULL)
                {
                    socket_io_instance->io_state = IO_STATE_ERROR;
                    indicate_error(socket_io_instance);
                    LogError("Failure: retrieving socket from list");
                    break;
                }

                int send_result = send(socket_io_instance->socket, pending_socket_io->bytes, pending_socket_io->size, 0);
                if (send_result != pending_socket_io->size)
                {
                    if (send_result == INVALID_SOCKET)
                    {
                        if (errno == EAGAIN) /*send says "come back later" with EAGAIN - likely the socket buffer cannot accept more data*/
                        {
                            /*do nothing until next dowork */
                            break;
                        }
                        else
                        {
                            free(pending_socket_io->bytes);
                            free(pending_socket_io);
                            (void)singlylinkedlist_remove(socket_io_instance->pending_io_list, first_pending_io);

                            LogError("Failure: sending Socket information. errno=%d (%s).", errno, strerror(errno));
                            socket_io_instance->io_state = IO_STATE_ERROR;
                            indicate_error(socket_io_instance);
                        }
                    }
                    else
                    {
                        /* simply wait until next dowork */
                        (void)memmove(pending_socket_io->bytes, pending_socket_io->bytes + send_result, pending_socket_io->size - send_result);
                        pending_socket_io->size -= send_result;
                        break;
                    }
                }
                else
                {
                    if (pending_socket_io->on_send_complete != NULL)
                    {
                        pending_socket_io->on_send_complete(pending_socket_io->callback_context, IO_SEND_OK);
                    }

                    free(pending_socket_io->bytes);
                    free(pending_socket_io);
                    if (singlylinkedlist_remove(socket_io_instance->pending_io_list, first_pending_io) != 0)
                    {
                        socket_io_instance->io_state = IO_STATE_ERROR;
                        indicate_error(socket_io_instance);
                        LogError("Failure: unable to remove socket from list");
                    }
                }

                first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
            }

            while (received > 0)
            {
                unsigned char* recv_bytes = malloc(RECEIVE_BYTES_VALUE);
                if (recv_bytes == NULL)
                {
                    LogError("Socketio_Failure: NULL allocating input buffer.");
                    indicate_error(socket_io_instance);
                }
                else
                {
                    received = recv(socket_io_instance->socket, recv_bytes, RECEIVE_BYTES_VALUE, 0);
                    if (received > 0)
                    {
                        if (socket_io_instance->on_bytes_received != NULL)
                        {
                            /* explictly ignoring here the result of the callback */
                            (void)socket_io_instance->on_bytes_received(socket_io_instance->on_bytes_received_context, recv_bytes, received);
                        }
                    }
                    free(recv_bytes);
                }
            }
        }
    }
}

// Edison is missing this from netinet/tcp.h, but this code still works if we manually define it.
#ifndef SOL_TCP
#define SOL_TCP 6
#endif


/**
 * @brief Socket set option
 * @param CONCRETE_IO_HANDLE socket_io
 * @param const char* optionName
 * @param const void* value
 * @retval err no
 */
int socketio_setoption(CONCRETE_IO_HANDLE socket_io, const char* optionName, const void* value)
{
    int result;

#ifdef DEBUG_SOCKETIO
  AZURE_PRINTF("socketio_setoption\r\n");
#endif /* DEBUG_SOCKETIO */
    if (socket_io == NULL ||
        optionName == NULL ||
        value == NULL)
    {
        result = __LINE__;
    }
    else
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;

        if (strcmp(optionName, "tcp_keepalive") == 0)
        {
            result = setsockopt(socket_io_instance->socket, SOL_SOCKET, SO_KEEPALIVE, value, sizeof(int));
            if (result == -1) result = errno;
        }
        else if (strcmp(optionName, "tcp_keepalive_time") == 0)
        {
#ifdef __APPLE__
            result = setsockopt(socket_io_instance->socket, IPPROTO_TCP, TCP_KEEPALIVE, value, sizeof(int));
#else
            result = setsockopt(socket_io_instance->socket, SOL_TCP, TCP_KEEPIDLE, value, sizeof(int));
#endif
            if (result == -1) result = errno;
        }
        else if (strcmp(optionName, "tcp_keepalive_interval") == 0)
        {
            result = setsockopt(socket_io_instance->socket, SOL_TCP, TCP_KEEPINTVL, value, sizeof(int));
            if (result == -1) result = errno;
        }
        else
        {
            result = __LINE__;
        }
    }

    return result;
}

/**
 * @brief  Get interface description
 * @retval const IO_INTERFACE_DESCRIPTION
 */
const IO_INTERFACE_DESCRIPTION* socketio_get_interface_description(void)
{
#ifdef DEBUG_SOCKETIO
  AZURE_PRINTF("socketio_get_interface_description\r\n");
#endif /* DEBUG_SOCKETIO */
    return &socket_io_interface_description;
}

/******************* (C) COPYRIGHT 2017 STMicroelectronics *****END OF FILE****/
