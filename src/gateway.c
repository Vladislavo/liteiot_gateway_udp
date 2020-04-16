#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<sys/socket.h>
#include<arpa/inet.h> //inet_addr
#include<unistd.h>
#include<stdint.h>
#include<pthread.h>
#include"gateway_protocol.h"
#include<sys/time.h>
#include<libpq-fe.h>
#include"base64.h"
#include<math.h>
#include<signal.h>

#define TIMEDATE_LENGTH		32
#define PEND_SEND_RETRIES_MAX	5

typedef struct {
	uint32_t utc;
	char timedate[TIMEDATE_LENGTH];

	float dht22_t_esp;
	float dht22_h_esp;
	float sht85_t_esp;
	float sht85_h_esp;
	float hih8121_t_esp;
	float hih8121_h_esp;
	float tmp36_0_esp;
	float tmp36_1_esp;
	float tmp36_2_esp;
	float hih4030_esp;
	float hh10d_esp;

	float dht22_t_mkr;
	float dht22_h_mkr;
	float sht85_t_mkr;
	float sht85_h_mkr;
	float hih8121_t_mkr;
	float hih8121_h_mkr;
	float hh10d_mkr;

	float dht22_t_wis;
	float dht22_h_wis;
	float sht85_t_wis;
	float sht85_h_wis;
	float hih8121_t_wis;
	float hih8121_h_wis;
	float tmp102_wis;
	float hh10d_wis;
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

int send_gcom_ch(gcom_ch_t *gch, uint8_t *pck, uint8_t pck_size);
int recv_gcom_ch(gcom_ch_t *gch, uint8_t *pck, uint8_t *pck_length, uint16_t pck_size);

void * connection_handler (void *args);
uint8_t gateway_protocol_data_send_payload_decode(sensor_data_t *sensor_data, const uint8_t *payload, const uint8_t payload_length);
void prepare_di_query(sensor_data_t *sensor_data, char *q, uint16_t q_size); // data insert
void filter_query(char *com);
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

void gateway_protocol_mk_stat(
	gcom_ch_t *gch,
	gateway_protocol_stat_t stat,
	uint8_t *pck,
	uint8_t *pck_len);

void send_utc(gcom_ch_t *pch);

void ctrc_handler (int sig);
static volatile uint8_t working = 1;

int main (int argc, char **argv) {
	gcom_ch_t gch;
	uint8_t buf[1024];
	uint8_t buf_len = 0;
	uint8_t payload[256];
	uint8_t payload_length = 0;
	PGresult *res;
	
	signal(SIGINT, ctrc_handler);

	PGconn *conn = PQconnectdb("user=pi dbname=gateway");
	if (PQstatus(conn) == CONNECTION_BAD) {
		printf("connection to db error: %s\n", PQerrorMessage(conn));
		return EXIT_FAILURE;
	}

	if ((gch.server_desc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
		perror("socket creation error");
		return EXIT_FAILURE;
	}

	gch.server.sin_family 		= AF_INET;
	gch.server.sin_port		= htons(9043);
	gch.server.sin_addr.s_addr 	= INADDR_ANY;

	if (bind(gch.server_desc, (struct sockaddr *) &gch.server, sizeof(gch.server)) < 0) {
		perror("binding error");
		return EXIT_FAILURE;
	}

	gateway_protocol_packet_type_t packet_type;
	
	while (working) {
		buf_len = 0;
		printf("listenninig...\n");
		gch.sock_len = sizeof(gch.client);
		
		recv_gcom_ch(&gch, buf, &buf_len, 1024);

		if (packet_decode(
			gch.app_key,
			&gch.dev_id,
			&packet_type,
			&payload_length, payload,
			buf_len, buf))
		{
			if (packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_TIME_REQ) {
				printf("TIME REQ received\n");
				send_utc(&gch);
			} else if (packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_DATA_SEND) {
				sensor_data_t sensor_data;
				printf("DATA SEND received\n");
		        	if (gateway_protocol_data_send_payload_decode(&sensor_data, payload, payload_length)) {
					prepare_di_query(&gch, &sensor_data, buf, sizeof(buf));
					filter_query(buf);
					printf("%s\n", buf);
					res = PQexec(conn, buf);
					if (PQresultStatus(res) == PGRES_COMMAND_OK) {
						PQclear(res);

						sprintf(buf, "SELECT * FROM pend_msgs WHERE dev_id = %d", gch.dev_id);
						res = PQexec(conn, buf);
						if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res)) {
							gateway_protocol_mk_stat(
								&gch, 
								GATEWAY_PROTOCOL_STAT_ACK_PEND,
								buf, &buf_len);
							printf("ACK_PEND prepared\n");
						} else {
							gateway_protocol_mk_stat(
								&gch, 
								GATEWAY_PROTOCOL_STAT_ACK,
								buf, &buf_len);
							printf("ACK prepared\n");
						}
						
						send_gcom_ch(&gch, buf, buf_len);
					} else {
						fprintf(stderr, "database error : %s\n", PQerrorMessage(conn));
					}
					PQclear(res);
				} else {
					gateway_protocol_mk_stat(
						&gch,
						GATEWAY_PROTOCOL_STAT_NACK,
						buf, &buf_len);
					
					send_gcom_ch(&gch, buf, buf_len);
					
					fprintf(stderr, "payload decode error\n");
				}
			} else if (packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_PEND_REQ) {
				sprintf(buf, "SELECT * FROM pend_msgs WHERE app_key = %s AND dev_id = %d", 
						(char *)gch.app_key, gch.dev_id);
				res = PQexec(conn, buf);
				if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res)) {
					char msg_cont[150];
					strncpy(msg_cont, PQgetvalue(res, 0, 1), sizeof(msg_cont));
					printf("PEND_SEND prepared : %s\n", PQgetvalue(res, 0, 1));

					base64_decode(PQgetvalue(res, 0, 1), strlen(PQgetvalue(res, 0, 1)), payload);
					payload_length = BASE64_DECODE_OUT_SIZE(strlen(PQgetvalue(res, 0, 1)));
					PQclear(res);
					printf("prepared to send %d bytes : %s\n", payload_length, (char *)payload);
					
					// send the msg until ack is received
					uint8_t received_ack = 0;
					uint8_t pend_send_retries = PEND_SEND_RETRIES_MAX;
					do {
						packet_encode(
							gch.app_key,
							gch.dev_id, 
							GATEWAY_PROTOCOL_PACKET_TYPE_PEND_SEND,
							payload_length, payload,
							&buf_len, buf);
	
						send_gcom_ch(&gch, buf, buf_len);
						recv_gcom_ch(&gch, buf, &buf_len, 1024);

						uint8_t recv_app_key[GATEWAY_PROTOCOL_APP_KEY_SIZE];
						uint8_t recv_dev_id = 0xFF;
						if (packet_decode(
							recv_app_key,
							&recv_dev_id, 
							&packet_type,
							&buf_len, buf,
							buf_len, buf)) 
						{
							if (!memcpy(recv_app_key, gch.app_key, GATEWAY_PROTOCOL_APP_KEY_SIZE) &&
								recv_dev_id == gch.dev_id &&
								packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_STAT &&
								buf_len == 1 &&
								buf[0] == GATEWAY_PROTOCOL_STAT_ACK)
							{
								sprintf(buf, "DELETE FROM pend_msgs WHERE app_key = %s AND dev_id = %d AND msg = '%s'", (char *)gch.app_key, gch.dev_id, msg_cont);
								printf("%s", buf);
								res = PQexec(conn, buf);
								if (PQresultStatus(res) != PGRES_COMMAND_OK) {
									fprintf(stderr, "error db deleting : %s", PQerrorMessage(conn));
								}
								PQclear(res);
								received_ack = 1;
								printf("ACK received\n");
							}
						}
					} while (!received_ack && pend_send_retries--);
				}
			} else {
				fprintf(stderr, "packet type error : %02X\n", packet_type);
			}
		} else {
			fprintf(stderr, "packet decode error (type : %02X, pck_len = %d, pay_len = %d\n", 
					packet_type, buf_len, payload_length);
		}
	}

	close(gch.server_desc);
	PQfinish(conn);

	return EXIT_SUCCESS;
}

