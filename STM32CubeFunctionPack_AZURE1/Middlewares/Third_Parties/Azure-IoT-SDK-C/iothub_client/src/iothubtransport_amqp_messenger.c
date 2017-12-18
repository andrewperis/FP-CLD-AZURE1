// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#include <stdbool.h>
#include "azure_c_shared_utility/optimize_size.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/agenttime.h" 
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/uniqueid.h"
#include "azure_c_shared_utility/singlylinkedlist.h"
#include "azure_uamqp_c/link.h"
#include "azure_uamqp_c/messaging.h"
#include "azure_uamqp_c/message_sender.h"
#include "azure_uamqp_c/message_receiver.h"
#include "uamqp_messaging.h"
#include "iothub_client_private.h"
#include "iothub_client_version.h"
#include "iothubtransport_amqp_messenger.h"

#define RESULT_OK 0
#define INDEFINITE_TIME ((time_t)(-1))

#define IOTHUB_DEVICES_PATH_FMT                         "%s/devices/%s"
#define IOTHUB_EVENT_SEND_ADDRESS_FMT                   "amqps://%s/messages/events"
#define IOTHUB_MESSAGE_RECEIVE_ADDRESS_FMT              "amqps://%s/messages/devicebound"
#define MESSAGE_SENDER_LINK_NAME_PREFIX                 "link-snd"
#define MESSAGE_SENDER_MAX_LINK_SIZE                    UINT64_MAX
#define MESSAGE_RECEIVER_LINK_NAME_PREFIX               "link-rcv"
#define MESSAGE_RECEIVER_MAX_LINK_SIZE                  65536
#define DEFAULT_EVENT_SEND_RETRY_LIMIT                  10
#define DEFAULT_EVENT_SEND_TIMEOUT_SECS                 600
#define MAX_MESSAGE_SENDER_STATE_CHANGE_TIMEOUT_SECS    300
#define MAX_MESSAGE_RECEIVER_STATE_CHANGE_TIMEOUT_SECS  300
#define UNIQUE_ID_BUFFER_SIZE                           37
#define STRING_NULL_TERMINATOR                          '\0'
 
typedef struct MESSENGER_INSTANCE_TAG
{
	STRING_HANDLE device_id;
    STRING_HANDLE product_info;
	STRING_HANDLE iothub_host_fqdn;
	SINGLYLINKEDLIST_HANDLE waiting_to_send;
	SINGLYLINKEDLIST_HANDLE in_progress_list;
	MESSENGER_STATE state;
	
	ON_MESSENGER_STATE_CHANGED_CALLBACK on_state_changed_callback;
	void* on_state_changed_context;

	bool receive_messages;
	ON_MESSENGER_MESSAGE_RECEIVED on_message_received_callback;
	void* on_message_received_context;

	SESSION_HANDLE session_handle;
	LINK_HANDLE sender_link;
	MESSAGE_SENDER_HANDLE message_sender;
	MESSAGE_SENDER_STATE message_sender_current_state;
	MESSAGE_SENDER_STATE message_sender_previous_state;
	LINK_HANDLE receiver_link;
	MESSAGE_RECEIVER_HANDLE message_receiver;
	MESSAGE_RECEIVER_STATE message_receiver_current_state;
	MESSAGE_RECEIVER_STATE message_receiver_previous_state;

	size_t event_send_retry_limit;
	size_t event_send_error_count;
	size_t event_send_timeout_secs;
	time_t last_message_sender_state_change_time;
	time_t last_message_receiver_state_change_time;
} MESSENGER_INSTANCE;

typedef struct MESSENGER_SEND_EVENT_TASK_TAG
{
	IOTHUB_MESSAGE_LIST* message;
	ON_MESSENGER_EVENT_SEND_COMPLETE on_event_send_complete_callback;
	void* context;
	time_t send_time;
	MESSENGER_INSTANCE *messenger;
	bool is_timed_out;
} MESSENGER_SEND_EVENT_TASK;

// @brief
//     Evaluates if the ammount of time since start_time is greater or lesser than timeout_in_secs.
// @param is_timed_out
//     Set to 1 if a timeout has been reached, 0 otherwise. Not set if any failure occurs.
// @returns
//     0 if no failures occur, non-zero otherwise.
static int is_timeout_reached(time_t start_time, size_t timeout_in_secs, int *is_timed_out)
{
	int result;

	if (start_time == INDEFINITE_TIME)
	{
		LogError("Failed to verify timeout (start_time is INDEFINITE)");
		result = __FAILURE__;
	}
	else
	{
		time_t current_time;

		if ((current_time = get_time(NULL)) == INDEFINITE_TIME)
		{
			LogError("Failed to verify timeout (get_time failed)");
			result = __FAILURE__;
		}
		else
		{
			if (get_difftime(current_time, start_time) >= timeout_in_secs)
			{
				*is_timed_out = 1;
			}
			else
			{
				*is_timed_out = 0;
			}

			result = RESULT_OK;
		}
	}

	return result;
}

static STRING_HANDLE create_devices_path(STRING_HANDLE iothub_host_fqdn, STRING_HANDLE device_id)
{
	STRING_HANDLE devices_path;

	if ((devices_path = STRING_new()) == NULL)
	{
		LogError("Failed creating devices_path (STRING_new failed)");
	}
	else
        {
		const char* iothub_host_fqdn_char_ptr = STRING_c_str(iothub_host_fqdn);
		const char* device_id_char_ptr = STRING_c_str(device_id);
        	if (STRING_sprintf(devices_path, IOTHUB_DEVICES_PATH_FMT, iothub_host_fqdn_char_ptr, device_id_char_ptr) != RESULT_OK)
		{
			STRING_delete(devices_path);
			devices_path = NULL;
			LogError("Failed creating devices_path (STRING_sprintf failed)");
		}
        }

	return devices_path;
}

static STRING_HANDLE create_event_send_address(STRING_HANDLE devices_path)
{
	STRING_HANDLE event_send_address;

	if ((event_send_address = STRING_new()) == NULL)
	{
		LogError("Failed creating the event_send_address (STRING_new failed)");
	}
	else
	{
		const char* devices_path_char_ptr = STRING_c_str(devices_path);
		if (STRING_sprintf(event_send_address, IOTHUB_EVENT_SEND_ADDRESS_FMT, devices_path_char_ptr) != RESULT_OK)
		{
			STRING_delete(event_send_address);
			event_send_address = NULL;
			LogError("Failed creating the event_send_address (STRING_sprintf failed)");
		}
	}

	return event_send_address;
}

static STRING_HANDLE create_event_sender_source_name(STRING_HANDLE link_name)
{
	STRING_HANDLE source_name;
	
	if ((source_name = STRING_new()) == NULL)
	{
		LogError("Failed creating the source_name (STRING_new failed)");
	}
	else
	{
		const char* link_name_char_ptr = STRING_c_str(link_name);
		if (STRING_sprintf(source_name, "%s-source", link_name_char_ptr) != RESULT_OK)
		{
			STRING_delete(source_name);
			source_name = NULL;
			LogError("Failed creating the source_name (STRING_sprintf failed)");
		}
	}

	return source_name;
}

static STRING_HANDLE create_message_receive_address(STRING_HANDLE devices_path)
{
	STRING_HANDLE message_receive_address;

	if ((message_receive_address = STRING_new()) == NULL)
	{
		LogError("Failed creating the message_receive_address (STRING_new failed)");
	}
	else
	{
		const char* devices_path_char_ptr = STRING_c_str(devices_path);
		if (STRING_sprintf(message_receive_address, IOTHUB_MESSAGE_RECEIVE_ADDRESS_FMT, devices_path_char_ptr) != RESULT_OK)
		{
			STRING_delete(message_receive_address);
			message_receive_address = NULL;
			LogError("Failed creating the message_receive_address (STRING_sprintf failed)");
		}
	}

	return message_receive_address;
}

static STRING_HANDLE create_message_receiver_target_name(STRING_HANDLE link_name)
{
	STRING_HANDLE target_name;

	if ((target_name = STRING_new()) == NULL)
	{
		LogError("Failed creating the target_name (STRING_new failed)");
	}
	else
	{
		const char* link_name_char_ptr = STRING_c_str(link_name);
		if (STRING_sprintf(target_name, "%s-target", link_name_char_ptr) != RESULT_OK)
		{
			STRING_delete(target_name);
			target_name = NULL;
			LogError("Failed creating the target_name (STRING_sprintf failed)");
		}
	}

	return target_name;
}

static STRING_HANDLE create_link_name(const char* prefix, const char* infix)
{
	char* unique_id;
	STRING_HANDLE tag = NULL;

	if ((unique_id = (char*)malloc(sizeof(char) * UNIQUE_ID_BUFFER_SIZE + 1)) == NULL)
	{
		LogError("Failed generating an unique tag (malloc failed)");
	}
	else
	{
        memset(unique_id, 0, sizeof(char) * UNIQUE_ID_BUFFER_SIZE + 1);

		if (UniqueId_Generate(unique_id, UNIQUE_ID_BUFFER_SIZE) != UNIQUEID_OK)
		{
			LogError("Failed generating an unique tag (UniqueId_Generate failed)");
		}
		else if ((tag = STRING_new()) == NULL)
		{
			LogError("Failed generating an unique tag (STRING_new failed)");
		}
		else if (STRING_sprintf(tag, "%s-%s-%s", prefix, infix, unique_id) != RESULT_OK)
		{
			STRING_delete(tag);
			tag = NULL;
			LogError("Failed generating an unique tag (STRING_sprintf failed)");
		}

		free(unique_id);
	}

	return tag;
}

