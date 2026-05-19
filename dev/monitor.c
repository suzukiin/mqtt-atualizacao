#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <mosquitto.h>
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

#define DEFAULT_MQTT_CONFIG_FILE "/etc/jupiter/mqtt.json"
#define DEFAULT_IDENTITY_FILE "/etc/jupiter/jupiter.json"
#define DEFAULT_INFO_FILE "/home/http/config/info.json"
#define DEFAULT_INPUTS_FILE "/home/http/config/virtual_inputs.json"
#define DEFAULT_OUTPUT_FILE "/home/http/public/telemetry_data.json"

#define LEGACY_INFO_FILE "../http/config/info.json"
#define LEGACY_INPUTS_FILE "../http/config/virtual_inputs.json"
#define LEGACY_OUTPUT_FILE "../http/public/telemetry_data.json"

#define DEFAULT_MQTT_HOST "127.0.0.1"
#define DEFAULT_MQTT_PORT 1883
#define DEFAULT_DEVICE_ID "JUP104260001"
#define DEFAULT_CLIENT_ID DEFAULT_DEVICE_ID
#define DEFAULT_SNMP_COMMUNITY "public"
#define DEFAULT_FW_VERSION "1.0.0"
#define DEFAULT_POLLING_INTERVAL 10
#define MQTT_KEEPALIVE_SECONDS 60
#define MQTT_QOS 1

#define MAX_TOPIC_LEN 256
#define MAX_FIELD_LEN 128
#define MAX_PATH_LEN 256
#define SERIAL_BUFFER_LEN 512
#define RELAY_COUNT 2
#define ANALOG_INPUT_COUNT 4

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_mqtt_connected = 0;

struct app_config {
    char mqtt_host[MAX_FIELD_LEN];
    int mqtt_port;
    char device_id[MAX_FIELD_LEN];
    char username[MAX_FIELD_LEN];
    char password[MAX_FIELD_LEN];
    char client_id[MAX_FIELD_LEN];
    char snmp_community[MAX_FIELD_LEN];
    char fw_version[MAX_FIELD_LEN];
    char identity_file[MAX_PATH_LEN];
    char info_file[MAX_PATH_LEN];
    char inputs_file[MAX_PATH_LEN];
    char output_file[MAX_PATH_LEN];
    int polling_interval;
    char relay_paths[RELAY_COUNT][MAX_PATH_LEN];
    char vin_status_path[MAX_PATH_LEN];
    char analog_names[ANALOG_INPUT_COUNT][MAX_FIELD_LEN];
    char analog_paths[ANALOG_INPUT_COUNT][MAX_PATH_LEN];
    char analog_units[ANALOG_INPUT_COUNT][MAX_FIELD_LEN];
    double analog_scales[ANALOG_INPUT_COUNT];
    double analog_offsets[ANALOG_INPUT_COUNT];
};

struct mqtt_topics {
    char telemetry[MAX_TOPIC_LEN];
    char status[MAX_TOPIC_LEN];
    char wakeup[MAX_TOPIC_LEN];
    char cmd[MAX_TOPIC_LEN];
    char config[MAX_TOPIC_LEN];
    char identity[MAX_TOPIC_LEN];
};

struct mqtt_context {
    const struct app_config *cfg;
    const struct mqtt_topics *topics;
};

static void handle_signal(int signum)
{
    (void)signum;
    g_running = 0;
}

static void set_string(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_len, "%s", src);
}

static const char *json_get_string(cJSON *root, const char *name, const char *fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) ? item->valuestring : fallback;
}

static int json_get_int(cJSON *root, const char *name, int fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static double json_get_double(cJSON *root, const char *name, double fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static bool read_file_content(const char *filename, char **out)
{
    FILE *f = fopen(filename, "rb");
    long length;
    char *buffer;

    if (!f) {
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }

    length = ftell(f);
    if (length < 0) {
        fclose(f);
        return false;
    }

    rewind(f);
    buffer = calloc((size_t)length + 1, 1);
    if (!buffer) {
        fclose(f);
        return false;
    }

    if (length > 0 && fread(buffer, 1, (size_t)length, f) != (size_t)length) {
        free(buffer);
        fclose(f);
        return false;
    }

    fclose(f);
    *out = buffer;
    return true;
}

static bool write_file_content(const char *filename, const char *content)
{
    FILE *f = fopen(filename, "w");

    if (!f) {
        return false;
    }

    if (fputs(content, f) == EOF) {
        fclose(f);
        return false;
    }

    fclose(f);
    return true;
}

static bool read_first_available_file(const char *primary, const char *fallback, char **out)
{
    if (read_file_content(primary, out)) {
        return true;
    }

    return fallback && read_file_content(fallback, out);
}

static cJSON *read_json_file(const char *primary, const char *fallback)
{
    char *content = NULL;
    cJSON *json;

    if (!read_first_available_file(primary, fallback, &content)) {
        return NULL;
    }

    json = cJSON_Parse(content);
    free(content);
    return json;
}

static bool read_text_value(const char *path, char *buffer, size_t buffer_len)
{
    FILE *f;
    size_t len;

    if (!path || path[0] == '\0' || !buffer || buffer_len == 0) {
        return false;
    }

    f = fopen(path, "r");
    if (!f) {
        return false;
    }

    if (!fgets(buffer, (int)buffer_len, f)) {
        fclose(f);
        return false;
    }

    fclose(f);
    len = strlen(buffer);
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r' ||
                       buffer[len - 1] == ' ' || buffer[len - 1] == '\t')) {
        buffer[--len] = '\0';
    }

    return len > 0;
}