void ctrc_handler (int sig) {
	working = 0;
}

uint8_t gateway_protocol_data_send_payload_decode(
	sensor_data_t *sensor_data, 
	const uint8_t *payload, 
	const uint8_t payload_length) 
{
	uint8_t p_len = 0;

	memcpy(&sensor_data->utc, &payload[p_len], sizeof(sensor_data->utc));
	p_len += sizeof(sensor_data->utc);

	memcpy(&sensor_data->timedate, &payload[p_len], sizeof(sensor_data->timedate));
	//p_len += sizeof(sensor_data->timedate);
	p_len += TIMEDATE_LENGTH;


	memcpy(&sensor_data->dht22_t_esp, &payload[p_len], sizeof(sensor_data->dht22_t_esp));
	p_len += sizeof(sensor_data->dht22_t_esp);

	memcpy(&sensor_data->dht22_h_esp, &payload[p_len], sizeof(sensor_data->dht22_h_esp));
	p_len += sizeof(sensor_data->dht22_h_esp);

	memcpy(&sensor_data->sht85_t_esp, &payload[p_len], sizeof(sensor_data->sht85_t_esp));
	p_len += sizeof(sensor_data->sht85_t_esp);

	memcpy(&sensor_data->sht85_h_esp, &payload[p_len], sizeof(sensor_data->sht85_h_esp));
	p_len += sizeof(sensor_data->sht85_h_esp);

	memcpy(&sensor_data->hih8121_t_esp, &payload[p_len], sizeof(sensor_data->hih8121_t_esp));
	p_len += sizeof(sensor_data->hih8121_t_esp);

	memcpy(&sensor_data->hih8121_h_esp, &payload[p_len], sizeof(sensor_data->hih8121_h_esp));
	p_len += sizeof(sensor_data->hih8121_h_esp);

	memcpy(&sensor_data->tmp36_0_esp, &payload[p_len], sizeof(sensor_data->tmp36_0_esp));
	p_len += sizeof(sensor_data->tmp36_0_esp);

	memcpy(&sensor_data->tmp36_1_esp, &payload[p_len], sizeof(sensor_data->tmp36_1_esp));
	p_len += sizeof(sensor_data->tmp36_1_esp);

	memcpy(&sensor_data->tmp36_2_esp, &payload[p_len], sizeof(sensor_data->tmp36_2_esp));
	p_len += sizeof(sensor_data->tmp36_2_esp);

	memcpy(&sensor_data->hih4030_esp, &payload[p_len], sizeof(sensor_data->hih4030_esp));
	p_len += sizeof(sensor_data->hih4030_esp);

	memcpy(&sensor_data->hh10d_esp, &payload[p_len], sizeof(sensor_data->hh10d_esp));
	p_len += sizeof(sensor_data->hh10d_esp);


	memcpy(&sensor_data->dht22_t_mkr, &payload[p_len], sizeof(sensor_data->dht22_t_mkr));
	p_len += sizeof(sensor_data->dht22_t_mkr);

	memcpy(&sensor_data->dht22_h_mkr, &payload[p_len], sizeof(sensor_data->dht22_h_mkr));
	p_len += sizeof(sensor_data->dht22_h_mkr);

	memcpy(&sensor_data->sht85_t_mkr, &payload[p_len], sizeof(sensor_data->sht85_t_mkr));
	p_len += sizeof(sensor_data->sht85_t_mkr);

	memcpy(&sensor_data->sht85_h_mkr, &payload[p_len], sizeof(sensor_data->sht85_h_mkr));
	p_len += sizeof(sensor_data->sht85_h_esp);

	memcpy(&sensor_data->hih8121_t_mkr, &payload[p_len], sizeof(sensor_data->hih8121_t_mkr));
	p_len += sizeof(sensor_data->hih8121_t_mkr);

	memcpy(&sensor_data->hih8121_h_mkr, &payload[p_len], sizeof(sensor_data->hih8121_h_mkr));
	p_len += sizeof(sensor_data->hih8121_h_mkr);

	memcpy(&sensor_data->hh10d_mkr, &payload[p_len], sizeof(sensor_data->hh10d_mkr));
	p_len += sizeof(sensor_data->hh10d_mkr);

	
	memcpy(&sensor_data->dht22_t_wis, &payload[p_len], sizeof(sensor_data->dht22_t_wis));
	p_len += sizeof(sensor_data->dht22_t_wis);

	memcpy(&sensor_data->dht22_h_wis, &payload[p_len], sizeof(sensor_data->dht22_h_wis));
	p_len += sizeof(sensor_data->dht22_h_wis);

	memcpy(&sensor_data->sht85_t_wis, &payload[p_len], sizeof(sensor_data->sht85_t_wis));
	p_len += sizeof(sensor_data->sht85_t_wis);

	memcpy(&sensor_data->sht85_h_wis, &payload[p_len], sizeof(sensor_data->sht85_h_wis));
	p_len += sizeof(sensor_data->sht85_h_wis);

	memcpy(&sensor_data->hih8121_t_wis, &payload[p_len], sizeof(sensor_data->hih8121_t_wis));
	p_len += sizeof(sensor_data->hih8121_t_wis);

	memcpy(&sensor_data->hih8121_h_wis, &payload[p_len], sizeof(sensor_data->hih8121_h_wis));
	p_len += sizeof(sensor_data->hih8121_h_wis);

	memcpy(&sensor_data->tmp102_wis, &payload[p_len], sizeof(sensor_data->tmp102_wis));
	p_len += sizeof(sensor_data->tmp102_wis);

	memcpy(&sensor_data->hh10d_wis, &payload[p_len], sizeof(sensor_data->hh10d_wis));
	p_len += sizeof(sensor_data->hh10d_wis);

	printf("p_len = %d, payload_length = %d\n", p_len, payload_length);
				

	return (p_len == payload_length);
}