static void update_messenger_state(MESSENGER_INSTANCE* instance, MESSENGER_STATE new_state)
{
	if (new_state != instance->state)
	{
		MESSENGER_STATE previous_state = instance->state;
		instance->state = new_state;

		if (instance->on_state_changed_callback != NULL)
		{
			instance->on_state_changed_callback(instance->on_state_changed_context, previous_state, new_state);
		}
	}
}

static void attach_device_client_type_to_link(LINK_HANDLE link, STRING_HANDLE product_info)
{
	fields attach_properties;
	AMQP_VALUE device_client_type_key_name;
	AMQP_VALUE device_client_type_value;
	int result;

	if ((attach_properties = amqpvalue_create_map()) == NULL)
	{
		LogError("Failed to create the map for device client type.");
	}
	else
	{
		if ((device_client_type_key_name = amqpvalue_create_symbol("com.microsoft:client-version")) == NULL)
		{
			LogError("Failed to create the key name for the device client type.");
		}
		else
		{
			if ((device_client_type_value = amqpvalue_create_string(STRING_c_str(product_info))) == NULL)
			{
				LogError("Failed to create the key value for the device client type.");
			}
			else
			{
				if ((result = amqpvalue_set_map_value(attach_properties, device_client_type_key_name, device_client_type_value)) != 0)
				{
					LogError("Failed to set the property map for the device client type (error code is: %d)", result);
				}
				else if ((result = link_set_attach_properties(link, attach_properties)) != 0)
				{
					LogError("Unable to attach the device client type to the link properties (error code is: %d)", result);
				}

				amqpvalue_destroy(device_client_type_value);
			}

			amqpvalue_destroy(device_client_type_key_name);
		}

		amqpvalue_destroy(attach_properties);
	}
}

static void destroy_event_sender(MESSENGER_INSTANCE* instance)
{
	if (instance->message_sender != NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_060: [`instance->message_sender` shall be destroyed using messagesender_destroy()]
		messagesender_destroy(instance->message_sender);
		instance->message_sender = NULL;
		instance->message_sender_current_state = MESSAGE_SENDER_STATE_IDLE;
		instance->message_sender_previous_state = MESSAGE_SENDER_STATE_IDLE;
		instance->last_message_sender_state_change_time = INDEFINITE_TIME;
	}

	if (instance->sender_link != NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_063: [`instance->sender_link` shall be destroyed using link_destroy()]
		link_destroy(instance->sender_link);
		instance->sender_link = NULL;
	}
}

static void on_event_sender_state_changed_callback(void* context, MESSAGE_SENDER_STATE new_state, MESSAGE_SENDER_STATE previous_state)
{
	if (context == NULL)
	{
		LogError("on_event_sender_state_changed_callback was invoked with a NULL context; although unexpected, this failure will be ignored");
	}
	else
	{
		if (new_state != previous_state)
		{
			MESSENGER_INSTANCE* instance = (MESSENGER_INSTANCE*)context;
			instance->message_sender_current_state = new_state;
			instance->message_sender_previous_state = previous_state;
			instance->last_message_sender_state_change_time = get_time(NULL);
		}
	}
}

