#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<sys/socket.h>
#include<arpa/inet.h> //inet_addr
#include<unistd.h>
#include<stdint.h>

#include<pthread.h>

#include<lib/gateway_protocol/gateway_protocol.h>

#include<sys/time.h>

void * connection_handler (void *args);

int main (int argc, char **argv) {
	int server_desc, client_desc;
       	struct sockaddr_in server, client;
	socklen_t client_socklen;
	char buf[1024] = "";

	if ((server_desc = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
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

	listen(server_desc, 3);

	while ((client_desc = accept(server_desc, (struct sockaddr *) &client, &client_socklen))) {
		//strncpy(buf, "Hello, client!", sizeof(buf));
		//write(client_desc, buf, strlen(buf));

		pthread_t thr;
		if (pthread_create(&thr, NULL, connection_handler, (void *) &client_desc) < 0) {
			perror("thread creation failed");
			return EXIT_FAILURE;
		}

		pthread_join(thr, NULL);
	}

	return EXIT_SUCCESS;
}

void *connection_handler(void *args) {
	int client_desc = *(int *)args;
	
	uint8_t buf[128] = "";
	uint8_t buf_len = 0;
	uint8_t payload[128];
	uint8_t payload_length = 0;

	//strncpy(buf, "connection handler greetings!", sizeof(buf));

	//write(client_desc, buf, strlen(buf));

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
			}
		}
	}

}
