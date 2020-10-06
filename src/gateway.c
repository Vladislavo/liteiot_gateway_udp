#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h> //inet_addr
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>
#include <libpq-fe.h>
#include <math.h>
#include <signal.h>
#include <time.h>

#include <errno.h>

#include "gateway_protocol.h"
#include "base64.h"
#include "task_queue.h"
#include "json.h"
#include "aes.h"

#define NTHREAD_MAX			10

#define TIMEDATE_LENGTH			32
#define PEND_SEND_RETRIES_MAX		5
#define GATEWAY_PROTOCOL_APP_KEY_SIZE	8
#define DEVICE_DATA_MAX_LENGTH		256

typedef struct {
	char 		gw_id[17 +1];
	char 		gw_pass[30 +1];
	uint16_t 	gw_port;
	char 		db_type[20];
	char 		db_conn_addr[15+1];
	uint16_t 	db_conn_port;
	char 		db_conn_name[16];
	char 		db_conn_user_name[32];
	char 		db_conn_user_pass[32];
} gw_conf_t;

typedef struct {
	uint32_t utc;
	char timedate[TIMEDATE_LENGTH];

	uint8_t data[DEVICE_DATA_MAX_LENGTH];
	uint8_t data_length;
} sensor_data_t;

typedef struct {
	uint8_t app_key[GATEWAY_PROTOCOL_APP_KEY_SIZE +1];
	uint8_t dev_id;
	int server_desc;
	int client_desc;
	struct sockaddr_in server;
	struct sockaddr_in client;
	int sock_len;
} gcom_ch_t; // gateway communication channel

typedef struct {
	gcom_ch_t gch;	
	gateway_protocol_packet_type_t packet_type;
	uint8_t packet[DEVICE_DATA_MAX_LENGTH];
	uint8_t packet_length;
} gcom_ch_request_t;

static const char * gw_conf_file = "../gateway.conf";
static void process_gw_conf(json_value* value, gw_conf_t *gw_conf);
static int  read_gw_conf(const char *gw_conf_file, gw_conf_t *gw_conf);

void process_packet(void *request);

int send_gcom_ch(gcom_ch_t *gch, uint8_t *pck, uint8_t pck_size);
int recv_gcom_ch(gcom_ch_t *gch, uint8_t *pck, uint8_t *pck_length, uint16_t pck_size);

void packet_encode(
	const uint8_t *app_key,
	const uint8_t dev_id, 
	const gateway_protocol_packet_type_t p_type, 
	const uint8_t payload_length,
	const uint8_t *payload,
	uint8_t *packet_length,
	uint8_t *packet);
uint8_t packet_decode(
	uint8_t *app_key,
	uint8_t *dev_id,
	gateway_protocol_packet_type_t *ptype,
	uint8_t *payload_length,
	uint8_t *payload,
	const uint8_t packet_length,
	const uint8_t *packet);
void gateway_protocol_data_send_payload_decode(
	sensor_data_t *sensor_data, 
	const uint8_t *payload, 
	const uint8_t payload_length);

void gateway_protocol_mk_stat(
	gcom_ch_t *gch,
	gateway_protocol_stat_t stat,
	uint8_t *pck,
	uint8_t *pck_len);

void send_utc(gcom_ch_t *pch);

void ctrc_handler (int sig);
static volatile uint8_t working = 1;

pthread_mutex_t mutex;
PGconn *conn;