static bool read_int_value(const char *path, int *value)
{
    char buffer[64];
    char *endptr = NULL;
    long parsed;

    if (!read_text_value(path, buffer, sizeof(buffer))) {
        return false;
    }

    errno = 0;
    parsed = strtol(buffer, &endptr, 10);
    if (errno != 0 || endptr == buffer) {
        return false;
    }

    *value = (int)parsed;
    return true;
}

static bool read_double_value(const char *path, double *value)
{
    char buffer[64];
    char *endptr = NULL;
    double parsed;

    if (!read_text_value(path, buffer, sizeof(buffer))) {
        return false;
    }

    errno = 0;
    parsed = strtod(buffer, &endptr);
    if (errno != 0 || endptr == buffer) {
        return false;
    }

    *value = parsed;
    return true;
}

static void load_defaults(struct app_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    set_string(cfg->mqtt_host, sizeof(cfg->mqtt_host), DEFAULT_MQTT_HOST);
    cfg->mqtt_port = DEFAULT_MQTT_PORT;
    set_string(cfg->device_id, sizeof(cfg->device_id), DEFAULT_DEVICE_ID);
    set_string(cfg->username, sizeof(cfg->username), DEFAULT_DEVICE_ID);
    set_string(cfg->client_id, sizeof(cfg->client_id), DEFAULT_CLIENT_ID);
    set_string(cfg->snmp_community, sizeof(cfg->snmp_community), DEFAULT_SNMP_COMMUNITY);
    set_string(cfg->fw_version, sizeof(cfg->fw_version), DEFAULT_FW_VERSION);
    set_string(cfg->identity_file, sizeof(cfg->identity_file), DEFAULT_IDENTITY_FILE);
    set_string(cfg->info_file, sizeof(cfg->info_file), DEFAULT_INFO_FILE);
    set_string(cfg->inputs_file, sizeof(cfg->inputs_file), DEFAULT_INPUTS_FILE);
    set_string(cfg->output_file, sizeof(cfg->output_file), DEFAULT_OUTPUT_FILE);
    cfg->polling_interval = DEFAULT_POLLING_INTERVAL;

    set_string(cfg->relay_paths[0], sizeof(cfg->relay_paths[0]), "/dev/relay1");
    set_string(cfg->relay_paths[1], sizeof(cfg->relay_paths[1]), "/dev/relay2");
    set_string(cfg->vin_status_path, sizeof(cfg->vin_status_path), "/dev/vin_status");

    for (int i = 0; i < ANALOG_INPUT_COUNT; i++) {
        snprintf(cfg->analog_names[i], sizeof(cfg->analog_names[i]), "analog_%d", i + 1);
        snprintf(cfg->analog_paths[i], sizeof(cfg->analog_paths[i]),
                 "/sys/bus/iio/devices/iio:device0/in_voltage%d_raw", i);
        set_string(cfg->analog_units[i], sizeof(cfg->analog_units[i]), "raw");
        cfg->analog_scales[i] = 1.0;
        cfg->analog_offsets[i] = 0.0;
    }
}

static void apply_info_file(struct app_config *cfg)
{
    char *content = NULL;
    cJSON *json;

    if (!read_first_available_file(cfg->info_file, LEGACY_INFO_FILE, &content)) {
        return;
    }

    json = cJSON_Parse(content);
    free(content);
    if (!json) {
        return;
    }

    set_string(cfg->fw_version, sizeof(cfg->fw_version),
               json_get_string(json, "version", cfg->fw_version));

    cJSON_Delete(json);
}

static void apply_identity_file(struct app_config *cfg)
{
    char *content = NULL;
    cJSON *json;
    const char *device_id;

    if (!read_file_content(cfg->identity_file, &content)) {
        return;
    }

    json = cJSON_Parse(content);
    free(content);
    if (!json) {
        fprintf(stderr, "Invalid device identity JSON: %s\n", cfg->identity_file);
        return;
    }

    device_id = json_get_string(json, "device_id", NULL);
    if (!device_id) {
        device_id = json_get_string(json, "deviceId", NULL);
    }
    if (!device_id) {
        device_id = json_get_string(json, "id", NULL);
    }

    if (device_id && device_id[0] != '\0') {
        set_string(cfg->device_id, sizeof(cfg->device_id), device_id);
        set_string(cfg->username, sizeof(cfg->username), device_id);
        set_string(cfg->client_id, sizeof(cfg->client_id), device_id);
    }

    cJSON_Delete(json);
}

static void apply_hardware_config(struct app_config *cfg, cJSON *json)
{
    cJSON *relays = cJSON_GetObjectItemCaseSensitive(json, "relay_paths");
    cJSON *analog_inputs = cJSON_GetObjectItemCaseSensitive(json, "analog_inputs");

    set_string(cfg->vin_status_path, sizeof(cfg->vin_status_path),
               json_get_string(json, "vin_status_path", cfg->vin_status_path));

    if (cJSON_IsArray(relays)) {
        for (int i = 0; i < RELAY_COUNT; i++) {
            cJSON *relay_path = cJSON_GetArrayItem(relays, i);
            if (cJSON_IsString(relay_path)) {
                set_string(cfg->relay_paths[i], sizeof(cfg->relay_paths[i]),
                           relay_path->valuestring);
            }
        }
    }

    if (cJSON_IsArray(analog_inputs)) {
        for (int i = 0; i < ANALOG_INPUT_COUNT; i++) {
            cJSON *analog = cJSON_GetArrayItem(analog_inputs, i);
            if (!cJSON_IsObject(analog)) {
                continue;
            }

            set_string(cfg->analog_names[i], sizeof(cfg->analog_names[i]),
                       json_get_string(analog, "name", cfg->analog_names[i]));
            set_string(cfg->analog_paths[i], sizeof(cfg->analog_paths[i]),
                       json_get_string(analog, "path", cfg->analog_paths[i]));
            set_string(cfg->analog_units[i], sizeof(cfg->analog_units[i]),
                       json_get_string(analog, "unit", cfg->analog_units[i]));
            cfg->analog_scales[i] = json_get_double(analog, "scale", cfg->analog_scales[i]);
            cfg->analog_offsets[i] = json_get_double(analog, "offset", cfg->analog_offsets[i]);
        }
    }
}

