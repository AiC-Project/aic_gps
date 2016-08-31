#ifndef GPS_GOBY_H_
#define GPS_GOBY_H_

typedef struct s_all_tokens {
    Token time;
    Token latitude;
    Token latitudeHemi;
    Token longitude;
    Token fixStatus;
    Token longitudeHemi;
    Token accuracy;
    Token altitude;
    Token altitudeUnits;
    Token speed;
    Token bearing;
    Token date;
} t_all_tokens;
#endif