static int create_event_sender(MESSENGER_INSTANCE* instance)
{
	int result;

	STRING_HANDLE link_name = NULL;
	STRING_HANDLE source_name = NULL;
	AMQP_VALUE source = NULL;
	AMQP_VALUE target = NULL;
	STRING_HANDLE devices_path = NULL;
	STRING_HANDLE event_send_address = NULL;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_033: [A variable, named `devices_path`, shall be created concatenating `instance->iothub_host_fqdn`, "/devices/" and `instance->device_id`]
	if ((devices_path = create_devices_path(instance->iothub_host_fqdn, instance->device_id)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_034: [If `devices_path` fails to be created, messenger_do_work() shall fail and return]
		result = __FAILURE__;
		LogError("Failed creating the message sender (failed creating the 'devices_path')");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_035: [A variable, named `event_send_address`, shall be created concatenating "amqps://", `devices_path` and "/messages/events"]
	else if ((event_send_address = create_event_send_address(devices_path)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_036: [If `event_send_address` fails to be created, messenger_do_work() shall fail and return]
		result = __FAILURE__;
		LogError("Failed creating the message sender (failed creating the 'event_send_address')");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_037: [A `link_name` variable shall be created using an unique string label per AMQP session]
	else if ((link_name = create_link_name(MESSAGE_SENDER_LINK_NAME_PREFIX, STRING_c_str(instance->device_id))) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_038: [If `link_name` fails to be created, messenger_do_work() shall fail and return]
		result = __FAILURE__;
		LogError("Failed creating the message sender (failed creating an unique link name)");
	}
	else if ((source_name = create_event_sender_source_name(link_name)) == NULL)
	{
		result = __FAILURE__;
		LogError("Failed creating the message sender (failed creating an unique source name)");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_039: [A `source` variable shall be created with messaging_create_source() using an unique string label per AMQP session]
	else if ((source = messaging_create_source(STRING_c_str(source_name))) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_040: [If `source` fails to be created, messenger_do_work() shall fail and return]
		result = __FAILURE__;
		LogError("Failed creating the message sender (messaging_create_source failed)");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_041: [A `target` variable shall be created with messaging_create_target() using `event_send_address`]
	else if ((target = messaging_create_target(STRING_c_str(event_send_address))) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_042: [If `target` fails to be created, messenger_do_work() shall fail and return]
		result = __FAILURE__;
		LogError("Failed creating the message sender (messaging_create_target failed)");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_043: [`instance->sender_link` shall be set using link_create(), passing `instance->session_handle`, `link_name`, "role_sender", `source` and `target` as parameters]
	else if ((instance->sender_link = link_create(instance->session_handle, STRING_c_str(link_name), role_sender, source, target)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_044: [If link_create() fails, messenger_do_work() shall fail and return]
		result = __FAILURE__;
		LogError("Failed creating the message sender (link_create failed)");
	}
	else
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_047: [`instance->sender_link` maximum message size shall be set to UINT64_MAX using link_set_max_message_size()]
		if (link_set_max_message_size(instance->sender_link, MESSAGE_SENDER_MAX_LINK_SIZE) != RESULT_OK)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_048: [If link_set_max_message_size() fails, it shall be logged and ignored.]
			LogError("Failed setting message sender link max message size.");
		}

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_049: [`instance->sender_link` should have a property "com.microsoft:client-version" set as `CLIENT_DEVICE_TYPE_PREFIX/IOTHUB_SDK_VERSION`, using amqpvalue_set_map_value() and link_set_attach_properties()]
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_050: [If amqpvalue_set_map_value() or link_set_attach_properties() fail, the failure shall be ignored]
		attach_device_client_type_to_link(instance->sender_link, instance->product_info);

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_051: [`instance->message_sender` shall be created using messagesender_create(), passing the `instance->sender_link` and `on_event_sender_state_changed_callback`]
		if ((instance->message_sender = messagesender_create(instance->sender_link, on_event_sender_state_changed_callback, (void*)instance)) == NULL)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_052: [If messagesender_create() fails, messenger_do_work() shall fail and return]
			result = __FAILURE__;
			link_destroy(instance->sender_link);
			instance->sender_link = NULL;
			LogError("Failed creating the message sender (messagesender_create failed)");
		}
		else
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_053: [`instance->message_sender` shall be opened using messagesender_open()]
			if (messagesender_open(instance->message_sender) != RESULT_OK)
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_054: [If messagesender_open() fails, messenger_do_work() shall fail and return]
				result = __FAILURE__;
				messagesender_destroy(instance->message_sender);
				instance->message_sender = NULL;
				link_destroy(instance->sender_link);
				instance->sender_link = NULL;
				LogError("Failed opening the AMQP message sender.");
			}
			else
			{
				result = RESULT_OK;
			}
		}
	}

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_055: [Before returning, messenger_do_work() shall release all the temporary memory it has allocated]
	if (link_name != NULL)
		STRING_delete(link_name);
	if (source_name != NULL)
		STRING_delete(source_name);
	if (source != NULL)
		amqpvalue_destroy(source);
	if (target != NULL)
		amqpvalue_destroy(target);
	if (devices_path != NULL)
		STRING_delete(devices_path);
	if (event_send_address != NULL)
		STRING_delete(event_send_address);

	return result;
}

static void destroy_message_receiver(MESSENGER_INSTANCE* instance)
{
	if (instance->message_receiver != NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_061: [`instance->message_receiver` shall be closed using messagereceiver_close()]
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_093: [`instance->message_receiver` shall be closed using messagereceiver_close()]
		if (messagereceiver_close(instance->message_receiver) != RESULT_OK)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_094: [If messagereceiver_close() fails, it shall be logged and ignored]
			LogError("Failed closing the AMQP message receiver (this failure will be ignored).");
		}

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_062: [`instance->message_receiver` shall be destroyed using messagereceiver_destroy()]
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_095: [`instance->message_receiver` shall be destroyed using messagereceiver_destroy()]
		messagereceiver_destroy(instance->message_receiver);

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_096: [`instance->message_receiver` shall be set to NULL]
		instance->message_receiver = NULL;
		instance->message_receiver_current_state = MESSAGE_RECEIVER_STATE_IDLE;
		instance->message_receiver_previous_state = MESSAGE_RECEIVER_STATE_IDLE;
		instance->last_message_receiver_state_change_time = INDEFINITE_TIME;
	}

	if (instance->receiver_link != NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_064: [`instance->receiver_link` shall be destroyed using link_destroy()]
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_097: [`instance->receiver_link` shall be destroyed using link_destroy()]
		link_destroy(instance->receiver_link);
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_098: [`instance->receiver_link` shall be set to NULL]
		instance->receiver_link = NULL;
	}
}

static void on_message_receiver_state_changed_callback(const void* context, MESSAGE_RECEIVER_STATE new_state, MESSAGE_RECEIVER_STATE previous_state)
{
	if (context == NULL)
	{
		LogError("on_message_receiver_state_changed_callback was invoked with a NULL context; although unexpected, this failure will be ignored");
	}
	else
	{
		if (new_state != previous_state)
		{
			MESSENGER_INSTANCE* instance = (MESSENGER_INSTANCE*)context;
			instance->message_receiver_current_state = new_state;
			instance->message_receiver_previous_state = previous_state;
			instance->last_message_receiver_state_change_time = get_time(NULL);
		}
	}
}

static MESSENGER_MESSAGE_DISPOSITION_INFO* create_message_disposition_info(MESSENGER_INSTANCE* messenger)
{
	MESSENGER_MESSAGE_DISPOSITION_INFO* result;

	if ((result = (MESSENGER_MESSAGE_DISPOSITION_INFO*)malloc(sizeof(MESSENGER_MESSAGE_DISPOSITION_INFO))) == NULL)
	{
		LogError("Failed creating MESSENGER_MESSAGE_DISPOSITION_INFO container (malloc failed)");
		result = NULL;
	}
	else
	{
		delivery_number message_id;

		if (messagereceiver_get_received_message_id(messenger->message_receiver, &message_id) != RESULT_OK)
		{
			LogError("Failed creating MESSENGER_MESSAGE_DISPOSITION_INFO container (messagereceiver_get_received_message_id failed)");
			free(result);
			result = NULL;
		}
		else
		{
			const char* link_name;

			if (messagereceiver_get_link_name(messenger->message_receiver, &link_name) != RESULT_OK)
			{
				LogError("Failed creating MESSENGER_MESSAGE_DISPOSITION_INFO container (messagereceiver_get_link_name failed)");
				free(result);
				result = NULL;
			}
			else if (mallocAndStrcpy_s(&result->source, link_name) != RESULT_OK)
			{
				LogError("Failed creating MESSENGER_MESSAGE_DISPOSITION_INFO container (failed copying link name)");
				free(result);
				result = NULL;
			}
			else
			{
				result->message_id = message_id;
			}
		}
	}

	return result;
}

static void destroy_message_disposition_info(MESSENGER_MESSAGE_DISPOSITION_INFO* disposition_info)
{
	free(disposition_info->source);
	free(disposition_info);
}

static AMQP_VALUE create_uamqp_disposition_result_from(MESSENGER_DISPOSITION_RESULT disposition_result)
{
	AMQP_VALUE uamqp_disposition_result;

	if (disposition_result == MESSENGER_DISPOSITION_RESULT_NONE)
	{
		uamqp_disposition_result = NULL; // intentionally not sending an answer.
	}
	else if (disposition_result == MESSENGER_DISPOSITION_RESULT_ACCEPTED)
	{
		uamqp_disposition_result = messaging_delivery_accepted();
	}
	else if (disposition_result == MESSENGER_DISPOSITION_RESULT_RELEASED)
	{
		uamqp_disposition_result = messaging_delivery_released();
	}
	else if (disposition_result == MESSENGER_DISPOSITION_RESULT_REJECTED)
	{
		uamqp_disposition_result = messaging_delivery_rejected("Rejected by application", "Rejected by application");
	}
	else
	{
		LogError("Failed creating a disposition result for messagereceiver (result %d is not supported)", disposition_result);
		uamqp_disposition_result = NULL;
	}

	return uamqp_disposition_result;
}

static AMQP_VALUE on_message_received_internal_callback(const void* context, MESSAGE_HANDLE message)
{
	AMQP_VALUE result;
	int api_call_result;
	IOTHUB_MESSAGE_HANDLE iothub_message;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_121: [An IOTHUB_MESSAGE_HANDLE shall be obtained from MESSAGE_HANDLE using IoTHubMessage_CreateFromUamqpMessage()]
	if ((api_call_result = IoTHubMessage_CreateFromUamqpMessage(message, &iothub_message)) != RESULT_OK)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_122: [If IoTHubMessage_CreateFromUamqpMessage() fails, on_message_received_internal_callback shall return the result of messaging_delivery_rejected()]
		result = messaging_delivery_rejected("Rejected due to failure reading AMQP message", "Failed reading AMQP message");

		LogError("on_message_received_internal_callback failed (IoTHubMessage_CreateFromUamqpMessage; error = %d).", api_call_result);
	}
	else
	{
		MESSENGER_INSTANCE* instance = (MESSENGER_INSTANCE*)context;
		MESSENGER_MESSAGE_DISPOSITION_INFO* message_disposition_info;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_186: [A MESSENGER_MESSAGE_DISPOSITION_INFO instance shall be created containing the source link name and message delivery ID]
		if ((message_disposition_info = create_message_disposition_info(instance)) == NULL)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_187: [**If the MESSENGER_MESSAGE_DISPOSITION_INFO instance fails to be created, on_message_received_internal_callback shall return messaging_delivery_released()]
			LogError("on_message_received_internal_callback failed (failed creating MESSENGER_MESSAGE_DISPOSITION_INFO).");
			result = messaging_delivery_released();
		}
		else
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_123: [`instance->on_message_received_callback` shall be invoked passing the IOTHUB_MESSAGE_HANDLE and MESSENGER_MESSAGE_DISPOSITION_INFO instance]
			MESSENGER_DISPOSITION_RESULT disposition_result = instance->on_message_received_callback(iothub_message, message_disposition_info, instance->on_message_received_context);

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_188: [The memory allocated for the MESSENGER_MESSAGE_DISPOSITION_INFO instance shall be released]
			destroy_message_disposition_info(message_disposition_info);

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_125: [If `instance->on_message_received_callback` returns MESSENGER_DISPOSITION_RESULT_ACCEPTED, on_message_received_internal_callback shall return the result of messaging_delivery_accepted()]
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_126: [If `instance->on_message_received_callback` returns MESSENGER_DISPOSITION_RESULT_RELEASED, on_message_received_internal_callback shall return the result of messaging_delivery_released()]
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_127: [If `instance->on_message_received_callback` returns MESSENGER_DISPOSITION_RESULT_REJECTED, on_message_received_internal_callback shall return the result of messaging_delivery_rejected()]
			result = create_uamqp_disposition_result_from(disposition_result);
		}
	}

	return result;
}