static bool load_config(const char *config_file, struct app_config *cfg)
{
    char *content = NULL;
    cJSON *json;
    const char *password_env;

    load_defaults(cfg);

    if (read_file_content(config_file, &content)) {
        json = cJSON_Parse(content);
        free(content);
        if (!json) {
            fprintf(stderr, "Invalid MQTT config JSON: %s\n", config_file);
            return false;
        }

        set_string(cfg->mqtt_host, sizeof(cfg->mqtt_host),
                   json_get_string(json, "host", cfg->mqtt_host));
        cfg->mqtt_port = json_get_int(json, "port", cfg->mqtt_port);
        set_string(cfg->device_id, sizeof(cfg->device_id),
                   json_get_string(json, "device_id", cfg->device_id));
        set_string(cfg->username, sizeof(cfg->username),
                   json_get_string(json, "username", cfg->device_id));
        set_string(cfg->password, sizeof(cfg->password),
                   json_get_string(json, "password", cfg->password));
        set_string(cfg->client_id, sizeof(cfg->client_id),
                   json_get_string(json, "client_id", cfg->device_id));
        set_string(cfg->snmp_community, sizeof(cfg->snmp_community),
                   json_get_string(json, "snmp_community", cfg->snmp_community));
        set_string(cfg->fw_version, sizeof(cfg->fw_version),
                   json_get_string(json, "fw_version", cfg->fw_version));
        set_string(cfg->identity_file, sizeof(cfg->identity_file),
                   json_get_string(json, "identity_file", cfg->identity_file));
        set_string(cfg->info_file, sizeof(cfg->info_file),
                   json_get_string(json, "info_file", cfg->info_file));
        set_string(cfg->inputs_file, sizeof(cfg->inputs_file),
                   json_get_string(json, "inputs_file", cfg->inputs_file));
        set_string(cfg->output_file, sizeof(cfg->output_file),
                   json_get_string(json, "output_file", cfg->output_file));
        cfg->polling_interval = json_get_int(json, "polling_interval", cfg->polling_interval);
        apply_hardware_config(cfg, json);

        cJSON_Delete(json);
    }

    apply_identity_file(cfg);

    password_env = getenv("JUPITER_MQTT_PASSWORD");
    if (password_env && password_env[0] != '\0') {
        set_string(cfg->password, sizeof(cfg->password), password_env);
    }

    apply_info_file(cfg);

    if (cfg->device_id[0] == '\0' || cfg->username[0] == '\0' ||
        cfg->client_id[0] == '\0' || cfg->password[0] == '\0') {
        fprintf(stderr, "MQTT identity is incomplete. Check %s or JUPITER_MQTT_PASSWORD.\n",
                config_file);
        return false;
    }

    if (strcmp(cfg->username, cfg->device_id) != 0) {
        fprintf(stderr, "MQTT username must match device_id for ACL isolation.\n");
        return false;
    }

    if (cfg->mqtt_port <= 0 || cfg->mqtt_port > 65535) {
        fprintf(stderr, "Invalid MQTT port: %d\n", cfg->mqtt_port);
        return false;
    }

    if (cfg->polling_interval <= 0) {
        cfg->polling_interval = DEFAULT_POLLING_INTERVAL;
    }

    return true;
}

static void build_topics(const char *device_id, struct mqtt_topics *topics)
{
    snprintf(topics->telemetry, sizeof(topics->telemetry),
             "jupiter/%s/telemetry", device_id);
    snprintf(topics->status, sizeof(topics->status),
             "jupiter/%s/status", device_id);
    snprintf(topics->wakeup, sizeof(topics->wakeup),
             "jupiter/%s/wakeup", device_id);
    snprintf(topics->cmd, sizeof(topics->cmd),
             "jupiter/%s/cmd", device_id);
    snprintf(topics->config, sizeof(topics->config),
             "jupiter/%s/config", device_id);
    snprintf(topics->identity, sizeof(topics->identity),
             "jupiter/%s/identity", device_id);
}

static void json_upsert_number(cJSON *object, const char *name, double number)
{
    if (cJSON_HasObjectItem(object, name)) {
        cJSON_ReplaceItemInObject(object, name, cJSON_CreateNumber(number));
    } else {
        cJSON_AddNumberToObject(object, name, number);
    }
}

static void json_upsert_string(cJSON *object, const char *name, const char *string)
{
    if (cJSON_HasObjectItem(object, name)) {
        cJSON_ReplaceItemInObject(object, name, cJSON_CreateString(string));
    } else {
        cJSON_AddStringToObject(object, name, string);
    }
}

static void json_upsert_bool(cJSON *object, const char *name, int boolean)
{
    if (cJSON_HasObjectItem(object, name)) {
        cJSON_ReplaceItemInObject(object, name, cJSON_CreateBool(boolean));
    } else {
        cJSON_AddBoolToObject(object, name, boolean);
    }
}