int main (int argc, char **argv) {
	gw_conf_t *gw_conf = (gw_conf_t *)malloc(sizeof(gw_conf_t));
	char *db_conninfo = (char *)malloc(512);
	gcom_ch_t gch;
	task_queue_t *tq;

	signal(SIGINT, ctrc_handler);
	
	read_gw_conf(gw_conf_file, gw_conf);
	snprintf(db_conninfo, 512, 
			"hostaddr=%s port=%d dbname=%s user=%s password=%s", 
			gw_conf->db_conn_addr,
			gw_conf->db_conn_port,
			gw_conf->db_conn_name,
			gw_conf->db_conn_user_name,
			gw_conf->db_conn_user_pass);
							
	conn = PQconnectdb(db_conninfo);
	free(db_conninfo);
	if (PQstatus(conn) == CONNECTION_BAD) {
		fprintf(stderr,"connection to db error: %s\n", PQerrorMessage(conn));
		free(gw_conf);
		return EXIT_FAILURE;
	}

	if ((gch.server_desc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
		perror("socket creation error");
		free(gw_conf);
		return EXIT_FAILURE;
	}

	gch.server.sin_family 		= AF_INET;
	gch.server.sin_port		= htons(gw_conf->gw_port);
	gch.server.sin_addr.s_addr 	= INADDR_ANY;

	if (bind(gch.server_desc, (struct sockaddr *) &gch.server, sizeof(gch.server)) < 0) {
		perror("binding error");
		free(gw_conf);
		return EXIT_FAILURE;
	}

	if(!(tq = task_queue_create(NTHREAD_MAX))) {
		perror("task_queue creation error");
		free(gw_conf);
		return EXIT_FAILURE;
	}

	pthread_mutex_init(&mutex, NULL);
	
	while (working) {
		gcom_ch_request_t *req = (gcom_ch_request_t *)malloc(sizeof(gcom_ch_request_t));
		memset(req, 0x0, sizeof(gcom_ch_request_t));
		memcpy(&req->gch, &gch, sizeof(gcom_ch_t));
	
		printf("listenninig...\n");
		req->gch.sock_len = sizeof(req->gch.client);
		
		if (recv_gcom_ch(&req->gch, req->packet, &req->packet_length, DEVICE_DATA_MAX_LENGTH)) {
			task_queue_enqueue(tq, process_packet, req);
		} else {
			fprintf(stderr, "packet receive error\n");
		}
	}

	free(gw_conf);
	pthread_mutex_destroy(&mutex);
	close(gch.server_desc);
	PQfinish(conn);

	return EXIT_SUCCESS;
}

void ctrc_handler (int sig) {
	working = 0;
}

void process_packet(void *request) {
	gcom_ch_request_t *req = (gcom_ch_request_t *)request;
	uint8_t payload[DEVICE_DATA_MAX_LENGTH];
	uint8_t payload_length;	
	PGresult *res;

	if (packet_decode(
		req->gch.app_key,
		&(req->gch.dev_id),
		&(req->packet_type),
		&payload_length, payload,
		req->packet_length, req->packet))
	{
		if (req->packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_TIME_REQ) {
			printf("TIME REQ received\n");
			send_utc(&(req->gch));
		} else if (req->packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_DATA_SEND) {
			sensor_data_t sensor_data;
			time_t t;
			// DEVICE_DATA_MAX_LENGTH*2 {hex} + 150
			char db_query[662];

			printf("DATA SEND received\n");
			gateway_protocol_data_send_payload_decode(&sensor_data, payload, payload_length);
			
			if (sensor_data.utc == 0) {
				struct timeval tv;
				gettimeofday(&tv, NULL);
				t = tv.tv_sec;
			} else {
				t = sensor_data.utc;
			}
			
			strftime(sensor_data.timedate, TIMEDATE_LENGTH, "%d/%m/%Y %H:%M:%S", localtime(&t));
			snprintf(db_query, sizeof(db_query), 
				"INSERT INTO dev_%s_%d VALUES (%lu, '%s', $1)", 
				(char *)req->gch.app_key, req->gch.dev_id, t, sensor_data.timedate
			);
			
			const char *params[1];
			int paramslen[1];
			int paramsfor[1];
			params[0] = sensor_data.data;
			paramslen[0] = sensor_data.data_length;
			paramsfor[0] = 1; // format - binary

			pthread_mutex_lock(&mutex);
			res = PQexecParams(conn, db_query, 1, NULL, params, paramslen, paramsfor, 0);
			pthread_mutex_unlock(&mutex);

			if (PQresultStatus(res) == PGRES_COMMAND_OK) {
				PQclear(res);

				snprintf(db_query, sizeof(db_query),
					 "SELECT * FROM pend_msgs WHERE app_key='%s' and dev_id = %d and ack = False", 
					(char *)req->gch.app_key, req->gch.dev_id
				);
				
				pthread_mutex_lock(&mutex);
				res = PQexec(conn, db_query);
				pthread_mutex_unlock(&mutex);
				
				if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res)) {
					gateway_protocol_mk_stat(
						&(req->gch), 
						GATEWAY_PROTOCOL_STAT_ACK_PEND,
						req->packet, &(req->packet_length));
					printf("ACK_PEND prepared\n");
				} else {
					gateway_protocol_mk_stat(
						&(req->gch), 
						GATEWAY_PROTOCOL_STAT_ACK,
						req->packet, &(req->packet_length));
					printf("ACK prepared\n");
				}
				
				send_gcom_ch(&(req->gch), req->packet, req->packet_length);
			} else {
				fprintf(stderr, "database error : %s\n", PQerrorMessage(conn));
			}
			PQclear(res);
		} else if (req->packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_PEND_REQ) {
			char db_query[200];
			snprintf(db_query, sizeof(db_query),
				 "SELECT * FROM pend_msgs WHERE app_key = '%s' AND dev_id = %d AND ack = False", 
				(char *)req->gch.app_key, req->gch.dev_id
			);
			pthread_mutex_lock(&mutex);
			res = PQexec(conn, db_query);
			pthread_mutex_unlock(&mutex);
			
			if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res)) {
				char msg_cont[150];
				strncpy(msg_cont, PQgetvalue(res, 0, 2), sizeof(msg_cont));
				printf("PEND_SEND prepared : %s\n", msg_cont);
				PQclear(res);
			
				base64_decode(msg_cont, strlen(msg_cont)-1, payload);
				payload_length = BASE64_DECODE_OUT_SIZE(strlen(msg_cont));
				printf("prepared to send %d bytes : %s\n", payload_length, payload);
				
				// send the msg until ack is received
				uint8_t received_ack = 0;
				uint8_t pend_send_retries = PEND_SEND_RETRIES_MAX;
				packet_encode(
					req->gch.app_key,
					req->gch.dev_id, 
					GATEWAY_PROTOCOL_PACKET_TYPE_PEND_SEND,
					payload_length, payload,
					&(req->packet_length), req->packet);
				do {
					send_gcom_ch(&(req->gch), req->packet, req->packet_length);
					
					// 300 ms
					usleep(300000);

					pthread_mutex_lock(&mutex);
					res = PQexec(conn, db_query);
					pthread_mutex_unlock(&mutex);
					
					if (PQresultStatus(res) == PGRES_TUPLES_OK) {
						if (!PQntuples(res) || strcmp(PQgetvalue(res, 0, 2), msg_cont)) {
							received_ack = 1;
						}
					}
					PQclear(res);
					printf("received_ack = %d, retries = %d\n", received_ack, pend_send_retries);
				} while (!received_ack && pend_send_retries--);
			} else {
				gateway_protocol_mk_stat(
					&(req->gch),
					GATEWAY_PROTOCOL_STAT_NACK,
					req->packet, &(req->packet_length));
				
				send_gcom_ch(&(req->gch), req->packet, req->packet_length);
				
				printf("nothing for app %s dev %d\n", (char *)req->gch.app_key, req->gch.dev_id);
			}
		} else if (req->packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_STAT) {
			// TODO change to ACK_PEND = 0x01
			if (payload[0] == 0x00) {
				char db_query[200];
				snprintf(db_query, sizeof(db_query),
					 "SELECT * FROM pend_msgs WHERE app_key = '%s' AND dev_id = %d AND ack = False", 
					(char *)req->gch.app_key, req->gch.dev_id
				);
				pthread_mutex_lock(&mutex);
				res = PQexec(conn, db_query);
				pthread_mutex_unlock(&mutex);
				if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res)) {
					snprintf(db_query, sizeof(db_query),
						"UPDATE pend_msgs SET ack = True WHERE app_key = '%s' AND dev_id = %d AND msg = '%s'",
						(char *)req->gch.app_key, req->gch.dev_id, PQgetvalue(res, 0, 2)
					);
					PQclear(res);
					pthread_mutex_lock(&mutex);
					res = PQexec(conn, db_query);
					pthread_mutex_unlock(&mutex);
					if (PQresultStatus(res) == PGRES_COMMAND_OK) {
						printf("pend_msgs updated\n");
					} else {
						fprintf(stderr, "database error : %s\n", PQerrorMessage(conn));
					}
				}
				PQclear(res);
			}
		} else {
			gateway_protocol_mk_stat(
				&(req->gch),
				GATEWAY_PROTOCOL_STAT_NACK,
				req->packet, &(req->packet_length));
			
			send_gcom_ch(&(req->gch), req->packet, req->packet_length);
				
			fprintf(stderr, "packet type error : %02X\n", req->packet_type);
		}
	} else {
		fprintf(stderr, "payload decode error\n");
	}
	
	free(req);
}

