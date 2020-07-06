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
#include<time.h>

#include<errno.h>

#define TIMEDATE_LENGTH			32
#define PEND_SEND_RETRIES_MAX		5
#define GATEWAY_PROTOCOL_APP_KEY_SIZE	8
#define DEVICE_DATA_MAX_LENGTH		256

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

/* for multithreading impl */
void * connection_handler (void *args);

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

int main (int argc, char **argv) {
	gcom_ch_t gch;
	uint8_t buf[1024];
	uint8_t buf_len = 0;
	uint8_t payload[256];
	uint8_t payload_length = 0;
	PGresult *res;
	
	memset(&gch, 0x0, sizeof(gch));

	signal(SIGINT, ctrc_handler);

	PGconn *conn = PQconnectdb("user=vlad dbname=iotserver password=dev");
	if (PQstatus(conn) == CONNECTION_BAD) {
		fprintf(stderr,"connection to db error: %s\n", PQerrorMessage(conn));
		return EXIT_FAILURE;
	}

	if ((gch.server_desc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
		perror("socket creation error");
		return EXIT_FAILURE;
	}

	gch.server.sin_family 		= AF_INET;
	gch.server.sin_port		= htons(54345);
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
		
		if (	recv_gcom_ch(&gch, buf, &buf_len, 1024) 
			&
			packet_decode(
				gch.app_key,
				&gch.dev_id,
				&packet_type,
				&payload_length, payload,
				buf_len, buf)			)
		{
			if (packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_TIME_REQ) {
				printf("TIME REQ received\n");
				send_utc(&gch);
			} else if (packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_DATA_SEND) {
				sensor_data_t sensor_data;
				time_t t;

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
				snprintf(buf, sizeof(buf), 
					"INSERT INTO dev_%s_%d VALUES (%d, '%s', $1)", (char *)gch.app_key, gch.dev_id, t, sensor_data.timedate);
				
				const char *params[1];
				int paramlen[1];
				int paramfor[1];
				params[0] = sensor_data.data;
				paramlen[0] = sensor_data.data_length;
				paramfor[0] = 1; // binary

				res = PQexecParams(conn, buf, 1, NULL, params, paramlen, paramfor, 0);
				if (PQresultStatus(res) == PGRES_COMMAND_OK) {
					PQclear(res);

					sprintf(buf, "SELECT * FROM pend_msgs WHERE app_key='%s' and dev_id = %d and ack = False", (char *)gch.app_key, gch.dev_id);
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
			} else if (packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_PEND_REQ) {
				sprintf(buf, "SELECT * FROM pend_msgs WHERE app_key = '%s' AND dev_id = %d AND ack = False", 
						(char *)gch.app_key, gch.dev_id);
				res = PQexec(conn, buf);
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
					struct timeval tv;
					tv.tv_sec = 0;
					tv.tv_usec = 300000; // 300ms ack recv timeout
					do {
						packet_encode(
							gch.app_key,
							gch.dev_id, 
							GATEWAY_PROTOCOL_PACKET_TYPE_PEND_SEND,
							payload_length, payload,
							&buf_len, buf);

						send_gcom_ch(&gch, buf, buf_len);	
						// set timeout
						if (setsockopt(gch.server_desc, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
							perror("setsockopt error");
						}
						recv_gcom_ch(&gch, buf, &buf_len, 1024);
						if (buf_len > 9) { /* min packet size. timeout -> -1 */
							uint8_t recv_app_key[GATEWAY_PROTOCOL_APP_KEY_SIZE +1];
							uint8_t recv_dev_id = 0xFF;
							if (packet_decode(
								recv_app_key,
								&recv_dev_id, 
								&packet_type,
								&buf_len, buf,
								buf_len, buf)) 
							{
								if (!memcmp(recv_app_key, gch.app_key, GATEWAY_PROTOCOL_APP_KEY_SIZE) &&
									recv_dev_id == gch.dev_id &&
									packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_STAT &&
									buf_len == 1 &&
									buf[0] == GATEWAY_PROTOCOL_STAT_ACK)
								{
									sprintf(buf, "UPDATE pend_msgs SET ack = True WHERE app_key = '%s' AND dev_id = %d AND msg = '%s'", (char *)gch.app_key, gch.dev_id, msg_cont);
									res = PQexec(conn, buf);
									if (PQresultStatus(res) != PGRES_COMMAND_OK) {
										fprintf(stderr, "error db deleting : %s", PQerrorMessage(conn));
									}
									PQclear(res);
									received_ack = 1;
									printf("ACK received\n");
								} else {
									printf("error: packet_type = %02X, not STAT\n");
								}
							}
						}
					} while (!received_ack && pend_send_retries--);
					// cancel timeout
					tv.tv_usec = 0;	
					if (setsockopt(gch.server_desc, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
						perror("setsockopt error");
					}
				} else {
					gateway_protocol_mk_stat(
						&gch,
						GATEWAY_PROTOCOL_STAT_NACK,
						buf, &buf_len);
					
					send_gcom_ch(&gch, buf, buf_len);
					
					printf("nothing for app %s dev %d\n", (char *)gch.app_key, gch.dev_id);
				}
			} else {
				gateway_protocol_mk_stat(
					&gch,
					GATEWAY_PROTOCOL_STAT_NACK,
					buf, &buf_len);
				
				send_gcom_ch(&gch, buf, buf_len);
				
				fprintf(stderr, "packet type error : %02X\n", packet_type);
			}
		} else {
			
			fprintf(stderr, "payload decode error\n");
		}

	}

	close(gch.server_desc);
	PQfinish(conn);

	return EXIT_SUCCESS;
}

void ctrc_handler (int sig) {
	working = 0;
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
	int ret, i;
	
	if (sendto(gch->server_desc, (char *)pck, pck_size, 0, (struct sockaddr *)&gch->client, gch->sock_len) < 0) {
		perror("sendto error");
	}
	return ret;
}

int recv_gcom_ch(gcom_ch_t *gch, uint8_t *pck, uint8_t *pck_length, uint16_t pck_size) {
	int i;

	if ((*pck_length = recvfrom(gch->server_desc, (char *)pck, pck_size, MSG_WAITALL, (struct sockaddr *)&gch->client, &gch->sock_len)) < 0) {
		perror("socket receive error");
	}
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
