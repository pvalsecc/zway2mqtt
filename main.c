#include <alloca.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <ZWayLib.h>
#include <ZLogging.h>

#include <mosquitto.h>

static ZWay zway = NULL;
static struct mosquitto *mqtt;

static void data2mqtt(ZDataHolder data, uint8_t **payload,  uint32_t *payloadLen)
{
    ZWDataType type;
    zdata_get_type(data, &type);

    ZWBOOL bool_val;
    int int_val;
    float float_val;
    ZWCSTR str_val;
    const ZWBYTE *binary;
    size_t len;

    switch (type)
    {
        case Empty:
            *payloadLen = 1;
            *payload = malloc(*payloadLen);
            break;
        case Boolean:
            zdata_get_boolean(data, &bool_val);
            *payloadLen = 2;
            *payload = malloc(*payloadLen);
            (*payload)[1] = bool_val;
            break;
        case Integer:
            zdata_get_integer(data, &int_val);
            *payloadLen = 5;
            *payload = malloc(*payloadLen);
            uint8_t* pl = *payload;
            pl[1] = ((unsigned)int_val) >> 24;
            pl[2] = ((unsigned)int_val) >> 16;
            pl[3] = ((unsigned)int_val) >> 8;
            pl[4] = ((unsigned)int_val);
            break;
        case Float:
            zdata_get_float(data, &float_val);
            *payloadLen = 1 + sizeof(float);
            *payload = malloc(*payloadLen);
            memcpy((*payload)+1, &float_val, sizeof(float));
            break;
        case String:
            zdata_get_string(data, &str_val);
            *payloadLen = 1 + strlen(str_val) + 1;
            *payload = calloc(*payloadLen, 1);
            strcpy(((char*)*payload)+1, str_val);
            break;
        case Binary:
            zdata_get_binary(data, &binary, &len);
            *payloadLen = 1 + len;
            *payload = malloc(*payloadLen);
            memcpy((*payload)+1, binary, len);
            break;
        case ArrayOfInteger:
        case ArrayOfFloat:
        case ArrayOfString:
        default:
            *payloadLen = 1;
            *payload = malloc(*payloadLen);
            //TODO
            break;
    }
    (*payload)[0] = type;
}

static void subscribe_data(const ZWay zway, ZDataHolder data);

static char* path2topic(const char* path) {
    static const char PREFIX[] = "zwave/get/";
    const int len = strlen(path);
    char *ret = malloc(sizeof(PREFIX) + len);
    char *tmp = ret;
    strcpy(ret, PREFIX);
    strcat(ret, path);
    while(*tmp) {
        if(*tmp == '.') *tmp = '/';
        ++tmp;
    }
    return ret;
}

static void notify_update(ZDataHolder data) {
    char *path = zdata_get_path(data);
    char *topic = path2topic(path);
    free(path);

    uint8_t *payload;
    uint32_t payloadLen;
    data2mqtt(data, &payload, &payloadLen);

    int r = mosquitto_publish(mqtt, NULL, topic, payloadLen, payload, 0, TRUE);
    if(r != MOSQ_ERR_SUCCESS) {
        zway_log(zway, Error, ZSTR("Failed to publish %s len=%d to MQTT: %d"),
                 path, payloadLen, r);
    }

    free(payload);
    free(topic);
}

static void data_callback(const ZDataRootObject root, ZWDataChangeType type,
                          ZDataHolder data, void *arg) {
    if(type & ChildCreated) {
        subscribe_data(zway, data);
    }
    if(type & Updated) {
        notify_update(data);
    }
    if(type & Invalidated) {}
    //TODO: send empty payload when removed
}

static void subscribe_data(const ZWay zway, ZDataHolder data) {
    ZWError r;
    char *path = zdata_get_path(data);
    printf("Subscribe to %s\n", path);
    r = zdata_add_callback_ex(data, data_callback,
                            FALSE, NULL);
    if (r != NoError) {
        zway_log_error(zway, Critical, "Failed to add data callback", r);
    }
    free(path);

    ZDataIterator child = zdata_first_child(data);
    while (child != NULL)
    {
        subscribe_data(zway, child->data);
        child = zdata_next_child(child);
    }
}