void gateway_protocol_data_send_payload_decode(
	sensor_data_t *sensor_data, 
	const uint8_t *payload, 
	const uint8_t payload_length) 
{
	uint8_t p_len = 0;

	memcpy(&sensor_data->utc, &payload[p_len], sizeof(sensor_data->utc));
	p_len += sizeof(sensor_data->utc);

	memcpy(sensor_data->data, &payload[p_len], payload_length - p_len);
	sensor_data->data_length = payload_length - p_len;
}

void packet_encode(
	const uint8_t *app_key,
	const uint8_t dev_id, 
	const gateway_protocol_packet_type_t p_type, 
	const uint8_t payload_length,
	const uint8_t *payload,
	uint8_t *packet_length,
	uint8_t *packet) 
{
	*packet_length = 0;
	
	memcpy(&packet[*packet_length], app_key, GATEWAY_PROTOCOL_APP_KEY_SIZE);
	*packet_length += GATEWAY_PROTOCOL_APP_KEY_SIZE;

	packet[*packet_length] = dev_id;
	(*packet_length)++;

	packet[*packet_length] = p_type;
	(*packet_length)++;

	packet[*packet_length] = payload_length;
	(*packet_length)++;

	memcpy(&packet[*packet_length], payload, payload_length);
	*packet_length += payload_length;
}

