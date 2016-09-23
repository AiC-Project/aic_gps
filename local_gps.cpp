#define LOG_TAG "local_gps"
#include <cutils/properties.h>
#include <cutils/log.h>

#include <netinet/tcp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>

#include "aic.h"

#include "gps.hpp"
#include "sensors_packet.pb.h"
#include <sys/types.h>

#define GPS_UPDATE_PERIOD 1 /* period in sec between 2 gps fix emission */

#define STRING_GPGGA "$GPGGA,%02d%02d%02d,%02d%02d.%04d,%c,%02d%02d.%04d,%c,1,08,%i,%f,M,0.,M,,,*47\n"
#define STRING_GPRMC "$GPRMC,%02d%02d%02d,A,%02d%02d.%04d,%c,%02d%02d.%04d,%c,%f,%f,%02d%02d%02d,%f,*47\n"


// Protobuff
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>


using namespace google::protobuf::io;

google::protobuf::uint32 readHdr(char *buf)
{
  google::protobuf::uint32 size;
  google::protobuf::io::ArrayInputStream ais(buf,4);
  CodedInputStream coded_input(&ais);
  coded_input.ReadVarint32(&size);//Decode the HDR and get the size
  ALOGE(" readHdr --   size of payload is %d", size);
  return size;
}

void readBody(int csock, google::protobuf::uint32 siz)
{
    char c_status[255], c_latitude[255], c_longitude[255], c_altitude[255], c_bearing[255];

    int bytecount;
    sensors_packet payload;
    char* buffer = (char*) calloc(siz+4, sizeof(char));//size of the payload and hdr
    //Read the entire buffer including the hdr
    if ((bytecount = recv(csock, (void*) buffer, 4+siz, MSG_WAITALL))== -1)
    {
        ALOGE(" readBody: error receiving data (%d)", errno);
        free((void*) buffer);
        return;
    }
    else if (bytecount != siz+4)
    {
        ALOGE(" readBody: Expected to read %d bytes, received %d", siz+4, bytecount);
        free((void*) buffer);
        return;
    }
    ALOGE(" readBody --  Second read byte count is %d",bytecount);
    //Assign ArrayInputStream with enough memory
    google::protobuf::io::ArrayInputStream ais(buffer, siz+4);
    CodedInputStream coded_input(&ais);
    //Read an unsigned integer with Varint encoding, truncating to 32 bits.
    coded_input.ReadVarint32(&siz);
    //After the message's length is read, PushLimit() is used to prevent the CodedInputStream
    //from reading beyond that length.Limits are used when parsing length-delimited
    //embedded messages
    google::protobuf::io::CodedInputStream::Limit msgLimit = coded_input.PushLimit(siz);
    //De-Serialize
    payload.ParseFromCodedStream(&coded_input);
    //Once the embedded message has been parsed, PopLimit() is called to undo the limit
    coded_input.PopLimit(msgLimit);


    if (payload.has_gps() ){

        if (payload.gps().status() == sensors_packet_GPSPayload_GPSStatusType_ENABLED )
            strcpy(c_status, GPS_ENABLED);
        else
            strcpy(c_status, GPS_DISABLED);

        sprintf(c_latitude ,"%lf",payload.gps().latitude()  );
        sprintf(c_longitude,"%lf",payload.gps().longitude() );
        sprintf(c_altitude ,"%lf",payload.gps().altitude() );
        sprintf(c_bearing  ,"%lf",payload.gps().bearing() );

        property_set(GPS_STATUS , GPS_ENABLED);
        property_set(GPS_LATITUDE   , c_latitude);
        property_set(GPS_LONGITUDE  , c_longitude);
        property_set(GPS_ALTITUDE   , c_altitude);
        property_set(GPS_BEARING    , c_bearing);

        SLOGE("  unpack_gps_data -  GPS_LATITUDE=%lf - GPS_LONGITUDE=%lf - GPS_ALTITUDE=%lf - GPS_BEARING=%lf", \
                payload.gps().latitude(),payload.gps().longitude(), payload.gps().altitude(), payload.gps().bearing());

        SLOGE("  unpack_gps_data -  GPS_LATITUDE=%s - GPS_LONGITUDE=%s - GPS_ALTITUDE=%s - GPS_BEARING=%s", \
                GPS_LATITUDE, GPS_LONGITUDE, GPS_ALTITUDE, GPS_BEARING);

    }else{
        ALOGE(" Unpack_sensor_data_GPS: incorrect message ");
    }
    free((void*) buffer);
}