static int snmp_get_value(const struct app_config *cfg, const char *ip, const char *oid_str,
                          double *out_val, char *out_str, int str_len)
{
    netsnmp_session session, *ss;
    netsnmp_pdu *pdu;
    netsnmp_pdu *response = NULL;
    oid anOID[MAX_OID_LEN];
    size_t anOID_len = MAX_OID_LEN;
    int status;
    int ret = -1;

    snmp_sess_init(&session);
    session.peername = strdup(ip);
    session.version = SNMP_VERSION_2c;
    session.community = (u_char *)cfg->snmp_community;
    session.community_len = strlen(cfg->snmp_community);
    session.timeout = 1000000;
    session.retries = 1;

    SOCK_STARTUP;
    ss = snmp_open(&session);
    if (!ss) {
        SOCK_CLEANUP;
        free(session.peername);
        return -1;
    }

    pdu = snmp_pdu_create(SNMP_MSG_GET);
    if (!snmp_parse_oid(oid_str, anOID, &anOID_len)) {
        snmp_close(ss);
        SOCK_CLEANUP;
        free(session.peername);
        return -1;
    }

    snmp_add_null_var(pdu, anOID, anOID_len);
    status = snmp_synch_response(ss, pdu, &response);

    if (status == STAT_SUCCESS && response && response->errstat == SNMP_ERR_NOERROR) {
        netsnmp_variable_list *vars = response->variables;
        if (vars) {
            if (vars->type == ASN_INTEGER || vars->type == ASN_COUNTER ||
                vars->type == ASN_GAUGE || vars->type == ASN_TIMETICKS ||
                vars->type == ASN_UNSIGNED) {
                if (vars->val.integer) {
                    *out_val = (double)*vars->val.integer;
                    ret = 0;
                }
            } else if (vars->type == ASN_OCTET_STR) {
                if (out_str && vars->val.string && str_len > 0) {
                    int len = (int)vars->val_len;
                    if (len >= str_len) {
                        len = str_len - 1;
                    }
                    memcpy(out_str, vars->val.string, (size_t)len);
                    out_str[len] = '\0';
                    ret = 1;
                }
            }
        }
    }

    if (response) {
        snmp_free_pdu(response);
    }
    snmp_close(ss);
    SOCK_CLEANUP;
    free(session.peername);
    return ret;
}

static double read_cpu_temp(void)
{
    FILE *fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    char buffer[64];
    char *endptr = NULL;
    long temp_millicelsius;

    if (!fp) {
        return NAN;
    }

    if (!fgets(buffer, sizeof(buffer), fp)) {
        fclose(fp);
        return NAN;
    }

    fclose(fp);
    temp_millicelsius = strtol(buffer, &endptr, 10);
    if (endptr == buffer) {
        return NAN;
    }

    return (double)temp_millicelsius / 1000.0;
}

static int read_modem_rssi(void)
{
    int fd = open("/dev/ttyUSB1", O_RDWR | O_NOCTTY);
    struct termios options;
    char buffer[SERIAL_BUFFER_LEN];
    int total_read = 0;

    if (fd == -1) {
        return -1;
    }

    if (tcgetattr(fd, &options) != 0) {
        close(fd);
        return -1;
    }

    cfsetispeed(&options, B115200);
    cfsetospeed(&options, B115200);
    options.c_cflag |= (CLOCAL | CREAD | CS8);
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_oflag &= ~OPOST;
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 10;

    if (tcsetattr(fd, TCSANOW, &options) != 0) {
        close(fd);
        return -1;
    }

    memset(buffer, 0, sizeof(buffer));
    tcflush(fd, TCIOFLUSH);
    (void)write(fd, "ATE0\r\n", 6);
    usleep(100000);
    tcflush(fd, TCIFLUSH);
    (void)write(fd, "AT+CSQ\r\n", 8);

    for (int i = 0; i < 5; i++) {
        int n = read(fd, buffer + total_read, sizeof(buffer) - (size_t)total_read - 1);
        if (n > 0) {
            total_read += n;
        }
        if (strstr(buffer, "OK") || strstr(buffer, "ERROR")) {
            break;
        }
        usleep(200000);
    }

    close(fd);

    char *p = strstr(buffer, "+CSQ:");
    if (p) {
        int rssi;
        int ber;
        if (sscanf(p, "+CSQ: %d,%d", &rssi, &ber) == 2 && rssi != 99) {
            return rssi;
        }
    }

    return -1;
}

static cJSON *create_status_payload(const struct app_config *cfg, const char *status)
{
    cJSON *payload = cJSON_CreateObject();

    if (!payload) {
        return NULL;
    }

    cJSON_AddStringToObject(payload, "device_id", cfg->device_id);
    cJSON_AddStringToObject(payload, "status", status);
    cJSON_AddStringToObject(payload, "fw_version", cfg->fw_version);
    cJSON_AddNumberToObject(payload, "timestamp", (double)time(NULL));
    return payload;
}

static cJSON *create_wakeup_payload(const struct app_config *cfg)
{
    cJSON *payload = cJSON_CreateObject();

    if (!payload) {
        return NULL;
    }

    cJSON_AddStringToObject(payload, "device_id", cfg->device_id);
    cJSON_AddStringToObject(payload, "event", "wakeup");
    cJSON_AddStringToObject(payload, "fw_version", cfg->fw_version);
    cJSON_AddNumberToObject(payload, "timestamp", (double)time(NULL));
    return payload;
}