static void subscribe_command(const ZWay zway, ZWBYTE node_id,
                              ZWBYTE instance_id, ZWBYTE command_id) {
    ZDataHolder data = zway_find_device_instance_cc_data(zway, node_id, instance_id,
                                                         command_id, NULL);
    subscribe_data(zway, data);
}

static void device_callback(const ZWay zway, ZWDeviceChangeType type, ZWBYTE node_id,
                            ZWBYTE instance_id, ZWBYTE command_id, void *arg) {
    switch (type) {
        case DeviceAdded:
            printf("New device added: %i\n", node_id);
            break;
        
        case DeviceRemoved:
            printf("Device removed: %i\n", node_id);
            break;
        
        case InstanceAdded:
            printf("New instance added to device %i: %i\n", node_id, instance_id);
            break;
        
        case InstanceRemoved:
            printf("Instance removed from device %i: %i\n", node_id, instance_id);
            break;
        
        case CommandAdded:
            printf("New Command Class added to device %i:%i: %i\n", 
                   node_id, instance_id, command_id);
            subscribe_command(zway, node_id, instance_id, command_id);
            break;
        
        case CommandRemoved:        
            printf("Command Class removed from device %i:%i: %i\n",
                   node_id, instance_id, command_id);
            break;
    }
}

static void do_work() {
    while(TRUE) {
        mosquitto_loop(mqtt, 1000);
    }
}

static void print_zway_terminated(ZWay zway, void* arg) {
    zway_log(zway, Information, ZSTR("Z-Way terminated")); 
}

static bool is_boolean(const struct mosquitto_message *message) {
    return message->payloadlen == 2 && message->payload[0] == Boolean;
}

static bool get_boolean(const struct mosquitto_message *message) {
    assert(is_boolean(message));
    return message->payload[1] ? TRUE : FALSE;
}

static bool is_integer(const struct mosquitto_message *message) {
    return message->payloadlen == 5 && message->payload[0] == Integer;
}

static int get_integer(const struct mosquitto_message *message) {
    assert(is_integer(message));
    const uint8_t* pl = message->payload;
    return pl[1]<<24 | pl[2]<<16 | pl[3]<<8 | pl[4];
}

static void handle_devices(const struct mosquitto_message *message) {
    int device_id, instance_id, cc_id;
    char *rest = alloca(strlen(message->topic));
    if(sscanf(message->topic, "zwave/set/devices/%d/instances/%d/commandClasses/%d/data/%s",
              &device_id, &instance_id, &cc_id, rest) != 4) {
        zway_log(zway, Error, ZSTR("Cannot parse %s"), message->topic);
        return;
    }
    ZWError err = InvalidOperation;
    switch(cc_id) {
        case 37: //switch_binary
            if (is_boolean(message) && strcmp(rest, "level") == 0) {
                err = zway_cc_switch_binary_set(zway, device_id, instance_id,
                                                get_boolean(message),
                                                NULL, NULL, NULL);
            } else {
                zway_log(zway, Error, ZSTR("Invalid type or path for class %d"), cc_id);
                err = InvalidType;
            }
            break;
        case 112: {//configuration
            int parameter;
            char *sub = alloca(strlen(rest));
            if (is_integer(message) &&
                    sscanf(rest, "%d/%s", &parameter, sub) == 1 && strcmp(sub, "val")) {
                err = zway_cc_configuration_set(zway, device_id, instance_id,
                                                parameter, get_integer(message),
                                                0, NULL, NULL, NULL);
            } else {
                zway_log(zway, Error, ZSTR("Invalid type or path for class %d"), cc_id);
                err = InvalidType;
            }
            break;
        }
        default:
            zway_log(zway, Error, ZSTR("Cannot set data for unknown class %d"), cc_id);
            err = InvalidOperation;
    }
    if(err == NoError) {
        zway_log(zway, Information, ZSTR("Set data for %s"), message->topic);
    } else {
        zway_log(zway, Error, ZSTR("Failed to set data for %s: %s"), message->topic, zstrerror(err));
    }
}

