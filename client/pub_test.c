/*
 * This example shows how to publish messages from outside of the Mosquitto network loop.
 */

#include <mosquitto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


/* Callback called when the client receives a CONNACK message from the broker. */
void on_connect(struct mosquitto *mosq, void *obj, int reason_code)
{
	/* Print out the connection result. mosquitto_connack_string() produces an
	 * appropriate string for MQTT v3.x clients, the equivalent for MQTT v5.0
	 * clients is mosquitto_reason_string().
	 */
	printf("on_connect: %s\n", mosquitto_connack_string(reason_code));
	if(reason_code != 0){
		/* If the connection fails for any reason, we don't want to keep on
		 * retrying in this example, so disconnect. Without this, the client
		 * will attempt to reconnect. */
		mosquitto_disconnect(mosq);
	}

	/* You may wish to set a flag here to indicate to your application that the
	 * client is now connected. */
}


/* Callback called when the client knows to the best of its abilities that a
 * PUBLISH has been successfully sent. For QoS 0 this means the message has
 * been completely written to the operating system. For QoS 1 this means we
 * have received a PUBACK from the broker. For QoS 2 this means we have
 * received a PUBCOMP from the broker. */
void on_publish(struct mosquitto *mosq, void *obj, int mid)
{
	printf("Message with mid %d has been published.\n", mid);
}


int get_temperature(void)
{
	sleep(1); /* Prevent a storm of messages - this pretend sensor works at 1Hz */
	return random()%100;
}

/* This function pretends to read some data from a sensor and publish it.*/
void publish_sensor_data(struct mosquitto *mosq)
{
	char payload[1024];
	int temp;
	int rc;
	static int id = 0;

	sleep(1);

    long long ust;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    ust /= 1000000;
    //fprintf(stdout, "ust: %ld\n", ust);

	/* Get our pretend data */
	//temp = get_temperature();
	/* Print it to a string for easy human reading - payload format is highly
	 * application dependent. */
	snprintf(payload, sizeof(payload), "[{\"dn\": \"d1\",\"properties\": {\"f1\":%d,\"f2\":%d},\"time\":%lld}, {\"dn\": \"d2\",\"properties\": {\"f1\": %d,\"f2\":%d},\"time\":%lld}]", id,id+10,ust,id+100,id+1000,ust);
	id++;
	/* Publish the message
	 * mosq - our client instance
	 * *mid = NULL - we don't want to know what the message id for this message is
	 * topic = "example/temperature" - the topic on which this message will be published
	 * payloadlen = strlen(payload) - the length of our payload in bytes
	 * payload - the actual payload
	 * qos = 2 - publish with QoS 2 for this example
	 * retain = false - do not use the retained message feature for this message
	 */
	rc = mosquitto_publish(mosq, NULL, "/edge/property/SN12312313/post", strlen(payload), payload, 0, false);
	if(rc != MOSQ_ERR_SUCCESS){
		fprintf(stderr, "Error publishing: %s\n", mosquitto_strerror(rc));
	}
}


int main(int argc, char *argv[])
{
	struct mosquitto *mosq;
	int rc;

	/* Required before calling other mosquitto functions */
	mosquitto_lib_init();

	/* Create a new client instance.
	 * id = NULL -> ask the broker to generate a client id for us
	 * clean session = true -> the broker should remove old sessions when we connect
	 * obj = NULL -> we aren't passing any of our private data for callbacks
	 */
	mosq = mosquitto_new("a1dbcds5|securemode=3,signmethod=hmacsha256,timestamp=1648110652|", true, NULL);
	if(mosq == NULL){
		fprintf(stderr, "Error: Out of memory.\n");
		return 1;
	}

	rc = mosquitto_username_pw_set(mosq,"SN12312313|SN12312313","33471828e4781907ac85527cb8f50cc3a26ce1048ede12fcc4d16d293f9cd186");
	if(rc != MOSQ_ERR_SUCCESS){
		mosquitto_destroy(mosq);
		fprintf(stderr, "Error: %s\n", mosquitto_strerror(rc));
		return 1;
	}

	/* Configure callbacks. This should be done before connecting ideally. */
	mosquitto_connect_callback_set(mosq, on_connect);
	mosquitto_publish_callback_set(mosq, on_publish);

	/* Connect to test.mosquitto.org on port 1883, with a keepalive of 60 seconds.
	 * This call makes the socket connection only, it does not complete the MQTT
	 * CONNECT/CONNACK flow, you should use mosquitto_loop_start() or
	 * mosquitto_loop_forever() for processing net traffic. */
	rc = mosquitto_connect(mosq, "49.234.26.12", 1883, 60);
	if(rc != MOSQ_ERR_SUCCESS){
		mosquitto_destroy(mosq);
		fprintf(stderr, "Error: %s\n", mosquitto_strerror(rc));
		return 1;
	}

	/* Run the network loop in a background thread, this call returns quickly. */
	rc = mosquitto_loop_start(mosq);
	if(rc != MOSQ_ERR_SUCCESS){
		mosquitto_destroy(mosq);
		fprintf(stderr, "Error: %s\n", mosquitto_strerror(rc));
		return 1;
	}

	/* At this point the client is connected to the network socket, but may not
	 * have completed CONNECT/CONNACK.
	 * It is fairly safe to start queuing messages at this point, but if you
	 * want to be really sure you should wait until after a successful call to
	 * the connect callback.
	 * In this case we know it is 1 second before we start publishing.
	 */
	while(1){
		publish_sensor_data(mosq);
	}

	mosquitto_lib_cleanup();
	return 0;
}