static void publish_json(struct mosquitto *mosq, const char *topic, cJSON *json, bool retain)
{
    char *payload;
    int rc;

    if (!mosq || !topic || !json || !g_mqtt_connected) {
        return;
    }

    payload = cJSON_PrintUnformatted(json);
    if (!payload) {
        return;
    }

    rc = mosquitto_publish(mosq, NULL, topic, (int)strlen(payload), payload, MQTT_QOS, retain);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "MQTT publish failed on %s: %s\n", topic, mosquitto_strerror(rc));
    }

    free(payload);
}

static void publish_status(struct mosquitto *mosq, const struct app_config *cfg,
                           const struct mqtt_topics *topics, const char *status)
{
    cJSON *payload = create_status_payload(cfg, status);

    if (!payload) {
        return;
    }

    publish_json(mosq, topics->status, payload, true);
    cJSON_Delete(payload);
}

static void publish_wakeup(struct mosquitto *mosq, const struct app_config *cfg,
                           const struct mqtt_topics *topics)
{
    cJSON *payload = create_wakeup_payload(cfg);

    if (!payload) {
        return;
    }

    publish_json(mosq, topics->wakeup, payload, false);
    cJSON_Delete(payload);
}

static cJSON *get_first_object_from_array(cJSON *array)
{
    cJSON *item;

    cJSON_ArrayForEach(item, array) {
        if (cJSON_IsObject(item)) {
            return item;
        }
    }

    return NULL;
}

static cJSON *extract_identity_object(cJSON *root)
{
    cJSON *info;

    if (cJSON_IsArray(root)) {
        return get_first_object_from_array(root);
    }

    if (!cJSON_IsObject(root)) {
        return NULL;
    }

    info = cJSON_GetObjectItemCaseSensitive(root, "info");
    if (!cJSON_IsObject(info)) {
        info = cJSON_GetObjectItemCaseSensitive(root, "identification");
    }
    if (!cJSON_IsObject(info)) {
        info = cJSON_GetObjectItemCaseSensitive(root, "identificacao");
    }
    if (cJSON_IsArray(info)) {
        return get_first_object_from_array(info);
    }
    if (cJSON_IsObject(info)) {
        return info;
    }

    return root;
}

static bool is_backend_site_payload(cJSON *info)
{
    return cJSON_IsObject(info) &&
           (cJSON_HasObjectItem(info, "nome") ||
            cJSON_HasObjectItem(info, "cidade") ||
            cJSON_HasObjectItem(info, "estado") ||
            cJSON_HasObjectItem(info, "canal") ||
            cJSON_HasObjectItem(info, "cliente_id"));
}

static void add_string_if_present(cJSON *dst, const char *dst_name,
                                  cJSON *src, const char *src_name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(src, src_name);

    if (cJSON_IsString(item)) {
        cJSON_AddStringToObject(dst, dst_name, item->valuestring);
    }
}

static void add_number_if_present(cJSON *dst, const char *dst_name,
                                  cJSON *src, const char *src_name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(src, src_name);

    if (cJSON_IsNumber(item)) {
        cJSON_AddNumberToObject(dst, dst_name, item->valuedouble);
    }
}

static void add_existing_string_or_default(cJSON *dst, const char *name,
                                           cJSON *existing, const char *fallback)
{
    cJSON *item = existing ? cJSON_GetObjectItemCaseSensitive(existing, name) : NULL;

    if (cJSON_IsString(item)) {
        cJSON_AddStringToObject(dst, name, item->valuestring);
    } else {
        cJSON_AddStringToObject(dst, name, fallback ? fallback : "");
    }
}

static void add_client_value(cJSON *dst, cJSON *site, cJSON *existing)
{
    cJSON *client = cJSON_GetObjectItemCaseSensitive(site, "cliente");
    char client_id[32];

    if (!cJSON_IsString(client)) {
        client = cJSON_GetObjectItemCaseSensitive(site, "cliente_nome");
    }
    if (!cJSON_IsString(client)) {
        client = cJSON_GetObjectItemCaseSensitive(site, "client");
    }

    if (cJSON_IsString(client)) {
        cJSON_AddStringToObject(dst, "client", client->valuestring);
        return;
    }

    client = cJSON_GetObjectItemCaseSensitive(site, "cliente_id");
    if (cJSON_IsNumber(client)) {
        snprintf(client_id, sizeof(client_id), "%.0f", client->valuedouble);
        cJSON_AddStringToObject(dst, "client", client_id);
        return;
    }

    add_existing_string_or_default(dst, "client", existing, "--");
}

static void add_location_value(cJSON *dst, cJSON *site, cJSON *existing)
{
    cJSON *cidade = cJSON_GetObjectItemCaseSensitive(site, "cidade");
    cJSON *estado = cJSON_GetObjectItemCaseSensitive(site, "estado");
    cJSON *nome = cJSON_GetObjectItemCaseSensitive(site, "nome");
    char location[MAX_FIELD_LEN * 2];

    if (cJSON_IsString(cidade) && cJSON_IsString(estado)) {
        snprintf(location, sizeof(location), "%s-%s", cidade->valuestring, estado->valuestring);
        cJSON_AddStringToObject(dst, "location", location);
        return;
    }

    if (cJSON_IsString(nome)) {
        cJSON_AddStringToObject(dst, "location", nome->valuestring);
        return;
    }

    add_existing_string_or_default(dst, "location", existing, "--");
}