static int create_message_receiver(MESSENGER_INSTANCE* instance)
{
	int result;

	STRING_HANDLE devices_path = NULL;
	STRING_HANDLE message_receive_address = NULL;
	STRING_HANDLE link_name = NULL;
	STRING_HANDLE target_name = NULL;
	AMQP_VALUE source = NULL;
	AMQP_VALUE target = NULL;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_068: [A variable, named `devices_path`, shall be created concatenating `instance->iothub_host_fqdn`, "/devices/" and `instance->device_id`]
	if ((devices_path = create_devices_path(instance->iothub_host_fqdn, instance->device_id)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_069: [If `devices_path` fails to be created, messenger_do_work() shall fail and return]
		result = __FAILURE__;
		LogError("Failed creating the message receiver (failed creating the 'devices_path')");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_070: [A variable, named `message_receive_address`, shall be created concatenating "amqps://", `devices_path` and "/messages/devicebound"]
	else if ((message_receive_address = create_message_receive_address(devices_path)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_071: [If `message_receive_address` fails to be created, messenger_do_work() shall fail and return]
		result = __FAILURE__;
		LogError("Failed creating the message receiver (failed creating the 'message_receive_address')");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_072: [A `link_name` variable shall be created using an unique string label per AMQP session]
	else if ((link_name = create_link_name(MESSAGE_RECEIVER_LINK_NAME_PREFIX, STRING_c_str(instance->device_id))) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_073: [If `link_name` fails to be created, messenger_do_work() shall fail and return]
		result = __FAILURE__;
		LogError("Failed creating the message receiver (failed creating an unique link name)");
	}
	else if ((target_name = create_message_receiver_target_name(link_name)) == NULL)
	{
		result = __FAILURE__;
		LogError("Failed creating the message receiver (failed creating an unique target name)");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_074: [A `target` variable shall be created with messaging_create_target() using an unique string label per AMQP session]
	else if ((target = messaging_create_target(STRING_c_str(target_name))) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_075: [If `target` fails to be created, messenger_do_work() shall fail and return]
		result = __FAILURE__;
		LogError("Failed creating the message receiver (messaging_create_target failed)");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_076: [A `source` variable shall be created with messaging_create_source() using `message_receive_address`]
	else if ((source = messaging_create_source(STRING_c_str(message_receive_address))) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_077: [If `source` fails to be created, messenger_do_work() shall fail and return]
		result = __FAILURE__;
		LogError("Failed creating the message receiver (messaging_create_source failed)");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_078: [`instance->receiver_link` shall be set using link_create(), passing `instance->session_handle`, `link_name`, "role_receiver", `source` and `target` as parameters]
	else if ((instance->receiver_link = link_create(instance->session_handle, STRING_c_str(link_name), role_receiver, source, target)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_079: [If link_create() fails, messenger_do_work() shall fail and return]
		result = __FAILURE__;
		LogError("Failed creating the message receiver (link_create failed)");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_080: [`instance->receiver_link` settle mode shall be set to "receiver_settle_mode_first" using link_set_rcv_settle_mode(), ]
	else if (link_set_rcv_settle_mode(instance->receiver_link, receiver_settle_mode_first) != RESULT_OK)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_081: [If link_set_rcv_settle_mode() fails, messenger_do_work() shall fail and return]
		result = __FAILURE__;
		LogError("Failed creating the message receiver (link_set_rcv_settle_mode failed)");
	}
	else
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_082: [`instance->receiver_link` maximum message size shall be set to 65536 using link_set_max_message_size()]
		if (link_set_max_message_size(instance->receiver_link, MESSAGE_RECEIVER_MAX_LINK_SIZE) != RESULT_OK)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_083: [If link_set_max_message_size() fails, it shall be logged and ignored.]
			LogError("Failed setting message receiver link max message size.");
		}

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_084: [`instance->receiver_link` should have a property "com.microsoft:client-version" set as `CLIENT_DEVICE_TYPE_PREFIX/IOTHUB_SDK_VERSION`, using amqpvalue_set_map_value() and link_set_attach_properties()]
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_085: [If amqpvalue_set_map_value() or link_set_attach_properties() fail, the failure shall be ignored]
		attach_device_client_type_to_link(instance->receiver_link, instance->product_info);

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_086: [`instance->message_receiver` shall be created using messagereceiver_create(), passing the `instance->receiver_link` and `on_messagereceiver_state_changed_callback`]
		if ((instance->message_receiver = messagereceiver_create(instance->receiver_link, on_message_receiver_state_changed_callback, (void*)instance)) == NULL)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_087: [If messagereceiver_create() fails, messenger_do_work() shall fail and return]
			result = __FAILURE__;
			link_destroy(instance->receiver_link);
			LogError("Failed creating the message receiver (messagereceiver_create failed)");
		}
		else
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_088: [`instance->message_receiver` shall be opened using messagereceiver_open(), passing `on_message_received_internal_callback`]
			if (messagereceiver_open(instance->message_receiver, on_message_received_internal_callback, (void*)instance) != RESULT_OK)
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_089: [If messagereceiver_open() fails, messenger_do_work() shall fail and return]
				result = __FAILURE__;
				messagereceiver_destroy(instance->message_receiver);
				link_destroy(instance->receiver_link);
				LogError("Failed opening the AMQP message receiver.");
			}
			else
			{
				result = RESULT_OK;
			}
		}
	}

	if (devices_path != NULL)
		STRING_delete(devices_path);
	if (message_receive_address != NULL)
		STRING_delete(message_receive_address);
	if (link_name != NULL)
		STRING_delete(link_name);
	if (target_name != NULL)
		STRING_delete(target_name);
	if (source != NULL)
		amqpvalue_destroy(source);
	if (target != NULL)
		amqpvalue_destroy(target);

	return result;
}

static int move_event_to_in_progress_list(MESSENGER_SEND_EVENT_TASK* task)
{
	int result; 

	if (singlylinkedlist_add(task->messenger->in_progress_list, (void*)task) == NULL)
	{
		result = __FAILURE__;
		LogError("Failed moving event to in_progress list (singlylinkedlist_add failed)");
	}
	else
	{
		result = RESULT_OK;
	}

	return result;
}

static bool find_MESSENGER_SEND_EVENT_TASK_on_list(LIST_ITEM_HANDLE list_item, const void* match_context)
{
	return (list_item != NULL && singlylinkedlist_item_get_value(list_item) == match_context);
}

static void remove_event_from_in_progress_list(MESSENGER_SEND_EVENT_TASK *task)
{
	LIST_ITEM_HANDLE list_item = singlylinkedlist_find(task->messenger->in_progress_list, find_MESSENGER_SEND_EVENT_TASK_on_list, (void*)task);

	if (list_item != NULL)
	{
		if (singlylinkedlist_remove(task->messenger->in_progress_list, list_item) != RESULT_OK)
		{
			LogError("Failed removing event from in_progress list (singlylinkedlist_remove failed)");
		}
	}
}

static int copy_events_to_list(SINGLYLINKEDLIST_HANDLE from_list, SINGLYLINKEDLIST_HANDLE to_list)
{
	int result;
	LIST_ITEM_HANDLE list_item;

	result = RESULT_OK;
	list_item = singlylinkedlist_get_head_item(from_list);

	while (list_item != NULL)
	{
		MESSENGER_SEND_EVENT_TASK *task = (MESSENGER_SEND_EVENT_TASK*)singlylinkedlist_item_get_value(list_item);

		if (singlylinkedlist_add(to_list, task) == NULL)
		{
			LogError("Failed copying event to destination list (singlylinkedlist_add failed)");
			result = __FAILURE__;
			break;
		}
		else
		{
			list_item = singlylinkedlist_get_next_item(list_item);
		}
	}

	return result;
}

static int singlylinkedlist_clear(SINGLYLINKEDLIST_HANDLE list)
{
	int result;
	LIST_ITEM_HANDLE list_item;

	result = RESULT_OK;

	while ((list_item = singlylinkedlist_get_head_item(list)) != NULL)
	{
		if (singlylinkedlist_remove(list, list_item) != RESULT_OK)
		{
			LogError("Failed removing items from list (%d)", list);
			result = __FAILURE__;
			break;
		}
	}

	return result;
}

static int move_events_to_wait_to_send_list(MESSENGER_INSTANCE* instance)
{
	int result;
	LIST_ITEM_HANDLE list_item;

	if ((list_item = singlylinkedlist_get_head_item(instance->in_progress_list)) == NULL)
	{
		result = RESULT_OK;
	}
	else
	{
		SINGLYLINKEDLIST_HANDLE new_wait_to_send_list;

		if ((new_wait_to_send_list = singlylinkedlist_create()) == NULL)
		{
			LogError("Failed moving events back to wait_to_send list (singlylinkedlist_create failed to create new wait_to_send_list)");
			result = __FAILURE__;
		}
		else
		{
			SINGLYLINKEDLIST_HANDLE new_in_progress_list;
		
			if (copy_events_to_list(instance->in_progress_list, new_wait_to_send_list) != RESULT_OK)
			{
				LogError("Failed moving events back to wait_to_send list (failed adding in_progress_list items to new_wait_to_send_list)");
				singlylinkedlist_destroy(new_wait_to_send_list);
				result = __FAILURE__;
			}
			else if (copy_events_to_list(instance->waiting_to_send, new_wait_to_send_list) != RESULT_OK)
			{
				LogError("Failed moving events back to wait_to_send list (failed adding wait_to_send items to new_wait_to_send_list)");
				singlylinkedlist_destroy(new_wait_to_send_list);
				result = __FAILURE__;
			}
			else if ((new_in_progress_list = singlylinkedlist_create()) == NULL)
			{
				LogError("Failed moving events back to wait_to_send list (singlylinkedlist_create failed to create new in_progress_list)");
				singlylinkedlist_destroy(new_wait_to_send_list);
				result = __FAILURE__;
			}
			else 
			{
				singlylinkedlist_destroy(instance->waiting_to_send);
				singlylinkedlist_destroy(instance->in_progress_list);
				instance->waiting_to_send = new_wait_to_send_list;
				instance->in_progress_list = new_in_progress_list;
				result = RESULT_OK;
			}
		}
	}

	return result;
}

static void internal_on_event_send_complete_callback(void* context, MESSAGE_SEND_RESULT send_result)
{ 
	if (context != NULL)
	{
		MESSENGER_SEND_EVENT_TASK* task = (MESSENGER_SEND_EVENT_TASK*)context;

		if (task->messenger->message_sender_current_state != MESSAGE_SENDER_STATE_ERROR)
		{
			if (task->is_timed_out == false)
			{
				MESSENGER_EVENT_SEND_COMPLETE_RESULT messenger_send_result;

				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_107: [If no failure occurs, `task->on_event_send_complete_callback` shall be invoked with result EVENT_SEND_COMPLETE_RESULT_OK]  
				if (send_result == MESSAGE_SEND_OK)
				{
					messenger_send_result = MESSENGER_EVENT_SEND_COMPLETE_RESULT_OK;
				}
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_108: [If a failure occurred, `task->on_event_send_complete_callback` shall be invoked with result EVENT_SEND_COMPLETE_RESULT_ERROR_FAIL_SENDING] 
				else
				{
					messenger_send_result = MESSENGER_EVENT_SEND_COMPLETE_RESULT_ERROR_FAIL_SENDING;
				}

				task->on_event_send_complete_callback(task->message, messenger_send_result, (void*)task->context);
			}
			else
			{
				LogInfo("messenger on_event_send_complete_callback invoked for timed out event %p; not firing upper layer callback.", task->message);
			}

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_128: [`task` shall be removed from `instance->in_progress_list`]  
			remove_event_from_in_progress_list(task);

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_130: [`task` shall be destroyed using free()]  
			free(task);
		}
	}
}

static MESSENGER_SEND_EVENT_TASK* get_next_event_to_send(MESSENGER_INSTANCE* instance)
{
	MESSENGER_SEND_EVENT_TASK* task;
	LIST_ITEM_HANDLE list_item;

	if ((list_item = singlylinkedlist_get_head_item(instance->waiting_to_send)) == NULL)
	{
		task = NULL;
	}
	else
	{
		task = (MESSENGER_SEND_EVENT_TASK*)singlylinkedlist_item_get_value(list_item);

		if (singlylinkedlist_remove(instance->waiting_to_send, list_item) != RESULT_OK)
		{
			LogError("Failed removing item from waiting_to_send list (singlylinkedlist_remove failed)");
		}
	}

	return task;
}

static int send_pending_events(MESSENGER_INSTANCE* instance)
{
	int result = RESULT_OK;

	MESSENGER_SEND_EVENT_TASK* task;

	while ((task = get_next_event_to_send(instance)) != NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_153: [messenger_do_work() shall move each event to be sent from `instance->wait_to_send_list` to `instance->in_progress_list`] 
		if (move_event_to_in_progress_list(task) != RESULT_OK)
		{
			result = __FAILURE__;
			task->on_event_send_complete_callback(task->message, MESSENGER_EVENT_SEND_COMPLETE_RESULT_ERROR_FAIL_SENDING, (void*)task->context);
			break;
		}
		else
		{
			int uamqp_result;
			MESSAGE_HANDLE amqp_message = NULL;

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_154: [A MESSAGE_HANDLE shall be obtained out of the event's IOTHUB_MESSAGE_HANDLE instance by using message_create_from_iothub_message()]  
			if ((uamqp_result = message_create_from_iothub_message(task->message->messageHandle, &amqp_message)) != RESULT_OK)
			{
				LogError("Failed sending event message (failed creating AMQP message; error: %d).", uamqp_result);

				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_155: [If message_create_from_iothub_message() fails, `task->on_event_send_complete_callback` shall be invoked with result EVENT_SEND_COMPLETE_RESULT_ERROR_CANNOT_PARSE]  
				task->on_event_send_complete_callback(task->message, MESSENGER_EVENT_SEND_COMPLETE_RESULT_ERROR_CANNOT_PARSE, (void*)task->context);

				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_160: [If any failure occurs the event shall be removed from `instance->in_progress_list` and destroyed]  
				remove_event_from_in_progress_list(task);
				free(task);
				
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_156: [If message_create_from_iothub_message() fails, messenger_do_work() shall skip to the next event to be sent]  
			}
			else
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_157: [The MESSAGE_HANDLE shall be submitted for sending using messagesender_send(), passing `internal_on_event_send_complete_callback`]  
				uamqp_result = messagesender_send(instance->message_sender, amqp_message, internal_on_event_send_complete_callback, task);
				task->send_time = get_time(NULL);

				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_159: [The MESSAGE_HANDLE shall be destroyed using message_destroy().]
				message_destroy(amqp_message);

				if (uamqp_result != RESULT_OK)
				{
					LogError("Failed sending event (messagesender_send failed; error: %d)", uamqp_result);

					result = __FAILURE__;

					// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_158: [If messagesender_send() fails, `task->on_event_send_complete_callback` shall be invoked with result EVENT_SEND_COMPLETE_RESULT_ERROR_FAIL_SENDING]
					task->on_event_send_complete_callback(task->message, MESSENGER_EVENT_SEND_COMPLETE_RESULT_ERROR_FAIL_SENDING, (void*)task->context);

					// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_160: [If any failure occurs the event shall be removed from `instance->in_progress_list` and destroyed]  
					remove_event_from_in_progress_list(task);
					free(task);

					break;
				}
			}
		}
	}

	return result;
}

// @brief
//     Goes through each task in in_progress_list and checks if the events timed out to be sent.
// @remarks
//     If an event is timed out, it is marked as such but not removed, and the upper layer callback is invoked.
// @returns
//     0 if no failures occur, non-zero otherwise.
static int process_event_send_timeouts(MESSENGER_INSTANCE* instance)
{
	int result = RESULT_OK;

	if (instance->event_send_timeout_secs > 0)
	{
		LIST_ITEM_HANDLE list_item = singlylinkedlist_get_head_item(instance->in_progress_list);

		while (list_item != NULL)
		{
			MESSENGER_SEND_EVENT_TASK* task = (MESSENGER_SEND_EVENT_TASK*)singlylinkedlist_item_get_value(list_item);

			if (task->is_timed_out == false)
			{
				int is_timed_out;

				if (is_timeout_reached(task->send_time, instance->event_send_timeout_secs, &is_timed_out) == RESULT_OK)
				{
					if (is_timed_out)
					{
						task->is_timed_out = true;

						if (task->on_event_send_complete_callback != NULL)
						{
							task->on_event_send_complete_callback(task->message, MESSENGER_EVENT_SEND_COMPLETE_RESULT_ERROR_TIMEOUT, task->context);
						}
					}
				}
				else
				{
					LogError("messenger failed to evaluate event send timeout of event %d", task->message);
					result = __FAILURE__;
				}
			}

			list_item = singlylinkedlist_get_next_item(list_item);
		}
	}

	return result;
}

// @brief
//     Removes all the timed out events from the in_progress_list, without invoking callbacks or detroying the messages.
static void remove_timed_out_events(MESSENGER_INSTANCE* instance)
{
	LIST_ITEM_HANDLE list_item = singlylinkedlist_get_head_item(instance->in_progress_list);

	while (list_item != NULL)
	{
		MESSENGER_SEND_EVENT_TASK* task = (MESSENGER_SEND_EVENT_TASK*)singlylinkedlist_item_get_value(list_item);

		if (task->is_timed_out == true)
		{
			remove_event_from_in_progress_list(task);

			free(task);
		}

		list_item = singlylinkedlist_get_next_item(list_item);
	}
}


// ---------- Set/Retrieve Options Helpers ----------//

static void* messenger_clone_option(const char* name, const void* value)
{
	void* result;

	if (name == NULL)
	{
		LogError("Failed to clone messenger option (name is NULL)");
		result = NULL;
	}
	else if (value == NULL)
	{
		LogError("Failed to clone messenger option (value is NULL)");
		result = NULL;
	}
	else
	{
		if (strcmp(MESSENGER_OPTION_EVENT_SEND_TIMEOUT_SECS, name) == 0 ||
			strcmp(MESSENGER_OPTION_SAVED_OPTIONS, name) == 0)
		{
			result = (void*)value;
		}
		else
		{
			LogError("Failed to clone messenger option (option with name '%s' is not suppported)", name);
			result = NULL;
		}
	}

	return result;
}

static void messenger_destroy_option(const char* name, const void* value)
{
	if (name == NULL)
	{
		LogError("Failed to destroy messenger option (name is NULL)");
	}
	else if (value == NULL)
	{
		LogError("Failed to destroy messenger option (value is NULL)");
	}
	else
	{
		// Nothing to be done for the supported options.
	}
}


// Public API:

int messenger_subscribe_for_messages(MESSENGER_HANDLE messenger_handle, ON_MESSENGER_MESSAGE_RECEIVED on_message_received_callback, void* context)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_016: [If `messenger_handle` is NULL, messenger_subscribe_for_messages() shall fail and return __FAILURE__]
	if (messenger_handle == NULL)
	{
		result = __FAILURE__;
		LogError("messenger_subscribe_for_messages failed (messenger_handle is NULL)");
	}
	else
	{
		MESSENGER_INSTANCE* instance = (MESSENGER_INSTANCE*)messenger_handle;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_017: [If `instance->receive_messages` is already true, messenger_subscribe_for_messages() shall fail and return __FAILURE__]
		if (instance->receive_messages)
		{
			result = __FAILURE__;
			LogError("messenger_subscribe_for_messages failed (messenger already subscribed)");
		}
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_018: [If `on_message_received_callback` is NULL, messenger_subscribe_for_messages() shall fail and return __FAILURE__]
		else if (on_message_received_callback == NULL)
		{
			result = __FAILURE__;
			LogError("messenger_subscribe_for_messages failed (on_message_received_callback is NULL)");
		}
		else
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_019: [`on_message_received_callback` shall be saved on `instance->on_message_received_callback`]
			instance->on_message_received_callback = on_message_received_callback;

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_020: [`context` shall be saved on `instance->on_message_received_context`]
			instance->on_message_received_context = context;

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_021: [messenger_subscribe_for_messages() shall set `instance->receive_messages` to true]
			instance->receive_messages = true;

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_022: [If no failures occurr, messenger_subscribe_for_messages() shall return 0]
			result = RESULT_OK;
		}
	}

	return result;
}

int messenger_unsubscribe_for_messages(MESSENGER_HANDLE messenger_handle)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_023: [If `messenger_handle` is NULL, messenger_unsubscribe_for_messages() shall fail and return __FAILURE__]
	if (messenger_handle == NULL)
	{
		result = __FAILURE__;
		LogError("messenger_unsubscribe_for_messages failed (messenger_handle is NULL)");
	}
	else
	{
		MESSENGER_INSTANCE* instance = (MESSENGER_INSTANCE*)messenger_handle;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_024: [If `instance->receive_messages` is already false, messenger_unsubscribe_for_messages() shall fail and return __FAILURE__]
		if (instance->receive_messages == false)
		{
			result = __FAILURE__;
			LogError("messenger_unsubscribe_for_messages failed (messenger is not subscribed)");
		}
		else
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_025: [messenger_unsubscribe_for_messages() shall set `instance->receive_messages` to false]
			instance->receive_messages = false;
			
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_026: [messenger_unsubscribe_for_messages() shall set `instance->on_message_received_callback` to NULL]
			instance->on_message_received_callback = NULL;
			
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_027: [messenger_unsubscribe_for_messages() shall set `instance->on_message_received_context` to NULL]
			instance->on_message_received_context = NULL;
			
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_028: [If no failures occurr, messenger_unsubscribe_for_messages() shall return 0]
			result = RESULT_OK;
		}
	}

	return result;
}

