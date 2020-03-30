#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<sys/socket.h>
#include<arpa/inet.h> //inet_addr
#include<unistd.h>
#include<stdint.h>

#include<pthread.h>

#include<gateway_protocol.h>

#include<sys/time.h>

#include<libpq-fe.h>

#define TIMEDATE_LENGTH		32

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

void * connection_handler (void *args);
uint8_t gateway_protocol_data_send_payload_decode(sensor_data_t *sensor_data, const uint8_t *payload, const uint8_t payload_length);

int main (int argc, char **argv) {
	int server_desc, client_desc;
       	struct sockaddr_in server, client;
	socklen_t client_socklen;
	uint8_t buf[1024];
	uint8_t buf_len = 0;
	uint8_t payload[128];
	uint8_t payload_length = 0;
	int sock_len;

	PGconn *conn = PQconnectdb("user=root dbname=gateway");
	if (PQstatus(conn) == CONNECTION_BAD) {
		printf("connection to db error: %s\n", PQerrorMessage(conn));
		return EXIT_FAILURE;
	}

	if ((server_desc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
		perror("socket creation error");
		return EXIT_FAILURE;
	}

	server.sin_family 	= AF_INET;
	server.sin_port		= htons(9043);
	server.sin_addr.s_addr 	= INADDR_ANY;

	if (bind(server_desc, (struct sockaddr *) &server, sizeof(server)) < 0) {
		perror("binding error");
		return EXIT_FAILURE;
	}

	while (1) {
		buf_len = 0;
		printf("listenninig...\n");
		if ((buf_len = recvfrom(server_desc, (char *)buf, 1024, MSG_WAITALL, (struct sockaddr *)&client, &sock_len)) < 0) {
			perror("socket receive error");
		}

		printf("packet received!\n");

		for (uint8_t i = 0; i < buf_len; i++) {
			printf("%02X :", buf[i]);
		}
		printf("\n");

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
			} else if (packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_DATA_SEND) {
				sensor_data_t sensor_data;
				printf("DATA SEND received\n");
		        	if (gateway_protocol_data_send_payload_decode(&sensor_data, payload, payload_length)) {
					PGresult *res;
					snprintf(buf, sizeof(buf), "INSERT INTO esp32 VALUES("
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
						sensor_data.utc, sensor_data.timedate,
						sensor_data.dht22_t_esp, sensor_data.dht22_h_esp,
						sensor_data.sht85_t_esp, sensor_data.sht85_h_esp,
						sensor_data.hih8121_t_esp, sensor_data.hih8121_h_esp,
						sensor_data.tmp36_0_esp, sensor_data.tmp36_1_esp, sensor_data.tmp36_2_esp,
						sensor_data.hih4030_esp,
						sensor_data.hh10d_esp,
						sensor_data.dht22_t_mkr, sensor_data.dht22_h_mkr,
						sensor_data.sht85_t_mkr, sensor_data.sht85_h_mkr,
						sensor_data.hih8121_t_mkr, sensor_data.hih8121_h_mkr,
						sensor_data.hh10d_mkr,
						sensor_data.dht22_t_wis, sensor_data.dht22_h_wis,
						sensor_data.sht85_t_wis, sensor_data.sht85_h_wis,
						sensor_data.hih8121_t_wis, sensor_data.hih8121_h_wis,
						sensor_data.tmp102_wis,
						sensor_data.hh10d_wis
					);
					//printf("%s\n", buf);
					res = PQexec(conn, buf);
					if (PQresultStatus(res) == PGRES_COMMAND_OK) {
						PQclear(res);
						fprintf(stderr, "%s\n", PQerrorMessage(conn));

						sprintf(buf, "SELECT * FROM pend_msgs WHERE dev_id = %d", dev_id);
						res = PQexec(conn, buf);
						if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res)) {
							buf[2] = GATEWAY_PROTOCOL_STAT_ACK_PEND;
							printf("ACK_PEND prepared\n");
						} else {
							buf[2] = GATEWAY_PROTOCOL_STAT_ACK;
							printf("ACK prepared\n");
						}

						buf[0] = dev_id;				
						buf[1] = GATEWAY_PROTOCOL_PACKET_TYPE_STAT;
						buf_len = 3;
					}
					PQclear(res);
				} else {
					buf[0] = dev_id;				
					buf[1] = GATEWAY_PROTOCOL_PACKET_TYPE_STAT;
					buf[2] = GATEWAY_PROTOCOL_STAT_NACK;
					buf_len = 3;

					printf("payload decode error\n");
				}
			} else if (packet_type == GATEWAY_PROTOCOL_PACKET_TYPE_PEND_REQ) {
			
			} else {
				printf("packet type error\n");
			}

			if (sendto(server_desc, (char *) buf, buf_len, 0, (struct sockaddr *)&client, sock_len) < 0) {
				perror("sendto error");
			}
		} else {
			perror("packet decode error");
		}
	}

	close(server_desc);
	PQfinish(conn);

	return EXIT_SUCCESS;
}

uint8_t gateway_protocol_data_send_payload_decode(sensor_data_t *sensor_data, const uint8_t *payload, const uint8_t payload_length) {
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

	return (p_len == payload_length);
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