static cJSON *create_normalized_info_payload(const struct app_config *cfg,
                                             cJSON *root, cJSON *site)
{
    cJSON *out = cJSON_CreateObject();
    cJSON *existing;
    cJSON *backend;

    if (!out) {
        return NULL;
    }

    existing = read_json_file(cfg->info_file, LEGACY_INFO_FILE);

    add_location_value(out, site, existing);
    add_client_value(out, site, existing);
    cJSON_AddStringToObject(out, "deviceId", cfg->device_id);
    add_existing_string_or_default(out, "wanIp", existing, "--");
    add_existing_string_or_default(out, "lanIp", existing, "--");
    cJSON_AddStringToObject(out, "version", cfg->fw_version);

    add_number_if_present(out, "siteId", site, "id");
    add_string_if_present(out, "siteName", site, "nome");
    add_number_if_present(out, "clientId", site, "cliente_id");
    add_string_if_present(out, "address", site, "endereco");
    add_string_if_present(out, "city", site, "cidade");
    add_string_if_present(out, "state", site, "estado");
    add_string_if_present(out, "postalCode", site, "cep");
    add_number_if_present(out, "latitude", site, "latitude");
    add_number_if_present(out, "longitude", site, "longitude");
    add_string_if_present(out, "type", site, "tipo");
    add_string_if_present(out, "siteStatus", site, "status");
    add_string_if_present(out, "observations", site, "observacoes");
    add_string_if_present(out, "registeredAt", site, "data_cadastro");
    add_string_if_present(out, "updatedAt", site, "data_atualizacao");
    add_string_if_present(out, "channel", site, "canal");
    add_string_if_present(out, "configuration", site, "configuracao");
    add_string_if_present(out, "complement", site, "complemento");
    add_number_if_present(out, "transmissionProfileId", site, "perfil_transmissao_id");

    backend = cJSON_Duplicate(site, true);
    if (backend) {
        cJSON_AddItemToObject(out, "backend", backend);
    }

    if (cJSON_IsArray(root)) {
        backend = cJSON_Duplicate(root, true);
        if (backend) {
            cJSON_AddItemToObject(out, "backendSites", backend);
        }
    }

    cJSON_Delete(existing);
    return out;
}

static bool save_identity_payload(const struct app_config *cfg, const char *payload,
                                  int payload_len)
{
    char *payload_copy;
    cJSON *root;
    cJSON *info;
    cJSON *normalized = NULL;
    char *content;
    bool saved;

    if (!cfg || !payload || payload_len <= 0) {
        return false;
    }

    payload_copy = calloc((size_t)payload_len + 1, 1);
    if (!payload_copy) {
        return false;
    }

    memcpy(payload_copy, payload, (size_t)payload_len);
    root = cJSON_Parse(payload_copy);
    free(payload_copy);
    if (!root) {
        fprintf(stderr, "Invalid identity payload JSON.\n");
        return false;
    }

    info = extract_identity_object(root);
    if (!info) {
        cJSON_Delete(root);
        return false;
    }

    if (is_backend_site_payload(info)) {
        normalized = create_normalized_info_payload(cfg, root, info);
        if (normalized) {
            info = normalized;
        }
    }

    content = cJSON_Print(info);
    if (!content) {
        cJSON_Delete(normalized);
        cJSON_Delete(root);
        return false;
    }

    saved = write_file_content(cfg->info_file, content);
    if (!saved) {
        saved = write_file_content(LEGACY_INFO_FILE, content);
    }

    free(content);
    cJSON_Delete(normalized);
    cJSON_Delete(root);
    return saved;
}

static cJSON *collect_system_metrics(void)
{
    cJSON *system = cJSON_CreateObject();
    double cpu_temp = read_cpu_temp();
    int rssi = read_modem_rssi();

    if (!system) {
        return NULL;
    }

    if (!isnan(cpu_temp)) {
        cJSON_AddNumberToObject(system, "cpu_temp_c", cpu_temp);
    }

    if (rssi >= 0) {
        cJSON_AddNumberToObject(system, "modem_rssi_csq", rssi);
    }

    return system;
}

static cJSON *collect_hardware_state(const struct app_config *cfg)
{
    cJSON *hardware = cJSON_CreateObject();
    cJSON *relays = cJSON_CreateObject();
    cJSON *analog_inputs = cJSON_CreateObject();
    int vin_status;

    if (!hardware) {
        return NULL;
    }

    if (read_int_value(cfg->vin_status_path, &vin_status)) {
        cJSON_AddNumberToObject(hardware, "vin_status", vin_status);
        cJSON_AddBoolToObject(hardware, "vin_present", vin_status != 0);
        cJSON_AddStringToObject(hardware, "power_mode",
                                vin_status != 0 ? "normal" : "battery");
    }

    if (relays) {
        for (int i = 0; i < RELAY_COUNT; i++) {
            int relay_state;
            char name[32];
            snprintf(name, sizeof(name), "relay%d", i + 1);
            if (read_int_value(cfg->relay_paths[i], &relay_state)) {
                cJSON_AddBoolToObject(relays, name, relay_state != 0);
            }
        }
        cJSON_AddItemToObject(hardware, "relays", relays);
    }

    if (analog_inputs) {
        for (int i = 0; i < ANALOG_INPUT_COUNT; i++) {
            double raw_value;
            cJSON *analog;

            if (!read_double_value(cfg->analog_paths[i], &raw_value)) {
                continue;
            }

            analog = cJSON_CreateObject();
            if (!analog) {
                continue;
            }

            cJSON_AddNumberToObject(analog, "raw", raw_value);
            cJSON_AddNumberToObject(analog, "value",
                                    (raw_value * cfg->analog_scales[i]) +
                                    cfg->analog_offsets[i]);
            cJSON_AddStringToObject(analog, "unit", cfg->analog_units[i]);
            cJSON_AddItemToObject(analog_inputs, cfg->analog_names[i], analog);
        }
        cJSON_AddItemToObject(hardware, "analog_inputs", analog_inputs);
    }

    return hardware;
}