static int start_server(uint16_t port) {
    int server = -1;
    struct sockaddr_in srv_addr;
    long haddr;

    bzero(&srv_addr, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = INADDR_ANY;
    srv_addr.sin_port = htons(port);

    if ((server = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        SLOGE(" GPS Unable to create socket\n");
        return -1;
    }

    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    if (bind(server, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        SLOGE(" GPS Unable to bind socket, errno=%d\n", errno);
        return -1;
    }

    return server;
}

static int wait_for_client(int server) {
    int client = -1;

    if (listen(server, 1) < 0) {
        SLOGE("Unable to listen to socket, errno=%d\n", errno);
        return -1;
    }

    client = accept(server, NULL, 0);

    if (client < 0) {
        SLOGE("Unable to accept socket for main conection, errno=%d\n", errno);
        return -1;
    }

    return client;
}

int main(int argc, char *argv[]) {
    int server = -1; int sim_server = -1;
    int client = -1; int sim_client = -1;

    char gps_latitude[PROPERTY_VALUE_MAX];
    char gps_longitude[PROPERTY_VALUE_MAX];
    char gps_altitude[PROPERTY_VALUE_MAX];
    char gps_bearing[PROPERTY_VALUE_MAX];
    char gps_status[PROPERTY_VALUE_MAX];
    char gps_precision[PROPERTY_VALUE_MAX];
    char gpgga[128];
    char gprmc[128];

    double i_lat=0., i_lng=0., i_alt=0., i_bearing=0.;
    double o_lat, o_lng, o_alt;
    int o_latdeg, o_latmin, o_lngdeg, o_lngmin;
    char o_clat, o_clng;

    char buffer[4];
    int bytecount=0;

    if ((server = start_server(GPS_PORT)) == -1) {
        SLOGE(" GPS Unable to create socket\n");
        return 1;
    }

    property_get(GPS_STATUS, gps_status, GPS_DEFAULT_STATUS);

    property_set(GPS_LATITUDE, "0");
    property_set(GPS_LONGITUDE, "0");
    property_set(GPS_ALTITUDE, "0");
    property_set(GPS_BEARING, "0");


    if ((sim_server = start_server(SIM_GPS_PORT)) == -1) {
        SLOGE(" GPS Unable to create socket\n");
        return 1;
    }

    // Listen for main connection
    while ((client = wait_for_client(server)) != -1) {

        while (1) {
            // Update GPS info every GPS_UPDATE_PERIOD seconds
            sleep(GPS_UPDATE_PERIOD*2);

            if (GPS_DEBUG) SLOGD("GPS enabled, parsing properties - %d" , client);

            if ( (sim_client = wait_for_client(sim_server) ) != -1){
                memset(buffer, '\0', 4);

                //Peek into the socket and get the packet size
                if((bytecount = recv(sim_client,buffer, 4, MSG_PEEK))== -1)
                    SLOGE("GPS::  Error receiving data");
                else if (bytecount == 0)
                    SLOGE("GPS:: First read byte count is empty");
                else
                {
                    SLOGD("GPS:: First read byte count is %d",bytecount);
                    google::protobuf::uint32 framing_size = readHdr(buffer);
                    if (framing_size < 4*1024*1024) // Don't expect protobufs > 4MiB
                        readBody(sim_client, framing_size);
                    else
                    {
                        SLOGE("GPS:: Framing size too big (%d)", framing_size);
                        bytecount = 0;
                    }
                }
            }
            close(sim_client);

            property_get(GPS_STATUS, gps_status, GPS_DEFAULT_STATUS);

            if (bytecount > 0 && strcmp(gps_status, GPS_ENABLED) == 0)
            {
                property_get(GPS_LATITUDE, gps_latitude, "0");
                property_get(GPS_LONGITUDE, gps_longitude, "0");
                property_get(GPS_ALTITUDE, gps_altitude, "0");
                property_get(GPS_BEARING, gps_bearing, "0");

                i_lat = atof(gps_latitude);
                i_lng = atof(gps_longitude);
                i_alt = atof(gps_altitude);
                i_bearing = atof(gps_bearing);

                if (i_lat<0) {
                    o_lat = -i_lat;
                    o_clat = 'S';
                } else {
                    o_lat = i_lat;
                    o_clat = 'N';
                }

                o_latdeg = (int)o_lat;
                o_lat = 60. * (o_lat - (double)o_latdeg);
                o_latmin = (int) o_lat;
                o_lat = 10000. * (o_lat - (double)o_latmin);

                if (i_lng<0) {
                    o_lng = -i_lng;
                    o_clng = 'W';
                } else {
                    o_lng = i_lng;
                    o_clng = 'E';
                }

                o_lngdeg = (int)o_lng;
                o_lng = 60. * (o_lng - (double)o_lngdeg);
                o_lngmin = (int) o_lng;
                o_lng = 10000. * (o_lng - (double)o_lngmin);

                /* HDOP (horizontal dilution of precision) */
                property_get(GPS_ACCURACY, gps_precision, GPS_DEFAULT_ACCURACY);
                float precision = atof(gps_precision);
                if (precision < 0. || precision > 200.) {
                    SLOGE("Invalid precision %s, should be [0..200]");
                    continue;
                }

                struct timeval tv;
                struct tm tm;

                if (gettimeofday(&tv, NULL) == -1) {
                    SLOGE("gettimeofday");
                    continue;
                }

                if (!gmtime_r(&tv.tv_sec, &tm)) {
                    SLOGE("gmtime_r");
                    continue;
                }

                snprintf(gpgga, sizeof(gpgga), STRING_GPGGA,
                        tm.tm_hour, tm.tm_min, tm.tm_sec,
                        o_latdeg, o_latmin, (int)o_lat, o_clat,
                        o_lngdeg, o_lngmin, (int)o_lng, o_clng,
                        (int)precision,
                        i_alt);

                snprintf(gprmc, sizeof(gprmc), STRING_GPRMC,
                        tm.tm_hour, tm.tm_min, tm.tm_sec,
                        o_latdeg, o_latmin, (int)o_lat, o_clat,
                        o_lngdeg, o_lngmin, (int)o_lng, o_clng,
                        0.0,
                        i_bearing,
                        tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100,
                        i_bearing);

                if (GPS_DEBUG) {
                    SLOGD("GGA command : %s", gpgga);
                    SLOGD("RMC command : %s", gprmc);
                }

                if (send(client, gpgga, strlen(gpgga), MSG_NOSIGNAL) < 0) {
                    SLOGE("Can't send GGA command");
                    break;
                }

                if (send(client, gprmc, strlen(gprmc), MSG_NOSIGNAL) < 0) {
                    SLOGE("Can't send RMC command");
                    break;
                }
            }
        }

        close(client);

    }

    close(server);

    return (server == -1 || client != -1);
}