uint8_t packet_decode(
	uint8_t *app_key,
	uint8_t *dev_id,
	gateway_protocol_packet_type_t *ptype,
	uint8_t *payload_length,
	uint8_t *payload,
	const uint8_t packet_length,
	const uint8_t *packet)
{
	uint8_t p_len = 0;
	int i;

	memcpy(app_key, &packet[p_len], GATEWAY_PROTOCOL_APP_KEY_SIZE);
	p_len += GATEWAY_PROTOCOL_APP_KEY_SIZE;
	
	app_key[GATEWAY_PROTOCOL_APP_KEY_SIZE] = '\0';

	*dev_id = packet[p_len];
	p_len++;

	*ptype = (gateway_protocol_packet_type_t) packet[p_len];
	p_len++;

	*payload_length = packet[p_len];
	p_len++;

	memcpy(payload, &packet[p_len], *payload_length);
	p_len += *payload_length;

	printf("payload_length = %d , calc = %d, recv = %d\n", *payload_length, p_len, packet_length);

	return p_len == packet_length;
}

void gateway_protocol_mk_stat(
	gcom_ch_t *gch,
	gateway_protocol_stat_t stat,
	uint8_t *pck,
	uint8_t *pck_len)
{
	packet_encode(
		gch->app_key,
		gch->dev_id,
		GATEWAY_PROTOCOL_PACKET_TYPE_STAT,
		1, (uint8_t *)&stat,
		pck_len, pck);
}



void send_utc(gcom_ch_t *gch) {
	uint8_t buf[50];
	uint8_t buf_len = 0;
	struct timeval tv;
				
	gettimeofday(&tv, NULL);
				
	packet_encode (
		gch->app_key,
		gch->dev_id,
		GATEWAY_PROTOCOL_PACKET_TYPE_TIME_SEND,
		sizeof(uint32_t), (uint8_t *)&tv.tv_sec,
		&buf_len, buf
	);
					
	send_gcom_ch(gch, buf, buf_len);
}

int send_gcom_ch(gcom_ch_t *gch, uint8_t *pck, uint8_t pck_size) {
	int ret;
	
	if ((ret = sendto(gch->server_desc, (char *)pck, pck_size, 0, (struct sockaddr *)&gch->client, gch->sock_len)) < 0) {
		perror("sendto error");
	}

	return ret;
}