void prepare_di_query(gcom_ch_t *gch, sensor_data_t *sensor_data, char *q, uint16_t q_size) {
	snprintf(q, q_size, 
		"INSERT INTO dev_%s_%d ("
			"utc, timedate, "
			"dht22_t_esp, dht22_h_esp, "
			"sht85_t_esp, sht85_h_esp, "
			"hih8121_t_esp, hih8121_h_esp, "
			"tmp36_0_esp, tmp36_1_esp, tmp36_2_esp, "
			"hih4030_esp, "
			"hh10d_esp, "
			"dht22_t_mkr, dht22_h_mkr, "
			"sht85_t_mkr, sht85_h_mkr, "
			"hih8121_t_mkr, hih8121_h_mkr, "
			"hh10d_mkr, "
			"dht22_t_wis, dht22_h_wis, "
			"sht85_t_wis, sht85_h_wis, "
			"hih8121_t_wis, hih8121_h_wis, "
			"tmp102_wis, "
			"hh10d_wis) "
		"VALUES("
			"%lu, '%s', "
			"%.2f, %.2f, "  // esp
			"%.2f, %.2f, "
			"%.2f, %.2f, "
			"%.2f, %.2f, %.2f, "
			"%.2f, "
			"%.2f, "
			"%.2f, %.2f, "  // mkr
			"%.2f, %.2f, "
			"%.2f, %.2f, "
			"%.2f, "
			"%.2f, %.2f, "  // wis
			"%.2f, %.2f, "
			"%.2f, %.2f, "
			"%.2f, "
			"%.2f)",
			(char *)gch->app_key, gch->dev_id,
			sensor_data->utc, sensor_data->timedate,
			sensor_data->dht22_t_esp, sensor_data->dht22_h_esp,
			sensor_data->sht85_t_esp, sensor_data->sht85_h_esp,
			sensor_data->hih8121_t_esp, sensor_data->hih8121_h_esp,
			sensor_data->tmp36_0_esp, sensor_data->tmp36_1_esp, sensor_data->tmp36_2_esp,
			sensor_data->hih4030_esp,
			sensor_data->hh10d_esp,
			sensor_data->dht22_t_mkr, sensor_data->dht22_h_mkr,
			sensor_data->sht85_t_mkr, sensor_data->sht85_h_mkr,
			sensor_data->hih8121_t_mkr, sensor_data->hih8121_h_mkr,
			sensor_data->hh10d_mkr,
			sensor_data->dht22_t_wis, sensor_data->dht22_h_wis,
			sensor_data->sht85_t_wis, sensor_data->sht85_h_wis,
			sensor_data->hih8121_t_wis, sensor_data->hih8121_h_wis,
			sensor_data->tmp102_wis,
			sensor_data->hh10d_wis
	);
}

