/*
*   Copyright (C) 2016,2017 by Jonathan Naylor G4KLX
*   Copyright (C) 2018 by Andy Uribe CA6JAU
*   Copyright (C) 2018 by Manuel Sanchez EA7EE
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "APRSReader.h"
#include "Timer.h"
#include "Log.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h> 
#include <time.h>
#include <sys/time.h>

CAPRSReader::CAPRSReader(std::string ApiKey,int refres_time) :
CThread(),
m_ApiKey(ApiKey),
m_cs(),
m_stop(false),
m_new_callsign(false),
m_refres_time(refres_time),
m_lat_table(),
m_lon_table(),
m_time_table()
{
	m_lat_table.clear();
	m_lon_table.clear();
	m_time_table.clear();
	run();
}

CAPRSReader::~CAPRSReader()
{

}

void CAPRSReader::entry()
{
	LogMessage("Started the APRS Reader lookup thread");

	while (!m_stop) {
		
		while(!m_new_callsign) sleep(1000U);
		load_call();
		m_new_callsign=false;
	}

	LogMessage("Stopped the APRS Reader lookup thread");
}

void CAPRSReader::stop()
{
	m_stop = true;
}

void CAPRSReader::formatGPS(unsigned char *buffer, int latitude, int longitude)
{
	int lon_sign, lat_sign, a, b;

	if (longitude < 0) {
		longitude *= -1;
		lon_sign = 0x30;
	} else
		lon_sign = 0x50;

	if (latitude < 0) {
		lat_sign = 0x30;
		latitude *= -1;
	} else
		lat_sign = 0x50;

	// Latitude
	int lat_dec = latitude / 10000;
	int lat_uni = (latitude / 1000) % 10;
	*(buffer + 5U) = lat_dec | 0x50;
	*(buffer + 6U) = lat_uni | lon_sign;

	int lat_min = (latitude - (lat_dec * 10 + lat_uni) * 1000) * 6;

	int lat_min_dec = lat_min / 1000;
	int lat_min_uni = (lat_min / 100) % 10;
	*(buffer+7U) = lat_min_dec | 0x50;
	*(buffer+8U) = lat_min_uni | lat_sign;

	int lat_frac = lat_min - (lat_min_dec * 10 + lat_min_uni) * 100;
	int lat_frac_dec = lat_frac / 10;
	int lat_frac_uni = lat_frac % 10;

	int lon_grad = longitude / 1000;

	if (lon_grad >= 0 && lon_grad <= 9) {
		a = 0x50;
		b = (lon_grad + 0x76U);
	} else if (lon_grad >= 10 && lon_grad <= 99) {
		a = 0x30;
		b = ((lon_grad - 10U) + 0x26U);
	} else if (lon_grad >= 100 && lon_grad <= 109) {
		a = 0x50;
		b = ((lon_grad - 100U) + 0x6CU);
	}
	else {
		a = 0x50;
		b = ((lon_grad - 110U) + 0x26U);
	}

	*(buffer + 9U) = lat_frac_dec | a;
	*(buffer + 10U) = lat_frac_uni | 0x50;
	*(buffer + 11U) = b;

	int long_min = ((longitude - (lon_grad * 1000)) * 6) / 100;

	if (long_min >= 0 && long_min <= 9)
		b = long_min + 0x58U;
	else
		b=(long_min - 10U) + 0x26;

	*(buffer + 12U) = b;

	int long_min_frac = ((longitude - (lon_grad * 1000)) * 6) - (long_min * 100);
	*(buffer + 13U) = long_min_frac + 0x1CU;

	//crc calculation
	int i;
	int crc = 0;

	for (i = 0;i < 19; i++)
		crc += *(buffer + i);

	crc &= 0x00FF;
	*(buffer + 19U) = crc;
}

bool CAPRSReader::load_call() 
{
	struct timeval timeinfo;
	unsigned long epoch;

	int sockfd = 0;
	struct sockaddr_in SockAddr;
	struct hostent *host;
	int longitude = 0;
	int latitude = 0;
	
	char buffer[10000];
	int nDataLength;
	std::string website_HTML;

	// get information
	LogMessage("Searching %s", m_cs.c_str());
	// website url
	std::string url = "/api/get?name=" + m_cs + "&what=loc&apikey=" + m_ApiKey + "&format=json";
	//HTTP GET
	std::string get_http = "GET " + url + " HTTP/1.1\r\nHost: api.aprs.fi\r\nUser-Agent: YSF2DMR/0.12\r\n\r\n";

	sockfd = socket(AF_INET, SOCK_STREAM, 0);

	if (sockfd < 0) {
		LogMessage("Could not open socket");
		return false;
	}

	host = gethostbyname("api.aprs.fi");
	if (host == NULL) {
		LogMessage("ERROR, no such host");
		return false;
	}

	bzero((char *) &SockAddr, sizeof(SockAddr));
	SockAddr.sin_family = AF_INET;
	bcopy((char *)host->h_addr, (char *)&SockAddr.sin_addr.s_addr, host->h_length);
	SockAddr.sin_port = htons(80);

	if(connect(sockfd, (struct sockaddr*) &SockAddr, sizeof(SockAddr)) < 0){
		LogMessage("Could not connect");
		return false;
	}

	// send GET / HTTP
	write(sockfd, get_http.c_str(), strlen(get_http.c_str()));
	// recieve html
	gettimeofday(&timeinfo,0);
	epoch = timeinfo.tv_sec;

	while ((nDataLength = read(sockfd, buffer, 10000)) > 0){
		int i = 0;
		char tmp_str[20];
		for (i = 0; i < nDataLength; i++) {
			if (i > 5 && buffer[i - 4] == '\"' && buffer[i - 3] == 'l' && buffer[i - 2] == 'a' && buffer[i - 1] == 't' && buffer[i] == '\"'){
				::memcpy(tmp_str, buffer + i + 3, 8U);
				tmp_str[8] = 0;
				//LogMessage("Latitude: %s", tmp_str);
				latitude = (int)(atof(tmp_str) * 1000);
				m_lat_table[m_cs] = latitude;
			}
			if (i > 5 && buffer[i - 4] == '\"' && buffer[i - 3] == 'l' && buffer[i - 2] == 'n' && buffer[i - 1] == 'g' && buffer[i] == '\"'){
				::memcpy(tmp_str,buffer + i + 3, 8U);
				tmp_str[8] = 0;
				//LogMessage("Longitude: %s", tmp_str);
				longitude = (int)(atof(tmp_str) * 1000);
				m_lon_table[m_cs] = longitude;
			}
		}
	}

	m_time_table[m_cs] = epoch;

	close(sockfd);

	if (latitude == 0 || longitude == 0) {
		m_lat_table[m_cs] = 0;
		m_lon_table[m_cs] = 0;
		LogMessage("Call %s not found", m_cs.c_str());
		return false;
	}
	else {
		LogMessage("Call %s found", m_cs.c_str());
		return true;
	}
}

bool CAPRSReader::findCall(std::string cs, int *latitude, int *longitude)
{
	bool not_found = false;
	struct timeval timeinfo;
	unsigned int epoch, tempo;
	
	if (m_new_callsign)
		return false;
	
	try {
		*latitude = m_lat_table.at(cs);
	} catch (...) {
		not_found = true;
	}

	try {
		*longitude = m_lon_table.at(cs);
	} catch (...) {
		not_found = true;
	}
	
	if (not_found) {
		if (!m_new_callsign) {
			m_new_callsign = true;
			m_cs = cs;
		}
		return false;
	}
	else {
		gettimeofday(&timeinfo, 0);
		epoch = timeinfo.tv_sec;
		tempo = m_time_table.at(cs);
		
		if (epoch > (tempo + m_refres_time)) {
			//LogMessage("Location expired");
			if (!m_new_callsign) {
				m_new_callsign = true;
				m_cs = cs;
			}
		}
	}
	
	if (*latitude == 0 || *longitude == 0)
		return false;
	else
		return true;
}