int messenger_send_message_disposition(MESSENGER_HANDLE messenger_handle, MESSENGER_MESSAGE_DISPOSITION_INFO* disposition_info, MESSENGER_DISPOSITION_RESULT disposition_result)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_179: [If `messenger_handle` or `disposition_info` are NULL, messenger_send_message_disposition() shall fail and return __FAILURE__]  
	if (messenger_handle == NULL || disposition_info == NULL)
	{
		LogError("Failed sending message disposition (either messenger_handle (%p) or disposition_info (%p) are NULL)", messenger_handle, disposition_info);
		result = __FAILURE__;
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_180: [If `disposition_info->source` is NULL, messenger_send_message_disposition() shall fail and return __FAILURE__]  
	else if (disposition_info->source == NULL)
	{
		LogError("Failed sending message disposition (disposition_info->source is NULL)");
		result = __FAILURE__;
	}
	else
	{
		MESSENGER_INSTANCE* messenger = (MESSENGER_INSTANCE*)messenger_handle;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_189: [If `messenger_handle->message_receiver` is NULL, messenger_send_message_disposition() shall fail and return __FAILURE__]
		if (messenger->message_receiver == NULL)
		{
			LogError("Failed sending message disposition (message_receiver is not created; check if it is subscribed)");
			result = __FAILURE__;
		}
		else
		{
			AMQP_VALUE uamqp_disposition_result;

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_181: [An AMQP_VALUE disposition result shall be created corresponding to the `disposition_result` provided]
			if ((uamqp_disposition_result = create_uamqp_disposition_result_from(disposition_result)) == NULL)
			{
				LogError("Failed sending message disposition (disposition result %d is not supported)", disposition_result);
				result = __FAILURE__;
			}
			else
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_182: [`messagereceiver_send_message_disposition()` shall be invoked passing `disposition_info->source`, `disposition_info->message_id` and the corresponding AMQP_VALUE disposition result]  
				if (messagereceiver_send_message_disposition(messenger->message_receiver, disposition_info->source, disposition_info->message_id, uamqp_disposition_result) != RESULT_OK)
				{
					// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_183: [If `messagereceiver_send_message_disposition()` fails, messenger_send_message_disposition() shall fail and return __FAILURE__]  
					LogError("Failed sending message disposition (messagereceiver_send_message_disposition failed)");
					result = __FAILURE__;
				}
				else
				{
					// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_185: [If no failures occurr, messenger_send_message_disposition() shall return 0]  
					result = RESULT_OK;
				}

				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_184: [messenger_send_message_disposition() shall destroy the AMQP_VALUE disposition result]
				amqpvalue_destroy(uamqp_disposition_result);
			}
		}
	}

	return result;
}