void filter_query(char *q) {
	char *pchr;
	const char nanstr[] = "'NaN'";

	while((pchr = strstr(q, "nan"))) {
		memmove(&pchr[5], &pchr[3], strlen(pchr)+1);
		memcpy(pchr, nanstr, sizeof(nanstr)-1);
	}
	while(pchr = strchr(q, '-')) {
		memmove(pchr, &pchr[1], strlen(pchr));
	}
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
	
	memcpy(app_key, &packet[p_len], GATEWAY_PROTOCOL_APP_KEY_SIZE);
	p_len += GATEWAY_PROTOCOL_APP_KEY_SIZE;

	*dev_id = packet[p_len];
	p_len++;

	*ptype = (gateway_protocol_packet_type_t) packet[p_len];
	p_len++;

	*payload_length = packet[p_len];
	p_len++;

	memcpy(payload, &packet[p_len], *payload_length);
	p_len += *payload_length;

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
		1, &stat,
		pck_len, pck);
}



void send_utc(gcom_ch_t *gch) {
	uint8_t buf[10];
	uint8_t buf_len = 0;
	struct timeval tv;
				
	gettimeofday(&tv, NULL);
				
	packet_encode (
		gch->app_key,
		gch->dev_id,
		GATEWAY_PROTOCOL_PACKET_TYPE_TIME_SEND,
		sizeof(tv.tv_sec), (uint8_t *)&tv.tv_sec,
		&buf_len, buf
	);
					
	send_gcom_ch(gch, buf, buf_len);
}