int recv_gcom_ch(gcom_ch_t *gch, uint8_t *pck, uint8_t *pck_length, uint16_t pck_size) {
	int ret, i;
	if ((ret = recvfrom(gch->server_desc, (char *)pck, pck_size, MSG_WAITALL, (struct sockaddr *)&gch->client, &gch->sock_len)) < 0) {
		perror("socket receive error");
	} else {
		*pck_length = ret;
		
	}

	//uint8_t decyphered[160];
	uint8_t skey[16] = { 0x73, 0x60, 0xe4, 0x5e, 0x09, 0xa0, 0x5e, 0xab, 0xb1, 0x69, 0xdf, 0x1f, 0x8c, 0x80, 0x72, 0xd5 };
	struct AES_ctx ctx;
	AES_init_ctx(&ctx, skey);

	printf("%d\n", ret);
	for (i = 0; i < *pck_length; i++) {
		printf("%02X : ", pck[i]);
	}
	printf("\n");

	for (i = 0; i < *pck_length-9 ; i+= 16)
		AES_ECB_decrypt(&ctx, &pck[9+i]);
		
	for (i = 0; i < *pck_length; i++) {
		printf("%02X : ", pck[i]);
	}
	printf("\n");
	

	return ret;
}


static void process_gw_conf(json_value* value, gw_conf_t *gw_conf) {
	json_value *pv = value;

	strncpy(gw_conf->gw_id, pv->u.object.values[0].value->u.string.ptr, sizeof(gw_conf->gw_id));
	strncpy(gw_conf->gw_pass, pv->u.object.values[1].value->u.string.ptr, sizeof(gw_conf->gw_pass));
	gw_conf->gw_port = pv->u.object.values[2].value->u.integer;
	strncpy(gw_conf->db_type, pv->u.object.values[3].value->u.string.ptr, sizeof(gw_conf->db_type));
	
	pv = pv->u.object.values[4].value;
	
	strncpy(gw_conf->db_conn_addr, pv->u.object.values[0].value->u.string.ptr, sizeof(gw_conf->db_conn_addr));
	gw_conf->db_conn_port = pv->u.object.values[1].value->u.integer;
	strncpy(gw_conf->db_conn_name, pv->u.object.values[2].value->u.string.ptr, sizeof(gw_conf->db_conn_name));
	strncpy(gw_conf->db_conn_user_name, pv->u.object.values[3].value->u.string.ptr, sizeof(gw_conf->db_conn_user_name));
	strncpy(gw_conf->db_conn_user_pass, pv->u.object.values[4].value->u.string.ptr, sizeof(gw_conf->db_conn_user_pass));
}

static int read_gw_conf(const char *gw_conf_file, gw_conf_t *gw_conf) {
	struct stat filestatus;
	FILE *fp;
	char *file_contents;
	json_char *json;
	json_value *jvalue;

	if (stat(gw_conf_file, &filestatus)) {
		fprintf(stderr, "File %s not found.", gw_conf_file);
		return 1;
	}
	file_contents = (char *)malloc(filestatus.st_size);
	if (!file_contents) {
		fprintf(stderr, "Memory error allocating %d bytes.", filestatus.st_size);
		return 1;
	}
	fp = fopen(gw_conf_file, "rt");
	if (!fp) {
		fprintf(stderr, "Unable to open %s.", gw_conf_file);
		fclose(fp);
		free(file_contents);
		return 1;
	}
	if (fread(file_contents, filestatus.st_size, 1, fp) != 1) {
		fprintf(stderr, "Unable to read %s.", gw_conf_file);
		fclose(fp);
		free(file_contents);
		return 1;
	}
	fclose(fp);
	
	printf("%s\n", file_contents);
	
	json = (json_char *)file_contents;
	jvalue = json_parse(json, filestatus.st_size);
	if (!jvalue) {
		perror("Unable to parse json.");
		free(file_contents);
		return 1;
	}
	
	process_gw_conf(jvalue, gw_conf);

	json_value_free(jvalue);
	free(file_contents);
	
	return 0;
}