int messenger_send_async(MESSENGER_HANDLE messenger_handle, IOTHUB_MESSAGE_LIST* message, ON_MESSENGER_EVENT_SEND_COMPLETE on_messenger_event_send_complete_callback, void* context)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_134: [If `messenger_handle` is NULL, messenger_send_async() shall fail and return a non-zero value]  
	if (messenger_handle == NULL)
	{
		LogError("Failed sending event (messenger_handle is NULL)");
		result = __FAILURE__;
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_135: [If `message` is NULL, messenger_send_async() shall fail and return a non-zero value]  
	else if (message == NULL)
	{
		LogError("Failed sending event (message is NULL)");
		result = __FAILURE__;
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_136: [If `on_event_send_complete_callback` is NULL, messenger_send_async() shall fail and return a non-zero value] 
	else if (on_messenger_event_send_complete_callback == NULL)
	{
		LogError("Failed sending event (on_event_send_complete_callback is NULL)");
		result = __FAILURE__;
	}
	else
	{
		MESSENGER_SEND_EVENT_TASK *task;
		MESSENGER_INSTANCE *instance = (MESSENGER_INSTANCE*)messenger_handle;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_137: [messenger_send_async() shall allocate memory for a MESSENGER_SEND_EVENT_TASK structure (aka `task`)]  
		if ((task = (MESSENGER_SEND_EVENT_TASK*)malloc(sizeof(MESSENGER_SEND_EVENT_TASK))) == NULL)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_138: [If malloc() fails, messenger_send_async() shall fail and return a non-zero value]
			LogError("Failed sending event (failed to create struct for task; malloc failed)");
			result = __FAILURE__;
		}
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_100: [`task` shall be added to `instance->waiting_to_send` using singlylinkedlist_add()]  
		else if (singlylinkedlist_add(instance->waiting_to_send, task) == NULL)
		{
			LogError("Failed sending event (singlylinkedlist_add failed)");

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_139: [If singlylinkedlist_add() fails, messenger_send_async() shall fail and return a non-zero value]
			result = __FAILURE__;

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_142: [If any failure occurs, messenger_send_async() shall free any memory it has allocated]
			free(task);
		}
		else
		{
			memset(task, 0, sizeof(MESSENGER_SEND_EVENT_TASK));
			task->message = message;
			task->on_event_send_complete_callback = on_messenger_event_send_complete_callback;
			task->context = context;
			task->send_time = INDEFINITE_TIME;
			task->messenger = instance;
			task->is_timed_out = false;
			
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_143: [If no failures occur, messenger_send_async() shall return zero]  
			result = RESULT_OK;
		}
	}

	return result;
}

int messenger_get_send_status(MESSENGER_HANDLE messenger_handle, MESSENGER_SEND_STATUS* send_status)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_144: [If `messenger_handle` is NULL, messenger_get_send_status() shall fail and return a non-zero value] 
	if (messenger_handle == NULL)
	{
		LogError("messenger_get_send_status failed (messenger_handle is NULL)");
		result = __FAILURE__;
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_145: [If `send_status` is NULL, messenger_get_send_status() shall fail and return a non-zero value] 
	else if (send_status == NULL)
	{
		LogError("messenger_get_send_status failed (send_status is NULL)");
		result = __FAILURE__;
	}
	else
	{
		MESSENGER_INSTANCE* instance = (MESSENGER_INSTANCE*)messenger_handle;
		LIST_ITEM_HANDLE wts_list_head = singlylinkedlist_get_head_item(instance->waiting_to_send);
		LIST_ITEM_HANDLE ip_list_head = singlylinkedlist_get_head_item(instance->in_progress_list);

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_147: [If `instance->in_progress_list` and `instance->wait_to_send_list` are empty, send_status shall be set to MESSENGER_SEND_STATUS_IDLE] 
		if (wts_list_head == NULL && ip_list_head == NULL)
		{
			*send_status = MESSENGER_SEND_STATUS_IDLE;
		}
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_148: [Otherwise, send_status shall be set to MESSENGER_SEND_STATUS_BUSY] 
		else
		{
			*send_status = MESSENGER_SEND_STATUS_BUSY;
		}

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_149: [If no failures occur, messenger_get_send_status() shall return 0]
		result = RESULT_OK;
	}

	return result;
}

int messenger_start(MESSENGER_HANDLE messenger_handle, SESSION_HANDLE session_handle)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_029: [If `messenger_handle` is NULL, messenger_start() shall fail and return __FAILURE__]
	if (messenger_handle == NULL)
	{
		result = __FAILURE__;
		LogError("messenger_start failed (messenger_handle is NULL)");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_030: [If `session_handle` is NULL, messenger_start() shall fail and return __FAILURE__]
	else if (session_handle == NULL)
	{
		result = __FAILURE__;
		LogError("messenger_start failed (session_handle is NULL)");
	}
	else
	{
		MESSENGER_INSTANCE* instance = (MESSENGER_INSTANCE*)messenger_handle;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_031: [If `instance->state` is not MESSENGER_STATE_STOPPED, messenger_start() shall fail and return __FAILURE__]
		if (instance->state != MESSENGER_STATE_STOPPED)
		{
			result = __FAILURE__;
			LogError("messenger_start failed (current state is %d; expected MESSENGER_STATE_STOPPED)", instance->state);
		}
		else
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_032: [`session_handle` shall be saved on `instance->session_handle`]
			instance->session_handle = session_handle;

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_115: [If no failures occurr, `instance->state` shall be set to MESSENGER_STATE_STARTING, and `instance->on_state_changed_callback` invoked if provided]
			update_messenger_state(instance, MESSENGER_STATE_STARTING);

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_056: [If no failures occurr, messenger_start() shall return 0]
			result = RESULT_OK;
		}
	}

	return result;
}