static void collect_snmp_metrics(const struct app_config *cfg, cJSON *inputs, cJSON *payload)
{
    cJSON *equipamentos = cJSON_GetObjectItem(inputs, "equipamentos");
    cJSON *equipment_array = cJSON_CreateArray();
    cJSON *equipamento;

    if (!equipment_array) {
        return;
    }

    cJSON_ArrayForEach(equipamento, equipamentos) {
        cJSON *ip_item = cJSON_GetObjectItem(equipamento, "ip");
        cJSON *site_item = cJSON_GetObjectItem(equipamento, "site_id");
        cJSON *modelo_item = cJSON_GetObjectItem(equipamento, "modelo");
        cJSON *serial_item = cJSON_GetObjectItem(equipamento, "numero_de_serie");
        cJSON *oids = cJSON_GetObjectItem(equipamento, "OIDS");
        cJSON *metrics = cJSON_CreateObject();
        cJSON *equipment_payload = cJSON_CreateObject();
        cJSON *oid_item;
        bool has_metric = false;

        if (!cJSON_IsString(ip_item) || !cJSON_IsString(site_item) ||
            !cJSON_IsArray(oids) || !metrics || !equipment_payload) {
            cJSON_Delete(metrics);
            cJSON_Delete(equipment_payload);
            continue;
        }

        cJSON_ArrayForEach(oid_item, oids) {
            cJSON *oid_addr_item = cJSON_GetObjectItem(oid_item, "oid");
            cJSON *topico_item = cJSON_GetObjectItem(oid_item, "topico");
            cJSON *mascara_item = cJSON_GetObjectItem(oid_item, "mascara");
            double raw_value = 0;
            double mask = cJSON_IsNumber(mascara_item) ? mascara_item->valuedouble : 1.0;
            char string_value[256] = {0};
            int res;

            if (!cJSON_IsString(oid_addr_item) || !cJSON_IsString(topico_item)) {
                continue;
            }

            res = snmp_get_value(cfg, ip_item->valuestring, oid_addr_item->valuestring,
                                 &raw_value, string_value, sizeof(string_value));
            if (res == -1) {
                json_upsert_bool(oid_item, "last_error", true);
                continue;
            }

            json_upsert_bool(oid_item, "last_error", false);
            json_upsert_number(oid_item, "last_timestamp", (double)time(NULL));

            if (res == 0) {
                double processed_value = raw_value * mask;
                cJSON_AddNumberToObject(metrics, topico_item->valuestring, processed_value);
                json_upsert_number(oid_item, "last_value", processed_value);

                cJSON *enums = cJSON_GetObjectItem(oid_item, "enum");
                if (enums) {
                    char key[32];
                    cJSON *enum_val;
                    snprintf(key, sizeof(key), "%d", (int)raw_value);
                    enum_val = cJSON_GetObjectItem(enums, key);
                    if (cJSON_IsString(enum_val)) {
                        char status_name[MAX_FIELD_LEN];
                        snprintf(status_name, sizeof(status_name), "%s_status",
                                 topico_item->valuestring);
                        cJSON_AddStringToObject(metrics, status_name, enum_val->valuestring);
                        json_upsert_string(oid_item, "last_status", enum_val->valuestring);
                    }
                }
            } else {
                cJSON_AddStringToObject(metrics, topico_item->valuestring, string_value);
                json_upsert_string(oid_item, "last_value", string_value);
            }

            has_metric = true;
        }

        if (has_metric) {
            cJSON_AddStringToObject(equipment_payload, "site_id", site_item->valuestring);
            cJSON_AddStringToObject(equipment_payload, "ip", ip_item->valuestring);
            cJSON_AddStringToObject(equipment_payload, "model",
                                    cJSON_IsString(modelo_item) ? modelo_item->valuestring : "unknown");
            cJSON_AddStringToObject(equipment_payload, "serial",
                                    cJSON_IsString(serial_item) ? serial_item->valuestring : "unknown");
            cJSON_AddItemToObject(equipment_payload, "metrics", metrics);
            cJSON_AddItemToArray(equipment_array, equipment_payload);
        } else {
            cJSON_Delete(metrics);
            cJSON_Delete(equipment_payload);
        }
    }

    cJSON_AddItemToObject(payload, "equipment", equipment_array);
}

static cJSON *build_telemetry_payload(const struct app_config *cfg, cJSON *inputs,
                                      unsigned long sequence)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON *system;
    cJSON *hardware;

    if (!payload) {
        return NULL;
    }

    cJSON_AddStringToObject(payload, "device_id", cfg->device_id);
    cJSON_AddNumberToObject(payload, "timestamp", (double)time(NULL));
    cJSON_AddNumberToObject(payload, "seq", (double)sequence);

    system = collect_system_metrics();
    if (system) {
        cJSON_AddItemToObject(payload, "system", system);
    }

    hardware = collect_hardware_state(cfg);
    if (hardware) {
        cJSON_AddItemToObject(payload, "hardware", hardware);
    }

    collect_snmp_metrics(cfg, inputs, payload);
    return payload;
}