static void handle_control(const struct mosquitto_message *message) {
    ZWError err = InvalidOperation;
    if(strcmp(message->topic, "zwave/control/add_node")==0) {
        if(is_boolean(message)) {
            err = zway_fc_add_node_to_network(zway, get_boolean(message), TRUE,
                                              NULL, NULL, NULL);
        } else {
            zway_log(zway, Error, ZSTR("Invalid type for add_node len=%d type=%d"), message->payloadlen, message->payload[0]);
            err = InvalidType;
        }
    } else if(strcmp(message->topic, "zwave/control/remove_node")==0) {
        if(is_boolean(message)) {
            err = zway_fc_remove_node_from_network(zway, get_boolean(message), TRUE,
                                              NULL, NULL, NULL);
        } else {
            zway_log(zway, Error, ZSTR("Invalid type for add_node"));
            err = InvalidType;
        }
    }
    //TODO: call zddx_save_to_xml or check <SaveDataAfterInterviewSteps> is
    //      set to 1 in config/Defaults.xml
    if(err == NoError) {
        zway_log(zway, Information, ZSTR("Control for %s"), message->topic);
    } else {
        zway_log(zway, Error, ZSTR("Failed to control %s: %s"), message->topic, zstrerror(err));
    }
}

static void mqtt_callback(void* obj, const struct mosquitto_message *message) {
    const char DEVICES[] = "zwave/set/devices/";
    const char CONTROL[] = "zwave/control/";
    if(strncmp(message->topic, DEVICES, sizeof(DEVICES) - 1) == 0) {
        handle_devices(message);
    } else if(strncmp(message->topic, CONTROL, sizeof(CONTROL) - 1) == 0) {
        handle_control(message);
    } else {
        zway_log(zway, Error, ZSTR("Unknown path: %s"), message->topic);
    }
}

static ZWBOOL init_mqtt() {
    mosquitto_lib_init();
#if LIBMOSQUITTO_MAJOR == 0
    mqtt = mosquitto_new("z-way", NULL);
    int mqttErr = mosquitto_connect(mqtt, "localhost", 1883, 300, TRUE);
#else
    mqtt = mosquitto_new("z-way", TRUE, NULL);
    int mqttErr = mosquitto_connect(mqtt, "localhost", 1883, 300);
#endif
    if (mqttErr != MOSQ_ERR_SUCCESS) {
        zway_log_error(zway, Critical, "Failed to connect the MQTT server", mqttErr);
        return FALSE;
    }
    mosquitto_message_callback_set(mqtt, mqtt_callback);
    mqttErr = mosquitto_subscribe(mqtt, NULL, "zwave/set/#", 0);
    if (mqttErr != MOSQ_ERR_SUCCESS) {
        zway_log_error(zway, Critical, "Failed to subscribe to the MQTT server", mqttErr);
        return FALSE;
    }
    mqttErr = mosquitto_subscribe(mqtt, NULL, "zwave/control/#", 0);
    if (mqttErr != MOSQ_ERR_SUCCESS) {
        zway_log_error(zway, Critical, "Failed to subscribe to the MQTT server", mqttErr);
        return FALSE;
    }
    return TRUE;
}

int main(int argc, char **argv) {
    ZWLog logger = zlog_create(stdout, Information);
    ZWError r = zway_init(&zway, ZSTR("/dev/ttyAMA0"), "/opt/z-way-server/config",
                          "/opt/z-way-server/translations", "/opt/z-way-server/ZDDX",
                          NULL, logger);
    if (!init_mqtt()) {
        return -1;
    }
    
    if (r != NoError) {
        zway_log_error(zway, Critical, "Failed to init ZWay", r);
        return -1;
    }
    
    zway_device_add_callback(zway,
                             DeviceAdded | DeviceRemoved | InstanceAdded | InstanceRemoved | 
                             CommandAdded | CommandRemoved | EnumerateExisting,
                             device_callback, NULL);
    
    zway_log(zway, Information, ZSTR("Starting z-Way"));
    r = zway_start(zway, print_zway_terminated, NULL);
    if (r != NoError) {
        zway_log_error(zway, Critical, "Failed to start ZWay", r);
        return -1;
    }
    
    r = zway_discover(zway);
    if (r != NoError) {
        zway_log_error(zway, Critical, "Failed to negotiate with Z-Wave stick", r);
        return -1;
    }

    do_work();
    
    r = zway_stop(zway);
    zway_terminate(&zway);
    
    mosquitto_disconnect(mqtt);
    mosquitto_destroy(mqtt);
    return 0;
}