int messenger_stop(MESSENGER_HANDLE messenger_handle)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_057: [If `messenger_handle` is NULL, messenger_stop() shall fail and return a non-zero value]
	if (messenger_handle == NULL)
	{
		result = __FAILURE__;
		LogError("messenger_stop failed (messenger_handle is NULL)");
	}
	else
	{
		MESSENGER_INSTANCE* instance = (MESSENGER_INSTANCE*)messenger_handle;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_058: [If `instance->state` is MESSENGER_STATE_STOPPED, messenger_stop() shall fail and return a non-zero value]
		if (instance->state == MESSENGER_STATE_STOPPED)
		{
			result = __FAILURE__;
			LogError("messenger_stop failed (messenger is already stopped)");
		}
		else
		{
			update_messenger_state(instance, MESSENGER_STATE_STOPPING);

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_152: [messenger_stop() shall close and destroy `instance->message_sender` and `instance->message_receiver`]  
			destroy_event_sender(instance);
			destroy_message_receiver(instance);

			remove_timed_out_events(instance);

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_162: [messenger_stop() shall move all items from `instance->in_progress_list` to the beginning of `instance->wait_to_send_list`]
			if (move_events_to_wait_to_send_list(instance) != RESULT_OK)
			{
				LogError("Messenger failed to move events in progress back to wait_to_send list");
				
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_163: [If not all items from `instance->in_progress_list` can be moved back to `instance->wait_to_send_list`, `instance->state` shall be set to MESSENGER_STATE_ERROR, and `instance->on_state_changed_callback` invoked]
				update_messenger_state(instance, MESSENGER_STATE_ERROR);
				result = __FAILURE__;
			}
			else
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_164: [If all items get successfuly moved back to `instance->wait_to_send_list`, `instance->state` shall be set to MESSENGER_STATE_STOPPED, and `instance->on_state_changed_callback` invoked]
				update_messenger_state(instance, MESSENGER_STATE_STOPPED);
				result = RESULT_OK;
			}
		}
	}

	return result;
}

// @brief
//     Sets the messenger module state based on the state changes from messagesender and messagereceiver
static void process_state_changes(MESSENGER_INSTANCE* instance)
{
	// Note: messagesender and messagereceiver are still not created or already destroyed 
	//       when state is MESSENGER_STATE_STOPPED, so no checking is needed there.

	if (instance->state == MESSENGER_STATE_STARTED)
	{
		if (instance->message_sender_current_state != MESSAGE_SENDER_STATE_OPEN)
		{
			LogError("messagesender reported unexpected state %d while messenger was started", instance->message_sender_current_state);
			update_messenger_state(instance, MESSENGER_STATE_ERROR);
		}
		else if (instance->message_receiver != NULL && instance->message_receiver_current_state != MESSAGE_RECEIVER_STATE_OPEN)
		{
			if (instance->message_receiver_current_state == MESSAGE_RECEIVER_STATE_OPENING)
			{
				int is_timed_out;
				if (is_timeout_reached(instance->last_message_receiver_state_change_time, MAX_MESSAGE_RECEIVER_STATE_CHANGE_TIMEOUT_SECS, &is_timed_out) != RESULT_OK)
				{
					LogError("messenger got an error (failed to verify messagereceiver start timeout)");
					update_messenger_state(instance, MESSENGER_STATE_ERROR);
				}
				else if (is_timed_out == 1)
				{
					LogError("messenger got an error (messagereceiver failed to start within expected timeout (%d secs))", MAX_MESSAGE_RECEIVER_STATE_CHANGE_TIMEOUT_SECS);
					update_messenger_state(instance, MESSENGER_STATE_ERROR);
				}
			}
			else if (instance->message_receiver_current_state == MESSAGE_RECEIVER_STATE_ERROR ||
				instance->message_receiver_current_state == MESSAGE_RECEIVER_STATE_IDLE)
			{
				LogError("messagereceiver reported unexpected state %d while messenger is starting", instance->message_receiver_current_state);
				update_messenger_state(instance, MESSENGER_STATE_ERROR);
			}
		}
	}
	else
	{
		if (instance->state == MESSENGER_STATE_STARTING)
		{
			if (instance->message_sender_current_state == MESSAGE_SENDER_STATE_OPEN)
			{
				update_messenger_state(instance, MESSENGER_STATE_STARTED);
			}
			else if (instance->message_sender_current_state == MESSAGE_SENDER_STATE_OPENING)
			{
				int is_timed_out;
				if (is_timeout_reached(instance->last_message_sender_state_change_time, MAX_MESSAGE_SENDER_STATE_CHANGE_TIMEOUT_SECS, &is_timed_out) != RESULT_OK)
				{
					LogError("messenger failed to start (failed to verify messagesender start timeout)");
					update_messenger_state(instance, MESSENGER_STATE_ERROR);
				}
				else if (is_timed_out == 1)
				{
					LogError("messenger failed to start (messagesender failed to start within expected timeout (%d secs))", MAX_MESSAGE_SENDER_STATE_CHANGE_TIMEOUT_SECS);
					update_messenger_state(instance, MESSENGER_STATE_ERROR);
				}
			}
			// For this module, the only valid scenario where messagesender state is IDLE is if 
			// the messagesender hasn't been created yet or already destroyed.
			else if ((instance->message_sender_current_state == MESSAGE_SENDER_STATE_ERROR) ||
				(instance->message_sender_current_state == MESSAGE_SENDER_STATE_CLOSING) ||
				(instance->message_sender_current_state == MESSAGE_SENDER_STATE_IDLE && instance->message_sender != NULL))
			{
				LogError("messagesender reported unexpected state %d while messenger is starting", instance->message_sender_current_state);
				update_messenger_state(instance, MESSENGER_STATE_ERROR);
			}
		}
		// message sender and receiver are stopped/destroyed synchronously, so no need for state control.
	}
}

void messenger_do_work(MESSENGER_HANDLE messenger_handle)
{
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_065: [If `messenger_handle` is NULL, messenger_do_work() shall fail and return]
	if (messenger_handle == NULL)
	{
		LogError("messenger_do_work failed (messenger_handle is NULL)");
	}
	else
	{
		MESSENGER_INSTANCE* instance = (MESSENGER_INSTANCE*)messenger_handle;

		process_state_changes(instance);

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_151: [If `instance->state` is MESSENGER_STATE_STARTING, messenger_do_work() shall create and open `instance->message_sender`]
		if (instance->state == MESSENGER_STATE_STARTING)
		{
			if (instance->message_sender == NULL)
			{
				if (create_event_sender(instance) != RESULT_OK)
				{
					update_messenger_state(instance, MESSENGER_STATE_ERROR);
				}
			}
		}
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_066: [If `instance->state` is not MESSENGER_STATE_STARTED, messenger_do_work() shall return]
		else if (instance->state == MESSENGER_STATE_STARTED)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_067: [If `instance->receive_messages` is true and `instance->message_receiver` is NULL, a message_receiver shall be created]
			if (instance->receive_messages == true &&
				instance->message_receiver == NULL &&
				create_message_receiver(instance) != RESULT_OK)
			{
				LogError("messenger_do_work warning (failed creating the message receiver [%s])", STRING_c_str(instance->device_id));
			}
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_092: [If `instance->receive_messages` is false and `instance->message_receiver` is not NULL, it shall be destroyed]
			else if (instance->receive_messages == false && instance->message_receiver != NULL)
			{
				destroy_message_receiver(instance);
			}

			if (process_event_send_timeouts(instance) != RESULT_OK)
			{
				update_messenger_state(instance, MESSENGER_STATE_ERROR);
			}
			else if (send_pending_events(instance) != RESULT_OK && instance->event_send_retry_limit > 0)
			{
				instance->event_send_error_count++;

				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_161: [If messenger_do_work() fail sending events for `instance->event_send_retry_limit` times in a row, it shall invoke `instance->on_state_changed_callback`, if provided, with error code MESSENGER_STATE_ERROR]
				if (instance->event_send_error_count >= instance->event_send_retry_limit)
				{
					LogError("messenger_do_work failed (failed sending events; reached max number of consecutive attempts)");
					update_messenger_state(instance, MESSENGER_STATE_ERROR);
				}
			}
			else
			{
				instance->event_send_error_count = 0;
			}
		}
	}
}

void messenger_destroy(MESSENGER_HANDLE messenger_handle)
{
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_109: [If `messenger_handle` is NULL, messenger_destroy() shall fail and return]
	if (messenger_handle == NULL)
	{
		LogError("messenger_destroy failed (messenger_handle is NULL)");
	}
	else
	{
		LIST_ITEM_HANDLE list_node;
		MESSENGER_INSTANCE* instance = (MESSENGER_INSTANCE*)messenger_handle;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_110: [If the `instance->state` is not MESSENGER_STATE_STOPPED, messenger_destroy() shall invoke messenger_stop()]
		if (instance->state != MESSENGER_STATE_STOPPED)
		{
			(void)messenger_stop(messenger_handle);
		}

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_111: [All elements of `instance->in_progress_list` and `instance->wait_to_send_list` shall be removed, invoking `task->on_event_send_complete_callback` for each with EVENT_SEND_COMPLETE_RESULT_MESSENGER_DESTROYED]

		// Note: yes messenger_stop() tried to move all events from in_progress_list to wait_to_send_list, 
		//       but we need to iterate through in case any events failed to be moved.
		while ((list_node = singlylinkedlist_get_head_item(instance->in_progress_list)) != NULL)
		{
			MESSENGER_SEND_EVENT_TASK* task = (MESSENGER_SEND_EVENT_TASK*)singlylinkedlist_item_get_value(list_node);

			(void)singlylinkedlist_remove(instance->in_progress_list, list_node);

			if (task != NULL)
			{
				task->on_event_send_complete_callback(task->message, MESSENGER_EVENT_SEND_COMPLETE_RESULT_MESSENGER_DESTROYED, (void*)task->context);
				free(task);
			}
		}

		while ((list_node = singlylinkedlist_get_head_item(instance->waiting_to_send)) != NULL)
		{
			MESSENGER_SEND_EVENT_TASK* task = (MESSENGER_SEND_EVENT_TASK*)singlylinkedlist_item_get_value(list_node);

			(void)singlylinkedlist_remove(instance->waiting_to_send, list_node);

			if (task != NULL)
			{
				task->on_event_send_complete_callback(task->message, MESSENGER_EVENT_SEND_COMPLETE_RESULT_MESSENGER_DESTROYED, (void*)task->context);
				free(task);
			}
		}

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_150: [`instance->in_progress_list` and `instance->wait_to_send_list` shall be destroyed using singlylinkedlist_destroy()]
		singlylinkedlist_destroy(instance->waiting_to_send);
		singlylinkedlist_destroy(instance->in_progress_list);

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_112: [`instance->iothub_host_fqdn` shall be destroyed using STRING_delete()]
		STRING_delete(instance->iothub_host_fqdn);
		
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_113: [`instance->device_id` shall be destroyed using STRING_delete()]
		STRING_delete(instance->device_id);

        STRING_delete(instance->product_info);

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_114: [messenger_destroy() shall destroy `instance` with free()]
		(void)free(instance);
	}
}