static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    const struct mqtt_context *context = userdata;
    const struct mqtt_topics *topics = context ? context->topics : NULL;
    const struct app_config *cfg = context ? context->cfg : NULL;

    if (rc != 0) {
        g_mqtt_connected = 0;
        fprintf(stderr, "MQTT connect failed: %s\n", mosquitto_connack_string(rc));
        return;
    }

    g_mqtt_connected = 1;
    if (topics) {
        (void)mosquitto_subscribe(mosq, NULL, topics->cmd, MQTT_QOS);
        (void)mosquitto_subscribe(mosq, NULL, topics->config, MQTT_QOS);
        (void)mosquitto_subscribe(mosq, NULL, topics->identity, MQTT_QOS);
    }
    if (cfg && topics) {
        publish_wakeup(mosq, cfg, topics);
        fprintf(stdout, "MQTT connected. Wakeup sent to %s. Subscribed to %s, %s and %s\n",
                topics->wakeup, topics->cmd, topics->config, topics->identity);
    }
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    (void)userdata;
    g_mqtt_connected = 0;

    if (rc != 0) {
        fprintf(stderr, "MQTT disconnected unexpectedly: %s\n", mosquitto_strerror(rc));
    }
}

static void on_message(struct mosquitto *mosq, void *userdata,
                       const struct mosquitto_message *message)
{
    (void)mosq;
    const struct mqtt_context *context = userdata;
    const struct mqtt_topics *topics = context ? context->topics : NULL;
    const struct app_config *cfg = context ? context->cfg : NULL;

    if (!message || !message->topic) {
        return;
    }

    fprintf(stdout, "MQTT message received on %s: %.*s\n",
            message->topic, message->payloadlen,
            message->payload ? (const char *)message->payload : "");

    if (cfg && topics && message->payload &&
        (strcmp(message->topic, topics->identity) == 0 ||
         strcmp(message->topic, topics->config) == 0)) {
        if (save_identity_payload(cfg, (const char *)message->payload, message->payloadlen)) {
            fprintf(stdout, "Telemetry identity saved to %s\n", cfg->info_file);
        } else {
            fprintf(stderr, "Unable to save telemetry identity to %s\n", cfg->info_file);
        }
    }
}

int main(int argc, char *argv[])
{
    const char *config_file = argc > 1 ? argv[1] : DEFAULT_MQTT_CONFIG_FILE;
    struct app_config cfg;
    struct mqtt_topics topics;
    struct mqtt_context context;
    struct mosquitto *mosq = NULL;
    unsigned long sequence = 0;
    int rc;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (!load_config(config_file, &cfg)) {
        return 1;
    }

    build_topics(cfg.device_id, &topics);
    context.cfg = &cfg;
    context.topics = &topics;
    mosquitto_lib_init();
    init_snmp("jupiter-monitor");

    mosq = mosquitto_new(cfg.client_id, true, &context);
    if (!mosq) {
        fprintf(stderr, "Unable to create MQTT client.\n");
        mosquitto_lib_cleanup();
        return 1;
    }

    mosquitto_username_pw_set(mosq, cfg.username, cfg.password);
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_message_callback_set(mosq, on_message);
    mosquitto_reconnect_delay_set(mosq, 2, 60, true);

    cJSON *will = create_status_payload(&cfg, "offline");
    if (will) {
        char *will_payload = cJSON_PrintUnformatted(will);
        if (will_payload) {
            mosquitto_will_set(mosq, topics.status, (int)strlen(will_payload),
                               will_payload, MQTT_QOS, true);
            free(will_payload);
        }
        cJSON_Delete(will);
    }

    rc = mosquitto_connect_async(mosq, cfg.mqtt_host, cfg.mqtt_port, MQTT_KEEPALIVE_SECONDS);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Unable to start MQTT connection to %s:%d: %s\n",
                cfg.mqtt_host, cfg.mqtt_port, mosquitto_strerror(rc));
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    rc = mosquitto_loop_start(mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Unable to start MQTT loop: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    fprintf(stdout, "Jupiter monitor started as %s on %s:%d\n",
            cfg.device_id, cfg.mqtt_host, cfg.mqtt_port);

    while (g_running) {
        char *json_content = NULL;
        cJSON *inputs;
        cJSON *payload;
        char *updated_json;

        if (!read_first_available_file(cfg.inputs_file, LEGACY_INPUTS_FILE, &json_content)) {
            fprintf(stderr, "Unable to read telemetry input file: %s\n", cfg.inputs_file);
            sleep((unsigned int)cfg.polling_interval);
            continue;
        }

        inputs = cJSON_Parse(json_content);
        free(json_content);
        if (!inputs) {
            fprintf(stderr, "Invalid telemetry input JSON.\n");
            sleep((unsigned int)cfg.polling_interval);
            continue;
        }

        payload = build_telemetry_payload(&cfg, inputs, ++sequence);
        if (payload) {
            publish_status(mosq, &cfg, &topics, "online");
            publish_json(mosq, topics.telemetry, payload, false);
            cJSON_Delete(payload);
        }

        updated_json = cJSON_Print(inputs);
        if (updated_json) {
            if (!write_file_content(cfg.output_file, updated_json)) {
                (void)write_file_content(LEGACY_OUTPUT_FILE, updated_json);
            }
            free(updated_json);
        }

        cJSON_Delete(inputs);
        sleep((unsigned int)cfg.polling_interval);
    }

    publish_status(mosq, &cfg, &topics, "offline");
    mosquitto_disconnect(mosq);
    mosquitto_loop_stop(mosq, true);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