int send_gcom_ch(gcom_ch_t *gch, uint8_t *pck, uint8_t pck_size) {
	int ret;
	
	for(uint8_t i = 0; i < pck_size; i++) {
		printf("%02X : ", pck[i]);
	}
	printf("\n");
	
	if (sendto(gch->server_desc, (char *)pck, pck_size, 0, (struct sockaddr *)&gch->client, gch->sock_len) < 0) {
		perror("sendto error");
	}
	return ret;
}

int recv_gcom_ch(gcom_ch_t *gch, uint8_t *pck, uint8_t *pck_length, uint16_t pck_size) {
	if ((*pck_length = recvfrom(gch->server_desc, (char *)pck, pck_size, MSG_WAITALL, (struct sockaddr *)&gch->client, &gch->sock_len)) < 0) {
		perror("socket receive error");
	}

	for(uint8_t i = 0; i < *pck_length; i++) {
		printf("%02X : ", pck[i]);
	}
	printf("\n");
}
/* connection handler for multithreading version */
#ifdef MULTITHREADING_VER
void *connection_handler(void *args) {
	int client_desc = *(int *)args;
	
	uint8_t buf[128] = "";
	uint8_t buf_len = 0;
	uint8_t payload[128];
	uint8_t payload_length = 0;

	if ((buf_len = recv(client_desc, buf, sizeof(buf), 0)) > 0) {
		uint8_t dev_id = 0xFF;
		gateway_protocol_packet_type_t packet_type;
		
		if (gateway_protocol_packet_decode(	
					&dev_id,
					&packet_type,
					&payload_length, payload,
					buf_len, buf))
		{
			if (packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_TIME_REQ) {
				printf("TIME REQ received\n");
				struct timeval tv;
				buf_len = 0;
				
				buf[0] = dev_id;
				buf_len++;

				buf[1] = GATEWAY_PROTOCOL_PACKET_TYPE_TIME_SEND;
				buf_len++;

				gettimeofday(&tv, NULL);
				memcpy(&buf[buf_len], &tv.tv_sec, sizeof(uint32_t));
				buf_len += sizeof(uint32_t);

				write(client_desc, buf, buf_len);
			} else {
				perror("packet type error");
			}
		} else {
			perror("packet decode error");
		}
	}

}
#endif