MESSENGER_HANDLE messenger_create(const MESSENGER_CONFIG* messenger_config, const char* product_info)
{
	MESSENGER_HANDLE handle;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_001: [If parameter `messenger_config` is NULL, messenger_create() shall return NULL]
	if (messenger_config == NULL)
	{
		handle = NULL;
		LogError("messenger_create failed (messenger_config is NULL)");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_002: [If `messenger_config->device_id` is NULL, messenger_create() shall return NULL]
	else if (messenger_config->device_id == NULL)
	{
		handle = NULL;
		LogError("messenger_create failed (messenger_config->device_id is NULL)");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_003: [If `messenger_config->iothub_host_fqdn` is NULL, messenger_create() shall return NULL]
	else if (messenger_config->iothub_host_fqdn == NULL)
	{
		handle = NULL;
		LogError("messenger_create failed (messenger_config->iothub_host_fqdn is NULL)");
	}
	else
	{
		MESSENGER_INSTANCE* instance;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_006: [messenger_create() shall allocate memory for the messenger instance structure (aka `instance`)]
		if ((instance = (MESSENGER_INSTANCE*)malloc(sizeof(MESSENGER_INSTANCE))) == NULL)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_007: [If malloc() fails, messenger_create() shall fail and return NULL]
			handle = NULL;
			LogError("messenger_create failed (messenger_config->wait_to_send_list is NULL)");
		}
		else
		{
			memset(instance, 0, sizeof(MESSENGER_INSTANCE));
			instance->state = MESSENGER_STATE_STOPPED;
			instance->message_sender_current_state = MESSAGE_SENDER_STATE_IDLE;
			instance->message_sender_previous_state = MESSAGE_SENDER_STATE_IDLE;
			instance->message_receiver_current_state = MESSAGE_RECEIVER_STATE_IDLE;
			instance->message_receiver_previous_state = MESSAGE_RECEIVER_STATE_IDLE;
			instance->event_send_retry_limit = DEFAULT_EVENT_SEND_RETRY_LIMIT;
			instance->event_send_timeout_secs = DEFAULT_EVENT_SEND_TIMEOUT_SECS;
			instance->last_message_sender_state_change_time = INDEFINITE_TIME;
			instance->last_message_receiver_state_change_time = INDEFINITE_TIME;

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_008: [messenger_create() shall save a copy of `messenger_config->device_id` into `instance->device_id`]
			if ((instance->device_id = STRING_construct(messenger_config->device_id)) == NULL)
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_009: [If STRING_construct() fails, messenger_create() shall fail and return NULL]
				handle = NULL;
				LogError("messenger_create failed (device_id could not be copied; STRING_construct failed)");
			}
            else if ((instance->product_info = STRING_construct(product_info)) == NULL)
            {
                handle = NULL;
                LogError("messenger_create failed (product_info could not be copied; STRING_construct failed)");
            }
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_010: [messenger_create() shall save a copy of `messenger_config->iothub_host_fqdn` into `instance->iothub_host_fqdn`]
			else if ((instance->iothub_host_fqdn = STRING_construct(messenger_config->iothub_host_fqdn)) == NULL)
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_011: [If STRING_construct() fails, messenger_create() shall fail and return NULL]
				handle = NULL;
				LogError("messenger_create failed (iothub_host_fqdn could not be copied; STRING_construct failed)");
			}
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_165: [`instance->wait_to_send_list` shall be set using singlylinkedlist_create()]
			else if ((instance->waiting_to_send = singlylinkedlist_create()) == NULL)
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_166: [If singlylinkedlist_create() fails, messenger_create() shall fail and return NULL]  
				handle = NULL;
				LogError("messenger_create failed (singlylinkedlist_create failed to create wait_to_send_list)");
			}
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_132: [`instance->in_progress_list` shall be set using singlylinkedlist_create()]  
			else if ((instance->in_progress_list = singlylinkedlist_create()) == NULL)
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_133: [If singlylinkedlist_create() fails, messenger_create() shall fail and return NULL] 
				handle = NULL;
				LogError("messenger_create failed (singlylinkedlist_create failed to create in_progress_list)");
			}
			else
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_013: [`messenger_config->on_state_changed_callback` shall be saved into `instance->on_state_changed_callback`]
				instance->on_state_changed_callback = messenger_config->on_state_changed_callback;

				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_014: [`messenger_config->on_state_changed_context` shall be saved into `instance->on_state_changed_context`]
				instance->on_state_changed_context = messenger_config->on_state_changed_context;

				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_015: [If no failures occurr, messenger_create() shall return a handle to `instance`]
				handle = (MESSENGER_HANDLE)instance;
			}
		}

		if (handle == NULL)
		{
			messenger_destroy((MESSENGER_HANDLE)instance);
		}
	}

	return handle;
}

int messenger_set_option(MESSENGER_HANDLE messenger_handle, const char* name, void* value)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_167: [If `messenger_handle` or `name` or `value` is NULL, messenger_set_option shall fail and return a non-zero value]
	if (messenger_handle == NULL || name == NULL || value == NULL)
	{
		LogError("messenger_set_option failed (one of the followin are NULL: messenger_handle=%p, name=%p, value=%p)",
			messenger_handle, name, value);
		result = __FAILURE__;
	}
	else
	{
		MESSENGER_INSTANCE* instance = (MESSENGER_INSTANCE*)messenger_handle;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_168: [If name matches MESSENGER_OPTION_EVENT_SEND_TIMEOUT_SECS, `value` shall be saved on `instance->event_send_timeout_secs`]
		if (strcmp(MESSENGER_OPTION_EVENT_SEND_TIMEOUT_SECS, name) == 0)
		{
			instance->event_send_timeout_secs = *((size_t*)value);
			result = RESULT_OK;
		}
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_169: [If name matches MESSENGER_OPTION_SAVED_OPTIONS, `value` shall be applied using OptionHandler_FeedOptions]
		else if (strcmp(MESSENGER_OPTION_SAVED_OPTIONS, name) == 0)
		{
			if (OptionHandler_FeedOptions((OPTIONHANDLER_HANDLE)value, messenger_handle) != OPTIONHANDLER_OK)
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_170: [If OptionHandler_FeedOptions fails, messenger_set_option shall fail and return a non-zero value]
				LogError("messenger_set_option failed (OptionHandler_FeedOptions failed)");
				result = __FAILURE__;
			}
			else
			{
				result = RESULT_OK;
			}
		}
		else
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_171: [If name does not match any supported option, authentication_set_option shall fail and return a non-zero value]
			LogError("messenger_set_option failed (option with name '%s' is not suppported)", name);
			result = __FAILURE__;
		}
	}

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_172: [If no errors occur, messenger_set_option shall return 0]
	return result;
}

OPTIONHANDLER_HANDLE messenger_retrieve_options(MESSENGER_HANDLE messenger_handle)
{
	OPTIONHANDLER_HANDLE result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_173: [If `messenger_handle` is NULL, messenger_retrieve_options shall fail and return NULL]
	if (messenger_handle == NULL)
	{
		LogError("Failed to retrieve options from messenger instance (messenger_handle is NULL)");
		result = NULL;
	}
	else
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_174: [An OPTIONHANDLER_HANDLE instance shall be created using OptionHandler_Create]
		OPTIONHANDLER_HANDLE options = OptionHandler_Create(messenger_clone_option, messenger_destroy_option, (pfSetOption)messenger_set_option);

		if (options == NULL)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_175: [If an OPTIONHANDLER_HANDLE instance fails to be created, messenger_retrieve_options shall fail and return NULL]
			LogError("Failed to retrieve options from messenger instance (OptionHandler_Create failed)");
			result = NULL;
		}
		else
		{
			MESSENGER_INSTANCE* instance = (MESSENGER_INSTANCE*)messenger_handle;

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_176: [Each option of `instance` shall be added to the OPTIONHANDLER_HANDLE instance using OptionHandler_AddOption]
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_177: [If OptionHandler_AddOption fails, messenger_retrieve_options shall fail and return NULL]
			if (OptionHandler_AddOption(options, MESSENGER_OPTION_EVENT_SEND_TIMEOUT_SECS, (void*)&instance->event_send_timeout_secs) != OPTIONHANDLER_OK)
			{
				LogError("Failed to retrieve options from messenger instance (OptionHandler_Create failed for option '%s')", MESSENGER_OPTION_EVENT_SEND_TIMEOUT_SECS);
				result = NULL;
			}
			else
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_179: [If no failures occur, messenger_retrieve_options shall return the OPTIONHANDLER_HANDLE instance]
				result = options;
			}

			if (result == NULL)
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_178: [If messenger_retrieve_options fails, any allocated memory shall be freed]
				OptionHandler_Destroy(options);
			}
		}
	}

	return result;
}
